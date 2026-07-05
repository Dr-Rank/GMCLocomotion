// GMCMotion - GMC traversal movement component: deterministic per-window motion warping for
// traversal montages, applied inside the GMC prediction loop (PreProcessRootMotion).
//
// GMC extracts raw montage root motion via its own FGMC_AnimMontageInstance.Advance(),
// bypassing the engine MotionWarpingComponent (which hooks the AnimInstance root-motion
// modifier pipeline). So the warp must be re-implemented here, driven by the GMC-predicted
// montage position + replicated ledge targets, to stay deterministic across client/server.

#pragma once

#include "CoreMinimal.h"
#include "Components/GMCOrganicMovementComponent.h"
#include "GameplayTagContainer.h"
#include "GMCMotion.generated.h"

class UAnimMontage;
class UCurveFloat;

// GASP gait enum — values must match the Blueprint E_Gait asset used by Chooser Tables.
UENUM(BlueprintType)
enum class EGMCMotion_Gait : uint8
{
	Walk = 0,
	Run = 1,
	Sprint = 2
};

// GASP rotation mode — values must match the Blueprint E_RotationMode asset.
UENUM(BlueprintType)
enum class EGMCMotion_RotationMode : uint8
{
	VelocityDirection = 0,
	Aiming = 1
};

// GASP 6-way movement direction — values must match the Blueprint E_MovementDirection asset.
// Note: GASP uses 6 directions (not 8). "LR" = Left-Rear, "RR" = Right-Rear.
// The numeric values match the BP enum exactly (F=0, B=1, LR=2, LL=3, RL=4, RR=5).
UENUM(BlueprintType)
enum class EGMCMotion_MovementDirection : uint8
{
	F  = 0,   // Forward
	B  = 1,   // Backward
	LR = 2,   // Left-Rear (backward-left diagonal)
	LL = 3,   // Left (perpendicular left)
	RL = 4,   // Right (perpendicular right)
	RR = 5    // Right-Rear (backward-right diagonal)
};

// GASP stance — values must match the Blueprint E_Stance asset.
UENUM(BlueprintType)
enum class EGMCMotion_Stance : uint8
{
	Standing = 0,
	Crouching = 1
};

// GASP locomotion mode — values must match the Blueprint E_LocomotionMode asset.
UENUM(BlueprintType)
enum class EGMCMotion_LocomotionMode : uint8
{
	Strafing = 0,
	NonStrafing = 1
};

// Bridge struct for BP-local variables that need to reach C++ UPROPERTY members.
// Add new fields here as needed — the Make node in BP will gain a new pin automatically.
USTRUCT(BlueprintType)
struct FGASPBridgeData
{
	GENERATED_BODY()

	// BP's AimingRotation → C++ CachedAimingRotation
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	FRotator AimingRotation = FRotator::ZeroRotator;

	// BP's TurningStrenght → C++ TurningStrength
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	double TurningStrength = 0.0;

	// BP's Trj_TurnAngle → C++ Trj_TurnAngle (BP writes to _0 shadow)
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	float TurnAngle = 0.f;

	// BP's FutureFacingDelta → C++ FutureFacingDelta (BP writes to _0 shadow)
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	float FutureFacingDelta = 0.f;

	// BP's RotationMode (E_RotationMode) → C++ RotationMode (uint8, same byte values)
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	uint8 RotationMode = 0;

	// BP's MovementDirection (E_MovementDirection) → C++ MovementDirection (uint8)
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	uint8 MovementDirection = 0;

	// BP's Gait (E_Gait) → C++ CurrentGait (EGMCMotion_Gait, same byte values)
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	uint8 Gait = 1; // default Run (matches EGMCMotion_Gait::Run)
};

/**
 *
 */
UCLASS(ClassGroup = (GMC), meta = (BlueprintSpawnableComponent))
class GMCMOTION_API UGMCMotion : public UGMC_OrganicMovementCmp
{
	GENERATED_BODY()

public:
	// Dedicated movement mode held for the whole traversal montage, mirroring GASP's traversal state.
	// Custom1 (=4) is GMC's first free custom slot. While in this mode GMC's automatic
	// Airborne<->Grounded switching (UpdateMovementModeStatic) is bypassed, so the pawn never does an
	// Airborne->Grounded transition mid-traversal — which is what fired the jump-landing/fall anim and
	// flip-flopped the capsule size. See PhysicsCustom_Implementation / UpdateMovementModeDynamic_Implementation.
	static constexpr EGMC_MovementMode TraversalMovementMode = EGMC_MovementMode::Custom1;

	// A/B master switch. TRUE = dedicated Custom1 traversal mode (PhysicsCustom, no ProcessLanded) +
	// all the Custom1 vertical compensations (FrontLedge radius inset, jump-over apex cap, jump-over
	// FrontLedge no-hang). FALSE = fall back to stock Airborne (PhysicsAirborne + ProcessLanded does the
	// geometry/landing) with NONE of those compensations — i.e. the pristine pre-Custom1 trajectory, but
	// the landing/fall anim returns. Lets us compare A (Airborne) vs B (Custom1) without deleting work.
	// When false, the BP start SetMovementMode should be Airborne (it is overridden to Custom1 each frame
	// by UpdateMovementModeDynamic only when this is true).
	// DEFAULT false = Option A (Airborne) — chosen after testing: Airborne's native geometry/landing is
	// correct, and the traversal warp + clearances (inset, BackLedge corner clearance, jump-over no-land /
	// stay-airborne-over-obstacle) handle the rest. Custom1 (true) kept for reference / A-B comparison.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bUseTraversalCustomMode = false;

	// Master switch. Set true by BP when a traversal montage starts, false when it ends.
	// Bind this (BindBool) so the server warps the same move the client predicted.
	UPROPERTY(BlueprintReadWrite, Category = "Traversal Warp")
	bool bTraversalWarpActive = false;

	// World-space warp targets, copied from the detection result (S_TraversalCheckResult).
	// Bind these (BindCompressedVector, ClientAuth_Input) so they replicate with the move.
	UPROPERTY(BlueprintReadWrite, Category = "Traversal Warp")
	FVector WarpFrontLedge = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Traversal Warp")
	FVector WarpBackLedge = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "Traversal Warp")
	FVector WarpBackFloor = FVector::ZeroVector;

	// Outward normal of the front ledge face (S_TraversalCheckResult.FrontLedgeNormal), copied from
	// the detection result. The pawn is warped to face -Normal (into the obstacle) during the
	// FrontLedge window. Bind this (BindCompressedVector, ClientAuth_Input) so the server squares up
	// to the same facing the client predicted.
	UPROPERTY(BlueprintReadWrite, Category = "Traversal Warp")
	FVector WarpFrontLedgeNormal = FVector::ZeroVector;

	// Per-axis scale is clamped to this range to avoid extreme/reversed root motion when
	// the obstacle is far from what the animation assumes.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float MinWarpScale = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float MaxWarpScale = 4.0f;

	// Clearance (cm) of the capsule bottom above the obstacle top when targeting the FrontLedge window
	// of a jump-over montage (hurdle). Replaces the hand-plant "attach" offset (which sinks the capsule
	// below the top and pins it against the face). Raise if the pawn still grazes/sticks on the front
	// face; lower if it pops too high before clearing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float JumpFrontLedgeClearance = 10.0f;

	// For jump-over montages (hurdle — identified by having a landing window), follow the animation's
	// authored up-and-forward arc on every axis instead of pulling the capsule straight to the targets.
	// Pulling straight makes the pawn climb the obstacle face in place on the way up and drop onto the
	// obstacle top on the way down (the "running in place"). Reach montages (vault / mantle / climb,
	// no landing window) keep the convergent pull to reach far targets. Set false for the old behaviour.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bArcPreserveJumpArc = true;

	// Floor the convergent warp pace by the remaining-window-time fraction so the capsule keeps
	// progressing toward the target even on frames where the animation holds its root still (the
	// hurdle/vault apex). Prevents the "running in place" hang + abrupt catch-up plunge. Set false
	// to pace purely by animation motion (original behaviour).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bTimeFloorWarp = true;

	// Warp the pawn's yaw to face the obstacle (-FrontLedgeNormal) during the FrontLedge window.
	// Fixes angled/corner approaches that would otherwise carry the pawn off to the side. Set false
	// to fall back to translation-only warp.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bEnableRotationWarp = true;

	// Extra vertical nudge applied on top of the computed warp-point offset (cm, +up).
	// The offset math (see PreProcessRootMotion) should land the pawn correctly on its own;
	// this is a tuning knob to absorb any residual height error without recompiling.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float WarpPointVerticalBias = 0.0f;

	// Clearance (cm) above the obstacle top for the apex of a JUMP-OVER (hurdle/vault). Between the
	// FrontLedge window and the landing window no warp is active, so the raw root motion drives the jump
	// arc — if the anim was authored for a taller obstacle it balloons high above this one. The apex is
	// capped at (highest warp target Z) + this clearance. Only affects jump-overs; reach montages
	// (climb/mantle) are never clamped. Raise if the pawn clips the obstacle top; lower if it floats over.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float GapApexClearance = 25.0f;

	// During a JUMP-OVER (vault/hurdle) in Airborne, any landing surface higher than (BackFloor target Z +
	// this) is treated as the OBSTACLE TOP and rejected as a landing spot, so the pawn vaults OVER it
	// instead of GMC snapping it up onto the obstacle mid-jump (the "capsule monte"). Only the far floor
	// (= BackFloor level) is accepted. Reach montages (climb/mantle) are unaffected — landing on the ledge
	// IS their goal. Raise if the pawn still lands on a low obstacle top; lower if it refuses to land where
	// it should. See IsValidLandingSpot.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float TraversalNoLandHeightAboveFloor = 50.0f;

	// Post-window clearance (cm) for reach/climb/mantle montages. After the FrontLedge warp window ends,
	// the capsule Z is capped at (WarpFrontLedge.Z + HalfHeight + this). Prevents post-window root motion
	// from pushing the character above the ledge surface. Only affects non-jump-over montages.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float ReachPostWindowClearance = 10.0f;

	// FULL vertical clearance (cm) added to the BackLedge (obstacle-top) cross target on a jump-over, mode A.
	// The capsule is rounded, so crossing exactly at the obstacle top wedges its bottom hemisphere on the
	// top corner. This lift rides it clear. Trade-off: 0 = no lift (no "bump" but the capsule can wedge);
	// ~capsule radius = safe no-wedge but a visible small rise above the obstacle. Tune to the SMALLEST
	// value that still clears the corner on your obstacles. See PreProcessRootMotion (BackLedge clearance).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float TraversalCornerClearance = 0.0f;

	// --- Network error tolerances during the traversal ---
	// During a traversal the warped move legitimately diverges between client and server (root motion +
	// latency), which trips GMC's per-variable correction (ActorLocation/LinearVelocity/... "was not valid").
	// While bTraversalWarpActive (+ a short grace), we swap GMC's DefaultErrorTolerances for these wider
	// values, then restore the editor-configured originals. The server STILL validates (bounded widening),
	// so it's anti-cheat-safe — NOT ClientAuth/blind-trust. Tune to your target latency. NOTE: this does NOT
	// fix the Lfoot/Rfoot montage-variant divergence (that's a discrete mismatch, not a tolerance).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Net Tolerance")
	float TraversalActorLocationTolerance = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Net Tolerance")
	float TraversalActorRotationTolerance = 45.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Net Tolerance")
	float TraversalLinearVelocityTolerance = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Net Tolerance")
	float TraversalAngularVelocityTolerance = 200.0f;

	// Keep the widened tolerances this long (s) AFTER the traversal ends. Covers the in-flight client-side
	// validations of the last traversal moves, which are checked ~one RTT later (when the traversal is over
	// and the tolerance would otherwise already be restored). Set ~ your max expected RTT.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Net Tolerance")
	float TraversalToleranceGraceTime = 0.4f;

	// Ignore collision against the obstacle during the traversal. Defaults OFF — and should stay off for
	// predicted (networked) traversal: IgnoreComponentWhenMoving is runtime primitive state, NOT part of
	// the synced GMC move state, so enabling it makes the client replay/resim collide differently than the
	// live frame -> divergence -> correction -> JITTER (confirmed in test). The capsule-penetrates-the-obstacle
	// problem it was meant to fix is solved instead by insetting the FrontLedge warp target out of the wall
	// by the capsule radius (see PreProcessRootMotion) — that keeps collision ON and stays deterministic.
	// Kept only for single-player / non-predicted experiments.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bDisableObstacleCollision = false;

	// How far (cm) to trace toward the FrontLedge target to find the obstacle to ignore. Should exceed
	// the capsule-to-obstacle-face distance at traversal start.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float ObstacleTraceDistance = 150.0f;

	// GASP's traversal montages end with a "Force Blend Out" notify that, in the stock project, calls
	// the engine Montage_Stop to hand control back to locomotion. That call is a no-op on GMC montages
	// (GMC drives them through its MontageTracker, not the AnimInstance), so the montage runs to its
	// full length and the pawn stays frozen/locked for the whole recovery tail. When true, we stop the
	// GMC montage ourselves once its blend-out notify is reached, which lets the BP end check return
	// control immediately — the proper fix for the post-traversal freeze and residual glide.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bEndTraversalAtBlendOut = true;

	// Blend-out time (s) used when we stop the GMC montage at its blend-out notify. Matches the value
	// authored on GASP's TraversalBlendOut notify (0.3).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	float TraversalBlendOutTime = 0.3f;

	// --- Traversal Hand IK ---
	// Computes world-space hand targets from the FrontLedge warp data so the AnimBP
	// can apply Two Bone IK to pin hands to the ledge edge during climb/mantle.

	// Master switch. When false, TraversalHandIKAlpha is always 0 and targets are not computed.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Hand IK")
	bool bEnableTraversalHandIK = true;

	// Horizontal spread (cm) from the ledge center point for left/right hand targets.
	// Larger = hands further apart on the ledge edge. Roughly half shoulder width.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Hand IK", meta = (ClampMin = "0.0"))
	float HandIKSpread = 20.f;

	// Vertical offset (cm) from the ledge top for the hand targets. Negative = fingers
	// drape below the edge. Zero = hands exactly at ledge height.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Hand IK")
	float HandIKVerticalOffset = -2.f;

	// Forward offset (cm) from the ledge edge into the obstacle top surface. Positive =
	// hands further onto the obstacle top (palm flat on surface). Zero = fingertips at edge.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Hand IK", meta = (ClampMin = "0.0"))
	float HandIKDepthOffset = 5.f;

	// Blend alpha for traversal hand IK (0 = off, 1 = fully active). Set to 1 during the
	// FrontLedge warp window (climb/mantle), 0 otherwise. The AnimBP should smooth this
	// with FInterpTo for clean blend-in/out transitions.
	UPROPERTY(BlueprintReadOnly, Category = "Traversal Warp|Hand IK")
	float TraversalHandIKAlpha = 0.f;

	// World-space left hand IK target. Valid only when TraversalHandIKAlpha > 0.
	UPROPERTY(BlueprintReadOnly, Category = "Traversal Warp|Hand IK")
	FVector TraversalHandIK_LeftTarget = FVector::ZeroVector;

	// World-space right hand IK target. Valid only when TraversalHandIKAlpha > 0.
	UPROPERTY(BlueprintReadOnly, Category = "Traversal Warp|Hand IK")
	FVector TraversalHandIK_RightTarget = FVector::ZeroVector;

	// After the last warp window the montage keeps playing its (un-warped) blend-out root motion,
	// which drives the capsule forward with nothing to stop it — a post-traversal glide, worst on
	// the fast "run" variants. When true, root-motion translation is zeroed once we're past the
	// last warp window so grounded braking settles the pawn instead. Set false to keep the original
	// blend-out carry.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bSuppressBlendOutGlide = true;

	// When true, logs the per-window warp math (target, offset, half height, current vs target
	// location) under LogGMCMotion with the "[TWarp]" prefix. Diagnostic only — no behaviour change.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bDebugTraversalWarp = false;

	// True if the traversal montage's name ends with the right-foot suffix ("Rfoot"). Used to read which
	// foot the chooser picked for a given montage (the foot is otherwise emergent from Motion Matching on
	// the local pose history, which diverges client/server -> variant mismatch corrections).
	UFUNCTION(BlueprintPure, Category = "Traversal Warp")
	bool IsTraversalMontageRightFoot(UAnimMontage* Montage) const;

	// Returns the foot-variant of a traversal montage matching bRightFoot, by swapping the "_Lfoot"/"_Rfoot"
	// name suffix and loading the mirror asset. If it already matches (or no suffix / load fails), returns
	// the input unchanged. Lets us force the SAME foot on client and server (deterministic, or driven by a
	// replicated bit) so AnimMontageReference/StartTime/root-motion no longer diverge.
	UFUNCTION(BlueprintCallable, Category = "Traversal Warp")
	UAnimMontage* GetTraversalMontageForFoot(UAnimMontage* Montage, bool bRightFoot) const;

	// Zeroes the pawn's velocity. Call at traversal start (drop entry momentum so it doesn't
	// fight the warp) and end (stop the pawn sliding after the montage). Must be called inside
	// the movement update so it stays predicted.
	UFUNCTION(BlueprintCallable, Category = "Traversal Warp")
	void StopTraversalMovement();

	// Hands the animation's own landing velocity to locomotion at the end of the traversal, instead of
	// zeroing it. The convergent warp eases the horizontal velocity to ~0 as it reaches the landing
	// target, so without this the pawn stops dead on touchdown even though the run animation still
	// carries speed. Sets the horizontal velocity to the captured raw root-motion velocity (the anim's
	// run speed) and keeps the current Z. For montages that end standing (mantle / climb) the captured
	// velocity is ~0, so they are unaffected. Call in place of StopTraversalMovement on a grounded exit.
	UFUNCTION(BlueprintCallable, Category = "Traversal Warp")
	void RestoreTraversalExitVelocity();

	// Diagnostic helper for the Blueprint-side traversal montage arbitration. Logs the client-pending
	// choice, server-detected choice, selected/confirmed choice, and the active GMC montage.
	UFUNCTION(BlueprintCallable, Category = "Traversal Warp|Debug")
	void DebugLogTraversalChoice(
		const FString& Phase,
		bool bDetectionSuccess,
		UAnimMontage* DetectedMontage,
		float DetectedStartTime,
		bool bAcceptPending,
		UAnimMontage* SelectedMontage,
		float SelectedStartTime);

	// =====================================================================================
	//  GASP Pipeline — Hybrid C++/BP Architecture
	// =====================================================================================
	//
	// The BP (BP_GMCMovement) drives two event chains. Some functions are replaced by C++
	// equivalents; others remain BP-driven because the C++ versions caused hitching at
	// direction transitions (A/B tested via per-function toggles).
	//
	//   Event PrePhysicsUpdate (BP chain, inside GMC prediction loop):
	//     Parent::PrePhysicsUpdate → Set TurningStrength
	//     → UpdateRotationMode_CPP          [C++ UFUNCTION, called from BP]
	//     → CalculateRotations_CPP          [C++ UFUNCTION, called from BP]
	//       (replaces IsSimulatedProxy branch + Set AimingRotation + Set OrientationIntent)
	//     → Set AimingRotation = CachedAimingRotation  (bridge for downstream BP consumers)
	//     → [traversal logic]
	//     → UpdateInputAcceleration         [C++ — called from C++ pipeline, NOT BP]
	//     → UpdateControlRotationRate → UpdateGait → Set Gait → Update_Grounded_Physic
	//
	//   C++ pipeline (runs after BP's PrePhysicsUpdate completes):
	//     UpdateInputAcceleration → UpdateRotationMode_CPP
	//     → UpdateGroundedFacing → UpdateTrajectoryMetrics
	//
	//   Event MovementUpdate (BP chain, after movement simulation):
	//     GetMovementDirectionAndOffset     [BP-driven — writes MovementDirection + RotationOffset]
	//     → Update_Grounded                 [BP-driven — writes OverridenDesiredFacing]
	//     → Update_TrajectoryMetrics        [BP-driven]
	//     → Parent::MovementUpdate
	//
	// MovementDirection and CalculateRotations remain BP-driven because the C++ versions
	// introduced hitching at direction transitions (confirmed via A/B testing).
	// CalculateRotations_CPP is still a UFUNCTION — BP calls it from its own chain,
	// giving BP control over execution order relative to other BP-only nodes.
	//
	// Rotation architecture: OverridenDesiredFacing is the raw target (can jump at direction
	// boundaries). ApplyRotation smooths via RotateYawTowardsDirection at 650°/s.
	// The BP's ApplyFacingSpring is dead code — never called.
	//
	// AnimInstance passthrough: UGMCMotion_AnimInstance::UpdateGASPState() reads all GASP
	// variables from UGMCMotion's public UPROPERTY members. Both BP and C++ writers land
	// on the same members, so the AnimInstance always sees current values regardless of
	// which side computed them.
	// =====================================================================================

	// Master switch: when true, the C++ GASP pipeline runs in PrePhysicsUpdate.
	// When false, ALL C++ GASP calls are skipped — Blueprint drives everything.
	// ApplyRotation reads OverridenDesiredFacing regardless and smooths via
	// RotateYawTowardsDirection. Replication bindings remain active either way.
	// When false (default), BP drives the entire GASP pipeline. The C++ pipeline is skipped
	// and replication bindings are handled by the BP's ReplicationGraph instead.
	// The AnimInstance (UGMCMotion_AnimInstance) reads from the C++ UPROPERTY members
	// regardless — BP just needs to write to the inherited properties, not _0 variants.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP")
	bool bEnableGASPPipeline = false;

	// --- Per-function toggles for A/B testing C++ vs BP ---
	// When false, the C++ version is skipped and the BP version should drive.
	// Only checked when bEnableGASPPipeline is true.
	//
	// CalculateRotations and MovementDirection are BP-driven by default (false):
	//   BP calls CalculateRotations_CPP from its PrePhysicsUpdate chain, and
	//   BP's GetMovementDirectionAndOffset handles direction + offset.
	//   The C++ pipeline does NOT call these — BP controls their execution order.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|Debug")
	bool bCPP_GroundedFacing = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|Debug")
	bool bCPP_InputAcceleration = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|Debug")
	bool bCPP_TrajectoryMetrics = true;

	// =====================================================================================
	//  GASP State — replicated via GMC bind system
	//
	//  The following variables are managed in Blueprint (BP_GMCMovement) and bound there:
	//    Gait, Stance, WantsToSprint, WantsToCrouch, WantsToJump
	//  The enums (EGMCMotion_Gait, EGMCMotion_RotationMode, EGMCMotion_MovementDirection)
	//  are still defined above for C++ code that needs them.
	// =====================================================================================

	// No UPROPERTY — hidden from BP to prevent _0 renaming of the BP's own variables.
	// Synced from BP via VariableToAnimBPBridge → FGASPBridgeData.
	EGMCMotion_RotationMode RotationMode = EGMCMotion_RotationMode::VelocityDirection;

	// No UPROPERTY — hidden from BP. Byte values match E_MovementDirection: F=0, B=1, LR=2, LL=3, RL=4, RR=5.
	uint8 MovementDirection = 0;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|Input")
	bool WantsToTraverse = false;

	// Strafing input flag (replicated). Drives RotationMode via UpdateRotationMode().
	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|Input")
	bool WantsToStrafe = false;

	// Cached aiming rotation computed from GetControllerRotation_GMC() with ClampAxis on each
	// component. Written by CalculateRotations_CPP() (skipped for simulated proxies, which use
	// the replicated value). Replaces the BP's own AimingRotation variable when fully embedded.
	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP")
	FRotator CachedAimingRotation = FRotator::ZeroRotator;

	// Rotation offset from the direction-specific curve evaluation (replicated).
	// Written by UpdateMovementDirection(), consumed by UpdateGroundedFacing().
	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP")
	double RotationOffset = 0.0;

	// No UPROPERTY — hidden from BP to prevent _0 renaming. Synced via FGASPBridgeData.
	float FutureFacingDelta = 0.0f;

	// True when the pawn is moving in a sustained turn (replicated).
	// Written by UpdateTrajectoryMetrics().
	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|Trajectory")
	bool Trj_IsCircling = false;

	// No UPROPERTY — hidden from BP to prevent _0 renaming. Synced via FGASPBridgeData.
	// Turning strength metric. Magnitude of the current turning rate.
	double TurningStrength = 0.0;

	// --- Trajectory predictor data (replicated) ---
	// Property names MUST match exactly — the GASP trajectory predictor uses FindPropertyByName().

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|Trajectory")
	FVector CustomComputeInputAcceleration = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|Trajectory")
	FVector OrientationIntent = FVector::ForwardVector;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|Trajectory")
	FRotator OverridenDesiredFacing = FRotator::ZeroRotator;

	// --- Trajectory metrics (computed, replicated for sim proxies) ---

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|Trajectory")
	FVector Trj_FutureVelocity = FVector::ZeroVector;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|Trajectory")
	FVector Trj_NearFutureVelocity = FVector::ZeroVector;

	// No UPROPERTY — hidden from BP to prevent _0 renaming. Synced via FGASPBridgeData.
	float Trj_TurnAngle = 0.0f;

	// No UPROPERTY — hidden from BP. Synced via FGASPBridgeData.
	float AngularVelocityRad = 0.0f;

	// --- Facing spring config ---

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|Facing")
	float FacingSpringHalfLife = 0.1f;

	// --- Facing time config (drives FacingSpringHalfLife by speed in UpdateGroundedFacing) ---

	// IdleFacingTime, MovingFacingTime, SprintFacingTime: IdleFacingTime and SprintFacingTime
	// are hidden from BP reflection (see RotationMode comment). MovingFacingTime has no BP
	// equivalent so it keeps UPROPERTY.
	float IdleFacingTime = 0.2f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "GMCMotion|GASP|Facing")
	float MovingFacingTime = 0.1f;

	float SprintFacingTime = 0.05f;

	// =====================================================================================
	//  Locomotion State Stubs
	//  Previously inherited from UGMCLoco_MovementComponent. Now owned by UGMCMotion
	//  so the AnimInstance and downstream BP code can read them without GMCLocomotion.
	//  Values are populated by the BP (BP_GMCMovement) via its own logic.
	// =====================================================================================

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|Locomotion")
	float ReplicatedSpeed = 0.f;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|Locomotion")
	float ReplicatedLocomotionAngle = 0.f;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|Locomotion")
	EGMCMotion_Gait CurrentGait = EGMCMotion_Gait::Run;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|Locomotion")
	EGMCMotion_Stance CurrentStance = EGMCMotion_Stance::Standing;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|Locomotion")
	EGMCMotion_LocomotionMode LocomotionMode = EGMCMotion_LocomotionMode::NonStrafing;

	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|Locomotion")
	FGameplayTag ActiveMovementTag;

	UPROPERTY(BlueprintReadOnly, Category = "GMCMotion|Locomotion")
	FVector LastLandingVelocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Locomotion")
	float SprintSpeedThreshold = 600.0f;

	// Enable distance matching predictions (stop/pivot). When false, IsStopPredicted
	// and IsPivotPredicted always return false (saves the per-frame kinematic math).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Locomotion")
	bool bPrecalculateDistanceMatches = true;

	// Minimum angle (degrees) between velocity and acceleration for a pivot prediction.
	// Clamped to [90, 179]. Lower = more sensitive pivot detection.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Locomotion", meta = (ClampMin = "90.0", ClampMax = "179.0"))
	float PivotPredictionAngleThreshold = 90.f;

	// Minimum obstacle height (cm) for traversal. Obstacles shorter than this are skipped —
	// BP should trigger a jump ability instead. Set to 0 to disable the guard.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Locomotion", meta = (ClampMin = "0.0"))
	float MinTraversalObstacleHeight = 60.f;

	// Returns true if the obstacle is too short for traversal (height < MinTraversalObstacleHeight).
	// BP calls this from the traversal detection logic before setting WantsToTraverse.
	UFUNCTION(BlueprintPure, Category = "GMCMotion|Locomotion")
	bool IsObstacleTooLowForTraversal(float ObstacleHeight) const;

	// --- Locomotion Getter Stubs ---
	// Sensible defaults; override in BP or replace with real implementations later.

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Locomotion")
	bool IsInputPresent(bool bAllowGrace = false) const;

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Locomotion")
	FRotator GetCurrentAimRotation() const;

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Locomotion")
	float GetCurrentAimYawRate() const;

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Locomotion")
	float GetCurrentComponentYawRate() const;

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Locomotion")
	FVector GetCurrentAnimationAcceleration() const;

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Locomotion")
	bool IsStopPredicted(FVector& OutStopLocation) const;

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Locomotion")
	bool IsPivotPredicted(FVector& OutPivotLocation) const;

	// --- Rotation Offset Curves (per-direction, evaluated at locomotion angle) ---
	// Assign a CurveFloat per movement direction. The curve is evaluated at the locomotion angle
	// (velocity yaw relative to facing, [-180, 180]) and the result offsets the facing target.
	// These match the GASP CHT_RotationOffsetCurve Chooser table rows 1:1:
	//   F → Curve_RotationOffset_F,   B → Curve_RotationOffset_B,
	//   LL → Curve_RotationOffset_LL, LR → Curve_RotationOffset_LR,
	//   RL → Curve_RotationOffset_RL, RR → Curve_RotationOffset_RR

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|RotationOffset")
	UCurveFloat* RotationOffsetCurve_F = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|RotationOffset")
	UCurveFloat* RotationOffsetCurve_B = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|RotationOffset")
	UCurveFloat* RotationOffsetCurve_LL = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|RotationOffset")
	UCurveFloat* RotationOffsetCurve_LR = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|RotationOffset")
	UCurveFloat* RotationOffsetCurve_RL = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|RotationOffset")
	UCurveFloat* RotationOffsetCurve_RR = nullptr;

	// Sliding override: when IsSliding and direction != F, use this curve (matches
	// CHT_RotationOffsetCurve rows 6-7). When direction == F during a slide, uses the F curve.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|RotationOffset")
	UCurveFloat* RotationOffsetCurve_SlideKnees = nullptr;

	// Set true by BP when the pawn is sliding. Drives the rotation offset curve override
	// (Slide_Knees for non-forward directions) matching the Chooser table's movement mode column.
	UPROPERTY(BlueprintReadWrite, Category = "GMCMotion|GASP|RotationOffset")
	bool bIsSliding = false;

	// --- Movement Direction Threshold Mode ---
	// 0 = 4-way (F/B/L/R + diagonals), 1 = 2-way (F/B only), 2 = forward-only
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "GMCMotion|GASP|Direction")
	int32 DirectionThresholdMode = 0;

	// =====================================================================================
	//  GASP Utility Functions
	// =====================================================================================

	// Copies BP-local GASP variables to their C++ UPROPERTY counterparts so that
	// UGMCMotion_AnimInstance::UpdateGASPState() can read them. Call once per frame
	// from BP after the BP pipeline has finished writing its local variables.
	//
	// Uses a struct so new fields can be added without changing the function signature
	// or breaking existing BP call sites. To add a new bridged variable:
	//  1. Add a member to FGASPBridgeData (the Make node in BP gains a new pin)
	//  2. Add the assignment in VariableToAnimBPBridge() (BD.Field → C++ member)
	//  3. Add a FindPropertyByName fallback in TickComponent's IsSimulatedProxy() block
	//     (sim proxies don't run BP gameplay logic, so the bridge call never fires —
	//     the fallback reads the replicated BP variable directly into the C++ member)
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|GASP")
	void VariableToAnimBPBridge(const FGASPBridgeData& BD);

	// Pipeline version: reads RawInputVector from GMC (world-space), normalises, and multiplies
	// by GetInputAcceleration(). Result stored in CustomComputeInputAcceleration.
	// Call this from PrePhysicsUpdate when the pawn provides input via AddMovementInput().
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|GASP")
	void UpdateInputAcceleration();

	// Transforms 2D input axis (raw stick values) into world-space acceleration,
	// relative to the camera. Result stored in CustomComputeInputAcceleration.
	// Use this when providing custom 2D input (e.g., from an Enhanced Input action value).
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category = "GMCMotion|GASP")
	void ComputeCustomInputAcceleration(const FVector2D& InputAxis);

	// --- ApplyFacingSpring (UNUSED) ---
	//
	// Critically-damped yaw spring. NOT called — the reference project's ApplyFacingSpring
	// is dead code too. Rotation smoothing is handled by RotateYawTowardsDirection in
	// ApplyRotation, and direction continuity comes from the RotationOffset curves.
	// Kept for potential future use.
	void ApplyFacingSpring(float TargetYaw, float DeltaSeconds);

	// --- DEFERRED: UpdateTrajectoryMetrics ---
	//
	// Runs in the MovementUpdate chain (same chain as the other deferred functions).
	// The BP's Update_TrajectoryMetrics (18 nodes) is a SIMPLIFIED version — not empty:
	//
	//   - FutureVelocity / NearFutureVelocity = raw GetVelocity() (NO projection).
	//     C++ projects velocity forward at 0.5s / 0.1s horizons with accel, clamped to MaxSpeed.
	//   - TurnAngle = Abs(DeltaYaw(VelocityRot, OrientationIntentRot)) — always positive.
	//     C++ is signed, uses projected future velocity vs OverridenDesiredFacing.
	//   - FutureFacingDelta = DeltaYaw(ActorRotation, OrientationIntentRot) — actor vs intent.
	//     C++ computes OverridenDesiredFacing vs FutureVelocity (same as TurnAngle).
	//   - IsCircling = hardcoded false (input pin disconnected). C++ uses speed+angle check.
	//   - TurningStrength is NOT computed here — set separately in PrePhysicsUpdate EventGraph.
	//
	// Trj_TurnAngle and FutureFacingDelta have no UPROPERTY (hidden to avoid _0 renaming).
	// BP writes to BP-local variables with these names; values reach C++ via FGASPBridgeData.
	//
	// Deferred to embedding phase with the rest of the MovementUpdate chain.
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|GASP")
	void UpdateTrajectoryMetrics();

	// C++ version of direction quantization + rotation offset curve evaluation.
	// NOT called from the C++ pipeline — BP's GetMovementDirectionAndOffset drives
	// MovementDirection and RotationOffset instead (C++ version caused hitching).
	// Kept for reference / future use.
	void UpdateMovementDirection();

	// Maps WantsToStrafe → RotationMode (Aiming / VelocityDirection).
	// _CPP suffix avoids name conflict with the BP's own UpdateRotationMode function.
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|GASP")
	void UpdateRotationMode_CPP();

	// Sets OverridenDesiredFacing directly to IntentYaw + clamped RotationOffset.
	// Matching the BP's Update_Grounded, which sets the raw target without any spring.
	// ApplyRotation then smooths via RotateYawTowardsDirection.
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|GASP")
	void UpdateGroundedFacing(float DeltaSeconds);

	// Computes CachedAimingRotation (from controller rotation, skipped for sim proxies) and
	// OrientationIntent (from movement mode, rotation mode, input state, and aiming rotation).
	// Combines the BP's AimingRotation computation + IsSimulatedProxy branch + GetOrientationIntent
	// into a single call. _CPP suffix avoids name conflict with BP functions.
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|GASP")
	void CalculateRotations_CPP();

	// Returns the orientation intent vector (flat): camera forward when Aiming, velocity when VelocityDirection.
	// UFUNCTION removed: the reference BP has its own GetOrientationIntent. Re-add when BP version is removed.
	FVector GetOrientationIntent() const;

	// Returns the target orientation as a rotator: camera rotation when Aiming, velocity rotation when VelocityDirection.
	UFUNCTION(BlueprintPure, Category = "GMCMotion|GASP")
	FRotator GetTargetOrientation() const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	// Runs the GASP pipeline (input acceleration, rotation mode, facing, direction, trajectory)
	// AFTER the BP's PrePhysicsUpdate event completes (Jump, UpdateGait, SetGait, Update_Grounded_Physic).
	virtual void PrePhysicsUpdate_Implementation(float DeltaSeconds) override;

	// Computes ReplicatedSpeed, ReplicatedLocomotionAngle, and captures LastLandingVelocity.
	// Runs in the GMC prediction loop so these values participate in replication.
	virtual void MovementUpdate_Implementation(float DeltaSeconds) override;

	// Overrides base rotation to use the GASP facing spring output (OverridenDesiredFacing)
	// instead of the base class's default rotation logic.
	virtual void ApplyRotation(bool bIsDirectBotMove, const FGMC_RootMotionVelocitySettings& RootMotionMetaData, float DeltaSeconds) override;

	// GMC replication data binding — registers GASP variables for network sync.
	virtual void BindReplicationData_Implementation() override;
	// Physics for the dedicated traversal mode (Custom1). Mirrors the base PhysicsAirborne
	// (CalculateVelocity + trapezoidal integration of the root-motion velocity) but routes the move
	// through MoveThroughAirNoLanding so a brushed walkable surface never triggers ProcessLanded /
	// a flip to Grounded. The warp re-converges from the live location each frame, so per-frame
	// slide-and-resolve is all we need. Gravity: ApplyExternalForces only adds it when IsAirborne()
	// (false here) AND skips it under root motion anyway, so the montage drives Z exactly as before.
	virtual void PhysicsCustom_Implementation(float DeltaSeconds) override;

	// While a traversal is active, force/hold TraversalMovementMode and return true so GMC skips
	// UpdateMovementModeStatic (no auto Airborne<->Grounded, no ProcessLanded). Otherwise defer to base.
	virtual bool UpdateMovementModeDynamic_Implementation(UPARAM(Ref) FGMC_FloorParams& Floor, float DeltaSeconds) override;

	// Reject the obstacle TOP as a landing spot during a jump-over traversal (so the vault passes over it
	// instead of GMC grounding the pawn onto the obstacle mid-jump). Only relevant in Airborne (mode A);
	// in Custom1 the base landing path isn't reached. See TraversalNoLandHeightAboveFloor.
	virtual bool IsValidLandingSpot(const FHitResult& Hit) override;

	// Swaps GMC's DefaultErrorTolerances to the Traversal* variants while traversing (+ grace), restores the
	// editor-configured originals after. Called every frame from UpdateMovementModeDynamic (before the move /
	// validation). See the Traversal*Tolerance properties.
	void UpdateTraversalErrorTolerances(float DeltaSeconds);

	// Editor-configured error tolerances captured once (first call), restored when the traversal ends.
	float SavedActorLocationTolerance = 0.0f;
	float SavedActorRotationTolerance = 0.0f;
	float SavedLinearVelocityTolerance = 0.0f;
	float SavedAngularVelocityTolerance = 0.0f;
	bool bTraversalToleranceDefaultsSaved = false;
	bool bTraversalToleranceWidened = false;
	float TraversalToleranceGraceRemaining = 0.0f;

	// Copy of the base MoveThroughAir collision/slide resolution (SafeMove -> HandleImpact ->
	// AdjustVelocityFromHitAirborne -> ComputeSlideVector -> TwoWallAdjust -> side-step) with EVERY
	// IsValidLandingSpot/ProcessLanded branch removed. Treats all blocking hits as walls to slide
	// along, never landing. Not reusing MoveThroughAir directly because it asserts gmc_ck(IsAirborne()).
	bool MoveThroughAirNoLanding(const FVector& LocationDelta, float DeltaSeconds);

	virtual void PreProcessRootMotion(
		const FGMC_AnimMontageInstance& MontageInstance,
		FRootMotionMovementParams& InOutRootMotionParams,
		float MontageDelta,
		float DeltaSeconds) override;

	// Stops the active traversal montage through the GMC montage system once it reaches its blend-out
	// notify, so control returns to locomotion instead of waiting out the full recovery tail.
	virtual void PostMovementUpdate_Implementation(float DeltaSeconds) override;

	// Adds the obstacle to / removes it from the capsule's move-ignore list as the traversal starts and
	// ends, so GMC's collision sweeps pass through it while the warp drives the arc. See bDisableObstacleCollision.
	void UpdateTraversalCollisionIgnore();

	// The obstacle component currently ignored for movement (valid only during a traversal). Weak so a
	// streamed-out / destroyed obstacle doesn't dangle.
	TWeakObjectPtr<class UPrimitiveComponent> IgnoredObstacle;

	// The raw (un-warped) animation horizontal velocity from the most recent traversal frame, captured
	// in PreProcessRootMotion and handed to locomotion by RestoreTraversalExitVelocity on landing.
	FVector TraversalExitVelocity = FVector::ZeroVector;

	// True if the active traversal montage is a jump-over (vault / hurdle, has a BackLedge/BackFloor landing
	// window) rather than a reach (mantle / climb, FrontLedge only, ends standing). Captured during the warp
	// in PreProcessRootMotion. Exposed so the BP exit logic can decide whether to inherit the run momentum
	// (jump-over) or settle (reach).
	UPROPERTY(BlueprintReadOnly, Category = "Traversal Warp")
	bool bLastTraversalIsJumpOver = false;

	// Finds the active MotionWarping window at MontagePosition and resolves its target.
	// OutWarpPointBone is the modifier's WarpPointAnimBoneName when its provider is Bone
	// (GASP's "attach" bone on the FrontLedge window), or NAME_None when the provider is None
	// (BackLedge / BackFloor) — in which case the stored target already is the root target.
	bool GetActiveWarpTarget(UAnimMontage* Montage, float Position, FVector& OutTarget, float& OutWindowEndTime, FName& OutWarpPointBone, FName& OutWarpTargetName) const;

	// Trigger time of the montage's traversal blend-out notify (BP_NotifyState_MontageBlendOut), or 0
	// if it has none. This is where GASP wants the montage to blend out and hand control back.
	float GetTraversalBlendOutTime(UAnimMontage* Montage) const;

	// End time of the last *landing* warp window (BackLedge / BackFloor) in the montage, or 0 if it
	// has none. Vault/hurdle land behind the obstacle, so past this their remaining root motion is
	// just recovery (the glide). Mantle/climb have only a FrontLedge window — their post-window root
	// motion is the pull-up/stand and must be kept — so they return 0 and are never suppressed.
	float GetLastLandingWindowEnd(UAnimMontage* Montage) const;

	// Highest capsule-center warp target Z across all the montage's warp windows (= the obstacle top),
	// in the same corrected frame as the active-window targets. Used to cap the jump apex in the
	// un-warped gap of a jump-over so the raw root motion can't fly the capsule far above the obstacle.
	float GetMaxWarpCorrectedZ(UAnimMontage* Montage, float HalfHeight) const;

	// Maps a montage warp-target name (FrontLedge / BackLedge / BackFloor) to a stored target.
	bool GetTargetByName(FName Name, FVector& OutTarget) const;

	// Component-space offset (root - WarpPointBone) evaluated at the window's end time. Mirrors
	// what the engine's SkewWarp does via CalculateRootTransformRelativeToWarpPointAtTime, but
	// sourced from the montage's skeleton (not the live mesh) so it is identical on client and
	// server. Only the Z component is used for the vertical correction today; X/Y are returned
	// for diagnostics (and a future full-offset/rotation warp). Zero if bone/skeleton unresolved.
	FVector ComputeRootMinusBoneOffset(UAnimMontage* Montage, float Time, FName BoneName) const;

private:
	// Returns the movement direction for the given locomotion angle based on DirectionThresholdMode and RotationMode.
	EGMCMotion_MovementDirection GetDirectionFromAngle(float Angle) const;

	// Returns the rotation offset curve asset for the given direction.
	UCurveFloat* GetRotationOffsetCurveForDirection(EGMCMotion_MovementDirection Dir) const;

	// True after the first GASP pipeline tick initializes OverridenDesiredFacing
	// from the actor's current rotation. Prevents a snap to world-forward on spawn.
	bool bFacingInitialized = false;

	// --- Locomotion helper tracking (frame-over-frame deltas) ---
	float PreviousComponentYaw = 0.f;
	float PreviousAimYaw = 0.f;
	FVector PreviousAnimVelocity = FVector::ZeroVector;
	float CachedComponentYawRate = 0.f;
	float CachedAimYawRate = 0.f;
	FVector CachedAnimAcceleration = FVector::ZeroVector;
	bool bPreviousWasAirborne = false;

	// Computes frame-over-frame yaw rates and animation acceleration.
	// Called from TickComponent — visual/animation only, no prediction needed.
	void UpdateLocomotionHelpers(float DeltaTime);

	// True after the rotation offset curve setup has been validated (one-time check).
	bool bRotationOffsetCurvesValidated = false;

	// Velocity state for ApplyFacingSpring (unused — kept for potential future use).
	float FacingSpringVelocityYaw = 0.f;

	// GMC bind indices for GASP variables (only those owned by C++).
	int32 BI_RotationMode = -1;
	int32 BI_MovementDirection = -1;
	int32 BI_WantsToTraverse = -1;
	int32 BI_WantsToStrafe = -1;
	int32 BI_CachedAimingRotation = -1;
	int32 BI_RotationOffset = -1;
	int32 BI_FutureFacingDelta = -1;
	int32 BI_Trj_IsCircling = -1;
	int32 BI_TurningStrength = -1;
	int32 BI_CustomComputeInputAcceleration = -1;
	int32 BI_OrientationIntent = -1;
	int32 BI_OverridenDesiredFacing = -1;
	int32 BI_Trj_FutureVelocity = -1;
	int32 BI_Trj_NearFutureVelocity = -1;
	int32 BI_Trj_TurnAngle = -1;
	int32 BI_AngularVelocityRad = -1;
};
