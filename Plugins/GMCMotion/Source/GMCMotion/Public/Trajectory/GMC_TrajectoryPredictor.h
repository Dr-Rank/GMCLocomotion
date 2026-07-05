// GMCMotion - Trajectory predictor for GMC pawns.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Animation/TrajectoryTypes.h"
#include "PoseSearch/PoseSearchTrajectoryPredictor.h"
#include "GMC_TrajectoryPredictor.generated.h"

/**
 * Trajectory predictor for GMC. Implements IPoseSearchTrajectoryPredictor so
 * that the GASP Mover AnimBP pipeline (which expects a Predictor) can run on a
 * GMC pawn instead of a Mover one. Keeps an internal history buffer; future
 * samples are produced via a simple kinematic projection from current velocity
 * and intent acceleration.
 */
UCLASS(BlueprintType, Blueprintable)
class GMCMOTION_API UGMC_TrajectoryPredictor : public UObject, public IPoseSearchTrajectoryPredictorInterface
{
	GENERATED_BODY()

public:
	UGMC_TrajectoryPredictor();

	/** Bind the predictor to a GMC component. Call once after Construct Object. */
	UFUNCTION(BlueprintCallable, Category = "GMC|Trajectory")
	void Setup(UGMC_OrganicMovementCmp* InMovement);

	// IPoseSearchTrajectoryPredictorInterface
	virtual void Predict(FTransformTrajectory& InOutTrajectory, int32 NumPredictionSamples, float SecondsPerPredictionSample, int32 NumHistorySamples) override;
	virtual void GetGravity(FVector& OutGravityAccel) override;
	virtual void GetCurrentState(FVector& OutPosition, FQuat& OutFacing, FVector& OutVelocity) override;
	virtual void GetVelocity(FVector& OutVelocity) override;
	virtual void GetAngularVelocity(FVector& OutAngularVelocityDegrees) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UGMC_OrganicMovementCmp> Movement;

	/** Ring of past samples, oldest first. Pushed every Predict() call. */
	UPROPERTY(Transient)
	TArray<FTransformTrajectorySample> HistoryBuffer;

	/** Game time (World->GetTimeSeconds) at which the last history sample was captured. */
	float LastCaptureTime = 0.0f;

	/** Capture interval for history samples (matches typical PoseSearch settings). */
	static constexpr float HistorySamplingInterval = 1.0f / 30.0f;
};
