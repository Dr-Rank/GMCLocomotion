// GMCMotion - Trajectory predictor for GMC pawns.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Animation/TrajectoryTypes.h"
#include "PoseSearch/PoseSearchTrajectoryPredictor.h"
#include "GMC_TrajectoryPredictor.generated.h"

class UGMC_OrganicMovementCmp;
class UGMCMotion;
class USceneComponent;

/**
 * Trajectory predictor for GMC. Implements IPoseSearchTrajectoryPredictor so
 * that the GASP Mover AnimBP pipeline (which expects a Predictor) can run on a
 * GMC pawn instead of a Mover one.
 *
 * Contract (UPoseSearchTrajectoryLibrary::PoseSearchGenerateTransformTrajectoryWithPredictor):
 * the library owns the history and the current sample (index NumHistorySamples); Predict only
 * fills the future samples in place, like Mover's UMoverTrajectoryPredictor. Future samples come
 * from a kinematic projection of the current velocity and intent acceleration.
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
	/** Mesh driven by the owning AnimInstance (the body), else the pawn's first skeletal mesh. */
	USceneComponent* GetVisualComponent() const;

	UPROPERTY(Transient)
	TObjectPtr<UGMC_OrganicMovementCmp> Movement;

	/** Movement as our GMC component (null for a plain organic component: reflection fallbacks are used then). */
	UPROPERTY(Transient)
	TObjectPtr<UGMCMotion> Motion;

	/** Cached by Setup / first use; see GetVisualComponent. */
	mutable TWeakObjectPtr<USceneComponent> VisualComponent;

	/** BP_GMCMovement variables replayed by the prediction (VelocityAdjustments / TurningStrengthFunction), found by
	 *  name in Setup. Any missing -> the velocity re-aim is not predicted. */
	const FProperty* RunSpeedProp = nullptr;
	const FProperty* SprintSpeedProp = nullptr;
	const FProperty* RunTurnStrengthProp = nullptr;
	const FProperty* SprintTurnStrengthProp = nullptr;
	const FProperty* TraversingProp = nullptr;

	/** BP TurningStrengthFunction: turn strength mapped from the XY speed (run speed..sprint speed). <0 when unknown. */
	float GetTurnStrength(float SpeedXY) const;

	bool bWarnedTrajectorySize = false;
};
