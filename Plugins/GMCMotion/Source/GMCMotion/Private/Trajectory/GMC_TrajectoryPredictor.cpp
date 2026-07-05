// GMCMotion - Trajectory predictor for GMC pawns.

#include "Trajectory/GMC_TrajectoryPredictor.h"
#include "GMCMotionLog.h"
#include "Components/GMCOrganicMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"

UGMC_TrajectoryPredictor::UGMC_TrajectoryPredictor()
{
}

void UGMC_TrajectoryPredictor::Setup(UGMC_OrganicMovementCmp* InMovement)
{
	Movement = InMovement;
	HistoryBuffer.Reset();
	LastCaptureTime = 0.0f;
}

void UGMC_TrajectoryPredictor::GetVelocity(FVector& OutVelocity)
{
	OutVelocity = Movement ? Movement->GetLinearVelocity_GMC() : FVector::ZeroVector;
}

void UGMC_TrajectoryPredictor::GetAngularVelocity(FVector& OutAngularVelocityDegrees)
{
	OutAngularVelocityDegrees = FVector::ZeroVector;
}

void UGMC_TrajectoryPredictor::GetGravity(FVector& OutGravityAccel)
{
	if (Movement)
	{
		OutGravityAccel = Movement->GetGravity();
	}
	else if (const UWorld* World = GetWorld())
	{
		OutGravityAccel = FVector(0.0f, 0.0f, World->GetGravityZ());
	}
	else
	{
		OutGravityAccel = FVector(0.0f, 0.0f, -981.0f);
	}
}

void UGMC_TrajectoryPredictor::GetCurrentState(FVector& OutPosition, FQuat& OutFacing, FVector& OutVelocity)
{
	if (!Movement)
	{
		OutPosition = FVector::ZeroVector;
		OutFacing = FQuat::Identity;
		OutVelocity = FVector::ZeroVector;
		return;
	}
	const AActor* Owner = Movement->GetOwner();
	if (Owner)
	{
		OutPosition = Owner->GetActorLocation();
		// Trajectory facing in MESH/component space, matching Mover's MoverTrajectoryPredictor
		// (its samples report Facing yaw = -90, the mesh import offset). Avoids injecting the
		// ~90deg mesh offset into the trajectory facing (which caused the spawn pivot / constant
		// offset-root reconciliation). The pawn still turns via anim root motion + ApplyFacingSpring.
		const USkeletalMeshComponent* Mesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
		OutFacing = Mesh ? Mesh->GetComponentQuat() : Owner->GetActorQuat();
	}
	else
	{
		OutPosition = FVector::ZeroVector;
		OutFacing = FQuat::Identity;
	}
	OutVelocity = Movement->GetLinearVelocity_GMC();
}

void UGMC_TrajectoryPredictor::Predict(FTransformTrajectory& InOutTrajectory, int32 NumPredictionSamples, float SecondsPerPredictionSample, int32 NumHistorySamples)
{
	InOutTrajectory.Samples.Reset();

	FVector CurrentPos;
	FQuat CurrentFacing;
	FVector CurrentVelocity;
	GetCurrentState(CurrentPos, CurrentFacing, CurrentVelocity);

	// Capture a history sample, stamped with the REAL game time. Emitting past samples
	// with a fixed 1/30 label (the old code) over-estimated past velocity whenever frames
	// ran slower than 30fps - GetTrajectoryVelocity divides dPos by the labeled dt, so a
	// real 0.045s frame labeled as 0.0333s reported ~1.36x the true speed, and it jittered
	// with frame time. The schema samples history at -0.05s, so that corrupted, noisy feature
	// fed Motion Matching and caused pose-selection flip-flop. Use real elapsed time instead.
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.0f;
	if (HistoryBuffer.Num() == 0 || (Now - LastCaptureTime) >= HistorySamplingInterval)
	{
		FTransformTrajectorySample HistorySample;
		HistorySample.TimeInSeconds = Now;   // store ABSOLUTE capture time; converted to relative on emit
		HistorySample.Position = CurrentPos;
		HistorySample.Facing = CurrentFacing;
		HistoryBuffer.Add(HistorySample);
		LastCaptureTime = Now;
	}

	// Trim buffer to the requested history length.
	const int32 MaxHistory = FMath::Max(NumHistorySamples, 1);
	while (HistoryBuffer.Num() > MaxHistory)
	{
		HistoryBuffer.RemoveAt(0, EAllowShrinking::No);
	}

	// Past samples: emit oldest first with REAL negative TimeInSeconds relative to now,
	// so GetTrajectoryVelocity recovers the true past velocity (and the -0.05s schema sample
	// lands on the correct physical position).
	const int32 HistoryCount = HistoryBuffer.Num();
	for (int32 i = 0; i < HistoryCount; ++i)
	{
		FTransformTrajectorySample Sample = HistoryBuffer[i];
		Sample.TimeInSeconds = HistoryBuffer[i].TimeInSeconds - Now;
		InOutTrajectory.Samples.Add(Sample);
	}

	// Future samples: mimic Mover's per-step replay of the movement mode.
	//  - Linear: accelerate toward input dir + clamp to MaxSpeed, OR brake+friction when input is zero.
	//  - Angular: step yaw toward control rotation or input direction at RotationRate (matches
	//    GMC's bOrientToControlRotationDirection / bOrientToInputDirection).
	if (Movement)
	{
		FVector ProjPos = CurrentPos;
		FVector ProjVel = CurrentVelocity;
		FQuat   ProjFacing = CurrentFacing;

		// Prefer the bound CustomComputeInputAcceleration (BindCompressedVector) when available.
		// It is in world space and consistent across all roles, unlike GetProcessedInputVector()
		// which on the autonomous proxy can be in camera-local space during client-side prediction,
		// producing trajectories that diverge from the authority for the same pawn.
		FVector InputVector = FVector::ZeroVector;
		if (const FProperty* InputProp = Movement->GetClass()->FindPropertyByName(TEXT("CustomComputeInputAcceleration")))
		{
			if (const FStructProperty* SP = CastField<FStructProperty>(InputProp))
			{
				if (SP->Struct == TBaseStructure<FVector>::Get())
				{
					InputVector = *SP->ContainerPtrToValuePtr<FVector>(Movement);
				}
			}
		}

		const FVector InputDir = InputVector.GetSafeNormal();
		const bool bHasInput = !InputDir.IsNearlyZero();

		const float InputAccel = Movement->GetInputAcceleration();
		const float MaxSpeed = Movement->GetMaxSpeed();
		const float Friction = Movement->GetGroundFriction();
		const float Braking = Movement->GetBrakingDeceleration();

		// Target yaw the pawn rotates toward each tick.
		// The actor is rotated by ApplyFacingSpring (critically-damped spring) toward
		// OverridenDesiredFacing, computed in Update_Grounded as Combine(Get_TargetOrientation,
		// strafe/turn yaw offset). That offset is the ACTUAL goal the pawn settles on
		// (e.g. -90 in stable left strafe). Earlier this read OrientationIntent, which is only
		// the aim component WITHOUT the strafe offset -> the predictor believed the pawn had to
		// keep turning by that offset every frame, encoding a constant angular velocity equal to
		// RotationRate in the trajectory (the 0,0,650 vs Mover's 0,0,0). Target the real spring
		// goal so a settled strafe yields zero trajectory angular velocity. Read via reflection
		// (world-space FRotator); fall back to OrientationIntent / native flags if absent.
		const float YawRate = Movement->RotationRate;
		bool  bHasTargetYaw = false;
		float TargetYawDeg = 0.f;

		if (const FProperty* FacingProp = Movement->GetClass()->FindPropertyByName(TEXT("OverridenDesiredFacing")))
		{
			if (const FStructProperty* SP = CastField<FStructProperty>(FacingProp))
			{
				if (SP->Struct == TBaseStructure<FRotator>::Get())
				{
					TargetYawDeg = SP->ContainerPtrToValuePtr<FRotator>(Movement)->Yaw; // world space
					bHasTargetYaw = true;
				}
			}
		}

		// Fallback: OrientationIntent (aim component only, world-space vector). Used if the
		// component does not expose OverridenDesiredFacing (e.g. a different movement mode).
		if (!bHasTargetYaw)
		{
			if (const FProperty* IntentProp = Movement->GetClass()->FindPropertyByName(TEXT("OrientationIntent")))
			{
				if (const FStructProperty* StructProp = CastField<FStructProperty>(IntentProp))
				{
					if (StructProp->Struct == TBaseStructure<FVector>::Get())
					{
						const FVector OrientationIntent = *StructProp->ContainerPtrToValuePtr<FVector>(Movement);
						if (!OrientationIntent.IsNearlyZero())
						{
							const FVector IntentFlat = FVector(OrientationIntent.X, OrientationIntent.Y, 0.f).GetSafeNormal();
							if (!IntentFlat.IsNearlyZero())
							{
								TargetYawDeg = IntentFlat.Rotation().Yaw;
								bHasTargetYaw = true;
							}
						}
					}
				}
			}
		}
		// ProjFacing is in mesh space (see GetCurrentState). The target above is in world/actor space.
		// Convert TargetYaw to mesh space by applying the mesh's yaw offset from the actor.
		if (bHasTargetYaw)
		{
			if (const AActor* Owner = Movement->GetOwner())
			{
				if (const USkeletalMeshComponent* Mesh = Owner->FindComponentByClass<USkeletalMeshComponent>())
				{
					const float MeshYawOffset = FRotator::NormalizeAxis(
						Mesh->GetComponentQuat().Rotator().Yaw - Owner->GetActorQuat().Rotator().Yaw);
					TargetYawDeg = FRotator::NormalizeAxis(TargetYawDeg + MeshYawOffset);
				}
			}
		}

		if (!bHasTargetYaw)
		{
			if (Movement->bOrientToControlRotationDirection)
			{
				// Read AimingRotation2 (BindCompressedRotator) instead of the local controller.
				// GetController() is null on simulated proxies; the bound value is correct on all roles.
				if (const FProperty* Prop = Movement->GetClass()->FindPropertyByName(TEXT("AimingRotation2")))
				{
					if (const FStructProperty* SP = CastField<FStructProperty>(Prop))
					{
						if (SP->Struct == TBaseStructure<FRotator>::Get())
						{
							const FRotator R = *SP->ContainerPtrToValuePtr<FRotator>(Movement);
							TargetYawDeg = R.Yaw;
							bHasTargetYaw = true;
						}
					}
				}
			}
			else if (Movement->bOrientToInputDirection && bHasInput)
			{
				const FVector InputDirFlat = FVector(InputDir.X, InputDir.Y, 0.f).GetSafeNormal();
				if (!InputDirFlat.IsNearlyZero())
				{
					TargetYawDeg = InputDirFlat.Rotation().Yaw;
					bHasTargetYaw = true;
				}
			}
		}

		for (int32 i = 1; i <= NumPredictionSamples; ++i)
		{
			const float t = i * SecondsPerPredictionSample;
			const float dt = SecondsPerPredictionSample;

			// --- Linear update ---
			FVector NewVel;
			if (bHasInput)
			{
				NewVel = ProjVel + InputDir * (InputAccel * dt);
				NewVel = NewVel.GetClampedToMaxSize(MaxSpeed);
			}
			else
			{
				const float Speed = ProjVel.Size();
				if (Speed > KINDA_SMALL_NUMBER)
				{
					// CMC-style stop: linear braking + proportional friction.
					const float Decel = (Braking + Friction * Speed) * dt;
					const float NewSpeed = FMath::Max(0.f, Speed - Decel);
					NewVel = ProjVel * (NewSpeed / Speed);
				}
				else
				{
					NewVel = FVector::ZeroVector;
				}
			}

			ProjPos += (ProjVel + NewVel) * 0.5f * dt;
			ProjVel = NewVel;

			// --- Angular update ---
			if (bHasTargetYaw && YawRate > 0.f)
			{
				FRotator ProjRot = ProjFacing.Rotator();
				const float DeltaYaw = FMath::FindDeltaAngleDegrees(ProjRot.Yaw, TargetYawDeg);
				const float MaxStep = YawRate * dt;
				const float Step = FMath::Clamp(DeltaYaw, -MaxStep, MaxStep);
				ProjRot.Yaw += Step;
				ProjFacing = ProjRot.Quaternion();
			}

			FTransformTrajectorySample FutureSample;
			FutureSample.TimeInSeconds = t;
			FutureSample.Position = ProjPos;
			FutureSample.Facing = ProjFacing;
			InOutTrajectory.Samples.Add(FutureSample);
		}
	}
}
