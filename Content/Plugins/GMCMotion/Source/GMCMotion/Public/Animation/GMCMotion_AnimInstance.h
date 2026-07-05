// Copyright 2026 Execute Games, all rights reserved

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/TrajectoryTypes.h"
#include "Components/GMCMotion.h"
#include "Utility/GameplayElementMapping.h"
#include "GMCMotion_AnimInstance.generated.h"

// GASP movement mode — values must match the Blueprint E_MovementMode asset.
UENUM(BlueprintType)
enum class EGMCMotion_MovementMode : uint8
{
	OnGround = 0,
	InAir = 1,
	Sliding = 2,
	Traversing = 3
};

class AGMC_Pawn;
class UGMCMotion;
class UGMC_AbilitySystemComponent;

/**
 * Unified AnimInstance for GMCMotion pawns.
 *
 * Consolidates three previously separate layers:
 *  1. Locomotion caching (speed, gait, stance, velocity, aim offset, distance matching, trajectory)
 *  2. GMAS tag-property binding (TagPropertyMap + AbilitySystemComponent discovery)
 *  3. GASP variable caching (rotation mode, movement direction, trajectory metrics, etc.)
 *
 * Flattens the inheritance chain from:
 *   UAnimInstance -> UGMCLoco_AnimInstance -> UCLEAR_AnimInstance -> AnimBP
 * to:
 *   UAnimInstance -> UGMCMotion_AnimInstance -> AnimBP
 *
 * The AnimBP reads GASP variables as native C++ properties (EGMCMotion_RotationMode, etc.)
 * instead of through a BP struct with BP enum conversions.
 */
UCLASS(BlueprintType, Blueprintable, meta=(DisplayName="GMCMotion Animation Blueprint"))
class GMCMOTION_API UGMCMotion_AnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UGMCMotion_AnimInstance(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	virtual ~UGMCMotion_AnimInstance();

	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	virtual void NativeThreadSafeUpdateAnimation(float DeltaSeconds) override;
	virtual void NativeBeginPlay() override;

	// === Component References ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|References")
	TObjectPtr<AGMC_Pawn> OwnerPawn;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|References")
	TObjectPtr<UGMCMotion> MovementComponent;

	// === GMC Core State ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GMC")
	FTransform ActorTransformGMC = FTransform::Identity;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GMC")
	float InputAcceleration = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GMC")
	FVector GMC_Velocity = FVector::ZeroVector;

	/** C++ sets OnGround/InAir/Traversing automatically. BP can override to Sliding. */
	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GMC")
	EGMCMotion_MovementMode E_MovementMode = EGMCMotion_MovementMode::OnGround;

	// === Locomotion State ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|State")
	bool bIsMoving = false;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|State")
	bool bHasInput = false;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|State")
	float Speed = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|State")
	float GroundSpeed = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|State")
	EGMCMotion_Gait CurrentGait = EGMCMotion_Gait::Run;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|State")
	EGMCMotion_Stance CurrentStance = EGMCMotion_Stance::Standing;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|State")
	EGMCMotion_LocomotionMode LocomotionMode = EGMCMotion_LocomotionMode::Strafing;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|State")
	float LocomotionAngle = 0.f;

	// === Velocity / Acceleration ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Derived")
	FVector WorldVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Derived")
	FVector LocalVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Derived")
	FVector WorldAcceleration = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Derived")
	FRotator CachedAimOffset = FRotator::ZeroRotator;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Derived")
	float AimYawRate = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Derived")
	float StrideWarpScale = 1.f;

	// === Movement Mode ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Movement")
	bool bIsOnGround = true;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Movement")
	bool bIsInAir = false;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Movement")
	FVector LastLandingVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Movement")
	float LandVelocityZ = 0.f;

	// === Floor Hit ===

	/** CurrentFloor.ShapeHitResult.Location */
	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Floor")
	FVector FloorHitLocation = FVector::ZeroVector;

	/** CurrentFloor.ShapeHitResult.Normal */
	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Floor")
	FVector FloorHitNormal = FVector::UpVector;

	/** CurrentFloor.ShapeHitResult.ImpactNormal */
	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Floor")
	FVector FloorHitImpactNormal = FVector::UpVector;

	// === Distance Matching ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Distance Matching")
	bool bIsStopping = false;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Distance Matching")
	float PredictedStopDistance = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Distance Matching")
	bool bIsPivoting = false;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Distance Matching")
	float PredictedPivotDistance = 0.f;

	// === Trajectory (Pose Search) ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Trajectory")
	FTransformTrajectory CurrentTrajectory;

	// === Animation Helpers ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Helpers")
	float ComponentYawRate = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Helpers")
	FVector AnimAcceleration = FVector::ZeroVector;

	// === Traversal Hand IK ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Traversal")
	float TraversalHandIKAlpha = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Traversal")
	FVector TraversalHandIK_LeftTarget = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Traversal")
	FVector TraversalHandIK_RightTarget = FVector::ZeroVector;

	// === GASP State (cached from UGMCMotion) ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	EGMCMotion_RotationMode E_RotationMode = EGMCMotion_RotationMode::VelocityDirection;

	// uint8 so AnimBP can compare against BP enum E_MovementDirection directly (byte values match).
	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	uint8 E_MovementDirection = 0;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	EGMCMotion_Gait E_Gait = EGMCMotion_Gait::Run;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	FRotator GASPAimingRotation = FRotator::ZeroRotator;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	FVector GASPOrientationIntent = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	double GASPRotationOffset = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	double GASPTurningStrength = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	FVector GASPInputAcceleration = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP")
	FRotator GASPDesiredFacing = FRotator::ZeroRotator;

	// === GASP Trajectory ===

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP|Trajectory")
	FVector GASPFutureVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP|Trajectory")
	FVector GASPNearFutureVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP|Trajectory")
	float GASPTurnAngle = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP|Trajectory")
	float GASPAngularVelocityRad = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP|Trajectory")
	float GASPFutureFacingDelta = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|GASP|Trajectory")
	bool bGASPIsCircling = false;

	// === GMAS Integration ===

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Ability System")
	UGMC_AbilitySystemComponent* GetAbilitySystemComponent() const { return AbilitySystemComponent; }

protected:
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, AdvancedDisplay, Category = "Ability System", meta=(EditConditionHides))
	UGMC_AbilitySystemComponent* AbilitySystemComponent;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ability System")
	FGMCGameplayElementTagPropertyMap TagPropertyMap;

	friend UGMC_AbilitySystemComponent;

private:
	FVector PreviousVelocity = FVector::ZeroVector;
	float PreviousAimYaw = 0.f;

	void CacheComponentReferences();
	void UpdateLocomotionState();
	void UpdateVelocityAndAcceleration(float DeltaSeconds);
	void UpdateCachedAimOffset();
	void UpdateDistanceMatching();
	void UpdateTrajectory();
	void UpdateStrideWarpScale();
	void UpdateGASPState();
};
