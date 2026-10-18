using UnrealBuildTool;

public class GMCMotion : ModuleRules
{
	public GMCMotion(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Feature level of this GMCMotion, matching "Version" in GMCMotion.uplugin, for code that supports several
		// (#if GMCMOTION_VERSION >= 2). 2: requested slope slide (SetSlopeSlideRequested), camera yaw limit,
		// GMCMotionRagdoll module.
		PublicDefinitions.Add("GMCMOTION_VERSION=2");

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GMCCore",
			"GMCAbilitySystem",
			"PoseSearch",
			"MotionWarping",
			"GameplayTags"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"EnhancedInput",
			"AnimationWarpingRuntime",
			"AnimGraphRuntime"
		});
	}
}
