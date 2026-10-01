// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;

public class GMCGASP : ModuleRules
{
	public GMCGASP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// No Public/Private split in this module: make "Combat/..." includes resolve from the module root.
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "GameplayTags" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"EnhancedInput",
			// Quantized vectors of the replicated ragdoll pose, body instances of the physics layer
			"NetCore",
			"PhysicsCore",
			// GMC pawn / aggregator, GMAS (death and ragdoll tags, effects)
			"GMCCore",
			"GMCAbilitySystem",
			// Ragdoll (Combat/GMCGASPRagdollComponent): the capsule-follow movement mode, active ragdoll controls
			"GMCMotion",
			"PhysicsControl",
			// Get-ups motion matched against the ragdoll pose (GASP58 CHT_GetUpMontages), interactions (MotionMatchMulti)
			"PoseSearch",
			// Interaction montages: settings of their motion warping notifies, skew warp of the root motion
			"MotionWarping",
			// Player smart objects (Interaction/GMCGASPSmartObjectUser): slot search, claim, slot transforms
			"SmartObjectsModule"
		});

		// Editor-only asset helpers (Tools/GMCGASPAssetTools): EditorAnimUtils lives in UnrealEd
		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
