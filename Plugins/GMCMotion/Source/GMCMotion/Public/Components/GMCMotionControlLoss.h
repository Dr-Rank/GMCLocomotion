// GMCMotion - a component on the pawn that can take control of it away from the player (a knockdown, a get-up, being
// the target of a shove). UGMCMotion asks it, through this interface, whether to hold the camera yaw limit
// (KnockdownCameraYawRange), so GMCMotion does not depend on whichever module implements the ragdoll.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "GMCMotionControlLoss.generated.h"

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UGMCMotionControlLossSource : public UInterface
{
	GENERATED_BODY()
};

class GMCMOTION_API IGMCMotionControlLossSource
{
	GENERATED_BODY()

public:
	// True while the player has no control of the pawn. Read on the owning client only (the camera limit is local).
	virtual bool IsPlayerControlLost() const = 0;

	// True while the control loss is a get-up (the body follows a montage now, it no longer tumbles): UGMCMotion turns the
	// view to the pawn's facing then (GetUpViewAlignTime) instead of holding it on the facing at the loss of control.
	virtual bool IsGettingUp() const { return false; }
};
