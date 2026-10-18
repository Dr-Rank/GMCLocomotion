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

class AActor;
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
	Crouching = 1,
	Crawling = 2,
	Swimming = 3,
	SwimmingIdle = 4,
	SwimmingMoving = 5,
	PrimalCrawl = 6
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

	// BP's Stance (E_Stance) → C++ CurrentStance (EGMCMotion_Stance, same byte values)
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	uint8 Stance = 0; // default Standing (matches EGMCMotion_Stance::Standing)

	// BP's Downed → C++ Downed. Server-owned gameplay state (health/GMAS drives it), exposed to
	// the AnimBP so motion matching can select the downed-crawl set instead of normal crawl.
	UPROPERTY(BlueprintReadWrite, Category = "GASP")
	bool Downed = false;
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

	// Extra horizontal push (cm) applied to reach/climb/mantle FrontLedge warp targets, moving the
	// target INWARD past the ledge edge (opposite to the face normal). The radius inset positions the
	// capsule AT the ledge edge; this push moves it further onto the obstacle top so the pawn ends up
	// standing on it rather than balanced on the edge. Only affects reach montages, not jump-overs.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp", meta = (ClampMin = "0.0"))
	float ReachLedgePush = 20.0f;

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

	// Before the first warp window of a jump-over, keep the animation's run-up root motion, capped so the capsule stops
	// short of the obstacle's front face (radius + PreWindowFaceClearance). The GASP "run" hurdles start around 0.28 s
	// and open their FrontLedge window around 0.51 s. False = old behaviour: horizontal root motion zeroed until the
	// window opens, which froze the capsule ~0.23 s at traversal start, then the warp caught up at ~1000 cm/s.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bKeepPreWindowRunUp = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp", meta = (ClampMin = "0.0"))
	float PreWindowFaceClearance = 5.0f;

	// Pre-window run-up paced for the whole approach: the horizontal scale is (distance to the first warp window's target)
	// / (anim run-up until that window ends), so the speed does not step when the window opens (was 581 -> 270 cm/s in
	// one frame on a late press). Recomputed every frame (convergent, up to PreWindowMaxScale). False = run-up at anim
	// speed, only stopped at the face.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bPreWindowMatchFirstWindow = true;

	// Ceiling of that pre-window pace. Above 1 it only makes up the distance lost to the entry blend: stretching the flat
	// run-up for a far obstacle (740 cm/s measured) brought the capsule to the face before the jump rose, and it stalled
	// against the wall; the rest of the stretch is left to the window (rising part).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp", meta = (ClampMin = "1.0"))
	float PreWindowMaxScale = 1.1f;

	// Over this much montage time after the traversal starts, the pre-window horizontal speed goes from the speed the
	// pawn entered with (TraversalEntrySpeed) to the run-up speed, instead of jumping to the anim's (~600 cm/s from a
	// 375 cm/s run; GASP58 ramps ~375 -> 490). 0 = off.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp", meta = (ClampMin = "0.0"))
	float TraversalEntrySpeedBlendTime = 0.25f;

	// After a jump-over's warp window closes, the horizontal speed goes from the window's last warped speed to the
	// animation's over this much montage time (a late press compresses the FrontLedge window to ~240 cm/s, the raw anim
	// then resumed at ~535 in one frame). 0 = off.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp", meta = (ClampMin = "0.0"))
	float WarpWindowExitBlendTime = 0.12f;

	// BP_GMCMovement calls StopTraversalMovement at traversal START (right after the montage is played). Zeroing the
	// velocity there makes the first traversal move stand still (Custom1 / Airborne integrate the previous velocity;
	// the montage root motion only takes over on the next move): a one-frame freeze. False = keep the run velocity.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bZeroVelocityAtTraversalStart = false;

	// Floor of the horizontal warp scale in the landing (BackFloor) window of a jump-over. BackFloor is a floor point
	// just behind the obstacle, often only 25-45 cm ahead when the window opens while the animation still runs ~100 cm:
	// the convergent warp slowed the capsule to 30-45 % of the anim speed, then to 0 once the point was reached (the
	// capsule dropped vertically for ~0.1 s, then RestoreTraversalExitVelocity relaunched it). 1 = never slower than
	// the animation (lands a little past the point, like GASP58), 0 = old behaviour.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp", meta = (ClampMin = "0.0"))
	float HurdleLandingMinWarpScale = 1.0f;

	// After a jump-over, RestoreTraversalExitVelocity hands the anim's landing speed (~520-550 cm/s) to locomotion, above
	// the run max (375): GMC's LimitSpeed then dropped it to max in 1-2 frames (OverMaxSpeedDecelerationGrounded 10000),
	// a visible speed sawtooth between chained hurdles. While that carry lasts (until back at max speed, input released
	// or turned away, or the next traversal) the over-speed deceleration is this value instead (GASP58 decays
	// ~510 -> 375 in ~0.4 s). 0 = off (old behaviour).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp", meta = (ClampMin = "0.0"))
	float TraversalExitOverSpeedDeceleration = 400.0f;

	// The over-speed carry above is only meant for a jump-over with the input still along the run. Anything else (mantle,
	// or input released / turned away) gets the anim's exit speed capped at the current max speed: the GASP run mantles
	// (Mantle_*_run) root-move at a constant 517 cm/s, for a 375 run max, and with the input released the braking alone
	// took ~0.85 s / 180 cm to stop (slid across a 2.8 m platform, user report 2026-10-01). false = old behaviour.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bCapTraversalExitSpeed = true;

	// A traversal that ends in the air (vault over a drop) hands the anim's downward speed to the fall instead of Z = 0,
	// which restarted the fall from rest (a hang/bump in mid-air). false = old behaviour.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bKeepTraversalExitFallSpeed = true;

	// Air catches (GASP "Catch" montages: mantle / cliff / hurdle / vault grabbed while falling) start inside their
	// FrontLedge window (~0.1 s left) with the body hanging out from the wall. Their horizontal FrontLedge target follows
	// the animation's root -> warp-bone offset at window end (like GASP's motion warping), at least the capsule radius
	// out from the face, with no ReachLedgePush. False = old behaviour: radius inset + push like a standing mantle, which
	// threw the capsule at the wall at 600-1350 cm/s on the first catch frames.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bCatchWarpUsesAnimOffset = true;

	// Same horizontal target for the ground reach montages (mantle / vault / climb): at the FrontLedge window end the GASP
	// anims still have the root ~50 cm out from the wall (hands on the ledge). The radius inset + ReachLedgePush target
	// made the capsule cover ~180 cm instead of the anim's ~128 in the 0.24 s window: 375 -> 765 cm/s on a run mantle,
	// 894 on a run vault (user report 2026-10-01). False = old behaviour.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bReachWarpUsesAnimOffset = true;

	// True for the GASP air-catch traversal montages (name contains "Catch").
	UFUNCTION(BlueprintPure, Category = "Traversal Warp")
	static bool IsCatchTraversalMontage(const UAnimMontage* Montage);

	// Ground reach montages (mantle / vault / climb, not jump-overs or catches): the chooser's pose search picks the start
	// frame mostly on pose / trajectory, not on the distance to the ledge. With the 5.8 Relaxed montages it picked the end
	// of the searchable range (ledge expected at 137 cm, real 199): 62 cm to make up in the 0.12 s before the FrontLedge
	// window = 951 cm/s (2026-10-01). The start is moved inside the montage's searchable range (PoseSearchSamplingRange,
	// else PoseSearchBranchIn) so that the anim's run-up to the first FrontLedge window end covers the real distance to the
	// warp target at TraversalStartTimeWarpScale. false = keep the chooser's start time.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bAdjustTraversalStartTimeByDistance = true;

	// Warp scale aimed at by the start-time adjustment: < 1 starts a bit early so the warp slows the run-up down (GASP58
	// errs that way: ~0.67), > 1 would speed it up.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp", meta = (ClampMin = "0.3", ClampMax = "1.5"))
	float TraversalStartTimeWarpScale = 0.9f;

	// Start time to play Montage at (see bAdjustTraversalStartTimeByDistance), for the pawn owning Context's movement
	// component. Returns StartTime unchanged when not applicable. Called from TryTraversalAction right after the chooser.
	UFUNCTION(BlueprintPure, Category = "Traversal Warp", meta = (DefaultToSelf = "Context"))
	static float AdjustTraversalStartTimeForDistance(const UObject* Context, UObject* Montage, float StartTime,
		FVector FrontLedgeLocation, FVector FrontLedgeNormal);

	// ─── Equipment-Gated Traversal ──────────────────────────────────────────────
	//
	// When the game mode requires holster-before-traverse, the equipment system sets
	// bEquipmentBlocksTraversal = true whenever a weapon is in-hand or mid-transition.
	// ShouldBlockTraversalForEquipment() checks the flag + the exempt-montage set.
	// The BP calls this alongside IsObstacleTooLowForTraversal in its detection logic,
	// and PrePhysicsUpdate has a safety net that undoes a traversal start if the BP
	// missed the check.

	// Set by the equipment system. True when a weapon is in-hand or mid-transition
	// AND the game mode requires holster before traversal.
	UPROPERTY(BlueprintReadWrite, Category = "Traversal Warp|Equipment Gate")
	bool bEquipmentBlocksTraversal = false;

	// Set by EquipmentManager::TraversalHolster when the incoming traversal montage is
	// in the skip-holster set. Tells the equipment gate in UpdateMovementModeDynamic to
	// allow this traversal even though a weapon is in hand. Cleared on trailing edge
	// (bTraversalWarpActive going true→false) so it doesn't leak to the next traversal.
	UPROPERTY(BlueprintReadWrite, Category = "Traversal Warp|Equipment Gate")
	bool bTraversalExemptFromEquipmentGate = false;

	// Tracks previous bTraversalWarpActive for trailing-edge detection in the equipment
	// gate. When traversal ends (true→false), the exempt flag is cleared so it doesn't
	// leak to the next traversal attempt.
	bool bPrevTraversalWarpActive_Gate = false;

	// Traversal montages exempt from the equipment block (hurdles, short vaults that
	// allow keeping the weapon in hand). Populated by the equipment system at BeginPlay
	// from its own TraversalMontagesSkipHolster set.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp|Equipment Gate")
	TSet<TObjectPtr<UAnimMontage>> TraversalMontagesAllowedWithWeapon;

	// Returns true if the given traversal montage should be blocked by equipment state.
	// False = traversal allowed. True = weapon must be holstered first.
	UFUNCTION(BlueprintPure, Category = "Traversal Warp|Equipment Gate")
	bool ShouldBlockTraversalForEquipment(UAnimMontage* TraversalMontage = nullptr) const;

	// Static version callable from any BP without needing a Cast to GMCMotion.
	// Finds the UGMCMotion component on OwnerActor and delegates to ShouldBlockTraversalForEquipment.
	// Returns false (allow traversal) if OwnerActor is null or has no GMCMotion component.
	UFUNCTION(BlueprintPure, Category = "Traversal Warp|Equipment Gate", meta = (DefaultToSelf = "OwnerActor"))
	static bool IsTraversalBlockedByEquipment(AActor* OwnerActor, UAnimMontage* TraversalMontage = nullptr);

	// ─────────────────────────────────────────────────────────────────────────────

	// When true, logs the per-window warp math (target, offset, half height, current vs target
	// location) under LogGMCMotion with the "[TWarp]" prefix. Diagnostic only — no behaviour change.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Traversal Warp")
	bool bDebugTraversalWarp = false;

	// Rising-edge latch for the once-per-climb "[TSTART]" summary line. Set when the FrontLedge reach
	// window first opens, cleared whenever no warp window is active. Diagnostic only; not replicated.
	bool bTStartLatched = false;


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

	// Zeroes the pawn's velocity when bZeroVelocityAtTraversalStart, else does nothing (only logs). Called by
	// BP_GMCMovement at traversal start; the root motion replaces the velocity from the next move anyway, so zeroing
	// only froze the first move. Must be called inside the movement update so it stays predicted.
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
	// Rotation architecture: UpdateGroundedFacing writes OverridenDesiredFacing directly to the
	// target (zero-lag tracking). ApplyRotation then calls RotateYawTowardsDirection at
	// RotationRate — which BP_GMCMovement sets to 0, and a rate of 0 in GMC means an INSTANT
	// SNAP, not a rate limit. So the capsule reaches its target facing every frame, and nothing
	// smooths it. (An earlier version of this comment claimed 650°/s; that was never the case
	// with RotationRate at 0.)
	// The BP's ApplyFacingSpring is disconnected from MovementUpdate; the C++ one is reachable
	// only via the sprint reversal hold, which itself cannot run while RotationRate is 0.
	//
	// AnimInstance passthrough: UGMCMotion_AnimInstance::UpdateGASPState() reads all GASP
	// variables from UGMCMotion's public UPROPERTY members. Both BP and C++ writers land
	// on the same members, so the AnimInstance always sees current values regardless of
	// which side computed them.
	// =====================================================================================

	// The GASP pipeline is always on.
	//
	// It used to be switchable via bEnableGASPPipeline, with per-function bCPP_* toggles for
	// A/B testing C++ against BP during the migration. Both are gone, for a correctness reason
	// rather than tidiness: bEnableGASPPipeline also gated the whole body of
	// BindReplicationData_Implementation, and GMC bind registration defines the network channel
	// layout. Because the flag was EditAnywhere it could differ between pawn types (the CDO read
	// false while BP_GMC_Pawn's component instance overrode it to true), and because it was
	// BlueprintReadWrite it could be flipped at runtime — after bindings had already registered.
	// Either way the client and server binding layouts diverge, and that does not fail loudly;
	// it silently corrupts replication.
	//
	// CalculateRotations_CPP and MovementDirection remain BP-driven: BP calls
	// CalculateRotations_CPP from its own PrePhysicsUpdate chain, and BP's
	// GetMovementDirectionAndOffset owns direction + rotation offset.

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

	// --- Bridge members (Tools/manage_bridge_variables.py inserts below this line) ---

	// True while the pawn is in the downed (crawling, awaiting revive) state.
	//
	// Written from BP via VariableToAnimBPBridge → FGASPBridgeData on authority/autonomous, and
	// GMC-bound below (BI_Downed) so simulated proxies receive it too. When the GASP pipeline is
	// off there is no binding, and TickComponent falls back to reading BP's own "Downed" instead.
	//
	// NO UPROPERTY, and that is load-bearing: BP_GMCMovement already owns a bool named "Downed".
	// Adding a UPROPERTY of the same name here would rename the BP's variable to "Downed_0" and
	// silently detach every existing BP node that reads or writes it.
	bool Downed = false;

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
	// Written by BP's GetMovementDirectionAndOffset, consumed by UpdateGroundedFacing() —
	// though only when bDisableFacingRotationOffset is false, which it is not by default.
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

	// When true, RotationOffset is NOT added to OverridenDesiredFacing in UpdateGroundedFacing.
	// The capsule faces pure aim direction (IntentYaw only). Enable this when weapon animation
	// layers (e.g., SKG's Layered Blend Per Bone) replace the upper body — since LBPB discards
	// the Orientation Warping correction applied inside BlendStack, the upper body inherits the
	// capsule's orientation directly. Without this flag, the capsule is shifted toward movement
	// by RotationOffset, causing the weapon/upper body to face movement direction instead of aim.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|GASP|Facing")
	bool bDisableFacingRotationOffset = true;

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

	// --- Sprint forward cone ---
	//
	// Sprint animation coverage is a forward cone only. Stop/plant animations exist for
	// F/FL/FR at sprint, against all ten directions (F FL FR L R B BL BR + lateral variants) at
	// walk and run. Allowing sprint outside that cone asks the chooser for animations that do not
	// exist: it returns no valid anim, the transition state blends straight through to the loop,
	// and motion matching falls back to whatever is nearest — which reads as the feet failing to
	// plant and snapping between directions.
	//
	// CanSprintInCurrentDirection() reports whether the pawn's movement intent lies inside the
	// cone. Gait selection should refuse Sprint when it returns false, dropping to Run — which has
	// full directional coverage and plants correctly. Gait drives speed, so the pawn also slows to
	// RunSpeed rather than running run animations at sprint speed.

	// Master switch for the forward-cone restriction. Turn OFF only if your animation set has
	// strafe sprint coverage; with the stock GASP set it must stay ON.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Locomotion|Sprint",
		meta = (ToolTip = "Restrict sprinting to a forward cone.\n\nNOTE: GASP does not natively include strafe sprint anims - sprint stops exist only for F/FL/FR, while walk and run have all ten directions. Keep this ON if you are using those, otherwise sprinting sideways asks the chooser for animations that do not exist: it returns no valid anim, the transition state blends straight through, and the feet fail to plant and snap between directions.\n\nTurn OFF only once your set includes lateral and rear sprint coverage."))
	bool bRestrictSprintToForwardCone = true;

	// Half-angle (degrees) between input direction and orientation intent within which sprint is
	// permitted. Roughly matches the FL..FR spread of the authored sprint set.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Locomotion|Sprint", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float SprintForwardConeAngle = 50.f;

	// Extra tolerance (degrees) applied while already sprinting, so hovering exactly on the cone
	// boundary cannot flip the gait every frame. Gait flapping is itself an animation-selection
	// bug, so this is deliberately not zero.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Locomotion|Sprint", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float SprintForwardConeHysteresis = 15.f;

	// True when movement intent is inside the sprint cone. Call this from gait selection and
	// refuse Sprint when it is false.
	//
	// Returns true when there is no movement input, so starting a sprint from standing is never
	// blocked. In VelocityDirection mode the pawn turns to face its input, so OrientationIntent
	// tracks the input and this naturally always passes — the cone only constrains strafing.
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "GMCMotion|Locomotion|Sprint")
	bool CanSprintInCurrentDirection() const;

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
	// Critically-damped yaw spring.
	//
	// There IS a call site — UpdateGroundedFacing calls this during the post-reversal settling
	// window. But that window is entered only when the sprint reversal hold releases, and the
	// hold requires RotationRate > 0 while BP_GMCMovement sets it to 0. So in the current
	// configuration this is unreachable in practice, though not dead by inspection.
	// (An earlier comment here stated it was never called, which sent a debugging session
	// looking in the wrong place — the call at UpdateGroundedFacing is real.)
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

	// =====================================================================================
	//  Stance collision (stand / crouch)
	// =====================================================================================

	// Drives the root collision toward the shape of DesiredStance (E_Stance byte: 0 Stand, 1 Crouch) and returns the
	// stance the collision allows, to be written to the BP's bound Stance. Lerps the half height of the vertical capsule.
	// Standing up only starts when a vertical capsule of the stand height fits (ECC_Pawn: attached actors ignore that
	// channel), otherwise the crouch is kept. The returned stance switches as soon as the change is allowed, so the
	// animation transition plays while the collision interpolates. bCollisionChanged is true when the collision moved
	// this call (CL_DoNotCombineNextMove). While slope sliding the capsule lerps to CrouchHalfHeight (bottom kept on the
	// slope) whatever DesiredStance is.
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|Stance")
	uint8 UpdateStanceCollision(uint8 DesiredStance, float StandHalfHeight, float CrouchHalfHeight, float InterpSpeed,
		float DeltaTime, bool bAdjustPosition, bool& bCollisionChanged);

	// Instantly restores a vertical capsule of StandHalfHeight (current radius kept), e.g. when leaving the ground while
	// crouched. Returns true if the collision changed. Does nothing while slope sliding (see UpdateStanceCollision).
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|Stance")
	bool ResetStanceCollision(float StandHalfHeight);

	// True when a vertical capsule of TargetHalfHeight (current radius) fits with its bottom at the current collision bottom.
	UFUNCTION(BlueprintPure, Category = "GMCMotion|Stance")
	bool HasRoomForVerticalCapsule(float TargetHalfHeight) const;

	// True while the last UpdateStanceCollision call still moved the collision (stance transition in progress).
	UFUNCTION(BlueprintPure, Category = "GMCMotion|Stance")
	bool IsStanceCollisionChanging() const { return bStanceCollisionChanging; }

	// =====================================================================================
	//  Steep slope slide: involuntary slide down floors steeper than SlopeSlideStartAngle
	// =====================================================================================
	// GMC's grounded movement has no downhill force (the pawn can stand still on a 44 deg slope), and above
	// WalkableFloorAngle it turns Airborne and falls along the slope. From SlopeSlideStartAngle this mode takes over
	// instead: gravity along the floor plane minus friction, limited lateral steering, the pawn turned to the slide
	// direction and kept on any surface (walkable or not). It ends under SlopeSlideStopAngle (GMC's static mode update
	// then grounds the pawn) or when the floor is lost (Airborne). Decided in UpdateMovementModeDynamic from the move's
	// floor and GMC state only, so prediction, replays and simulated proxies agree; GMC replicates the movement mode.
	static constexpr EGMC_MovementMode SlopeSlideMovementMode = EGMC_MovementMode::Custom2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide")
	bool bSlopeSlideEnabled = true;

	// Floor angle (deg) from which the pawn starts sliding. Keep it under WalkableFloorAngle (45) to slide before GMC
	// makes the pawn fall.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0", ClampMax = "89"))
	float SlopeSlideStartAngle = 35.f;

	// Floor angle (deg) under which the slide ends, once the speed is back under SlopeSlideExitSpeed. Lower than the
	// start angle so the pawn doesn't flicker between walking and sliding around the threshold.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0", ClampMax = "89"))
	float SlopeSlideStopAngle = 28.f;

	// Run-out: on a gentler floor the slide goes on (slowed by the friction) until the speed drops under this (cm/s),
	// instead of GMC's ground braking stopping a fast slide almost instantly.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0"))
	float SlopeSlideExitSpeed = 250.f;

	// Friction coefficient along the slope: deceleration = Friction * g * cos(angle). Gravity and friction balance at
	// atan(Friction) (0.3 -> 17 deg): the slide accelerates on every slope it starts on (~320 cm/s^2 at 35 deg, ~400 at
	// 40) and brakes the run-out on flatter ground (~290 cm/s^2 on the flat).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0", UIMax = "1"))
	float SlopeSlideFriction = 0.3f;

	// Friction coefficient of the run-out, once the floor is flatter than SlopeSlideStopAngle: ~780 cm/s^2 on the flat,
	// a 3-4 m run-out from full speed (with SlopeSlideFriction the pawn kept sliding ~8 m past the bottom of a ramp).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0", UIMax = "2"))
	float SlopeSlideRunOutFriction = 0.8f;

	// Speed cap along the slope (cm/s).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0"))
	float SlopeSlideMaxSpeed = 900.f;

	// Lateral steering: the move input can only add speed across the slope, up to SlopeSlideSteerSpeed (cm/s), reached
	// at SlopeSlideSteerAcceleration (cm/s^2). No braking and no climbing: the input along the fall line is ignored.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0"))
	float SlopeSlideSteerSpeed = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0"))
	float SlopeSlideSteerAcceleration = 800.f;

	// Yaw rate (deg/s) turning the pawn to its slide direction (the slide animations face the way the body moves).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0"))
	float SlopeSlideTurnRate = 360.f;

	// Gap (cm) under the collision the slide keeps floor contact through (crests, bumps). Beyond it the pawn is Airborne.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0"))
	float SlopeSlideSnapDistance = 15.f;

	// Small obstacles (logs, rocks, kerbs) have steep sides too. A slide only starts if the floor this far (cm,
	// horizontally) downhill of the contact is not walkable ground within the step height below it: otherwise the
	// steep surface is a bump the pawn steps over. 0 disables the check.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0"))
	float SlopeSlideBumpProbeDistance = 50.f;

	// Start the slide by itself from the floor angle (SlopeSlideStartAngle), on the ground and when landing. Off: a slide
	// only starts when requested (SetSlopeSlideRequested, e.g. a crouch press while sprinting) and the pawn walks down
	// steep slopes it does not ask to slide on.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide")
	bool bSlopeSlideAutoStart = true;

	// A requested slide needs a floor at least this steep (deg), moving downhill.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0", ClampMax = "89"))
	float SlopeSlideTriggerMinAngle = 10.f;

	// A requested slide needs at least this speed down the fall line (cm/s): sprinting, not running.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide", meta = (ClampMin = "0"))
	float SlopeSlideTriggerMinSpeed = 450.f;

	// Releasing the request (crouch) ends the slide. Ignored while bSlopeSlideAutoStart is on.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Slope Slide")
	bool bSlopeSlideEndOnRelease = true;

	// Optional: a bool on this component (e.g. a Blueprint's GMC-bound WantsToCrouch) that IS the slide request, read at
	// the start of every move. A held crouch then starts a slide where CanStartSlopeSlide allows it (in practice a press
	// while sprinting downhill: crouching drops the speed under SlopeSlideTriggerMinSpeed within a few frames) and
	// releasing it ends the slide. The bool must be GMC-bound as an input so the server reads the same value on the same
	// move. None = not used; drive the request with SetSlopeSlideRequested instead.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "GMCMotion|Slope Slide")
	FName SlopeSlideRequestProperty = NAME_None;

	// The player's slide request: true while crouch is held after a qualifying press. Write it ONLY through here (it is
	// bound ClientAuth_Input), from the input event, so it travels with the move. Ignored while SlopeSlideRequestProperty
	// is set, which overwrites it every move.
	UFUNCTION(BlueprintCallable, Category = "GMCMotion|Slope Slide")
	void SetSlopeSlideRequested(bool bRequested);

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Slope Slide")
	bool IsSlopeSlideRequested() const { return bSlopeSlideRequested; }

	// Whether a requested slide would start now: grounded, floor >= SlopeSlideTriggerMinAngle, downhill speed >=
	// SlopeSlideTriggerMinSpeed. Ask it at the crouch press to choose the slide over a crouch; the movement update checks
	// the same thing again inside the move.
	UFUNCTION(BlueprintPure, Category = "GMCMotion|Slope Slide")
	bool CanStartSlopeSlide() const;

	static bool ShouldStartRequestedSlopeSlide(bool bRequested, bool bOnGround, float FloorAngle, float MinAngle,
		double DownhillSpeed, double MinSpeed);

	// bEndOnRelease is the effective switch (bSlopeSlideEndOnRelease && !bSlopeSlideAutoStart).
	static bool ShouldKeepSlopeSlide(bool bRequested, bool bEndOnRelease, float FloorAngle, float StopAngle, double Speed,
		double ExitSpeed);

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Slope Slide")
	bool IsSlopeSliding() const { return GetMovementMode() == SlopeSlideMovementMode; }

	// The velocity a GROUNDED pawn carries into the slide: its horizontal velocity, lifted onto the floor plane (the
	// horizontal components are kept, Z is solved so the result lies on the plane). A grounded pawn's Velocity is
	// horizontal, and PhysicsSlopeSlide projects onto the plane, which cost a factor of cos(slope) on the first frame:
	// measured 375 -> 239 cm/s horizontal on a 35 deg slope, a visible stall that sprinting hid. Falls back to the
	// projection on a near-vertical surface, where lifting would explode.
	static FVector ComputeSlopeSlideEntryVelocity(const FVector& GroundVelocity, const FVector& FloorNormal);

	// =====================================================================================
	//  Camera yaw limit (local player only)
	// =====================================================================================
	// Limits the local player's view yaw (PlayerCameraManager ViewYawMin/Max) to a window either side of the pawn's
	// facing, so a first-person player cannot turn round and see their own body. Applied after the movement update, on
	// the locally controlled pawn only (never on a server's copy or a simulated proxy); the camera manager's own values
	// are restored exactly when the limit lifts. 0 disables a limit.

	// While slope sliding. Re-centred every frame on the pawn's facing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Camera", meta = (ClampMin = "0", ClampMax = "180"))
	float SlopeSlideCameraYawRange = 0.f;

	// While a component implementing IGMCMotionControlLossSource reports the player out of control (knocked down,
	// getting up, the target of a shove). Held on the facing at the moment control was lost: a ragdoll's capsule turns
	// with the tumbling body, which would yank the view.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Camera", meta = (ClampMin = "0", ClampMax = "180"))
	float KnockdownCameraYawRange = 0.f;

	// When a get-up starts (IGMCMotionControlLossSource::IsGettingUp) the local player's view turns to the direction the
	// body gets up facing over this long (s); with KnockdownCameraYawRange the window then follows the body for the rest
	// of the get-up. The capsule turns with the tumbling body while it is down, so the get-up can face anywhere: a first-
	// person view held on the facing at the knockdown looked at the rising body side-on, and in any view a pawn that faces
	// its view snapped round to it the moment control came back (measured: 234 -> 180 in CLEAR, -103 -> 180 in third
	// person here). 0 = no turn.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Camera", meta = (ClampMin = "0"))
	float GetUpViewAlignTime = 0.35f;

	// While slope sliding (and CameraYawLimitConditionProperty, e.g. first person): the local player cannot look further
	// down than SlopeSlideCameraMinPitch, so a first-person view does not look down into its own sliding body. Only
	// during the slide: the limit eases on when it starts (a view already lower is raised smoothly rather than snapped)
	// and eases off when it ends, back to the camera manager's own pitch limit (SlopeSlideCameraPitchInterpSpeed).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Camera")
	bool bLimitSlopeSlideCameraPitch = false;

	// Lowest view pitch while sliding (deg, negative = looking down).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Camera", meta = (ClampMin = "-89", ClampMax = "89"))
	float SlopeSlideCameraMinPitch = -30.f;

	// How fast the lowest allowed pitch rises to SlopeSlideCameraMinPitch when the view was lower at the slide start
	// (FInterpTo speed: 10 settles in ~0.3 s). 0 = at once.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Camera", meta = (ClampMin = "0"))
	float SlopeSlideCameraPitchInterpSpeed = 10.f;

	// Where the lowest allowed pitch starts at the slide entry: the current view pitch if it is lower than the limit
	// (so the view is not snapped up), else the limit. ViewPitch is a control rotation pitch (0..360 or -180..180).
	static float ComputeSlideCameraPitchMinStart(float ViewPitch, float MinPitch)
	{
		return FMath::Min(FRotator::NormalizeAxis(ViewPitch), MinPitch);
	}

	// The view yaw Alpha (0..1, eased) of the way from FromYaw to ToYaw, turning the short way round.
	static float ComputeGetUpViewYaw(float FromYaw, float ToYaw, float Alpha);

	// Optional: a bool on the owning pawn (e.g. bFirstPersonView) that must be true for either limit to apply, so a
	// third-person view stays free. None = the limits always apply.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "GMCMotion|Camera")
	FName CameraYawLimitConditionProperty = NAME_None;

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Camera")
	bool IsCameraYawLimited() const { return bCameraYawLimited; }

	// The view yaw window for a centre (deg) and a range either side. Not normalised: APlayerCameraManager::LimitViewYaw
	// clamps through FMath::ClampAngle, which handles a window that crosses 0/360.
	static void ComputeCameraYawWindow(float Centre, float Range, float& OutMin, float& OutMax)
	{
		OutMin = Centre - Range;
		OutMax = Centre + Range;
	}

	// =====================================================================================
	//  Ground velocity smoothing (GASP58 SmoothWalkingMode)
	// =====================================================================================
	// GASP58 walks with Mover's USmoothWalkingMode: the acceleration / braking (same values as ours) build an
	// intermediate velocity, and the real velocity follows it through a critically damped spring (0.1 s). GMC applied
	// them instantly: our starts reached 90 % of the run speed in 0.45 s instead of 0.56 s, stops slid 67 cm instead of
	// 108, and the start anims (authored for the smoothed ramp) slid 1.5-2x more (gasp58_locomotion_parity_roadmap.md).
	// Here GMC's own CalculateVelocity computes the intermediate velocity, then the spring gives the velocity. Grounded
	// only, not under root motion, traversal or bot direct moves. The spring state is GMC-bound.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ground Smoothing")
	bool bSmoothGroundVelocity = true;

	// Spring smoothing time (s) while speeding up / slowing down (GASP58 Acceleration/DecelerationSmoothingTime).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ground Smoothing", meta = (ClampMin = "0"))
	float GroundAccelerationSmoothingTime = 0.1f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ground Smoothing", meta = (ClampMin = "0"))
	float GroundDecelerationSmoothingTime = 0.1f;

	// Rotates the intermediate velocity toward the input direction, keeping its length, before the acceleration
	// (GASP58 TurningStrength 8). Without it, turning the camera while running left the intermediate velocity behind
	// the input (GMC turns it through the ground friction only) and the speed fell to ~210 cm/s at 180 deg/s while the
	// legs kept running (user report 2026-10-01). 0 disables.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ground Smoothing", meta = (ClampMin = "0"))
	float GroundTurningStrength = 8.f;

	// On the ground, a speed above the max speed (sprint released, crouch while sprinting) slows down at the braking
	// deceleration, like GASP58's walking mode (Deceleration = GaitChangeDeceleration 300 with an input, StoppingDeceleration
	// 1000 without; BP_GMCMovement sets the same values as BrakingDecelerationGrounded). GMC used its over-max deceleration
	// (10000): sprint -> run took ~0.02 s instead of 0.78 s and a sprint stop slid 144 cm instead of 229.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ground Smoothing")
	bool bGroundOverMaxSpeedUsesBraking = true;

	// How fast the intermediate velocity is pulled back to the real one when something else changed it (collisions),
	// scaled by how different they are (GASP58 OutsideInfluenceSmoothingTime).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ground Smoothing", meta = (ClampMin = "0"))
	float GroundOutsideInfluenceSmoothingTime = 0.05f;

	// =====================================================================================
	//  Ragdoll: the capsule follows the ragdoll (GASP58 BP_MovementMode_Ragdoll)
	// =====================================================================================
	// The ragdoll itself lives in UGMCMotionRagdollComponent (GMCMotionRagdoll): server-authoritative bodies, pose streamed to
	// the clients. This mode only keeps the capsule on the body. bRagdollMode is decided by the server and GMC-bound
	// (ServerAuth), so every machine enters the mode at the same move. Each machine moves its capsule to the target its
	// own ragdoll gives (SetRagdollCapsuleTarget): the clients' bodies follow the server's, so the capsules agree within a
	// few cm and the location tolerance is widened meanwhile (see RagdollActorLocationTolerance) instead of correcting.
	// Movement mode None (death) is never overridden.
	static constexpr EGMC_MovementMode RagdollMovementMode = EGMC_MovementMode::Custom3;

	// Server only: enters / leaves the ragdoll movement mode (GMC-bound, reaches the owner and the proxies). The request
	// is copied into the bound value inside the next move (UpdateMovementModeDynamic): a bound value changed outside the
	// movement cycle is overwritten by the server's end-of-move state for remote clients.
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "GMCMotion|Ragdoll")
	void SetRagdollMode(bool bEnable) { bRagdollModeRequest = bEnable; }

	UFUNCTION(BlueprintPure, Category = "GMCMotion|Ragdoll")
	bool IsRagdollMode() const { return bRagdollMode; }

	// Where the capsule should be (location = capsule centre, only the yaw of the rotation is used). Local, set every
	// frame by the ragdoll component on every machine. bHold keeps the capsule where it is (get-up montage).
	void SetRagdollCapsuleTarget(const FTransform& Target, bool bHold);
	// Same, with the velocity the capsule has on its way (a montage path: smart objects). Without it the velocity is the
	// move's jump to the target over the move's time, which explodes when the moves of several frames arrive in one batch
	// (220 ms ping) and carried the pawn metres forward when the mode ended (walk exit of a bench, 2026-10-01).
	void SetRagdollCapsuleTargetWithVelocity(const FTransform& Target, const FVector& InVelocity);
	void ClearRagdollCapsuleTarget() { bHasRagdollCapsuleTarget = false; bHasRagdollCapsuleVelocity = false; }

	// Location / rotation tolerances while the ragdoll drives the capsule (and for TraversalToleranceGraceTime after).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ragdoll", meta = (ClampMin = "0"))
	float RagdollActorLocationTolerance = 1000.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ragdoll", meta = (ClampMin = "0"))
	float RagdollActorRotationTolerance = 360.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GMCMotion|Ragdoll", meta = (ClampMin = "0"))
	float RagdollVelocityTolerance = 100000.0f;

	// GMC never updates CurrentFloor on simulated proxies (GenSimulationTick runs no floor query), so the floor data the
	// AnimInstance reads kept its initial value on the other clients: slope angle always 0 (no slope anims) and the slide
	// pose projected on a plane through the world origin (the mesh under the slope). Runs the same floor query as the
	// owner's moves, on simulated proxies only, while grounded or sliding and only after the pawn moved (a standing
	// crowd costs nothing). Called by UGMCMotion_AnimInstance before it reads the floor (game thread).
	void UpdateSimulatedProxyFloor();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	// Runs the GASP pipeline (input acceleration, rotation mode, facing, direction, trajectory)
	// AFTER the BP's PrePhysicsUpdate event completes (Jump, UpdateGait, SetGait, Update_Grounded_Physic).
	virtual void PrePhysicsUpdate_Implementation(float DeltaSeconds) override;

	// Regular GMC velocity, smoothed on the ground (see bSmoothGroundVelocity).
	virtual void CalculateVelocity(float DeltaSeconds) override;
	virtual float GetOverMaxSpeedDeceleration() const override;

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
	float RagdollToleranceGraceRemaining = 0.0f;

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

	// The raw (un-warped) animation velocity (Z only used for an exit in the air) from the most recent traversal frame, captured
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

	// Start time of the first warp window in the montage. Used to detect the pre-window phase where
	// horizontal root motion must be suppressed for jump-overs (prevents the capsule from running
	// through the collision-disabled obstacle before the warp can redirect it).
	float GetFirstWarpWindowStart(UAnimMontage* Montage) const;

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

	// Hysteresis latch for CanSprintInCurrentDirection(). Mutable because the query is const and
	// BlueprintPure; it is idempotent for a given input, so repeated calls in one frame agree.
	mutable bool bSprintConeLatched = false;

	// Internal state for ApplyFacingSpring (called from UpdateGroundedFacing each frame).
	// Position uses its own variable (not OverridenDesiredFacing.Yaw) so the BP's
	// direct writes to OverridenDesiredFacing don't corrupt the spring state.
	float FacingSpringPositionYaw = 0.f;
	float FacingSpringVelocityYaw = 0.f;

	// Sprint strafe reversal hold: freezes the facing spring so the capsule doesn't
	// rotate 180° during the direction flip. The high angular velocity from the spring
	// causes the GASP Chooser to select pivot animations (body rotation) instead of
	// stop/plant transitions. By freezing the facing, angular velocity stays near zero,
	// the Chooser sees a decelerating character and selects a stop transition. The hold
	// releases when velocity drops below the threshold, allowing the spring to smoothly
	// transition to the new facing direction at low speed.
	bool bSprintReversalHoldActive = false;

	// True while the facing spring is settling from a reversal hold release.
	// During settling, ApplyFacingSpring runs normally (~90° transition over ~0.3s).
	// Once displacement drops below threshold, this clears and the system switches
	// to direct tracking (OverridenDesiredFacing = target, zero lag) for responsive
	// camera following. Without this, the spring's steady-state lag (~72° at 500°/s
	// mouse speed) causes the player to see their own body during fast rotation.
	bool bFacingSpringSettling = false;

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
	int32 BI_Downed = -1;

	// Enters, keeps or leaves SlopeSlideMovementMode from the move's floor (see SlopeSlideStartAngle). Returns true when
	// the slide owns the mode for this move (UpdateMovementModeDynamic then skips GMC's static update).
	bool UpdateSlopeSlideMode(const FGMC_FloorParams& Floor);

	// The conditions under which a slide may exist at all (enabled, not downed, upright capsule, gravity, not in water).
	// Shared by UpdateSlopeSlideMode and CanStartSlopeSlide so the Blueprint's pre-check and the move agree.
	bool IsSlopeSlideAllowed() const;

	// Speed along the floor's fall line (cm/s, positive = downhill) for a floor normal.
	double GetSlopeSlideDownhillSpeed(const FVector& FloorNormal) const;

	// The slope slide request (see SetSlopeSlideRequested). Bound ClientAuth_Input: it originates on the client, and the
	// slide's start and release must evaluate on the same move on every machine, replays included.
	bool bSlopeSlideRequested = false;
	int32 BI_SlopeSlideRequested = -1;

	// Applies or lifts the camera yaw limit (see SlopeSlideCameraYawRange). Called from TickComponent.
	void UpdateCameraYawLimit();
	bool bCameraYawLimited = false;
	bool bCameraYawLimitHeld = false;
	float CameraYawLimitCentre = 0.f;
	// The get-up view turn (GetUpViewAlignTime): started for this get-up, finished, the yaw it started from, time spent.
	bool bGetUpViewAlignStarted = false;
	bool bGetUpViewAligned = false;
	float GetUpViewAlignFromYaw = 0.f;
	float GetUpViewAlignElapsed = 0.f;
	// The camera manager's values before the limit, restored when it lifts.
	float SavedViewYawMin = 0.f;
	float SavedViewYawMax = 359.998993f;
	TWeakObjectPtr<class APlayerCameraManager> LimitedCamera;
	// The slide pitch limit (bLimitSlopeSlideCameraPitch): on, the lowest pitch it currently allows, the camera manager's
	// own minimum to restore, and the camera it was applied to.
	bool bCameraPitchLimited = false;
	float CameraPitchMinCurrent = 0.f;
	float SavedViewPitchMin = -89.9f;
	TWeakObjectPtr<class APlayerCameraManager> PitchLimitedCamera;

	// Floor normal and angle (deg) the slide uses. The line hit (straight under the collision centre) is preferred: on
	// stairs it finds the flat tread where the shape hit finds the step edge. False when the shape is not within
	// MAX_DISTANCE_TO_FLOOR + ExtraRange of a floor.
	bool GetSlopeSlideFloor(const FGMC_FloorParams& Floor, float ExtraRange, FVector& OutNormal, float& OutAngle) const;

	// True when the steep floor is a small obstacle rather than a slope (see SlopeSlideBumpProbeDistance): a line down,
	// SlopeSlideBumpProbeDistance downhill of the shape contact, finds walkable ground within the step height below it.
	bool IsSlopeSlideBump(const FGMC_FloorParams& Floor, const FVector& FloorNormal) const;

	// PhysicsCustom for SlopeSlideMovementMode: velocity in the floor plane (gravity along the plane, friction, lateral
	// steering, speed cap), yaw turned to the slide direction, swept move sliding along obstacles, then the floor is
	// updated and the collision kept at GMC's floor distance.
	void PhysicsSlopeSlide(float DeltaSeconds);

	// Ragdoll mode (see RagdollMovementMode). GMC-bound, ServerAuth (BI_RagdollMode).
	bool bRagdollMode = false;
	int32 BI_RagdollMode = -1;
	bool bRagdollModeRequest = false;
	FTransform RagdollCapsuleTarget = FTransform::Identity;
	bool bHasRagdollCapsuleTarget = false;
	bool bHoldRagdollCapsule = false;
	FVector RagdollCapsuleVelocity = FVector::ZeroVector;
	bool bHasRagdollCapsuleVelocity = false;

	// PhysicsCustom for RagdollMovementMode: puts the capsule on the ragdoll target (no sweep, the body already
	// resolved the collisions), yaw only.
	void PhysicsRagdoll(float DeltaSeconds);

	// Location of the last UpdateSimulatedProxyFloor query (simulated proxies only).
	FVector LastSimulatedFloorQueryLocation = FVector(UE_BIG_NUMBER);

	// Ground velocity smoothing state (see bSmoothGroundVelocity), horizontal. Movement state carried from move to move:
	// intermediate velocity (what the acceleration / braking give, unsmoothed), spring velocity output last move and its
	// acceleration. Not valid (re-initialised from the velocity) after a move without smoothing.
	FVector2D SmoothIntermediateVelocity = FVector2D::ZeroVector;
	FVector2D SmoothSpringVelocity = FVector2D::ZeroVector;
	FVector2D SmoothSpringAcceleration = FVector2D::ZeroVector;
	bool bSmoothStateValid = false;
	int32 BI_SmoothIntermediateVelocity = -1;
	int32 BI_SmoothSpringVelocity = -1;
	int32 BI_SmoothSpringAcceleration = -1;
	int32 BI_SmoothStateValid = -1;

	// True when this move's ground velocity is smoothed (see bSmoothGroundVelocity).
	bool ShouldSmoothGroundVelocity() const;

	// Post-jump-over speed carry (see TraversalExitOverSpeedDeceleration): set by RestoreTraversalExitVelocity, cleared
	// in CalculateVelocity. Movement state carried from move to move.
	bool bTraversalExitCarry = false;
	int32 BI_TraversalExitCarry = -1;

	// Horizontal speed at traversal start (captured by StopTraversalMovement), see TraversalEntrySpeedBlendTime.
	float TraversalEntrySpeed = 0.0f;
	int32 BI_TraversalEntrySpeed = -1;

	// Last warped horizontal speed of a jump-over window and that window's end (montage time), see
	// WarpWindowExitBlendTime. Movement state carried from move to move.
	float LastWarpHorizontalSpeed = 0.0f;
	float LastWarpWindowEnd = -1.0f;
	int32 BI_LastWarpHorizontalSpeed = -1;
	int32 BI_LastWarpWindowEnd = -1;

	// BP_GMCMovement's ConfirmedTraversalStartTime (montage start position of the running traversal), found on first use.
	mutable const FProperty* ConfirmedStartTimeProp = nullptr;
	mutable bool bConfirmedStartTimePropSearched = false;
	float GetConfirmedTraversalStartTime() const;

	// Set by UpdateStanceCollision: the collision moved during the last call.
	bool bStanceCollisionChanging = false;
};
