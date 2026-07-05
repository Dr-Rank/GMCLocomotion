"""
GASP Bridge Variable Manager
=============================
Add or remove variables from FGASPBridgeData, VariableToAnimBPBridge,
and the simulated proxy fallback in TickComponent — all three are required
for a bridge variable to work correctly on all network roles.

Usage:
    python Tools/manage_bridge_variables.py

After running, recompile C++ in Visual Studio (Development Editor | Win64).
The Make FGASPBridgeData node in BP will update automatically.
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

def has_sim_proxy_fallback(cpp_content, bp_var_name):
    """Check if a FindPropertyByName block exists for the given BP variable name."""
    pattern = rf'FindPropertyByName\(TEXT\("{re.escape(bp_var_name)}"\)\)'
    return bool(re.search(pattern, cpp_content))

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

def find_sim_proxy_insert_pos(cpp_content):
    """Find the position to insert a new sim proxy block (before the closing } of IsSimulatedProxy())."""
    match = re.search(r'if\s*\(IsSimulatedProxy\(\)\)\s*\{', cpp_content)
    if not match:
        return -1

    open_brace = match.end() - 1  # position of the {
    close_brace = find_block_end(cpp_content, open_brace)
    if close_brace == -1:
        return -1

    return close_brace

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
    print(f"  [1/3] Added {var_type} {field_name} to FGASPBridgeData")

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
    print(f"  [2/3] Added bridge assignment: {member_name} = BD.{field_name}")

    # === 3. Update cpp: add sim proxy fallback in TickComponent ===
    label = f"{bp_var_name}"
    if bp_var_name != field_name:
        label = f"{bp_var_name} (bridge field: {field_name})"

    block = generate_sim_proxy_block(var_type, member_name, bp_var_name, cast_type, label)
    cpp_content = insert_sim_proxy_block(cpp_content, block)
    print(f"  [3/3] Added sim proxy fallback: FindPropertyByName(\"{bp_var_name}\")")

    print(f"\nDone. '{field_name}' ({var_type}) -> {member_name} (BP: {bp_var_name})")
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
        print(f"  [3/3] Removed sim proxy fallback for \"{bp_var_name}\"")
    else:
        print(f"  [3/3] No sim proxy fallback found for \"{bp_var_name}\" (skipped)")

    print(f"\nRemoved '{field_name}'")
    return header_content, cpp_content

def list_variables(header_content, cpp_content):
    """Show current bridge variables and their mappings."""
    variables = parse_existing_variables(header_content)
    assignments = parse_sync_assignments(cpp_content)

    field_to_member = {a["field"]: a["member"] for a in assignments}

    print("\n--- Current Bridge Variables ---\n")
    print(f"  {'Struct Field':<20} {'Type':<10} {'C++ Member':<25} {'Default':<20} {'Sim Proxy'}")
    print(f"  {'-'*20} {'-'*10} {'-'*25} {'-'*20} {'-'*12}")
    for v in variables:
        member = field_to_member.get(v["name"], "???")
        has_proxy = has_sim_proxy_fallback(cpp_content, v["name"])
        proxy_status = "OK" if has_proxy else "MISSING"
        print(f"  {v['name']:<20} {v['type']:<10} {member:<25} {v['default']:<20} {proxy_status}")

    print(f"\n  Total: {len(variables)} variables")

    # Warn about missing sim proxy fallbacks
    missing = [v["name"] for v in variables if not has_sim_proxy_fallback(cpp_content, v["name"])]
    if missing:
        print(f"\n  WARNING: {len(missing)} variable(s) missing sim proxy fallback:")
        for name in missing:
            print(f"    - {name}")
        print("  Sim proxies won't receive updates for these variables.")

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
