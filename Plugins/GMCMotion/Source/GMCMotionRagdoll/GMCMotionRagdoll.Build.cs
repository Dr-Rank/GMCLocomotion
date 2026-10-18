using UnrealBuildTool;

public class GMCMotionRagdoll : ModuleRules
{
	public GMCMotionRagdoll(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"GameplayTags",
			// IGMCMotionControlLossSource, the capsule-follow movement mode (UGMCMotion::SetRagdollMode)
			"GMCMotion",
			// FPoseSearchContinuingProperties / constraints in the component's header
			"PoseSearch"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"EnhancedInput",
			// Quantized vectors of the replicated ragdoll pose, body instances of the physics layer
			"NetCore",
			"PhysicsCore",
			// GMC pawn / aggregator, GMAS (death and ragdoll tags, effects)
			"GMCCore",
			"GMCAbilitySystem",
			// Active ragdoll controls (GASP58 PhysicsControl asset)
			"PhysicsControl",
			// Interaction montages: settings of their motion warping notifies, skew warp of the root motion
			"MotionWarping"
		});
	}
}
