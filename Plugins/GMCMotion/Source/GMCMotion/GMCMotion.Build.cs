using UnrealBuildTool;

public class GMCMotion : ModuleRules
{
	public GMCMotion(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

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
			"EnhancedInput"
		});
	}
}
