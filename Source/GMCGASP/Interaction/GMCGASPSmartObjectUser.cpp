#include "Interaction/GMCGASPSmartObjectUser.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/GMCGASPRagdollComponent.h"
#include "Components/GMCMotion.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "SmartObjectBlueprintFunctionLibrary.h"
#include "SmartObjectComponent.h"
#include "SmartObjectRequestTypes.h"
#include "SmartObjectSubsystem.h"
#include "SmartObjectTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogGMCGASPSmartObject, Log, All);

namespace
{
	constexpr float PathSampleRate = 30.f;

	FTransform YawOnly(const FTransform& Transform)
	{
		return FTransform(FRotator(0., Transform.Rotator().Yaw, 0.), Transform.GetLocation());
	}

	FTransform SamplePath(const TArray<FTransform>& Samples, float Time)
	{
		if (Samples.IsEmpty())
		{
			return FTransform::Identity;
		}
		const float Index = FMath::Clamp(Time * PathSampleRate, 0.f, static_cast<float>(Samples.Num() - 1));
		const int32 A = FMath::FloorToInt32(Index);
		const int32 B = FMath::Min(A + 1, Samples.Num() - 1);
		const float Alpha = Index - A;
		FTransform Result;
		Result.SetLocation(FMath::Lerp(Samples[A].GetLocation(), Samples[B].GetLocation(), static_cast<double>(Alpha)));
		Result.SetRotation(FQuat::Slerp(Samples[A].GetRotation(), Samples[B].GetRotation(), Alpha));
		return Result;
	}
}

UGMCGASPSmartObjectUser::UGMCGASPSmartObjectUser()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	SetIsReplicatedByDefault(true);
}

void UGMCGASPSmartObjectUser::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UGMCGASPSmartObjectUser, State);
}

void UGMCGASPSmartObjectUser::BeginPlay()
{
	Super::BeginPlay();
	// Tags resolved at runtime rather than in the constructor (the tag tables are not loaded for the CDO).
	if (UserTags.IsEmpty())
	{
		const FGameplayTag PlayerTag = FGameplayTag::RequestGameplayTag(TEXT("SmartObject.ObjectType.Player"), false);
		if (PlayerTag.IsValid())
		{
			UserTags.AddTag(PlayerTag);
		}
	}
}

void UGMCGASPSmartObjectUser::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ReleaseClaim();
	Super::EndPlay(EndPlayReason);
}

UGMCMotion* UGMCGASPSmartObjectUser::GetMotion() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UGMCMotion>() : nullptr;
}

USkeletalMeshComponent* UGMCGASPSmartObjectUser::GetMesh() const
{
	// The body mesh by name: weapons and gear are skeletal meshes too.
	TInlineComponentArray<USkeletalMeshComponent*> Meshes(GetOwner());
	for (USkeletalMeshComponent* Candidate : Meshes)
	{
		if (Candidate->GetFName() == MeshName)
		{
			return Candidate;
		}
	}
	return nullptr;
}

bool UGMCGASPSmartObjectUser::IsRagdollBusy() const
{
	const UGMCGASPRagdollComponent* Ragdoll = GetOwner() ? GetOwner()->FindComponentByClass<UGMCGASPRagdollComponent>() : nullptr;
	return Ragdoll && Ragdoll->GetPhase() != EGMCGASPRagdollPhase::None;
}

bool UGMCGASPSmartObjectUser::CanUse() const
{
	const UGMCMotion* Motion = GetMotion();
	if (IsUsingSmartObject() || !Motion || !Motion->IsMovingOnGround() || IsRagdollBusy() || EntryMontages.IsEmpty())
	{
		return false;
	}
	// A crouch stands up first (RequestUse).
	return true;
}

void UGMCGASPSmartObjectUser::ClearCrouchInput()
{
	// GASP58 stands the character up before sitting. WantsToCrouch is the owner's input (BP_GMCMovement, bound
	// ClientAuth): cleared where the input lives, so the pawn is standing when it gets up from the bench.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	UGMCMotion* Motion = GetMotion();
	if (!Pawn || !Pawn->IsLocallyControlled() || !Motion)
	{
		return;
	}
	if (const FBoolProperty* Property = FindFProperty<FBoolProperty>(Motion->GetClass(), TEXT("WantsToCrouch")))
	{
		Property->SetPropertyValue_InContainer(Motion, false);
	}
}

//--------------------
// Claim (server)

bool UGMCGASPSmartObjectUser::HasFreeSlot(AActor* SmartObjectActor) const
{
	if (!SmartObjectActor || !USmartObjectSubsystem::GetCurrent(GetWorld()))
	{
		// Clients may have no SmartObject subsystem: the server checks again.
		return SmartObjectActor != nullptr;
	}
	FSmartObjectRequestFilter Filter;
	Filter.UserTags = UserTags;
	TArray<FSmartObjectRequestResult> Results;
	return USmartObjectBlueprintFunctionLibrary::FindSmartObjectsInActor(Filter, SmartObjectActor, Results, GetOwner()) && Results.Num() > 0;
}

bool UGMCGASPSmartObjectUser::TryUse(AActor* SmartObjectActor)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority() || !SmartObjectActor || !CanUse())
	{
		return false;
	}
	FSmartObjectRequestFilter Filter;
	Filter.UserTags = UserTags;
	TArray<FSmartObjectRequestResult> Results;
	USmartObjectBlueprintFunctionLibrary::FindSmartObjectsInActor(Filter, SmartObjectActor, Results, Owner);
	return ClaimClosestSlot(Results);
}

void UGMCGASPSmartObjectUser::RequestUseNearby()
{
	RequestUse(nullptr);
}

void UGMCGASPSmartObjectUser::RequestUse(AActor* SmartObjectActor)
{
	if (!CanUse())
	{
		return;
	}
	// GASP58 stands up before sitting. The entry montage blended straight from the crouch pose straightened the legs
	// faster than the pelvis rose: feet 9 cm into the ground during the blend-in. Stand up first, then ask the server.
	if (GetStance() != 0)
	{
		ClearCrouchInput();
		PendingUseActor = SmartObjectActor;
		bPendingUse = true;
		PendingUseTime = 0.f;
		PendingStandingTime = 0.f;
		return;
	}
	SendUse(SmartObjectActor);
}

void UGMCGASPSmartObjectUser::SendUse(AActor* SmartObjectActor)
{
	if (GetOwner()->HasAuthority())
	{
		SmartObjectActor ? TryUse(SmartObjectActor) : TryUseNearby();
	}
	else
	{
		ServerUse(SmartObjectActor);
	}
}

void UGMCGASPSmartObjectUser::ServerUse_Implementation(AActor* SmartObjectActor)
{
	SmartObjectActor ? TryUse(SmartObjectActor) : TryUseNearby();
}

void UGMCGASPSmartObjectUser::UpdatePendingUse(float DeltaTime)
{
	if (!bPendingUse)
	{
		return;
	}
	PendingUseTime += DeltaTime;
	if (GetStance() == 0)
	{
		PendingStandingTime += DeltaTime;
	}
	if (PendingStandingTime >= StandUpBeforeUseTime)
	{
		bPendingUse = false;
		SendUse(PendingUseActor.Get());
	}
	else if (PendingUseTime > StandUpBeforeUseTime + 1.f)
	{
		// Couldn't stand up (low ceiling): give up.
		bPendingUse = false;
	}
}

uint8 UGMCGASPSmartObjectUser::GetStance() const
{
	const UGMCMotion* Motion = GetMotion();
	const FByteProperty* Property = Motion ? FindFProperty<FByteProperty>(Motion->GetClass(), TEXT("Stance")) : nullptr;
	return Property ? Property->GetPropertyValue_InContainer(Motion) : 0;
}

bool UGMCGASPSmartObjectUser::TryUseNearby()
{
	AActor* Owner = GetOwner();
	const USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(GetWorld());
	if (!Owner || !Owner->HasAuthority() || !Subsystem || !CanUse())
	{
		return false;
	}
	// GASP58 AC_SmartObjectAnimation traces a box in front of the pawn; any free slot within MaxUseDistance here.
	FSmartObjectRequestFilter Filter;
	Filter.UserTags = UserTags;
	const FVector Location = Owner->GetActorLocation();
	const FSmartObjectRequest Request(FBox(Location - FVector(MaxUseDistance), Location + FVector(MaxUseDistance)), Filter);
	TArray<FSmartObjectRequestResult> Results;
	Subsystem->FindSmartObjects(Request, Results, FConstStructView::Make(FSmartObjectActorUserData(Owner)));
	return ClaimClosestSlot(Results);
}

bool UGMCGASPSmartObjectUser::ClaimClosestSlot(const TArray<FSmartObjectRequestResult>& Results)
{
	AActor* Owner = GetOwner();
	USmartObjectSubsystem* Subsystem = USmartObjectSubsystem::GetCurrent(GetWorld());
	if (!Owner || !Subsystem || Results.IsEmpty())
	{
		return false;
	}

	// Closest free slot (GASP58 Find Closest Free Slot).
	const FVector PawnLocation = Owner->GetActorLocation();
	const FSmartObjectRequestResult* Best = nullptr;
	FTransform BestTransform;
	double BestDistSq = FMath::Square(MaxUseDistance);
	for (const FSmartObjectRequestResult& Result : Results)
	{
		const TOptional<FTransform> SlotTransform = Subsystem->GetSlotTransform(Result);
		if (!SlotTransform.IsSet())
		{
			continue;
		}
		const double DistSq = FVector::DistSquared2D(SlotTransform->GetLocation(), PawnLocation);
		if (DistSq < BestDistSq)
		{
			Best = &Result;
			BestTransform = YawOnly(*SlotTransform);
			BestDistSq = DistSq;
		}
	}
	if (!Best)
	{
		return false;
	}
	// The montages are authored with the root on the floor: the seat is at the height of the floor the pawn stands on
	// (the slot sits a few cm above it on BP_SmartBench, which left the capsule high and dropped it at the end of the exit).
	if (const USkeletalMeshComponent* Mesh = GetMesh())
	{
		FVector SeatLocation = BestTransform.GetLocation();
		SeatLocation.Z = Mesh->GetComponentLocation().Z;
		BestTransform.SetLocation(SeatLocation);
	}

	UAnimMontage* Montage = nullptr;
	float StartTime = 0.f;
	if (!ChooseEntry(Owner->GetActorTransform(), BestTransform, Montage, StartTime))
	{
		return false;
	}

	ClaimHandle = USmartObjectBlueprintFunctionLibrary::MarkSmartObjectSlotAsClaimed(this, Best->SlotHandle, Owner);
	if (!ClaimHandle.IsValid())
	{
		return false;
	}

	State.SlotLocation = BestTransform.GetLocation();
	State.SlotYaw = BestTransform.Rotator().Yaw;
	// At the feet: the capsule switches to its standing height when the entry starts (from a crouch: 60 -> 90 cm half
	// height around its centre), a capsule-centre gap would sink the feet 30 cm and take the whole fade to come back.
	const USkeletalMeshComponent* StartMesh = GetMesh();
	State.StartLocation = StartMesh ? StartMesh->GetComponentLocation() : Owner->GetActorLocation();
	State.StartYaw = Owner->GetActorRotation().Yaw;
	if (UGMCMotion* Motion = GetMotion())
	{
		Motion->SetRagdollMode(true);
	}
	SetPhase(EGMCGASPSmartObjectPhase::Entering, Montage, StartTime);
	UE_LOG(LogGMCGASPSmartObject, Log, TEXT("%s uses a slot of %s: %s from %.2f s"), *Owner->GetName(),
		*GetNameSafe(Subsystem->GetSmartObjectComponentByRequestResult(*Best)), *GetNameSafe(Montage), StartTime);
	return true;
}

void UGMCGASPSmartObjectUser::ReleaseClaim()
{
	if (ClaimHandle.IsValid())
	{
		USmartObjectBlueprintFunctionLibrary::MarkSmartObjectSlotAsFree(this, ClaimHandle);
		ClaimHandle.Invalidate();
	}
}

//--------------------
// Phases

void UGMCGASPSmartObjectUser::RequestExit()
{
	if (State.Phase != EGMCGASPSmartObjectPhase::Seated)
	{
		return;
	}
	if (GetOwner()->HasAuthority())
	{
		ServerExit(false);
	}
	else
	{
		ServerRequestExit(false);
	}
}

void UGMCGASPSmartObjectUser::ServerRequestExit_Implementation(bool bWalk)
{
	ServerExit(bWalk);
}

void UGMCGASPSmartObjectUser::ServerExit(bool bWalk)
{
	if (State.Phase != EGMCGASPSmartObjectPhase::Seated)
	{
		return;
	}
	UAnimMontage* Montage = bWalk && ExitWalkMontage ? ExitWalkMontage : ExitStandMontage;
	if (!Montage)
	{
		ServerFinish(false);
		return;
	}
	SetPhase(EGMCGASPSmartObjectPhase::Exiting, Montage, 0.f);
}

void UGMCGASPSmartObjectUser::ServerFinish(bool bKeepMovementMode)
{
	ReleaseClaim();
	if (!bKeepMovementMode)
	{
		if (UGMCMotion* Motion = GetMotion())
		{
			Motion->SetRagdollMode(false);
		}
	}
	SetPhase(EGMCGASPSmartObjectPhase::None, nullptr, 0.f);
}

void UGMCGASPSmartObjectUser::SetPhase(EGMCGASPSmartObjectPhase Phase, UAnimMontage* Montage, float StartTime)
{
	State.Phase = Phase;
	State.Montage = Montage;
	State.MontageStartTime = StartTime;
	++State.Serial;
	GetOwner()->ForceNetUpdate();
	ApplyState();
}

void UGMCGASPSmartObjectUser::OnRep_State()
{
	ApplyState();
}

void UGMCGASPSmartObjectUser::ApplyState()
{
	if (AppliedSerial == State.Serial)
	{
		return;
	}
	AppliedSerial = State.Serial;
	LocalTime = State.MontageStartTime;
	if (State.Phase == EGMCGASPSmartObjectPhase::Entering)
	{
		ClearCrouchInput();
	}
	// Target set at once: the first move in the capsule-follow mode can run before this component ticks, and without a
	// target the capsule stayed put while the stance grew it from crouch to standing height around its centre (mesh
	// 30 cm into the ground for a frame, then dragged by Offset Root Bone).
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (State.Phase != EGMCGASPSmartObjectPhase::None && Pawn && (Pawn->HasAuthority() || Pawn->IsLocallyControlled()))
	{
		UpdateCapsuleTarget();
	}
	if (State.Phase != EGMCGASPSmartObjectPhase::Seated)
	{
		bSeatedInputReleased = false;
	}

	USkeletalMeshComponent* Mesh = GetMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return;
	}
	if (State.Montage)
	{
		AnimInstance->Montage_Play(State.Montage, 1.f, EMontagePlayReturnType::MontageLength, State.MontageStartTime, true);
	}
	else if (!IsRagdollBusy())
	{
		// Back to locomotion (the ragdoll, if it took over, plays its own montages).
		AnimInstance->Montage_Stop(MontageBlendOutTime);
		if (UGMCMotion* Motion = GetMotion())
		{
			Motion->ClearRagdollCapsuleTarget();
		}
	}
}

void UGMCGASPSmartObjectUser::BindInput()
{
	// The pawn's input component may not exist yet at BeginPlay (possession): retried every tick until bound.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (bInputBound || !InteractAction || !Pawn || !Pawn->IsLocallyControlled())
	{
		return;
	}
	if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(Pawn->InputComponent))
	{
		Input->BindAction(InteractAction, ETriggerEvent::Started, this, &UGMCGASPSmartObjectUser::Interact);
		bInputBound = true;
	}
}

void UGMCGASPSmartObjectUser::Interact()
{
	if (IsUsingSmartObject())
	{
		RequestExit();
	}
	else
	{
		RequestUseNearby();
	}
}

void UGMCGASPSmartObjectUser::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	BindInput();
	UpdatePendingUse(DeltaTime);
	if (State.Phase == EGMCGASPSmartObjectPhase::None)
	{
		return;
	}
	LocalTime += DeltaTime;

	if (GetOwner()->HasAuthority())
	{
		if (IsRagdollBusy())
		{
			// Hit or killed: the ragdoll owns the movement mode and the montages now.
			ServerFinish(true);
			return;
		}
		switch (State.Phase)
		{
		case EGMCGASPSmartObjectPhase::Entering:
		{
			const FMontagePath& Path = GetMontagePath(State.Montage, true);
			if (LocalTime >= FMath::Max(Path.Length - MontageBlendOutTime, Path.KeyTime))
			{
				SetPhase(EGMCGASPSmartObjectPhase::Seated, IdleMontage, 0.f);
			}
			break;
		}
		case EGMCGASPSmartObjectPhase::Seated:
		{
			// A move input stands up, once released after sitting down (the key held on the way in doesn't count).
			const UGMCMotion* Motion = GetMotion();
			const bool bInput = Motion && Motion->IsInputPresent(false);
			bSeatedInputReleased |= !bInput;
			if (bInput && bSeatedInputReleased)
			{
				// The walk exit goes out the way the seat faces: only for an input that way, else stand up first.
				const FVector Forward = FRotator(0., State.SlotYaw, 0.).Vector();
				const FVector Input = Motion->GetProcessedInputVector().GetSafeNormal2D();
				ServerExit((Input | Forward) >= WalkExitMinInputDot);
			}
			else if (IdleMontage && LocalTime >= IdleMontage->GetPlayLength() - MontageBlendOutTime)
			{
				SetPhase(EGMCGASPSmartObjectPhase::Seated, IdleMontage, 0.f);
			}
			break;
		}
		case EGMCGASPSmartObjectPhase::Exiting:
		{
			if (LocalTime >= GetMontagePath(State.Montage, false).KeyTime)
			{
				ServerFinish(false);
				return;
			}
			break;
		}
		default:
			break;
		}
	}

	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (State.Phase != EGMCGASPSmartObjectPhase::None && Pawn && (Pawn->HasAuthority() || Pawn->IsLocallyControlled()))
	{
		UpdateCapsuleTarget();
	}
}

//--------------------
// Capsule path

const UGMCGASPSmartObjectUser::FMontagePath& UGMCGASPSmartObjectUser::GetMontagePath(const UAnimMontage* Montage, bool bEndsOnSlot) const
{
	TMap<TObjectKey<UAnimMontage>, FMontagePath>& Cache = bEndsOnSlot ? EntryPaths : ExitPaths;
	if (const FMontagePath* Found = Cache.Find(Montage))
	{
		return *Found;
	}
	FMontagePath& Path = Cache.Add(Montage);
	if (!Montage || Montage->SlotAnimTracks.IsEmpty() || Montage->SlotAnimTracks[0].AnimTrack.AnimSegments.IsEmpty())
	{
		return Path;
	}
	// The bench montages are authored in the slot's space: the slot is the animation's origin, not the seated root
	// (that one sits 21 cm in front of it; taking it as the slot put the pawn 21 cm too deep in the bench, legs in the
	// seat and back through the backrest, user 2026-10-01). Absolute root at the entry's end / the exit's start.
	const TArray<FAnimSegment>& Segments = Montage->SlotAnimTracks[0].AnimTrack.AnimSegments;
	const FAnimSegment& AnchorSegment = bEndsOnSlot ? Segments.Last() : Segments[0];
	const UAnimSequenceBase* AnchorAnim = AnchorSegment.GetAnimReference();
	const FTransform Anchor = AnchorAnim
		? AnchorAnim->ExtractRootTrackTransform(FAnimExtractContext(static_cast<double>(bEndsOnSlot ? AnchorSegment.AnimEndTime : AnchorSegment.AnimStartTime)), nullptr)
		: FTransform::Identity;
	Path.Length = Montage->GetPlayLength();
	const int32 Count = FMath::Max(2, FMath::CeilToInt32(Path.Length * PathSampleRate) + 1);
	Path.Samples.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Time = FMath::Min(Index / PathSampleRate, Path.Length);
		// Root at Time relative to the seated root (the entry's end or the exit's start), then in the slot's space.
		const FTransform Relative = bEndsOnSlot
			? Montage->ExtractRootMotionFromTrackRange(Time, Path.Length, FAnimExtractContext()).Inverse()
			: Montage->ExtractRootMotionFromTrackRange(0.f, Time, FAnimExtractContext());
		Path.Samples.Add(Relative * Anchor);
	}

	if (bEndsOnSlot)
	{
		// When the root reaches the slot.
		Path.KeyTime = Path.Length;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FTransform Remaining = Path.Samples[Index].GetRelativeTransform(Anchor);
			if (Remaining.GetLocation().Size2D() < 1. && FMath::Abs(Remaining.Rotator().Yaw) < 2.)
			{
				Path.KeyTime = Index / PathSampleRate;
				break;
			}
		}
	}
	else
	{
		// When the root speed settles (0 for the stand exit, the walk speed for the walk exit): locomotion takes over.
		auto Speed = [&Path](int32 Index)
		{
			return (Path.Samples[Index].GetLocation() - Path.Samples[Index - 1].GetLocation()).Size2D() * PathSampleRate;
		};
		const double FinalSpeed = Speed(Count - 1);
		const double Tolerance = FMath::Max(FinalSpeed * 0.05, 5.);
		Path.KeyTime = 0.f;
		for (int32 Index = Count - 1; Index >= 1; --Index)
		{
			if (FMath::Abs(Speed(Index) - FinalSpeed) > Tolerance)
			{
				Path.KeyTime = FMath::Min((Index + 1) / PathSampleRate, Path.Length);
				break;
			}
		}
	}
	UE_LOG(LogGMCGASPSmartObject, Verbose, TEXT("%s path: %d samples, key time %.2f s"), *Montage->GetName(), Count, Path.KeyTime);
	return Path;
}

FTransform UGMCGASPSmartObjectUser::GetStandingMeshRelative() const
{
	// The capsule is standing for the whole use (BP_GMCMovement.UpdateStance wants Stand off the ground), but from a
	// crouch it only grows in the first move: the current offset (-60) placed the capsule for the crouch height, the
	// mesh dropped 30 cm for that frame and Offset Root Bone dragged the feet 11 cm into the ground for 0.15 s.
	// The mesh sits at -HalfHeight (MaintainMeshOffset): use the standing half height.
	const USkeletalMeshComponent* Mesh = GetMesh();
	FTransform MeshRelative = Mesh ? Mesh->GetRelativeTransform() : FTransform::Identity;
	const UGMCMotion* Motion = GetMotion();
	const FNumericProperty* Property = Motion ? FindFProperty<FNumericProperty>(Motion->GetClass(), TEXT("DefaultHalfHeight")) : nullptr;
	if (Property)
	{
		FVector Location = MeshRelative.GetLocation();
		Location.Z = -Property->GetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Motion));
		MeshRelative.SetLocation(Location);
	}
	return MeshRelative;
}

FTransform UGMCGASPSmartObjectUser::GetPathTransform(const UAnimMontage* Montage, bool bEndsOnSlot, float Time) const
{
	return GetPathTransform(Montage, bEndsOnSlot, Time, FVector(State.SlotLocation), State.SlotYaw);
}

FTransform UGMCGASPSmartObjectUser::GetPathTransform(const UAnimMontage* Montage, bool bEndsOnSlot, float Time, const FVector& SlotLocation, float SlotYaw) const
{
	const FMontagePath& Path = GetMontagePath(Montage, bEndsOnSlot);
	const FTransform RootInSlot = SamplePath(Path.Samples, Time);
	const USkeletalMeshComponent* Mesh = GetMesh();
	const FTransform MeshRelative = GetStandingMeshRelative();
	// The root motion is in the mesh's space: the animation's origin is the mesh component as it would sit with the
	// capsule on the slot (actor yaw = slot yaw), then back from the mesh to the capsule.
	const FTransform SeatedMesh(FRotator(0., SlotYaw, 0.).Quaternion() * MeshRelative.GetRotation(), SlotLocation);
	const FTransform RootWorld = RootInSlot * SeatedMesh;
	const FQuat ActorRotation = FRotator(0., (RootWorld.GetRotation() * MeshRelative.GetRotation().Inverse()).Rotator().Yaw, 0.).Quaternion();
	return FTransform(ActorRotation, RootWorld.GetLocation() - ActorRotation.RotateVector(MeshRelative.GetLocation()));
}

bool UGMCGASPSmartObjectUser::ChooseEntry(const FTransform& PawnTransform, const FTransform& SlotTransform, UAnimMontage*& OutMontage, float& OutStartTime) const
{
	const FVector SlotLocation = SlotTransform.GetLocation();
	const float SlotYaw = SlotTransform.Rotator().Yaw;
	double BestCost = TNumericLimits<double>::Max();
	OutMontage = nullptr;
	const float PawnYaw = PawnTransform.Rotator().Yaw;
	for (UAnimMontage* Montage : EntryMontages)
	{
		if (!Montage)
		{
			continue;
		}
		const FMontagePath& Path = GetMontagePath(Montage, true);
		const float LastStart = FMath::Max(0.f, Path.KeyTime - MinEntryDuration);
		for (float Time = 0.f; Time <= LastStart; Time += 1.f / PathSampleRate)
		{
			const FTransform Candidate = GetPathTransform(Montage, true, Time, SlotLocation, SlotYaw);
			const double Distance = FVector::Dist2D(Candidate.GetLocation(), PawnTransform.GetLocation());
			const double YawDifference = FMath::Abs(FRotator::NormalizeAxis(Candidate.Rotator().Yaw - PawnYaw));
			const double Cost = Distance + EntryYawCost * YawDifference;
			if (Cost < BestCost)
			{
				BestCost = Cost;
				OutMontage = Montage;
				OutStartTime = Time;
			}
		}
	}
	return OutMontage != nullptr;
}

void UGMCGASPSmartObjectUser::UpdateCapsuleTarget()
{
	UGMCMotion* Motion = GetMotion();
	if (!Motion)
	{
		return;
	}
	FTransform Target;
	FVector Velocity = FVector::ZeroVector;
	switch (State.Phase)
	{
	case EGMCGASPSmartObjectPhase::Entering:
	{
		const FMontagePath& Path = GetMontagePath(State.Montage, true);
		Target = GetPathTransform(State.Montage, true, LocalTime);
		// The pawn's gap to the path where it started fades out until the root reaches the slot.
		const FTransform PathStart = GetPathTransform(State.Montage, true, State.MontageStartTime);
		const float Span = FMath::Max(Path.KeyTime - State.MontageStartTime, UE_KINDA_SMALL_NUMBER);
		const float Alpha = FMath::SmoothStep(0.f, 1.f, FMath::Clamp((LocalTime - State.MontageStartTime) / Span, 0.f, 1.f));
		const FVector MeshOffset = GetStandingMeshRelative().GetLocation();
		const FVector Gap = FVector(State.StartLocation) - (PathStart.GetLocation() + PathStart.GetRotation().RotateVector(MeshOffset));
		const float YawGap = FRotator::NormalizeAxis(State.StartYaw - PathStart.Rotator().Yaw);
		Target.SetLocation(Target.GetLocation() + Gap * (1.f - Alpha));
		Target.SetRotation(FRotator(0., Target.Rotator().Yaw + YawGap * (1.f - Alpha), 0.).Quaternion());
		if (LocalTime < Path.KeyTime)
		{
			Velocity = (GetPathTransform(State.Montage, true, LocalTime + 1.f / PathSampleRate).GetLocation()
				- GetPathTransform(State.Montage, true, LocalTime).GetLocation()) * PathSampleRate;
		}
		break;
	}
	case EGMCGASPSmartObjectPhase::Seated:
		// The idle loop's root, which doesn't move (the entry's last pose).
		Target = GetPathTransform(State.Montage, false, 0.f);
		break;
	case EGMCGASPSmartObjectPhase::Exiting:
	{
		const FMontagePath& Path = GetMontagePath(State.Montage, false);
		// The path's own velocity, kept at the end: the walk exit hands it to locomotion.
		const float Time = FMath::Min(LocalTime, Path.KeyTime);
		Target = GetPathTransform(State.Montage, false, Time);
		const float Previous = FMath::Max(Time - 1.f / PathSampleRate, 0.f);
		if (Time > Previous)
		{
			Velocity = (Target.GetLocation() - GetPathTransform(State.Montage, false, Previous).GetLocation()) / (Time - Previous);
		}
		break;
	}
	default:
		return;
	}
	Motion->SetRagdollCapsuleTargetWithVelocity(Target, Velocity);
}
