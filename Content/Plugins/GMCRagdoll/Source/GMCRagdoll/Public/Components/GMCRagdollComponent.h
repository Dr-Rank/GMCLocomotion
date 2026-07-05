#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GMCRagdollComponent.generated.h"

class UGMC_OrganicMovementCmp;
class USkeletalMeshComponent;
class UCapsuleComponent;

UENUM(BlueprintType)
enum class EGMCRagdollState : uint8
{
	None,           // Not ragdolling
	BlendingIn,     // Blending from animation to physics
	Active,         // Fully physics-driven
	Settled,        // Physics settled, waiting for external action
	BlendingOut     // Blending back to animation (recovery)
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRagdollStarted);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRagdollSettled);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnRagdollEnded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRevived, bool, bFaceUp);

/**
 * Cosmetic ragdoll component for GMCv2 characters.
 *
 * Each client independently simulates ragdoll physics — bone positions are NOT
 * replicated. The capsule provides the replicated position via GMC's normal
 * pipeline. GMC runs normally on all roles; this component does NOT change
 * movement mode or skip Super.
 *
 * Replication: The component replicates a single bool (bRagdollActive). When
 * the server calls EnableRagdoll(), the bool replicates to clients via OnRep
 * and each client enables ragdoll locally. No bone positions are replicated.
 *
 * Usage:
 *   1. Add this component to your character alongside the GMC movement component.
 *   2. Call EnableRagdoll() on the server/authority when the character should ragdoll.
 *   3. Call DisableRagdoll() on the server/authority to blend back to animation.
 *   4. Clients receive the state automatically via replication.
 */
UCLASS(ClassGroup=(GMCRagdoll), meta=(BlueprintSpawnableComponent, DisplayName="GMC Ragdoll Component"))
class GMCRAGDOLL_API UGMCRagdollComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGMCRagdollComponent();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// ── Core Functions ──

	/** Enable ragdoll physics. Optionally apply an initial velocity to all bones.
	 *  Can be called on any role. On Authority: multicasts to all clients.
	 *  On non-Authority: enables locally only (server multicast will follow). */
	UFUNCTION(BlueprintCallable, Category="GMC Ragdoll")
	void EnableRagdoll(FVector InitialBoneVelocity = FVector::ZeroVector);

	/** Begin recovery from ragdoll — blend physics out and return to animation.
	 *  Can be called on any role. On Authority: multicasts to all clients. */
	UFUNCTION(BlueprintCallable, Category="GMC Ragdoll")
	void DisableRagdoll();

	/**
	 * Handle a revive event: saves a pose snapshot for AnimGraph blending,
	 * captures face-up/down state, begins ragdoll blend-out, and broadcasts
	 * OnRevived with the face-up result so Blueprint can select the correct
	 * get-up animation.
	 *
	 * Call this from the revive ability (e.g. GA_Drag) on the drag target's
	 * ragdoll component. GMAS tag/effect cleanup should be done separately
	 * by the calling ability.
	 */
	UFUNCTION(BlueprintCallable, Category="GMC Ragdoll")
	void HandleRevived();

	// ── Queries ──

	/** Is the character currently in any ragdoll state? */
	UFUNCTION(BlueprintPure, Category="GMC Ragdoll")
	bool IsRagdolling() const { return RagdollState != EGMCRagdollState::None; }

	/** Has the ragdoll settled (physics nearly at rest)? */
	UFUNCTION(BlueprintPure, Category="GMC Ragdoll")
	bool IsRagdollSettled() const { return RagdollState == EGMCRagdollState::Settled; }

	/** Current ragdoll state. */
	UFUNCTION(BlueprintPure, Category="GMC Ragdoll")
	EGMCRagdollState GetRagdollState() const { return RagdollState; }

	/**
	 * Is the ragdoll face-up or face-down? Useful for selecting get-up animations.
	 * Returns true if the pelvis Z-axis points upward (face-up).
	 */
	UFUNCTION(BlueprintPure, Category="GMC Ragdoll")
	bool IsRagdollFaceUp() const;

	/** Velocity of the ragdoll bone at the moment DisableRagdoll() was called. */
	UFUNCTION(BlueprintPure, Category="GMC Ragdoll")
	FVector GetRagdollRecoveryVelocity() const { return RagdollRecoveryVelocity; }

	// ── Capsule Impulse ──

	/**
	 * Add velocity to the movement component's capsule (e.g., from projectile hit).
	 * Call BEFORE EnableRagdoll so the capsule coasts with this velocity.
	 */
	UFUNCTION(BlueprintCallable, Category="GMC Ragdoll")
	void ApplyCapsuleImpulse(FVector Impulse);

	// ── Delegates ──

	/** Fires when ragdoll physics is enabled. */
	UPROPERTY(BlueprintAssignable, Category="GMC Ragdoll")
	FOnRagdollStarted OnRagdollStarted;

	/** Fires when ragdoll bones have stopped moving. */
	UPROPERTY(BlueprintAssignable, Category="GMC Ragdoll")
	FOnRagdollSettled OnRagdollSettled;

	/** Fires when blend-out completes and the character returns to animation. */
	UPROPERTY(BlueprintAssignable, Category="GMC Ragdoll")
	FOnRagdollEnded OnRagdollEnded;

	/** Fires when HandleRevived() is called. bFaceUp indicates ragdoll orientation
	 *  for selecting the correct get-up animation (face-up vs face-down). */
	UPROPERTY(BlueprintAssignable, Category="GMC Ragdoll")
	FOnRevived OnRevived;

	// ── Configuration ──

	/** Root bone for physics simulation. All bodies below this bone simulate physics. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	FName RagdollBoneName = FName(TEXT("pelvis"));

	/** Seconds to blend from animation to full physics. 0 = instant (recommended). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	float BlendInDuration = 0.0f;

	/** Seconds to blend from physics back to animation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	float BlendOutDuration = 0.3f;

	/** Bone velocity magnitude below which ragdoll is considered settled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	float SettleVelocityThreshold = 5.0f;

	/** Consecutive frames bone velocity must stay below threshold to trigger settle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	int32 SettleFrameCount = 10;

	/** If true, capsule ignores Pawn collision channel during ragdoll. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	bool bDisableCapsulePawnCollision = true;

	/** If true, shrink capsule height during ragdoll. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	bool bShrinkCapsule = false;

	/** Target capsule half-height when shrinking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config", meta=(EditCondition="bShrinkCapsule", EditConditionHides))
	float ShrunkCapsuleHalfHeight = 20.0f;

	/** Force applied each frame to keep the ragdoll mesh near the capsule. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	float CapsuleTetherForce = 1000.0f;

	/** Pose snapshot name saved by HandleRevived(). Use this name in the AnimGraph
	 *  Pose Snapshot node to blend from the ragdoll pose into the get-up animation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Config")
	FName RagdollPoseSnapshotName = FName(TEXT("RagdollPose"));

	// ── Physics Body Tuning ──

	/** Multiplier applied to all ragdoll body masses. Higher = heavier, less floaty.
	 *  1.0 = default physics asset mass. 2.0 = double mass, etc. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Physics", meta=(ClampMin="0.1", UIMin="0.1", UIMax="10.0"))
	float RagdollMassScale = 1.0f;

	/** Linear damping applied to all ragdoll bodies. Higher = more drag, bodies slow down faster.
	 *  0 = no damping (floaty). 5-10 = heavy, weighted feel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Physics", meta=(ClampMin="0.0", UIMin="0.0", UIMax="20.0"))
	float RagdollLinearDamping = 1.0f;

	/** Angular damping applied to all ragdoll bodies. Higher = less spinning/rotation.
	 *  0 = no damping. 5-10 = limbs resist spinning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="GMC Ragdoll|Physics", meta=(ClampMin="0.0", UIMin="0.0", UIMax="20.0"))
	float RagdollAngularDamping = 1.0f;

private:

	/** Multicast RPC — fires on all instances when server enables ragdoll. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_EnableRagdoll();

	/** Multicast RPC — fires on all instances when server disables ragdoll. */
	UFUNCTION(NetMulticast, Reliable)
	void Multicast_DisableRagdoll();

	/** Called when bRagdollActive replicates to clients (handles late joiners / relevancy). */
	UFUNCTION()
	void OnRep_RagdollActive();

	/** Performs the local ragdoll enable logic (physics, blend, etc.). */
	void EnableRagdollLocal(FVector InitialBoneVelocity = FVector::ZeroVector);

	/** Performs the local ragdoll disable logic (begin blend-out). */
	void DisableRagdollLocal();

	void UpdateRagdollBlend(float DeltaTime);
	void CheckRagdollSettled();
	void FinishBlendOut();

	/** Apply impulse from pelvis toward capsule base to keep ragdoll mesh tethered to capsule. */
	void KeepMeshOnCapsule();

	/** Cache references to owner's mesh and movement component. Returns true if valid. */
	bool CacheOwnerReferences();

	// ── Replicated state (for late joiners / relevancy changes) ──
	UPROPERTY(ReplicatedUsing=OnRep_RagdollActive)
	bool bRagdollActive = false;

	// ── Cached references ──
	UPROPERTY()
	TObjectPtr<USkeletalMeshComponent> CachedMesh;

	UPROPERTY()
	TObjectPtr<UGMC_OrganicMovementCmp> CachedMovementComp;

	// ── State ──
	EGMCRagdollState RagdollState = EGMCRagdollState::None;

	// ── Blend state ──
	float BlendAlpha = 0.0f;
	float BlendTimer = 0.0f;

	// ── Settling detection ──
	int32 SettleFrameCounter = 0;

	// ── Recovery data ──
	FVector RagdollRecoveryVelocity = FVector::ZeroVector;

	// ── Saved state for restore ──
	FName SavedMeshCollisionProfileName = NAME_None;
	ECollisionEnabled::Type SavedMeshCollisionEnabled = ECollisionEnabled::NoCollision;
	EVisibilityBasedAnimTickOption SavedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	TMap<ECollisionChannel, ECollisionResponse> SavedCapsuleResponses;
	float SavedCapsuleHalfHeight = 0.0f;
	bool bCapsuleWasShrunk = false;

	// ── Saved physics body state (per-body, restored on blend-out) ──
	TArray<float> SavedBodyMassScales;
	TArray<float> SavedBodyLinearDamping;
	TArray<float> SavedBodyAngularDamping;
};
