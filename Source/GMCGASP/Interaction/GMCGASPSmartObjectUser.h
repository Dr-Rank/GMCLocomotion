#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "SmartObjectRuntime.h"
#include "SmartObjectRequestTypes.h"
#include "GMCGASPSmartObjectUser.generated.h"

class UAnimMontage;
class UGMCMotion;
class UInputAction;
class USkeletalMeshComponent;

UENUM(BlueprintType)
enum class EGMCGASPSmartObjectPhase : uint8
{
	None,
	/** Walking to the slot and sitting down (entry montage). */
	Entering,
	/** Idle loop on the slot. */
	Seated,
	/** Standing up (exit montage). */
	Exiting
};

/** Replicated use of a smart object slot (server-decided). Every machine plays the montage and computes the capsule
 *  path from it (see UGMCGASPSmartObjectUser). */
USTRUCT()
struct FGMCGASPSmartObjectState
{
	GENERATED_BODY()

	UPROPERTY()
	EGMCGASPSmartObjectPhase Phase = EGMCGASPSmartObjectPhase::None;

	/** Bumped at every phase change: replays the phase on the clients even if it is the same. */
	UPROPERTY()
	uint8 Serial = 0;

	/** Montage of the phase (entry, idle loop or exit). */
	UPROPERTY()
	TObjectPtr<UAnimMontage> Montage = nullptr;

	/** Where the montage starts (entry: the point of its path closest to the pawn). */
	UPROPERTY()
	float MontageStartTime = 0.f;

	/** Slot transform (the root of the seated pose), yaw only. */
	UPROPERTY()
	FVector_NetQuantize10 SlotLocation = FVector::ZeroVector;

	UPROPERTY()
	float SlotYaw = 0.f;

	/** Entering: where the pawn stood when the entry started, at its feet (mesh origin), and its yaw. The gap to the
	 *  montage path fades out until seated. */
	UPROPERTY()
	FVector_NetQuantize10 StartLocation = FVector::ZeroVector;

	UPROPERTY()
	float StartYaw = 0.f;
};

/**
 * Player use of smart object slots (GASP58 AC_SmartObjectAnimation + ST_Player_SandboxCharacter_SmartObject: benches),
 * server-authoritative (gasp58_locomotion_parity_roadmap.md §6).
 *
 * The server claims the slot (TryUse, from the bench's UGMCGASPSmartObjectInteractable), picks the entry montage and its
 * start time (the point of the montage's root path, laid out on the slot, closest to the pawn: what GASP58's
 * CHT_SmartObject_BenchAnim angle rows + pose search column do) and replicates the phase. Every machine plays the
 * montage on its AnimInstance; the server and the owner move the capsule along the montage's root path through the
 * GMC ragdoll mode (capsule-follow, widened tolerances), like the character interactions of UGMCMotionRagdollComponent.
 * The pawn's start gap to the path fades out until the root reaches the seat. A move input (server: the GMC input of
 * the moves) or the interact key while seated stands the pawn up; the slot is released at the end of the exit, or at
 * once if the ragdoll takes over (hit, death).
 */
UCLASS(ClassGroup = (GMCGASP), meta = (BlueprintSpawnableComponent))
class GMCGASP_API UGMCGASPSmartObjectUser : public UActorComponent
{
	GENERATED_BODY()

public:
	UGMCGASPSmartObjectUser();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** Server: claim the closest free slot of @SmartObjectActor and walk to it. False if none is free or the pawn is busy. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "SmartObject")
	bool TryUse(AActor* SmartObjectActor);

	/** Interact key on @SmartObjectActor, or the nearest free slot if null (owning machine). From a crouch the pawn
	 *  stands up first (StandUpBeforeUseTime), then asks the server. */
	UFUNCTION(BlueprintCallable, Category = "SmartObject")
	void RequestUse(AActor* SmartObjectActor);

	/** Interact key without a crosshair target (any machine; the owning client asks the server): the closest free slot
	 *  within MaxUseDistance. */
	UFUNCTION(BlueprintCallable, Category = "SmartObject")
	void RequestUseNearby();

	/** Server: RequestUseNearby. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "SmartObject")
	bool TryUseNearby();

	/** Stand up (any machine; the owning client asks the server). Only while seated. */
	UFUNCTION(BlueprintCallable, Category = "SmartObject")
	void RequestExit();

	/** Using a slot (entering, seated or exiting). */
	UFUNCTION(BlueprintPure, Category = "SmartObject")
	bool IsUsingSmartObject() const { return State.Phase != EGMCGASPSmartObjectPhase::None; }

	UFUNCTION(BlueprintPure, Category = "SmartObject")
	EGMCGASPSmartObjectPhase GetPhase() const { return State.Phase; }

	/** Can the pawn start using a slot now (on the ground, not busy)? */
	UFUNCTION(BlueprintPure, Category = "SmartObject")
	bool CanUse() const;

	/** Free slot of @SmartObjectActor this pawn may claim (server: SmartObject subsystem). */
	bool HasFreeSlot(AActor* SmartObjectActor) const;

	/** Entry montages (GASP58 M_interaction_bench_into_*). The root path of each ends on the slot. */
	UPROPERTY(EditAnywhere, Category = "SmartObject")
	TArray<TObjectPtr<UAnimMontage>> EntryMontages;

	UPROPERTY(EditAnywhere, Category = "SmartObject")
	TObjectPtr<UAnimMontage> IdleMontage;

	/** Exit on the interact key. */
	UPROPERTY(EditAnywhere, Category = "SmartObject")
	TObjectPtr<UAnimMontage> ExitStandMontage;

	/** Exit on a move input. */
	UPROPERTY(EditAnywhere, Category = "SmartObject")
	TObjectPtr<UAnimMontage> ExitWalkMontage;

	/** A move input exits with ExitWalkMontage when its direction is within acos(this) of the seat's forward, else with
	 *  ExitStandMontage (the walk exit only goes forward: a camera facing the bench made the pawn walk back into it). */
	UPROPERTY(EditAnywhere, Category = "SmartObject", meta = (ClampMin = -1, ClampMax = 1))
	float WalkExitMinInputDot = 0.5f;

	/** User tags of the claim request (the slot definitions filter on SmartObject.ObjectType.Player). */
	UPROPERTY(EditAnywhere, Category = "SmartObject")
	FGameplayTagContainer UserTags;

	/** Entry start time choice: cost of a degree of facing difference, in cm of distance. */
	UPROPERTY(EditAnywhere, Category = "SmartObject", meta = (ClampMin = 0))
	float EntryYawCost = 1.f;

	/** The entry starts at least this long before the root reaches the slot (room to fade the start gap out). */
	UPROPERTY(EditAnywhere, Category = "SmartObject", meta = (ClampMin = 0))
	float MinEntryDuration = 1.f;

	/** Farthest slot TryUse accepts from the pawn. */
	UPROPERTY(EditAnywhere, Category = "SmartObject", meta = (ClampMin = 0))
	float MaxUseDistance = 400.f;

	UPROPERTY(EditAnywhere, Category = "SmartObject")
	float MontageBlendOutTime = 0.25f;

	/** From a crouch: seconds standing (stand-up animation) before the use is sent to the server. */
	UPROPERTY(EditAnywhere, Category = "SmartObject", meta = (ClampMin = 0))
	float StandUpBeforeUseTime = 0.4f;

	/** Name of the pawn's body mesh component. */
	UPROPERTY(EditAnywhere, Category = "SmartObject")
	FName MeshName = TEXT("Mesh");

	/** Interact key, bound on the locally controlled pawn: seated -> stand up, otherwise the closest free slot
	 *  (RequestUseNearby). Leave empty when another component handles the key (it can call RequestUse / RequestExit). */
	UPROPERTY(EditAnywhere, Category = "SmartObject")
	TObjectPtr<UInputAction> InteractAction;

	/** Interact key handler (see InteractAction). */
	UFUNCTION(BlueprintCallable, Category = "SmartObject")
	void Interact();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION(Server, Reliable)
	void ServerRequestExit(bool bWalk);

	/** Null actor = the nearest free slot. */
	UFUNCTION(Server, Reliable)
	void ServerUse(AActor* SmartObjectActor);

	void SendUse(AActor* SmartObjectActor);
	void UpdatePendingUse(float DeltaTime);
	/** BP_GMCMovement's bound Stance (E_Stance: 0 Stand, 1 Crouch). */
	uint8 GetStance() const;

	/** Server: claims the free slot of @Results closest to the pawn and starts the entry. */
	bool ClaimClosestSlot(const TArray<FSmartObjectRequestResult>& Results);

	UFUNCTION()
	void OnRep_State();

	/** Server: next phase (bumps the serial, applies it locally). */
	void SetPhase(EGMCGASPSmartObjectPhase Phase, UAnimMontage* Montage, float StartTime);
	void ServerExit(bool bWalk);
	/** Server: back to locomotion; @bKeepMovementMode when the ragdoll took over the movement mode. */
	void ServerFinish(bool bKeepMovementMode);
	void ReleaseClaim();

	/** Every machine: plays the phase's montage, starts the local clock. */
	void ApplyState();
	/** Server / owner: moves the capsule target along the montage path. */
	void UpdateCapsuleTarget();

	/** Capsule transform at @Time of the phase's montage. */
	FTransform GetPathTransform(const UAnimMontage* Montage, bool bEndsOnSlot, float Time) const;
	FTransform GetPathTransform(const UAnimMontage* Montage, bool bEndsOnSlot, float Time, const FVector& SlotLocation, float SlotYaw) const;
	/** Root path of the montage in the slot's space (the animation's origin is the slot), sampled at SampleRate. */
	struct FMontagePath
	{
		TArray<FTransform> Samples;
		float Length = 0.f;
		/** Entry: when the root reaches the slot. Exit: when the root speed settles (locomotion takes over). */
		float KeyTime = 0.f;
	};
	const FMontagePath& GetMontagePath(const UAnimMontage* Montage, bool bEndsOnSlot) const;
	bool ChooseEntry(const FTransform& PawnTransform, const FTransform& SlotTransform, UAnimMontage*& OutMontage, float& OutStartTime) const;

	UGMCMotion* GetMotion() const;
	void BindInput();
	bool bInputBound = false;
	USkeletalMeshComponent* GetMesh() const;
	bool IsRagdollBusy() const;
	/** Owning machine: clears the crouch input when the entry starts (the pawn stands up, like GASP58). */
	void ClearCrouchInput();
	/** Mesh relative transform with the standing offset (the capsule is standing while a slot is used). */
	FTransform GetStandingMeshRelative() const;

	UPROPERTY(ReplicatedUsing = OnRep_State)
	FGMCGASPSmartObjectState State;

	FSmartObjectClaimHandle ClaimHandle;
	/** Montage time of the current phase on this machine. */
	float LocalTime = 0.f;
	uint8 AppliedSerial = 0;
	bool bSeatedInputReleased = false;
	TWeakObjectPtr<AActor> PendingUseActor;
	bool bPendingUse = false;
	float PendingUseTime = 0.f;
	float PendingStandingTime = 0.f;

	mutable TMap<TObjectKey<UAnimMontage>, FMontagePath> EntryPaths;
	mutable TMap<TObjectKey<UAnimMontage>, FMontagePath> ExitPaths;
};
