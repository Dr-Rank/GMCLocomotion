#include "Interaction/GMCGASPSmartObjectInteractable.h"

#include "GameFramework/Pawn.h"
#include "Interaction/GMCGASPSmartObjectUser.h"

#define LOCTEXT_NAMESPACE "GMCGASPSmartObject"

UGMCGASPSmartObjectInteractable::UGMCGASPSmartObjectInteractable()
{
	PrimaryComponentTick.bCanEverTick = false;
	Prompt = LOCTEXT("Sit", "Sit");
}

bool UGMCGASPSmartObjectInteractable::CanInteract_Implementation(APawn* Instigator) const
{
	const UGMCGASPSmartObjectUser* User = Instigator ? Instigator->FindComponentByClass<UGMCGASPSmartObjectUser>() : nullptr;
	return User && User->CanUse() && User->HasFreeSlot(GetOwner());
}

FText UGMCGASPSmartObjectInteractable::GetInteractionText_Implementation(APawn* Instigator) const
{
	return Prompt;
}

void UGMCGASPSmartObjectInteractable::Interact_Implementation(APawn* Instigator)
{
	if (UGMCGASPSmartObjectUser* User = Instigator ? Instigator->FindComponentByClass<UGMCGASPSmartObjectUser>() : nullptr)
	{
		User->TryUse(GetOwner());
	}
}

#undef LOCTEXT_NAMESPACE
