// Copyright 2026 Execute Games, all rights reserved

#include "Animation/GMCMotion_AnimInstance.h"

#include "GMCMotionLog.h"
#include "Components/GMCMotion.h"
#include "Actors/GMCPawn.h"
#include "Components/GMCOrganicMovementComponent.h"
#include "GMCAbilityComponent.h"
#include "Utility/GMASUtilities.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "AnimationWarpingLibrary.h"
#include "BoneControllers/AnimNode_OffsetRootBone.h"
#if WITH_EDITOR
#include "PoseSearch/PoseSearchDerivedData.h"
#include "UObject/UObjectHash.h"
#endif

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
	WarmupPoseSearchIndexes();

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
	UpdateBasedMovement();

	BlueprintUpdateAnimation(DeltaSeconds);
}

// -----------------------------------------------------------------------------
// Based Movement (moving platforms)
// -----------------------------------------------------------------------------

void UGMCMotion_AnimInstance::UpdateBasedMovement()
{
	BasedMovementDeltaTranslation = FVector::ZeroVector;
	BasedMovementDeltaRotation = FQuat::Identity;

	const UPrimitiveComponent* Base = OwnerPawn ? OwnerPawn->GetMovementBase() : nullptr;
	if (!Base)
	{
		LastMovementBase.Reset();
		return;
	}

	const FTransform BaseTransform = Base->GetComponentTransform();
	if (LastMovementBase.Get() == Base)
	{
		// A point riding the base moves from P to (inverse old base, then new base) applied to P.
		const FTransform Delta = LastMovementBaseTransform.Inverse() * BaseTransform;
		if (const USkeletalMeshComponent* Mesh = GetSkelMeshComponent())
		{
			const FVector Root = Mesh->GetComponentLocation();
			BasedMovementDeltaTranslation = Delta.TransformPosition(Root) - Root;
		}
		BasedMovementDeltaRotation = Delta.GetRotation();
	}
	LastMovementBase = Base;
	LastMovementBaseTransform = BaseTransform;
}

void UGMCMotion_AnimInstance::OnUpdateOffsetRootBone(const FAnimUpdateContext& Context, const FAnimNodeReference& Node)
{
	if (BasedMovementDeltaTranslation.IsZero() && BasedMovementDeltaRotation.Equals(FQuat::Identity))
	{
		return;
	}
	// Same as UAnimationWarpingLibrary::ApplyDeltaToOffsetRootBone, which is not exported from its module (and neither is
	// FAnimNode_OffsetRootBone::StaticStruct): the exported conversion checks the node type, then the inline accessors.
	FOffsetRootBoneAnimNodeReference OffsetRootBone;
	bool bResult = false;
	UAnimationWarpingLibrary::ConvertToOffsetRootBoneNodePure(Node, OffsetRootBone, bResult);
	if (!bResult)
	{
		return;
	}
	if (FAnimNode_Base* Base = OffsetRootBone.GetAnimNodePtr<FAnimNode_Base>())
	{
		FAnimNode_OffsetRootBone* OffsetRoot = static_cast<FAnimNode_OffsetRootBone*>(Base);
		OffsetRoot->SetOffsetRootTranslation(OffsetRoot->GetOffsetRootTranslation() + BasedMovementDeltaTranslation);
		OffsetRoot->SetOffsetRootRotation(BasedMovementDeltaRotation * OffsetRoot->GetOffsetRootRotation());
	}
}

void UGMCMotion_AnimInstance::WarmupPoseSearchIndexes() const
{
#if WITH_EDITOR
	// A chooser's PoseSearch column databases are subobjects of its root chooser.
	for (const UObject* Chooser : PoseSearchIndexWarmupChoosers)
	{
		if (!Chooser)
		{
			continue;
		}
		TArray<UObject*> Inner;
		GetObjectsWithOuter(Chooser, Inner, true);
		for (const UObject* Object : Inner)
		{
			if (const UPoseSearchDatabase* Database = Cast<UPoseSearchDatabase>(Object))
			{
				UE::PoseSearch::FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(Database, UE::PoseSearch::ERequestAsyncBuildFlag::ContinueRequest);
			}
		}
	}
#endif
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
	// Map GMC movement mode to GASP E_MovementMode. Sliding = the steep slope slide (UGMCMotion::SlopeSlideMovementMode);
	// the movement mode is GMC-replicated, so simulated proxies get it too.
	if (MovementComponent->bTraversalWarpActive)
	{
		E_MovementMode = EGMCMotion_MovementMode::Traversing;
	}
	else if (MovementComponent->IsSlopeSliding())
	{
		E_MovementMode = EGMCMotion_MovementMode::Sliding;
	}
	else if (MovementComponent->GetMovementMode() == EGMC_MovementMode::Grounded
		|| MovementComponent->GetMovementMode() == UGMCMotion::RagdollMovementMode || MovementComponent->IsRagdollMode())
	{
		// The ragdoll mode counts as on the ground (GASP58's locomotion stays grounded while ragdolling): mapped to InAir,
		// the locomotion under the ragdoll played the fall loop, then the land at the get-up, which showed through the
		// get-up montage's blend-in (jolt at the start of the get-up, user 2026-09-30, seen in the Rewind Debugger).
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
	bIsInAir = !bIsOnGround && E_MovementMode != EGMCMotion_MovementMode::Sliding;
	LastLandingVelocity = MovementComponent->LastLandingVelocity;
	LandVelocityZ = LastLandingVelocity.Z;

	ComponentYawRate = MovementComponent->GetCurrentComponentYawRate();
	AnimAcceleration = MovementComponent->GetCurrentAnimationAcceleration();

	// Floor hit data from the movement component's current floor trace (queried here on simulated proxies, where GMC
	// doesn't update it).
	MovementComponent->UpdateSimulatedProxyFloor();
	const FGMC_FloorParams& Floor = MovementComponent->GetCurrentFloor();
	if (Floor.HasValidShapeData())
	{
		const FHitResult& ShapeHit = Floor.ShapeHit();
		FloorHitLocation = ShapeHit.ImpactPoint;
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
	bGASPDowned = MovementComponent->Downed;

	// Periodic diagnostic log (every ~1s at 60fps).
	static uint32 FrameCounter = 0;
	if (++FrameCounter % 60 == 0)
	{
		UE_LOG(LogGMCMotion, Log,
			TEXT("[AnimInst GASP] Dir=%d Rot=%d Gait=%d RotOffset=%.1f "
				 "InputAccel=(%.0f,%.0f,%.0f) OrientIntent=(%.2f,%.2f,%.2f) "
				 "AimRot=(P=%.1f,Y=%.1f) DesiredFacing=(P=%.1f,Y=%.1f) "
				 "TurnAngle=%.1f AngVelRad=%.2f FutureFacingDelta=%.1f IsCircling=%d "
				 "Speed=%.0f GroundSpeed=%.0f LocoAngle=%.1f"),
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

// -----------------------------------------------------------------------------
// Slopes
// -----------------------------------------------------------------------------

float UGMCMotion_AnimInstance::GetAnimationAssetPlayLength(const UAnimationAsset* Asset)
{
	return Asset ? Asset->GetPlayLength() : 0.f;
}

