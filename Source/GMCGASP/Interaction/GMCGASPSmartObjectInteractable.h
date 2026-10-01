#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Interaction/GMCGASPInteractable.h"
#include "GMCGASPSmartObjectInteractable.generated.h"

/**
 * Makes a smart object actor (BP_SmartBench) usable with the interact key: the interaction component's trace finds it,
 * the server hands it to the pawn's UGMCGASPSmartObjectUser, which claims a slot and sits down.
 */
UCLASS(ClassGroup = (GMCGASP), meta = (BlueprintSpawnableComponent))
class GMCGASP_API UGMCGASPSmartObjectInteractable : public UActorComponent, public IGMCGASPInteractable
{
	GENERATED_BODY()

public:
	UGMCGASPSmartObjectInteractable();

	/** Prompt shown under the crosshair. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction")
	FText Prompt;

	virtual bool CanInteract_Implementation(APawn* Instigator) const override;
	virtual FText GetInteractionText_Implementation(APawn* Instigator) const override;
	virtual void Interact_Implementation(APawn* Instigator) override;
};
