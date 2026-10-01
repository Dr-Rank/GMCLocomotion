#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "GameFramework/Actor.h"
#include "Combat/GMCGASPRagdollComponent.h"
#include "GMCGASPRagdollTriggers.generated.h"

class UBoxComponent;

/**
 * Puts the pawn into ragdoll from an animation (GASP58 BP_AnimNotify_TriggerRagdoll: shoves, tackles, takedowns).
 * The server decides: the notify only acts where the pawn has authority, the clients get the ragdoll through replication.
 */
UCLASS(meta = (DisplayName = "Trigger Ragdoll (GMCGASP)"))
class GMCGASP_API UGMCGASPAnimNotify_TriggerRagdoll : public UAnimNotify
{
	GENERATED_BODY()

public:
	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override { return TEXT("Trigger Ragdoll"); }

	UPROPERTY(EditAnywhere, Category = "Ragdoll")
	EGMCGASPRagdollInjury Injury = EGMCGASPRagdollInjury::None;
};

/**
 * Zero gravity volume (GASP58 BP_ZeroGZone): pawns inside float, and so do their ragdolls. Overlaps run on every machine
 * (the capsule overlaps the box at the same moves), so each machine switches its own copy.
 */
UCLASS()
class GMCGASP_API AGMCGASPZeroGZone : public AActor
{
	GENERATED_BODY()

public:
	AGMCGASPZeroGZone();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Zero G")
	TObjectPtr<UBoxComponent> Zone;

	/** Gravity scale of the pawns (GMC GravityScale) and of the ragdolls inside. */
	UPROPERTY(EditAnywhere, Category = "Zero G", meta = (ClampMin = 0.))
	float GravityScale = 0.f;

protected:
	UFUNCTION()
	void OnZoneBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	UFUNCTION()
	void OnZoneEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex);

private:
	void ApplyGravity(AActor* Pawn, float Scale) const;
};
