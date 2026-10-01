#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "GMCGASPInteractable.generated.h"

class APawn;

UINTERFACE(MinimalAPI, BlueprintType)
class UGMCGASPInteractable : public UInterface
{
	GENERATED_BODY()
};

/**
 * Something a pawn can use with the interact key (pickups, corpses, ...). Found by UAC_GMCGASPInteraction's trace.
 * CanInteract / GetInteractionText run on the interacting player's machine (prompt) and again on the server;
 * Interact only runs on the server.
 */
class GMCGASP_API IGMCGASPInteractable
{
	GENERATED_BODY()

public:
	/** Can @Instigator use this right now? Evaluated every trace, keep it cheap. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Interaction")
	bool CanInteract(APawn* Instigator) const;

	/** Prompt shown to @Instigator, e.g. "Pick up M4A1". */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Interaction")
	FText GetInteractionText(APawn* Instigator) const;

	/** Perform the interaction. Server only; the interaction component has already validated CanInteract and the range. */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "Interaction")
	void Interact(APawn* Instigator);
};
