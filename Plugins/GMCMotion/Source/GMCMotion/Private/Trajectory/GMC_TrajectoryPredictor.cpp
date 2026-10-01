// GMCMotion - Trajectory predictor for GMC pawns.

#include "Trajectory/GMC_TrajectoryPredictor.h"
#include "GMCMotionLog.h"
#include "Components/GMCMotion.h"
#include "Components/GMCOrganicMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"

UGMC_TrajectoryPredictor::UGMC_TrajectoryPredictor()
{
}

void UGMC_TrajectoryPredictor::Setup(UGMC_OrganicMovementCmp* InMovement)
{
	Movement = InMovement;
	Motion = Cast<UGMCMotion>(InMovement);
	VisualComponent = nullptr;
	GetVisualComponent();

	const UClass* MovementClass = InMovement ? InMovement->GetClass() : nullptr;
	auto FindNumeric = [MovementClass](const TCHAR* Name) -> const FProperty*
	{
		const FNumericProperty* Prop = CastField<FNumericProperty>(MovementClass ? MovementClass->FindPropertyByName(Name) : nullptr);
		return Prop && Prop->IsFloatingPoint() ? Prop : nullptr;
	};
	RunSpeedProp = FindNumeric(TEXT("RunSpeed"));
	SprintSpeedProp = FindNumeric(TEXT("SprintSpeed"));
	RunTurnStrengthProp = FindNumeric(TEXT("Walk/RunTurnStrength"));
	SprintTurnStrengthProp = FindNumeric(TEXT("SprintTurnStrength"));
	TraversingProp = MovementClass ? CastField<FBoolProperty>(MovementClass->FindPropertyByName(TEXT("Traversing"))) : nullptr;
}

float UGMC_TrajectoryPredictor::GetTurnStrength(float SpeedXY) const
{
	if (!Movement || !RunSpeedProp || !SprintSpeedProp || !RunTurnStrengthProp || !SprintTurnStrengthProp)
	{
		return -1.f;
	}
	auto Read = [this](const FProperty* Prop)
	{
		return static_cast<float>(CastField<FNumericProperty>(Prop)->GetFloatingPointPropertyValue(Prop->ContainerPtrToValuePtr<void>(Movement)));
	};
	// Same as BP_GMCMovement::TurningStrengthFunction (MapRangeClamped).
	return FMath::GetMappedRangeValueClamped(FVector2f(Read(RunSpeedProp), Read(SprintSpeedProp)),
		FVector2f(Read(RunTurnStrengthProp), Read(SprintTurnStrengthProp)), SpeedXY);
}

USceneComponent* UGMC_TrajectoryPredictor::GetVisualComponent() const
{
	if (USceneComponent* Cached = VisualComponent.Get())
	{
		return Cached;
	}
	// The ABP constructs the predictor with itself as outer: its mesh is the body the trajectory is for. The pawn can
	// carry other skeletal meshes (first-person body, modular parts), so FindComponentByClass is only a fallback.
	USceneComponent* Found = nullptr;
	if (const UAnimInstance* AnimInstance = Cast<UAnimInstance>(GetOuter()))
	{
		Found = AnimInstance->GetSkelMeshComponent();
	}
	if (!Found && Movement)
	{
		if (const AActor* Owner = Movement->GetOwner())
		{
			Found = Owner->FindComponentByClass<USkeletalMeshComponent>();
		}
	}
	VisualComponent = Found;
	return Found;
}

void UGMC_TrajectoryPredictor::GetVelocity(FVector& OutVelocity)
{
	OutVelocity = Movement ? Movement->GetLinearVelocity_GMC() : FVector::ZeroVector;
}

void UGMC_TrajectoryPredictor::GetAngularVelocity(FVector& OutAngularVelocityDegrees)
{
	// Yaw rate of the capsule from the last move (ApplyRotation), bound so simulated proxies get it too. The library
	// subtracts it from the frame's facing change to infer a moving-platform rotation applied to the history: returning
	// 0 while the pawn turns would make it rotate the whole history with the pawn.
	OutAngularVelocityDegrees = Motion
		? FVector(0.f, 0.f, FMath::RadiansToDegrees(Motion->AngularVelocityRad))
		: FVector::ZeroVector;
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
	// Position and facing of the visual mesh, like Mover's predictor (bUseVisualComponentRoot): the trajectory is the
	// root bone's path (feet level, what the pose search databases and HandleTransformTrajectoryWorldCollisions expect),
	// and it includes GMC's mesh smoothing on simulated proxies. Facing stays in mesh space (Mover's samples report the
	// -90 import offset too); putting the actor yaw here injected that offset into the trajectory (spawn pivot).
	if (const USceneComponent* Visual = GetVisualComponent())
	{
		OutPosition = Visual->GetComponentLocation();
		OutFacing = Visual->GetComponentQuat();
	}
	else if (const AActor* Owner = Movement->GetOwner())
	{
		OutPosition = Owner->GetActorLocation();
		OutFacing = Owner->GetActorQuat();
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
	// The library has already written the history and the current sample (index NumHistorySamples, stamped with the
	// frame's delta time); only the future samples are ours, in place.
	if (!Movement || InOutTrajectory.Samples.Num() != NumHistorySamples + 1 + NumPredictionSamples)
	{
		if (Movement && !bWarnedTrajectorySize)
		{
			bWarnedTrajectorySize = true;
			UE_LOG(LogGMCMotion, Warning, TEXT("UGMC_TrajectoryPredictor::Predict - trajectory has %d samples, expected %d history + 1 current + %d predicted; left untouched."),
				InOutTrajectory.Samples.Num(), NumHistorySamples, NumPredictionSamples);
		}
		return;
	}

	const FTransformTrajectorySample& Current = InOutTrajectory.Samples[NumHistorySamples];
	const float CurrentTime = Current.TimeInSeconds;
	FVector ProjPos = Current.Position;
	FQuat ProjFacing = Current.Facing;
	FVector ProjVel = Movement->GetLinearVelocity_GMC();

	// Future samples: replay of the GMC move (Mover does the same with its own movement mode), see the step below.

	// CustomComputeInputAcceleration (bound, world space) is consistent across all roles, unlike
	// GetProcessedInputVector() which on the autonomous proxy can be in camera-local space during client-side
	// prediction, producing trajectories that diverge from the authority for the same pawn.
	FVector InputVector = FVector::ZeroVector;
	if (Motion)
	{
		InputVector = Motion->CustomComputeInputAcceleration;
	}
	else if (const FStructProperty* SP = CastField<FStructProperty>(Movement->GetClass()->FindPropertyByName(TEXT("CustomComputeInputAcceleration"))))
	{
		if (SP->Struct == TBaseStructure<FVector>::Get())
		{
			InputVector = *SP->ContainerPtrToValuePtr<FVector>(Movement);
		}
	}

	const FVector InputDir = InputVector.GetSafeNormal();
	const bool bHasInput = !InputDir.IsNearlyZero();

	const float InputAccel = Movement->GetInputAcceleration();
	const float MaxSpeed = Movement->GetMaxSpeed();
	const float Friction = Movement->GetGroundFriction();
	const float Braking = Movement->GetBrakingDeceleration();
	const float OverMaxSpeedDecel = Movement->GetOverMaxSpeedDeceleration();
	const FVector Gravity = Movement->GetGravity();

	// Target yaw the pawn rotates toward each tick: OverridenDesiredFacing (world space), the goal UpdateGroundedFacing
	// writes and ApplyRotation turns the capsule to, strafe offset included. OrientationIntent is only the aim
	// component WITHOUT that offset (a settled strafe would then read as a constant turn).
	const float YawRate = Movement->RotationRate;
	bool  bHasTargetYaw = false;
	float TargetYawDeg = 0.f;

	if (Motion)
	{
		TargetYawDeg = Motion->OverridenDesiredFacing.Yaw;
		bHasTargetYaw = true;
	}
	else
	{
		// Plain organic component: same targets read by name when a subclass declares them.
		const UClass* MovementClass = Movement->GetClass();
		if (const FStructProperty* SP = CastField<FStructProperty>(MovementClass->FindPropertyByName(TEXT("OverridenDesiredFacing"))))
		{
			if (SP->Struct == TBaseStructure<FRotator>::Get())
			{
				TargetYawDeg = SP->ContainerPtrToValuePtr<FRotator>(Movement)->Yaw;
				bHasTargetYaw = true;
			}
		}
		if (!bHasTargetYaw)
		{
			if (const FStructProperty* SP = CastField<FStructProperty>(MovementClass->FindPropertyByName(TEXT("OrientationIntent"))))
			{
				if (SP->Struct == TBaseStructure<FVector>::Get())
				{
					const FVector Intent = *SP->ContainerPtrToValuePtr<FVector>(Movement);
					const FVector IntentFlat = FVector(Intent.X, Intent.Y, 0.f).GetSafeNormal();
					if (!IntentFlat.IsNearlyZero())
					{
						TargetYawDeg = IntentFlat.Rotation().Yaw;
						bHasTargetYaw = true;
					}
				}
			}
		}
		if (!bHasTargetYaw)
		{
			if (Movement->bOrientToControlRotationDirection)
			{
				// AimingRotation2 (BindCompressedRotator) instead of the local controller, null on simulated proxies.
				if (const FStructProperty* SP = CastField<FStructProperty>(MovementClass->FindPropertyByName(TEXT("AimingRotation2"))))
				{
					if (SP->Struct == TBaseStructure<FRotator>::Get())
					{
						TargetYawDeg = SP->ContainerPtrToValuePtr<FRotator>(Movement)->Yaw;
						bHasTargetYaw = true;
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
	}

	// ProjFacing is in mesh space (see GetCurrentState), the target in actor space: add the mesh's yaw offset.
	if (bHasTargetYaw)
	{
		const USceneComponent* Visual = GetVisualComponent();
		const AActor* Owner = Movement->GetOwner();
		if (Visual && Owner)
		{
			const float MeshYawOffset = FRotator::NormalizeAxis(
				Visual->GetComponentQuat().Rotator().Yaw - Owner->GetActorQuat().Rotator().Yaw);
			TargetYawDeg = FRotator::NormalizeAxis(TargetYawDeg + MeshYawOffset);
		}
	}

	// Linear model per movement mode:
	//  - grounded / airborne: GMC's CalculateVelocity (UGMC_OrganicMovementCmp::ApplyInputVelocity, ApplyExternalForces,
	//    ApplyDeceleration, LimitSpeed; traction 0) then BP_GMCMovement::VelocityAdjustments (PostPhysicsUpdate: while
	//    grounded with input, the XY velocity is re-aimed at the input direction at constant speed, VInterpTo at the BP
	//    TurningStrength). Stepped at ~60 Hz like the moves, sampled every SecondsPerPredictionSample.
	//  - anything else (slope slide, custom modes): the current velocity is kept.
	const bool bGrounded = Movement->IsMovingOnGround();
	const bool bAirborne = Movement->IsAirborne();
	const bool bReplayMove = (bGrounded || bAirborne) && !(Motion && Motion->IsSlopeSliding());
	const bool bTraversing = TraversingProp && CastField<FBoolProperty>(TraversingProp)->GetPropertyValue_InContainer(Movement);
	const FVector InputAcceleration = bHasInput ? InputDir * InputAccel * (bGrounded ? FMath::Min(Friction, 1.f) : 1.f) : FVector::ZeroVector;
	constexpr float MinDeceleration = 5.f;      // UGMC_OrganicMovementCmp::MIN_DECELERATION
	constexpr float BrakeToStopVelocity = UE_KINDA_SMALL_NUMBER;

	auto StepMove = [&](FVector& V, float H)
	{
		if (bGrounded)
		{
			V.Z = 0.;
		}
		// ApplyInputVelocity + ApplyExternalForces.
		V += InputAcceleration * H;
		FVector TransientAcceleration = InputAcceleration;
		if (bAirborne)
		{
			V += Gravity * H;
			TransientAcceleration += Gravity;
		}
		// ApplyDeceleration: brakes only against the part of the velocity we are not accelerating along.
		if (!V.IsZero())
		{
			const float Scale = FMath::Clamp(1.f - (TransientAcceleration.GetSafeNormal() | V.GetSafeNormal()), 0.f, 1.f);
			FVector Decel = Braking * Scale * -V.GetSafeNormal();
			if (bGrounded)
			{
				Decel *= Friction;
			}
			else if (bAirborne && V.Z < 0.)
			{
				Decel.Z = 0.;   // no braking against gravity
			}
			if (!Decel.IsZero())
			{
				const FVector DecelXY(Decel.X, Decel.Y, 0.);
				if (DecelXY.SizeSquared() > 0. && DecelXY.SizeSquared() < FMath::Square(MinDeceleration))
				{
					Decel = DecelXY.GetClampedToSize(MinDeceleration, UE_BIG_NUMBER) + FVector(0., 0., Decel.Z);
				}
				const FVector Start = V;
				V += Decel * H;
				if ((Start | V) <= 0. || V.SizeSquared() <= FMath::Square(BrakeToStopVelocity))
				{
					V = FVector::ZeroVector;
				}
			}
		}
		// LimitSpeed (grounded: XYZ, airborne: XY), never below the max speed.
		const FVector Limited = bAirborne ? FVector(V.X, V.Y, 0.) : V;
		if (Limited.SizeSquared() > FMath::Square(MaxSpeed))
		{
			const FVector Dir = Limited.GetSafeNormal();
			FVector NewLimited = Limited - Dir * OverMaxSpeedDecel * H;
			if ((NewLimited | Limited) <= 0. || NewLimited.SizeSquared() < FMath::Square(MaxSpeed))
			{
				NewLimited = Dir * MaxSpeed;
			}
			V = bAirborne ? FVector(NewLimited.X, NewLimited.Y, V.Z) : NewLimited;
		}
		// BP VelocityAdjustments (PostPhysicsUpdate).
		const float SpeedXY = V.Size2D();
		if (bHasInput && !bAirborne && !bTraversing && SpeedXY > 1.f)
		{
			const float TurnStrength = GetTurnStrength(SpeedXY);
			if (TurnStrength >= 0.f)
			{
				const FVector CurrentXY(V.X, V.Y, 0.);
				const FVector TargetXY = FVector(InputDir.X, InputDir.Y, 0.).GetSafeNormal() * SpeedXY;
				const FVector NewXY = FMath::VInterpTo(CurrentXY, TargetXY, H, TurnStrength);
				V.X = NewXY.X;
				V.Y = NewXY.Y;
			}
		}
	};

	const float dt = SecondsPerPredictionSample;
	const int32 SubSteps = FMath::Max(1, FMath::CeilToInt32(dt * 60.f));
	const float H = dt / SubSteps;
	for (int32 i = 1; i <= NumPredictionSamples; ++i)
	{
		// --- Linear update ---
		for (int32 Step = 0; Step < SubSteps; ++Step)
		{
			if (bReplayMove)
			{
				FVector NewVel = ProjVel;
				StepMove(NewVel, H);
				ProjPos += (ProjVel + NewVel) * 0.5f * H;
				ProjVel = NewVel;
			}
			else
			{
				ProjPos += ProjVel * H;
			}
		}

		// --- Angular update ---
		// BP_GMCMovement runs with RotationRate 0 (instant snap in GMC): the capsule is already on its target every
		// frame, so the facing is kept. A free look would make a predicted snap wrong.
		if (bHasTargetYaw && YawRate > 0.f)
		{
			FRotator ProjRot = ProjFacing.Rotator();
			const float DeltaYaw = FMath::FindDeltaAngleDegrees(ProjRot.Yaw, TargetYawDeg);
			const float Step = FMath::Clamp(DeltaYaw, -YawRate * dt, YawRate * dt);
			ProjRot.Yaw += Step;
			ProjFacing = ProjRot.Quaternion();
		}

		FTransformTrajectorySample& FutureSample = InOutTrajectory.Samples[NumHistorySamples + i];
		FutureSample.TimeInSeconds = CurrentTime + i * dt;
		FutureSample.Position = ProjPos;
		FutureSample.Facing = ProjFacing;
	}
}
