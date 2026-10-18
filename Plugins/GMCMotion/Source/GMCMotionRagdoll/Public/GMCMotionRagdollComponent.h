#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "PoseSearch/PoseSearchConstraints.h"
#include "Components/GMCMotionControlLoss.h"
#include "GMCMotionRagdollComponent.generated.h"

class UAnimMontage;
class UGMCAbilityEffect;
class UGMCMotion;
class UGMC_AbilitySystemComponent;
class UInputAction;
class UMultiAnimAsset;
class UPhysicsControlAsset;
class UPhysicsControlComponent;
class UPoseSearchDatabase;
class USkeletalMeshComponent;

/** Pose the ragdoll's anim graph shows on the ground (GASP58 E_RagdollInjuryState, same order). */
UENUM(BlueprintType)
enum class EGMCMotionRagdollInjury : uint8
{
	None,
	Limp,
	Stunned,
	Head_Face,
	Head_Back,
	Body_Front,
	Groin
};

UENUM(BlueprintType)
enum class EGMCMotionRagdollPhase : uint8
{
	None,
	Ragdoll,
	GettingUp,
	Dead,
	/** Playing a two-character interaction montage (shove / tackle / takedown), as initiator or target. */
	Interacting
};

/** GASP58 interaction request (CHT_CharacterInteractionPSDs input). A shove becomes a tackle at run speed (chooser rows);
 * Tackle is kept for the old callers and searches like Shove. */
UENUM(BlueprintType)
enum class EGMCMotionInteractionType : uint8
{
	Shove,
	Tackle,
	Takedown
};

/** One row of GASP58's CHT_CharacterInteractionPSDs: the pose search database of an interaction type within a speed
 * band of the initiator, and the roles each side can take in it. */
USTRUCT(BlueprintType)
struct FGMCMotionInteractionDatabase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
	EGMCMotionInteractionType Type = EGMCMotionInteractionType::Shove;

	/** Initiator speed band (cm/s, GASP Get_Speed): Min included, Max excluded. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
	float MinSpeed = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
	float MaxSpeed = 100000.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
	TObjectPtr<UPoseSearchDatabase> Database;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
	TArray<FName> InitiatorRoles = {TEXT("Attacker")};

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Interaction")
	TArray<FName> TargetRoles = {TEXT("Victim")};
};

/** Server-decided ragdoll state, replicated to every machine. */
USTRUCT()
struct FGMCMotionRagdollState
{
	GENERATED_BODY()

	UPROPERTY()
	EGMCMotionRagdollPhase Phase = EGMCMotionRagdollPhase::None;

	UPROPERTY()
	EGMCMotionRagdollInjury Injury = EGMCMotionRagdollInjury::None;

	/** Incremented on every server transition, so a client that missed one still applies the latest. */
	UPROPERTY()
	uint8 Serial = 0;

	/** Kick given when the ragdoll started (world, kg*cm/s), at ImpulseLocation. Zero = none. */
	UPROPERTY()
	FVector_NetQuantize10 Impulse = FVector::ZeroVector;

	UPROPERTY()
	FVector_NetQuantize ImpulseLocation = FVector::ZeroVector;

	/** Get-up (GettingUp) or interaction montage (Interacting) picked by the server. */
	UPROPERTY()
	TObjectPtr<UAnimMontage> Montage = nullptr;

	/** Where the montage starts (pose search SelectedTime: CHT_GetUpMontages MontageStartTime, MotionMatchMulti result). */
	UPROPERTY()
	float MontageStartTime = 0.f;

	/** Interacting: the multi-character asset (GASP PSIA), this pawn's role in it, the other character, mirroring. */
	UPROPERTY()
	TObjectPtr<UMultiAnimAsset> InteractionAsset = nullptr;

	UPROPERTY()
	FName Role;

	UPROPERTY()
	TObjectPtr<AActor> Partner = nullptr;

	UPROPERTY()
	bool bMirrored = false;

	/** Interacting: this pawn started the interaction (its mesh ticks after the partner's, GASP58 Try_MultiCharacterInteraction). */
	UPROPERTY()
	bool bInitiator = false;

	/** Interacting: where the capsule must be at the end of the montage's motion warping window (GASP
	 * BP_NotifyState_MMI_MotionWarping OnWarpBegin: CalculateFullAlignedTransform + 90 deg, as a capsule transform). */
	UPROPERTY()
	bool bWarp = false;

	UPROPERTY()
	FVector_NetQuantize WarpLocation = FVector::ZeroVector;

	UPROPERTY()
	float WarpYaw = 0.f;
};

/** One sample of the server's bodies: the pelvis in world space, every other body relative to it (quantized). */
USTRUCT()
struct FGMCMotionRagdollNetPose
{
	GENERATED_BODY()

	// UPROPERTY so the replicated FinalPose is compared field by field (the wire format is NetSerialize's).

	/** Server world time (AGameStateBase::GetServerWorldTimeSeconds) of the sample. */
	UPROPERTY()
	double ServerTime = 0.0;

	/** Transforms of the physics asset bodies, in Mesh->Bodies order, world space. */
	UPROPERTY()
	TArray<FTransform> Bodies;

	/** Index of the reference body (pelvis) in Bodies. */
	UPROPERTY()
	uint8 RootIndex = 0;

	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};

template<>
struct TStructOpsTypeTraits<FGMCMotionRagdollNetPose> : public TStructOpsTypeTraitsBase2<FGMCMotionRagdollNetPose>
{
	enum { WithNetSerializer = true };
};

/**
 * Networked active ragdoll for the GMC pawns, port of GASP58's SandboxCharacter_Mover_Ragdoll (ragdoll_system.md):
 *  - the SERVER decides (TriggerRagdoll, ExitRagdoll, death through the GMAS State.Dead tag) and simulates the reference
 *    ragdoll, an active one driven by a Physics Control Component toward the anim graph's ragdoll poses (reach / flail
 *    in the air, injury / rolling poses on the ground), with GASP's behaviours (impact prediction, auto roll on slopes,
 *    fall speed cap, get-up of the bots once still);
 *  - the bodies are streamed to every client (unreliable multicast, PoseSendRate) and each client steers its own
 *    simulated bodies toward them (velocity steering, interpolated InterpolationDelay in the past), so the pose is the
 *    same everywhere while still colliding locally;
 *  - the capsule follows the body through UGMCMotion's ragdoll movement mode (GMC-bound, server-decided);
 *  - the get-up montage is picked by the server (face up / face down / rolling) and played on every machine from a
 *    snapshot of the ragdoll pose (ABP "Ragdoll States" + MovementAction on BP_GMCMovement);
 *  - death = the same ragdoll, limp, never getting up; the weapon is dropped on death (BP DropHeldItems) and kept in
 *    the right hand otherwise.
 * Lives on the pawn (BP_GMC_Pawn).
 */
UCLASS(ClassGroup = (GMC), meta = (BlueprintSpawnableComponent))
class GMCMOTIONRAGDOLL_API UGMCMotionRagdollComponent : public UActorComponent, public IGMCMotionControlLossSource
{
	GENERATED_BODY()

public:
	UGMCMotionRagdollComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Server: puts the pawn into ragdoll (ignored when dead or already ragdolling). */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ragdoll")
	void TriggerRagdoll(EGMCMotionRagdollInjury Injury, FVector Impulse, FVector ImpulseLocation);

	/** Server: gets up (ignored unless ragdolling). bForce skips the "on the floor" check. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ragdoll")
	void ExitRagdoll(bool bForce = false);

	/** Server: an impact (bullet, projectile, blow...) hit this living pawn. Knocked down: kick the closest body.
	 * Standing: the hit adds to the knockdown score (KnockdownBoneWeights) and knocks the pawn down past
	 * KnockdownThreshold, otherwise the hit limb plays a hit reaction. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ragdoll")
	void AddHitImpulse(FVector ImpactVelocity, FVector ImpactLocation, float ImpartedEnergy = 0.f);

	/** Server: a Health modifier (HealthDelta < 0) is about to be applied to Target as a ServerAuth effect (e.g. a shot).
	 * GMAS applies it once the owning client acks (~1 RTT), then GE_Dead waits for another ack: when the damage in
	 * flight is lethal the pawn dies now (ragdoll phase Dead + BP HandleDeath on every machine), GMAS catches up. Call
	 * it BEFORE applying the effect (the host's and bots' effects apply at once and consume the pending damage). */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ragdoll", meta = (DefaultToSelf = "Target"))
	static void RegisterPendingDamage(AActor* Target, float HealthDelta);

	UFUNCTION(BlueprintPure, Category = "Ragdoll")
	bool IsDead() const { return State.Phase == EGMCMotionRagdollPhase::Dead; }

	/** Death decided by the server as soon as the damage in flight is lethal (RegisterPendingDamage). Turn it off when
	 * lethal damage does not kill at once (e.g. a downed state first), or it ragdolls a pawn that is only going down. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Death")
	bool bEarlyServerDeath = true;

	/** Pending damage not applied by GMAS after this long is forgotten. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Death", meta = (ClampMin = 0.1))
	float PendingDamageLifetime = 2.f;

	/** Early death whose Health is still above 0 once GMAS applied the damage in flight (BP_GMC_Pawn applies GE_Dead
	 * on a Health <= 0 change, missed when a GE_Regen_Health tick lands in the same frame: 0.x HP left) or after
	 * DeathFallbackDelay (acks lost): the server applies DeathFallbackEffect so the GMAS death (GE_Dead, respawn)
	 * happens anyway. DeathFallbackCheckDelay: wait after the last damage in flight was applied. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Death", meta = (ClampMin = 0.5))
	float DeathFallbackDelay = 3.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Death", meta = (ClampMin = 0.))
	float DeathFallbackCheckDelay = 0.25f;

	/** Defaults to GE_InstantKill. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Death")
	TSubclassOf<UGMCAbilityEffect> DeathFallbackEffect;

	/** Knockdown by impacts (AddHitImpulse): each hit on a weighted bone adds ImpartedEnergy (J) x bone weight x (1 +
	 * speed / KnockdownSpeedReference); the score decays by KnockdownDecayPerSecond; past KnockdownThreshold the pawn falls
	 * into the ragdoll with the impact's kick. A 5.56 bullet at close range imparts 400-1500 J: a running pawn shot in the
	 * leg falls, a standing one takes two quick hits. 0 = off. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Knockdown")
	float KnockdownThreshold = 1500.f;

	/** Bones that count, and their weight: the hit body's bone, or its first listed parent (legs by default). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Knockdown")
	TMap<FName, float> KnockdownBoneWeights = {{TEXT("thigh_l"), 1.f}, {TEXT("thigh_r"), 1.f}, {TEXT("pelvis"), 0.5f}};

	/** Planar speed (cm/s) that doubles a hit's score: a running pawn trips more easily. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Knockdown", meta = (ClampMin = 1.))
	float KnockdownSpeedReference = 300.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Knockdown", meta = (ClampMin = 0.))
	float KnockdownDecayPerSecond = 750.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Knockdown")
	EGMCMotionRagdollInjury KnockdownInjury = EGMCMotionRagdollInjury::None;

	/** Kick of the bullet that knocks the pawn down (kg*cm/s, along the flattened bullet direction, slightly up). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Knockdown", meta = (ClampMin = 0.))
	float KnockdownImpulseStrength = 2500.f;

	/** Ragdoll camera (GASP58 CameraRig_Far_Ragdoll): while knocked down, getting up or dead, the local player's
	 * shoulder camera rig (component RagdollCameraRigName, BP_ShoulderCameraRig) pivots on the body (pelvis +
	 * RagdollCameraHeight, smoothed) with no shoulder offset and a longer arm; the player still orbits it. Back to its
	 * place once standing. Without it the rig stayed on the capsule with its shoulder offset: the body sat 35-58 deg off
	 * the camera axis, in a corner of the screen. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Camera")
	bool bRagdollCamera = true;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Camera")
	FName RagdollCameraRigName = TEXT("ShoulderCameraRig");

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Camera")
	float RagdollCameraHeight = 40.f;

	/** Camera distance while ragdolling, relative to the standing one (the rig sits ~3 m behind the capsule plus its
	 * arm: the arm becomes that whole distance x this scale). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Camera", meta = (ClampMin = 0.1))
	float RagdollCameraArmScale = 1.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Camera", meta = (ClampMin = 0.))
	float RagdollCameraPivotSpeed = 8.f;

	/** Hit reaction of a standing pawn that doesn't fall: the hit limb goes physical, takes the bullet's kick
	 * (HitReactionImpulse per 1000 J imparted, clamped to 0.3-2x), is pulled back to the animation by its controls for
	 * HitReactionDuration, then blends back over HitReactionBlendOut. Legs and head; arms and torso only when unarmed (the
	 * weapon stays on ik_hand_gun, the hands would leave it). Cosmetic, played by every machine. 0 = off. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|HitReaction", meta = (ClampMin = 0.))
	float HitReactionImpulse = 1500.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|HitReaction", meta = (ClampMin = 0.))
	float HitReactionDuration = 0.25f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|HitReaction", meta = (ClampMin = 0.01))
	float HitReactionBlendOut = 0.2f;

	/** Controls of the reacting limb (GASP58 PhysicalAnimation profile values). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|HitReaction", meta = (ClampMin = 0.))
	float HitReactionParentStrength = 15.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|HitReaction", meta = (ClampMin = 0.))
	float HitReactionWorldStrength = 3.f;

	/** Kick of a bullet on a knocked-down pawn (kg*cm/s, along the flattened bullet direction). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Behaviour")
	float HitImpulseStrength = 6000.f;

	/** Owning client or server: ragdoll when standing, get up when ragdolling (input). */
	UFUNCTION(BlueprintCallable, Category = "Ragdoll")
	void RequestToggleRagdoll();

	UFUNCTION(BlueprintPure, Category = "Ragdoll")
	EGMCMotionRagdollPhase GetPhase() const { return State.Phase; }

	// --- Character interactions (GASP58 TryCharacterInteraction) ----------------------------------------------------

	/** Owning client or server: interaction with the characters in front (input). */
	UFUNCTION(BlueprintCallable, Category = "Ragdoll|Interaction")
	void RequestInteraction(EGMCMotionInteractionType Type);

	/** Server: GASP58 Try_MultiCharacterInteraction. The chooser rows (InteractionDatabases) give the databases for Type
	 * and this pawn's speed; MotionMatchMulti searches them with this pawn as initiator and every interactable
	 * character (CheckForInteractableCharacters, or only Target when given) as target, and every character of the best
	 * result plays its role's montage from the matched time. False when nothing started. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Ragdoll|Interaction")
	bool TryInteraction(EGMCMotionInteractionType Type, AActor* Target = nullptr);

	/** GASP58 CanDoCharacterInteraction: on the ground, no montage in DefaultSlot, not aiming. */
	UFUNCTION(BlueprintPure, Category = "Ragdoll|Interaction")
	bool CanDoCharacterInteraction() const;

	/** GASP58 CanBeInteractedWith: on the ground (GASP's graph returns only that). */
	UFUNCTION(BlueprintPure, Category = "Ragdoll|Interaction")
	bool CanBeInteractedWith() const;

	/** GASP58 CheckForInteractableCharacters: characters within 2 m, 50 cm up or down, 60 deg in front, unobstructed. */
	UFUNCTION(BlueprintCallable, Category = "Ragdoll|Interaction")
	TArray<AActor*> FindInteractableCharacters() const;

	/** True while the actor's ragdoll component simulates the body as a ragdoll (knocked down or dead). The physics
	 * layer of the default profile doesn't count: use this, not IsSimulatingPhysics, to leave the mesh alone. */
	UFUNCTION(BlueprintPure, Category = "Ragdoll")
	static bool IsActorRagdolling(const AActor* Actor);

	/** True while the actor's ragdoll is up (knocked down or dead). Use it to gate logic that must run once per death
	 * (e.g. a death chain on the movement Blueprint), not every tick on a corpse. */
	UFUNCTION(BlueprintPure, Category = "Ragdoll", meta = (DefaultToSelf = "Actor"))
	static bool IsDeathRagdollActive(const AActor* Actor);

	/** The replicated phase (None, Ragdoll = knocked down, GettingUp, Dead, Interacting). */
	UFUNCTION(BlueprintPure, Category = "Ragdoll")
	EGMCMotionRagdollPhase GetRagdollPhase() const { return State.Phase; }

	UFUNCTION(BlueprintPure, Category = "Ragdoll")
	bool IsRagdolling() const { return State.Phase == EGMCMotionRagdollPhase::Ragdoll || State.Phase == EGMCMotionRagdollPhase::Dead; }

	/** Knocked down, getting up, or the target of a shove / tackle / takedown - the player has no control of the
	 * pawn until this turns false. The initiator of an interaction is still in control. UGMCMotion holds its
	 * KnockdownCameraYawRange on this (IGMCMotionControlLossSource). */
	UFUNCTION(BlueprintPure, Category = "Ragdoll")
	bool IsOutOfPlayerControl() const
	{
		return State.Phase == EGMCMotionRagdollPhase::Ragdoll || State.Phase == EGMCMotionRagdollPhase::GettingUp
			|| (State.Phase == EGMCMotionRagdollPhase::Interacting && !State.bInitiator);
	}

	virtual bool IsPlayerControlLost() const override { return IsOutOfPlayerControl(); }

	// --- Setup -------------------------------------------------------------------------------------------------------

	/** Byte / enum property of the movement component the ragdoll writes its action to (GMCLocomotion's MovementAction
	 * enum: 0 None, 1 Ragdoll, 2 GettingUp), read by the ABP. None = write nothing. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	FName MovementActionProperty = TEXT("MovementAction");

	/** The MovementActionProperty values for none / ragdoll / getting up. The defaults are GMCLocomotion's enum; change
	 * them when the property is another enum (e.g. an ALS-style MovementAction: None 0, Mantling 1, Ragdoll 2,
	 * GettingUp 3). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	uint8 MovementActionNoneValue = 0;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	uint8 MovementActionRagdollValue = 1;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	uint8 MovementActionGettingUpValue = 2;

	/** Name of the third-person skeletal mesh component that ragdolls. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	FName MeshName = TEXT("Mesh");

	/** Controls and body modifiers (profiles Kinematic / PhysicalAnimation / Ragdoll, GASP58 PCA_SandboxCharacter). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	TObjectPtr<UPhysicsControlAsset> PhysicsControlAsset;

	/** Profile outside the ragdoll. GASP58 runs "PhysicalAnimation" (DDCVar.CharacterPhysics.DefaultProfile = 1), an
	 * always-on physics layer; off here by default: GMC ticks the mesh after physics (TG_PostPhysics), so the controls get
	 * their targets one step late and the simulated body trailed the capsule while walking (pelvis 10 cm, hands 20-28 cm
	 * off a held weapon), and the bots' skipped evaluations (update rate optimizations) made their bodies tremble.
	 * "Kinematic": the bones stay on the animation outside the ragdoll, as before the port. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	FName DefaultPhysicsProfile = TEXT("Kinematic");

	/** GASP58 SetPhysicsProfile: the legs don't collide with each other outside the ragdoll. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	TArray<FName> LeftLegBones = {TEXT("thigh_l"), TEXT("calf_l"), TEXT("foot_l")};

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	TArray<FName> RightLegBones = {TEXT("thigh_r"), TEXT("calf_r"), TEXT("foot_r")};

	/** The first-person camera hangs on the head bone (and a weapon system may aim from it): a locally controlled pawn in first person
	 * keeps these bodies kinematic in the default profile. Not in GASP58 (third person only). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	bool bKinematicHeadInFirstPerson = true;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	TArray<FName> FirstPersonKinematicBodies = {TEXT("neck_01"), TEXT("head")};

	/** Collision profile of the mesh while ragdolling. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	FName RagdollCollisionProfile = TEXT("Ragdoll");

	/** GMAS effect granting State.Ragdoll while knocked down (blocks the abilities). Removed through EffectTag. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	TSubclassOf<UGMCAbilityEffect> RagdollEffect;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	FGameplayTag RagdollEffectTag;

	/** Abilities ended when the ragdoll starts (on the server and the owner). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	TArray<FGameplayTag> AbilitiesToEnd;

	/** Toggle (standing: ragdoll, ragdolling: get up), and get up (GASP: jump). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	TObjectPtr<UInputAction> ToggleRagdollAction;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Setup")
	TObjectPtr<UInputAction> GetUpAction;

	// --- Interactions ----------------------------------------------------------------------------------------------------

	/** GASP58 CHT_CharacterInteractionPSDs rows (the default databases are loaded when empty and
	 * bLoadDefaultInteractionDatabases is set). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	TArray<FGMCMotionInteractionDatabase> InteractionDatabases;

	/** Load GASP58's interaction databases (shove, tackle, takedown) from their /Game paths when InteractionDatabases is
	 * empty. Turn it off in a project without that content. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	bool bLoadDefaultInteractionDatabases = true;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	TObjectPtr<UInputAction> ShoveAction;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	TObjectPtr<UInputAction> TakedownAction;

	/** GASP58 CheckForInteractableCharacters: sphere radius, height tolerance, half angle in front. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	float InteractionRange = 200.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	float InteractionMaxHeight = 50.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	float InteractionHalfAngle = 60.f;

	/** Sockets of the hands driven by the interaction constraints (GASP58 Update_InteractionConstraints). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	FName InteractionHandL = TEXT("hand_l");

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Interaction")
	FName InteractionHandR = TEXT("hand_r");

	// --- Get-up --------------------------------------------------------------------------------------------------------

	/** GASP58 CHT_GetUpMontages: the get-ups are motion matched (schema PSS_Ragdoll) against the ragdoll's pose, put
	 * into the pose history (OverridePoseHistoryFromOwningMesh). Rolling get-ups when the centre of mass moves faster
	 * than RollingGetUpSpeed (chooser column RollingGetup), the others otherwise. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	TObjectPtr<UPoseSearchDatabase> GetUpDatabase;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	TObjectPtr<UPoseSearchDatabase> RollingGetUpDatabase;

	/** Tag of the anim graph's Pose History node. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	FName PoseHistoryName = TEXT("PoseHistory");

	/** Fallback get-ups when the pose search finds nothing (no database). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	TObjectPtr<UAnimMontage> GetUpFaceDown;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	TObjectPtr<UAnimMontage> GetUpFaceUp;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	TObjectPtr<UAnimMontage> GetUpRollLeft;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	TObjectPtr<UAnimMontage> GetUpRollRight;

	/** Horizontal centre of mass speed over which the rolling get-ups are used (GASP: 150). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	float RollingGetUpSpeed = 150.f;

	/** Bots get up after staying under BotGetUpSpeed for BotGetUpDelay seconds (GASP: 10 cm/s, 3 s). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	float BotGetUpSpeed = 10.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	float BotGetUpDelay = 3.f;

	/** Not in GASP (no bots): a body that never settles (lying across an edge) gets up after this long, under the players'
	 *  conditions (floor, PlayerGetUpMaxSpeed). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	float BotGetUpMaxDelay = 8.f;

	/** A bot still down after this long gets up whatever its body is doing (0 = never). BotGetUpMaxDelay only retries a
	 * non-forced exit, which refuses while the body moves: a body that cannot settle (spawned inside a prop's collision -
	 * every body at 1-5 m/s, never moving, measured in CLEAR) stays down for good without this. Matters for hordes. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp", meta = (ClampMin = 0.))
	float BotForceGetUpDelay = 0.f;

	/** Sockets of the mesh whose attached items are held in the hand. A knockdown moves them onto HeldItemHandBone,
	 * keeping their exact held offset, and the end of the get-up puts them back. Needed for any socket on a bone with no
	 * physics body (e.g. ik_hand_gun, an IK bone under ik_hand_root): during the ragdoll it follows the animation, and a
	 * gun attached there floated ~90 cm from the simulated hand (measured in CLEAR). Empty = off. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|HeldItems")
	TArray<FName> HeldItemSockets;

	/** The physics-simulated bone held items ride during a knockdown. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|HeldItems")
	FName HeldItemHandBone = TEXT("hand_r");

	/** Players get up the way bots do (still for BotGetUpDelay, or BotGetUpMaxDelay) instead of asking (GetUpAction). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	bool bPlayersGetUpAutomatically = false;

	/** Players can only get up on the floor, under this speed. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|GetUp")
	float PlayerGetUpMaxSpeed = 100.f;

	// --- Behaviours (server) -----------------------------------------------------------------------------------------

	/** Socket the impact prediction starts from (GASP: RagdollTrace, on spine_03). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Behaviour")
	FName TraceSocket = TEXT("RagdollTrace");

	/** Body whose velocity is the ragdoll's speed, and where the roll torque goes (GASP: spine_05). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Behaviour")
	FName SpineBone = TEXT("spine_05");

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Behaviour")
	FName PelvisBone = TEXT("pelvis");

	/** Floor angles (deg) between which the auto roll ramps from 0 to full (GASP: 20..30). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Behaviour")
	FVector2D AutoRollAngles = FVector2D(20.f, 30.f);

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Behaviour")
	float RollTorque = 100000.f;

	/** Gravity is cut past this fall speed so a long fall can't tunnel through the floor (GASP: 2500). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Behaviour")
	float MaxFallSpeed = 2500.f;

	/** Gravity multiplier of the ragdoll (0 in a zero-g zone). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ragdoll|Behaviour")
	float RagdollGravityMultiplier = 1.f;

	/** Seconds the capsule keeps its yaw after the ragdoll starts (GASP: 0.25). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Behaviour")
	float FreezeCapsuleRotationTime = 0.25f;

	// --- Network ---------------------------------------------------------------------------------------------------------

	/** Body samples sent per second while the ragdoll moves. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network", meta = (ClampMin = 1.))
	float PoseSendRate = 15.f;

	/** How far in the past the clients show the server's bodies (covers the send interval and jitter). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network", meta = (ClampMin = 0.))
	float InterpolationDelay = 0.15f;

	/** At the ragdoll start the delay grows from 0 to InterpolationDelay over this time (the server's bodies play ~15 %
	 * slower meanwhile); until the first samples cover the render time the bodies simulate freely. Without it the
	 * client body stood still for InterpolationDelay at every start (the only sample was the standing pose). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network", meta = (ClampMin = 0.))
	float InterpolationDelayRamp = 1.f;

	/** Time for a client body to close its gap to the server's (velocity steering). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network", meta = (ClampMin = 0.01))
	float SteeringTime = 0.08f;

	/** A client body farther than this from the server's is teleported. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network")
	float SnapDistance = 60.f;

	/** A corpse stops streaming once every body stays under this speed for SettleTime. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network")
	float SettleSpeed = 5.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network")
	float SettleTime = 1.f;

	/** Once settled, the server re-captures FinalPose when any body has moved more than this from it (cm), checked every
	 * FinalPoseRecheckInterval. A body still sliding under SettleSpeed kept going after the capture: a client held a final
	 * pose 9 cm (a foot 20 cm) off the server's corpse for good (measured in CLEAR, 2-player PIE). */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network", meta = (ClampMin = 0.1))
	float FinalPoseTolerance = 2.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Network", meta = (ClampMin = 0.05))
	float FinalPoseRecheckInterval = 0.5f;

	float FinalPoseRecheckTimer = 0.f;

	// --- Obstacles (GASP58 TraceForObstacles) --------------------------------------------------------------------------

	/** Server: a sphere from the neck to between the feet; an Obstacle object in it knocks the pawn down. Obstacles may
	 * brush the limbs, only one reaching the body line ragdolls. */
	UPROPERTY(EditAnywhere, Category = "Ragdoll|Obstacles")
	bool bTraceForObstacles = true;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Obstacles")
	TEnumAsByte<ECollisionChannel> ObstacleObjectChannel = ECC_GameTraceChannel6;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Obstacles")
	float ObstacleTraceRadius = 15.f;

	UPROPERTY(EditAnywhere, Category = "Ragdoll|Debug")
	bool bDebug = false;

protected:
	UFUNCTION()
	void OnRep_State();

	UFUNCTION()
	void OnRep_FinalPose();

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastPose(const FGMCMotionRagdollNetPose& Pose);

	UFUNCTION(Server, Reliable)
	void ServerRequestToggleRagdoll();

	UFUNCTION(Server, Reliable)
	void ServerRequestGetUp();

	UFUNCTION(Server, Reliable)
	void ServerRequestInteraction(EGMCMotionInteractionType Type);

	UFUNCTION(NetMulticast, Unreliable)
	void MulticastHitReaction(FName Bone, FVector_NetQuantize10 Impulse, FVector_NetQuantize Location);

	void OnShoveInput();
	void OnTakedownInput();

	UFUNCTION()
	void OnActiveTagsChanged(FGameplayTagContainer AddedTags, FGameplayTagContainer RemovedTags);

	UFUNCTION()
	void OnEffectApplied(UGMCAbilityEffect* AppliedEffect);

	// Server: phase Dead (GMAS State.Dead, or lethal damage still in flight).
	void ServerEnterDeath();
	// Every machine: the pawn's BP HandleDeath (weapon drop, abilities, FP mesh; guarded by its bIsDead) and the corpse.
	void RunOwnerDeathHandling();
	void AddPendingDamage(float HealthDelta);

	void OnToggleRagdollInput();
	void OnGetUpInput();

private:
	UPROPERTY(ReplicatedUsing = OnRep_State)
	FGMCMotionRagdollState State;

	/** Last pose of a settled corpse (reliable, late joiners). */
	UPROPERTY(ReplicatedUsing = OnRep_FinalPose)
	FGMCMotionRagdollNetPose FinalPose;

	UPROPERTY(Transient)
	TObjectPtr<UPhysicsControlComponent> PhysicsControl;

	USkeletalMeshComponent* GetMesh() const;
	UGMCMotion* GetMotion() const;
	UGMC_AbilitySystemComponent* GetAbilities() const;
	bool IsLocallyControlledPawn() const;
	double GetServerTime() const;

	void SetupPhysicsControl();
	/** Destroys the physics control component and restores the mesh's update mode, unconditionally. A standing mesh with
	 * no collision has no bodies: the controls are made per ragdoll, once the Ragdoll profile has created the bodies, and
	 * released when the bodies go away again. */
	void ReleasePhysicsControl();
	void ApplyPhase(EGMCMotionRagdollPhase NewPhase, EGMCMotionRagdollPhase OldPhase);
	void EnterRagdollLocal(bool bDead);
	void ExitRagdollLocal();
	/** HeldItemSockets -> HeldItemHandBone at the knockdown, and back once the get-up montage is over. */
	void RebindHeldItemsToHand();
	void RestoreHeldItems();
	struct FReboundHeldItem
	{
		TWeakObjectPtr<USceneComponent> Component;
		FName Socket;
		FTransform Relative;
	};
	TArray<FReboundHeldItem> ReboundHeldItems;
	TWeakObjectPtr<UAnimMontage> HeldItemsGetUpMontage;
	void FinishGetUp();
	void SetMovementAction(uint8 Action);
	void SetViewModeForRagdoll(bool bRagdoll);
	void BindInput();

	// Server behaviours (GASP58 Ragdoll_UpdateBehaviors).
	void UpdateBehaviours(float DeltaTime);
	void UpdateStrengths(float DeltaTime);
	UAnimMontage* PickGetUpMontage(float& OutStartTime);

	// Physics profiles (GASP58 SetPhysicsProfile / UpdateDefaultPhysicsProfileFromCvar).
	void SetPhysicsProfile(FName Profile);
	void ApplyDefaultPhysicsProfile();
	void UpdateKinematicBodies(bool bForce);
	void ApplyPhysicalCollision(bool bForce = false);
	void RestorePhysicalCollision();
	// Console switches applied live (gmc.Mesh.TickAfterGMCMoves, gmc.Ragdoll.PhysicsLayer).
	friend struct FGMCMotionRagdollCVars;
	void ApplyMeshTickOrder();
	void ApplyPhysicsLayerSwitch();
	bool bMeshTickChained = false;
	/** Update rate optimizations off whenever the animation drives the capsule or the bodies (any phase but None, the
	 * physics layer); the mesh's own setting is restored once back to None without the layer. */
	void UpdateMeshUpdateRate();
	bool bUROOverridden = false;
	bool bSavedURO = false;
	TEnumAsByte<ETickingGroup> SavedMeshTickGroup = TG_PostPhysics;
	FName ConfiguredPhysicsProfile;
	// The mesh collision before the physics layer's physical collision (captured once, restored by the live switch).
	bool bStandingCollisionCaptured = false;
	TEnumAsByte<ECollisionChannel> StandingMeshObjectType = ECC_Pawn;
	TEnumAsByte<ECollisionEnabled::Type> StandingMeshCollisionEnabled = ECollisionEnabled::QueryOnly;
	FCollisionResponseContainer StandingMeshResponses;
	// Momentum at the ragdoll start. Kinematic bodies are teleported onto the animation (no velocity): the ragdoll of a
	// sprinting pawn (585 cm/s) started still and slid ~30 cm, where GASP's (simulated) bodies keep their momentum.
	// Every tick outside the ragdoll the body bones' transforms are cached; the ragdoll gives each body the velocity of
	// its last step.
	/** The camera sweep ignores the pawn and its attached actors, but the ragdoll mesh is detached from the capsule:
	 * the held weapon (Holdable blocks Camera) and the helmet gear hanging on it were no longer ignored and the camera
	 * went into the body and back (529 -> 25 -> 417 cm). They ignore Camera while ragdolling. */
	void SetMeshAttachmentsIgnoreCamera(bool bIgnore);
	TArray<TPair<TWeakObjectPtr<UPrimitiveComponent>, TEnumAsByte<ECollisionResponse>>> SavedAttachmentCameraResponses;
	void UpdateRagdollCamera(float DeltaTime);
	TWeakObjectPtr<USceneComponent> RagdollCameraRig;
	bool bRagdollCameraActive = false;
	FVector RagdollCameraRelative = FVector::ZeroVector;
	FVector RagdollCameraPivot = FVector::ZeroVector;
	double SavedRigShoulderOffset = 0.;
	double SavedRigArmLength = 0.;
	double SavedRigVerticalOffset = 0.;
	void CacheBodyMotion(float DeltaTime);
	void ApplyInheritedBodyVelocities();
	TArray<FName> MotionBodyBones;
	TArray<FTransform> MotionPrevTM;
	TArray<FTransform> MotionCurTM;
	float MotionDeltaTime = 0.f;
	double MotionCacheTime = -1.;
	int32 MotionSamples = 0;
	// Hit reaction of a standing pawn (every machine).
	void StartHitReaction(FName Bone, const FVector& Impulse, const FVector& Location);
	void UpdateHitReaction(float DeltaTime);
	TArray<FName> HitReactionLimbs;
	float HitReactionTimeLeft = 0.f;
	bool bPendingHitImpulse = false;
	/** The reaction gave the mesh physical collision (the physics blend needs it): put back once standing again. */
	bool bHitReactionCollision = false;
	FName PendingHitBone;
	FVector PendingHitImpulse = FVector::ZeroVector;
	FVector PendingHitLocation = FVector::ZeroVector;
	// Knockdown by bullets.
	bool TryHitKnockdown(const FVector& ImpactVelocity, const FVector& ImpactLocation, float ImpartedEnergy);
	float KnockdownScore = 0.f;
	double KnockdownScoreTime = 0.;
	FName AppliedPhysicsProfile;
	bool bDefaultProfilePending = false;
	bool bHeadKinematic = false;
	bool bPhysicalCollisionApplied = false;
	bool FindFloor(FVector& OutLocation, FVector& OutNormal);

	// First bone at or above Bone that has a physics body (the hitbox physics asset has no spine_05).
	FName ResolveBodyBone(FName Bone) const;
	void TraceForObstacles();
	FName SpineBody;

	// Capsule target from the body (GASP58 Get_RagdollTransform / Get_RagdollTargetOrientation).
	void UpdateCapsuleTarget(float DeltaTime);

	// Anim graph inputs (ABP variables, by name).
	void PushAnimValues();

	// Pose streaming.
	bool CapturePose(FGMCMotionRagdollNetPose& OutPose) const;
	void ServerStreamPose(float DeltaTime);
	void ClientFollowPose(float DeltaTime);
	bool SamplePose(double Time, TArray<FTransform>& OutBodies) const;
	void ApplyPoseInstant(const FGMCMotionRagdollNetPose& Pose);

	FGameplayTag DeadTag;
	FGameplayTag HealthTag;
	struct FPendingDamage
	{
		float Delta = 0.f;
		double Time = 0.;
	};
	TArray<FPendingDamage> PendingDamage;
	// Server: when to check an early death against GMAS's Health (-1 = nothing to check).
	double DeathFallbackTime = -1.;
	bool bCapsuleCollisionUntilDeadTag = false;
	double LocalRagdollStartTime = 0.;
	EGMCMotionRagdollPhase LocalPhase = EGMCMotionRagdollPhase::None;
	uint8 LocalSerial = 0;
	bool bLocalRagdollActive = false;
	bool bPhysicsControlReady = false;
	// The mesh's PhysicsTransformUpdateMode before SetupPhysicsControl changed it.
	bool bSavedPhysicsTransformUpdateMode = false;
	EPhysicsTransformUpdateMode::Type SavedPhysicsTransformUpdateMode = EPhysicsTransformUpdateMode::SimulationUpatesComponentTransform;
	bool bInputBound = false;

	// Saved at ragdoll start, restored at the get-up.
	FName SavedMeshProfile;
	// A game-side hitbox setup may be a custom response set, not a profile: saved and put back whole.
	FCollisionResponseContainer SavedMeshResponses;
	ECollisionChannel SavedMeshObjectType = ECC_PhysicsBody;
	ECollisionEnabled::Type SavedMeshCollisionEnabled = ECollisionEnabled::QueryOnly;
	// The physics sync moves the simulated mesh component away from the capsule: put back at the get-up.
	FTransform SavedMeshRelative = FTransform::Identity;
	// Detached while ragdolling: GMC teleports the capsule on corrections / replays / state restores, and an attached
	// simulating mesh is dragged along (TeleportPhysics moves simulated bodies) -> runaway with the capsule-follow.
	TWeakObjectPtr<USceneComponent> SavedMeshParent;
	FName SavedMeshSocket;
	float DebugTimer = 0.f;
	int32 DebugPosesReceived = 0;
	ECollisionResponse SavedCapsulePawnResponse = ECR_Block;
	ECollisionResponse SavedCapsuleCameraResponse = ECR_Block;
	bool bSavedFirstPersonView = false;
	TWeakObjectPtr<USceneComponent> SavedSmoothComponent;

	// Server behaviours state.
	FVector ComVelocity = FVector::ZeroVector;
	FVector LastCom = FVector::ZeroVector;
	bool bHasLastCom = false;
	FVector SpineVelocity = FVector::ZeroVector;
	float RagdollSpeed = 0.f;
	float TimeToImpact = 0.f;
	FVector2D ImpactDirection = FVector2D::ZeroVector;
	/** GASP58's RagdollTrace socket (spine_03, X = chest forward, Z = up, 20 cm up the spine) for meshes without it: the
	 * Military mesh has none and the spine_05 fallback (X up the spine) turned the impact direction by 90 deg, so the
	 * ragdoll poses (reach / flail / on ground) were picked from wrong yaw / pitch: arms up, legs straight, a lying pose
	 * no get-up matched (pose search cost 23-27 vs 0.04 in GASP, opposite get-up, jolt at the start of the get-up). */
	FTransform GetRagdollTraceTransform() const;
	mutable bool bTraceFrameCached = false;
	mutable FTransform TraceFrameRelativeToSpine = FTransform::Identity;
	FVector AutoRollForce = FVector::ZeroVector;
	FVector RollForce = FVector::ZeroVector;
	bool bHasFloor = false;
	FVector FloorLocation = FVector::ZeroVector;
	FVector FloorNormal = FVector::UpVector;
	float StillTime = 0.f;
	float RagdollTime = 0.f;
	float GetUpTimeLeft = 0.f;

	// Get-up root motion: the montage's root moves the capsule (root motion is not consumed outside GMC).
	FTransform GetUpStartActor = FTransform::Identity;
	FQuat GetUpMeshRotation = FQuat::Identity;
	void UpdateMontageCapsule();

	// Interactions: the server starts them, every machine plays them.
	bool StartInteractionFromResult(const struct FPoseSearchBlueprintResult& Result, AActor* InPartner);
	void EnterInteractionLocal();
	void ExitInteractionLocal();
	void FinishInteraction();
	/** Every machine: the montage's root motion moves the capsule, skew-warped over the motion warping window
	 * (URootMotionModifier_SkewWarp::ProcessRootMotion, with the settings of the montage's notify). */
	void UpdateInteractionMotion();
	/** Every machine: GASP58 Update_InteractionConstraints (hand IK targets of the ABP's full body IK). */
	void UpdateInteractionConstraints(float DeltaTime);
	void ClearInteractionConstraints();
	void UpdateInteractionIKBlendOut(float DeltaTime);
	float InteractionTimeLeft = 0.f;
	float InteractionRagdollTime = -1.f;
	FTransform InteractionRoot = FTransform::Identity;
	float InteractionLastPosition = 0.f;
	bool bHasWarpWindow = false;
	bool bWarpStarted = false;
	float WarpStartTime = 0.f;
	float WarpEndTime = 0.f;
	float WarpActualStartTime = 0.f;
	FTransform WarpStartTransform = FTransform::Identity;
	bool bWarpTranslation = true;
	bool bWarpIgnoreZ = true;
	bool bWarpRotation = true;
	float WarpRotationTimeMultiplier = 1.f;
	uint8 WarpEasing = 0;
	/** UPoseSearchInteractionLibrary::UpdateConstraints' state (FPoseSearchConstraint), computed here: the engine function
	 * reads its sampled poses with the MESH bone index where they are indexed by SKELETON bone, and our SK_Mannequin
	 * (merged military parts: 370 bones) doesn't have the mesh's bone order (hand_r: 146 in the mesh, 66 in the skeleton),
	 * so the hands' targets came from other bones. Same math, skeleton indices. */
	struct FInteractionIKConstraint
	{
		FName FromRole, FromSocket, ToRole, ToSocket;
		float TranslationWeight = 0.f, RotationWeight = 0.f, RampUpTime = 0.2f, CoolDownTime = 0.2f, DesiredReach = 0.f;
		FTransform FromTransform = FTransform::Identity;
		FTransform ToTransform = FTransform::Identity;
	};
	TArray<FInteractionIKConstraint> InteractionConstraints;
	bool bInteractionIKActive = false;
	/** ABP MMI_IK_Alpha: GASP58's Blend Poses by bool (Enable_MMI_IK, 0.1 s in / 0.2 s out) as the Control Rig's alpha. */
	float InteractionIKAlpha = 0.f;
	ECollisionResponse SavedInteractionPawnResponse = ECR_Block;
	TWeakObjectPtr<USkeletalMeshComponent> InteractionPrerequisiteMesh;

	// Per-limb strengths (GASP58 Ragdoll_Strength_* curves), smoothed.
	FVector4 Strengths = FVector4(1., 1., 1., 1.);

	// Streaming state.
	float SendAccumulator = 0.f;
	float SettledFor = 0.f;
	bool bSettled = false;
	TArray<FGMCMotionRagdollNetPose> PoseBuffer;
	bool bSnapOnNextPose = false;
};
