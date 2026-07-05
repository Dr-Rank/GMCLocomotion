// GMCMotion - GMC traversal movement component.

#include "Components/GMCMotion.h"
#include "GMCMotionLog.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
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

void UGMCMotion::PhysicsCustom_Implementation(float DeltaSeconds)
{
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

	if (bUseTraversalCustomMode && bTraversalWarpActive)
	{
		// Hold the dedicated traversal mode for the whole montage. Returning true makes GMC skip
		// UpdateMovementModeStatic, so its Airborne<->Grounded auto-switching (and the ProcessLanded it
		// triggers when brushing a walkable floor) never runs: no landing/fall anim, no capsule-resize
		// flip-flop. BP sets bTraversalWarpActive true at start (PrePhysics) and false at end; the
		// Custom1->Grounded transition on exit does NOT fire a landing (ProcessLanded only runs from the
		// Static path on IsAirborne()).
		if (GetMovementMode() != TraversalMovementMode)
		{
			SetMovementMode(TraversalMovementMode);
		}
		return true;
	}

	// Mode A (Airborne) JUMP-OVER: keep the pawn AIRBORNE while it is above the far-floor level (i.e. while
	// it is over the obstacle), so NEITHER grounding path snaps it onto the obstacle TOP mid-vault (the
	// "capsule monte / bump Z+"). The IsValidLandingSpot override only guards the MoveThroughAir landing
	// path; the proximity grounding in UpdateMovementModeStatic does NOT call it, so it would ground the
	// pawn onto the walkable obstacle top anyway. Forcing Airborne here (return true) skips Static entirely.
	// Once the pawn has descended near the far floor (center within the threshold of BackFloor+HalfHeight)
	// we fall through to normal grounding so it lands properly on the far side. Reach montages (not
	// jump-over) are untouched — landing on the ledge is their goal. Deterministic (replicated BackFloor).
	if (bTraversalWarpActive && bLastTraversalIsJumpOver && !WarpBackFloor.IsNearlyZero())
	{
		float HalfHeight = 0.0f;
		if (const UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(UpdatedComponent))
		{
			HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
		}

		const double GroundLandCenterZ = WarpBackFloor.Z + HalfHeight;
		if (UpdatedComponent->GetComponentLocation().Z > GroundLandCenterZ + TraversalNoLandHeightAboveFloor)
		{
			if (!IsAirborne())
			{
				SetMovementMode(EGMC_MovementMode::Airborne);
			}
			return true;
		}
	}

	return Super::UpdateMovementModeDynamic_Implementation(Floor, DeltaSeconds);
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

	// Grace: keep the widened tolerances for a bit after the traversal ends so in-flight (RTT-late) client
	// validations of the last traversal moves still use the wide tolerance.
	if (bTraversalWarpActive)
	{
		TraversalToleranceGraceRemaining = TraversalToleranceGraceTime;
	}
	else if (TraversalToleranceGraceRemaining > 0.0f)
	{
		TraversalToleranceGraceRemaining = FMath::Max(0.0f, TraversalToleranceGraceRemaining - DeltaSeconds);
	}

	const bool bWiden = bTraversalWarpActive || TraversalToleranceGraceRemaining > 0.0f;

	if (bWiden && !bTraversalToleranceWidened)
	{
		Tol.ActorLocation = TraversalActorLocationTolerance;
		Tol.ActorRotation = TraversalActorRotationTolerance;
		Tol.LinearVelocity = TraversalLinearVelocityTolerance;
		Tol.AngularVelocity = TraversalAngularVelocityTolerance;
		bTraversalToleranceWidened = true;
		if (bDebugTraversalWarp)
		{
			UE_LOG(LogGMCMotion, Warning, TEXT("[TTol] WIDEN loc=%.0f rot=%.0f lvel=%.0f avel=%.0f"),
				TraversalActorLocationTolerance, TraversalActorRotationTolerance,
				TraversalLinearVelocityTolerance, TraversalAngularVelocityTolerance);
		}
	}
	else if (!bWiden && bTraversalToleranceWidened)
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

	// Capture the animation's own horizontal velocity (the raw, un-warped root motion this frame, already
	// world-space after Super) so RestoreTraversalExitVelocity can hand it to locomotion at landing.
	{
		const FVector RawT = InOutRootMotionParams.GetRootMotionTransform().GetTranslation();
		TraversalExitVelocity = FVector(RawT.X, RawT.Y, 0.0) / FMath::Max(DeltaSeconds, (float)KINDA_SMALL_NUMBER);
	}

	const float Position = MontageInstance.GetPosition();

	FVector TargetLoc;
	float WindowEnd;
	FName WarpPointBone;
	FName WarpTargetName;
	if (!GetActiveWarpTarget(Montage, Position, TargetLoc, WindowEnd, WarpPointBone, WarpTargetName))
	{
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
						TEXT("[TWarp] %s TAIL-KILL pos=%.3f lastEnd=%.3f warpActive=%d | killing rootMotion=(%.1f,%.1f,%.1f) curZ=%.1f"),
						*GetNameSafe(Montage), Position, LastEnd, bTraversalWarpActive ? 1 : 0, T.X, T.Y, T.Z, Cur.Z);
				}
				Tail.SetTranslation(FVector::ZeroVector);
				InOutRootMotionParams.Set(Tail);
			}
			else if (bDebugTraversalWarp)
			{
				const FVector Cur = UpdatedComponent->GetComponentLocation();
				UE_LOG(LogGMCMotion, Warning,
					TEXT("[TWarp] %s TAIL-KEEP (still on obstacle) pos=%.3f curZ=%.1f backFloorZ=%.1f"),
					*GetNameSafe(Montage), Position, Cur.Z, WarpBackFloor.Z);
			}
			return;
		}

		// Inter-window gap of a JUMP-OVER: cap the apex.
		if (bUseTraversalCustomMode && bIsJumpOver && Position < LastEnd)
		{
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
		return;
	}

	if (WindowEnd <= Position)
	{
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

	const double TargetBoneOffsetZ = (bUseTraversalCustomMode && bHurdle) ? 0.0 : RootMinusBone.Z;
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
			TargetLoc += FaceNormal * Radius;
		}
	}

	if (bDebugTraversalWarp)
	{
		const FVector Cur = UpdatedComponent->GetComponentLocation();
		const double DXY = FVector::Dist2D(TargetLoc, Cur);
		UE_LOG(LogGMCMotion, Warning,
			TEXT("[TWarp] %s tgt=%s bone=%s pos=%.3f end=%.3f | corrTgtZ=%.1f curZ=%.1f dZ=%.1f | curXY=(%.0f,%.0f) dXY=%.1f"),
			*GetNameSafe(Montage), *WarpTargetName.ToString(), *WarpPointBone.ToString(),
			Position, WindowEnd,
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

	// When the bone offset drags corrTgtZ below the character (walk-speed climbs), the
	// warp Z stalls at 0 and the capsule stays at ground level for the entire FrontLedge
	// window. Fix: lerp the target between the bone-offset position and standing-on-obstacle
	// using ONLY invariant values (no CurrentLoc dependency). As the character rises,
	// ToTarget.Z naturally decreases, giving smooth deceleration matching run montages.
	// REACH-CAP still caps post-window overshoot.
	if (bReachFrontLedge && TargetLoc.Z < CurrentLoc.Z)
	{
		const double StandingZ = RawTargetLoc.Z + HalfHeight;
		TargetLoc.Z = FMath::Lerp(TargetLoc.Z, StandingZ, 0.35);
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

	auto WarpAxis = [&](double ToTargetAxis, double RemainingAnimAxis, double FrameAnimAxis) -> double
		{
			if (FMath::Abs(RemainingAnimAxis) > 1.0)
			{
				return FrameAnimAxis * FMath::Clamp(ToTargetAxis / RemainingAnimAxis, 0.0, (double)MaxWarpScale);
			}
			return ToTargetAxis * Fraction;
		};

	const bool bFollowAnimPath = bArcPreserveJumpArc;

	FVector Warped;
	if (bFollowAnimPath)
	{
		Warped.X = WarpAxis(ToTarget.X, RemainingWorld.X, FrameDelta.X);
		Warped.Y = WarpAxis(ToTarget.Y, RemainingWorld.Y, FrameDelta.Y);
		Warped.Z = WarpAxis(ToTarget.Z, RemainingWorld.Z, FrameDelta.Z);

		// Z-snap for reach FrontLedge (climb / mantle): distribute the vertical gap
		// evenly over the remaining window time instead of using the animation-ratio warp.
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
	RootMotionTransform.SetTranslation(Warped);

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
			TEXT("[TWarp]   %s frameAnim=(%.1f,%.1f,%.1f) warped=(%.1f,%.1f,%.1f) | curX=%.0f tgtX=%.0f dX=%.1f animPath=%d"),
			*WarpTargetName.ToString(),
			FrameDelta.X, FrameDelta.Y, FrameDelta.Z, Warped.X, Warped.Y, Warped.Z,
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
		UE_LOG(LogGMCMotion, Warning, TEXT("[TExit] STOP (vel->0) | jumpOver=%d capturedExitVel=(%.0f,%.0f) speed2D=%.0f"),
			bLastTraversalIsJumpOver ? 1 : 0, TraversalExitVelocity.X, TraversalExitVelocity.Y, TraversalExitVelocity.Size2D());
	}
	UpdateVelocity(FVector::ZeroVector);
}

void UGMCMotion::RestoreTraversalExitVelocity()
{
	if (bDebugTraversalWarp)
	{
		UE_LOG(LogGMCMotion, Warning, TEXT("[TExit] RESTORE (inherit anim vel) | jumpOver=%d injectVel=(%.0f,%.0f) speed2D=%.0f"),
			bLastTraversalIsJumpOver ? 1 : 0, TraversalExitVelocity.X, TraversalExitVelocity.Y, TraversalExitVelocity.Size2D());
	}
	UpdateVelocity(FVector(TraversalExitVelocity.X, TraversalExitVelocity.Y, 0.0));
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

	const bool bWantIgnore = bDisableObstacleCollision && bTraversalWarpActive && bLastTraversalIsJumpOver;

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
	// Let the BP pipeline run first (Jump, UpdateGait, SetGait, Update_Grounded_Physic, traversal).
	Super::PrePhysicsUpdate_Implementation(DeltaSeconds);

	// Initialize OverridenDesiredFacing from the actor's current rotation on the first tick,
	// so the facing spring starts at the actual pawn facing instead of world-forward (0,0,0).
	if (!bFacingInitialized && UpdatedComponent)
	{
		OverridenDesiredFacing = UpdatedComponent->GetComponentRotation();
		bFacingInitialized = true;
	}

	// One-time validation: warn if rotation offset curves are not assigned.
	if (bEnableGASPPipeline && !bRotationOffsetCurvesValidated)
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
	// Only functions that tested well in C++ are called here. CalculateRotations_CPP
	// and MovementDirection remain BP-driven (BP calls CalculateRotations_CPP as a UFUNCTION;
	// BP's GetMovementDirectionAndOffset handles direction + offset).
	if (bEnableGASPPipeline)
	{
		if (bCPP_InputAcceleration) UpdateInputAcceleration();
		UpdateRotationMode_CPP();
		// CalculateRotations_CPP: called from BP's PrePhysicsUpdate chain (not here)
		// MovementDirection: driven by BP's GetMovementDirectionAndOffset (not here)
		if (bCPP_GroundedFacing) UpdateGroundedFacing(DeltaSeconds);
		if (bCPP_TrajectoryMetrics) UpdateTrajectoryMetrics();
	}
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
}

void UGMCMotion::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

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
	if (IsSimulatedProxy())
	{
		const UClass* MyClass = GetClass();

		// MovementDirection (BP variable is a byte/enum)
		if (const FProperty* Prop = MyClass->FindPropertyByName(TEXT("MovementDirection")))
		{
			if (const FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
			{
				MovementDirection = *ByteProp->ContainerPtrToValuePtr<uint8>(this);
			}
			else if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
			{
				const FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
				MovementDirection = static_cast<uint8>(UnderlyingProp->GetSignedIntPropertyValue(EnumProp->ContainerPtrToValuePtr<void>(this)));
			}
		}

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

	// Rate-limited rotation toward OverridenDesiredFacing.
	// The BP's Update_Grounded sets OverridenDesiredFacing directly (IntentYaw + RotationOffset)
	// and the C++ ApplyRotation smooths via RotateYawTowardsDirection. The rotation offset curves
	// handle visual continuity at direction boundaries — no spring needed.
	// ApplyFacingSpring (CriticalSpringDampQuat) exists in the BP but is dead code.
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
	if (bEnableGASPPipeline && UpdatedComponent && DeltaSeconds > UE_KINDA_SMALL_NUMBER)
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
	// When bEnableGASPPipeline is false, skip ALL GASP bindings — the reference BP's
	// ReplicationGraph binds these same variables (and its own _0 variants). Double-binding
	// the same UPROPERTY crashes or silently corrupts the replication state.
	if (bEnableGASPPipeline)
	{
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
	} // bEnableGASPPipeline
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

	// No input + moving = predict stop using kinematic equation
	const FVector Vel = GetLinearVelocity_GMC();
	const float Speed2D = Vel.Size2D();

	if (Speed2D < 10.f || !GetProcessedInputVector().IsNearlyZero())
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
	if (!bPrecalculateDistanceMatches)
	{
		OutPivotLocation = FVector::ZeroVector;
		return false;
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

	// Displacement from target (wrapped to [-180, 180])
	const float x0 = FMath::FindDeltaAngleDegrees(TargetYaw, OverridenDesiredFacing.Yaw);
	const float v0 = FacingSpringVelocityYaw;

	const float ExpTerm = FMath::Exp(-Omega * DeltaSeconds);
	const float VPlusOmegaX = v0 + Omega * x0;

	const float NewX = (x0 + VPlusOmegaX * DeltaSeconds) * ExpTerm;
	const float NewV = (v0 - Omega * VPlusOmegaX * DeltaSeconds) * ExpTerm;

	const float OldYaw = OverridenDesiredFacing.Yaw;
	OverridenDesiredFacing.Yaw = FRotator::NormalizeAxis(TargetYaw + NewX);
	FacingSpringVelocityYaw = NewV;

	// Angular velocity from actual step (for trajectory metrics / TurningStrength).
	const float StepDeg = FMath::FindDeltaAngleDegrees(OldYaw, OverridenDesiredFacing.Yaw);
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

// UpdateMovementDirection: threshold-based direction quantization + rotation offset curve evaluation.
void UGMCMotion::UpdateMovementDirection()
{
	const uint8 PrevDirection = MovementDirection;
	const double PrevOffset = RotationOffset;

	// VelocityDirection mode: early-return Forward + 0 offset (orient-to-movement).
	if (RotationMode == EGMCMotion_RotationMode::VelocityDirection)
	{
		MovementDirection = static_cast<uint8>(EGMCMotion_MovementDirection::F);
		RotationOffset = 0.0;
		return;
	}

	// Direction source: use input acceleration when grounded, velocity when airborne.
	// Matches the BP's GetMovementDirectionAndOffset which reads CustomComputeInputAcceleration
	// on ground and velocity in air.
	const bool bGrounded = GetMovementMode() == EGMC_MovementMode::Grounded;
	const FVector DirectionVector = bGrounded ? CustomComputeInputAcceleration : GetLinearVelocity_GMC();

	if (DirectionVector.Size2D() < 1.0f)
	{
		return; // Keep current direction when nearly stationary.
	}

	// Locomotion angle: direction relative to the orientation intent (camera forward in Aiming).
	const float DirYaw = DirectionVector.Rotation().Yaw;
	const float FacingYaw = GetOrientationIntent().Rotation().Yaw;
	const float LocomotionAngle = FMath::FindDeltaAngleDegrees(FacingYaw, DirYaw);

	// Sprint override: force Forward when gait is Sprint (matches BP's Gait == Sprint check).
	if (CurrentGait == EGMCMotion_Gait::Sprint)
	{
		MovementDirection = static_cast<uint8>(EGMCMotion_MovementDirection::F);
	}
	else
	{
		MovementDirection = static_cast<uint8>(GetDirectionFromAngle(LocomotionAngle));
	}

	// Evaluate rotation offset curve for the computed direction.
	UCurveFloat* Curve = GetRotationOffsetCurveForDirection(static_cast<EGMCMotion_MovementDirection>(MovementDirection));
	RotationOffset = Curve ? Curve->GetFloatValue(LocomotionAngle) : 0.0f;

	// --- Diagnostic: log direction transitions and offset jumps ---
	if (MovementDirection != PrevDirection)
	{
		UE_LOG(LogGMCMotion, Warning,
			TEXT("[GASP Dir] CHANGE %d -> %d | locoAngle=%.1f | offset %.1f -> %.1f (delta=%.1f) | curve=%s | speed=%.0f sliding=%d sprint=%d"),
			(int32)PrevDirection, (int32)MovementDirection,
			LocomotionAngle,
			PrevOffset, RotationOffset, RotationOffset - PrevOffset,
			Curve ? *Curve->GetName() : TEXT("(null)"),
			GetLinearVelocity_GMC().Size2D(),
			bIsSliding ? 1 : 0,
			(CurrentGait == EGMCMotion_Gait::Sprint) ? 1 : 0);
	}
}

EGMCMotion_MovementDirection UGMCMotion::GetDirectionFromAngle(float Angle) const
{
	// Mode 2: forward-only
	if (DirectionThresholdMode == 2)
	{
		return EGMCMotion_MovementDirection::F;
	}

	// Mode 1: 2-way (F/B only) with hysteresis
	if (DirectionThresholdMode == 1)
	{
		// Hysteresis: when currently in B, narrower forward zone (±120);
		// when in F or lateral, wider forward zone (±140).
		const auto CurDir = static_cast<EGMCMotion_MovementDirection>(MovementDirection);
		const float FrontLimit = (CurDir == EGMCMotion_MovementDirection::B) ? 120.0f : 140.0f;
		return FMath::Abs(Angle) <= FrontLimit
			? EGMCMotion_MovementDirection::F
			: EGMCMotion_MovementDirection::B;
	}

	// Mode 0: GASP 4-way (F, LL, RL, B) with hysteresis from S_MovementDirectionThresholds.
	// The original GASP Get_MovementDirectionFromThresholds only returns these 4 directions.
	// LR/RR enum values exist but are never produced by the threshold function.
	//
	// The thresholds change based on the current direction to prevent flickering at boundaries:
	//   When currently in F or B:  FL=-60, FR=60,  BL=-120, BR=120
	//   When currently in lateral: FL=-40, FR=40,  BL=-140, BR=140
	float FL, FR, BL, BR;
	const auto CurDir0 = static_cast<EGMCMotion_MovementDirection>(MovementDirection);
	if (CurDir0 == EGMCMotion_MovementDirection::F || CurDir0 == EGMCMotion_MovementDirection::B)
	{
		FL = -60.0f; FR = 60.0f; BL = -120.0f; BR = 120.0f;
	}
	else
	{
		FL = -40.0f; FR = 40.0f; BL = -140.0f; BR = 140.0f;
	}

	// Forward zone
	if (Angle >= FL && Angle <= FR)
	{
		return EGMCMotion_MovementDirection::F;
	}

	// Left zone: [BL, FL)
	if (Angle >= BL && Angle < FL)
	{
		return EGMCMotion_MovementDirection::LL;
	}

	// Right zone: (FR, BR]
	if (Angle > FR && Angle <= BR)
	{
		return EGMCMotion_MovementDirection::RL;
	}

	// Everything else is Backward
	return EGMCMotion_MovementDirection::B;
}

UCurveFloat* UGMCMotion::GetRotationOffsetCurveForDirection(EGMCMotion_MovementDirection Dir) const
{
	// Sliding override: matches CHT_RotationOffsetCurve rows 6-7.
	// When sliding + F: use the F curve.  When sliding + anything else: use SlideKnees.
	if (bIsSliding)
	{
		if (Dir == EGMCMotion_MovementDirection::F)
		{
			return RotationOffsetCurve_F;
		}
		return RotationOffsetCurve_SlideKnees;
	}

	// Normal (not sliding): 1:1 match of CHT_RotationOffsetCurve rows 0-5.
	switch (Dir)
	{
	case EGMCMotion_MovementDirection::F:  return RotationOffsetCurve_F;
	case EGMCMotion_MovementDirection::B:  return RotationOffsetCurve_B;
	case EGMCMotion_MovementDirection::LL: return RotationOffsetCurve_LL;
	case EGMCMotion_MovementDirection::LR: return RotationOffsetCurve_LR;
	case EGMCMotion_MovementDirection::RL: return RotationOffsetCurve_RL;
	case EGMCMotion_MovementDirection::RR: return RotationOffsetCurve_RR;
	default: return nullptr;
	}
}

FVector UGMCMotion::GetOrientationIntent() const
{
	if (RotationMode == EGMCMotion_RotationMode::Aiming)
	{
		if (const APawn* Pawn = Cast<APawn>(GetOwner()))
		{
			if (const AController* PC = Pawn->GetController())
			{
				FVector Forward = PC->GetControlRotation().Vector();
				Forward.Z = 0.0f;
				if (!Forward.IsNearlyZero())
				{
					Forward.Normalize();
					return Forward;
				}
			}
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
		if (const APawn* Pawn = Cast<APawn>(GetOwner()))
		{
			if (const AController* PC = Pawn->GetController())
			{
				return PC->GetControlRotation();
			}
		}
		return FRotator::ZeroRotator;
	}

	const FVector Vel = GetLinearVelocity_GMC();
	if (Vel.Size2D() > 1.0f)
	{
		return Vel.Rotation();
	}

	return OverridenDesiredFacing;
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
		return;
	}

	// 1. Compute the orientation intent and cache it for the trajectory predictor
	// (which reads OrientationIntent via FindPropertyByName).
	OrientationIntent = GetOrientationIntent();
	const float IntentYaw = OrientationIntent.Rotation().Yaw;

	// 2. Clamp the rotation offset so the target is always on the "closer" side to the
	// current actor rotation. Matches the BP's Update_Grounded:
	//   CurrentOffset = Delta(ActorRotation, TargetOrientation).Yaw
	//   ClampedOffset = Clamp(RotationOffset, CurrentOffset - 179, CurrentOffset + 179)
	// This prevents the pawn from rotating the "long way around" when the offset jumps.
	const float ActorYaw = UpdatedComponent ? UpdatedComponent->GetComponentRotation().Yaw : IntentYaw;
	const float CurrentOffset = FRotator::NormalizeAxis(ActorYaw - IntentYaw);
	const float ClampedRotationOffset = FMath::Clamp(RotationOffset, CurrentOffset - 179.f, CurrentOffset + 179.f);

	// 3. Set OverridenDesiredFacing directly (no spring). ApplyRotation handles smoothing
	// via RotateYawTowardsDirection, matching the reference project exactly.
	OverridenDesiredFacing = FRotator(0.0, FRotator::NormalizeAxis(IntentYaw + ClampedRotationOffset), 0.0);
}
