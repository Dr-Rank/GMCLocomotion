// GMCMotion - GMC traversal movement component.

#include "Components/GMCMotion.h"
#include "Components/GMCMotionControlLoss.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "GMCMotionLog.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Misc/ScopeExit.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/GMCFlatCapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "AnimNotifyState_MotionWarping.h"
#include "RootMotionModifier.h"
#include "MotionWarpingComponent.h"   // UMotionWarpingUtilities::ExtractComponentSpacePose
#include "BonePose.h"
#include "BoneContainer.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "UObject/UnrealType.h"
#include "Curves/CurveFloat.h"
#include "Math/SpringMath.h"

void UGMCMotion::PhysicsCustom_Implementation(float DeltaSeconds)
{
	if (GetMovementMode() == RagdollMovementMode)
	{
		PhysicsRagdoll(DeltaSeconds);
		return;
	}

	if (GetMovementMode() == SlopeSlideMovementMode)
	{
		PhysicsSlopeSlide(DeltaSeconds);
		return;
	}

	// Traversal (Custom1).
	// Mirror PhysicsAirborne exactly, only swapping the move call for the no-landing variant.
	// Velocity here is the root-motion velocity set by the previous frame's MontageUpdate (the warp's
	// per-frame delta), so this integration moves the capsule along the warped path just like Airborne.
	const FVector OldVelocity = Velocity;
	CalculateVelocity(DeltaSeconds);

	const FVector LocationDelta = 0.5 * (OldVelocity + Velocity) * DeltaSeconds;
	MoveThroughAirNoLanding(LocationDelta, DeltaSeconds);
}

bool UGMCMotion::UpdateMovementModeDynamic_Implementation(FGMC_FloorParams& Floor, float DeltaSeconds)
{
	// Set the obstacle collision-ignore HERE (start of the movement cycle, BEFORE RunPhysics moves the
	// capsule) rather than in PostMovementUpdate. UpdateMovementModeDynamic runs every frame including
	// client replay/re-sim, so the ignore state is reconstructed before each move -> deterministic (the
	// PostMovementUpdate timing left the first replayed move with a stale ignore = the old jitter).
	UpdateTraversalCollisionIgnore();

	// Widen GMC's network error tolerances during the traversal (+ grace) so the warped move's legitimate
	// client/server divergence doesn't trip a correction; restored after. Server still validates (bounded).
	UpdateTraversalErrorTolerances(DeltaSeconds);

	// Ragdoll: held for as long as the server says so (bRagdollMode is GMC-bound), except over death (None).
	if (PawnOwner && PawnOwner->HasAuthority())
	{
		bRagdollMode = bRagdollModeRequest;
	}
	if (GetMovementMode() != EGMC_MovementMode::None)
	{
		if (bRagdollMode)
		{
			if (GetMovementMode() != RagdollMovementMode)
			{
				SetMovementMode(RagdollMovementMode);
			}
			return true;
		}
		if (GetMovementMode() == RagdollMovementMode)
		{
			// Back to the base update, which grounds the pawn on the floor under the capsule.
			SetMovementMode(EGMC_MovementMode::Airborne);
		}
	}

	if (bTraversalWarpActive)
	{
		// ─── Equipment gate ─────────────────────────────────────────────────────
		// When bEquipmentBlocksTraversal is true (weapon in hand, game mode requires holster),
		// block the traversal unless bTraversalExemptFromEquipmentGate was set by
		// TraversalHolster (montage is in the skip-holster set, e.g. hurdles/vaults).
		//
		// TraversalHolster runs during PrePhysicsUpdate (via BP), which GMC guarantees
		// executes BEFORE UpdateMovementModeDynamic. The exempt flag is therefore
		// already set correctly for this frame by the time we read it here.
		//
		// NOTE: We do NOT try to identify the active montage here. On the first frame
		// where bTraversalWarpActive is true, the traversal montage is often not yet in
		// the montage tracker, and GetCurrentActiveMontage() can return a weapon hold/idle
		// montage — which would incorrectly override the exempt flag to false.
		if (bEquipmentBlocksTraversal && !bTraversalExemptFromEquipmentGate)
		{
			UE_LOG(LogGMCMotion, Warning,
				TEXT("[TWarp] Equipment gate BLOCKED traversal. Weapon must be holstered first."));

			bTraversalWarpActive = false;
			bTraversalExemptFromEquipmentGate = false;
			WantsToTraverse = false;

			// Stop any traversal montage that may have started
			if (SkeletalMesh)
			{
				if (UAnimMontage* ActiveMontage = GetActiveRootMotionMontage(MontageTracker))
				{
					StopMontage(SkeletalMesh, MontageTracker, 0.15f);
				}
			}

			WarpFrontLedge = FVector::ZeroVector;
			WarpBackLedge = FVector::ZeroVector;
			WarpBackFloor = FVector::ZeroVector;
			WarpFrontLedgeNormal = FVector::ZeroVector;

			bPrevTraversalWarpActive_Gate = false;
			return Super::UpdateMovementModeDynamic_Implementation(Floor, DeltaSeconds);
		}

		// Log on first frame of an exempt traversal (diagnostic)
		if (bEquipmentBlocksTraversal && bTraversalExemptFromEquipmentGate && !bPrevTraversalWarpActive_Gate)
		{
			UE_LOG(LogGMCMotion, Warning,
				TEXT("[TWarp] Equipment gate PASSED — traversal exempt (skip-holster montage)."));
		}

		bPrevTraversalWarpActive_Gate = true;

		// Force Custom1 (TraversalMovementMode) for ALL traversals — both reach and jump-over.
		// PhysicsCustom routes through MoveThroughAirNoLanding which sets bSweep=false during the warp,
		// preventing PhysicsGrounded from stripping Z (floor projection) or PhysicsAirborne from
		// re-landing the pawn (MoveThroughAir uses sweep=true). Custom1 is the only physics path
		// that bypasses collision during the warp.
		//
		// Returning true makes GMC skip UpdateMovementModeStatic, so Airborne<->Grounded auto-switching
		// (and the ProcessLanded it triggers when brushing a walkable floor) never runs: no landing/fall
		// anim, no capsule-resize flip-flop. BP sets bTraversalWarpActive true at start and false at end;
		// the Custom1->Grounded transition on exit does NOT fire a landing (ProcessLanded only runs from
		// the Static path on IsAirborne()).
		//
		// For reach montages: the warp positions the capsule at ledge height; when bTraversalWarpActive
		// goes false, Super transitions Custom1->Grounded via normal grounding onto the ledge surface.
		// Deterministic: bTraversalWarpActive is a GMC bound input.
		if (GetMovementMode() != TraversalMovementMode)
		{
			SetMovementMode(TraversalMovementMode);
		}
		return true;
	}

	// Trailing edge: traversal just ended (bTraversalWarpActive went true→false).
	// Clear the exempt flag so it doesn't leak to the next traversal attempt.
	// Safe because TraversalHolster (which SETS the flag) runs during PrePhysicsUpdate,
	// which executes BEFORE UpdateMovementModeDynamic. A new traversal's exempt flag is
	// always set before the gate reads it; this trailing-edge clear only affects flags
	// from completed traversals.
	if (bPrevTraversalWarpActive_Gate)
	{
		bTraversalExemptFromEquipmentGate = false;
		bPrevTraversalWarpActive_Gate = false;
	}

	if (UpdateSlopeSlideMode(Floor))
	{
		return true;
	}

	return Super::UpdateMovementModeDynamic_Implementation(Floor, DeltaSeconds);
}

// -----------------------------------------------------------------------------
// Steep slope slide (SlopeSlideMovementMode)
// -----------------------------------------------------------------------------

bool UGMCMotion::GetSlopeSlideFloor(const FGMC_FloorParams& Floor, float ExtraRange, FVector& OutNormal, float& OutAngle) const
{
	const float ContactRange = MAX_DISTANCE_TO_FLOOR + FMath::Max(ExtraRange, 0.f);
	if (!Floor.HasValidShapeData() || !Floor.ShapeHit().IsValidBlockingHit() || Floor.GetShapeDistanceToFloor() > ContactRange)
	{
		return false;
	}

	// The line starts under the collision centre, which on a slope is further from the plane than the shape's contact
	// point (radius * (1 / cos(angle) - 1): 14.5 cm for a 35 cm radius at 45 deg), hence the extra margin.
	constexpr float LineMargin = 25.f;
	const bool bUseLine = Floor.HasValidLineData()
		&& Floor.LineHit().IsValidBlockingHit()
		&& Floor.GetLineDistanceToFloor() <= ContactRange + LineMargin;

	OutNormal = (bUseLine ? Floor.LineHit() : Floor.ShapeHit()).ImpactNormal.GetSafeNormal();
	if (OutNormal.IsNearlyZero())
	{
		return false;
	}
	OutAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(OutNormal.Z, -1., 1.)));
	return true;
}

bool UGMCMotion::IsSlopeSlideBump(const FGMC_FloorParams& Floor, const FVector& FloorNormal) const
{
	const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(UpdatedComponent);
	UWorld* World = GetWorld();
	const FVector Downhill = FloorNormal.GetSafeNormal2D();
	if (SlopeSlideBumpProbeDistance <= 0.f || !Primitive || !World || Downhill.IsNearlyZero() || !Floor.HasValidShapeData())
	{
		return false;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(SlopeSlideBumpProbe), false, GetOwner());
	FCollisionResponseParams Response;
	Primitive->InitSweepCollisionParams(Params, Response);
	if (const AActor* Owner = GetOwner())
	{
		TArray<AActor*> Attached;
		Owner->GetAttachedActors(Attached, true, true);
		Params.AddIgnoredActors(Attached);
	}

	// A little above the contact (the ground past a bump is lower), down to the step height under it.
	const FVector Contact = Floor.ShapeHit().ImpactPoint;
	const FVector Probe = Contact + Downhill * SlopeSlideBumpProbeDistance;
	const FVector Start(Probe.X, Probe.Y, Contact.Z + 5.);
	const FVector End(Probe.X, Probe.Y, Contact.Z - GetMaxStepUpHeight());
	FHitResult Hit;
	if (!World->LineTraceSingleByChannel(Hit, Start, End, Primitive->GetCollisionObjectType(), Params, Response) || Hit.bStartPenetrating)
	{
		// Nothing within the step height (a real slope, or a drop): slide.
		return false;
	}
	// Still the steep surface there: a slope at least SlopeSlideBumpProbeDistance long.
	return Hit.ImpactNormal.Z >= FMath::Cos(FMath::DegreesToRadians(SlopeSlideStartAngle));
}

bool UGMCMotion::UpdateSlopeSlideMode(const FGMC_FloorParams& Floor)
{
	const bool bSliding = GetMovementMode() == SlopeSlideMovementMode;

	if (!SlopeSlideRequestProperty.IsNone())
	{
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(GetClass(), SlopeSlideRequestProperty);
		bSlopeSlideRequested = Property && Property->GetPropertyValue_InContainer(this);
	}

	FVector FloorNormal;
	float FloorAngle = 0.f;
	const bool bHasFloor = IsSlopeSlideAllowed() && GetSlopeSlideFloor(Floor, bSliding ? SlopeSlideSnapDistance : 0.f, FloorNormal, FloorAngle);

	bool bWantsSlide = false;
	bool bRequestedEntry = false;
	if (bHasFloor)
	{
		if (bSliding)
		{
			// Steep enough, or still running out (see SlopeSlideExitSpeed) - and, for a requested slide, still held.
			bWantsSlide = ShouldKeepSlopeSlide(bSlopeSlideRequested, bSlopeSlideEndOnRelease && !bSlopeSlideAutoStart,
				FloorAngle, SlopeSlideStopAngle, Velocity.Size(), SlopeSlideExitSpeed);
		}
		else if (IsMovingOnGround())
		{
			const bool bAuto = bSlopeSlideAutoStart && FloorAngle >= SlopeSlideStartAngle;
			bRequestedEntry = ShouldStartRequestedSlopeSlide(bSlopeSlideRequested, true, FloorAngle,
				SlopeSlideTriggerMinAngle, GetSlopeSlideDownhillSpeed(FloorNormal), SlopeSlideTriggerMinSpeed);
			bWantsSlide = (bAuto || bRequestedEntry) && !bReceivedUpwardForce;
		}
		else if (IsAirborne() && bSlopeSlideAutoStart)
		{
			// Landing on (or falling along) a steep slope, including unwalkable ones GMC would keep the pawn airborne on.
			bWantsSlide = FloorAngle >= SlopeSlideStartAngle && !bReceivedUpwardForce && Velocity.Z <= 0.;
		}

		// Entry only: the side of a log or a rock is steep but short, the pawn steps over it. Automatic starts only: the
		// probe looks SlopeSlideBumpProbeDistance downhill for ground within step height, which on a 20-30 deg ramp is
		// the ramp itself (18-29 cm lower at 50 cm) - it held a crouch-requested slide off until ~30 deg, measured
		// 0.8 s / 3.5 m late in CLEAR. A player asking to slide is not stepping over a log.
		if (bWantsSlide && !bSliding && !bRequestedEntry && IsSlopeSlideBump(Floor, FloorNormal))
		{
			bWantsSlide = false;
		}
	}

	bIsSliding = bWantsSlide;
	if (!bWantsSlide)
	{
		// Leaving the slide (or not sliding): GMC's static update grounds the pawn on a walkable floor, else Airborne.
		return false;
	}

	if (!bSliding)
	{
		// From the ground, carry the running speed into the slide (see ComputeSlopeSlideEntryVelocity). From the air the
		// pawn really does lose its speed into the slope, so PhysicsSlopeSlide's projection is left to handle it.
		if (IsMovingOnGround())
		{
			Velocity = ComputeSlopeSlideEntryVelocity(Velocity, FloorNormal);
		}
		SetMovementMode(SlopeSlideMovementMode);
	}
	return true;
}

bool UGMCMotion::IsSlopeSlideAllowed() const
{
	return bSlopeSlideEnabled
		&& !Downed
		&& GetRootCollisionShape() != EGMC_CollisionShape::HorizontalCapsule
		&& IsAffectedByGravity()
		&& CurrentImmersionDepth < BuoyantStateMinImmersion;
}

double UGMCMotion::GetSlopeSlideDownhillSpeed(const FVector& FloorNormal) const
{
	const FVector Gravity = GetGravity();
	const FVector FallLine = (Gravity - (Gravity | FloorNormal) * FloorNormal).GetSafeNormal();
	return FallLine.IsNearlyZero() ? 0. : (Velocity | FallLine);
}

void UGMCMotion::SetSlopeSlideRequested(bool bRequested)
{
	bSlopeSlideRequested = bRequested;
}

bool UGMCMotion::CanStartSlopeSlide() const
{
	FVector FloorNormal;
	float FloorAngle = 0.f;
	if (!IsSlopeSlideAllowed() || !IsMovingOnGround() || !GetSlopeSlideFloor(CurrentFloor, 0.f, FloorNormal, FloorAngle))
	{
		return false;
	}
	return ShouldStartRequestedSlopeSlide(true, true, FloorAngle, SlopeSlideTriggerMinAngle,
		GetSlopeSlideDownhillSpeed(FloorNormal), SlopeSlideTriggerMinSpeed);
}

bool UGMCMotion::ShouldStartRequestedSlopeSlide(bool bRequested, bool bOnGround, float FloorAngle, float MinAngle,
	double DownhillSpeed, double MinSpeed)
{
	return bRequested && bOnGround && FloorAngle >= MinAngle && DownhillSpeed >= MinSpeed;
}

bool UGMCMotion::ShouldKeepSlopeSlide(bool bRequested, bool bEndOnRelease, float FloorAngle, float StopAngle,
	double Speed, double ExitSpeed)
{
	if (bEndOnRelease && !bRequested)
	{
		return false;
	}
	return FloorAngle >= StopAngle || Speed > ExitSpeed;
}

FVector UGMCMotion::ComputeSlopeSlideEntryVelocity(const FVector& GroundVelocity, const FVector& FloorNormal)
{
	// Lifting divides by the normal's Z: past ~84 deg that blows up, so project instead (no slide starts there anyway).
	constexpr double MinNormalZ = 0.1;
	if (FloorNormal.Z < MinNormalZ)
	{
		return GroundVelocity - (GroundVelocity | FloorNormal) * FloorNormal;
	}
	FVector Lifted(GroundVelocity.X, GroundVelocity.Y, 0.);
	Lifted.Z = -(FloorNormal.X * Lifted.X + FloorNormal.Y * Lifted.Y) / FloorNormal.Z;
	return Lifted;
}

void UGMCMotion::UpdateSimulatedProxyFloor()
{
	if (!IsSimulatedProxy() || !UpdatedComponent)
	{
		return;
	}

	const EGMC_MovementMode Mode = GetMovementMode();
	if (Mode != EGMC_MovementMode::Grounded && Mode != SlopeSlideMovementMode)
	{
		return;
	}

	const FVector Location = UpdatedComponent->GetComponentLocation();
	if (CurrentFloor.HasValidShapeData() && FVector::DistSquared(Location, LastSimulatedFloorQueryLocation) < 1.)
	{
		return;
	}
	LastSimulatedFloorQueryLocation = Location;

	// Query only (no auto adjust): the simulated pawn's location stays GMC's interpolated one.
	UpdateFloor(CurrentFloor, FVector::DownVector, FloorTraceLength, FloorUpdateTolerance, FloorTraceShapeScale, false, true);
}

void UGMCMotion::PhysicsSlopeSlide(float DeltaSeconds)
{
	FVector FloorNormal = FVector::UpVector;
	float FloorAngle = 0.f;
	if (!GetSlopeSlideFloor(CurrentFloor, SlopeSlideSnapDistance, FloorNormal, FloorAngle))
	{
		FloorNormal = FVector::UpVector;
	}

	const FVector Gravity = GetGravity();
	const FVector GravityAlongFloor = Gravity - (Gravity | FloorNormal) * FloorNormal;
	const FVector FallLine = GravityAlongFloor.GetSafeNormal();

	// Velocity in the floor plane, pulled down the fall line, slowed by friction.
	FVector NewVelocity = Velocity - (Velocity | FloorNormal) * FloorNormal;
	NewVelocity += GravityAlongFloor * DeltaSeconds;

	const double Speed = NewVelocity.Size();
	if (Speed > UE_KINDA_SMALL_NUMBER)
	{
		const float Friction = FloorAngle < SlopeSlideStopAngle ? SlopeSlideRunOutFriction : SlopeSlideFriction;
		const double FrictionDeceleration = Friction * FMath::Abs(Gravity | FloorNormal);
		NewVelocity *= FMath::Max(Speed - FrictionDeceleration * DeltaSeconds, 0.) / Speed;
	}

	// Lateral steering only: the input along the fall line (braking or climbing) is ignored.
	if (!FallLine.IsNearlyZero())
	{
		const FVector Across = FVector::CrossProduct(FloorNormal, FallLine).GetSafeNormal();
		const float SteerInput = FMath::Clamp(static_cast<float>(GetProcessedInputVector() | Across), -1.f, 1.f);
		const float AcrossSpeed = NewVelocity | Across;
		const float NewAcrossSpeed = FMath::FInterpConstantTo(AcrossSpeed, SteerInput * SlopeSlideSteerSpeed, DeltaSeconds, SlopeSlideSteerAcceleration);
		NewVelocity += Across * (NewAcrossSpeed - AcrossSpeed);
	}

	NewVelocity = NewVelocity.GetClampedToMaxSize(SlopeSlideMaxSpeed);
	UpdateVelocity(NewVelocity, DeltaSeconds);

	// Face the slide direction (yaw only).
	FRotator NewRotation = UpdatedComponent->GetComponentRotation();
	if (NewVelocity.SizeSquared2D() > FMath::Square(10.))
	{
		const float TargetYaw = FVector(NewVelocity.X, NewVelocity.Y, 0.).Rotation().Yaw;
		NewRotation.Yaw = FMath::FixedTurn(NewRotation.Yaw, TargetYaw, SlopeSlideTurnRate * DeltaSeconds);
	}
	const FQuat NewQuat = FRotator(0., NewRotation.Yaw, 0.).Quaternion();

	const FVector LocationDelta = NewVelocity * DeltaSeconds;
	FHitResult Hit;
	SafeMoveUpdatedComponent(LocationDelta, NewQuat, true, Hit);
	if (Hit.IsValidBlockingHit())
	{
		HandleImpact(Hit, DeltaSeconds, LocationDelta);
		// Drop the velocity going into the obstacle, then slide along it for the rest of the move.
		const double IntoHit = Velocity | Hit.Normal;
		if (IntoHit < 0.)
		{
			UpdateVelocity(Velocity - IntoHit * Hit.Normal, DeltaSeconds);
		}
		SlideAlongSurface(LocationDelta, 1.f - Hit.Time, Hit.Normal, Hit, true);
	}

	// Stay on the surface (walkable or not) over crests and bumps; beyond the snap distance the next mode update makes
	// the pawn Airborne.
	UpdateFloor(CurrentFloor, FVector::DownVector, FloorTraceLength, FloorUpdateTolerance, FloorTraceShapeScale, false, true);
	if (CurrentFloor.HasValidShapeData() && CurrentFloor.GetShapeDistanceToFloor() <= MAX_DISTANCE_TO_FLOOR + SlopeSlideSnapDistance)
	{
		MaintainDistanceToFloor(CurrentFloor, true);
	}
}

bool UGMCMotion::IsValidLandingSpot(const FHitResult& Hit)
{
	// During a JUMP-OVER traversal (vault/hurdle) the pawn passes OVER the obstacle — it must NOT land on
	// the obstacle top. GMC's Airborne otherwise grounds the pawn onto any walkable surface just below it,
	// so as the vault arcs over, the obstacle top (a walkable surface right under the capsule) gets picked
	// as a landing spot and the capsule is snapped UP onto it. Reject any candidate clearly above the far-
	// floor target (= the obstacle top), so only the real landing floor (= BackFloor) is accepted.
	// Penetrating hits fall through to base (which resolves the penetration). Reach montages (climb/mantle)
	// are NOT jump-overs, so they land on the ledge as intended. Deterministic: replicated BackFloor + hit.
	if (bTraversalWarpActive
		&& bLastTraversalIsJumpOver
		&& !Hit.bStartPenetrating
		&& !WarpBackFloor.IsNearlyZero()
		&& Hit.ImpactPoint.Z > WarpBackFloor.Z + TraversalNoLandHeightAboveFloor)
	{
		if (bDebugTraversalWarp)
		{
			UE_LOG(LogGMCMotion, Warning,
				TEXT("[TLand] reject obstacle-top landing: impactZ=%.1f backFloorZ=%.1f thr=%.1f"),
				Hit.ImpactPoint.Z, WarpBackFloor.Z, WarpBackFloor.Z + TraversalNoLandHeightAboveFloor);
		}
		return false;
	}

	return Super::IsValidLandingSpot(Hit);
}

void UGMCMotion::UpdateTraversalErrorTolerances(float DeltaSeconds)
{
	FGMC_DefaultErrorTolerances& Tol = ReplicationSettings.DefaultErrorTolerances;

	// Capture the editor-configured originals once (first call, before any widening).
	if (!bTraversalToleranceDefaultsSaved)
	{
		SavedActorLocationTolerance = Tol.ActorLocation;
		SavedActorRotationTolerance = Tol.ActorRotation;
		SavedLinearVelocityTolerance = Tol.LinearVelocity;
		SavedAngularVelocityTolerance = Tol.AngularVelocity;
		bTraversalToleranceDefaultsSaved = true;
	}

	// Grace: keep the widened tolerances for a bit after the traversal / ragdoll ends so in-flight (RTT-late) client
	// validations of the last moves still use the wide tolerance.
	if (bTraversalWarpActive)
	{
		TraversalToleranceGraceRemaining = TraversalToleranceGraceTime;
	}
	else if (TraversalToleranceGraceRemaining > 0.0f)
	{
		TraversalToleranceGraceRemaining = FMath::Max(0.0f, TraversalToleranceGraceRemaining - DeltaSeconds);
	}
	// The ragdoll's grace is longer: the get-up leaves each machine's capsule where its own body lay.
	if (bRagdollMode)
	{
		RagdollToleranceGraceRemaining = FMath::Max(1.0f, TraversalToleranceGraceTime);
	}
	else if (RagdollToleranceGraceRemaining > 0.0f)
	{
		RagdollToleranceGraceRemaining = FMath::Max(0.0f, RagdollToleranceGraceRemaining - DeltaSeconds);
	}

	const bool bRagdollWiden = bRagdollMode || RagdollToleranceGraceRemaining > 0.0f;
	const bool bWiden = bRagdollWiden || bTraversalWarpActive || TraversalToleranceGraceRemaining > 0.0f;

	if (bWiden)
	{
		const float Loc = bRagdollWiden ? RagdollActorLocationTolerance : TraversalActorLocationTolerance;
		const float Rot = bRagdollWiden ? RagdollActorRotationTolerance : TraversalActorRotationTolerance;
		const float LVel = bRagdollWiden ? RagdollVelocityTolerance : TraversalLinearVelocityTolerance;
		const float AVel = bRagdollWiden ? RagdollVelocityTolerance : TraversalAngularVelocityTolerance;
		if (bDebugTraversalWarp && (!bTraversalToleranceWidened || Tol.ActorLocation != Loc))
		{
			UE_LOG(LogGMCMotion, Warning, TEXT("[TTol] WIDEN loc=%.0f rot=%.0f lvel=%.0f avel=%.0f"), Loc, Rot, LVel, AVel);
		}
		Tol.ActorLocation = Loc;
		Tol.ActorRotation = Rot;
		Tol.LinearVelocity = LVel;
		Tol.AngularVelocity = AVel;
		bTraversalToleranceWidened = true;
	}
	else if (bTraversalToleranceWidened)
	{
		Tol.ActorLocation = SavedActorLocationTolerance;
		Tol.ActorRotation = SavedActorRotationTolerance;
		Tol.LinearVelocity = SavedLinearVelocityTolerance;
		Tol.AngularVelocity = SavedAngularVelocityTolerance;
		bTraversalToleranceWidened = false;
		if (bDebugTraversalWarp)
		{
			UE_LOG(LogGMCMotion, Warning, TEXT("[TTol] RESTORE loc=%.1f rot=%.1f lvel=%.1f avel=%.1f"),
				SavedActorLocationTolerance, SavedActorRotationTolerance,
				SavedLinearVelocityTolerance, SavedAngularVelocityTolerance);
		}
	}
}

void UGMCMotion::SetRagdollCapsuleTarget(const FTransform& Target, bool bHold)
{
	RagdollCapsuleTarget = Target;
	bHasRagdollCapsuleTarget = true;
	bHoldRagdollCapsule = bHold;
	bHasRagdollCapsuleVelocity = false;
}

void UGMCMotion::SetRagdollCapsuleTargetWithVelocity(const FTransform& Target, const FVector& InVelocity)
{
	SetRagdollCapsuleTarget(Target, false);
	RagdollCapsuleVelocity = InVelocity;
	bHasRagdollCapsuleVelocity = true;
}

void UGMCMotion::PhysicsRagdoll(float DeltaSeconds)
{
	Velocity = FVector::ZeroVector;
	if (!bHasRagdollCapsuleTarget || bHoldRagdollCapsule || !UpdatedComponent)
	{
		return;
	}

	const FVector Current = UpdatedComponent->GetComponentLocation();
	const FVector Delta = RagdollCapsuleTarget.GetLocation() - Current;
	FRotator NewRotation = UpdatedComponent->GetComponentRotation();
	NewRotation.Yaw = RagdollCapsuleTarget.Rotator().Yaw;
	// No sweep: the ragdoll body already resolved the collisions, the capsule only has to stay on it.
	MoveUpdatedComponent(Delta, NewRotation.Quaternion(), false);
	if (bHasRagdollCapsuleVelocity)
	{
		Velocity = RagdollCapsuleVelocity;
	}
	else if (DeltaSeconds > UE_KINDA_SMALL_NUMBER)
	{
		Velocity = Delta / DeltaSeconds;
	}
}

bool UGMCMotion::MoveThroughAirNoLanding(const FVector& LocationDelta, float DeltaSeconds)
{
	if (LocationDelta.IsNearlyZero(UE_DOUBLE_KINDA_SMALL_NUMBER))
	{
		return false;
	}

	const FQuat PawnRotation = UpdatedComponent->GetComponentQuat();

	// During the traversal warp, move WITHOUT sweeping: the capsule follows the warped path exactly,
	// with NO collision against anything (obstacle included) -> no corner wedge on ANY variant, and
	// fully deterministic (pure warp math, zero runtime collision state). The warp drives a clean
	// over-the-obstacle arc (capped apex, far-floor landing target); grounding re-collides once the
	// traversal ends.
	if (bTraversalWarpActive)
	{
		MoveUpdatedComponent(LocationDelta, PawnRotation, /*bSweep*/ false);
		return true;
	}

	FHitResult Hit{};
	SafeMoveUpdatedComponent(LocationDelta, PawnRotation, true, Hit);

	float LastMoveTimeSlice = DeltaSeconds;
	float DeltaSecondsRemaining = DeltaSeconds * (1.f - Hit.Time);
	float DeltaSecondsApplied = DeltaSeconds - DeltaSecondsRemaining;

	if (Hit.bBlockingHit)
	{
		// Depenetrate first, exactly as Airborne did.
		if (Hit.bStartPenetrating)
		{
			ResolvePenetration(GetPenetrationAdjustment(Hit), Hit, PawnRotation);
		}

		// Always treat the hit as a wall to slide along — never ProcessLanded.
		HandleImpact(Hit, LastMoveTimeSlice, LocationDelta);
		AdjustVelocityFromHitAirborne_Implementation(Hit, DeltaSecondsApplied);

		const FVector OldHitNormal = Hit.Normal;
		const FVector SavedDelta = Velocity * DeltaSecondsRemaining;
		FVector Delta = ComputeSlideVector(LocationDelta, 1.f - Hit.Time, OldHitNormal, Hit);

		if (DeltaSecondsRemaining >= MIN_DELTA_TIME && !DirectionsDiffer(Delta, SavedDelta))
		{
			SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);

			if (Hit.bBlockingHit)
			{
				LastMoveTimeSlice = DeltaSecondsRemaining;
				DeltaSecondsRemaining *= 1.f - Hit.Time;
				DeltaSecondsApplied = DeltaSeconds - DeltaSecondsRemaining;

				if (Hit.bStartPenetrating)
				{
					ResolvePenetration(GetPenetrationAdjustment(Hit), Hit, PawnRotation);
				}

				HandleImpact(Hit, LastMoveTimeSlice, Delta);
				AdjustVelocityFromHitAirborne_Implementation(Hit, DeltaSecondsApplied);

				TwoWallAdjust(Delta, Hit, OldHitNormal);
				SafeMoveUpdatedComponent(Delta, PawnRotation, true, Hit);

				if (Hit.Time == 0.f)
				{
					FVector SideDelta = (OldHitNormal + Hit.ImpactNormal).GetSafeNormal2D();
					if (SideDelta.IsNearlyZero(UE_DOUBLE_KINDA_SMALL_NUMBER))
					{
						SideDelta = FVector(OldHitNormal.Y, -OldHitNormal.X, 0.).GetSafeNormal();
					}
					SafeMoveUpdatedComponent(SideDelta, PawnRotation, true, Hit);
				}
			}
		}
	}

	return true;
}

void UGMCMotion::PreProcessRootMotion(
	const FGMC_AnimMontageInstance& MontageInstance,
	FRootMotionMovementParams& InOutRootMotionParams,
	float MontageDelta,
	float DeltaSeconds)
{
	// Let the base scale the root motion and convert it to world space first.
	Super::PreProcessRootMotion(MontageInstance, InOutRootMotionParams, MontageDelta, DeltaSeconds);

	if (!bTraversalWarpActive || !InOutRootMotionParams.bHasRootMotion || !SkeletalMesh || !UpdatedComponent)
	{
		return;
	}

	// Safety net: during GMC replay/re-simulation, UpdateMovementModeDynamic may have
	// already run with a stale bTraversalWarpActive (false), leaving the mode as Grounded.
	// PreProcessRootMotion always sees the correct value (from the recorded move inputs),
	// so force Custom1 here before physics selects its function. Without this,
	// PhysicsGrounded strips Z and the warp displacement is lost.
	if (GetMovementMode() != TraversalMovementMode)
	{
		SetMovementMode(TraversalMovementMode);
	}

	// Default: no hand IK this frame. Overridden below if in FrontLedge window.
	TraversalHandIKAlpha = 0.f;

	UAnimMontage* Montage = MontageInstance.GetMontage();
	if (!Montage)
	{
		return;
	}

	// Determinism probe.
	if (bDebugTraversalWarp)
	{
		const FVector Cur = UpdatedComponent->GetComponentLocation();
		const FVector Vel = GetLinearVelocity_GMC();
		const int32 Auth = (GetOwner() && GetOwner()->HasAuthority()) ? 1 : 0;
		const int32 Mode = static_cast<int32>(GetMovementMode());
		UE_LOG(LogGMCMotion, Warning,
			TEXT("[TDet] auth=%d mode=%d replay=%d pos=%.3f | velIn=(%.0f,%.0f,%.0f) loc=(%.0f,%.0f,%.0f) yaw=%.1f tgt=(%.0f,%.0f) warpActive=%d"),
			Auth, Mode, CL_IsReplaying() ? 1 : 0, MontageInstance.GetPosition(),
			Vel.X, Vel.Y, Vel.Z, Cur.X, Cur.Y, Cur.Z,
			UpdatedComponent->GetComponentRotation().Yaw,
			WarpFrontLedge.X, WarpFrontLedge.Y, bTraversalWarpActive ? 1 : 0);

		DebugLogTraversalChoice(TEXT("PreRM"), false, nullptr, MontageInstance.GetPosition(), false, Montage, MontageInstance.GetPosition());
	}

	const float Position = MontageInstance.GetPosition();

	// Capture the animation's own horizontal velocity (the raw, un-warped root motion this frame, already
	// world-space after Super) so RestoreTraversalExitVelocity can hand it to locomotion at landing.
	// Freeze the capture once past the last landing window of jump-overs (tail phase): during tail-kill
	// and blend-out the raw RM decays toward zero, which would cause RestoreTraversalExitVelocity to
	// inject near-zero velocity → Motion Matching picks a walk/idle pose instead of the correct gait.
	{
		const float LastEnd = GetLastLandingWindowEnd(Montage);
		const bool bInTailPhase = (LastEnd > 0.0f && Position >= LastEnd);
		if (!bInTailPhase)
		{
			const FVector RawT = InOutRootMotionParams.GetRootMotionTransform().GetTranslation();
			// Z kept for an exit in the air (see RestoreTraversalExitVelocity).
			TraversalExitVelocity = RawT / FMath::Max(DeltaSeconds, (float)KINDA_SMALL_NUMBER);
		}
	}

	FVector TargetLoc;
	float WindowEnd;
	FName WarpPointBone;
	FName WarpTargetName;
	if (!GetActiveWarpTarget(Montage, Position, TargetLoc, WindowEnd, WarpPointBone, WarpTargetName))
	{
		// No active warp window: re-arm the once-per-climb [TSTART] latch for the next reach window.
		bTStartLatched = false;

		// No active warp window.
		const float LastEnd = GetLastLandingWindowEnd(Montage);
		const bool bIsJumpOver = LastEnd > 0.0f;

		if (bDebugTraversalWarp)
		{
			float HalfHeight = 0.0f;
			if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(UpdatedComponent))
			{
				HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
			}

			const FVector Cur = UpdatedComponent->GetComponentLocation();
			const FVector Vel = GetLinearVelocity_GMC();
			const FVector RawT = InOutRootMotionParams.GetRootMotionTransform().GetTranslation();
			const FVector RawVel = RawT / FMath::Max(DeltaSeconds, (float)KINDA_SMALL_NUMBER);
			const float FrontStandCenterZ = WarpFrontLedge.IsNearlyZero() ? 0.0f : WarpFrontLedge.Z + HalfHeight;

			UE_LOG(LogGMCMotion, Warning,
				TEXT("[TRaw] %s NO-WARP pos=%.3f lastEnd=%.3f jumpOver=%d | rawRM=(%.1f,%.1f,%.1f) rawVel=(%.0f,%.0f,%.0f) velIn=(%.0f,%.0f,%.0f) locZ=%.1f frontStandZ=%.1f"),
				*GetNameSafe(Montage), Position, LastEnd, bIsJumpOver ? 1 : 0,
				RawT.X, RawT.Y, RawT.Z,
				RawVel.X, RawVel.Y, RawVel.Z,
				Vel.X, Vel.Y, Vel.Z,
				Cur.Z, FrontStandCenterZ);
		}

		if (bSuppressBlendOutGlide && bIsJumpOver && Position >= LastEnd)
		{
			float TailHalfHeight = 0.0f;
			if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(UpdatedComponent))
			{
				TailHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
			}
			const bool bNearFarFloor =
				WarpBackFloor.IsNearlyZero() ||
				UpdatedComponent->GetComponentLocation().Z <= WarpBackFloor.Z + TailHalfHeight + TraversalNoLandHeightAboveFloor;

			if (bNearFarFloor)
			{
				FTransform Tail = InOutRootMotionParams.GetRootMotionTransform();
				if (bDebugTraversalWarp)
				{
					const FVector Cur = UpdatedComponent->GetComponentLocation();
					const FVector T = Tail.GetTranslation();
					UE_LOG(LogGMCMotion, Warning,
						TEXT("[TWarp] %s TAIL-KILL pos=%.3f lastEnd=%.3f warpActive=%d | killing Z rootMotion=(%.1f,%.1f,%.1f) curZ=%.1f"),
						*GetNameSafe(Montage), Position, LastEnd, bTraversalWarpActive ? 1 : 0, T.X, T.Y, T.Z, Cur.Z);
				}
				// Only kill vertical root motion. Preserve horizontal so GMC maintains
				// run-speed velocity through the tail — otherwise the pawn exits with zero
				// velocity and locomotion starts at walk gait before accelerating back.
				FVector TailT = Tail.GetTranslation();
				TailT.Z = 0.0;
				Tail.SetTranslation(TailT);
				InOutRootMotionParams.Set(Tail);
			}
			else if (bDebugTraversalWarp)
			{
				const FVector Cur = UpdatedComponent->GetComponentLocation();
				UE_LOG(LogGMCMotion, Warning,
					TEXT("[TWarp] %s TAIL-KEEP (still on obstacle) pos=%.3f curZ=%.1f backFloorZ=%.1f"),
					*GetNameSafe(Montage), Position, Cur.Z, WarpBackFloor.Z);
			}
			// Apply any remaining Z directly (bypasses floor projection).
			{
				const FVector TailRM = InOutRootMotionParams.GetRootMotionTransform().GetTranslation();
				if (UpdatedComponent && FMath::Abs(TailRM.Z) > KINDA_SMALL_NUMBER)
				{
					MoveUpdatedComponent(FVector(0.0, 0.0, TailRM.Z), UpdatedComponent->GetComponentRotation(), /*bSweep*/ false);
					FTransform TailStripped = InOutRootMotionParams.GetRootMotionTransform();
					FVector TailT = TailStripped.GetTranslation();
					TailT.Z = 0.0;
					TailStripped.SetTranslation(TailT);
					InOutRootMotionParams.Set(TailStripped);
				}
			}
			return;
		}

		// PRE-WINDOW phase of a JUMP-OVER: before the first warp window opens, the run-up root motion must not
		// carry the capsule through the obstacle (Custom1 uses bSweep=false). The GASP "run" hurdles start around
		// 0.28 s with their first window around 0.51 s, so this phase is real: keep the run-up and stop it short of
		// the front face (bKeepPreWindowRunUp), or zero it (old behaviour, freezes the capsule until the window).
		if (bIsJumpOver)
		{
			const float FirstStart = GetFirstWarpWindowStart(Montage);
			if (FirstStart < FLT_MAX && Position < FirstStart)
			{
				FTransform PreRM = InOutRootMotionParams.GetRootMotionTransform();
				FVector PreT = PreRM.GetTranslation();

				if (bKeepPreWindowRunUp && !WarpFrontLedge.IsNearlyZero())
				{
					const FVector Cur = UpdatedComponent->GetComponentLocation();
					FVector Into(-WarpFrontLedgeNormal.X, -WarpFrontLedgeNormal.Y, 0.0);
					if (Into.IsNearlyZero())
					{
						Into = FVector(WarpFrontLedge.X - Cur.X, WarpFrontLedge.Y - Cur.Y, 0.0);
					}
					Into = Into.GetSafeNormal();
					if (!Into.IsNearlyZero())
					{
						float Radius = 0.0f;
						if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(UpdatedComponent))
						{
							Radius = Capsule->GetScaledCapsuleRadius();
						}
						// FrontLedge lies on the front face: distance from the capsule centre to the face plane.
						const double ToFace = FVector::DotProduct(WarpFrontLedge - Cur, Into);
						const double Allowed = FMath::Max(0.0, ToFace - Radius - PreWindowFaceClearance);
						const float FromTime = FMath::Max(0.0f, Position - MontageDelta);
						auto RunUpAlong = [&](float ToTime) -> double
						{
							const FTransform Local = Montage->ExtractRootMotionFromTrackRange(FromTime, ToTime, FAnimExtractContext());
							return FVector::DotProduct(SkeletalMesh->ConvertLocalRootMotionToWorld(Local).GetTranslation(), Into);
						};
						FVector Horizontal(PreT.X, PreT.Y, 0.0);
						const double Along = FVector::DotProduct(Horizontal, Into);
						double Scale = 1.0;

						// Pace the run-up for the whole approach up to the first window's end (same ratio the window's
						// convergent warp will then use -> no speed step when it opens).
						FVector FirstTarget;
						float FirstEnd = 0.0f;
						FName FirstBone, FirstName;
						if (bPreWindowMatchFirstWindow
							&& GetActiveWarpTarget(Montage, FirstStart + KINDA_SMALL_NUMBER, FirstTarget, FirstEnd, FirstBone, FirstName))
						{
							if (!FirstBone.IsNone())
							{
								// Same inset as the window: the anim's root -> warp-bone offset at its end (bReachWarpUsesAnimOffset),
								// at least the capsule radius out from the face.
								double OutFromFace = Radius;
								if (bReachWarpUsesAnimOffset && FirstName.ToString().TrimStartAndEnd().Equals(TEXT("FrontLedge"), ESearchCase::IgnoreCase))
								{
									const FQuat MeshQuat = SkeletalMesh->GetRelativeRotation().Quaternion();
									OutFromFace = FMath::Max((double)Radius, -MeshQuat.RotateVector(ComputeRootMinusBoneOffset(Montage, FirstEnd, FirstBone)).X);
								}
								FirstTarget += -Into * OutFromFace;
							}
							const double ToTarget = FVector::DotProduct(FirstTarget - Cur, Into);
							const double RunUpToEnd = RunUpAlong(FirstEnd);
							if (RunUpToEnd > 1.0)
							{
								// Recomputed every frame, so it converges: a slower entry (entry blend below) is made up
								// over the rest of the run-up instead of by the window in one go.
								Scale = FMath::Clamp(ToTarget / RunUpToEnd, 0.0, (double)FMath::Min(PreWindowMaxScale, MaxWarpScale));
							}
						}

						// Room left before the face (late jump press): the run-up until the window opens must fit.
						const double RunUpLeft = RunUpAlong(FirstStart);
						if (RunUpLeft * Scale > Allowed)
						{
							Scale = RunUpLeft > KINDA_SMALL_NUMBER ? Allowed / RunUpLeft : 0.0;
						}

						// Entry blend: from the speed the pawn entered with to the paced run-up speed.
						double FrameScale = Scale;
						const float StartTime = GetConfirmedTraversalStartTime();
						if (TraversalEntrySpeedBlendTime > 0.0f && StartTime >= 0.0f && Along > KINDA_SMALL_NUMBER && DeltaSeconds > KINDA_SMALL_NUMBER)
						{
							// Done by the time the window opens at the latest (late press: short pre-window), so the
							// window starts at the paced speed instead of stepping down to it.
							const float BlendTime = FMath::Min(TraversalEntrySpeedBlendTime, FMath::Max(FirstStart - StartTime, 0.02f));
							const double Alpha = FMath::Clamp((Position - StartTime) / BlendTime, 0.0f, 1.0f);
							const double FrameSpeed = Along / DeltaSeconds;
							const double Desired = FMath::Lerp((double)TraversalEntrySpeed, FrameSpeed * Scale, Alpha);
							FrameScale = Desired / FrameSpeed;
						}
						if (Along > 0.0)
						{
							Horizontal -= Into * (Along * (1.0 - FrameScale));
						}
						const double AlongScaled = FVector::DotProduct(Horizontal, Into);
						if (AlongScaled > Allowed)
						{
							Horizontal -= Into * (AlongScaled - Allowed);
						}

						if (bDebugTraversalWarp)
						{
							UE_LOG(LogGMCMotion, Warning,
								TEXT("[TWarp] %s PRE-WINDOW RUN-UP pos=%.3f firstWarp=%.3f toFace=%.1f | rootMotion=(%.1f,%.1f) -> (%.1f,%.1f)"),
								*GetNameSafe(Montage), Position, FirstStart, ToFace, PreT.X, PreT.Y, Horizontal.X, Horizontal.Y);
						}

						PreT.X = Horizontal.X;
						PreT.Y = Horizontal.Y;
						PreRM.SetTranslation(PreT);
						InOutRootMotionParams.Set(PreRM);
						return;
					}
				}

				if (bDebugTraversalWarp)
				{
					UE_LOG(LogGMCMotion, Warning,
						TEXT("[TWarp] %s PRE-WINDOW pos=%.3f firstWarp=%.3f | suppressing horizontal rootMotion=(%.1f,%.1f,%.1f)"),
						*GetNameSafe(Montage), Position, FirstStart, PreT.X, PreT.Y, PreT.Z);
				}

				PreT.X = 0.0;
				PreT.Y = 0.0;
				PreRM.SetTranslation(PreT);
				InOutRootMotionParams.Set(PreRM);
				return;
			}
		}

		// Inter-window gap of a JUMP-OVER: cap the apex so raw root motion doesn't balloon
		// the character high above the obstacle between FrontLedge and BackLedge windows.
		if (bIsJumpOver && Position < LastEnd)
		{
			// Ease out of the previous warp window: from its last warped horizontal speed to the anim's.
			if (WarpWindowExitBlendTime > 0.0f && LastWarpWindowEnd >= 0.0f && Position >= LastWarpWindowEnd
				&& Position < LastWarpWindowEnd + WarpWindowExitBlendTime && DeltaSeconds > KINDA_SMALL_NUMBER)
			{
				FTransform ExitRM = InOutRootMotionParams.GetRootMotionTransform();
				FVector ExitT = ExitRM.GetTranslation();
				const double RawSpeed = FVector(ExitT.X, ExitT.Y, 0.0).Size() / DeltaSeconds;
				if (RawSpeed > KINDA_SMALL_NUMBER)
				{
					const double Alpha = FMath::Clamp((Position - LastWarpWindowEnd) / WarpWindowExitBlendTime, 0.0f, 1.0f);
					const double Desired = FMath::Lerp((double)LastWarpHorizontalSpeed, RawSpeed, Alpha);
					ExitT.X *= Desired / RawSpeed;
					ExitT.Y *= Desired / RawSpeed;
					ExitRM.SetTranslation(ExitT);
					InOutRootMotionParams.Set(ExitRM);
				}
			}

			float HalfHeight = 0.0f;
			if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(UpdatedComponent))
			{
				HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
			}

			const float CeilingZ = GetMaxWarpCorrectedZ(Montage, HalfHeight) + GapApexClearance;
			const float CurZ = UpdatedComponent->GetComponentLocation().Z;

			FTransform GapRM = InOutRootMotionParams.GetRootMotionTransform();
			FVector GapDelta = GapRM.GetTranslation();
			const double HeadRoom = FMath::Max(0.0, (double)(CeilingZ - CurZ));
			const float CappedDeltaZ = FMath::Min((double)GapDelta.Z, HeadRoom);
			if (CappedDeltaZ < GapDelta.Z)
			{
				if (bDebugTraversalWarp)
				{
					UE_LOG(LogGMCMotion, Warning,
						TEXT("[TWarp] %s GAP-CAP pos=%.3f ceil=%.1f curZ=%.1f | rmZ %.1f -> %.1f"),
						*GetNameSafe(Montage), Position, CeilingZ, CurZ, GapDelta.Z, CappedDeltaZ);
				}
				GapDelta.Z = CappedDeltaZ;
				GapRM.SetTranslation(GapDelta);
				InOutRootMotionParams.Set(GapRM);
			}
		}

		// Post-window cap for REACH/CLIMB/MANTLE: prevent post-window root motion from
		// pushing the capsule above the ledge surface. The warp window already placed feet
		// at WarpFrontLedge.Z; any remaining upward root motion is overshoot.
		if (!bIsJumpOver && !WarpFrontLedge.IsNearlyZero())
		{
			float CapHalfHeight = 0.0f;
			if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(UpdatedComponent))
			{
				CapHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
			}

			const float CeilingZ = WarpFrontLedge.Z + CapHalfHeight + ReachPostWindowClearance;
			const float CurZ = UpdatedComponent->GetComponentLocation().Z;

			FTransform PostRM = InOutRootMotionParams.GetRootMotionTransform();
			FVector PostDelta = PostRM.GetTranslation();
			const double HeadRoom = FMath::Max(0.0, (double)(CeilingZ - CurZ));
			const float CappedDeltaZ = FMath::Min((double)PostDelta.Z, HeadRoom);
			if (CappedDeltaZ < PostDelta.Z)
			{
				if (bDebugTraversalWarp)
				{
					UE_LOG(LogGMCMotion, Warning,
						TEXT("[TWarp] %s REACH-CAP pos=%.3f ceil=%.1f curZ=%.1f | rmZ %.1f -> %.1f"),
						*GetNameSafe(Montage), Position, CeilingZ, CurZ, PostDelta.Z, CappedDeltaZ);
				}
				PostDelta.Z = CappedDeltaZ;
				PostRM.SetTranslation(PostDelta);
				InOutRootMotionParams.Set(PostRM);
			}
		}
		// Apply Z directly (bypasses floor projection in gap / post-window phase).
		{
			const FVector FinalRM = InOutRootMotionParams.GetRootMotionTransform().GetTranslation();
			if (UpdatedComponent && FMath::Abs(FinalRM.Z) > KINDA_SMALL_NUMBER)
			{
				MoveUpdatedComponent(FVector(0.0, 0.0, FinalRM.Z), UpdatedComponent->GetComponentRotation(), /*bSweep*/ false);
				FTransform GapStripped = InOutRootMotionParams.GetRootMotionTransform();
				FVector GapT = GapStripped.GetTranslation();
				GapT.Z = 0.0;
				GapStripped.SetTranslation(GapT);
				InOutRootMotionParams.Set(GapStripped);
			}
		}
		return;
	}

	if (WindowEnd <= Position)
	{
		// Apply Z directly (bypasses floor projection, edge case: past window end).
		const FVector EdgeRM = InOutRootMotionParams.GetRootMotionTransform().GetTranslation();
		if (UpdatedComponent && FMath::Abs(EdgeRM.Z) > KINDA_SMALL_NUMBER)
		{
			MoveUpdatedComponent(FVector(0.0, 0.0, EdgeRM.Z), UpdatedComponent->GetComponentRotation(), /*bSweep*/ false);
			FTransform EdgeStripped = InOutRootMotionParams.GetRootMotionTransform();
			FVector EdgeT = EdgeStripped.GetTranslation();
			EdgeT.Z = 0.0;
			EdgeStripped.SetTranslation(EdgeT);
			InOutRootMotionParams.Set(EdgeStripped);
		}
		return;
	}

	const FVector RawTargetLoc = TargetLoc;

	// Lift the target by the capsule half height (feet -> capsule center).
	float HalfHeight = 0.0f;
	if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(UpdatedComponent))
	{
		HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
	}

	FVector RootMinusBone = FVector::ZeroVector;
	if (!WarpPointBone.IsNone())
	{
		RootMinusBone = ComputeRootMinusBoneOffset(Montage, WindowEnd, WarpPointBone);
	}

	const bool bHurdle = (GetLastLandingWindowEnd(Montage) > 0.0f);
	bLastTraversalIsJumpOver = bHurdle;

	// Air catch (see bCatchWarpUsesAnimOffset): anim-offset FrontLedge target + additive vertical warp below.
	const bool bCatchWarp = bCatchWarpUsesAnimOffset && IsCatchTraversalMontage(Montage) && !WarpPointBone.IsNone();

	// Mode B (Custom1) zeroes the bone offset for hurdles — the custom physics bypasses grounding
	// so the hand-plant offset would sink the capsule against the face. Mode A (Airborne) keeps the
	// bone offset for hurdles because the animation's own Z root motion (via WarpAxis) drives the
	// arc naturally from the lower hand-plant target, and grounding handles the landing.
	// Exception: the FrontLedge window of a jump-over with the anim-offset target (bReachWarpUsesAnimOffset) keeps the
	// bone offset like GASP58's motion warping: the horizontal target then leaves the capsule out from the face, so it
	// no longer sinks into it, and without the offset the root ended ~25 cm too high (apex capped flat at top + 25).
	const bool bHurdleFrontLedgeAnimOffset = bReachWarpUsesAnimOffset && bHurdle
		&& WarpTargetName.ToString().TrimStartAndEnd().Equals(TEXT("FrontLedge"), ESearchCase::IgnoreCase);
	const double TargetBoneOffsetZ = (bUseTraversalCustomMode && bHurdle && !bHurdleFrontLedgeAnimOffset) ? 0.0 : RootMinusBone.Z;
	TargetLoc.Z += HalfHeight + TargetBoneOffsetZ + WarpPointVerticalBias;

	// Corner clearance (jump-over BackLedge only, mode A).
	if (bHurdle && !bUseTraversalCustomMode && WarpTargetName.ToString().TrimStartAndEnd().Equals(TEXT("BackLedge"), ESearchCase::IgnoreCase))
	{
		TargetLoc.Z += TraversalCornerClearance;
	}

	// Horizontal inset (FrontLedge / bone-provider window only).
	if (!WarpPointBone.IsNone() && !WarpFrontLedgeNormal.IsNearlyZero())
	{
		float Radius = 0.0f;
		if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(UpdatedComponent))
		{
			Radius = Capsule->GetScaledCapsuleRadius();
		}
		FVector FaceNormal = WarpFrontLedgeNormal;
		FaceNormal.Z = 0.0;
		if (!FaceNormal.IsNearlyZero())
		{
			FaceNormal.Normalize();

			// Air catch: the capsule ends where the animation puts its root relative to the warp bone (hanging out from
			// the face), rotated to the facing the warp squares up to (-FaceNormal).
			// Ground reach (mantle / vault / climb) and the FrontLedge window of a jump-over: same anim offset (see
			// bReachWarpUsesAnimOffset). With the radius inset the 5.8 Relaxed hurdle took off ~40 cm later than GASP58 and
			// peaked past the wall (2026-10-01).
			const bool bFrontLedgeWindow = WarpTargetName.ToString().TrimStartAndEnd().Equals(TEXT("FrontLedge"), ESearchCase::IgnoreCase);
			const bool bCatchAnimOffset = bCatchWarp
				|| (bReachWarpUsesAnimOffset && !WarpPointBone.IsNone() && (!bHurdle || bFrontLedgeWindow));
			double OutFromFace = Radius;
			if (bCatchAnimOffset)
			{
				const FQuat FacingQuat = FRotationMatrix::MakeFromX(-FaceNormal).ToQuat();
				const FQuat MeshQuat = SkeletalMesh->GetRelativeRotation().Quaternion();
				const FVector OffsetWorld = (FacingQuat * MeshQuat).RotateVector(RootMinusBone);
				OutFromFace = FMath::Max((double)Radius, FVector::DotProduct(OffsetWorld, FaceNormal));
				if (bDebugTraversalWarp)
				{
					UE_LOG(LogGMCMotion, Warning, TEXT("[TOffset] %s pos=%.3f end=%.3f rootMinusBone=%s meshYaw=%.1f offsetWorld=%s normal=%s radius=%.1f out=%.1f"),
						*GetNameSafe(Montage), Position, WindowEnd, *RootMinusBone.ToCompactString(), SkeletalMesh->GetRelativeRotation().Yaw,
						*OffsetWorld.ToCompactString(), *FaceNormal.ToCompactString(), Radius, OutFromFace);
				}
			}
			TargetLoc += FaceNormal * OutFromFace;

			// Reach only: push the target INWARD past the ledge edge so the pawn ends up on
			// top of the obstacle, not balanced on the edge with feet half off.
			if (!bHurdle && !bCatchAnimOffset)
			{
				TargetLoc -= FaceNormal * ReachLedgePush;
			}
		}
	}

	if (bDebugTraversalWarp)
	{
		const FVector Cur = UpdatedComponent->GetComponentLocation();
		const double DXY = FVector::Dist2D(TargetLoc, Cur);
		UE_LOG(LogGMCMotion, Warning,
			TEXT("[TWarp] %s tgt=%s bone=%s pos=%.3f end=%.3f mode=%d | corrTgtZ=%.1f curZ=%.1f dZ=%.1f | curXY=(%.0f,%.0f) dXY=%.1f"),
			*GetNameSafe(Montage), *WarpTargetName.ToString(), *WarpPointBone.ToString(),
			Position, WindowEnd, static_cast<int32>(GetMovementMode()),
			TargetLoc.Z, Cur.Z, TargetLoc.Z - Cur.Z,
			Cur.X, Cur.Y, DXY);

		const float ObstacleTopZ = GetMaxWarpCorrectedZ(Montage, HalfHeight);
		UE_LOG(LogGMCMotion, Warning,
			TEXT("[TGeo] %s HH=%.1f | FrontLedgeZ=%.1f BackLedgeZ=%.1f BackFloorZ=%.1f | obstacleTopZ=%.1f ceil=%.1f | collIgnored=%d"),
			*GetNameSafe(Montage), HalfHeight,
			WarpFrontLedge.Z, WarpBackLedge.Z, WarpBackFloor.Z,
			ObstacleTopZ, ObstacleTopZ + GapApexClearance,
			IgnoredObstacle.IsValid() ? 1 : 0);
	}

	// Remaining animation root motion over [Position, WindowEnd], converted to world space.
	const FTransform RemainingLocal = Montage->ExtractRootMotionFromTrackRange(Position, WindowEnd, FAnimExtractContext());
	const FVector RemainingWorld = SkeletalMesh->ConvertLocalRootMotionToWorld(RemainingLocal).GetTranslation();

	const FVector CurrentLoc = UpdatedComponent->GetComponentLocation();

	const bool bReachFrontLedge = !bHurdle && !WarpPointBone.IsNone();

	// Once-per-climb summary of every input that decides how the reach/climb warp behaves this run.
	// Emitted on the first frame the FrontLedge window opens (TargetLoc.Z here is PRE-Lerp = corrTgtZ).
	// Fields to diff good-vs-bad runs of the SAME foot:
	//   dZ<0 / lerpArms=1  -> the l.521 Lerp fires and injects vertical warp the anim doesn't carry.
	//   rmZwin / rmZpost   -> vertical the baked root motion itself provides in/after the window.
	if (bDebugTraversalWarp && bReachFrontLedge && !bTStartLatched)
	{
		bTStartLatched = true;

		const FVector TStartVelIn = GetLinearVelocity_GMC();
		const double TStartStandingZ = RawTargetLoc.Z + HalfHeight;
		const bool bTStartLerpArms = (TargetLoc.Z < CurrentLoc.Z);
		const FTransform TStartPostLocal = Montage->ExtractRootMotionFromTrackRange(WindowEnd, Montage->GetPlayLength(), FAnimExtractContext());
		const double TStartRmZPost = SkeletalMesh->ConvertLocalRootMotionToWorld(TStartPostLocal).GetTranslation().Z;

		UE_LOG(LogGMCMotion, Warning,
			TEXT("[TSTART] %s foot=%s pos=%.3f | curZ=%.1f corrTgtZ=%.1f dZ=%.1f standingZ=%.1f lerpArms=%d | frontLedgeZ=%.1f HH=%.1f winEnd=%.3f | rmZwin=%.1f rmZpost=%.1f | velIn=(%.0f,%.0f,%.0f) loc=(%.0f,%.0f,%.0f)"),
			*GetNameSafe(Montage),
			IsTraversalMontageRightFoot(Montage) ? TEXT("R") : TEXT("L"),
			Position,
			CurrentLoc.Z, TargetLoc.Z, TargetLoc.Z - CurrentLoc.Z, TStartStandingZ, bTStartLerpArms ? 1 : 0,
			WarpFrontLedge.Z, HalfHeight, WindowEnd,
			RemainingWorld.Z, TStartRmZPost,
			TStartVelIn.X, TStartVelIn.Y, TStartVelIn.Z,
			CurrentLoc.X, CurrentLoc.Y, CurrentLoc.Z);
	}

	// FrontLedge (reach/climb/catch) vertical target guard, for when corrTgtZ lands below the capsule.
	//
	// corrTgtZ is the capsule center that puts the hand on the ACTUAL ledge at window end; by montage
	// design it satisfies corrTgtZ ~= standingZ - rmZpost (post-window baked root motion finishes the climb
	// to standing). corrTgtZ can sit below curZ for two very different reasons - split on whether the
	// animation itself carries downward motion in this window (RemainingWorld.Z == rmZwin):
	//
	//   rmZwin >= 0 (anim does NOT descend, e.g. a walk climb whose bone-offset geometry alone dropped
	//     corrTgtZ under the capsule): do NOT force the capsule down against the rising pose - hold Z.
	//     The old lerp-to-standing instead injected a big upward pop (~+80) that overshot the ledge, which
	//     REACH-CAP then clipped -> the pop-then-pin. Holding lets post-window RM + REACH-CAP land it cleanly.
	//     Safe because corrTgtZ < curZ with rmZwin >= 0 implies rmZpost is large enough to finish.
	//
	//   rmZwin < 0 (anim DOES descend, e.g. Catch_Cliff catch-and-drop): the drop is real. Leave the target
	//     at corrTgtZ and let the warp follow the baked descent down to it. Clamping to curZ here would
	//     freeze the catch mid-air (capsule hangs high, then snaps up post-window).
	if (bReachFrontLedge && TargetLoc.Z < CurrentLoc.Z && RemainingWorld.Z >= 0.0)
	{
		TargetLoc.Z = CurrentLoc.Z;
	}

	const FVector ToTarget = TargetLoc - CurrentLoc;

	// This frame's world-space root motion translation (already world after Super).
	FTransform RootMotionTransform = InOutRootMotionParams.GetRootMotionTransform();
	const FVector FrameDelta = RootMotionTransform.GetTranslation();

	// Convergent warp pacing.
	const double RemainingMag = RemainingWorld.Size();
	const double FrameMag = FrameDelta.Size();
	const double RemainingTime = WindowEnd - Position;

	const double AnimFraction = (RemainingMag > 1.0) ? (FrameMag / RemainingMag) : 1.0;
	const double TimeFraction = (RemainingTime > KINDA_SMALL_NUMBER) ? ((double)MontageDelta / RemainingTime) : 1.0;

	double Fraction = bTimeFloorWarp ? FMath::Max(AnimFraction, TimeFraction) : AnimFraction;
	Fraction = FMath::Clamp(Fraction, 0.0, 1.0);

	auto WarpAxis = [&](double ToTargetAxis, double RemainingAnimAxis, double FrameAnimAxis, double MinScale) -> double
		{
			if (FMath::Abs(RemainingAnimAxis) > 1.0)
			{
				const double MaxScale = FMath::Max((double)MaxWarpScale, MinScale);
				return FrameAnimAxis * FMath::Clamp(ToTargetAxis / RemainingAnimAxis, MinScale, MaxScale);
			}
			return ToTargetAxis * Fraction;
		};

	const bool bFollowAnimPath = bArcPreserveJumpArc;

	FVector Warped;
	if (bFollowAnimPath)
	{
		Warped.X = WarpAxis(ToTarget.X, RemainingWorld.X, FrameDelta.X, 0.0);
		Warped.Y = WarpAxis(ToTarget.Y, RemainingWorld.Y, FrameDelta.Y, 0.0);
		Warped.Z = WarpAxis(ToTarget.Z, RemainingWorld.Z, FrameDelta.Z, 0.0);

		// Z-snap for reach FrontLedge (climb / mantle): distribute the vertical gap
		// evenly over the remaining window time instead of using the animation-ratio warp.
		// Hurdle FrontLedge uses the animation-ratio Z (WarpAxis above) so the animation's
		// natural arc drives the vertical motion from the bone-offset target.
		if (bReachFrontLedge && ToTarget.Z > 0.0)
		{
			const double ZTimeFraction = (RemainingTime > KINDA_SMALL_NUMBER)
				? FMath::Clamp((double)MontageDelta / RemainingTime, 0.0, 1.0)
				: 1.0;
			Warped.Z = ToTarget.Z * ZTimeFraction;
		}
	}
	else
	{
		Warped = ToTarget * Fraction;
	}

	// Air catch: the montage starts inside its FrontLedge window with only a few frames left. The anim-ratio Z above
	// does nothing while the anim descends toward a higher target, then closes the whole gap on the last frame (+27 cm
	// pop measured) - or misses it when a frame lands past the window end (the capsule then wedged against the wall).
	// The gap is spread evenly over the time left instead: a steady rise of a few cm per frame, like GASP58's skew warp
	// turns the anim's grab dip into a climb toward the target (following the dip first meant -33 cm then +50 cm).
	if (bCatchWarp)
	{
		const double ZTimeFraction = (RemainingTime > KINDA_SMALL_NUMBER)
			? FMath::Clamp((double)MontageDelta / RemainingTime, 0.0, 1.0)
			: 1.0;
		Warped.Z = ToTarget.Z * ZTimeFraction;
	}

	// Jump-over landing window: the forward progress (along the travel direction) is at least HurdleLandingMinWarpScale
	// of the anim's own frame motion, and never backwards once the capsule is past BackFloor (the target-seeking branch
	// of WarpAxis would pull it back at window end). Lateral corrections toward the target are kept.
	if (bHurdle && HurdleLandingMinWarpScale > 0.0f
		&& WarpTargetName.ToString().TrimStartAndEnd().Equals(TEXT("BackFloor"), ESearchCase::IgnoreCase))
	{
		FVector TravelDir(FrameDelta.X, FrameDelta.Y, 0.0);
		if (TravelDir.SizeSquared() < 1e-4)
		{
			TravelDir = FVector(Velocity.X, Velocity.Y, 0.0);
		}
		TravelDir = TravelDir.GetSafeNormal();
		if (!TravelDir.IsNearlyZero())
		{
			FVector WarpedXY(Warped.X, Warped.Y, 0.0);
			const double Along = WarpedXY | TravelDir;
			const double MinAlong = FMath::Max(0.0, (FVector(FrameDelta.X, FrameDelta.Y, 0.0) | TravelDir) * (double)HurdleLandingMinWarpScale);
			if (Along < MinAlong)
			{
				WarpedXY += TravelDir * (MinAlong - Along);
				Warped.X = WarpedXY.X;
				Warped.Y = WarpedXY.Y;
			}
		}
	}

	// Reach montages: no-overshoot clamp.
	if (!bHurdle)
	{
		auto ClampNoOvershoot = [](double Cur, double Delta, double Target) -> double
			{
				const double Next = Cur + Delta;
				if (Delta > 0.0 && Next > Target) { return Target - Cur; }
				if (Delta < 0.0 && Next < Target) { return Target - Cur; }
				return Delta;
			};
		Warped.X = ClampNoOvershoot(CurrentLoc.X, Warped.X, TargetLoc.X);
		Warped.Y = ClampNoOvershoot(CurrentLoc.Y, Warped.Y, TargetLoc.Y);
		Warped.Z = ClampNoOvershoot(CurrentLoc.Z, Warped.Z, TargetLoc.Z);

		// Velocity no-overshoot.
		const double Dt = FMath::Max((double)DeltaSeconds, (double)KINDA_SMALL_NUMBER);
		auto ClampVelNoOvershoot = [Dt](double Cur, double WarpedAxis, double Vel, double Target) -> double
			{
				const double AfterRoot = Cur + WarpedAxis;
				const double Remaining = Target - AfterRoot;
				const double VelMove = Vel * Dt;
				if (Remaining >= 0.0)
				{
					if (VelMove < 0.0) { return 0.0; }
					if (VelMove > Remaining) { return Remaining / Dt; }
				}
				else
				{
					if (VelMove > 0.0) { return 0.0; }
					if (VelMove < Remaining) { return Remaining / Dt; }
				}
				return Vel;
			};
		const FVector Vel = GetLinearVelocity_GMC();
		const FVector ClampedVel(
			ClampVelNoOvershoot(CurrentLoc.X, Warped.X, Vel.X, TargetLoc.X),
			ClampVelNoOvershoot(CurrentLoc.Y, Warped.Y, Vel.Y, TargetLoc.Y),
			ClampVelNoOvershoot(CurrentLoc.Z, Warped.Z, Vel.Z, TargetLoc.Z));
		if (!ClampedVel.Equals(Vel))
		{
			if (bDebugTraversalWarp)
			{
				UE_LOG(LogGMCMotion, Warning,
					TEXT("[TWarp] %s VEL-CLAMP vel=(%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f) | cur=(%.0f,%.0f,%.0f) warped=(%.1f,%.1f,%.1f) tgt=(%.0f,%.0f,%.0f)"),
					*GetNameSafe(Montage), Vel.X, Vel.Y, Vel.Z, ClampedVel.X, ClampedVel.Y, ClampedVel.Z,
					CurrentLoc.X, CurrentLoc.Y, CurrentLoc.Z, Warped.X, Warped.Y, Warped.Z,
					TargetLoc.X, TargetLoc.Y, TargetLoc.Z);
			}
			UpdateVelocity(ClampedVel);
		}
	}
	// Apply warped Z directly with no sweep, bypassing GMC's root motion floor projection.
	// GMC applies root motion with sweep BEFORE the physics function runs — if grounding is
	// active, that sweep strips the Z component. By applying Z here and zeroing it from the
	// root motion params, XY flows through the normal pipeline while Z bypasses floor collision.
	// PhysicsCustom's CalculateVelocity still adds gravity on top of this direct Z move.
	const double AppliedWarpZ = Warped.Z;
	if (UpdatedComponent && FMath::Abs(Warped.Z) > KINDA_SMALL_NUMBER)
	{
		MoveUpdatedComponent(FVector(0.0, 0.0, Warped.Z), UpdatedComponent->GetComponentRotation(), /*bSweep*/ false);
	}
	Warped.Z = 0.0;
	RootMotionTransform.SetTranslation(Warped);
	if (bHurdle && DeltaSeconds > KINDA_SMALL_NUMBER)
	{
		LastWarpHorizontalSpeed = FVector(Warped.X, Warped.Y, 0.0).Size() / DeltaSeconds;
		LastWarpWindowEnd = WindowEnd;
	}

	// --- Traversal Hand IK targets ---
	// Only during FrontLedge window (bone provider active = reach/climb/mantle).
	// Hands target the ledge edge, spread along the ledge tangent.
	if (bEnableTraversalHandIK && !WarpPointBone.IsNone() && !WarpFrontLedgeNormal.IsNearlyZero())
	{
		// Ledge tangent = horizontal direction along the ledge edge.
		// Cross(OutwardNormal, Up) gives the character's right when facing into the obstacle.
		FVector LedgeTangent = FVector::CrossProduct(WarpFrontLedgeNormal, FVector::UpVector);
		LedgeTangent.Z = 0.f;
		if (LedgeTangent.Normalize())
		{
			// Base hand position: on the ledge edge, offset into the top surface.
			FVector IntoSurface = -WarpFrontLedgeNormal;
			IntoSurface.Z = 0.f;
			IntoSurface.Normalize();

			FVector HandBase = WarpFrontLedge;
			HandBase.Z += HandIKVerticalOffset;
			HandBase += IntoSurface * HandIKDepthOffset;

			// Right hand = +tangent, left hand = -tangent (when facing the obstacle).
			TraversalHandIK_RightTarget = HandBase + LedgeTangent * HandIKSpread;
			TraversalHandIK_LeftTarget = HandBase - LedgeTangent * HandIKSpread;
			TraversalHandIKAlpha = 1.f;
		}
	}

	if (bDebugTraversalWarp)
	{
		const FVector Cur = UpdatedComponent->GetComponentLocation();
		UE_LOG(LogGMCMotion, Warning,
			TEXT("[TWarp]   %s frameAnim=(%.1f,%.1f,%.1f) warped=(%.1f,%.1f,%.1f) | curZ=%.1f tgtZ=%.1f dZ=%.1f | curX=%.0f tgtX=%.0f dX=%.1f animPath=%d"),
			*WarpTargetName.ToString(),
			FrameDelta.X, FrameDelta.Y, FrameDelta.Z, Warped.X, Warped.Y, AppliedWarpZ,
			Cur.Z, TargetLoc.Z, TargetLoc.Z - Cur.Z,
			Cur.X, TargetLoc.X, TargetLoc.X - Cur.X, bFollowAnimPath ? 1 : 0);
	}

	// --- Rotation warp ---
	if (bEnableRotationWarp && !WarpPointBone.IsNone() && !WarpFrontLedgeNormal.IsNearlyZero())
	{
		FVector Face = -WarpFrontLedgeNormal;
		Face.Z = 0.0;
		if (!Face.IsNearlyZero())
		{
			const double TargetYaw = Face.Rotation().Yaw;
			const double CurrentYaw = UpdatedComponent->GetComponentRotation().Yaw;
			const double DeltaYaw = FMath::FindDeltaAngleDegrees(CurrentYaw, TargetYaw);
			const double WarpedYaw = DeltaYaw * Fraction;
			RootMotionTransform.SetRotation(FRotator(0.0, WarpedYaw, 0.0).Quaternion());

			if (bDebugTraversalWarp)
			{
				UE_LOG(LogGMCMotion, Warning,
					TEXT("[TWarp] %s ROT tgtYaw=%.1f curYaw=%.1f dYaw=%.1f frac=%.3f warpedYaw=%.2f"),
					*GetNameSafe(Montage), TargetYaw, CurrentYaw, DeltaYaw, Fraction, WarpedYaw);
			}
		}
	}

	InOutRootMotionParams.Set(RootMotionTransform);
}

bool UGMCMotion::IsTraversalMontageRightFoot(UAnimMontage* Montage) const
{
	return Montage && Montage->GetName().EndsWith(TEXT("Rfoot"));
}

bool UGMCMotion::IsCatchTraversalMontage(const UAnimMontage* Montage)
{
	return Montage && Montage->GetName().Contains(TEXT("Catch"));
}

float UGMCMotion::AdjustTraversalStartTimeForDistance(const UObject* Context, UObject* MontageObject, float StartTime,
	FVector FrontLedgeLocation, FVector FrontLedgeNormal)
{
	const UActorComponent* ContextComponent = Cast<UActorComponent>(Context);
	const AActor* Owner = ContextComponent ? ContextComponent->GetOwner() : Cast<AActor>(Context);
	const UGMCMotion* Motion = Owner ? Owner->FindComponentByClass<UGMCMotion>() : nullptr;
	UAnimMontage* Montage = Cast<UAnimMontage>(MontageObject);
	if (!Motion || !Motion->bAdjustTraversalStartTimeByDistance || !Montage || !Motion->UpdatedComponent || !Motion->SkeletalMesh
		|| IsCatchTraversalMontage(Montage))
	{
		return StartTime;
	}

	// Searchable range of the montage (the chooser's pose search never starts outside it).
	float RangeStart = -1.0f, RangeEnd = -1.0f, BranchStart = -1.0f, BranchEnd = -1.0f;
	// First FrontLedge window end and its warp bone.
	float WindowEnd = FLT_MAX;
	FName WarpBone;
	for (const FAnimNotifyEvent& Event : Montage->Notifies)
	{
		if (!Event.NotifyStateClass)
		{
			continue;
		}
		const FString ClassName = Event.NotifyStateClass->GetClass()->GetName();
		if (ClassName.Contains(TEXT("PoseSearchSamplingRange")))
		{
			RangeStart = Event.GetTriggerTime();
			RangeEnd = Event.GetEndTriggerTime();
		}
		else if (ClassName.Contains(TEXT("PoseSearchBranchIn")))
		{
			BranchStart = Event.GetTriggerTime();
			BranchEnd = Event.GetEndTriggerTime();
		}
		else if (const UAnimNotifyState_MotionWarping* Warping = Cast<UAnimNotifyState_MotionWarping>(Event.NotifyStateClass))
		{
			const URootMotionModifier_Warp* Modifier = Cast<URootMotionModifier_Warp>(Warping->RootMotionModifier);
			if (Modifier && Modifier->WarpTargetName.ToString().TrimStartAndEnd().Equals(TEXT("FrontLedge"), ESearchCase::IgnoreCase)
				&& Event.GetEndTriggerTime() < WindowEnd)
			{
				WindowEnd = Event.GetEndTriggerTime();
				WarpBone = Modifier->WarpPointAnimProvider == EWarpPointAnimProvider::Bone ? Modifier->WarpPointAnimBoneName : NAME_None;
			}
		}
	}
	if (RangeStart < 0.0f)
	{
		RangeStart = BranchStart;
		RangeEnd = BranchEnd;
	}
	if (RangeStart < 0.0f || WindowEnd == FLT_MAX)
	{
		return StartTime;
	}
	RangeStart = FMath::Max(RangeStart, 0.0f);
	RangeEnd = FMath::Min(RangeEnd, WindowEnd);
	if (RangeEnd - RangeStart < 1.0f / 60.0f)
	{
		return StartTime;
	}

	// Distance the capsule has to cover: to the face, minus where the anim puts the root at the window end (same target
	// as the FrontLedge warp, see bReachWarpUsesAnimOffset).
	FVector Into(-FrontLedgeNormal.X, -FrontLedgeNormal.Y, 0.0);
	if (!Into.Normalize())
	{
		return StartTime;
	}
	float Radius = 0.0f;
	if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Motion->UpdatedComponent))
	{
		Radius = Capsule->GetScaledCapsuleRadius();
	}
	const FQuat MeshQuat = Motion->SkeletalMesh->GetRelativeRotation().Quaternion();
	double OutFromFace = Radius;
	if (!WarpBone.IsNone() && Motion->bReachWarpUsesAnimOffset)
	{
		OutFromFace = FMath::Max((double)Radius, -MeshQuat.RotateVector(Motion->ComputeRootMinusBoneOffset(Montage, WindowEnd, WarpBone)).X);
	}
	const double ToFace = FVector::DotProduct(FrontLedgeLocation - Motion->UpdatedComponent->GetComponentLocation(), Into);
	const double Needed = ToFace - OutFromFace;
	if (Needed <= 1.0)
	{
		return StartTime;
	}
	const double Desired = Needed / FMath::Max((double)Motion->TraversalStartTimeWarpScale, 0.1);

	// Anim run-up from t to the window end decreases with t: keep the sample closest to the desired run-up.
	double BestTime = StartTime, BestError = DBL_MAX;
	for (float Time = RangeStart; Time <= RangeEnd + KINDA_SMALL_NUMBER; Time += 1.0f / 120.0f)
	{
		const FTransform RunUp = Montage->ExtractRootMotionFromTrackRange(Time, WindowEnd, FAnimExtractContext());
		const double Error = FMath::Abs(RunUp.GetTranslation().Size2D() - Desired);
		if (Error < BestError)
		{
			BestError = Error;
			BestTime = Time;
		}
	}

	if (Motion->bDebugTraversalWarp)
	{
		UE_LOG(LogGMCMotion, Warning, TEXT("[TStartAdjust] %s chooser=%.3f -> %.3f | range=[%.3f,%.3f] winEnd=%.3f toFace=%.1f out=%.1f desired=%.1f err=%.1f"),
			*GetNameSafe(Montage), StartTime, BestTime, RangeStart, RangeEnd, WindowEnd, ToFace, OutFromFace, Desired, BestError);
	}
	return static_cast<float>(BestTime);
}

UAnimMontage* UGMCMotion::GetTraversalMontageForFoot(UAnimMontage* Montage, bool bRightFoot) const
{
	if (!Montage)
	{
		return Montage;
	}

	const bool bIsRight = Montage->GetName().EndsWith(TEXT("Rfoot"));
	if (bIsRight == bRightFoot)
	{
		return Montage; // already the wanted foot
	}

	const FString PathName = Montage->GetPathName();
	const FString MirrorPath = bIsRight
		? PathName.Replace(TEXT("Rfoot"), TEXT("Lfoot"))
		: PathName.Replace(TEXT("Lfoot"), TEXT("Rfoot"));

	if (MirrorPath == PathName)
	{
		return Montage; // no suffix found
	}

	UAnimMontage* Variant = LoadObject<UAnimMontage>(nullptr, *MirrorPath);
	if (bDebugTraversalWarp)
	{
		UE_LOG(LogGMCMotion, Warning, TEXT("[TFoot] %s -> wantRight=%d -> %s"),
			*Montage->GetName(), bRightFoot ? 1 : 0, Variant ? *Variant->GetName() : TEXT("(load failed, kept original)"));
	}
	return Variant ? Variant : Montage;
}

void UGMCMotion::StopTraversalMovement()
{
	if (bDebugTraversalWarp)
	{
		UE_LOG(LogGMCMotion, Warning, TEXT("[TExit] STOP (%s) | jumpOver=%d capturedExitVel=(%.0f,%.0f) speed2D=%.0f"),
			bZeroVelocityAtTraversalStart ? TEXT("vel->0") : TEXT("velocity kept"),
			bLastTraversalIsJumpOver ? 1 : 0, TraversalExitVelocity.X, TraversalExitVelocity.Y, TraversalExitVelocity.Size2D());
	}
	TraversalEntrySpeed = Velocity.Size2D();
	if (bZeroVelocityAtTraversalStart)
	{
		UpdateVelocity(FVector::ZeroVector);
	}
	else if (Velocity.Z > 0.0)
	{
		// Ground start: Space also fires the jump, whose impulse was integrated once (+9.7 cm in the first move), and the
		// traversal mode has no gravity to bring it back -> feet ~10 cm off the floor for the whole run-up (visible since
		// the 5.8 montages start earlier, 2026-10-01). Keep the run speed, drop the jump.
		UpdateVelocity(FVector(Velocity.X, Velocity.Y, 0.0));
	}
}

float UGMCMotion::GetConfirmedTraversalStartTime() const
{
	if (!bConfirmedStartTimePropSearched)
	{
		bConfirmedStartTimePropSearched = true;
		const FNumericProperty* Prop = CastField<FNumericProperty>(GetClass()->FindPropertyByName(TEXT("ConfirmedTraversalStartTime")));
		ConfirmedStartTimeProp = Prop && Prop->IsFloatingPoint() ? Prop : nullptr;
	}
	if (const FNumericProperty* Prop = CastField<FNumericProperty>(ConfirmedStartTimeProp))
	{
		return static_cast<float>(Prop->GetFloatingPointPropertyValue(Prop->ContainerPtrToValuePtr<void>(this)));
	}
	return -1.0f;
}

void UGMCMotion::RestoreTraversalExitVelocity()
{
	if (bDebugTraversalWarp)
	{
		UE_LOG(LogGMCMotion, Warning, TEXT("[TExit] RESTORE (inherit anim vel) | jumpOver=%d injectVel=(%.0f,%.0f) speed2D=%.0f"),
			bLastTraversalIsJumpOver ? 1 : 0, TraversalExitVelocity.X, TraversalExitVelocity.Y, TraversalExitVelocity.Size2D());
	}
	FVector ExitVelocity(TraversalExitVelocity.X, TraversalExitVelocity.Y, 0.0);

	// Exit in the air (vault over a drop: the anim's tail already descends at ~-400 cm/s): keep that fall speed. Restoring
	// Z = 0 restarted the fall from rest, a visible hang in mid-air (user report 2026-10-01). Never upward.
	// BP_GMCMovement sets the Grounded mode just before this call, so the mode says nothing: query the floor.
	bool bExitInAir = false;
	if (bKeepTraversalExitFallSpeed && TraversalExitVelocity.Z < 0.0)
	{
		UpdateFloor(CurrentFloor, FVector::DownVector, FloorTraceLength, FloorUpdateTolerance, FloorTraceShapeScale, false, true);
		if (!CurrentFloor.HasValidShapeData() || CurrentFloor.GetShapeDistanceToFloor() > MAX_DISTANCE_TO_FLOOR + 5.f)
		{
			bExitInAir = true;
			ExitVelocity.Z = TraversalExitVelocity.Z;
			SetMovementMode(EGMC_MovementMode::Airborne);
		}
	}

	// The cap is a ground thing: in the air the anim's horizontal speed is kept, like GASP58 (Catch_Vault exit 473 cm/s).
	const FVector Input = GetProcessedInputVector();
	const bool bInputAlong = !Input.IsNearlyZero()
		&& (ExitVelocity.GetSafeNormal2D() | FVector(Input.X, Input.Y, 0.).GetSafeNormal()) > 0.7;
	const bool bCarry = !bCapTraversalExitSpeed || bExitInAir || (bLastTraversalIsJumpOver && bInputAlong);
	if (!bCarry)
	{
		ExitVelocity = ExitVelocity.GetClampedToMaxSize2D(GetMaxSpeed());
	}
	UpdateVelocity(ExitVelocity);
	bTraversalExitCarry = bCarry && TraversalExitOverSpeedDeceleration > 0.0f;
}

void UGMCMotion::DebugLogTraversalChoice(
	const FString& Phase,
	bool bDetectionSuccess,
	UAnimMontage* DetectedMontage,
	float DetectedStartTime,
	bool bAcceptPending,
	UAnimMontage* SelectedMontage,
	float SelectedStartTime)
{
	if (!bDebugTraversalWarp)
	{
		return;
	}

	auto GetMontageVar = [this](const FName VarName) -> UAnimMontage*
		{
			if (const FObjectProperty* Property = FindFProperty<FObjectProperty>(GetClass(), VarName))
			{
				return Cast<UAnimMontage>(Property->GetObjectPropertyValue_InContainer(this));
			}
			return nullptr;
		};

	auto GetBoolVar = [this](const FName VarName) -> bool
		{
			if (const FBoolProperty* Property = FindFProperty<FBoolProperty>(GetClass(), VarName))
			{
				return Property->GetPropertyValue_InContainer(this);
			}
			return false;
		};

	auto GetFloatVar = [this](const FName VarName) -> double
		{
			if (const FNumericProperty* Property = FindFProperty<FNumericProperty>(GetClass(), VarName))
			{
				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(this);
				return Property->IsFloatingPoint()
					? Property->GetFloatingPointPropertyValue(ValuePtr)
					: static_cast<double>(Property->GetSignedIntPropertyValue(ValuePtr));
			}
			return 0.0;
		};

	UAnimMontage* PendingMontage = GetMontageVar(TEXT("PendingTraversalMontage"));
	UAnimMontage* ConfirmedMontage = GetMontageVar(TEXT("ConfirmedTraversalMontage"));
	UAnimMontage* ActiveMontage = GetActiveRootMotionMontage(MontageTracker);

	const bool bLocked = GetBoolVar(TEXT("bPendingTraversalChoiceLocked"));
	const double PendingStart = GetFloatVar(TEXT("PendingTraversalStartTime"));
	const double ConfirmedStart = GetFloatVar(TEXT("ConfirmedTraversalStartTime"));
	const double MaxAcceptedStart = GetFloatVar(TEXT("TraversalMaxAcceptedStartTime"));
	const int32 Auth = (GetOwner() && GetOwner()->HasAuthority()) ? 1 : 0;
	const bool bPendingValid = IsValid(PendingMontage);
	const FString PendingPath = GetPathNameSafe(PendingMontage);
	const FString DetectedPath = GetPathNameSafe(DetectedMontage ? DetectedMontage : SelectedMontage);

	auto HasTraversalFamily = [](const FString& Path) -> bool
		{
			return Path.Contains(TEXT("Hurdle"), ESearchCase::IgnoreCase)
				|| Path.Contains(TEXT("Vault"), ESearchCase::IgnoreCase)
				|| Path.Contains(TEXT("Mantle"), ESearchCase::IgnoreCase)
				|| Path.Contains(TEXT("Climb"), ESearchCase::IgnoreCase)
				|| Path.Contains(TEXT("Cliff"), ESearchCase::IgnoreCase);
		};

	auto GetTraversalFamily = [](const FString& Path) -> FString
		{
			if (Path.Contains(TEXT("Hurdle"), ESearchCase::IgnoreCase)) { return TEXT("Hurdle"); }
			if (Path.Contains(TEXT("Vault"), ESearchCase::IgnoreCase)) { return TEXT("Vault"); }
			if (Path.Contains(TEXT("Mantle"), ESearchCase::IgnoreCase)) { return TEXT("Mantle"); }
			if (Path.Contains(TEXT("Climb"), ESearchCase::IgnoreCase)) { return TEXT("Climb"); }
			if (Path.Contains(TEXT("Cliff"), ESearchCase::IgnoreCase)) { return TEXT("Cliff"); }
			return TEXT("None");
		};

	const FString PendingFamily = GetTraversalFamily(PendingPath);
	const FString DetectedFamily = GetTraversalFamily(DetectedPath);
	const bool bSameTraversalFamily = HasTraversalFamily(PendingPath)
		&& PendingFamily.Equals(DetectedFamily, ESearchCase::IgnoreCase);
	const bool bPendingInTraversalFolder = PendingPath.Contains(TEXT("/Traversal/"), ESearchCase::IgnoreCase);
	const bool bPendingStartInRange = PendingStart >= 0.0 && PendingStart <= MaxAcceptedStart;
	const bool bWouldAcceptPending = bLocked && bPendingValid && bSameTraversalFamily && bPendingInTraversalFolder;

	FString RejectReason;
	if (!bLocked)
	{
		RejectReason += TEXT("notLocked;");
	}
	if (!bPendingValid)
	{
		RejectReason += TEXT("pendingInvalid;");
	}
	if (!bSameTraversalFamily)
	{
		RejectReason += FString::Printf(TEXT("familyMismatch:%s!=%s;"), *PendingFamily, *DetectedFamily);
	}
	if (!bPendingInTraversalFolder)
	{
		RejectReason += TEXT("pendingNotInTraversalFolder;");
	}
	if (!bPendingStartInRange)
	{
		RejectReason += TEXT("pendingStartOutOfRange;");
	}
	if (RejectReason.IsEmpty())
	{
		RejectReason = TEXT("none");
	}

	UE_LOG(LogGMCMotion, Warning,
		TEXT("[TChoice] %s auth=%d replay=%d detectOk=%d acceptPending=%d wouldAccept=%d reason=%s | locked=%d pendingValid=%d sameFamily=%d pendingTraversalPath=%d pendingStartInRange=%d | detected=%s detFam=%s detStart=%.3f | selected=%s selStart=%.3f | pending=%s pFam=%s pStart=%.3f maxStart=%.3f confirmed=%s cStart=%.3f active=%s"),
		*Phase,
		Auth,
		CL_IsReplaying() ? 1 : 0,
		bDetectionSuccess ? 1 : 0,
		bAcceptPending ? 1 : 0,
		bWouldAcceptPending ? 1 : 0,
		*RejectReason,
		bLocked ? 1 : 0,
		bPendingValid ? 1 : 0,
		bSameTraversalFamily ? 1 : 0,
		bPendingInTraversalFolder ? 1 : 0,
		bPendingStartInRange ? 1 : 0,
		*GetNameSafe(DetectedMontage),
		*DetectedFamily,
		DetectedStartTime,
		*GetNameSafe(SelectedMontage),
		SelectedStartTime,
		*GetNameSafe(PendingMontage),
		*PendingFamily,
		PendingStart,
		MaxAcceptedStart,
		*GetNameSafe(ConfirmedMontage),
		ConfirmedStart,
		*GetNameSafe(ActiveMontage));
}

void UGMCMotion::UpdateTraversalCollisionIgnore()
{
	UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(UpdatedComponent);
	if (!Prim)
	{
		return;
	}

	// Kind of traversal read from the active montage itself (the bLastTraversal* flags are only refreshed once a warp
	// window opens, so before that they still describe the previous traversal).
	bool bCrossesOver = false;
	if (bDisableObstacleCollision && bTraversalWarpActive)
	{
		if (UAnimMontage* Montage = GetActiveRootMotionMontage(MontageTracker))
		{
			bCrossesOver = GetLastLandingWindowEnd(Montage) > 0.0f || Montage->GetName().Contains(TEXT("Vault"));
		}
	}
	const bool bWantIgnore = bCrossesOver;

	if (bWantIgnore && !IgnoredObstacle.IsValid())
	{
		const FVector Start = Prim->GetComponentLocation();
		FVector Dir = WarpFrontLedge - Start;
		Dir.Z = 0.0;
		if (!Dir.IsNearlyZero())
		{
			Dir.Normalize();
			const FVector End = Start + Dir * ObstacleTraceDistance;

			FHitResult Hit;
			FCollisionQueryParams Params(FName(TEXT("TraversalObstacle")), false, GetOwner());
			if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params) && Hit.GetComponent())
			{
				Prim->IgnoreComponentWhenMoving(Hit.GetComponent(), true);
				IgnoredObstacle = Hit.GetComponent();

				if (bDebugTraversalWarp)
				{
					UE_LOG(LogGMCMotion, Warning, TEXT("[TWarp] obstacle collision IGNORED: %s"), *GetNameSafe(Hit.GetComponent()));
				}
			}
		}
	}
	else if (!bWantIgnore && IgnoredObstacle.IsValid())
	{
		Prim->IgnoreComponentWhenMoving(IgnoredObstacle.Get(), false);
		IgnoredObstacle.Reset();

		if (bDebugTraversalWarp)
		{
			UE_LOG(LogGMCMotion, Warning, TEXT("[TWarp] obstacle collision RESTORED"));
		}
	}
}

void UGMCMotion::PostMovementUpdate_Implementation(float DeltaSeconds)
{
	Super::PostMovementUpdate_Implementation(DeltaSeconds);

	if (!bEndTraversalAtBlendOut || !bTraversalWarpActive || !SkeletalMesh)
	{
		return;
	}

	UAnimMontage* Montage = GetActiveRootMotionMontage(MontageTracker);
	if (!Montage)
	{
		return;
	}

	const float BlendStart = GetTraversalBlendOutTime(Montage);
	if (BlendStart <= 0.0f)
	{
		return;
	}

	const float Position = GetMontagePosition(SkeletalMesh, MontageTracker);
	if (Position < BlendStart)
	{
		return;
	}

	if (bDebugTraversalWarp)
	{
		UE_LOG(LogGMCMotion, Warning, TEXT("[TWarp] %s STOP-MONTAGE pos=%.3f blendStart=%.3f (return control)"),
			*GetNameSafe(Montage), Position, BlendStart);
	}

	StopMontage(SkeletalMesh, MontageTracker, TraversalBlendOutTime);
}

bool UGMCMotion::GetActiveWarpTarget(UAnimMontage* Montage, float Position, FVector& OutTarget, float& OutWindowEndTime, FName& OutWarpPointBone, FName& OutWarpTargetName) const
{
	for (const FAnimNotifyEvent& Event : Montage->Notifies)
	{
		const UAnimNotifyState_MotionWarping* MotionWarpingNotify = Cast<UAnimNotifyState_MotionWarping>(Event.NotifyStateClass);
		if (!MotionWarpingNotify)
		{
			continue;
		}

		const float Start = Event.GetTriggerTime();
		const float End = Event.GetEndTriggerTime();
		if (Position < Start || Position > End)
		{
			continue;
		}

		const URootMotionModifier_Warp* WarpModifier = Cast<URootMotionModifier_Warp>(MotionWarpingNotify->RootMotionModifier);
		if (!WarpModifier)
		{
			continue;
		}

		FVector Target;
		if (!GetTargetByName(WarpModifier->WarpTargetName, Target))
		{
			continue;
		}

		OutTarget = Target;
		OutWindowEndTime = End;
		OutWarpTargetName = WarpModifier->WarpTargetName;
		OutWarpPointBone = (WarpModifier->WarpPointAnimProvider == EWarpPointAnimProvider::Bone)
			? WarpModifier->WarpPointAnimBoneName
			: NAME_None;
		return true;
	}

	return false;
}

float UGMCMotion::GetTraversalBlendOutTime(UAnimMontage* Montage) const
{
	if (!Montage)
	{
		return 0.0f;
	}

	for (const FAnimNotifyEvent& Event : Montage->Notifies)
	{
		if (Event.NotifyStateClass && Event.NotifyStateClass->GetClass()->GetName().Contains(TEXT("MontageBlendOut")))
		{
			return Event.GetTriggerTime();
		}
	}

	return 0.0f;
}

float UGMCMotion::GetLastLandingWindowEnd(UAnimMontage* Montage) const
{
	float LastEnd = 0.0f;
	if (!Montage)
	{
		return LastEnd;
	}

	for (const FAnimNotifyEvent& Event : Montage->Notifies)
	{
		const UAnimNotifyState_MotionWarping* Notify = Cast<UAnimNotifyState_MotionWarping>(Event.NotifyStateClass);
		if (!Notify)
		{
			continue;
		}

		const URootMotionModifier_Warp* WarpModifier = Cast<URootMotionModifier_Warp>(Notify->RootMotionModifier);
		if (!WarpModifier)
		{
			continue;
		}

		const FString N = WarpModifier->WarpTargetName.ToString().TrimStartAndEnd();
		const bool bIsLanding =
			N.Equals(TEXT("BackLedge"), ESearchCase::IgnoreCase) ||
			N.Equals(TEXT("BackFloor"), ESearchCase::IgnoreCase);

		if (bIsLanding)
		{
			LastEnd = FMath::Max(LastEnd, Event.GetEndTriggerTime());
		}
	}

	return LastEnd;
}

float UGMCMotion::GetFirstWarpWindowStart(UAnimMontage* Montage) const
{
	float FirstStart = FLT_MAX;
	if (!Montage)
	{
		return FirstStart;
	}

	for (const FAnimNotifyEvent& Event : Montage->Notifies)
	{
		const UAnimNotifyState_MotionWarping* Notify = Cast<UAnimNotifyState_MotionWarping>(Event.NotifyStateClass);
		if (!Notify)
		{
			continue;
		}

		const URootMotionModifier_Warp* WarpModifier = Cast<URootMotionModifier_Warp>(Notify->RootMotionModifier);
		if (!WarpModifier)
		{
			continue;
		}

		FVector Target;
		if (!GetTargetByName(WarpModifier->WarpTargetName, Target))
		{
			continue;
		}

		FirstStart = FMath::Min(FirstStart, Event.GetTriggerTime());
	}

	return FirstStart;
}

float UGMCMotion::GetMaxWarpCorrectedZ(UAnimMontage* Montage, float HalfHeight) const
{
	float MaxZ = -UE_BIG_NUMBER;
	if (!Montage)
	{
		return MaxZ;
	}

	for (const FAnimNotifyEvent& Event : Montage->Notifies)
	{
		const UAnimNotifyState_MotionWarping* Notify = Cast<UAnimNotifyState_MotionWarping>(Event.NotifyStateClass);
		if (!Notify)
		{
			continue;
		}

		const URootMotionModifier_Warp* WarpModifier = Cast<URootMotionModifier_Warp>(Notify->RootMotionModifier);
		if (!WarpModifier)
		{
			continue;
		}

		FVector Target;
		if (!GetTargetByName(WarpModifier->WarpTargetName, Target))
		{
			continue;
		}

		MaxZ = FMath::Max(MaxZ, Target.Z + HalfHeight);
	}

	return MaxZ;
}

FVector UGMCMotion::ComputeRootMinusBoneOffset(UAnimMontage* Montage, float Time, FName BoneName) const
{
	if (!Montage || BoneName.IsNone())
	{
		return FVector::ZeroVector;
	}

	USkeleton* Skeleton = Montage->GetSkeleton();
	if (!Skeleton)
	{
		return FVector::ZeroVector;
	}

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();
	const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	TArray<FBoneIndexType> RequiredBoneIndexArray = { 0, static_cast<FBoneIndexType>(BoneIndex) };
	RefSkeleton.EnsureParentsExistAndSort(RequiredBoneIndexArray);

	FBoneContainer BoneContainer(
		RequiredBoneIndexArray,
		UE::Anim::FCurveFilterSettings(UE::Anim::ECurveFilterMode::DisallowAll),
		*Skeleton);

	FCSPose<FCompactPose> Pose;
	UMotionWarpingUtilities::ExtractComponentSpacePose(Montage, BoneContainer, Time, /*bExtractRootMotion=*/false, Pose);

	const FCompactPoseBoneIndex CompactRoot(0);
	const FCompactPoseBoneIndex CompactBone(RequiredBoneIndexArray.Num() - 1);

	const FTransform RootCS = Pose.GetComponentSpaceTransform(CompactRoot);
	const FTransform BoneCS = Pose.GetComponentSpaceTransform(CompactBone);

	return RootCS.GetTranslation() - BoneCS.GetTranslation();
}

bool UGMCMotion::GetTargetByName(FName Name, FVector& OutTarget) const
{
	const FString N = Name.ToString().TrimStartAndEnd();

	if (N.Equals(TEXT("FrontLedge"), ESearchCase::IgnoreCase)) { OutTarget = WarpFrontLedge; return true; }
	if (N.Equals(TEXT("BackLedge"), ESearchCase::IgnoreCase)) { OutTarget = WarpBackLedge;  return true; }
	if (N.Equals(TEXT("BackFloor"), ESearchCase::IgnoreCase)) { OutTarget = WarpBackFloor;  return true; }

	return false;
}

// =====================================================================================
//  GASP Pipeline
// =====================================================================================

void UGMCMotion::PrePhysicsUpdate_Implementation(float DeltaSeconds)
{
	// Let the BP pipeline run (Jump, UpdateGait, SetGait, Update_Grounded_Physic, traversal).
	// Equipment gate is handled in UpdateMovementModeDynamic (catches bTraversalWarpActive
	// regardless of when/where it's set). TraversalHolster flags exempt traversals via
	// bTraversalExemptFromEquipmentGate on UGMCMotion.
	Super::PrePhysicsUpdate_Implementation(DeltaSeconds);

	// Initialize OverridenDesiredFacing from the actor's current rotation on the first tick,
	// so the facing spring starts at the actual pawn facing instead of world-forward (0,0,0).
	if (!bFacingInitialized && UpdatedComponent)
	{
		OverridenDesiredFacing = UpdatedComponent->GetComponentRotation();
		FacingSpringPositionYaw = OverridenDesiredFacing.Yaw;
		bFacingInitialized = true;
	}

	// One-time validation: warn if rotation offset curves are not assigned.
	if (!bRotationOffsetCurvesValidated)
	{
		bRotationOffsetCurvesValidated = true;

		struct FCurveCheck { UCurveFloat* Curve; const TCHAR* Name; };
		const FCurveCheck Checks[] = {
			{ RotationOffsetCurve_F,  TEXT("F")  },
			{ RotationOffsetCurve_B,  TEXT("B")  },
			{ RotationOffsetCurve_LL, TEXT("LL") },
			{ RotationOffsetCurve_LR, TEXT("LR") },
			{ RotationOffsetCurve_RL, TEXT("RL") },
			{ RotationOffsetCurve_RR, TEXT("RR") },
		};

		TArray<FString> Missing;
		for (const FCurveCheck& C : Checks)
		{
			if (!C.Curve)
			{
				Missing.Add(C.Name);
			}
		}

		if (Missing.Num() > 0)
		{
			const FString MissingList = FString::Join(Missing, TEXT(", "));
			const FString Msg = FString::Printf(
				TEXT("GMCMotion: Rotation offset curves not assigned: [%s]. "
					 "Open BP_GMCMovement -> Details -> GMCMotion|GASP|RotationOffset and assign the "
					 "Curve_RotationOffset_* assets from /Game/Blueprints/Data/."),
				*MissingList);

			UE_LOG(LogGMCMotion, Warning, TEXT("%s"), *Msg);

			if (GEngine)
			{
				GEngine->AddOnScreenDebugMessage(-1, 10.f, FColor::Yellow, Msg);
			}
		}

		if (!RotationOffsetCurve_SlideKnees)
		{
			UE_LOG(LogGMCMotion, Log,
				TEXT("GMCMotion: RotationOffsetCurve_SlideKnees not assigned (optional — only needed if sliding is used). "
					 "Assign Curve_RotationOffset_Slide_Knees from /Game/Blueprints/Data/ in BP_GMCMovement details."));
		}
	}

	// GASP pipeline — runs after the BP's PrePhysicsUpdate event completes.
	// CalculateRotations_CPP is called from BP's own PrePhysicsUpdate chain, and movement
	// direction is owned by BP's GetMovementDirectionAndOffset — neither is called here.
	UpdateInputAcceleration();
	UpdateRotationMode_CPP();
	UpdateGroundedFacing(DeltaSeconds);
	UpdateTrajectoryMetrics();
}

void UGMCMotion::VariableToAnimBPBridge(const FGASPBridgeData& BD)
{
	CachedAimingRotation = BD.AimingRotation;
	TurningStrength = BD.TurningStrength;
	Trj_TurnAngle = BD.TurnAngle;
	FutureFacingDelta = BD.FutureFacingDelta;
	RotationMode = static_cast<EGMCMotion_RotationMode>(BD.RotationMode);
	MovementDirection = BD.MovementDirection;
	CurrentGait = static_cast<EGMCMotion_Gait>(BD.Gait);
	CurrentStance = static_cast<EGMCMotion_Stance>(BD.Stance);
	Downed = BD.Downed;
}

void UGMCMotion::UpdateCameraYawLimit()
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	APlayerCameraManager* Camera = PC && PC->IsLocalController() ? PC->PlayerCameraManager.Get() : nullptr;

	// Lost the camera (unpossessed, controller changed) while limited: give the old one its values back.
	if (bCameraYawLimited && LimitedCamera.Get() != Camera)
	{
		if (APlayerCameraManager* Old = LimitedCamera.Get())
		{
			Old->ViewYawMin = SavedViewYawMin;
			Old->ViewYawMax = SavedViewYawMax;
		}
		bCameraYawLimited = false;
		bCameraYawLimitHeld = false;
		LimitedCamera.Reset();
	}
	if (!Camera)
	{
		return;
	}

	bool bConditionMet = true;
	if (!CameraYawLimitConditionProperty.IsNone())
	{
		const FBoolProperty* Property = FindFProperty<FBoolProperty>(Pawn->GetClass(), CameraYawLimitConditionProperty);
		bConditionMet = Property && Property->GetPropertyValue_InContainer(Pawn);
	}

	float Range = 0.f;
	bool bControlLost = false;
	if (bConditionMet && IsSlopeSliding())
	{
		Range = SlopeSlideCameraYawRange;
	}
	if (bConditionMet && Range <= 0.f && KnockdownCameraYawRange > 0.f)
	{
		for (const UActorComponent* Component : Pawn->GetComponents())
		{
			const IGMCMotionControlLossSource* Source = Cast<IGMCMotionControlLossSource>(Component);
			if (Source && Source->IsPlayerControlLost())
			{
				Range = KnockdownCameraYawRange;
				bControlLost = true;
				break;
			}
		}
	}

	if (Range > 0.f)
	{
		if (!bCameraYawLimited)
		{
			SavedViewYawMin = Camera->ViewYawMin;
			SavedViewYawMax = Camera->ViewYawMax;
			LimitedCamera = Camera;
		}
		// The slide re-centres every frame on the facing; a control loss holds the facing it started at.
		if (!bControlLost || !bCameraYawLimited || !bCameraYawLimitHeld)
		{
			CameraYawLimitCentre = GetActorRotation_GMC().Yaw;
		}
		bCameraYawLimitHeld = bControlLost;
		ComputeCameraYawWindow(CameraYawLimitCentre, Range, Camera->ViewYawMin, Camera->ViewYawMax);
		bCameraYawLimited = true;
	}
	else if (bCameraYawLimited)
	{
		Camera->ViewYawMin = SavedViewYawMin;
		Camera->ViewYawMax = SavedViewYawMax;
		bCameraYawLimited = false;
		bCameraYawLimitHeld = false;
		LimitedCamera.Reset();
	}
}

void UGMCMotion::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UpdateCameraYawLimit();

	// Clear hand IK when traversal is not active. PreProcessRootMotion sets it to 1
	// during the FrontLedge window; this clears it during gaps, other windows, and
	// after the traversal ends.
	if (!bTraversalWarpActive)
	{
		TraversalHandIKAlpha = 0.f;
	}

	// On simulated proxies the BP gameplay logic (which calls VariableToAnimBPBridge) doesn't run,
	// so the non-UPROPERTY C++ members (MovementDirection, RotationMode, etc.) never get updated.
	// The BP-bound variables DO receive correct replicated values on sim proxies, so we pull them
	// into the C++ members here via FindPropertyByName. This runs every frame on all roles but
	// is only needed on sim proxies — authority/autonomous already get the bridge call from BP.
	//
	// ReplicatedSpeed and ReplicatedLocomotionAngle are computed locally from the replicated
	// velocity and rotation because MovementUpdate_Implementation (which computes them on
	// server/autonomous) does not run on sim proxies and they are not GMC-bound.
	if (IsSimulatedProxy())
	{
		// --- Compute locomotion values from replicated movement state ---
		ReplicatedSpeed = GetLinearVelocity_GMC().Size2D();

		const FVector Vel2D(GetLinearVelocity_GMC().X, GetLinearVelocity_GMC().Y, 0.f);
		if (Vel2D.SizeSquared() > 1.f && UpdatedComponent)
		{
			const float VelYaw = Vel2D.Rotation().Yaw;
			const float ActorYaw = UpdatedComponent->GetComponentRotation().Yaw;
			ReplicatedLocomotionAngle = FRotator::NormalizeAxis(VelYaw - ActorYaw);
		}

		const UClass* MyClass = GetClass();

		// MovementDirection is deliberately NOT recomputed here. It is GMC-bound (BindHalfByte,
		// Periodic_Output) and the replicated server value is computed from INPUT direction, not
		// velocity relative to the capsule. Recomputing locally is wrong in strafe/aiming mode:
		// the capsule faces velocity via RotationOffset, so ReplicatedLocomotionAngle is ~0 for
		// every strafe direction and they all collapse to Forward.

		// RotationMode (BP variable is a byte/enum)
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("RotationMode")))
		{
			if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				RotationMode = static_cast<EGMCMotion_RotationMode>(*ByteProp->ContainerPtrToValuePtr<uint8>(this));
			}
			else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
			{
				const FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				RotationMode = static_cast<EGMCMotion_RotationMode>(UnderlyingProp->GetSignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(this)));
			}
		}

		// Gait (BP variable is a byte/enum)
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("Gait")))
		{
			if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				CurrentGait = static_cast<EGMCMotion_Gait>(*ByteProp->ContainerPtrToValuePtr<uint8>(this));
			}
			else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
			{
				const FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				CurrentGait = static_cast<EGMCMotion_Gait>(UnderlyingProp->GetSignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(this)));
			}
		}

		// Stance (BP variable is a byte/enum — E_Stance)
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("Stance")))
		{
			if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				CurrentStance = static_cast<EGMCMotion_Stance>(*ByteProp->ContainerPtrToValuePtr<uint8>(this));
			}
			else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
			{
				const FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				CurrentStance = static_cast<EGMCMotion_Stance>(UnderlyingProp->GetSignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(this)));
			}
		}

		// Downed is deliberately NOT read from the BP variable here, unlike its neighbours.
		// It is GMC-bound in C++ (BI_Downed), so a simulated proxy already holds the correct
		// replicated value. Copying BP's own "Downed" over it would replace a good value with
		// the BP-local one, which is never written on a sim proxy and so reads false forever.
		// Same reasoning as the MovementDirection note above.

		// TurningStrength (BP name: TurningStrenght — note the typo matches the BP)
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("TurningStrenght")))
		{
			if (const FDoubleProperty* DblProp = CastField<FDoubleProperty>(Prop))
			{
				TurningStrength = *DblProp->ContainerPtrToValuePtr<double>(this);
			}
			else if (const FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))
			{
				TurningStrength = *FltProp->ContainerPtrToValuePtr<float>(this);
			}
		}

		// FutureFacingDelta
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("FutureFacingDelta")))
		{
			if (const FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))
			{
				FutureFacingDelta = *FltProp->ContainerPtrToValuePtr<float>(this);
			}
		}

		// Trj_TurnAngle
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("Trj_TurnAngle")))
		{
			if (const FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))
			{
				Trj_TurnAngle = *FltProp->ContainerPtrToValuePtr<float>(this);
			}
		}

		// AimingRotation → CachedAimingRotation
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("AimingRotation")))
		{
			if (const FStructProperty* SP = CastField<FStructProperty>(Prop))
			{
				if (SP->Struct == TBaseStructure<FRotator>::Get())
				{
					CachedAimingRotation = *SP->ContainerPtrToValuePtr<FRotator>(this);
				}
			}
		}

		// ReplicatedSpeed (float — C++ UPROPERTY, replicated via BP's ReplicationGraph)
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("ReplicatedSpeed")))
		{
			if (const FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))
			{
				ReplicatedSpeed = *FltProp->ContainerPtrToValuePtr<float>(this);
			}
			else if (const FDoubleProperty* DblProp = CastField<FDoubleProperty>(Prop))
			{
				ReplicatedSpeed = static_cast<float>(*DblProp->ContainerPtrToValuePtr<double>(this));
			}
		}

		// ReplicatedLocomotionAngle (float — C++ UPROPERTY, replicated via BP's ReplicationGraph)
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("ReplicatedLocomotionAngle")))
		{
			if (const FFloatProperty* FltProp = CastField<FFloatProperty>(Prop))
			{
				ReplicatedLocomotionAngle = *FltProp->ContainerPtrToValuePtr<float>(this);
			}
			else if (const FDoubleProperty* DblProp = CastField<FDoubleProperty>(Prop))
			{
				ReplicatedLocomotionAngle = static_cast<float>(*DblProp->ContainerPtrToValuePtr<double>(this));
			}
		}
	}

	// Compute frame-over-frame animation helpers (yaw rates, acceleration).
	UpdateLocomotionHelpers(DeltaTime);
}

void UGMCMotion::ApplyRotation(bool bIsDirectBotMove, const FGMC_RootMotionVelocitySettings& RootMotionMetaData, float DeltaSeconds)
{
	// The ragdoll sets the yaw itself (PhysicsRagdoll).
	if (GetMovementMode() == RagdollMovementMode)
	{
		return;
	}

	if (bIsDirectBotMove)
	{
		Super::ApplyRotation(bIsDirectBotMove, RootMotionMetaData, DeltaSeconds);
		return;
	}

	// During traversal, rotation is handled by the warp in PreProcessRootMotion.
	if (bTraversalWarpActive)
	{
		return;
	}


	// Skip rotation during root motion when the montage opts out of rotation application.
	if (HasRootMotion() && !RootMotionMetaData.bApplyRotationWithRootMotion)
	{
		return;
	}

	// Rotation toward OverridenDesiredFacing at RotationRate.
	//
	// NOTE: UpdateGroundedFacing writes OverridenDesiredFacing DIRECTLY to the target (zero-lag
	// tracking); the facing spring only runs during the post-reversal settling window, which is
	// itself unreachable while RotationRate is 0. And a RotationRate of 0 means RotateYawTowards-
	// Direction snaps instantly rather than rate-limiting — so despite the name, this is currently
	// an instant snap, not a smoothed rotation.
	const float YawBefore = UpdatedComponent ? UpdatedComponent->GetComponentRotation().Yaw : 0.f;

	const FVector FacingDirection = OverridenDesiredFacing.Vector();
	if (!FacingDirection.IsNearlyZero())
	{
		if (bUseSafeRotations)
		{
			RotateYawTowardsDirectionSafe(FacingDirection, RotationRate, SafeRotationCollisionTolerance, DeltaSeconds);
		}
		else
		{
			RotateYawTowardsDirection(FacingDirection, RotationRate, DeltaSeconds);
		}
	}

	// Compute angular velocity from actual rotation step (for trajectory metrics & anim).
	if (UpdatedComponent && DeltaSeconds > UE_KINDA_SMALL_NUMBER)
	{
		const float YawAfter = UpdatedComponent->GetComponentRotation().Yaw;
		const float StepDeg = FMath::FindDeltaAngleDegrees(YawBefore, YawAfter);
		AngularVelocityRad = FMath::DegreesToRadians(StepDeg / DeltaSeconds);
	}
}

// =====================================================================================
//  GASP Infrastructure
// =====================================================================================

void UGMCMotion::BindReplicationData_Implementation()
{
	Super::BindReplicationData_Implementation();

	// Gait, Stance, WantsToSprint, WantsToCrouch, WantsToJump
	// are bound in Blueprint (BP_GMCMovement). Only C++-owned variables are bound here.
	//
	// These bindings are unconditional. They used to sit inside "if (bEnableGASPPipeline)" so the
	// reference BP's ReplicationGraph could bind the same variables instead — double-binding the
	// same UPROPERTY crashes or silently corrupts replication state. That toggle is gone: bind
	// registration defines the network channel layout, so it must never depend on an EditAnywhere
	// per-instance bool that two pawn types can disagree about.
	BI_RotationMode = BindHalfByte(
		reinterpret_cast<uint8&>(RotationMode),
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	BI_MovementDirection = BindHalfByte(
		MovementDirection,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::Periodic_Output,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	BI_WantsToTraverse = BindBool(
		WantsToTraverse,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	// --- Trajectory predictor data ---

	BI_CustomComputeInputAcceleration = BindCompressedVector(
		CustomComputeInputAcceleration,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	BI_OrientationIntent = BindCompressedVector(
		OrientationIntent,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	BI_OverridenDesiredFacing = BindCompressedRotator(
		OverridenDesiredFacing,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	// --- Trajectory metrics ---

	BI_Trj_FutureVelocity = BindCompressedVector(
		Trj_FutureVelocity,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	BI_Trj_NearFutureVelocity = BindCompressedVector(
		Trj_NearFutureVelocity,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	BI_Trj_TurnAngle = BindSinglePrecisionFloat(
		Trj_TurnAngle,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	BI_AngularVelocityRad = BindSinglePrecisionFloat(
		AngularVelocityRad,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	// --- New pipeline variables (migrated from BP) ---

	BI_WantsToStrafe = BindBool(
		WantsToStrafe,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	BI_CachedAimingRotation = BindCompressedRotator(
		CachedAimingRotation,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	BI_RotationOffset = BindDoublePrecisionFloat(
		RotationOffset,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	BI_FutureFacingDelta = BindCompressedSinglePrecisionFloat(
		FutureFacingDelta,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	BI_Trj_IsCircling = BindBool(
		Trj_IsCircling,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	BI_TurningStrength = BindDoublePrecisionFloat(
		TurningStrength,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::Linear
	);

	// Downed — ServerAuth, unlike every other bool here.
	//
	// WantsToStrafe / WantsToTraverse are client INPUTS, so ClientAuth_Input is right for
	// them: the player decides. Downed is not an input — the server decides it from health,
	// and a client must never be able to assert it. Hence ServerAuth_Output_ClientValidated,
	// matching MovementDirection (the other server-computed value in this block).
	//
	// PeriodicAndOnChange_Output rather than MovementDirection's Periodic_Output because
	// this flips rarely and the animation must switch the moment it does, not at the next
	// periodic send.
	//
	// If BP starts predicting Downed locally on the owning client (setting it before the
	// server agrees) this is the line to revisit — ServerAuth will overwrite the prediction.
	BI_Downed = BindBool(
		Downed,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	// The slope slide request: originates on the client (a crouch press while sprinting), decides the slide's start and
	// its release, so it travels with the move.
	BI_SlopeSlideRequested = BindBool(
		bSlopeSlideRequested,
		EGMC_PredictionMode::ClientAuth_Input,
		EGMC_CombineMode::CombineIfUnchanged,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	// Ragdoll mode: decided by the server (UGMCMotionRagdollComponent), same contract as Downed.
	BI_RagdollMode = BindBool(
		bRagdollMode,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::PeriodicAndOnChange_Output,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	// Post-jump-over speed carry: movement-only state, carried from move to move.
	// Ground velocity smoothing state (bSmoothGroundVelocity): carried from move to move like the velocity.
	BI_SmoothIntermediateVelocity = BindCompressedVector2D(
		SmoothIntermediateVelocity,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::NearestNeighbour
	);
	BI_SmoothSpringVelocity = BindCompressedVector2D(
		SmoothSpringVelocity,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::NearestNeighbour
	);
	BI_SmoothSpringAcceleration = BindCompressedVector2D(
		SmoothSpringAcceleration,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::NearestNeighbour
	);
	BI_SmoothStateValid = BindBool(
		bSmoothStateValid,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	BI_TraversalExitCarry = BindBool(
		bTraversalExitCarry,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	// Traversal entry speed: movement-only state (captured at traversal start, read during the pre-window phase).
	BI_TraversalEntrySpeed = BindSinglePrecisionFloat(
		TraversalEntrySpeed,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::NearestNeighbour
	);

	// Jump-over window exit easing state (see WarpWindowExitBlendTime).
	BI_LastWarpHorizontalSpeed = BindSinglePrecisionFloat(
		LastWarpHorizontalSpeed,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::NearestNeighbour
	);
	BI_LastWarpWindowEnd = BindSinglePrecisionFloat(
		LastWarpWindowEnd,
		EGMC_PredictionMode::ServerAuth_Output_ClientValidated,
		EGMC_CombineMode::AlwaysCombine,
		EGMC_SimulationMode::None,
		EGMC_InterpolationFunction::NearestNeighbour
	);
}

// =====================================================================================
//  MovementUpdate — runs inside the GMC prediction loop (authority + autonomous proxy).
//  Computes replicated locomotion values (speed, angle, landing velocity).
// =====================================================================================

void UGMCMotion::MovementUpdate_Implementation(float DeltaSeconds)
{
	Super::MovementUpdate_Implementation(DeltaSeconds);

	// --- ReplicatedSpeed: 2D ground speed ---
	ReplicatedSpeed = GetLinearVelocity_GMC().Size2D();

	// --- ReplicatedLocomotionAngle: velocity direction relative to actor facing [-180, 180] ---
	const FVector Vel2D(GetLinearVelocity_GMC().X, GetLinearVelocity_GMC().Y, 0.f);
	if (Vel2D.SizeSquared() > 1.f && UpdatedComponent)
	{
		const float VelYaw = Vel2D.Rotation().Yaw;
		const float ActorYaw = UpdatedComponent->GetComponentRotation().Yaw;
		ReplicatedLocomotionAngle = FRotator::NormalizeAxis(VelYaw - ActorYaw);
	}
	// else: keep previous angle (avoids snapping to 0 when stationary)

	// --- LastLandingVelocity: capture on Airborne->Grounded transition ---
	const bool bCurrentlyAirborne = IsAirborne();
	if (bPreviousWasAirborne && !bCurrentlyAirborne)
	{
		// Just landed — capture the velocity from the moment before grounding.
		// GetLinearVelocity_GMC() is already the post-landing velocity;
		// use the animation velocity cache which was set last frame while still airborne.
		LastLandingVelocity = PreviousAnimVelocity;
	}
	bPreviousWasAirborne = bCurrentlyAirborne;
}

// =====================================================================================
//  UpdateLocomotionHelpers — frame-over-frame yaw rates and animation acceleration.
//  Called from TickComponent. Animation-facing only, no prediction needed.
// =====================================================================================

void UGMCMotion::UpdateLocomotionHelpers(float DeltaTime)
{
	if (DeltaTime < UE_KINDA_SMALL_NUMBER || !UpdatedComponent)
	{
		return;
	}

	// --- Component yaw rate ---
	const float CurrentYaw = UpdatedComponent->GetComponentRotation().Yaw;
	CachedComponentYawRate = FMath::FindDeltaAngleDegrees(PreviousComponentYaw, CurrentYaw) / DeltaTime;
	PreviousComponentYaw = CurrentYaw;

	// --- Aim yaw rate ---
	const float CurrentAimYaw = GetControllerRotation_GMC().Yaw;
	CachedAimYawRate = FMath::FindDeltaAngleDegrees(PreviousAimYaw, CurrentAimYaw) / DeltaTime;
	PreviousAimYaw = CurrentAimYaw;

	// --- Animation acceleration (velocity delta / dt) ---
	const FVector CurrentVel = GetLinearVelocity_GMC();
	CachedAnimAcceleration = (CurrentVel - PreviousAnimVelocity) / DeltaTime;
	PreviousAnimVelocity = CurrentVel;
}

// =====================================================================================
//  Locomotion Getters (previously stubs from UGMCLoco_MovementComponent separation)
// =====================================================================================

bool UGMCMotion::IsInputPresent(bool bAllowGrace) const
{
	return !GetProcessedInputVector().IsNearlyZero();
}

bool UGMCMotion::IsObstacleTooLowForTraversal(float ObstacleHeight) const
{
	return MinTraversalObstacleHeight > 0.f && ObstacleHeight < MinTraversalObstacleHeight;
}

bool UGMCMotion::ShouldBlockTraversalForEquipment(UAnimMontage* TraversalMontage) const
{
	if (!bEquipmentBlocksTraversal)
	{
		return false;
	}

	// Exempt montages (hurdles, short vaults) are allowed even with weapon in hand
	if (TraversalMontage && TraversalMontagesAllowedWithWeapon.Contains(TraversalMontage))
	{
		return false;
	}

	return true;
}

bool UGMCMotion::IsTraversalBlockedByEquipment(AActor* OwnerActor, UAnimMontage* TraversalMontage)
{
	if (!IsValid(OwnerActor))
	{
		return false;
	}

	UGMCMotion* Motion = OwnerActor->FindComponentByClass<UGMCMotion>();
	if (!Motion)
	{
		return false;
	}

	const bool bBlocked = Motion->ShouldBlockTraversalForEquipment(TraversalMontage);

	// If traversal is allowed despite equipment blocking being active (montage is in the exempt set),
	// set the exempt flag so the safety-net gate in UpdateMovementModeDynamic also passes.
	if (!bBlocked && Motion->bEquipmentBlocksTraversal)
	{
		Motion->bTraversalExemptFromEquipmentGate = true;
	}

	return bBlocked;
}

FRotator UGMCMotion::GetCurrentAimRotation() const
{
	// CachedAimingRotation is computed by CalculateRotations_CPP (or BP bridge)
	// and replicated for sim proxies — consistent across roles.
	return CachedAimingRotation;
}

float UGMCMotion::GetCurrentAimYawRate() const
{
	return CachedAimYawRate;
}

float UGMCMotion::GetCurrentComponentYawRate() const
{
	return CachedComponentYawRate;
}

FVector UGMCMotion::GetCurrentAnimationAcceleration() const
{
	return CachedAnimAcceleration;
}

bool UGMCMotion::IsStopPredicted(FVector& OutStopLocation) const
{
	if (!bPrecalculateDistanceMatches)
	{
		OutStopLocation = FVector::ZeroVector;
		return false;
	}

	const FVector Vel = GetLinearVelocity_GMC();
	const float Speed2D = Vel.Size2D();

	if (Speed2D < 10.f)
	{
		OutStopLocation = FVector::ZeroVector;
		return false;
	}

	bool bShouldStop = false;

	// Case 1: No input + moving = predict stop (original check).
	if (GetProcessedInputVector().IsNearlyZero())
	{
		bShouldStop = true;
	}
	// Case 2: Sprint direction reversal in strafing/aiming mode. The character must
	// decelerate to zero before accelerating in the new direction — kinematically
	// equivalent to a stop. Without this, the animation system plays a pivot (body
	// rotation) instead of a stop transition (feet planting without rotation).
	else if (CurrentGait == EGMCMotion_Gait::Sprint
		&& RotationMode == EGMCMotion_RotationMode::Aiming)
	{
		const FVector InputVec = GetProcessedInputVector();
		const FVector Accel = InputVec * GetInputAcceleration();
		if (!Accel.IsNearlyZero())
		{
			const FVector Vel2D(Vel.X, Vel.Y, 0.f);
			const FVector Accel2D(Accel.X, Accel.Y, 0.f);
			const float Dot = FVector::DotProduct(Vel2D.GetSafeNormal(), Accel2D.GetSafeNormal());
			// Input is more than ~120° from velocity — character will decelerate to zero.
			if (Dot < -0.5f)
			{
				bShouldStop = true;
			}
		}
	}

	if (!bShouldStop)
	{
		OutStopLocation = FVector::ZeroVector;
		return false;
	}

	// v^2 / (2 * deceleration * friction)
	const float Decel = GetBrakingDeceleration();
	const float Friction = GetGroundFriction();
	const float Denominator = 2.f * Decel * FMath::Max(Friction, 0.01f);

	if (Denominator < UE_KINDA_SMALL_NUMBER)
	{
		OutStopLocation = FVector::ZeroVector;
		return false;
	}

	const float StopDist = (Speed2D * Speed2D) / Denominator;
	const FVector Dir2D = FVector(Vel.X, Vel.Y, 0.f).GetSafeNormal();

	OutStopLocation = (UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector)
	                 + Dir2D * StopDist;
	return true;
}

bool UGMCMotion::IsPivotPredicted(FVector& OutPivotLocation) const
{
	// Forward-only mode (DirectionThresholdMode == 2): suppress pivot prediction entirely.
	// Pivot redirect animations rotate the upper body, which breaks first-person view
	// where the head must always face the camera direction.
	if (!bPrecalculateDistanceMatches || DirectionThresholdMode == 2)
	{
		OutPivotLocation = FVector::ZeroVector;
		return false;
	}

	// Sprint direction reversals in aiming mode are handled as STOP predictions (feet
	// planting without body rotation) rather than pivots (body rotation). Suppress pivot
	// here so the animation system picks the stop transition instead.
	if (CurrentGait == EGMCMotion_Gait::Sprint
		&& RotationMode == EGMCMotion_RotationMode::Aiming)
	{
		const FVector InputVec = GetProcessedInputVector();
		if (!InputVec.IsNearlyZero())
		{
			const FVector Vel = GetLinearVelocity_GMC();
			const FVector Accel = InputVec * GetInputAcceleration();
			if (!Accel.IsNearlyZero() && Vel.Size2D() >= 10.f)
			{
				const FVector Vel2D(Vel.X, Vel.Y, 0.f);
				const FVector Accel2D(Accel.X, Accel.Y, 0.f);
				const float Dot = FVector::DotProduct(Vel2D.GetSafeNormal(), Accel2D.GetSafeNormal());
				if (Dot < -0.5f)
				{
					OutPivotLocation = FVector::ZeroVector;
					return false;
				}
			}
		}
	}

	const FVector Vel = GetLinearVelocity_GMC();
	const float Speed2D = Vel.Size2D();

	if (Speed2D < 10.f || GetProcessedInputVector().IsNearlyZero())
	{
		OutPivotLocation = FVector::ZeroVector;
		return false;
	}

	// Check angle between velocity and acceleration (input direction)
	const FVector Accel = GetProcessedInputVector() * GetInputAcceleration();
	if (Accel.IsNearlyZero())
	{
		OutPivotLocation = FVector::ZeroVector;
		return false;
	}

	const FVector Vel2D(Vel.X, Vel.Y, 0.f);
	const FVector Accel2D(Accel.X, Accel.Y, 0.f);

	const float Dot = FVector::DotProduct(Vel2D.GetSafeNormal(), Accel2D.GetSafeNormal());
	const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Dot, -1.f, 1.f)));

	const float ClampedThreshold = FMath::Clamp(PivotPredictionAngleThreshold, 90.f, 179.f);

	if (AngleDeg < ClampedThreshold)
	{
		OutPivotLocation = FVector::ZeroVector;
		return false;
	}

	// Pivot point: where the pawn decelerates to zero then reverses.
	// Same kinematic as stop prediction.
	const float Decel = GetBrakingDeceleration();
	const float Friction = GetGroundFriction();
	const float Denominator = 2.f * Decel * FMath::Max(Friction, 0.01f);

	if (Denominator < UE_KINDA_SMALL_NUMBER)
	{
		OutPivotLocation = FVector::ZeroVector;
		return false;
	}

	const float PivotDist = (Speed2D * Speed2D) / Denominator;
	const FVector Dir2D = Vel2D.GetSafeNormal();

	OutPivotLocation = (UpdatedComponent ? UpdatedComponent->GetComponentLocation() : FVector::ZeroVector)
	                  + Dir2D * PivotDist;
	return true;
}

void UGMCMotion::UpdateInputAcceleration()
{
	// Exact match of BP SetComputeInputAcceleration:
	//   GetProcessedInputVector() -> VectorClampSizeMax(1) -> * GetInputAcceleration() -> ClampVectorSize(-1, 1)
	// The final ClampVectorSize(-1,1) clamps the result to unit magnitude (direction only).
	// This is what GASP's trajectory predictor expects — a normalised input direction, not a full acceleration vector.
	FVector Input = GetProcessedInputVector();
	CustomComputeInputAcceleration = (Input.GetClampedToMaxSize(1.0) * GetInputAcceleration()).GetClampedToSize(-1.0, 1.0);
}

void UGMCMotion::ComputeCustomInputAcceleration_Implementation(const FVector2D& InputAxis)
{
	if (InputAxis.IsNearlyZero())
	{
		CustomComputeInputAcceleration = FVector::ZeroVector;
		return;
	}

	// Get camera yaw from the owning pawn's controller.
	FRotator CameraRot = FRotator::ZeroRotator;
	if (const APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		if (const AController* PC = Pawn->GetController())
		{
			CameraRot = PC->GetControlRotation();
		}
	}

	// Flatten to yaw only.
	const FRotator YawRot(0.0f, CameraRot.Yaw, 0.0f);

	// Transform 2D input by camera yaw → world direction.
	const FVector Forward = FRotationMatrix(YawRot).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y);
	FVector WorldDir = Forward * InputAxis.Y + Right * InputAxis.X;
	WorldDir.Z = 0.0f;

	if (!WorldDir.IsNearlyZero())
	{
		WorldDir.Normalize();
	}

	CustomComputeInputAcceleration = WorldDir * GetInputAcceleration();
}

void UGMCMotion::ApplyFacingSpring(float TargetYaw, float DeltaSeconds)
{
	if (DeltaSeconds <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}

	// Critically-damped spring with velocity state, matching BP's CriticalSpringDampQuat.
	// The second-order spring naturally absorbs target jumps (e.g., rotation offset changes
	// at direction boundaries) because the velocity has to accelerate — unlike the first-order
	// exponential decay which chases jumps proportionally each frame.
	//
	// Math: x = displacement from target, v = dx/dt (velocity state)
	//   Exact solution over dt for ζ=1 (critically damped):
	//   x(dt) = (x₀ + (v₀ + ω·x₀)·dt) · exp(-ω·dt)
	//   v(dt) = (v₀ - ω·(v₀ + ω·x₀)·dt) · exp(-ω·dt)
	//   where ω = ln(2) / halfLife
	const float HalfLife = FMath::Max(FacingSpringHalfLife, UE_KINDA_SMALL_NUMBER);
	const float Omega = FMath::Loge(2.0f) / HalfLife;

	// Displacement from target using the spring's own position state.
	// We do NOT read OverridenDesiredFacing.Yaw here because the BP's Update_Grounded
	// overwrites it every frame (direct clamp assignment), which destroys the spring's
	// continuity. FacingSpringPositionYaw is only written by this function.
	//
	// FindDeltaAngleDegrees returns the shortest arc ([-180, 180]). For L↔R strafe
	// transitions (~180° jump), the shortest arc passes through the camera direction
	// (0°). This is CORRECT: in strafing mode the mesh stays forward-facing via the
	// offset root bone while the capsule rotates underneath. The offset root bone
	// absorbs small per-frame capsule deltas. Forcing the long arc (through ±180°/back)
	// would exceed the offset root bone's max rotation error (~85°), dragging the mesh
	// through a visible 180° spin.
	const float x0 = FMath::FindDeltaAngleDegrees(TargetYaw, FacingSpringPositionYaw);

	const float v0 = FacingSpringVelocityYaw;

	const float ExpTerm = FMath::Exp(-Omega * DeltaSeconds);
	const float VPlusOmegaX = v0 + Omega * x0;

	const float NewX = (x0 + VPlusOmegaX * DeltaSeconds) * ExpTerm;
	const float NewV = (v0 - Omega * VPlusOmegaX * DeltaSeconds) * ExpTerm;

	const float OldYaw = FacingSpringPositionYaw;
	FacingSpringPositionYaw = FRotator::NormalizeAxis(TargetYaw + NewX);
	FacingSpringVelocityYaw = NewV;

	// Write the smoothed yaw back to OverridenDesiredFacing for ApplyRotation to consume.
	OverridenDesiredFacing.Yaw = FacingSpringPositionYaw;

	// Angular velocity from actual step (for trajectory metrics / TurningStrength).
	const float StepDeg = FMath::FindDeltaAngleDegrees(OldYaw, FacingSpringPositionYaw);
	AngularVelocityRad = FMath::DegreesToRadians(StepDeg / DeltaSeconds);
}

void UGMCMotion::UpdateTrajectoryMetrics()
{
	const FVector Vel = GetLinearVelocity_GMC();
	const FVector Accel = CustomComputeInputAcceleration;
	const float MaxSpd = GetMaxSpeed();

	// Project velocity at 0.5s and 0.1s horizons, clamped to max speed.
	auto ProjectVelocity = [&](float Horizon) -> FVector
	{
		FVector Projected = Vel + Accel * Horizon;
		const float Spd = Projected.Size();
		if (Spd > MaxSpd && Spd > UE_KINDA_SMALL_NUMBER)
		{
			Projected *= MaxSpd / Spd;
		}
		return Projected;
	};

	Trj_FutureVelocity = ProjectVelocity(0.5f);
	Trj_NearFutureVelocity = ProjectVelocity(0.1f);

	// Sprint strafe reversal hold: suppress turn-related metrics so the GASP Chooser
	// sees a decelerating character with no rotation, selecting stop/plant animations
	// instead of pivot/redirect.
	if (bSprintReversalHoldActive)
	{
		Trj_TurnAngle = 0.0f;
		FutureFacingDelta = 0.0f;
		Trj_IsCircling = false;
		TurningStrength = 0.0;
		AngularVelocityRad = 0.0f;
		return;
	}

	// Forward-only mode (DirectionThresholdMode == 2): suppress turn-related trajectory
	// metrics. In first person the pawn faces the camera, so velocity is always lateral
	// during strafing — TurnAngle/FutureFacingDelta would permanently read ~±90° and
	// IsCircling would always be true, causing the animation system to select turn/redirect
	// animations that rotate the upper body away from the camera direction.
	if (DirectionThresholdMode == 2)
	{
		Trj_TurnAngle = 0.0f;
		FutureFacingDelta = 0.0f;
		Trj_IsCircling = false;
		TurningStrength = 0.0;
		return;
	}

	// Turn angle: delta between current facing yaw and projected velocity direction.
	if (!Trj_FutureVelocity.IsNearlyZero(1.0f))
	{
		const float VelYaw = Trj_FutureVelocity.Rotation().Yaw;
		const float FacingYaw = OverridenDesiredFacing.Yaw;
		Trj_TurnAngle = FMath::FindDeltaAngleDegrees(FacingYaw, VelYaw);
	}
	else
	{
		Trj_TurnAngle = 0.0f;
	}

	// FutureFacingDelta: angle between current facing and future velocity direction.
	if (!Trj_FutureVelocity.IsNearlyZero(1.0f))
	{
		FutureFacingDelta = FMath::FindDeltaAngleDegrees(
			OverridenDesiredFacing.Yaw, Trj_FutureVelocity.Rotation().Yaw);
	}
	else
	{
		FutureFacingDelta = 0.0f;
	}

	// Trj_IsCircling: true when the pawn is moving but the turn angle stays large
	// (velocity direction diverges from facing consistently).
	Trj_IsCircling = (Vel.Size2D() > 50.0f) && (FMath::Abs(Trj_TurnAngle) > 60.0f);

	// TurningStrength: absolute angular velocity in degrees/sec (from AngularVelocityRad).
	TurningStrength = FMath::Abs(FMath::RadiansToDegrees(AngularVelocityRad));
}

bool UGMCMotion::CanSprintInCurrentDirection() const
{
	// Restriction disabled: caller is asserting the animation set has strafe sprint coverage.
	if (!bRestrictSprintToForwardCone)
	{
		bSprintConeLatched = false;
		return true;
	}

	// No input: never block starting a sprint from standing.
	const FVector Input = GetProcessedInputVector();
	const FVector Input2D(Input.X, Input.Y, 0.f);
	if (Input2D.IsNearlyZero())
	{
		bSprintConeLatched = false;
		return true;
	}

	// Compare against OrientationIntent rather than actor facing. Intent is where the pawn is
	// trying to face, so the cone reacts the moment the player pushes sideways instead of waiting
	// for velocity or the capsule to catch up. In VelocityDirection mode OrientationIntent follows
	// the input, so the angle is ~0 and sprint is never restricted; the cone only bites in Aiming.
	const FVector Intent2D(OrientationIntent.X, OrientationIntent.Y, 0.f);
	if (Intent2D.IsNearlyZero())
	{
		return true;
	}

	const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(
		FMath::Clamp(FVector::DotProduct(Input2D.GetSafeNormal(), Intent2D.GetSafeNormal()), -1.f, 1.f)));

	// Wider threshold to stay in sprint than to enter it, so sitting on the boundary cannot flap
	// the gait — and with it the animation set — every frame.
	const float EnterAngle = FMath::Max(SprintForwardConeAngle, 0.f);
	const float ExitAngle = EnterAngle + FMath::Max(SprintForwardConeHysteresis, 0.f);

	bSprintConeLatched = bSprintConeLatched ? (AngleDeg <= ExitAngle) : (AngleDeg <= EnterAngle);
	return bSprintConeLatched;
}

namespace
{
	// Rotation the body should face in Aiming (strafe) mode.
	// Normally the raw control rotation (zero lag). While the player free-looks, BP_GMCMovement
	// freezes its AimingRotation on FreeLookAimRotation (WantsToFreeLook / FreeLookAimRotation are
	// GMC-bound inputs, so client, server and replays agree) and the body must stay on that
	// rotation while the camera/head roam. Read through reflection like the sim-proxy pull-ins.
	// Sim proxies have no controller: they use the replicated AimingRotation, already frozen.
	// (Free function on purpose: no header change, so Live Coding can rebuild this .cpp alone.)
	FRotator GetAimingRotationForFacing(const UGMCMotion& Movement, const FRotator& CachedAimingRotation)
	{
		const UClass* MovementClass = Movement.GetClass();
		if (const FBoolProperty* WantsFreeLook = CastField<FBoolProperty>(MovementClass->FindPropertyByName(TEXT("WantsToFreeLook"))))
		{
			if (WantsFreeLook->GetPropertyValue_InContainer(&Movement))
			{
				if (const FStructProperty* FreeLookRot = CastField<FStructProperty>(MovementClass->FindPropertyByName(TEXT("FreeLookAimRotation"))))
				{
					if (FreeLookRot->Struct == TBaseStructure<FRotator>::Get())
					{
						return *FreeLookRot->ContainerPtrToValuePtr<FRotator>(&Movement);
					}
				}
			}
		}
		if (const APawn* Pawn = Cast<APawn>(Movement.GetOwner()))
		{
			if (const AController* PC = Pawn->GetController())
			{
				return PC->GetControlRotation();
			}
		}
		return CachedAimingRotation;
	}
}

FVector UGMCMotion::GetOrientationIntent() const
{
	if (RotationMode == EGMCMotion_RotationMode::Aiming)
	{
		FVector Forward = GetAimingRotationForFacing(*this, CachedAimingRotation).Vector();
		Forward.Z = 0.0f;
		if (!Forward.IsNearlyZero())
		{
			Forward.Normalize();
			return Forward;
		}
		return FVector::ForwardVector;
	}

	const FVector Vel = GetLinearVelocity_GMC();
	if (Vel.Size2D() > 1.0f)
	{
		FVector Dir = Vel;
		Dir.Z = 0.0f;
		Dir.Normalize();
		return Dir;
	}

	return OverridenDesiredFacing.Vector();
}

FRotator UGMCMotion::GetTargetOrientation() const
{
	if (RotationMode == EGMCMotion_RotationMode::Aiming)
	{
		return GetAimingRotationForFacing(*this, CachedAimingRotation);
	}

	const FVector Vel = GetLinearVelocity_GMC();
	if (Vel.Size2D() > 1.0f)
	{
		return Vel.Rotation();
	}

	return OverridenDesiredFacing;
}

// =====================================================================================
//  Stance collision
// =====================================================================================

bool UGMCMotion::HasRoomForVerticalCapsule(float TargetHalfHeight) const
{
	if (!UpdatedComponent)
	{
		return false;
	}

	const float Radius = GetRootCollisionExtent(false).X;
	const float ScaleZ = FMath::Abs(static_cast<float>(UpdatedComponent->GetComponentScale().Z));
	// Keep the bottom of the collision where it is (GetRootCollisionHalfHeight is the Z extent for every shape).
	const FVector TestLocation = UpdatedComponent->GetComponentLocation()
		+ FVector(0., 0., (TargetHalfHeight - GetRootCollisionHalfHeight(false)) * ScaleZ);
	// 0.95: the test shape must not touch the floor the pawn stands on.
	return IsValidPosition(EGMC_CollisionShape::VerticalCapsule, FVector(Radius, Radius, TargetHalfHeight),
		TestLocation, UpdatedComponent->GetComponentQuat(), ECC_Pawn, 0.95f);
}

bool UGMCMotion::ResetStanceCollision(float StandHalfHeight)
{
	// The slope slide isn't Grounded, so the BP calls this when it starts: keep the slide's crouch-height capsule
	// (driven by UpdateStanceCollision) instead of popping to the standing height against the slope.
	if (!UpdatedComponent || IsSlopeSliding())
	{
		return false;
	}

	const float Radius = GetRootCollisionExtent(false).X;
	if (GetRootCollisionShape() != EGMC_CollisionShape::VerticalCapsule)
	{
		SetRootCollisionShape(EGMC_CollisionShape::VerticalCapsule, FVector(Radius, Radius, StandHalfHeight), false, NAME_None);
		return true;
	}
	if (!FMath::IsNearlyEqual(GetRootCollisionHalfHeight(false), StandHalfHeight, 0.01f))
	{
		SetRootCollisionHalfHeight(StandHalfHeight, false);
		return true;
	}
	return false;
}

uint8 UGMCMotion::UpdateStanceCollision(uint8 DesiredStance, float StandHalfHeight, float CrouchHalfHeight, float InterpSpeed,
	float DeltaTime, bool bAdjustPosition, bool& bCollisionChanged)
{
	bCollisionChanged = false;
	ON_SCOPE_EXIT { bStanceCollisionChanging = bCollisionChanged; };
	if (!UpdatedComponent)
	{
		return 0;
	}

	constexpr uint8 StanceStand = 0;
	constexpr uint8 StanceCrouch = 1;
	// Shrinking never collides, so the lerps use a lenient test (same value the crouch BP used).
	constexpr float ShrinkTolerance = 0.05f;
	constexpr float SizeTolerance = 0.5f;

	const auto Track = [&bCollisionChanged](float Delta)
	{
		if (Delta > 0.f)
		{
			bCollisionChanged = true;
		}
	};

	// --- Slope slide: crouch-height capsule --------------------------------------------------
	// The BP asks for Stand while not Grounded. The position is always adjusted (the BP only adjusts when Grounded) so the
	// capsule bottom stays on the slope. Stand is returned (the ABP plays its Slide state); leaving the slide, the
	// stand / crouch lerp below grows back only if there is room.
	const float CurrentHalfHeight = GetRootCollisionHalfHeight(false);
	if (IsSlopeSliding())
	{
		if (!FMath::IsNearlyEqual(CurrentHalfHeight, CrouchHalfHeight, 0.01f))
		{
			const bool bGrow = CrouchHalfHeight > CurrentHalfHeight;
			Track(LerpRootCollisionHalfHeight(CrouchHalfHeight, InterpSpeed, ShrinkTolerance, DeltaTime, true,
				bGrow ? EGMC_AdjustDirection::Up : EGMC_AdjustDirection::Down));
		}
		return StanceStand;
	}

	// --- Stand <-> crouch ---------------------------------------------------------------------
	uint8 Result = DesiredStance == StanceStand ? StanceStand : StanceCrouch;
	float TargetHalfHeight = Result == StanceStand ? StandHalfHeight : CrouchHalfHeight;
	if (Result == StanceStand && CurrentHalfHeight < StandHalfHeight - SizeTolerance && !HasRoomForVerticalCapsule(StandHalfHeight))
	{
		Result = StanceCrouch;
		TargetHalfHeight = CrouchHalfHeight;
	}

	if (!FMath::IsNearlyEqual(CurrentHalfHeight, TargetHalfHeight, 0.01f))
	{
		const bool bGrow = TargetHalfHeight > CurrentHalfHeight;
		Track(LerpRootCollisionHalfHeight(TargetHalfHeight, InterpSpeed, ShrinkTolerance, DeltaTime, bAdjustPosition,
			bGrow ? EGMC_AdjustDirection::Up : EGMC_AdjustDirection::Down));
	}
	return Result;
}

float UGMCMotion::GetOverMaxSpeedDeceleration() const
{
	if (bTraversalExitCarry && !IsMovingOnGround())
	{
		// Traversal exit in the air (vault over a drop): the anim's speed is kept until landing, like GASP58's falling mode.
		return 0.f;
	}
	if (bTraversalExitCarry && IsMovingOnGround())
	{
		// LimitSpeed runs after ApplyInputVelocity added the input acceleration this move: include it, so the NET
		// slow-down is TraversalExitOverSpeedDeceleration (returning the bare value let a held input keep accelerating
		// the pawn above its max speed: walking chained traversals ended up at run speed).
		return TraversalExitOverSpeedDeceleration + GetInputAcceleration() * FMath::Min(GetGroundFriction(), 1.f);
	}
	if (bGroundOverMaxSpeedUsesBraking && IsMovingOnGround() && GetMovementMode() == EGMC_MovementMode::Grounded)
	{
		// GASP58 parity (see bGroundOverMaxSpeedUsesBraking). With an input, LimitSpeed runs after ApplyInputVelocity added
		// the input acceleration: include it so the NET slow-down is the braking deceleration. Without one,
		// ApplyDeceleration already brakes: nothing more here.
		return GetProcessedInputVector().IsNearlyZero()
			? 0.f
			: GetBrakingDeceleration() + GetInputAcceleration() * FMath::Min(GetGroundFriction(), 1.f);
	}
	return Super::GetOverMaxSpeedDeceleration();
}

bool UGMCMotion::ShouldSmoothGroundVelocity() const
{
	return bSmoothGroundVelocity && IsMovingOnGround() && GetMovementMode() == EGMC_MovementMode::Grounded && !HasRootMotion()
		&& !bTraversalWarpActive
		&& !(IsServerBot() && !UseAccelerationForPathFollowing());
}

void UGMCMotion::CalculateVelocity(float DeltaSeconds)
{
	// Ground velocity smoothing (see bSmoothGroundVelocity): mirror of Mover's USmoothWalkingMode::GenerateWalkMove.
	const bool bSmooth = ShouldSmoothGroundVelocity() && DeltaSeconds > UE_KINDA_SMALL_NUMBER;
	const FVector2D ActualVelocity(Velocity.X, Velocity.Y);
	if (bSmooth)
	{
		if (!bSmoothStateValid)
		{
			SmoothIntermediateVelocity = ActualVelocity;
			SmoothSpringVelocity = ActualVelocity;
			SmoothSpringAcceleration = FVector2D::ZeroVector;
		}
		// The more the real velocity differs from what the spring gave last move (collisions), the faster the
		// intermediate velocity is pulled back to it.
		const double Match = FMath::Clamp((SmoothSpringVelocity | ActualVelocity)
			/ FMath::Max(ActualVelocity.Size() * SmoothSpringVelocity.Size(), UE_SMALL_NUMBER), 0., 1.);
		FMath::ExponentialSmoothingApprox(SmoothIntermediateVelocity, ActualVelocity, DeltaSeconds,
			static_cast<float>((GroundOutsideInfluenceSmoothingTime + UE_KINDA_SMALL_NUMBER) / FMath::Max(1. - Match, UE_KINDA_SMALL_NUMBER)));
		// Turn the intermediate velocity toward the input direction, keeping its length (Mover's TurningStrength).
		const FVector TurnInput = GetProcessedInputVector();
		const FVector2D TurnDirection = FVector2D(TurnInput.X, TurnInput.Y).GetSafeNormal();
		if (GroundTurningStrength > 0.f && !TurnDirection.IsNearlyZero())
		{
			FMath::ExponentialSmoothingApprox(SmoothIntermediateVelocity, TurnDirection * SmoothIntermediateVelocity.Size(),
				DeltaSeconds, SpringMath::StrengthToSmoothingTime(GroundTurningStrength));
		}
		// GMC's acceleration / braking / speed limits act on the intermediate velocity.
		Velocity.X = SmoothIntermediateVelocity.X;
		Velocity.Y = SmoothIntermediateVelocity.Y;
	}

	Super::CalculateVelocity(DeltaSeconds);

	if (bSmooth)
	{
		const FVector2D Intermediate(Velocity.X, Velocity.Y);
		const FVector Input = GetProcessedInputVector().GetClampedToMaxSize(1.0);
		const FVector2D Desired = FVector2D(Input.X, Input.Y) * GetMaxSpeed();
		const bool bAccelerating = 1.01 * Desired.SizeSquared() > ActualVelocity.SizeSquared();
		FVector2D Spring = ActualVelocity;
		SpringMath::CriticalSpringDamper(Spring, SmoothSpringAcceleration, Intermediate,
			bAccelerating ? GroundAccelerationSmoothingTime : GroundDecelerationSmoothingTime, DeltaSeconds);
		// Dead zones (Mover's): settle on the target instead of creeping.
		if ((Intermediate - Spring).SizeSquared() < FMath::Square(0.01))
		{
			Spring = Intermediate;
			if (SmoothSpringAcceleration.SizeSquared() < FMath::Square(0.001))
			{
				SmoothSpringAcceleration = FVector2D::ZeroVector;
			}
		}
		SmoothIntermediateVelocity = Intermediate;
		SmoothSpringVelocity = Spring;
		bSmoothStateValid = true;
		Velocity.X = Spring.X;
		Velocity.Y = Spring.Y;
	}
	else
	{
		bSmoothStateValid = false;
	}

	// Post-jump-over carry ends once back at max speed, when the input stops or turns away, or when the next traversal
	// takes over (its root motion owns the velocity). Not on "airborne": RestoreTraversalExitVelocity can run while the
	// traversal mode is still set; the carry only changes the grounded over-speed deceleration anyway.
	if (bTraversalExitCarry)
	{
		const FVector Input = GetProcessedInputVector();
		const FVector Vel2D(Velocity.X, Velocity.Y, 0.);
		const bool bInputAlong = !Input.IsNearlyZero() && (Vel2D.GetSafeNormal() | FVector(Input.X, Input.Y, 0.).GetSafeNormal()) > 0.7;
		if (bTraversalWarpActive || !bInputAlong || Vel2D.Size() <= GetMaxSpeed() + 1.0)
		{
			bTraversalExitCarry = false;
		}
	}
}

void UGMCMotion::UpdateRotationMode_CPP()
{
	RotationMode = WantsToStrafe
		? EGMCMotion_RotationMode::Aiming
		: EGMCMotion_RotationMode::VelocityDirection;
}

void UGMCMotion::CalculateRotations_CPP()
{
	// --- 1. Compute CachedAimingRotation from controller rotation ---
	// The BP skips this for simulated proxies (they use the replicated AimingRotation).
	// In GMC, GetControllerRotation_GMC() returns the replicated control rotation for all
	// roles, so this is safe for sim proxies too — but we match the BP's branch to keep
	// the same replication behaviour (sim proxies get AimingRotation from the bind channel).
	if (!IsSimulatedProxy())
	{
		const FRotator ControllerRot = GetControllerRotation_GMC();
		CachedAimingRotation = FRotator(
			FRotator::ClampAxis(ControllerRot.Pitch),
			FRotator::ClampAxis(ControllerRot.Yaw),
			FRotator::ClampAxis(ControllerRot.Roll)
		);
	}

	// --- 2. Only compute OrientationIntent when Grounded ---
	// The BP's GetOrientationIntent switches on MovementMode; only the Grounded pin is
	// connected. Non-grounded modes: OrientationIntent stays unchanged (preserves last value).
	if (GetMovementMode() != EGMC_MovementMode::Grounded)
	{
		return;
	}

	// Flat forward vector from CachedAimingRotation yaw only (pitch/roll zeroed).
	// Matches BP: GetForwardVector(MakeRotator(0, 0, AimingRotation.Yaw))
	const FVector AimForward = FRotator(0.0f, CachedAimingRotation.Yaw, 0.0f).Vector();

	// --- 3. Branch on movement input ---
	const bool bHasInput = !CustomComputeInputAcceleration.IsNearlyZero();

	if (bHasInput)
	{
		if (RotationMode == EGMCMotion_RotationMode::VelocityDirection)
		{
			// Has input + VelocityDirection: face the direction of movement input.
			OrientationIntent = CustomComputeInputAcceleration;
		}
		else // Aiming (BP also handles OrientToMovement identically here)
		{
			// Has input + Aiming: face the camera/aim direction.
			OrientationIntent = AimForward;
		}
	}
	else
	{
		if (RotationMode == EGMCMotion_RotationMode::Aiming)
		{
			// No input + Aiming: turn-in-place check.
			// When the character is rotated 60+ degrees away from the aiming direction,
			// snap the intent to the aim direction (triggers turn-in-place animation).
			// Matches BP: Delta(Rotator)(GetActorRotationGMC, AimingRotation) → Abs(Yaw) > 60
			const float ActorYaw = GetActorRotation_GMC().Yaw;
			const float DeltaYaw = FRotator::NormalizeAxis(ActorYaw - CachedAimingRotation.Yaw);
			if (FMath::Abs(DeltaYaw) > 60.0f)
			{
				OrientationIntent = AimForward;
			}
			// else: preserve last frame's OrientationIntent (no turn-in-place needed)
		}
		// VelocityDirection without input: preserve last OrientationIntent (don't change it).
	}
}

void UGMCMotion::UpdateGroundedFacing(float DeltaSeconds)
{
	// Traversal guard: during traversal, the warp drives rotation directly.
	// Snap OverridenDesiredFacing to the current actor rotation so ApplyRotation
	// doesn't fight the warp, and is ready to resume when the traversal ends.
	if (bTraversalWarpActive && UpdatedComponent)
	{
		OverridenDesiredFacing.Yaw = UpdatedComponent->GetComponentRotation().Yaw;
		FacingSpringPositionYaw = OverridenDesiredFacing.Yaw;
		FacingSpringVelocityYaw = 0.f;
		bFacingSpringSettling = false;
		return;
	}

	// 1. Compute the orientation intent and cache it for the trajectory predictor
	// (which reads OrientationIntent via FindPropertyByName).
	OrientationIntent = GetOrientationIntent();
	const float IntentYaw = OrientationIntent.Rotation().Yaw;

	// 2. Compute the raw target yaw early so the reversal hold can snap to it on release.
	// When bDisableFacingRotationOffset is true, the capsule faces pure aim (IntentYaw)
	// without the movement-direction shift. Required for weapon layers (SKG) that replace
	// the OW-corrected spine via Layered Blend Per Bone.
	const float EffectiveOffset = bDisableFacingRotationOffset ? 0.0 : RotationOffset;
	const float RawTargetYaw = FRotator::NormalizeAxis(IntentYaw + EffectiveOffset);

	// 3. Sprint strafe reversal hold.
	// When sprinting in aiming mode and the player reverses strafe direction, the
	// rotation offset jumps ~180° (e.g., -90 → +90). If the spring immediately chases
	// this new target, the capsule rotates 180° over ~0.3s, producing high angular
	// velocity (~340°/s). This causes the GASP Chooser to select pivot/redirect
	// animations instead of stop/plant transitions.
	//
	// Fix: freeze the facing spring until velocity drops, then release. The character
	// decelerates in place (stop/plant), then the spring resumes taking the shortest arc
	// (through 0°/camera in strafing). The offset root bone keeps the mesh forward-facing
	// while the capsule rotates underneath with small per-frame deltas.
	// Skip when RotationRate is 0 (first person) — the capsule tracks the camera directly
	// each frame, so the hold just builds up a delta that snaps on release.
	if (CurrentGait == EGMCMotion_Gait::Sprint
		&& RotationMode == EGMCMotion_RotationMode::Aiming
		&& RotationRate > 0.f)
	{
		const FVector InputVec = GetProcessedInputVector();
		if (!InputVec.IsNearlyZero())
		{
			const FVector Vel = GetLinearVelocity_GMC();
			const FVector Accel = InputVec * GetInputAcceleration();
			if (!Accel.IsNearlyZero() && Vel.Size2D() >= 10.f)
			{
				const FVector Vel2D(Vel.X, Vel.Y, 0.f);
				const FVector Accel2D(Accel.X, Accel.Y, 0.f);
				const float Dot = FVector::DotProduct(Vel2D.GetSafeNormal(), Accel2D.GetSafeNormal());
				if (Dot < -0.5f && !bSprintReversalHoldActive)
				{
					bSprintReversalHoldActive = true;
				}
			}
		}
	}

	if (bSprintReversalHoldActive)
	{
		const float Speed2D = GetLinearVelocity_GMC().Size2D();
		if (Speed2D < 50.f)
		{
			// Velocity has dropped enough — release the hold.
			//
			// Key insight: in strafing mode, the mesh faces the camera direction (IntentYaw)
			// via the offset root bone, while the capsule is offset by RotationOffset.
			// During a L→R reversal, the capsule was at (IntentYaw - 90°) and needs to
			// reach (IntentYaw + 90°) — a 180° jump. If we let the spring traverse all
			// 180°, the offset root bone may fail to fully compensate (max error ~85°),
			// causing the mesh to visibly rotate.
			//
			// Fix: snap the capsule to IntentYaw (camera direction) where the mesh already
			// is. The capsule and mesh are now aligned, so no visible change occurs. The
			// spring then only needs to travel ~90° to reach the new target, well within
			// the offset root bone's compensation range.
			bSprintReversalHoldActive = false;
			bFacingSpringSettling = true;

			FacingSpringPositionYaw = IntentYaw;
			FacingSpringVelocityYaw = 0.f;
			OverridenDesiredFacing.Yaw = IntentYaw;
			// Fall through — spring settling handles the ~90° transition below.
		}
		else
		{
			// Hold: keep the spring at the current position, zero out velocity state.
			// This prevents capsule rotation and keeps AngularVelocityRad near zero,
			// so the Chooser sees a decelerating character with no turning.
			FacingSpringVelocityYaw = 0.f;
			AngularVelocityRad = 0.f;
			return;
		}
	}

	// 4. Rotation output: spring settling or direct tracking.
	//
	// The facing spring (half-life 0.1s) produces smooth rotation for direction transitions
	// but introduces steady-state tracking lag proportional to mouse speed (~72° at 500°/s).
	// This causes the player to see their own body during fast camera rotation.
	//
	// Fix: only use the spring during the post-reversal-hold settling period (~90° over ~0.3s).
	// For all other frames, set OverridenDesiredFacing directly — zero lag camera tracking.
	if (bFacingSpringSettling)
	{
		const float Disp = FMath::Abs(FMath::FindDeltaAngleDegrees(RawTargetYaw, FacingSpringPositionYaw));
		if (Disp < 3.f)
		{
			// Spring has converged — switch to direct tracking.
			bFacingSpringSettling = false;
		}
		else
		{
			ApplyFacingSpring(RawTargetYaw, DeltaSeconds);
			return;
		}
	}

	// Direct tracking: zero-lag camera following. OverridenDesiredFacing matches the
	// target exactly, so the capsule snaps to the correct orientation every frame.
	// Angular velocity is computed from the per-frame change for AnimBP consumption.
	const float PrevYaw = OverridenDesiredFacing.Yaw;
	OverridenDesiredFacing.Yaw = RawTargetYaw;
	FacingSpringPositionYaw = RawTargetYaw;
	FacingSpringVelocityYaw = 0.f;

	if (DeltaSeconds > UE_KINDA_SMALL_NUMBER)
	{
		const float StepDeg = FMath::FindDeltaAngleDegrees(PrevYaw, RawTargetYaw);
		AngularVelocityRad = FMath::DegreesToRadians(StepDeg / DeltaSeconds);
	}
}
