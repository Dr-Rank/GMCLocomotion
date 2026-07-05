// Copyright 2026 Execute Games, all rights reserved

#include "Animation/GMCMotion_AnimInstance.h"

#include "GMCMotionLog.h"
#include "Components/GMCMotion.h"
#include "Actors/GMCPawn.h"
#include "Components/GMCOrganicMovementComponent.h"
#include "GMCAbilityComponent.h"
#include "Utility/GMASUtilities.h"

// -----------------------------------------------------------------------------
// Constructor / Destructor
// -----------------------------------------------------------------------------

UGMCMotion_AnimInstance::UGMCMotion_AnimInstance(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
#if WITH_EDITOR
	UGMASUtilities::ClearPropertyFlagsSafe(StaticClass(), TEXT("AbilitySystemComponent"), CPF_SimpleDisplay | CPF_Edit);
#endif
}

UGMCMotion_AnimInstance::~UGMCMotion_AnimInstance()
{
	TagPropertyMap.Reset();
}

// -----------------------------------------------------------------------------
// Initialization
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	CacheComponentReferences();

	// --- GMAS discovery ---
	bool bShouldInitializeProperties = true;

	if (!AbilitySystemComponent)
	{
		AActor* OwnerActor = GetOwningActor();
		if (OwnerActor)
		{
			AbilitySystemComponent = Cast<UGMC_AbilitySystemComponent>(
				OwnerActor->GetComponentByClass(UGMC_AbilitySystemComponent::StaticClass()));
		}

#if WITH_EDITOR
		if (!GetWorld()->IsGameWorld() && !IsValid(AbilitySystemComponent))
		{
			if (IsValid(OwnerPawn))
			{
				AbilitySystemComponent = Cast<UGMC_AbilitySystemComponent>(
					OwnerPawn->GetComponentByClass(UGMC_AbilitySystemComponent::StaticClass()));
			}

			if (!IsValid(AbilitySystemComponent))
			{
				bShouldInitializeProperties = false;
				AbilitySystemComponent = NewObject<UGMC_AbilitySystemComponent>();
			}
		}
#endif
	}

	if (AbilitySystemComponent)
	{
		if (bShouldInitializeProperties)
		{
			TagPropertyMap.Initialize(this, AbilitySystemComponent);
		}
		else
		{
			UE_LOG(LogGMCMotion, Log, TEXT("%s: skipping property map initialization in editor preview."),
				*GetClass()->GetName());
		}
	}
	else
	{
		if (GetWorld()->IsGameWorld())
		{
			UE_LOG(LogGMCMotion, Warning, TEXT("%s: unable to find GMC Ability System Component on %s. Will retry in BeginPlay."),
				*GetClass()->GetName(), GetOwningActor() ? *GetOwningActor()->GetName() : TEXT("NULL"));
		}
	}
}

void UGMCMotion_AnimInstance::CacheComponentReferences()
{
	AActor* OwningActor = GetOwningActor();
	if (!OwningActor)
	{
		return;
	}

	if (!OwnerPawn)
	{
		OwnerPawn = Cast<AGMC_Pawn>(TryGetPawnOwner());
	}

	if (!OwnerPawn)
	{
		if (GetWorld() && GetWorld()->IsGameWorld())
		{
			UE_LOG(LogGMCMotion, Warning,
				TEXT("UGMCMotion_AnimInstance::CacheComponentReferences - Could not find AGMC_Pawn for %s."),
				*OwningActor->GetName());
		}
		return;
	}

	if (!MovementComponent)
	{
		MovementComponent = OwnerPawn->FindComponentByClass<UGMCMotion>();
		if (!MovementComponent)
		{
			UE_LOG(LogGMCMotion, Warning,
				TEXT("UGMCMotion_AnimInstance::CacheComponentReferences - Could not find UGMCMotion on %s."),
				*OwnerPawn->GetName());
		}
	}
}

// -----------------------------------------------------------------------------
// BeginPlay (GMAS fallback)
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::NativeBeginPlay()
{
	Super::NativeBeginPlay();

	if (OwnerPawn && !AbilitySystemComponent)
	{
		AbilitySystemComponent = Cast<UGMC_AbilitySystemComponent>(
			OwnerPawn->GetComponentByClass(UGMC_AbilitySystemComponent::StaticClass()));

		if (!AbilitySystemComponent)
		{
			UE_LOG(LogGMCMotion, Error, TEXT("%s: failed to find GMC Ability System Component on %s in BeginPlay fallback."),
				*GetClass()->GetPathName(), *OwnerPawn->GetName());
			return;
		}

		TagPropertyMap.Initialize(this, AbilitySystemComponent);
	}
}

// -----------------------------------------------------------------------------
// Game Thread Update
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	// Guard against null UpdatedComponent (ragdoll/death).
	if (MovementComponent && !MovementComponent->UpdatedComponent)
	{
		UAnimInstance::NativeUpdateAnimation(DeltaSeconds);
		return;
	}

	Super::NativeUpdateAnimation(DeltaSeconds);

	// Lazy init if components were added after NativeInitializeAnimation.
	if (!MovementComponent)
	{
		CacheComponentReferences();
		if (!MovementComponent)
		{
			return;
		}
	}

	// Guard again after lazy init.
	if (!MovementComponent->UpdatedComponent)
	{
		return;
	}

	UpdateLocomotionState();
	UpdateVelocityAndAcceleration(DeltaSeconds);
	UpdateCachedAimOffset();
	UpdateDistanceMatching();
	UpdateTrajectory();
	UpdateStrideWarpScale();
	UpdateGASPState();

	BlueprintUpdateAnimation(DeltaSeconds);
}

// -----------------------------------------------------------------------------
// Thread-Safe Update
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::NativeThreadSafeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeThreadSafeUpdateAnimation(DeltaSeconds);
}

// -----------------------------------------------------------------------------
// Locomotion State
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::UpdateLocomotionState()
{
	// GMC core state.
	ActorTransformGMC = MovementComponent->GetActorTransform_GMC();
	InputAcceleration = MovementComponent->GetInputAcceleration();
	GMC_Velocity = MovementComponent->GetVelocity();
	// Map GMC movement mode to GASP E_MovementMode.
	// Sliding is BP-driven (set externally via BlueprintReadWrite); C++ handles the rest.
	if (MovementComponent->bTraversalWarpActive)
	{
		E_MovementMode = EGMCMotion_MovementMode::Traversing;
	}
	else if (MovementComponent->GetMovementMode() == EGMC_MovementMode::Grounded)
	{
		E_MovementMode = EGMCMotion_MovementMode::OnGround;
	}
	else
	{
		E_MovementMode = EGMCMotion_MovementMode::InAir;
	}

	Speed = GMC_Velocity.Size();
	GroundSpeed = MovementComponent->ReplicatedSpeed;
	bIsMoving = GroundSpeed > 1.f;
	bHasInput = MovementComponent->IsInputPresent(true);

	CurrentGait = MovementComponent->CurrentGait;
	CurrentStance = MovementComponent->CurrentStance;
	LocomotionMode = MovementComponent->LocomotionMode;
	LocomotionAngle = MovementComponent->ReplicatedLocomotionAngle;

	bIsOnGround = E_MovementMode == EGMCMotion_MovementMode::OnGround;
	bIsInAir = !bIsOnGround;
	LastLandingVelocity = MovementComponent->LastLandingVelocity;
	LandVelocityZ = LastLandingVelocity.Z;

	ComponentYawRate = MovementComponent->GetCurrentComponentYawRate();
	AnimAcceleration = MovementComponent->GetCurrentAnimationAcceleration();

	// Floor hit data from the movement component's current floor trace.
	const FGMC_FloorParams& Floor = MovementComponent->GetCurrentFloor();
	if (Floor.HasValidShapeData())
	{
		const FHitResult& ShapeHit = Floor.ShapeHit();
		FloorHitLocation = ShapeHit.Location;
		FloorHitNormal = ShapeHit.Normal;
		FloorHitImpactNormal = ShapeHit.ImpactNormal;
	}

	// Traversal hand IK targets — smooth the alpha for clean blend transitions.
	const float TargetHandIKAlpha = MovementComponent->TraversalHandIKAlpha;
	TraversalHandIKAlpha = FMath::FInterpTo(TraversalHandIKAlpha, TargetHandIKAlpha, GetDeltaSeconds(), 10.f);
	TraversalHandIK_LeftTarget = MovementComponent->TraversalHandIK_LeftTarget;
	TraversalHandIK_RightTarget = MovementComponent->TraversalHandIK_RightTarget;
}

// -----------------------------------------------------------------------------
// Velocity & Acceleration
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::UpdateVelocityAndAcceleration(float DeltaSeconds)
{
	WorldVelocity = GMC_Velocity;

	if (OwnerPawn)
	{
		LocalVelocity = OwnerPawn->GetActorRotation().UnrotateVector(WorldVelocity);
	}

	if (DeltaSeconds > SMALL_NUMBER)
	{
		WorldAcceleration = (WorldVelocity - PreviousVelocity) / DeltaSeconds;
	}

	PreviousVelocity = WorldVelocity;
}

// -----------------------------------------------------------------------------
// Aim Offset
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::UpdateCachedAimOffset()
{
	if (!OwnerPawn)
	{
		return;
	}

	const FRotator ControllerRotation = MovementComponent->GetCurrentAimRotation();
	const FRotator ActorRotation = OwnerPawn->GetActorRotation();

	CachedAimOffset.Pitch = FMath::ClampAngle(ControllerRotation.Pitch, -90.f, 90.f);
	CachedAimOffset.Yaw = FMath::ClampAngle(
		FRotator::NormalizeAxis(ControllerRotation.Yaw - ActorRotation.Yaw),
		-180.f, 180.f);
	CachedAimOffset.Roll = 0.f;

	AimYawRate = MovementComponent->GetCurrentAimYawRate();
}

// -----------------------------------------------------------------------------
// Distance Matching
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::UpdateDistanceMatching()
{
	FVector StopPrediction;
	bIsStopping = MovementComponent->IsStopPredicted(StopPrediction);
	PredictedStopDistance = bIsStopping ? StopPrediction.Size() : 0.f;

	FVector PivotPrediction;
	bIsPivoting = MovementComponent->IsPivotPredicted(PivotPrediction);
	PredictedPivotDistance = bIsPivoting ? PivotPrediction.Size() : 0.f;
}

// -----------------------------------------------------------------------------
// Trajectory (Pose Search)
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::UpdateTrajectory()
{
	// Trajectory is handled by UGMC_TrajectoryPredictor (UObject, not a component)
	// via the IPoseSearchTrajectoryPredictorInterface. No component lookup needed.
}

// -----------------------------------------------------------------------------
// Stride Warp Scale
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::UpdateStrideWarpScale()
{
	StrideWarpScale = 1.0f;
}

// -----------------------------------------------------------------------------
// GASP State (cached from UGMCMotion)
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::UpdateGASPState()
{
	if (!MovementComponent)
	{
		return;
	}

	// Direct C++ member reads — these are either UPROPERTY or plain members on UGMCMotion.
	// Since this is the same module, friend access isn't needed — they're public.
	E_RotationMode = MovementComponent->RotationMode;
	E_MovementDirection = MovementComponent->MovementDirection;
	E_Gait = MovementComponent->CurrentGait;
	GASPAimingRotation = MovementComponent->CachedAimingRotation;
	GASPOrientationIntent = MovementComponent->OrientationIntent;
	GASPInputAcceleration = MovementComponent->CustomComputeInputAcceleration;
	GASPDesiredFacing = MovementComponent->OverridenDesiredFacing;
	GASPRotationOffset = MovementComponent->RotationOffset;
	GASPTurningStrength = MovementComponent->TurningStrength;
	GASPFutureVelocity = MovementComponent->Trj_FutureVelocity;
	GASPNearFutureVelocity = MovementComponent->Trj_NearFutureVelocity;
	GASPTurnAngle = MovementComponent->Trj_TurnAngle;
	GASPAngularVelocityRad = MovementComponent->AngularVelocityRad;
	GASPFutureFacingDelta = MovementComponent->FutureFacingDelta;
	bGASPIsCircling = MovementComponent->Trj_IsCircling;

	// Periodic diagnostic log (every ~1s at 60fps).
	static uint32 FrameCounter = 0;
	if (++FrameCounter % 60 == 0)
	{
		UE_LOG(LogGMCMotion, Log,
			TEXT("[AnimInst GASP] Pipeline=%s Dir=%d Rot=%d Gait=%d RotOffset=%.1f "
				 "InputAccel=(%.0f,%.0f,%.0f) OrientIntent=(%.2f,%.2f,%.2f) "
				 "AimRot=(P=%.1f,Y=%.1f) DesiredFacing=(P=%.1f,Y=%.1f) "
				 "TurnAngle=%.1f AngVelRad=%.2f FutureFacingDelta=%.1f IsCircling=%d "
				 "Speed=%.0f GroundSpeed=%.0f LocoAngle=%.1f"),
			MovementComponent->bEnableGASPPipeline ? TEXT("C++") : TEXT("BP"),
			(int32)E_MovementDirection,
			(int32)E_RotationMode,
			(int32)E_Gait,
			GASPRotationOffset,
			GASPInputAcceleration.X, GASPInputAcceleration.Y, GASPInputAcceleration.Z,
			GASPOrientationIntent.X, GASPOrientationIntent.Y, GASPOrientationIntent.Z,
			GASPAimingRotation.Pitch, GASPAimingRotation.Yaw,
			GASPDesiredFacing.Pitch, GASPDesiredFacing.Yaw,
			GASPTurnAngle, GASPAngularVelocityRad, GASPFutureFacingDelta,
			bGASPIsCircling ? 1 : 0,
			Speed, GroundSpeed, LocomotionAngle);
	}
}
