"""
GASP Bridge Variable Manager
=============================
Add or remove a GASP bridge variable. Adding one touches five places, and all five
are required — a variable missing any of them fails silently rather than loudly.

Usage:
    python Tools/manage_bridge_variables.py

What "add" generates
--------------------
1. FGASPBridgeData field           - the new pin on the Make node in BP
2. C++ member on UGMCMotion        - what the rest of C++ reads
3. VariableToAnimBPBridge          - BD.Field -> member, on authority/autonomous
4. Sim-proxy fallback              - TickComponent, for when GASP is off
5. GMC replication binding         - BindX + a BI_<Name> handle

Step 5 is the one that makes it work for other players. Steps 1-4 move a value from
BP into a C++ member on the machine that computed it; without a binding the value is
correct on the host and the owning client and silently default everywhere else —
the "works on the host, fails for clients" bug this project keeps hitting.

The prediction mode prompt is not a formality. ClientAuth_Input is right for player
inputs; anything the SERVER owns (health, downed, team) must be ServerAuth, or a
client can assert it.

Bindings live inside `if (bEnableGASPPipeline)`. That flag defaults to false on
UGMCMotion, but what runs is the value on the component instance, which BP_GMC_Pawn
overrides. Check the instance, never the CDO.

Naming: if BP_GMCMovement already has a variable of the same name, the C++ member
MUST NOT be a UPROPERTY, or UHT renames the BP's variable to <Name>_0 and silently
detaches every node that used it. The script applies this rule automatically when
the member name and the BP variable name match.

Still by hand afterwards
------------------------
- Recompile with the editor CLOSED (new reflection data; Live Coding cannot do it)
- Wire the new pin on the Make FGASPBridgeData node in BP_GMCMovement
- If the AnimBP or a chooser needs the value, cache it into a BlueprintReadOnly
  property on UGMCMotion_AnimInstance in UpdateGASPState() — see bGASPDowned
"""

import re
import os
import sys

# Paths relative to project root
HEADER_REL = "Plugins/GMCMotion/Source/GMCMotion/Public/Components/GMCMotion.h"
CPP_REL = "Plugins/GMCMotion/Source/GMCMotion/Private/Components/GMCMotion.cpp"

# Supported types and their defaults
SUPPORTED_TYPES = {
    "float":    "0.f",
    "double":   "0.0",
    "int32":    "0",
    "uint8":    "0",
    "bool":     "false",
    "FVector":  "FVector::ZeroVector",
    "FRotator": "FRotator::ZeroRotator",
    "FVector2D":"FVector2D::ZeroVector",
}

# type -> (GMC bind function, interpolation function)
# Names verified against GMCCore (Replication/SyncSettings.h, Replication/Smoothing.h).
# FVector2D has no GMC bind function, so it cannot be replicated this way.
BIND_DEFAULTS = {
    "bool":     ("BindBool", "NearestNeighbour"),
    "uint8":    ("BindByte", "NearestNeighbour"),
    "int32":    ("BindInt", "NearestNeighbour"),
    "float":    ("BindSinglePrecisionFloat", "Linear"),
    "double":   ("BindDoublePrecisionFloat", "Linear"),
    "FVector":  ("BindCompressedVector", "Linear"),
    "FRotator": ("BindCompressedRotator", "Linear"),
    "FVector2D": (None, None),
}

# The two prediction modes that actually come up here. Getting this wrong is the
# expensive mistake: a server-owned value bound ClientAuth lets a client assert it.
PREDICTION_MODES = {
    "1": ("ClientAuth_Input", "CombineIfUnchanged",
          "player INPUT (the client decides: wants-to-strafe, wants-to-crawl)"),
    "2": ("ServerAuth_Output_ClientValidated", "AlwaysCombine",
          "SERVER-OWNED state (health, downed, team - a client must not assert it)"),
}

BIND_MARKER = "\t} // bEnableGASPPipeline"

MEMBER_MARKER = "\t// --- Bridge members (Tools/manage_bridge_variables.py inserts below this line) ---\n"

def find_project_root():
    """Walk up from script location to find the .uproject file."""
    d = os.path.dirname(os.path.abspath(__file__))
    for _ in range(10):
        parent = os.path.dirname(d)
        if any(f.endswith(".uproject") for f in os.listdir(parent)):
            return parent
        d = parent
    # Fallback: assume script is in Tools/ under project root
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def read_file(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()

def write_file(path, content):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)

def find_block_end(text, open_pos):
    """Find the closing brace matching the opening brace at open_pos."""
    depth = 0
    i = open_pos
    while i < len(text):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1

def parse_existing_variables(header_content):
    """Extract existing FGASPBridgeData fields."""
    match = re.search(
        r"struct\s+FGASPBridgeData\s*\{.*?GENERATED_BODY\(\)(.*?)\};",
        header_content, re.DOTALL
    )
    if not match:
        return []

    body = match.group(1)
    variables = []

    pattern = re.compile(
        r"((?:\s*//[^\n]*\n)*)"           # optional comment lines
        r"\s*UPROPERTY\([^)]*\)\s*\n"     # UPROPERTY(...)
        r"\s*(\w+)\s+(\w+)\s*=\s*([^;]+);", # type name = default;
        re.MULTILINE
    )

    for m in pattern.finditer(body):
        comment = m.group(1).strip()
        var_type = m.group(2)
        var_name = m.group(3)
        default = m.group(4).strip()
        variables.append({
            "comment": comment,
            "type": var_type,
            "name": var_name,
            "default": default,
        })

    return variables

def parse_sync_assignments(cpp_content):
    """Extract existing assignments in VariableToAnimBPBridge."""
    match = re.search(
        r"void\s+UGMCMotion::VariableToAnimBPBridge\s*\([^)]*\)\s*\{(.*?)\}",
        cpp_content, re.DOTALL
    )
    if not match:
        return []

    body = match.group(1)
    assignments = []

    pattern = re.compile(
        r"\s*(\w+)\s*=\s*(static_cast<[^>]+>\()?\s*BD\.(\w+)\)?\s*;"
    )

    for m in pattern.finditer(body):
        member = m.group(1)
        field = m.group(3)
        assignments.append({
            "member": member,
            "field": field,
            "has_cast": bool(m.group(2)),
        })

    return assignments

def has_sim_proxy_fallback(cpp_content, *candidate_names):
    """Check if a FindPropertyByName block exists for any of the given names.

    The fallback is keyed on the BP VARIABLE name, which is often not the struct
    field name (field 'TurnAngle' -> BP 'Trj_TurnAngle'), so callers should pass
    both the field name and the C++ member name. A BP variable named differently
    from both will still report a false MISSING - check by hand before believing it.
    """
    for name in candidate_names:
        if not name or name == "???":
            continue
        if re.search(rf'FindPropertyByName\(TEXT\("{re.escape(name)}"\)\)', cpp_content):
            return True
    return False

def generate_sim_proxy_block(var_type, member_name, bp_var_name, cast_type, label):
    """Generate the FindPropertyByName block for the IsSimulatedProxy() section."""
    indent = "\t\t"

    if var_type == "uint8":
        if cast_type:
            return (
                f"\n{indent}// {label} (BP variable is a byte/enum)\n"
                f"{indent}if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT(\"{bp_var_name}\")))\n"
                f"{indent}{{\n"
                f"{indent}\tif (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))\n"
                f"{indent}\t{{\n"
                f"{indent}\t\t{member_name} = static_cast<{cast_type}>(*ByteProp->ContainerPtrToValuePtr<uint8>(this));\n"
                f"{indent}\t}}\n"
                f"{indent}\telse if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))\n"
                f"{indent}\t{{\n"
                f"{indent}\t\tconst FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();\n"
                f"{indent}\t\t{member_name} = static_cast<{cast_type}>(UnderlyingProp->GetSignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(this)));\n"
                f"{indent}\t}}\n"
                f"{indent}}}\n"
            )
        else:
            return (
                f"\n{indent}// {label} (BP variable is a byte/enum)\n"
                f"{indent}if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT(\"{bp_var_name}\")))\n"
                f"{indent}{{\n"
                f"{indent}\tif (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))\n"
                f"{indent}\t{{\n"
                f"{indent}\t\t{member_name} = *ByteProp->ContainerPtrToValuePtr<uint8>(this);\n"
                f"{indent}\t}}\n"
                f"{indent}\telse if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))\n"
                f"{indent}\t{{\n"
                f"{indent}\t\tconst FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();\n"
                f"{indent}\t\t{member_name} = static_cast<uint8>(UnderlyingProp->GetSignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(this)));\n"
                f"{indent}\t}}\n"
                f"{indent}}}\n"
            )

    elif var_type == "float":
        return (
            f"\n{indent}// {label}\n"
            f"{indent}if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT(\"{bp_var_name}\")))\n"
            f"{indent}{{\n"
            f"{indent}\tif (const FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))\n"
            f"{indent}\t{{\n"
            f"{indent}\t\t{member_name} = *FltProp->ContainerPtrToValuePtr<float>(this);\n"
            f"{indent}\t}}\n"
            f"{indent}\telse if (const FDoubleProperty* DblProp = CastField<FDoubleProperty>(Prop))\n"
            f"{indent}\t{{\n"
            f"{indent}\t\t{member_name} = static_cast<float>(*DblProp->ContainerPtrToValuePtr<double>(this));\n"
            f"{indent}\t}}\n"
            f"{indent}}}\n"
        )

    elif var_type == "double":
        return (
            f"\n{indent}// {label}\n"
            f"{indent}if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT(\"{bp_var_name}\")))\n"
            f"{indent}{{\n"
            f"{indent}\tif (const FDoubleProperty* DblProp = CastField<FDoubleProperty>(Prop))\n"
            f"{indent}\t{{\n"
            f"{indent}\t\t{member_name} = *DblProp->ContainerPtrToValuePtr<double>(this);\n"
            f"{indent}\t}}\n"
            f"{indent}\telse if (const FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))\n"
            f"{indent}\t{{\n"
            f"{indent}\t\t{member_name} = *FltProp->ContainerPtrToValuePtr<float>(this);\n"
            f"{indent}\t}}\n"
            f"{indent}}}\n"
        )

    elif var_type == "int32":
        return (
            f"\n{indent}// {label}\n"
            f"{indent}if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT(\"{bp_var_name}\")))\n"
            f"{indent}{{\n"
            f"{indent}\tif (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))\n"
            f"{indent}\t{{\n"
            f"{indent}\t\t{member_name} = *IntProp->ContainerPtrToValuePtr<int32>(this);\n"
            f"{indent}\t}}\n"
            f"{indent}}}\n"
        )

    elif var_type == "bool":
        return (
            f"\n{indent}// {label}\n"
            f"{indent}if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT(\"{bp_var_name}\")))\n"
            f"{indent}{{\n"
            f"{indent}\tif (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))\n"
            f"{indent}\t{{\n"
            f"{indent}\t\t{member_name} = BoolProp->GetPropertyValue(BoolProp->ContainerPtrToValuePtr<void>(this));\n"
            f"{indent}\t}}\n"
            f"{indent}}}\n"
        )

    elif var_type in ("FVector", "FRotator", "FVector2D"):
        return (
            f"\n{indent}// {label}\n"
            f"{indent}if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT(\"{bp_var_name}\")))\n"
            f"{indent}{{\n"
            f"{indent}\tif (const FStructProperty* SP = CastField<FStructProperty>(Prop))\n"
            f"{indent}\t{{\n"
            f"{indent}\t\tif (SP->Struct == TBaseStructure<{var_type}>::Get())\n"
            f"{indent}\t\t{{\n"
            f"{indent}\t\t\t{member_name} = *SP->ContainerPtrToValuePtr<{var_type}>(this);\n"
            f"{indent}\t\t}}\n"
            f"{indent}\t}}\n"
            f"{indent}}}\n"
        )

    else:
        return f"\n{indent}// TODO: Add sim proxy fallback for {var_type} {member_name} (unsupported type)\n"

def has_member(header_content, member_name, var_type):
    """Check whether a C++ member of this name already exists on UGMCMotion."""
    return bool(re.search(rf'^\t{re.escape(var_type)}\s+{re.escape(member_name)}\s*=',
                          header_content, re.MULTILINE))

def insert_member(header_content, member_name, var_type, default, bp_var_name, comment):
    """Declare the C++ member on UGMCMotion, below the bridge-members marker.

    Collision rule: if the member name matches a variable that already exists in
    BP_GMCMovement, it MUST NOT be a UPROPERTY. UHT would rename the BP's variable
    to <Name>_0 and silently detach every node that reads or writes it.
    """
    pos = header_content.find(MEMBER_MARKER)
    if pos == -1:
        print("  WARNING: Could not find the bridge-members marker in the header.")
        print(f"  Declare '{var_type} {member_name} = {default};' on UGMCMotion by hand.")
        return header_content

    collides = (member_name == bp_var_name)
    text = "\n"
    if comment:
        text += f"\t// {comment}\n"
    if collides:
        text += (f"\t//\n"
                 f"\t// NO UPROPERTY deliberately: this member and BP_GMCMovement's own variable\n"
                 f"\t// share the name \"{bp_var_name}\". A UPROPERTY here would make UHT rename the BP\n"
                 f"\t// variable to \"{bp_var_name}_0\" and silently detach every node that used it.\n"
                 f"\t// (If BP has no such variable, a UPROPERTY is safe — but then the sim-proxy\n"
                 f"\t//  fallback in TickComponent has nothing to read and should be removed.)\n")
    else:
        text += f"\tUPROPERTY(BlueprintReadWrite, Category = \"GMCMotion|GASP\")\n"
    text += f"\t{var_type} {member_name} = {default};\n"

    insert_at = pos + len(MEMBER_MARKER)
    return header_content[:insert_at] + text + header_content[insert_at:]

def remove_member(header_content, member_name, var_type):
    """Remove the C++ member declaration and any comment/UPROPERTY lines above it."""
    pattern = re.compile(
        r'\n(?:\t//[^\n]*\n)*'
        r'(?:\tUPROPERTY\([^)]*\)\n)?'
        r'\t' + re.escape(var_type) + r'\s+' + re.escape(member_name) + r'\s*=[^;]*;\n',
        re.MULTILINE
    )
    return pattern.sub("", header_content, count=1)

def has_binding(cpp_content, member_name):
    """Check whether a BindX call already exists for this C++ member."""
    return bool(re.search(rf'BI_{re.escape(member_name)}\s*=\s*Bind', cpp_content))

def generate_bind_block(member_name, bind_func, pred_mode, combine_mode, sim_mode, interp_func):
    """Generate the BindX call for BindReplicationData_Implementation."""
    return (
        f"\n\t\tBI_{member_name} = {bind_func}(\n"
        f"\t\t\t{member_name},\n"
        f"\t\t\tEGMC_PredictionMode::{pred_mode},\n"
        f"\t\t\tEGMC_CombineMode::{combine_mode},\n"
        f"\t\t\tEGMC_SimulationMode::{sim_mode},\n"
        f"\t\t\tEGMC_InterpolationFunction::{interp_func}\n"
        f"\t\t);\n"
    )

def insert_bind_block(cpp_content, block_text):
    """Insert a BindX call just before the end of the if (bEnableGASPPipeline) block."""
    pos = cpp_content.find(BIND_MARKER)
    if pos == -1:
        print("  WARNING: Could not find the '} // bEnableGASPPipeline' marker.")
        print("  Binding NOT added. Add it manually in BindReplicationData_Implementation.")
        return cpp_content
    return cpp_content[:pos] + block_text + cpp_content[pos:]

def insert_bi_handle(header_content, member_name):
    """Add 'int32 BI_<member> = -1;' after the last existing BI_ handle."""
    matches = list(re.finditer(r'\tint32 BI_\w+ = -1;\n', header_content))
    if not matches:
        print("  WARNING: Could not find the BI_ handle block. Handle NOT added.")
        print(f"  Add 'int32 BI_{member_name} = -1;' manually to the private section.")
        return header_content
    pos = matches[-1].end()
    return header_content[:pos] + f"\tint32 BI_{member_name} = -1;\n" + header_content[pos:]

def remove_bind_block(cpp_content, member_name):
    """Remove the BindX call (and any comment lines directly above it) for this member."""
    pattern = re.compile(
        r'\n(?:\t\t//[^\n]*\n)*'
        r'\t\tBI_' + re.escape(member_name) + r'\s*=\s*Bind\w+\([^;]*?\);\n',
        re.DOTALL
    )
    return pattern.sub("", cpp_content, count=1)

def remove_bi_handle(header_content, member_name):
    """Remove the 'int32 BI_<member> = -1;' declaration."""
    return re.sub(rf'\tint32 BI_{re.escape(member_name)} = -1;\n', "", header_content, count=1)

def find_sim_proxy_insert_pos(cpp_content):
    """Find the position to insert a new sim proxy block (before the closing } of IsSimulatedProxy())."""
    match = re.search(r'if\s*\(IsSimulatedProxy\(\)\)\s*\{', cpp_content)
    if not match:
        return -1

    open_brace = match.end() - 1  # position of the {
    close_brace = find_block_end(cpp_content, open_brace)
    if close_brace == -1:
        return -1

    # Insert at the START of the closing brace's line, not at the brace itself.
    # Inserting at the brace leaves its indentation stranded on the previous line.
    return cpp_content.rfind('\n', 0, close_brace) + 1

def insert_sim_proxy_block(cpp_content, block_text):
    """Insert a sim proxy fallback block before the closing } of IsSimulatedProxy()."""
    insert_pos = find_sim_proxy_insert_pos(cpp_content)
    if insert_pos == -1:
        print("  WARNING: Could not find IsSimulatedProxy() block. Sim proxy fallback NOT added.")
        print("  You must add it manually in TickComponent's IsSimulatedProxy() section.")
        return cpp_content

    return cpp_content[:insert_pos] + block_text + cpp_content[insert_pos:]

def remove_sim_proxy_block(cpp_content, bp_var_name):
    """Remove the FindPropertyByName block for the given BP variable name."""
    # Match the full block: comment line + if (FindPropertyByName...) { ... }
    # We match from the comment line through the closing brace
    pattern = re.compile(
        r'\n\t\t// [^\n]*\n'
        r'\t\tif\s*\(const\s+FProperty\*\s+Prop\s*=\s*MyClass->FindPropertyByName\(TEXT\("'
        + re.escape(bp_var_name) +
        r'"\)\)\)',
        re.DOTALL
    )

    match = pattern.search(cpp_content)
    if not match:
        return cpp_content

    # Find the opening { after the if condition
    start = match.start()
    brace_search_start = match.end()

    # Find the opening brace
    brace_pos = cpp_content.index('{', brace_search_start)
    close_brace = find_block_end(cpp_content, brace_pos)
    if close_brace == -1:
        return cpp_content

    # Remove from start (including leading newline) through close_brace + newline
    end = close_brace + 1
    if end < len(cpp_content) and cpp_content[end] == '\n':
        end += 1

    return cpp_content[:start] + cpp_content[end:]

def add_variable(header_content, cpp_content):
    """Interactive flow to add a new bridge variable."""
    print("\n--- Add Bridge Variable ---\n")

    # Struct field name (appears on Make node pin in BP)
    field_name = input("Struct field name (appears as pin on Make node): ").strip()
    if not field_name:
        print("Cancelled.")
        return header_content, cpp_content

    # C++ member name on UGMCMotion
    member_name = input(f"C++ member name on UGMCMotion (leave blank if same as '{field_name}'): ").strip()
    if not member_name:
        member_name = field_name

    # BP variable name (for sim proxy FindPropertyByName)
    bp_var_name = input(f"BP variable name in BP_GMCMovement (leave blank if same as '{field_name}'): ").strip()
    if not bp_var_name:
        bp_var_name = field_name

    # Type
    print(f"\nSupported types: {', '.join(SUPPORTED_TYPES.keys())}")
    var_type = input("Type: ").strip()
    if var_type not in SUPPORTED_TYPES:
        print(f"Unsupported type '{var_type}'. Supported: {', '.join(SUPPORTED_TYPES.keys())}")
        return header_content, cpp_content

    default = SUPPORTED_TYPES[var_type]

    # Custom default
    custom_default = input(f"Default value (leave blank for '{default}'): ").strip()
    if custom_default:
        default = custom_default

    # Optional comment
    comment = input(f"Comment (optional, e.g. 'BP's {field_name} -> C++ {member_name}'): ").strip()

    # Cast type (for enum-to-uint8 etc.)
    cast_type = input("Cast type (leave blank for none, e.g. 'EGMCMotion_Gait'): ").strip()

    # Probe for the C++ member BEFORE adding the struct field. The field is declared
    # with the same type and often the same name, so probing afterwards matches the
    # field itself and wrongly concludes the member already exists.
    member_type = cast_type if cast_type else var_type
    member_pre_exists = has_member(header_content, member_name, member_type)

    # === 1. Update header: add field to FGASPBridgeData ===
    struct_end_pattern = r"(struct\s+FGASPBridgeData\s*\{.*?)((\s*)\};)"
    match = re.search(struct_end_pattern, header_content, re.DOTALL)
    if not match:
        print("ERROR: Could not find FGASPBridgeData struct in header.")
        return header_content, cpp_content

    new_field = "\n"
    if comment:
        new_field += f"\t// {comment}\n"
    new_field += f"\tUPROPERTY(BlueprintReadWrite, Category = \"GASP\")\n"
    new_field += f"\t{var_type} {field_name} = {default};\n"

    insert_pos = match.end(1)
    header_content = header_content[:insert_pos] + new_field + header_content[insert_pos:]
    print(f"  [1/5] Added {var_type} {field_name} to FGASPBridgeData")

    # === 1b. Declare the C++ member on UGMCMotion (unless it already exists) ===
    # Without this the assignment generated in step 2 references an undeclared name
    # and the module will not compile.
    if member_pre_exists:
        print(f"  [2/5] C++ member '{member_type} {member_name}' already exists (skipped)")
    else:
        member_default = default
        if cast_type:
            member_default = f"static_cast<{cast_type}>({default})"
        header_content = insert_member(header_content, member_name, member_type,
                                       member_default, bp_var_name, comment)
        note = " (no UPROPERTY - name collides with the BP variable)" if member_name == bp_var_name else ""
        print(f"  [2/5] Declared C++ member {member_type} {member_name}{note}")

    # === 2. Update cpp: add assignment in VariableToAnimBPBridge ===
    sync_pattern = r"(void\s+UGMCMotion::VariableToAnimBPBridge\s*\([^)]*\)\s*\{)(.*?)(\})"
    match = re.search(sync_pattern, cpp_content, re.DOTALL)
    if not match:
        print("ERROR: Could not find VariableToAnimBPBridge in cpp file.")
        return header_content, cpp_content

    if cast_type:
        new_assignment = f"\t{member_name} = static_cast<{cast_type}>(BD.{field_name});\n"
    else:
        new_assignment = f"\t{member_name} = BD.{field_name};\n"

    body_end = match.end(2)
    cpp_content = cpp_content[:body_end] + new_assignment + cpp_content[body_end:]
    print(f"  [3/5] Added bridge assignment: {member_name} = BD.{field_name}")

    # === 3. Update cpp: add sim proxy fallback in TickComponent ===
    label = f"{bp_var_name}"
    if bp_var_name != field_name:
        label = f"{bp_var_name} (bridge field: {field_name})"

    block = generate_sim_proxy_block(var_type, member_name, bp_var_name, cast_type, label)
    cpp_content = insert_sim_proxy_block(cpp_content, block)
    print(f"  [4/5] Added sim proxy fallback: FindPropertyByName(\"{bp_var_name}\")")

    # === 4. Update: GMC replication binding ===
    #
    # Without this the value is right on the host and the owning client and silently
    # default-valued for everyone else. The sim-proxy fallback above does NOT cover it
    # unless the BP variable is itself bound, and BP_GMCMovement binds almost nothing.
    bind_func, interp_func = BIND_DEFAULTS.get(var_type, (None, None))

    if bind_func is None:
        print(f"  [5/5] SKIPPED - {var_type} has no GMC bind function.")
        print(f"        '{member_name}' will NOT replicate. It is correct on the host and the")
        print(f"        owning client only. Split it into bindable fields if clients need it.")
    else:
        print(f"\n  Replication binding for '{member_name}':")
        print(f"    Bind function: {bind_func}  (blank to accept, or type another)")
        chosen = input("    > ").strip()
        if chosen:
            bind_func = chosen

        print("\n    Prediction mode - is this an input the CLIENT decides,")
        print("    or state the SERVER owns?")
        for key, (mode, _, desc) in PREDICTION_MODES.items():
            print(f"      {key}. {mode}\n         {desc}")
        mode_choice = input("    > ").strip()

        if mode_choice not in PREDICTION_MODES:
            print("    No valid choice - binding SKIPPED.")
            print(f"    '{member_name}' will not replicate until you add the bind call by hand.")
            print(f"\nDone (no binding). '{field_name}' ({var_type}) -> {member_name} (BP: {bp_var_name})")
            return header_content, cpp_content

        pred_mode, combine_mode, _ = PREDICTION_MODES[mode_choice]

        # PeriodicAndOnChange costs bandwidth per change but delivers the transition
        # immediately, which is what state flags want. Periodic suits values that
        # change every frame anyway.
        sim_default = "PeriodicAndOnChange_Output"
        print(f"\n    Simulation mode [{sim_default}] (blank to accept):")
        sim_mode = input("    > ").strip() or sim_default

        header_content = insert_bi_handle(header_content, member_name)
        block = generate_bind_block(member_name, bind_func, pred_mode,
                                    combine_mode, sim_mode, interp_func)
        cpp_content = insert_bind_block(cpp_content, block)
        print(f"  [5/5] Added binding: BI_{member_name} = {bind_func}(...{pred_mode})")

    print(f"\nDone. '{field_name}' ({var_type}) -> {member_name} (BP: {bp_var_name})")
    print("\nStill to do by hand:")
    print("  - Recompile with the editor CLOSED (new reflection data; Live Coding cannot do it)")
    print(f"  - Wire the '{field_name}' pin on the Make FGASPBridgeData node in BP_GMCMovement")
    print(f"  - To use it in the AnimBP, cache {member_name} into a BlueprintReadOnly property")
    print("    on UGMCMotion_AnimInstance in UpdateGASPState() - see bGASPDowned")
    return header_content, cpp_content

def remove_variable(header_content, cpp_content):
    """Interactive flow to remove a bridge variable."""
    variables = parse_existing_variables(header_content)
    if not variables:
        print("No variables found in FGASPBridgeData.")
        return header_content, cpp_content

    assignments = parse_sync_assignments(cpp_content)
    field_to_member = {a["field"]: a["member"] for a in assignments}

    print("\n--- Remove Bridge Variable ---\n")
    print("Current variables:")
    for i, v in enumerate(variables):
        member = field_to_member.get(v["name"], "???")
        has_proxy = has_sim_proxy_fallback(cpp_content, v["name"])
        proxy_tag = " [sim proxy OK]" if has_proxy else " [NO sim proxy]"
        print(f"  {i + 1}. {v['type']} {v['name']} -> {member}{proxy_tag}")

    choice = input("\nNumber to remove (0 to cancel): ").strip()
    try:
        idx = int(choice) - 1
    except ValueError:
        print("Cancelled.")
        return header_content, cpp_content

    if idx < 0 or idx >= len(variables):
        print("Cancelled.")
        return header_content, cpp_content

    var = variables[idx]
    field_name = var["name"]

    # Ask for BP variable name in case it differs
    bp_var_name = input(f"BP variable name to remove from sim proxy (leave blank if same as '{field_name}'): ").strip()
    if not bp_var_name:
        bp_var_name = field_name

    # === 1. Remove from header ===
    remove_pattern = re.compile(
        r"\n(?:\s*//[^\n]*\n)*"
        r"\s*UPROPERTY\([^)]*\)\s*\n"
        r"\s*" + re.escape(var["type"]) + r"\s+" + re.escape(field_name) + r"\s*=[^;]*;",
        re.MULTILINE
    )
    header_content = remove_pattern.sub("", header_content, count=1)
    print(f"  [1/3] Removed {field_name} from FGASPBridgeData")

    # === 2. Remove from cpp bridge function ===
    remove_cpp_pattern = re.compile(
        r"\s*\w+\s*=\s*(?:static_cast<[^>]+>\()?\s*BD\." + re.escape(field_name) + r"\)?\s*;\n?",
    )
    cpp_content = remove_cpp_pattern.sub("", cpp_content, count=1)
    print(f"  [2/3] Removed bridge assignment for {field_name}")

    # === 3. Remove sim proxy fallback ===
    if has_sim_proxy_fallback(cpp_content, bp_var_name):
        cpp_content = remove_sim_proxy_block(cpp_content, bp_var_name)
        print(f"  [3/4] Removed sim proxy fallback for \"{bp_var_name}\"")
    else:
        print(f"  [3/4] No sim proxy fallback found for \"{bp_var_name}\" (skipped)")

    # === 4. Remove the replication binding and its handle ===
    member_name = field_to_member.get(field_name, field_name)
    if has_binding(cpp_content, member_name):
        cpp_content = remove_bind_block(cpp_content, member_name)
        header_content = remove_bi_handle(header_content, member_name)
        print(f"  [4/4] Removed binding BI_{member_name} and its handle")
    else:
        print(f"  [4/4] No binding found for BI_{member_name} (skipped)")

    print(f"\nRemoved '{field_name}'")
    return header_content, cpp_content

def list_variables(header_content, cpp_content):
    """Show current bridge variables and their mappings."""
    variables = parse_existing_variables(header_content)
    assignments = parse_sync_assignments(cpp_content)

    field_to_member = {a["field"]: a["member"] for a in assignments}

    print("\n--- Current Bridge Variables ---\n")
    print(f"  {'Struct Field':<20} {'Type':<10} {'C++ Member':<25} {'Sim Proxy':<11} {'Replicated'}")
    print(f"  {'-'*20} {'-'*10} {'-'*25} {'-'*11} {'-'*10}")
    for v in variables:
        member = field_to_member.get(v["name"], "???")
        proxy_status = "OK" if has_sim_proxy_fallback(cpp_content, v["name"], member) else "MISSING"
        bind_status = "OK" if (member != "???" and has_binding(cpp_content, member)) else "NO BIND"
        print(f"  {v['name']:<20} {v['type']:<10} {member:<25} {proxy_status:<11} {bind_status}")

    print(f"\n  Total: {len(variables)} variables")

    # A variable can be missing the sim-proxy fallback OR the binding, and the two
    # failures look identical in game: correct on the host, wrong everywhere else.
    no_bind = [v["name"] for v in variables
               if field_to_member.get(v["name"]) and not has_binding(cpp_content, field_to_member[v["name"]])]
    if no_bind:
        print(f"\n  WARNING: {len(no_bind)} variable(s) have NO replication binding:")
        for name in no_bind:
            print(f"    - {name}")
        print("  These are only correct if BP_GMCMovement binds the BP variable itself.")
        print("  Otherwise they read as default on every non-owning client.")

    missing = [v["name"] for v in variables
               if not has_sim_proxy_fallback(cpp_content, v["name"], field_to_member.get(v["name"]))]
    if missing:
        print(f"\n  NOTE: {len(missing)} variable(s) have no sim proxy fallback:")
        for name in missing:
            print(f"    - {name}")
        print("  Fine if the C++ member is bound above (the binding covers sim proxies).")
        print("  Not fine if it is not - those get no value on sim proxies at all.")
        print("  Checked against the field and member names only; a BP variable named")
        print("  differently from both (e.g. the 'TurningStrenght' typo) reports a false MISSING.")

def main():
    root = find_project_root()
    header_path = os.path.join(root, HEADER_REL.replace("/", os.sep))
    cpp_path = os.path.join(root, CPP_REL.replace("/", os.sep))

    if not os.path.exists(header_path):
        print(f"ERROR: Header not found at {header_path}")
        sys.exit(1)
    if not os.path.exists(cpp_path):
        print(f"ERROR: Cpp not found at {cpp_path}")
        sys.exit(1)

    header_content = read_file(header_path)
    cpp_content = read_file(cpp_path)

    print("=" * 50)
    print("  GASP Bridge Variable Manager")
    print("=" * 50)
    print(f"\n  Header: {HEADER_REL}")
    print(f"  Cpp:    {CPP_REL}")

    while True:
        print("\n  1. List variables")
        print("  2. Add variable")
        print("  3. Remove variable")
        print("  4. Save and exit")
        print("  5. Exit without saving")

        choice = input("\nChoice: ").strip()

        if choice == "1":
            list_variables(header_content, cpp_content)
        elif choice == "2":
            header_content, cpp_content = add_variable(header_content, cpp_content)
        elif choice == "3":
            header_content, cpp_content = remove_variable(header_content, cpp_content)
        elif choice == "4":
            write_file(header_path, header_content)
            write_file(cpp_path, cpp_content)
            print("\nFiles saved. Recompile C++ in Visual Studio to apply changes.")
            break
        elif choice == "5":
            print("\nExited without saving.")
            break
        else:
            print("Invalid choice.")

if __name__ == "__main__":
    main()
