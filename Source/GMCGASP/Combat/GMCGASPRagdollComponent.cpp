#include "Combat/GMCGASPRagdollComponent.h"

#include "AlphaBlend.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "AnimationRuntime.h"
#include "Actors/GMCAggregator.h"
#include "AnimNotifyState_MotionWarping.h"
#include "Combat/GMCGASPRagdollTriggers.h"
#include "UObject/UObjectIterator.h"
#include "Components/CapsuleComponent.h"
#include "Components/GMCAbilityComponent.h"
#include "Effects/GMCAbilityEffect.h"
#include "Components/GMCMotion.h"
#include "Engine/OverlapResult.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Components/SkeletalMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "PhysicsControlBPLibrary.h"
#include "PhysicsControlComponent.h"
#include "Animation/AnimNotifyQueue.h"
#include "BonePose.h"
#include "PoseSearch/MultiAnimAsset.h"
#include "PoseSearch/PoseSearchAnimNotifies.h"
#include "PoseSearch/PoseSearchAssetSampler.h"
#include "PoseSearch/PoseSearchDatabase.h"
#if WITH_EDITOR
#include "PoseSearch/PoseSearchDerivedData.h"
#endif
#include "PoseSearch/PoseSearchInteractionLibrary.h"
#include "PoseSearch/PoseSearchLibrary.h"
#include "PoseSearch/PoseSearchResult.h"
#include "RootMotionModifier_SkewWarp.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/BodySetup.h"

DEFINE_LOG_CATEGORY_STATIC(LogGMCGASPRagdoll, Log, All);

// Experiment (branch mesh-tick-order): the physics layer needs the animation evaluated BEFORE physics, in the same frame
// as the GMC move. GMC's own mesh -> movement component prerequisite orders nothing when the GMC aggregator ticks the
// movement components (it disables their tick functions): chain the mesh after the aggregator instead.
static TAutoConsoleVariable<int32> CVarMeshTickAfterGMCMoves(
	TEXT("gmc.Mesh.TickAfterGMCMoves"), 0,
	TEXT("Live: 1 = mesh in TG_PrePhysics after the GMC aggregator (the moves of the frame), 0 = the Blueprint's tick group ")
	TEXT("(TG_PostPhysics)."));

static TAutoConsoleVariable<int32> CVarRagdollPhysicsLayer(
	TEXT("gmc.Ragdoll.PhysicsLayer"), -1,
	TEXT("Live (standing pawns): -1 = component's DefaultPhysicsProfile, 0 = Kinematic, 1 = PhysicalAnimation (GASP58 ")
	TEXT("DDCVar.CharacterPhysics.DefaultProfile)."));

// Re-applies the switches to every playing ragdoll component when they change.
struct FGMCGASPRagdollCVars
{
	static void Register()
	{
		static bool bRegistered = false;
		if (bRegistered)
		{
			return;
		}
		bRegistered = true;
		CVarMeshTickAfterGMCMoves->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnChanged));
		CVarRagdollPhysicsLayer->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OnChanged));
	}
	static void OnChanged(IConsoleVariable*)
	{
		for (TObjectIterator<UGMCGASPRagdollComponent> It; It; ++It)
		{
			UGMCGASPRagdollComponent* Component = *It;
			const UWorld* World = Component->GetWorld();
			if (World && World->IsGameWorld() && Component->HasBegunPlay() && !Component->IsBeingDestroyed())
			{
				Component->ApplyMeshTickOrder();
				Component->ApplyPhysicsLayerSwitch();
			}
		}
	}
};

namespace GMCGASPRagdoll
{
	const FName ProfileRagdoll(TEXT("Ragdoll"));
	const FName ProfileKinematic(TEXT("Kinematic"));
	const FName ProfilePhysicalAnimation(TEXT("PhysicalAnimation"));
	const FName SnapshotName(TEXT("RagdollPose"));
	// BP_GMCMovement.MovementAction (enum MovementAction: None, Ragdoll, GettingUp), read by the ABP.
	const FName MovementActionProperty(TEXT("MovementAction"));
	constexpr uint8 ActionNone = 0;
	constexpr uint8 ActionRagdoll = 1;
	constexpr uint8 ActionGettingUp = 2;
	// Per-limb control sets of the Physics Control Asset (head, torso, arms, legs).
	const FName StrengthSets[4] = {TEXT("ParentSpace_Head"), TEXT("ParentSpace_Torso"), TEXT("ParentSpace_Arms"), TEXT("ParentSpace_Legs")};

	// Quantization of the streamed bodies.
	constexpr float OffsetUnit = 0.5f; // cm per step of the int16 offsets to the pelvis (+-163 m)

	void WriteQuat(FArchive& Ar, FQuat& Q)
	{
		int16 X = 0, Y = 0, Z = 0;
		if (Ar.IsSaving())
		{
			FQuat N = Q.GetNormalized();
			if (N.W < 0.)
			{
				N = FQuat(-N.X, -N.Y, -N.Z, -N.W);
			}
			X = (int16)FMath::Clamp(FMath::RoundToInt(N.X * 32767.), -32767, 32767);
			Y = (int16)FMath::Clamp(FMath::RoundToInt(N.Y * 32767.), -32767, 32767);
			Z = (int16)FMath::Clamp(FMath::RoundToInt(N.Z * 32767.), -32767, 32767);
		}
		Ar << X << Y << Z;
		if (Ar.IsLoading())
		{
			const double QX = X / 32767., QY = Y / 32767., QZ = Z / 32767.;
			const double QW = FMath::Sqrt(FMath::Max(0., 1. - QX * QX - QY * QY - QZ * QZ));
			Q = FQuat(QX, QY, QZ, QW).GetNormalized();
		}
	}

	template <typename T>
	T* FindProp(UObject* Object, FName Name)
	{
		return Object ? FindFProperty<T>(Object->GetClass(), Name) : nullptr;
	}

	void SetReal(UObject* Object, FName Name, double Value)
	{
		if (FDoubleProperty* D = FindProp<FDoubleProperty>(Object, Name))
		{
			D->SetPropertyValue_InContainer(Object, Value);
		}
		else if (FFloatProperty* F = FindProp<FFloatProperty>(Object, Name))
		{
			F->SetPropertyValue_InContainer(Object, (float)Value);
		}
	}

	void SetBool(UObject* Object, FName Name, bool bValue)
	{
		if (FBoolProperty* B = FindProp<FBoolProperty>(Object, Name))
		{
			B->SetPropertyValue_InContainer(Object, bValue);
		}
	}

	void SetByte(UObject* Object, FName Name, uint8 Value)
	{
		if (FByteProperty* B = FindProp<FByteProperty>(Object, Name))
		{
			B->SetPropertyValue_InContainer(Object, Value);
		}
		else if (FEnumProperty* E = FindProp<FEnumProperty>(Object, Name))
		{
			E->GetUnderlyingProperty()->SetIntPropertyValue(E->ContainerPtrToValuePtr<void>(Object), (int64)Value);
		}
		else if (FIntProperty* I = FindProp<FIntProperty>(Object, Name))
		{
			I->SetPropertyValue_InContainer(Object, (int32)Value);
		}
	}

	bool GetNumber(UObject* Object, FName Name, double& OutValue)
	{
		FNumericProperty* N = FindProp<FNumericProperty>(Object, Name);
		if (!N || !N->IsFloatingPoint())
		{
			return false;
		}
		OutValue = N->GetFloatingPointPropertyValue(N->ContainerPtrToValuePtr<void>(Object));
		return true;
	}

	void SetNumber(UObject* Object, FName Name, double Value)
	{
		if (FNumericProperty* N = FindProp<FNumericProperty>(Object, Name); N && N->IsFloatingPoint())
		{
			N->SetFloatingPointPropertyValue(N->ContainerPtrToValuePtr<void>(Object), Value);
		}
	}

	bool GetBool(UObject* Object, FName Name, bool bDefault)
	{
		FBoolProperty* B = FindProp<FBoolProperty>(Object, Name);
		return B ? B->GetPropertyValue_InContainer(Object) : bDefault;
	}

	void SetVector2D(UObject* Object, FName Name, const FVector2D& Value)
	{
		FStructProperty* S = FindProp<FStructProperty>(Object, Name);
		if (S && S->Struct == TBaseStructure<FVector2D>::Get())
		{
			*S->ContainerPtrToValuePtr<FVector2D>(Object) = Value;
		}
	}

	void SetTransform(UObject* Object, FName Name, const FTransform& Value)
	{
		FStructProperty* S = FindProp<FStructProperty>(Object, Name);
		if (S && S->Struct == TBaseStructure<FTransform>::Get())
		{
			*S->ContainerPtrToValuePtr<FTransform>(Object) = Value;
		}
	}

	// GASP58 BP_NotifyState_MMI_MotionWarping: the montage's motion warping window, and its skew warp settings when the
	// notify is the engine's (the retargeted montages may only keep the GASP event's name and times).
	bool FindWarpWindow(const UAnimMontage* Montage, float& OutStart, float& OutEnd, const URootMotionModifier_Warp*& OutWarp)
	{
		OutWarp = nullptr;
		if (!Montage)
		{
			return false;
		}
		for (const FAnimNotifyEvent& Event : Montage->Notifies)
		{
			if (const UAnimNotifyState_MotionWarping* Notify = Cast<UAnimNotifyState_MotionWarping>(Event.NotifyStateClass))
			{
				OutStart = Event.GetTriggerTime();
				OutEnd = Event.GetEndTriggerTime();
				OutWarp = Cast<URootMotionModifier_Warp>(Notify->RootMotionModifier);
				return true;
			}
		}
		for (const FAnimNotifyEvent& Event : Montage->Notifies)
		{
			if (Event.NotifyName.ToString().Contains(TEXT("MotionWarping")))
			{
				OutStart = Event.GetTriggerTime();
				OutEnd = Event.GetEndTriggerTime();
				return true;
			}
		}
		return false;
	}

	// GASP58 BP_AnimNotify_TriggerRagdoll in an interaction montage: its time and injury (UGMCGASPAnimNotify_TriggerRagdoll,
	// or the GASP event's name).
	bool FindRagdollNotify(const UAnimMontage* Montage, float& OutTime, EGMCGASPRagdollInjury& OutInjury)
	{
		OutInjury = EGMCGASPRagdollInjury::None;
		if (!Montage)
		{
			return false;
		}
		for (const FAnimNotifyEvent& Event : Montage->Notifies)
		{
			const UGMCGASPAnimNotify_TriggerRagdoll* Notify = Cast<UGMCGASPAnimNotify_TriggerRagdoll>(Event.Notify);
			if (Notify || Event.NotifyName.ToString().Contains(TEXT("TriggerRagdoll")))
			{
				OutTime = Event.GetTriggerTime();
				OutInjury = Notify ? Notify->Injury : EGMCGASPRagdollInjury::None;
				return true;
			}
		}
		return false;
	}

	UAnimInstance* GetActorAnimInstance(const AActor* Actor)
	{
		const UGMCGASPRagdollComponent* Ragdoll = Actor ? Actor->FindComponentByClass<UGMCGASPRagdollComponent>() : nullptr;
		TInlineComponentArray<USkeletalMeshComponent*> Meshes(Actor);
		for (USkeletalMeshComponent* Mesh : Meshes)
		{
			if (Ragdoll && Mesh->GetFName() == Ragdoll->MeshName)
			{
				return Mesh->GetAnimInstance();
			}
		}
		return nullptr;
	}

	// UPhysicsControlBPLibrary::{Enable,Disable}CollisionBetweenBodyArrays are Blueprint functions declared private in
	// C++: called through reflection, as GASP58's Blueprint does.
	void CallBodyArraysCollision(FName Function, USkeletalMeshComponent* Mesh, const TArray<FName>& First, const TArray<FName>& Second)
	{
		UClass* Library = UPhysicsControlBPLibrary::StaticClass();
		UFunction* Func = Library ? Library->FindFunctionByName(Function) : nullptr;
		if (!Func || !Mesh)
		{
			return;
		}
		uint8* Params = (uint8*)FMemory_Alloca(Func->ParmsSize);
		FMemory::Memzero(Params, Func->ParmsSize);
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Params);
		}
		auto SetParam = [Func, Params](const TCHAR* Name, const void* Value)
		{
			if (FProperty* Property = Func->FindPropertyByName(Name))
			{
				Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Params), Value);
			}
		};
		UPrimitiveComponent* Component = Mesh;
		SetParam(TEXT("FirstComponent"), &Component);
		SetParam(TEXT("FirstBoneNames"), &First);
		SetParam(TEXT("SecondComponent"), &Component);
		SetParam(TEXT("SecondBoneNames"), &Second);
		Library->GetDefaultObject()->ProcessEvent(Func, Params);
		for (TFieldIterator<FProperty> It(Func); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Params);
		}
	}

	void CallNoParams(UObject* Object, FName Function)
	{
		if (UFunction* F = Object ? Object->FindFunction(Function) : nullptr)
		{
			if (F->ParmsSize == 0)
			{
				Object->ProcessEvent(F, nullptr);
			}
		}
	}

	// GASP58 strengths [head, torso, arms, legs] of the on-ground injury poses (ABP "Ragdoll On Ground" Modify Curves).
	FVector4 InjuryStrengths(EGMCGASPRagdollInjury Injury)
	{
		switch (Injury)
		{
		case EGMCGASPRagdollInjury::Limp:      return FVector4(0.3, 0.3, 0.3, 0.3);
		case EGMCGASPRagdollInjury::Stunned:   return FVector4(0.2, 1.0, 1.0, 1.0);
		case EGMCGASPRagdollInjury::Head_Face: return FVector4(1.0, 0.5, 1.0, 0.3);
		case EGMCGASPRagdollInjury::Head_Back: return FVector4(1.0, 0.5, 2.0, 0.1);
		case EGMCGASPRagdollInjury::Body_Front:return FVector4(1.0, 1.0, 2.0, 0.2);
		case EGMCGASPRagdollInjury::Groin:     return FVector4(2.0, 2.0, 2.0, 0.2);
		default:                               return FVector4(1.0, 1.0, 1.0, 0.2);
		}
	}
}

using namespace GMCGASPRagdoll;

// =====================================================================================================================
//  Net pose
// =====================================================================================================================

bool FGMCGASPRagdollNetPose::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	Ar << ServerTime;
	uint8 Count = (uint8)FMath::Min(Bodies.Num(), 255);
	Ar << Count;
	Ar << RootIndex;
	if (Ar.IsLoading())
	{
		Bodies.SetNum(Count);
	}
	if (Count == 0 || RootIndex >= Count)
	{
		bOutSuccess = Count == 0;
		return true;
	}

	FVector RootLocation = Bodies[RootIndex].GetLocation();
	bOutSuccess = SerializePackedVector<10, 24>(RootLocation, Ar);
	for (int32 i = 0; i < Count; ++i)
	{
		FQuat Rotation = Bodies[i].GetRotation();
		WriteQuat(Ar, Rotation);
		FVector Location = RootLocation;
		if (i != RootIndex)
		{
			int16 Offset[3] = {0, 0, 0};
			if (Ar.IsSaving())
			{
				const FVector Delta = (Bodies[i].GetLocation() - RootLocation) / OffsetUnit;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					Offset[Axis] = (int16)FMath::Clamp(FMath::RoundToInt(Delta[Axis]), -32767, 32767);
				}
			}
			Ar << Offset[0] << Offset[1] << Offset[2];
			Location = RootLocation + FVector(Offset[0], Offset[1], Offset[2]) * OffsetUnit;
		}
		if (Ar.IsLoading())
		{
			Bodies[i] = FTransform(Rotation, Location);
		}
	}
	return true;
}

// =====================================================================================================================
//  Setup
// =====================================================================================================================

UGMCGASPRagdollComponent::UGMCGASPRagdollComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// Before physics: forces, torques and steering belong to this frame's step.
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	SetIsReplicatedByDefault(true);
}

void UGMCGASPRagdollComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UGMCGASPRagdollComponent, State);
	DOREPLIFETIME(UGMCGASPRagdollComponent, FinalPose);
}

void UGMCGASPRagdollComponent::BeginPlay()
{
	Super::BeginPlay();
	FGMCGASPRagdollCVars::Register();
	ConfiguredPhysicsProfile = DefaultPhysicsProfile;
	ApplyPhysicsLayerSwitch();
	ApplyMeshTickOrder();
	DeadTag = FGameplayTag::RequestGameplayTag(TEXT("State.Dead"), false);
	HealthTag = FGameplayTag::RequestGameplayTag(TEXT("Attribute.Health"), false);
	if (!DeathFallbackEffect)
	{
		DeathFallbackEffect = LoadClass<UGMCAbilityEffect>(nullptr, TEXT("/Game/GMC/GMAS/Abilities/Effects/GE_InstantKill.GE_InstantKill_C"));
	}
	// Default get-up databases (GASP58 CHT_GetUpMontages rows, split by its RollingGetup column).
	if (!GetUpDatabase)
	{
		GetUpDatabase = LoadObject<UPoseSearchDatabase>(nullptr, TEXT("/Game/Characters/UE5_Mannequins/Animations/Ragdoll/PSD_GetUp_Stand.PSD_GetUp_Stand"));
	}
	if (!RollingGetUpDatabase)
	{
		RollingGetUpDatabase = LoadObject<UPoseSearchDatabase>(nullptr, TEXT("/Game/Characters/UE5_Mannequins/Animations/Ragdoll/PSD_GetUp_Roll.PSD_GetUp_Roll"));
	}
	if (InteractionDatabases.Num() == 0)
	{
		// GASP58 CHT_CharacterInteractionPSDs rows: Shove under 200 cm/s, Tackle above; Takedown stand / walk / run.
		struct FRow { EGMCGASPInteractionType Type; float Min; float Max; const TCHAR* Path; };
		const FRow Rows[] = {
			{EGMCGASPInteractionType::Shove, 0.f, 200.f, TEXT("/Game/Characters/UE5_Mannequins/Animations/Interactions/Shoves/PSD_Interaction_Shove.PSD_Interaction_Shove")},
			{EGMCGASPInteractionType::Shove, 200.f, 100000.f, TEXT("/Game/Characters/UE5_Mannequins/Animations/Interactions/Tackle/PSD_Interaction_Tackle.PSD_Interaction_Tackle")},
			{EGMCGASPInteractionType::Takedown, 0.f, 150.f, TEXT("/Game/Characters/UE5_Mannequins/Animations/Interactions/Takedowns/PSD_Interaction_takedown_stand.PSD_Interaction_takedown_stand")},
			{EGMCGASPInteractionType::Takedown, 150.f, 200.f, TEXT("/Game/Characters/UE5_Mannequins/Animations/Interactions/Takedowns/PSD_Interaction_takedown_Walk.PSD_Interaction_takedown_Walk")},
			{EGMCGASPInteractionType::Takedown, 200.f, 100000.f, TEXT("/Game/Characters/UE5_Mannequins/Animations/Interactions/Takedowns/PSD_Interaction_takedown_Run.PSD_Interaction_takedown_Run")}};
		for (const FRow& Row : Rows)
		{
			FGMCGASPInteractionDatabase& Entry = InteractionDatabases.AddDefaulted_GetRef();
			Entry.Type = Row.Type;
			Entry.MinSpeed = Row.Min;
			Entry.MaxSpeed = Row.Max;
			Entry.Database = LoadObject<UPoseSearchDatabase>(nullptr, Row.Path);
		}
	}
#if WITH_EDITOR
	// In the editor the databases' search index is built on first use, and that first search is skipped (PIE). Started
	// here instead, so the first get-up / interaction already finds its index.
	TArray<const UPoseSearchDatabase*> Databases = {GetUpDatabase, RollingGetUpDatabase};
	for (const FGMCGASPInteractionDatabase& Row : InteractionDatabases)
	{
		Databases.Add(Row.Database);
	}
	for (const UPoseSearchDatabase* Database : Databases)
	{
		if (Database)
		{
			UE::PoseSearch::FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(Database, UE::PoseSearch::ERequestAsyncBuildFlag::ContinueRequest);
		}
	}
#endif
	if (AbilitiesToEnd.Num() == 0)
	{
		for (const TCHAR* Name : {TEXT("Ability.Aim"), TEXT("Ability.Shoot"), TEXT("Ability.Reload")})
		{
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Name, false);
			if (Tag.IsValid())
			{
				AbilitiesToEnd.Add(Tag);
			}
		}
	}
	if (UGMC_AbilitySystemComponent* Abilities = GetAbilities())
	{
		Abilities->OnActiveTagsChanged.AddUniqueDynamic(this, &UGMCGASPRagdollComponent::OnActiveTagsChanged);
		Abilities->OnEffectApplied.AddUniqueDynamic(this, &UGMCGASPRagdollComponent::OnEffectApplied);
	}
	BindInput();
}

void UGMCGASPRagdollComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UGMC_AbilitySystemComponent* Abilities = GetAbilities())
	{
		Abilities->OnActiveTagsChanged.RemoveDynamic(this, &UGMCGASPRagdollComponent::OnActiveTagsChanged);
		Abilities->OnEffectApplied.RemoveDynamic(this, &UGMCGASPRagdollComponent::OnEffectApplied);
	}
	Super::EndPlay(EndPlayReason);
}

USkeletalMeshComponent* UGMCGASPRagdollComponent::GetMesh() const
{
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

UGMCMotion* UGMCGASPRagdollComponent::GetMotion() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UGMCMotion>() : nullptr;
}

UGMC_AbilitySystemComponent* UGMCGASPRagdollComponent::GetAbilities() const
{
	return GetOwner() ? GetOwner()->FindComponentByClass<UGMC_AbilitySystemComponent>() : nullptr;
}

bool UGMCGASPRagdollComponent::IsLocallyControlledPawn() const
{
	const APawn* Pawn = Cast<APawn>(GetOwner());
	return Pawn && Pawn->IsLocallyControlled();
}

double UGMCGASPRagdollComponent::GetServerTime() const
{
	const AGameStateBase* GameState = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	return GameState ? GameState->GetServerWorldTimeSeconds() : (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.);
}

void UGMCGASPRagdollComponent::SetupPhysicsControl()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (bPhysicsControlReady || !Mesh || !PhysicsControlAsset)
	{
		return;
	}
	// GASP58's mesh: the component never follows its simulated bodies (default mode SimulationUpatesComponentTransform
	// drags it with the root body, and with it the controls' animation targets: the physics layer runs away). It stays on
	// the capsule, and the capsule follows the pelvis.
	Mesh->PhysicsTransformUpdateMode = EPhysicsTransformUpdateMode::ComponentTransformIsKinematic;
	if (!PhysicsControl)
	{
		PhysicsControl = NewObject<UPhysicsControlComponent>(GetOwner(), TEXT("RagdollPhysicsControl"));
		PhysicsControl->PhysicsControlAsset = PhysicsControlAsset;
		PhysicsControl->RegisterComponent();
		// After the anim graph, so this frame's ragdoll pose is the controls' target (GASP: AC_PostABPTick).
		PhysicsControl->AddTickPrerequisiteComponent(Mesh);
	}
	bPhysicsControlReady = PhysicsControl->CreateControlsAndBodyModifiersFromPhysicsControlAsset(Mesh, nullptr, NAME_None);
	if (!bPhysicsControlReady)
	{
		UE_LOG(LogGMCGASPRagdoll, Warning, TEXT("%s: CreateControlsAndBodyModifiersFromPhysicsControlAsset failed"), *GetNameSafe(GetOwner()));
	}
}

void UGMCGASPRagdollComponent::BindInput()
{
	if (bInputBound || !IsLocallyControlledPawn())
	{
		return;
	}
	APawn* Pawn = Cast<APawn>(GetOwner());
	UEnhancedInputComponent* Input = Pawn ? Cast<UEnhancedInputComponent>(Pawn->InputComponent) : nullptr;
	if (!Input)
	{
		return;
	}
	if (ToggleRagdollAction)
	{
		Input->BindAction(ToggleRagdollAction, ETriggerEvent::Started, this, &UGMCGASPRagdollComponent::OnToggleRagdollInput);
	}
	if (GetUpAction)
	{
		Input->BindAction(GetUpAction, ETriggerEvent::Started, this, &UGMCGASPRagdollComponent::OnGetUpInput);
	}
	if (ShoveAction)
	{
		Input->BindAction(ShoveAction, ETriggerEvent::Started, this, &UGMCGASPRagdollComponent::OnShoveInput);
	}
	if (TakedownAction)
	{
		Input->BindAction(TakedownAction, ETriggerEvent::Started, this, &UGMCGASPRagdollComponent::OnTakedownInput);
	}
	bInputBound = true;
}

// =====================================================================================================================
//  Requests (server decides)
// =====================================================================================================================

void UGMCGASPRagdollComponent::OnToggleRagdollInput()
{
	RequestToggleRagdoll();
}

void UGMCGASPRagdollComponent::OnGetUpInput()
{
	if (State.Phase != EGMCGASPRagdollPhase::Ragdoll)
	{
		return;
	}
	if (GetOwner()->HasAuthority())
	{
		ExitRagdoll(false);
	}
	else
	{
		ServerRequestGetUp();
	}
}

void UGMCGASPRagdollComponent::RequestToggleRagdoll()
{
	if (!GetOwner()->HasAuthority())
	{
		ServerRequestToggleRagdoll();
		return;
	}
	if (State.Phase == EGMCGASPRagdollPhase::None)
	{
		TriggerRagdoll(EGMCGASPRagdollInjury::None, FVector::ZeroVector, FVector::ZeroVector);
	}
	else if (State.Phase == EGMCGASPRagdollPhase::Ragdoll)
	{
		ExitRagdoll(false);
	}
}

void UGMCGASPRagdollComponent::ServerRequestToggleRagdoll_Implementation()
{
	RequestToggleRagdoll();
}

void UGMCGASPRagdollComponent::ServerRequestGetUp_Implementation()
{
	ExitRagdoll(false);
}

void UGMCGASPRagdollComponent::TriggerRagdoll(EGMCGASPRagdollInjury Injury, FVector Impulse, FVector ImpulseLocation)
{
	// From an interaction too: GASP's TriggerRagdoll notify of the shove / tackle / takedown montages.
	if (!GetOwner()->HasAuthority() || (State.Phase != EGMCGASPRagdollPhase::None && State.Phase != EGMCGASPRagdollPhase::Interacting))
	{
		return;
	}
	const EGMCGASPRagdollPhase Old = State.Phase;
	InteractionRagdollTime = -1.f;
	State.InteractionAsset = nullptr;
	State.Partner = nullptr;
	State.bWarp = false;
	State.Phase = EGMCGASPRagdollPhase::Ragdoll;
	State.Injury = Injury;
	State.Impulse = Impulse;
	State.ImpulseLocation = ImpulseLocation;
	State.Montage = nullptr;
	++State.Serial;
	FinalPose = FGMCGASPRagdollNetPose();

	if (UGMCMotion* Motion = GetMotion())
	{
		Motion->SetRagdollMode(true);
	}
	UGMC_AbilitySystemComponent* Abilities = GetAbilities();
	if (Abilities && RagdollEffect)
	{
		bool bSuccess = false;
		int32 Handle = -1, Id = -1;
		UGMCAbilityEffect* Effect = nullptr;
		Abilities->ApplyAbilityEffectSafe(RagdollEffect, FGMCAbilityEffectData(), EGMCAbilityEffectQueueType::ServerAuth, bSuccess, Handle, Id, Effect);
	}
	ApplyPhase(State.Phase, Old);
}

void UGMCGASPRagdollComponent::ExitRagdoll(bool bForce)
{
	if (!GetOwner()->HasAuthority() || State.Phase != EGMCGASPRagdollPhase::Ragdoll)
	{
		return;
	}
	if (!bForce && (!bHasFloor || RagdollSpeed > PlayerGetUpMaxSpeed))
	{
		return;
	}
	const EGMCGASPRagdollPhase Old = State.Phase;
	State.Phase = EGMCGASPRagdollPhase::GettingUp;
	float StartTime = 0.f;
	const double T0 = FPlatformTime::Seconds();
	State.Montage = PickGetUpMontage(StartTime);
	const double T1 = FPlatformTime::Seconds();
	State.MontageStartTime = StartTime;
	++State.Serial;
	GetUpTimeLeft = State.Montage ? FMath::Max(0.2f, State.Montage->GetPlayLength() - StartTime - State.Montage->GetDefaultBlendOutTime()) : 0.2f;
	ApplyPhase(State.Phase, Old);
	UE_CLOG(bDebug, LogGMCGASPRagdoll, Log, TEXT("%s get-up cost: pick %.2f ms, apply phase (exit local, montage) %.2f ms"),
		*GetNameSafe(GetOwner()), (T1 - T0) * 1000., (FPlatformTime::Seconds() - T1) * 1000.);
}

void UGMCGASPRagdollComponent::AddHitImpulse(FVector ImpactVelocity, FVector ImpactLocation, float ImpartedEnergy)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (GetOwner()->HasAuthority() && State.Phase == EGMCGASPRagdollPhase::None && Mesh)
	{
		if (!TryHitKnockdown(ImpactVelocity, ImpactLocation, ImpartedEnergy) && HitReactionImpulse > 0.f && !ImpactVelocity.IsNearlyZero())
		{
			const FName Bone = Mesh->FindClosestBone(ImpactLocation, nullptr, 0.f, true);
			const float Scale = FMath::Clamp(ImpartedEnergy / 1000.f, 0.3f, 2.f);
			const FVector Direction = FVector(ImpactVelocity.X, ImpactVelocity.Y, 0.).GetSafeNormal();
			MulticastHitReaction(Bone, Direction * HitReactionImpulse * Scale, ImpactLocation);
		}
		return;
	}
	if (!GetOwner()->HasAuthority() || State.Phase != EGMCGASPRagdollPhase::Ragdoll || !Mesh || ImpactVelocity.IsNearlyZero())
	{
		return;
	}
	// Same shape as the death kick (ApplyDeathImpulse): no push into the floor, slightly up.
	FVector Direction = ImpactVelocity.GetSafeNormal();
	Direction.Z = FMath::Max(Direction.Z, 0.) + 0.3;
	const FName Bone = Mesh->FindClosestBone(ImpactLocation, nullptr, 0.f, true);
	Mesh->AddImpulseAtLocation(Direction.GetSafeNormal() * HitImpulseStrength, ImpactLocation, Bone);
}

bool UGMCGASPRagdollComponent::TryHitKnockdown(const FVector& ImpactVelocity, const FVector& ImpactLocation, float ImpartedEnergy)
{
	if (KnockdownThreshold <= 0.f || ImpartedEnergy <= 0.f)
	{
		return false;
	}
	// Weight of the hit bone: the closest body's bone, or its first weighted parent (KnockdownBoneWeights).
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return false;
	}
	const FName HitBone = Mesh->FindClosestBone(ImpactLocation, nullptr, 0.f, true);
	FName Bone = HitBone;
	const float* Weight = nullptr;
	while (!Bone.IsNone() && !(Weight = KnockdownBoneWeights.Find(Bone)))
	{
		Bone = Mesh->GetParentBone(Bone);
	}
	// The root body (pelvis) only counts when hit itself: the spine and the head are its children too.
	if (Bone != HitBone && Mesh->GetParentBone(Bone) == Mesh->GetBoneName(0))
	{
		Weight = nullptr;
	}
	if (!Weight || *Weight <= 0.f)
	{
		return false;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	KnockdownScore = FMath::Max(0.f, KnockdownScore - float(Now - KnockdownScoreTime) * KnockdownDecayPerSecond);
	KnockdownScoreTime = Now;
	const float Speed = GetOwner()->GetVelocity().Size2D();
	const float Added = ImpartedEnergy * *Weight * (1.f + Speed / KnockdownSpeedReference);
	KnockdownScore += Added;
	const bool bFall = KnockdownScore >= KnockdownThreshold;
	if (bDebug)
	{
		UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s knockdown: %s (%s) %.0f J at %.0f cm/s -> +%.0f, score %.0f / %.0f%s"), *GetNameSafe(GetOwner()),
			*HitBone.ToString(), *Bone.ToString(), ImpartedEnergy, Speed, Added, KnockdownScore, KnockdownThreshold, bFall ? TEXT(" -> FALL") : TEXT(""));
	}
	if (!bFall)
	{
		return false;
	}
	KnockdownScore = 0.f;
	// Same kick as a hit on a knocked-down pawn (AddHitImpulse): no push into the floor, slightly up.
	FVector Direction = ImpactVelocity.GetSafeNormal();
	Direction.Z = FMath::Max(Direction.Z, 0.) + 0.3;
	TriggerRagdoll(KnockdownInjury, Direction.GetSafeNormal() * KnockdownImpulseStrength, ImpactLocation);
	return true;
}

void UGMCGASPRagdollComponent::UpdateRagdollCamera(float DeltaTime)
{
	static const FName ShoulderOffsetName(TEXT("ShoulderOffset"));
	static const FName ArmLengthName(TEXT("ArmLength"));
	static const FName VerticalOffsetName(TEXT("VerticalOffset"));
	USkeletalMeshComponent* Mesh = GetMesh();
	const bool bWanted = bRagdollCamera && Mesh && IsLocallyControlledPawn()
		&& (bLocalRagdollActive || LocalPhase == EGMCGASPRagdollPhase::Ragdoll || LocalPhase == EGMCGASPRagdollPhase::GettingUp
			|| LocalPhase == EGMCGASPRagdollPhase::Dead);
	if (bWanted && !bRagdollCameraActive)
	{
		USceneComponent* Rig = nullptr;
		TInlineComponentArray<USceneComponent*> Components(GetOwner());
		for (USceneComponent* Component : Components)
		{
			if (Component->GetFName() == RagdollCameraRigName)
			{
				Rig = Component;
				break;
			}
		}
		if (!Rig)
		{
			return;
		}
		RagdollCameraRig = Rig;
		RagdollCameraRelative = Rig->GetRelativeLocation();
		RagdollCameraPivot = Rig->GetComponentLocation();
		GetNumber(Rig, ShoulderOffsetName, SavedRigShoulderOffset);
		GetNumber(Rig, ArmLengthName, SavedRigArmLength);
		GetNumber(Rig, VerticalOffsetName, SavedRigVerticalOffset);
		SetNumber(Rig, ShoulderOffsetName, 0.);
		SetNumber(Rig, ArmLengthName, (SavedRigArmLength + FVector2D(RagdollCameraRelative).Size()) * RagdollCameraArmScale);
		SetNumber(Rig, VerticalOffsetName, 0.);
		bRagdollCameraActive = true;
	}
	// First person back once the get-up is over (switched to third person by EnterRagdollLocal).
	if (!bWanted && bSavedFirstPersonView && !bLocalRagdollActive && LocalPhase == EGMCGASPRagdollPhase::None && IsLocallyControlledPawn())
	{
		SetViewModeForRagdoll(false);
	}
	USceneComponent* Rig = RagdollCameraRig.Get();
	if (!bRagdollCameraActive || !Rig)
	{
		bRagdollCameraActive = false;
		return;
	}
	// The rig's pivot is its own location (UpdateCamera): drive it to the body, then back to its place on the capsule.
	const FVector Home = Rig->GetAttachParent() ? Rig->GetAttachParent()->GetComponentTransform().TransformPosition(RagdollCameraRelative)
		: Rig->GetComponentLocation();
	const FVector Target = bWanted ? Mesh->GetSocketLocation(PelvisBone) + FVector(0., 0., RagdollCameraHeight) : Home;
	RagdollCameraPivot = FMath::VInterpTo(RagdollCameraPivot, Target, DeltaTime, RagdollCameraPivotSpeed);
	Rig->SetWorldLocation(RagdollCameraPivot);
	if (!bWanted)
	{
		SetNumber(Rig, ShoulderOffsetName, SavedRigShoulderOffset);
		SetNumber(Rig, ArmLengthName, SavedRigArmLength);
		SetNumber(Rig, VerticalOffsetName, SavedRigVerticalOffset);
		if (FVector::DistSquared(RagdollCameraPivot, Home) < 1.)
		{
			Rig->SetRelativeLocation(RagdollCameraRelative);
			bRagdollCameraActive = false;
		}
	}
}

void UGMCGASPRagdollComponent::SetMeshAttachmentsIgnoreCamera(bool bIgnore)
{
	if (!bIgnore)
	{
		for (const TPair<TWeakObjectPtr<UPrimitiveComponent>, TEnumAsByte<ECollisionResponse>>& Saved : SavedAttachmentCameraResponses)
		{
			if (UPrimitiveComponent* Primitive = Saved.Key.Get())
			{
				Primitive->SetCollisionResponseToChannel(ECC_Camera, Saved.Value);
			}
		}
		SavedAttachmentCameraResponses.Reset();
		return;
	}
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return;
	}
	// Other actors' components hanging on the mesh (weapon, parts, helmet gear), whatever their depth.
	TArray<USceneComponent*> Children;
	Mesh->GetChildrenComponents(true, Children);
	TSet<AActor*> Actors;
	for (const USceneComponent* Child : Children)
	{
		if (Child && Child->GetOwner() && Child->GetOwner() != GetOwner())
		{
			Actors.Add(Child->GetOwner());
		}
	}
	for (AActor* Actor : Actors)
	{
		TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
		for (UPrimitiveComponent* Primitive : Primitives)
		{
			const ECollisionResponse Response = Primitive->GetCollisionResponseToChannel(ECC_Camera);
			if (Response != ECR_Ignore)
			{
				SavedAttachmentCameraResponses.Emplace(Primitive, Response);
				Primitive->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
			}
		}
	}
}

void UGMCGASPRagdollComponent::CacheBodyMotion(float DeltaTime)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh || DeltaTime <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	if (MotionBodyBones.Num() == 0)
	{
		for (const FBodyInstance* Body : Mesh->Bodies)
		{
			if (Body && Body->BodySetup.IsValid())
			{
				MotionBodyBones.Add(Body->BodySetup->BoneName);
			}
		}
		MotionCurTM.SetNum(MotionBodyBones.Num());
		MotionPrevTM.SetNum(MotionBodyBones.Num());
		MotionSamples = 0;
	}
	for (int32 i = 0; i < MotionBodyBones.Num(); ++i)
	{
		MotionPrevTM[i] = MotionCurTM[i];
		MotionCurTM[i] = Mesh->GetSocketTransform(MotionBodyBones[i], RTS_World);
	}
	MotionDeltaTime = DeltaTime;
	MotionCacheTime = GetWorld()->GetTimeSeconds();
	++MotionSamples;
}

void UGMCGASPRagdollComponent::ApplyInheritedBodyVelocities()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	const int32 Samples = MotionSamples;
	// The next samples start after the ragdoll (the get-up moves the bones: the pair across it is no motion).
	MotionSamples = 0;
	// Two fresh samples needed (a teleport or a long pause would give nonsense).
	if (!Mesh || Samples < 2 || MotionDeltaTime <= UE_KINDA_SMALL_NUMBER || GetWorld()->GetTimeSeconds() - MotionCacheTime > 0.1)
	{
		return;
	}
	constexpr float MaxLinear = 2000.f;
	constexpr float MaxAngularDeg = 1440.f;
	for (int32 i = 0; i < MotionBodyBones.Num(); ++i)
	{
		const FVector Linear = ((MotionCurTM[i].GetLocation() - MotionPrevTM[i].GetLocation()) / MotionDeltaTime).GetClampedToMaxSize(MaxLinear);
		const FQuat Delta = (MotionCurTM[i].GetRotation() * MotionPrevTM[i].GetRotation().Inverse()).GetShortestArcWith(FQuat::Identity);
		const FVector Angular = (FMath::RadiansToDegrees(Delta.ToRotationVector()) / MotionDeltaTime).GetClampedToMaxSize(MaxAngularDeg);
		Mesh->SetPhysicsLinearVelocity(Linear, false, MotionBodyBones[i]);
		Mesh->SetPhysicsAngularVelocityInDegrees(Angular, false, MotionBodyBones[i]);
	}
}

void UGMCGASPRagdollComponent::MulticastHitReaction_Implementation(FName Bone, FVector_NetQuantize10 Impulse, FVector_NetQuantize Location)
{
	StartHitReaction(Bone, Impulse, Location);
}

void UGMCGASPRagdollComponent::StartHitReaction(FName Bone, const FVector& Impulse, const FVector& Location)
{
	using namespace GMCGASPRagdoll;
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh || !PhysicsControl || !bPhysicsControlReady || bLocalRagdollActive || LocalPhase != EGMCGASPRagdollPhase::None)
	{
		return;
	}
	if (AppliedPhysicsProfile != ProfileKinematic)
	{
		// Physics layer: the bodies already simulate.
		Mesh->AddImpulseAtLocation(Impulse, Location, Bone);
		return;
	}
	// The limb of the hit body (Physics Control Asset limbs).
	static const FName Limbs[] = {TEXT("Leg_L"), TEXT("Leg_R"), TEXT("Head"), TEXT("Arm_L"), TEXT("Arm_R"), TEXT("Torso")};
	FName Limb;
	const TArray<FName> Sets = PhysicsControl->GetSetsContainingBodyModifier(Bone);
	for (const FName& Candidate : Limbs)
	{
		if (Sets.Contains(Candidate))
		{
			Limb = Candidate;
			break;
		}
	}
	if (Limb.IsNone())
	{
		return;
	}
	if (!bPhysicalCollisionApplied)
	{
		// USkeletalMeshComponent::ShouldBlendPhysicsBones needs physics collision: with the hitboxes' query-only mesh
		// the leg simulated and moved (45 cm/s) but the bones never showed it.
		ApplyPhysicalCollision(true);
		bHitReactionCollision = true;
	}
	if (!HitReactionLimbs.Contains(Limb))
	{
		HitReactionLimbs.Add(Limb);
		PhysicsControl->SetBodyModifiersInSetMovementType(Limb, EPhysicsMovementType::Simulated);
		PhysicsControl->SetBodyModifiersInSetGravityMultiplier(Limb, 0.f);
		PhysicsControl->SetBodyModifiersInSetPhysicsBlendWeight(Limb, 1.f);
		FPhysicsControlData Parent;
		Parent.bEnabled = true;
		Parent.LinearStrength = 0.f;
		Parent.AngularStrength = HitReactionParentStrength;
		Parent.AngularDampingRatio = 3.f;
		PhysicsControl->SetControlDatasInSet(FName(*(TEXT("ParentSpace_") + Limb.ToString())), Parent);
		FPhysicsControlData World;
		World.bEnabled = true;
		World.LinearStrength = HitReactionWorldStrength;
		World.AngularStrength = HitReactionWorldStrength;
		PhysicsControl->SetControlDatasInSet(FName(*(TEXT("WorldSpace_") + Limb.ToString())), World);
	}
	HitReactionTimeLeft = HitReactionDuration + HitReactionBlendOut;
	// The body simulates once the Physics Control Component has applied its modifier (its next update).
	bPendingHitImpulse = true;
	PendingHitBone = Bone;
	PendingHitImpulse = Impulse;
	PendingHitLocation = Location;
}

void UGMCGASPRagdollComponent::UpdateHitReaction(float DeltaTime)
{
	if (HitReactionLimbs.Num() == 0)
	{
		if (bHitReactionCollision && !bLocalRagdollActive && LocalPhase == EGMCGASPRagdollPhase::None
			&& AppliedPhysicsProfile == GMCGASPRagdoll::ProfileKinematic)
		{
			bHitReactionCollision = false;
			RestorePhysicalCollision();
		}
		return;
	}
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh || !PhysicsControl || bLocalRagdollActive || LocalPhase != EGMCGASPRagdollPhase::None)
	{
		// The ragdoll or a montage took over (its profile replaces the reaction's modifiers and controls).
		HitReactionLimbs.Reset();
		bPendingHitImpulse = false;
		return;
	}
	if (bPendingHitImpulse && Mesh->IsSimulatingPhysics(PendingHitBone))
	{
		bPendingHitImpulse = false;
		Mesh->AddImpulseAtLocation(PendingHitImpulse, PendingHitLocation, PendingHitBone);
	}
	HitReactionTimeLeft -= DeltaTime;
	const float Weight = FMath::Clamp(HitReactionTimeLeft / HitReactionBlendOut, 0.f, 1.f);
	for (const FName& Limb : HitReactionLimbs)
	{
		PhysicsControl->SetBodyModifiersInSetPhysicsBlendWeight(Limb, Weight);
	}
	if (HitReactionTimeLeft <= 0.f)
	{
		HitReactionLimbs.Reset();
		bPendingHitImpulse = false;
		// Back to the default profile (kinematic bodies, controls off, blend weight 0).
		SetPhysicsProfile(AppliedPhysicsProfile);
	}
}

void UGMCGASPRagdollComponent::ApplyMeshTickOrder()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return;
	}
	const bool bWanted = CVarMeshTickAfterGMCMoves.GetValueOnGameThread() == 1;
	if (bWanted == bMeshTickChained)
	{
		return;
	}
	// Without aggregator (or with its aggregate tick off) GMC's own mesh -> movement component prerequisite applies.
	AGMC_Aggregator* Aggregator = AGMC_Aggregator::GetGMCAggregator(this);
	if (bWanted)
	{
		SavedMeshTickGroup = Mesh->PrimaryComponentTick.TickGroup;
		Mesh->SetTickGroup(TG_PrePhysics);
		if (Aggregator)
		{
			Mesh->AddTickPrerequisiteActor(Aggregator);
		}
	}
	else
	{
		if (Aggregator)
		{
			Mesh->RemoveTickPrerequisiteActor(Aggregator);
		}
		Mesh->SetTickGroup(SavedMeshTickGroup);
	}
	bMeshTickChained = bWanted;
}

void UGMCGASPRagdollComponent::ApplyPhysicsLayerSwitch()
{
	using namespace GMCGASPRagdoll;
	switch (CVarRagdollPhysicsLayer.GetValueOnGameThread())
	{
	case 0: DefaultPhysicsProfile = ProfileKinematic; break;
	case 1: DefaultPhysicsProfile = ProfilePhysicalAnimation; break;
	default: DefaultPhysicsProfile = ConfiguredPhysicsProfile; break;
	}
	// Standing pawns switch now; the others take it at their get-up (ApplyDefaultPhysicsProfile).
	if (!bPhysicsControlReady || bLocalRagdollActive || LocalPhase != EGMCGASPRagdollPhase::None || AppliedPhysicsProfile == DefaultPhysicsProfile)
	{
		return;
	}
	if (DefaultPhysicsProfile == ProfileKinematic)
	{
		RestorePhysicalCollision();
	}
	else
	{
		ApplyPhysicalCollision();
	}
	ApplyDefaultPhysicsProfile();
}

void UGMCGASPRagdollComponent::OnActiveTagsChanged(FGameplayTagContainer AddedTags, FGameplayTagContainer RemovedTags)
{
	if (!DeadTag.IsValid() || !AddedTags.HasTagExact(DeadTag))
	{
		return;
	}
	// Every machine goes limp at once (the tag is GMC-synced); the server then streams the corpse. Usually the phase is
	// already Dead here (early server death, RegisterPendingDamage).
	DeathFallbackTime = -1.;
	if (bCapsuleCollisionUntilDeadTag)
	{
		bCapsuleCollisionUntilDeadTag = false;
		if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(GetOwner()->GetRootComponent()))
		{
			Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}
	if (GetOwner()->HasAuthority())
	{
		ServerEnterDeath();
	}
	else if (LocalPhase != EGMCGASPRagdollPhase::Dead)
	{
		ApplyPhase(EGMCGASPRagdollPhase::Dead, LocalPhase);
	}
}

void UGMCGASPRagdollComponent::ServerEnterDeath()
{
	const EGMCGASPRagdollPhase Old = State.Phase;
	if (Old == EGMCGASPRagdollPhase::Dead)
	{
		return;
	}
	State.Phase = EGMCGASPRagdollPhase::Dead;
	++State.Serial;
	if (UGMCMotion* Motion = GetMotion())
	{
		Motion->SetRagdollMode(false);
	}
	GetOwner()->ForceNetUpdate();
	ApplyPhase(State.Phase, Old);
}

void UGMCGASPRagdollComponent::RunOwnerDeathHandling()
{
	// Same BP path as the State.Dead tag (BP_GMC_Pawn.HandleDeath: end abilities, drop the weapon, death kick, FP mesh);
	// its bIsDead guard makes the later tag call a no-op. After EnterRagdollLocal: the inherited body velocities would
	// overwrite the death kick.
	UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(GetOwner()->GetRootComponent());
	const UGMC_AbilitySystemComponent* Abilities = GetAbilities();
	const bool bDeadTagged = DeadTag.IsValid() && Abilities && Abilities->HasActiveTag(DeadTag);
	const ECollisionEnabled::Type CapsuleCollision = Capsule ? Capsule->GetCollisionEnabled() : ECollisionEnabled::NoCollision;
	if (UFunction* HandleDeath = GetOwner()->FindFunction(TEXT("HandleDeath")))
	{
		if (HandleDeath->NumParms == 0)
		{
			GetOwner()->ProcessEvent(HandleDeath, nullptr);
		}
	}
	// HandleDeath turns the capsule's collision off. Before the tag GMC still moves the pawn (the movement stops on
	// State.Dead, BP_GMCMovement.UpdateDeadMovementMode): without collision it fell through the floor (-235 cm at 220 ms
	// RTT). Kept until the tag (OnActiveTagsChanged).
	if (!bDeadTagged && Capsule && Capsule->GetCollisionEnabled() != CapsuleCollision)
	{
		Capsule->SetCollisionEnabled(CapsuleCollision);
		bCapsuleCollisionUntilDeadTag = true;
	}
}

void UGMCGASPRagdollComponent::RegisterPendingDamage(AActor* Target, float HealthDelta)
{
	if (!Target || !Target->HasAuthority())
	{
		return;
	}
	if (UGMCGASPRagdollComponent* Ragdoll = Target->FindComponentByClass<UGMCGASPRagdollComponent>())
	{
		Ragdoll->AddPendingDamage(HealthDelta);
	}
}

void UGMCGASPRagdollComponent::AddPendingDamage(float HealthDelta)
{
	UGMC_AbilitySystemComponent* Abilities = GetAbilities();
	if (!bEarlyServerDeath || !Abilities || !HealthTag.IsValid() || HealthDelta >= 0.f || IsDead())
	{
		return;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	PendingDamage.RemoveAll([this, Now](const FPendingDamage& P) { return Now - P.Time > PendingDamageLifetime; });
	PendingDamage.Add({HealthDelta, Now});
	const float Current = Abilities->GetAttributeValueByTag(HealthTag);
	float Health = Current;
	for (const FPendingDamage& P : PendingDamage)
	{
		Health += P.Delta;
	}
	// Also with "log LogGMCGASPRagdoll Verbose".
	if (bDebug || UE_LOG_ACTIVE(LogGMCGASPRagdoll, Verbose))
	{
		UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s pending damage %.1f: health %.1f -> %.1f with %d in flight"),
			*GetNameSafe(GetOwner()), HealthDelta, Current, Health, PendingDamage.Num());
	}
	if (Health <= 0.f && !(DeadTag.IsValid() && Abilities->HasActiveTag(DeadTag)))
	{
		UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s: early server death (health %.1f, %.1f in flight)"),
			*GetNameSafe(GetOwner()), Current, Health - Current);
		DeathFallbackTime = Now + DeathFallbackDelay;
		ServerEnterDeath();
	}
}

void UGMCGASPRagdollComponent::OnEffectApplied(UGMCAbilityEffect* AppliedEffect)
{
	if (!AppliedEffect || !GetOwner()->HasAuthority() || PendingDamage.Num() == 0)
	{
		return;
	}
	// GMAS applied damage that was in flight: consume it, oldest first (ServerAuth effects apply in queue order). Not
	// OnAttributeChanged: its edges are not one per effect (the pending damage was eaten early, measured).
	float Remaining = 0.f;
	for (const FGMCAttributeModifier& Modifier : AppliedEffect->EffectData.Modifiers)
	{
		if (Modifier.AttributeTag == HealthTag && Modifier.Op == EModifierType::Add
			&& Modifier.ValueType == EGMCAttributeModifierType::AMT_Value && Modifier.ModifierValue < 0.f)
		{
			Remaining += Modifier.ModifierValue;
		}
	}
	while (Remaining < -UE_KINDA_SMALL_NUMBER && PendingDamage.Num() > 0)
	{
		FPendingDamage& Oldest = PendingDamage[0];
		if (Oldest.Delta >= Remaining)
		{
			Remaining -= Oldest.Delta;
			PendingDamage.RemoveAt(0);
		}
		else
		{
			Oldest.Delta -= Remaining;
			Remaining = 0.f;
		}
	}
	if (DeathFallbackTime >= 0. && PendingDamage.Num() == 0)
	{
		DeathFallbackTime = FMath::Min(DeathFallbackTime, GetWorld()->GetTimeSeconds() + DeathFallbackCheckDelay);
	}
}

void UGMCGASPRagdollComponent::OnRep_State()
{
	if (State.Serial == LocalSerial && State.Phase == LocalPhase)
	{
		return;
	}
	LocalSerial = State.Serial;
	if (State.Phase != LocalPhase)
	{
		ApplyPhase(State.Phase, LocalPhase);
	}
}

void UGMCGASPRagdollComponent::UpdateMeshUpdateRate()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return;
	}
	// Bots use update rate optimizations (one evaluation every few frames, interpolated). Measured with them on:
	// - ragdoll / death: the skipped evaluations also skip the physics blend, a body killed off screen kept its standing
	//   bones for good;
	// - interaction / get-up montages: the montage (and the capsule it moves) advanced one frame in three, 10-12 cm steps
	//   then two still frames on a shoved bot ("it lags, like a correction");
	// - physics layer: the physics blend overwrites the interpolated frames, the animation ran as a 3-frame sawtooth and
	//   the simulated feet vibrated ~1.3 cm per frame.
	const bool bFullRate = LocalPhase != EGMCGASPRagdollPhase::None || bLocalRagdollActive
		|| AppliedPhysicsProfile == GMCGASPRagdoll::ProfilePhysicalAnimation;
	if (bFullRate && !bUROOverridden)
	{
		bSavedURO = Mesh->bEnableUpdateRateOptimizations;
		Mesh->bEnableUpdateRateOptimizations = false;
		bUROOverridden = true;
	}
	else if (!bFullRate && bUROOverridden)
	{
		Mesh->bEnableUpdateRateOptimizations = bSavedURO;
		bUROOverridden = false;
	}
}

void UGMCGASPRagdollComponent::ApplyPhase(EGMCGASPRagdollPhase NewPhase, EGMCGASPRagdollPhase OldPhase)
{
	LocalPhase = NewPhase;
	LocalSerial = State.Serial;
	UpdateMeshUpdateRate();
	if (bDebug)
	{
		UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s [%s] phase %d -> %d"), *GetNameSafe(GetOwner()),
			GetOwner()->HasAuthority() ? TEXT("server") : TEXT("client"), (int32)OldPhase, (int32)NewPhase);
	}
	if (OldPhase == EGMCGASPRagdollPhase::Interacting && NewPhase != EGMCGASPRagdollPhase::Interacting)
	{
		ExitInteractionLocal();
	}
	switch (NewPhase)
	{
	case EGMCGASPRagdollPhase::Ragdoll:
		EnterRagdollLocal(false);
		break;
	case EGMCGASPRagdollPhase::Dead:
		EnterRagdollLocal(true);
		RunOwnerDeathHandling();
		break;
	case EGMCGASPRagdollPhase::GettingUp:
		if (bLocalRagdollActive)
		{
			ExitRagdollLocal();
		}
		break;
	case EGMCGASPRagdollPhase::None:
		if (bLocalRagdollActive)
		{
			ExitRagdollLocal();
		}
		FinishGetUp();
		break;
	case EGMCGASPRagdollPhase::Interacting:
		EnterInteractionLocal();
		break;
	}
}

// =====================================================================================================================
//  Enter / exit (every machine)
// =====================================================================================================================

void UGMCGASPRagdollComponent::EnterRagdollLocal(bool bDead)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	UGMCMotion* Motion = GetMotion();
	if (!Mesh)
	{
		return;
	}
	const bool bServer = GetOwner()->HasAuthority();

	if (bLocalRagdollActive)
	{
		// Knocked down, then killed: the body goes limp.
		if (bDead)
		{
			if (PhysicsControl)
			{
				PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
			}
			bSettled = false;
			SettledFor = 0.f;
		}
		return;
	}
	bLocalRagdollActive = true;
	LocalRagdollStartTime = GetWorld()->GetTimeSeconds();
	bDefaultProfilePending = false;
	SetupPhysicsControl();
	SpineBody = ResolveBodyBone(SpineBone);

	if (UAnimInstance* AnimInstance = Mesh->GetAnimInstance())
	{
		AnimInstance->Montage_Stop(0.2f);
	}
	// Ended on the server and on the owner (End Abilities is local, both sides must do it: gmas-local-end-trap).
	UGMC_AbilitySystemComponent* Abilities = GetAbilities();
	if (Abilities && (bServer || IsLocallyControlledPawn()))
	{
		for (const FGameplayTag& Tag : AbilitiesToEnd)
		{
			if (Tag.IsValid())
			{
				Abilities->EndAbilitiesByTag(Tag);
			}
		}
	}
	if (!bDead)
	{
		// The get-ups end standing: leave crouch / prone (BP_GMCMovement.Stance, set by the owner and the server).
		if (Motion && (bServer || IsLocallyControlledPawn()))
		{
			SetByte(Motion, TEXT("Stance"), 0);
		}
	}
	if (IsLocallyControlledPawn())
	{
		SetViewModeForRagdoll(true);
	}

	// GMC mesh smoothing teleports simulating bodies every frame (death-ragdoll-gmc): off while ragdolling.
	if (Motion)
	{
		SavedSmoothComponent = Motion->GetComponentToSmooth();
		Motion->SetComponentToSmooth(nullptr);
	}
	Mesh->SetOwnerNoSee(false);
	UpdateMeshUpdateRate();
	SavedMeshRelative = Mesh->GetRelativeTransform();
	SavedMeshParent = Mesh->GetAttachParent();
	SavedMeshSocket = Mesh->GetAttachSocketName();
	Mesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	SavedMeshProfile = Mesh->GetCollisionProfileName();
	SavedMeshResponses = Mesh->GetCollisionResponseToChannels();
	SavedMeshObjectType = Mesh->GetCollisionObjectType();
	SavedMeshCollisionEnabled = Mesh->GetCollisionEnabled();
	Mesh->SetCollisionProfileName(RagdollCollisionProfile);
	if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(GetOwner()->GetRootComponent()))
	{
		SavedCapsulePawnResponse = Capsule->GetCollisionResponseToChannel(ECC_Pawn);
		SavedCapsuleCameraResponse = Capsule->GetCollisionResponseToChannel(ECC_Camera);
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	}

	// Dead too: the helmet gear stays on the corpse (the weapon is dropped, DropHeldItems).
	SetMeshAttachmentsIgnoreCamera(true);
	// Simulate now (the controls only switch the bodies when they tick, an impulse given before would be lost).
	const bool bBodiesWereKinematic = AppliedPhysicsProfile != ProfilePhysicalAnimation;
	Mesh->SetSimulatePhysics(true);
	if (bBodiesWereKinematic)
	{
		ApplyInheritedBodyVelocities();
	}
	if (PhysicsControl && bPhysicsControlReady)
	{
		SetPhysicsProfile(ProfileRagdoll);
		// Clients follow the server's bodies (ClientFollowPose); the dead are limp everywhere.
		if (!bServer || bDead)
		{
			PhysicsControl->SetControlsInSetEnabled(TEXT("All"), false);
		}
		PhysicsControl->SetBodyModifiersInSetGravityMultiplier(TEXT("All"), RagdollGravityMultiplier);
	}
	// The death kick is HandleDeath's (ApplyDeathImpulse, replicated hit); this one is the knock-down's.
	if (!bDead && !State.Impulse.IsNearlyZero())
	{
		const FName Bone = Mesh->FindClosestBone(State.ImpulseLocation, nullptr, 0.f, true);
		Mesh->AddImpulseAtLocation(State.Impulse, State.ImpulseLocation, Bone);
	}

	SetMovementAction(ActionRagdoll);
	bHasLastCom = false;
	ComVelocity = FVector::ZeroVector;
	AutoRollForce = FVector::ZeroVector;
	RollForce = FVector::ZeroVector;
	StillTime = 0.f;
	RagdollTime = 0.f;
	SendAccumulator = 1.f;
	SettledFor = 0.f;
	bSettled = false;
	PoseBuffer.Reset();
	bSnapOnNextPose = false;
	Strengths = bDead ? FVector4(0., 0., 0., 0.) : FVector4(1., 1., 1., 1.);
}

void UGMCGASPRagdollComponent::ExitRagdollLocal()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	UGMCMotion* Motion = GetMotion();
	bLocalRagdollActive = false;
	if (!Mesh)
	{
		return;
	}
	UAnimInstance* AnimInstance = Mesh->GetAnimInstance();
	// The ABP blends out of this snapshot (Ragdoll States / Blend Out Pose) into the get-up montage.
	FPoseSnapshot* Snapshot = nullptr;
	const FTransform OldComponent = Mesh->GetComponentTransform();
	if (AnimInstance)
	{
		Snapshot = &AnimInstance->AddPoseSnapshot(SnapshotName);
		Mesh->SnapshotPose(*Snapshot);
	}

	// GASP58 get-up step 4: the bodies stay simulated. The controls would read one frame of huge bone velocities while
	// the anim graph jumps to the snapshot: no target velocities on the parent space controls this frame, then the
	// default profile next frame (ApplyDefaultPhysicsProfile from the tick).
	if (PhysicsControl && bPhysicsControlReady)
	{
		FPhysicsControlData NoVelocity;
		NoVelocity.LinearTargetVelocityMultiplier = 0.f;
		NoVelocity.AngularTargetVelocityMultiplier = 0.f;
		PhysicsControl->SetControlDatasInSet(TEXT("ParentSpace"), NoVelocity);
		bDefaultProfilePending = true;
	}
	else
	{
		Mesh->SetSimulatePhysics(false);
	}
	// Back on the capsule; the snapshot's root is moved so the body stays where it lay. No teleport: the simulated bodies
	// stay where they are, the controls pull them onto the animation.
	// At runtime AttachToComponent detaches again a component whose root body simulates (SceneComponent.cpp): the
	// root body is kinematic for the attach only (PhysicsControl switches bodies with bPreserveExistingAttachment).
	if (USceneComponent* Parent = SavedMeshParent.Get())
	{
		FBodyInstance* RootBody = Mesh->GetBodyInstance();
		const bool bRootSimulated = RootBody && RootBody->bSimulatePhysics;
		if (bRootSimulated)
		{
			RootBody->SetInstanceSimulatePhysics(false, false, true);
		}
		Mesh->AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform, SavedMeshSocket);
		if (bRootSimulated)
		{
			RootBody->SetInstanceSimulatePhysics(true, false, true);
		}
		UE_CLOG(Mesh->GetAttachParent() != Parent, LogGMCGASPRagdoll, Warning, TEXT("%s: ragdoll mesh not re-attached"), *GetOwner()->GetName());
	}
	Mesh->SetRelativeTransform(SavedMeshRelative, false, nullptr, ETeleportType::None);
	if (Snapshot && Snapshot->bIsValid && Snapshot->LocalTransforms.Num() > 0)
	{
		Snapshot->LocalTransforms[0] = Snapshot->LocalTransforms[0] * OldComponent * Mesh->GetComponentTransform().Inverse();
	}
	Mesh->SetCollisionObjectType(SavedMeshObjectType);
	Mesh->SetCollisionResponseToChannels(SavedMeshResponses);
	Mesh->SetCollisionEnabled(SavedMeshCollisionEnabled);
	bPhysicalCollisionApplied = false;
	ApplyPhysicalCollision();
	if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(GetOwner()->GetRootComponent()))
	{
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, SavedCapsulePawnResponse);
		Capsule->SetCollisionResponseToChannel(ECC_Camera, SavedCapsuleCameraResponse);
	}
	if (Motion)
	{
		Motion->SetComponentToSmooth(SavedSmoothComponent.Get());
	}
	SetMeshAttachmentsIgnoreCamera(false);
	// First person comes back once standing (UpdateRagdollCamera), not here: restored at the start of the get-up the view
	// cut from the ragdoll camera onto the head, which then rode the whole get-up (jolt at the start, user 2026-09-30).

	SetMovementAction(ActionGettingUp);
	if (AnimInstance && State.Montage)
	{
		// Full weight at once: the ABP already fades from the ragdoll snapshot into the default branch (Blend Poses
		// (MovementAction), 0.5 s). The montage's own blend-in only let the locomotion under it show through: a fresh
		// chooser entry, a dead blending inertialization and Offset Root Bone catching the capsule (Rewind Debugger,
		// jolt at the start of the get-up, user 2026-09-30).
		AnimInstance->Montage_PlayWithBlendIn(State.Montage, FAlphaBlendArgs(0.f), 1.f, EMontagePlayReturnType::MontageLength,
			State.MontageStartTime, true);
	}
	GetUpStartActor = GetOwner()->GetActorTransform();
	GetUpMeshRotation = Mesh->GetComponentQuat();
	PoseBuffer.Reset();
}

void UGMCGASPRagdollComponent::FinishGetUp()
{
	SetMovementAction(ActionNone);
	if (UGMCMotion* Motion = GetMotion())
	{
		Motion->ClearRagdollCapsuleTarget();
	}
}

void UGMCGASPRagdollComponent::SetMovementAction(uint8 Action)
{
	SetByte(GetMotion(), MovementActionProperty, Action);
}

void UGMCGASPRagdollComponent::SetViewModeForRagdoll(bool bRagdoll)
{
	// First person spins with the head: third person while ragdolling (GASP uses a far camera).
	AActor* Owner = GetOwner();
	static const FName FirstPersonView(TEXT("bFirstPersonView"));
	static const FName ApplyViewMode(TEXT("ApplyViewMode"));
	if (bRagdoll)
	{
		bSavedFirstPersonView = GetBool(Owner, FirstPersonView, false);
		if (bSavedFirstPersonView)
		{
			SetBool(Owner, FirstPersonView, false);
			CallNoParams(Owner, ApplyViewMode);
		}
	}
	else if (bSavedFirstPersonView)
	{
		SetBool(Owner, FirstPersonView, true);
		CallNoParams(Owner, ApplyViewMode);
		bSavedFirstPersonView = false;
	}
}

// =====================================================================================================================
//  Tick
// =====================================================================================================================

void UGMCGASPRagdollComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	BindInput();
	UpdateMeshUpdateRate();

	// GASP58 BeginPlay: the controls exist for the whole life of the pawn, the default profile runs outside the ragdoll.
	// Here (first ticks) rather than BeginPlay: the mesh needs its physics state (and any game-side collision setup of
	// the mesh, done in BeginPlay, must be done before the physical collision is added to it).
	if (!bPhysicsControlReady && !bLocalRagdollActive)
	{
		SetupPhysicsControl();
		if (bPhysicsControlReady)
		{
			ApplyPhysicalCollision();
			ApplyDefaultPhysicsProfile();
		}
	}
	if (bDefaultProfilePending && !bLocalRagdollActive)
	{
		bDefaultProfilePending = false;
		ApplyDefaultPhysicsProfile();
	}
	if (DeathFallbackTime >= 0. && GetWorld()->GetTimeSeconds() >= DeathFallbackTime)
	{
		// Early death, but GMAS's Health is not 0 (no GE_Dead from the BP): kill through GMAS (GE_Dead, respawn timer).
		DeathFallbackTime = -1.;
		UGMC_AbilitySystemComponent* Abilities = GetAbilities();
		const float Health = Abilities && HealthTag.IsValid() ? Abilities->GetAttributeValueByTag(HealthTag) : 0.f;
		if (Abilities && DeathFallbackEffect && Health > 0.f && !(DeadTag.IsValid() && Abilities->HasActiveTag(DeadTag)))
		{
			UE_LOG(LogGMCGASPRagdoll, Warning, TEXT("%s: early death but Health %.2f (%d damage still in flight), applying %s"),
				*GetNameSafe(GetOwner()), Health, PendingDamage.Num(), *GetNameSafe(DeathFallbackEffect));
			bool bSuccess = false;
			int32 Handle = -1, Id = -1;
			UGMCAbilityEffect* Effect = nullptr;
			Abilities->ApplyAbilityEffectSafe(DeathFallbackEffect, FGMCAbilityEffectData(), EGMCAbilityEffectQueueType::ServerAuth, bSuccess, Handle, Id, Effect);
		}
	}
	UpdateHitReaction(DeltaTime);
	UpdateRagdollCamera(DeltaTime);
	if (!bLocalRagdollActive)
	{
		CacheBodyMotion(DeltaTime);
	}
	if (bPhysicsControlReady && !bLocalRagdollActive)
	{
		UpdateKinematicBodies(false);
	}

	const bool bServer = GetOwner()->HasAuthority();
	if (bServer && State.Phase == EGMCGASPRagdollPhase::GettingUp)
	{
		GetUpTimeLeft -= DeltaTime;
		if (GetUpTimeLeft <= 0.f)
		{
			const EGMCGASPRagdollPhase Old = State.Phase;
			State.Phase = EGMCGASPRagdollPhase::None;
			State.Montage = nullptr;
			++State.Serial;
			if (UGMCMotion* Motion = GetMotion())
			{
				Motion->SetRagdollMode(false);
			}
			if (UGMC_AbilitySystemComponent* Abilities = GetAbilities(); Abilities && RagdollEffectTag.IsValid())
			{
				Abilities->RemoveEffectByTagSafe(RagdollEffectTag, -1, EGMCAbilityEffectQueueType::ServerAuth);
			}
			ApplyPhase(State.Phase, Old);
		}
	}

	if (LocalPhase == EGMCGASPRagdollPhase::GettingUp)
	{
		UpdateMontageCapsule();
	}
	if (LocalPhase == EGMCGASPRagdollPhase::Interacting)
	{
		UpdateInteractionMotion();
		UpdateInteractionConstraints(DeltaTime);
	}
	UpdateInteractionIKBlendOut(DeltaTime);
	if (bServer && State.Phase == EGMCGASPRagdollPhase::Interacting)
	{
		// The montage's TriggerRagdoll notify hands the pawn to the ragdoll (UGMCGASPAnimNotify_TriggerRagdoll fires on the
		// server). Same moment from the montage time when the notify can't fire (GASP event without its class, or a
		// notify in the blend-out): a victim never leaves Interacting standing.
		const UAnimInstance* AnimInstance = GetMesh() ? GetMesh()->GetAnimInstance() : nullptr;
		const bool bPlaying = AnimInstance && State.Montage && AnimInstance->Montage_IsPlaying(State.Montage);
		const float Position = bPlaying ? AnimInstance->Montage_GetPosition(State.Montage) : UE_BIG_NUMBER;
		InteractionTimeLeft -= DeltaTime;
		if (InteractionRagdollTime >= 0.f && Position >= InteractionRagdollTime)
		{
			float Time = 0.f;
			EGMCGASPRagdollInjury Injury = EGMCGASPRagdollInjury::None;
			FindRagdollNotify(State.Montage, Time, Injury);
			TriggerRagdoll(Injury, FVector::ZeroVector, FVector::ZeroVector);
		}
		else if (InteractionTimeLeft <= 0.f)
		{
			FinishInteraction();
		}
	}
	if (bServer && State.Phase == EGMCGASPRagdollPhase::None && bTraceForObstacles)
	{
		TraceForObstacles();
	}
	if (!bLocalRagdollActive)
	{
		return;
	}
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return;
	}
	RagdollTime += DeltaTime;

	// Centre of mass velocity (GASP58 Ragdoll_UpdateVelocities): steadier than any single bone when rolling.
	const FVector Com = Mesh->GetSkeletalCenterOfMass();
	if (bHasLastCom && DeltaTime > UE_KINDA_SMALL_NUMBER)
	{
		ComVelocity = (Com - LastCom) / DeltaTime;
	}
	LastCom = Com;
	bHasLastCom = true;
	SpineVelocity = Mesh->GetPhysicsLinearVelocity(SpineBody);
	RagdollSpeed = SpineVelocity.Size();

	const bool bDead = LocalPhase == EGMCGASPRagdollPhase::Dead;
	if (bServer)
	{
		if (!bDead)
		{
			UpdateBehaviours(DeltaTime);
		}
		UpdateStrengths(DeltaTime);
		ServerStreamPose(DeltaTime);
	}
	else
	{
		ClientFollowPose(DeltaTime);
		FindFloor(FloorLocation, FloorNormal);
	}
	if (!bDead)
	{
		UpdateCapsuleTarget(DeltaTime);
	}
	// GASP58: the mesh stays on the capsule (which follows the pelvis). Ours is detached from it while ragdolling (GMC
	// teleports the capsule on corrections): it is moved along without teleport, the simulated bodies stay where they are.
	if (Mesh->GetAttachParent() == nullptr)
	{
		Mesh->SetWorldTransform(SavedMeshRelative * GetOwner()->GetActorTransform(), false, nullptr, ETeleportType::None);
	}
	PushAnimValues();
}

// =====================================================================================================================
//  Server behaviours (GASP58 Ragdoll_UpdateBehaviors)
// =====================================================================================================================

void UGMCGASPRagdollComponent::TraceForObstacles()
{
	const USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return;
	}
	const FVector Start = Mesh->GetSocketLocation(TEXT("neck_01"));
	const FVector End = (Mesh->GetSocketLocation(TEXT("foot_l")) + Mesh->GetSocketLocation(TEXT("foot_r"))) * 0.5;
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ObstacleObjectChannel);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollObstacles), false, GetOwner());
	FHitResult Hit;
	if (GetWorld()->SweepSingleByObjectType(Hit, Start, End, FQuat::Identity, Objects, FCollisionShape::MakeSphere(ObstacleTraceRadius), Params))
	{
		// No kick: the obstacle itself pushes the simulated bodies (GASP does the same).
		TriggerRagdoll(EGMCGASPRagdollInjury::None, FVector::ZeroVector, FVector::ZeroVector);
	}
}

FName UGMCGASPRagdollComponent::ResolveBodyBone(FName Bone) const
{
	const USkeletalMeshComponent* Mesh = GetMesh();
	FName Current = Bone;
	while (Mesh && !Current.IsNone())
	{
		if (Mesh->GetBodyInstance(Current))
		{
			return Current;
		}
		Current = Mesh->GetParentBone(Current);
	}
	return PelvisBone;
}

bool UGMCGASPRagdollComponent::FindFloor(FVector& OutLocation, FVector& OutNormal)
{
	const USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return false;
	}
	const FVector Start = Mesh->GetSocketLocation(PelvisBone);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollFloor), false, GetOwner());
	FHitResult Hit;
	const bool bHit = GetWorld()->SweepSingleByChannel(Hit, Start, Start - FVector(0., 0., 90.), FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(30.f), Params);
	bHasFloor = bHit;
	if (bHit)
	{
		OutLocation = Hit.ImpactPoint;
		OutNormal = Hit.ImpactNormal;
	}
	return bHit;
}

FTransform UGMCGASPRagdollComponent::GetRagdollTraceTransform() const
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return FTransform::Identity;
	}
	if (Mesh->DoesSocketExist(TraceSocket))
	{
		return Mesh->GetSocketTransform(TraceSocket);
	}
	static const FName TraceBone(TEXT("spine_03"));
	const int32 BoneIndex = Mesh->GetBoneIndex(TraceBone);
	if (BoneIndex == INDEX_NONE || !Mesh->GetSkinnedAsset())
	{
		return Mesh->GetSocketTransform(SpineBone);
	}
	if (!bTraceFrameCached)
	{
		// In the reference pose (standing, facing the component's +Y): X forward, Z up, 20 cm above spine_03.
		const FTransform BoneRef = FAnimationRuntime::GetComponentSpaceTransformRefPose(Mesh->GetSkinnedAsset()->GetRefSkeleton(), BoneIndex);
		const FTransform Desired(FRotationMatrix::MakeFromXZ(FVector(0., 1., 0.), FVector(0., 0., 1.)).ToQuat(),
			BoneRef.GetLocation() + FVector(0., 0., 20.));
		TraceFrameRelativeToSpine = Desired.GetRelativeTransform(BoneRef);
		bTraceFrameCached = true;
	}
	return TraceFrameRelativeToSpine * Mesh->GetSocketTransform(TraceBone);
}

void UGMCGASPRagdollComponent::UpdateBehaviours(float DeltaTime)
{
	USkeletalMeshComponent* Mesh = GetMesh();

	// Impact prediction (Ragdoll_UpdateImpactDirection): a projectile path along the spine's velocity.
	const FTransform Origin = GetRagdollTraceTransform();
	FPredictProjectilePathParams Predict(20.f, Origin.GetLocation(), SpineVelocity, 1.f, ECC_PhysicsBody, GetOwner());
	Predict.SimFrequency = 10.f;
	Predict.DrawDebugType = bDebug ? EDrawDebugTrace::ForOneFrame : EDrawDebugTrace::None;
	FPredictProjectilePathResult Result;
	UGameplayStatics::PredictProjectilePath(this, Predict, Result);
	TimeToImpact = Result.LastTraceDestination.Time;
	const FVector Direction = Origin.InverseTransformVectorNoScale((Result.LastTraceDestination.Location - Origin.GetLocation()).GetSafeNormal());
	ImpactDirection = FVector2D(FMath::RadiansToDegrees(FMath::Atan2(Direction.Y, Direction.X)),
		FMath::RadiansToDegrees(FMath::Atan2(Direction.Z, FVector2D(Direction.X, Direction.Y).Size())));

	// Auto roll on slopes (Ragdoll_UpdateRollBehavior) + the player's move input, as a torque on the spine.
	FindFloor(FloorLocation, FloorNormal);
	const float FloorAngle = bHasFloor ? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FloorNormal.Z, -1., 1.))) : 0.f;
	const FVector TargetAutoRoll = bHasFloor
		? FloorNormal * FMath::GetMappedRangeValueClamped(FVector2D(AutoRollAngles.X, AutoRollAngles.Y), FVector2D(0., 1.), FloorAngle)
		: FVector::ZeroVector;
	const float RollInterp = TargetAutoRoll.Size() < AutoRollForce.Size() ? 0.5f : 2.f;
	AutoRollForce = FMath::VInterpConstantTo(AutoRollForce, TargetAutoRoll, DeltaTime, RollInterp);
	// Only a player steers the roll: a bot keeps its AI path's input while knocked down, which rolled it along the path
	// (20 m at 400-600 cm/s) until it reached its goal, never still enough to get up.
	FVector MoveInput = FVector::ZeroVector;
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (const UGMCMotion* Motion = GetMotion(); Motion && OwnerPawn && OwnerPawn->IsPlayerControlled())
	{
		MoveInput = Motion->GetProcessedInputVector();
	}
	RollForce = (MoveInput + AutoRollForce).GetClampedToMaxSize(1.);
	if (Mesh->IsAnySimulatingPhysics() && !RollForce.IsNearlyZero())
	{
		Mesh->AddTorqueInDegrees(FVector::CrossProduct(FVector::UpVector, RollForce) * RollTorque, SpineBody, true);
	}

	// Fall speed cap (Ragdoll_UpdateGravity).
	if (PhysicsControl && bPhysicsControlReady)
	{
		const float Gravity = ComVelocity.Z <= -MaxFallSpeed ? 0.f : RagdollGravityMultiplier;
		PhysicsControl->SetBodyModifiersInSetGravityMultiplier(TEXT("All"), Gravity);
	}

	// Bots get up once still (NPC_GetupCheck); players ask (GetUpAction).
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (Pawn && !Pawn->IsPlayerControlled())
	{
		StillTime = RagdollSpeed <= BotGetUpSpeed ? StillTime + DeltaTime : 0.f;
		if (StillTime >= BotGetUpDelay)
		{
			ExitRagdoll(true);
		}
		else if (RagdollTime >= BotGetUpMaxDelay)
		{
			ExitRagdoll(false);
		}
	}
}

void UGMCGASPRagdollComponent::UpdateStrengths(float DeltaTime)
{
	if (!PhysicsControl || !bPhysicsControlReady)
	{
		return;
	}
	// GASP58 drives these from the ragdoll anim graph's curves; same values, picked from the same state here.
	FVector4 Target(0., 0., 0., 0.);
	if (LocalPhase == EGMCGASPRagdollPhase::Ragdoll)
	{
		const bool bOnGround = bHasFloor && TimeToImpact < 0.25f;
		if (!bOnGround)
		{
			Target = TimeToImpact <= 0.5f ? FVector4(1., 1., 1.5, 0.2) : FVector4(1., 1., 1., 1.);
		}
		else
		{
			const float Roll = FMath::Clamp((float)RollForce.Size(), 0.f, 1.f);
			Target = InjuryStrengths(State.Injury) * (1. - Roll) + FVector4(1., 1., 2., 1.) * Roll;
		}
	}
	const float Alpha = FMath::Clamp(DeltaTime / 0.2f, 0.f, 1.f);
	Strengths = Strengths + (Target - Strengths) * Alpha;
	for (int32 i = 0; i < 4; ++i)
	{
		FPhysicsControlMultiplier Multiplier;
		Multiplier.AngularStrengthMultiplier = Strengths[i];
		PhysicsControl->SetControlMultipliersInSet(StrengthSets[i], Multiplier, false);
	}
}

UAnimMontage* UGMCGASPRagdollComponent::PickGetUpMontage(float& OutStartTime)
{
	OutStartTime = 0.f;
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return GetUpFaceDown;
	}
	// Chest forward: the mesh faces its component +Y (mannequin import), the reference pose gives the spine's axis.
	const FTransform Spine = Mesh->GetSocketTransform(SpineBone);
	FVector ChestLocal(0., 1., 0.);
	const int32 SpineIndex = Mesh->GetBoneIndex(SpineBone);
	if (Mesh->GetSkinnedAsset() && SpineIndex != INDEX_NONE)
	{
		const FTransform SpineRef = FAnimationRuntime::GetComponentSpaceTransformRefPose(Mesh->GetSkinnedAsset()->GetRefSkeleton(), SpineIndex);
		ChestLocal = SpineRef.GetRotation().UnrotateVector(FVector(0., 1., 0.));
	}
	const FVector ChestWorld = Spine.GetRotation().RotateVector(ChestLocal);
	const bool bFaceUp = ChestWorld.Z > 0.;
	UAnimMontage* const OrientationMontage = bFaceUp ? GetUpFaceUp.Get() : GetUpFaceDown.Get();

	// GASP58 On_RagdollMode_Exit: the current (physical) pose goes into the pose history, then CHT_GetUpMontages motion
	// matches the get-ups of the right group against it and gives the montage and its start time.
	const bool bRolling = ComVelocity.Size2D() > RollingGetUpSpeed;
	UPoseSearchDatabase* Database = bRolling ? RollingGetUpDatabase.Get() : GetUpDatabase.Get();
	UAnimInstance* AnimInstance = Mesh->GetAnimInstance();
	if (Database && AnimInstance)
	{
		const double S0 = FPlatformTime::Seconds();
		UPoseSearchLibrary::OverridePoseHistoryFromOwningMesh(AnimInstance, PoseHistoryName);
		const double S1 = FPlatformTime::Seconds();
		FPoseSearchBlueprintResult Result;
		UPoseSearchLibrary::MotionMatch(AnimInstance, TArray<UObject*>{Database}, PoseHistoryName, FPoseSearchContinuingProperties(),
			FPoseSearchFutureProperties(), Result);
		UE_CLOG(bDebug, LogGMCGASPRagdoll, Log, TEXT("%s get-up search cost: history override %.2f ms, motion match %.2f ms"),
			*GetNameSafe(GetOwner()), (S1 - S0) * 1000., (FPlatformTime::Seconds() - S1) * 1000.);
		if (UAnimMontage* Selected = Cast<UAnimMontage>(Result.SelectedAnim.Get()))
		{
			OutStartTime = Result.SelectedTime;
			// Face down, the search picked the face-up stand get-up (cost 18-32, a real face-up landing costs 0.2-2.8): the
			// pawn snapped onto its back before getting up (user 2026-09-30). The stand get-ups must agree with the chest.
			const bool bStandGetUp = Selected == GetUpFaceUp || Selected == GetUpFaceDown;
			if (!bRolling && bStandGetUp && OrientationMontage && Selected != OrientationMontage)
			{
				UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s get-up: search picked %s (cost %.2f) but the chest faces %s: %s"),
					*GetNameSafe(GetOwner()), *Selected->GetName(), Result.SearchCost, bFaceUp ? TEXT("up") : TEXT("down"), *OrientationMontage->GetName());
				Selected = OrientationMontage;
			}
			else if (bDebug)
			{
				UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s get-up: %s at %.2f s (cost %.2f, rolling %d)"), *GetNameSafe(GetOwner()),
					*Selected->GetName(), Result.SelectedTime, Result.SearchCost, bRolling);
			}
			return Selected;
		}
		UE_LOG(LogGMCGASPRagdoll, Warning, TEXT("%s get-up: the pose search found nothing in %s, face up/down fallback"),
			*GetNameSafe(GetOwner()), *GetNameSafe(Database));
	}
	if (bRolling && GetUpRollLeft && GetUpRollRight)
	{
		const FVector Right = GetOwner()->GetActorRightVector();
		return FVector::DotProduct(ComVelocity, Right) >= 0. ? GetUpRollRight.Get() : GetUpRollLeft.Get();
	}
	if (bDebug)
	{
		UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s get-up: chest Z %.2f -> %s"), *GetNameSafe(GetOwner()), ChestWorld.Z, bFaceUp ? TEXT("face up") : TEXT("face down"));
	}
	return OrientationMontage;
}

// =====================================================================================================================
//  Capsule (GASP58 Get_RagdollTransform / Get_RagdollTargetOrientation / BP_MovementMode_Ragdoll)
// =====================================================================================================================

void UGMCGASPRagdollComponent::UpdateCapsuleTarget(float DeltaTime)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	UGMCMotion* Motion = GetMotion();
	if (!Mesh || !Motion)
	{
		return;
	}
	const FTransform Pelvis = Mesh->GetSocketTransform(PelvisBone);
	const FTransform Spine = Mesh->GetSocketTransform(SpineBone);
	FVector Along = Spine.GetLocation() - Pelvis.GetLocation();
	if (Spine.GetRotation().GetAxisY().Z > 0.)
	{
		Along = -Along;
	}
	FRotator Facing = GetOwner()->GetActorRotation();
	if (RagdollTime >= FreezeCapsuleRotationTime)
	{
		const FVector Planar = ComVelocity.Size2D() > RollingGetUpSpeed ? FVector(ComVelocity.X, ComVelocity.Y, 0.) : FVector(Along.X, Along.Y, 0.);
		if (!Planar.IsNearlyZero())
		{
			Facing = Planar.Rotation();
		}
	}
	FVector Location = Pelvis.GetLocation();
	if (bHasFloor)
	{
		Location.Z = FloorLocation.Z + Motion->GetRootCollisionHalfHeight(true) + 2.f;
	}
	Motion->SetRagdollCapsuleTarget(FTransform(FRotator(0., Facing.Yaw, 0.), Location), LocalPhase != EGMCGASPRagdollPhase::Ragdoll);
}

void UGMCGASPRagdollComponent::UpdateMontageCapsule()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	UGMCMotion* Motion = GetMotion();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	UAnimMontage* Montage = State.Montage;
	if (!Motion || !AnimInstance || !Montage || !AnimInstance->Montage_IsPlaying(Montage))
	{
		return;
	}
	// Root motion from the montage start to now (component space), turned into a capsule offset.
	const float Position = AnimInstance->Montage_GetPosition(Montage);
	const FTransform Delta = Montage->ExtractRootMotionFromTrackRange(State.MontageStartTime, Position, FAnimExtractContext());
	const FVector Offset = GetUpMeshRotation.RotateVector(Delta.GetTranslation());
	const FRotator Rotation(0., GetUpStartActor.Rotator().Yaw + Delta.GetRotation().Rotator().Yaw, 0.);
	Motion->SetRagdollCapsuleTarget(FTransform(Rotation, GetUpStartActor.GetLocation() + Offset), false);
}

void UGMCGASPRagdollComponent::PushAnimValues()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!AnimInstance)
	{
		return;
	}
	SetVector2D(AnimInstance, TEXT("RagdollImpactDirection"), ImpactDirection);
	SetReal(AnimInstance, TEXT("RagdollTimeToImpact"), TimeToImpact);
	SetReal(AnimInstance, TEXT("RagdollRollAlpha"), FMath::Clamp(RollForce.Size(), 0., 1.));
	SetReal(AnimInstance, TEXT("RagdollSpeed"), RagdollSpeed);
	SetBool(AnimInstance, TEXT("bRagdollOnGround"), bHasFloor && TimeToImpact < 0.25f);
	SetByte(AnimInstance, TEXT("RagdollInjury"), (uint8)State.Injury);
}

// =====================================================================================================================
//  Pose streaming
// =====================================================================================================================

bool UGMCGASPRagdollComponent::CapturePose(FGMCGASPRagdollNetPose& OutPose) const
{
	const USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh || Mesh->Bodies.Num() == 0)
	{
		return false;
	}
	OutPose.ServerTime = GetServerTime();
	OutPose.Bodies.SetNum(FMath::Min(Mesh->Bodies.Num(), 255));
	OutPose.RootIndex = 0;
	for (int32 i = 0; i < OutPose.Bodies.Num(); ++i)
	{
		const FBodyInstance* Body = Mesh->Bodies[i];
		OutPose.Bodies[i] = Body ? Body->GetUnrealWorldTransform() : FTransform::Identity;
		if (Body && Body->BodySetup.IsValid() && Body->BodySetup->BoneName == PelvisBone)
		{
			OutPose.RootIndex = (uint8)i;
		}
	}
	return true;
}

void UGMCGASPRagdollComponent::ServerStreamPose(float DeltaTime)
{
	if (bSettled)
	{
		return;
	}
	USkeletalMeshComponent* Mesh = GetMesh();
	if (LocalPhase == EGMCGASPRagdollPhase::Dead)
	{
		float MaxSpeed = 0.f;
		for (const FBodyInstance* Body : Mesh->Bodies)
		{
			if (Body)
			{
				MaxSpeed = FMath::Max(MaxSpeed, (float)Body->GetUnrealWorldVelocity().Size());
			}
		}
		SettledFor = MaxSpeed < SettleSpeed ? SettledFor + DeltaTime : 0.f;
		if (SettledFor >= SettleTime)
		{
			// Last pose, reliable (late joiners too), then the stream stops.
			bSettled = CapturePose(FinalPose);
			return;
		}
	}
	SendAccumulator += DeltaTime;
	if (SendAccumulator < 1.f / PoseSendRate)
	{
		return;
	}
	SendAccumulator = 0.f;
	FGMCGASPRagdollNetPose Pose;
	if (CapturePose(Pose))
	{
		MulticastPose(Pose);
	}
}

void UGMCGASPRagdollComponent::MulticastPose_Implementation(const FGMCGASPRagdollNetPose& Pose)
{
	if (GetOwner()->HasAuthority() || !bLocalRagdollActive)
	{
		return;
	}
	if (PoseBuffer.Num() > 0 && Pose.ServerTime <= PoseBuffer.Last().ServerTime)
	{
		return;
	}
	PoseBuffer.Add(Pose);
	++DebugPosesReceived;
	if (PoseBuffer.Num() > 16)
	{
		PoseBuffer.RemoveAt(0);
	}
}

void UGMCGASPRagdollComponent::OnRep_FinalPose()
{
	if (GetOwner()->HasAuthority() || FinalPose.Bodies.Num() == 0)
	{
		return;
	}
	if (!bLocalRagdollActive)
	{
		// Joined after the body settled: the phase arrives with it, start the ragdoll first.
		ApplyPhase(State.Phase, LocalPhase);
	}
	if (bLocalRagdollActive)
	{
		ApplyPoseInstant(FinalPose);
		PoseBuffer.Reset();
		FGMCGASPRagdollNetPose Held = FinalPose;
		Held.ServerTime = GetServerTime() + 3600.;
		PoseBuffer.Add(Held);
	}
}

bool UGMCGASPRagdollComponent::SamplePose(double Time, TArray<FTransform>& OutBodies) const
{
	if (PoseBuffer.Num() == 0)
	{
		return false;
	}
	int32 Next = 0;
	while (Next < PoseBuffer.Num() && PoseBuffer[Next].ServerTime < Time)
	{
		++Next;
	}
	if (Next == 0 || Next == PoseBuffer.Num())
	{
		OutBodies = PoseBuffer[Next == 0 ? 0 : PoseBuffer.Num() - 1].Bodies;
		return true;
	}
	const FGMCGASPRagdollNetPose& A = PoseBuffer[Next - 1];
	const FGMCGASPRagdollNetPose& B = PoseBuffer[Next];
	const float Alpha = (float)FMath::Clamp((Time - A.ServerTime) / FMath::Max(B.ServerTime - A.ServerTime, 0.001), 0., 1.);
	OutBodies.SetNum(FMath::Min(A.Bodies.Num(), B.Bodies.Num()));
	for (int32 i = 0; i < OutBodies.Num(); ++i)
	{
		OutBodies[i].SetLocation(FMath::Lerp(A.Bodies[i].GetLocation(), B.Bodies[i].GetLocation(), Alpha));
		OutBodies[i].SetRotation(FQuat::Slerp(A.Bodies[i].GetRotation(), B.Bodies[i].GetRotation(), Alpha));
		OutBodies[i].SetScale3D(FVector::OneVector);
	}
	return true;
}

void UGMCGASPRagdollComponent::ApplyPoseInstant(const FGMCGASPRagdollNetPose& Pose)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh)
	{
		return;
	}
	for (int32 i = 0; i < FMath::Min(Pose.Bodies.Num(), Mesh->Bodies.Num()); ++i)
	{
		if (FBodyInstance* Body = Mesh->Bodies[i])
		{
			Body->SetBodyTransform(Pose.Bodies[i], ETeleportType::TeleportPhysics);
			Body->SetLinearVelocity(FVector::ZeroVector, false);
			Body->SetAngularVelocityInRadians(FVector::ZeroVector, false);
		}
	}
}

void UGMCGASPRagdollComponent::ClientFollowPose(float DeltaTime)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh || PoseBuffer.Num() == 0 || DeltaTime <= UE_KINDA_SMALL_NUMBER)
	{
		return;
	}
	const double SinceStart = GetWorld()->GetTimeSeconds() - LocalRagdollStartTime;
	const bool bRamping = InterpolationDelayRamp > 0.f && SinceStart < InterpolationDelayRamp;
	const double Delay = bRamping ? InterpolationDelay * SinceStart / InterpolationDelayRamp : InterpolationDelay;
	const double RenderTime = GetServerTime() - Delay;
	if (bRamping && RenderTime > PoseBuffer.Last().ServerTime)
	{
		// No sample that recent yet: the local simulation (inherited velocities, death kick) carries the body.
		return;
	}
	TArray<FTransform> Target;
	TArray<FTransform> Ahead;
	if (!SamplePose(RenderTime, Target) || !SamplePose(RenderTime + 0.05, Ahead))
	{
		return;
	}
	// Drop the samples nobody will interpolate from again.
	while (PoseBuffer.Num() > 2 && PoseBuffer[1].ServerTime < RenderTime)
	{
		PoseBuffer.RemoveAt(0);
	}

	const int32 Count = FMath::Min(Target.Num(), Mesh->Bodies.Num());
	double ErrorSum = 0.;
	int32 Snaps = 0, Steered = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		FBodyInstance* Body = Mesh->Bodies[i];
		if (!Body || !Body->IsInstanceSimulatingPhysics())
		{
			continue;
		}
		Body->WakeInstance();
		const FTransform Current = Body->GetUnrealWorldTransform();
		const FVector Error = Target[i].GetLocation() - Current.GetLocation();
		ErrorSum += Error.Size();
		++Steered;
		if (Error.SizeSquared() > FMath::Square(SnapDistance))
		{
			++Snaps;
		}
		if (Error.SizeSquared() > FMath::Square(SnapDistance))
		{
			Body->SetBodyTransform(Target[i], ETeleportType::TeleportPhysics);
			continue;
		}
		// Velocity steering: the server's velocity plus closing the gap in SteeringTime. Contacts still resolve.
		const FVector TargetVelocity = (Ahead[i].GetLocation() - Target[i].GetLocation()) / 0.05;
		Body->SetLinearVelocity(TargetVelocity + Error / SteeringTime, false);

		const FQuat Delta = (Target[i].GetRotation() * Current.GetRotation().Inverse()).GetNormalized();
		FVector Axis;
		double Angle;
		Delta.ToAxisAndAngle(Axis, Angle);
		if (Angle > UE_PI)
		{
			Angle -= UE_TWO_PI;
		}
		const FQuat AheadDelta = (Ahead[i].GetRotation() * Target[i].GetRotation().Inverse()).GetNormalized();
		FVector AheadAxis;
		double AheadAngle;
		AheadDelta.ToAxisAndAngle(AheadAxis, AheadAngle);
		if (AheadAngle > UE_PI)
		{
			AheadAngle -= UE_TWO_PI;
		}
		Body->SetAngularVelocityInRadians(AheadAxis * (AheadAngle / 0.05) + Axis * (Angle / SteeringTime), false);
	}
	DebugTimer += DeltaTime;
	if (bDebug && DebugTimer >= 1.f)
	{
		DebugTimer = 0.f;
		UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s [client] poses %d buffer %d lag %.3f s, bodies %d/%d, mean error %.1f cm, snaps %d"),
			*GetNameSafe(GetOwner()), DebugPosesReceived, PoseBuffer.Num(), GetServerTime() - PoseBuffer.Last().ServerTime,
			Steered, Count, Steered ? ErrorSum / Steered : 0., Snaps);
	}
}

// =====================================================================================================================
//  Physics profiles (GASP58 SetPhysicsProfile)
// =====================================================================================================================

bool UGMCGASPRagdollComponent::IsActorRagdolling(const AActor* Actor)
{
	const UGMCGASPRagdollComponent* Ragdoll = Actor ? Actor->FindComponentByClass<UGMCGASPRagdollComponent>() : nullptr;
	return Ragdoll && Ragdoll->bLocalRagdollActive;
}

void UGMCGASPRagdollComponent::SetPhysicsProfile(FName Profile)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!PhysicsControl || !bPhysicsControlReady || !Mesh)
	{
		return;
	}
	// Profiles are recipes applied once (the PCC has no active profile): the last one is kept for the rest of the code.
	AppliedPhysicsProfile = Profile;
	PhysicsControl->InvokeControlProfile(Profile);
	// Kinematic, the bodies follow the animation a step late (GMC ticks the mesh after physics): the bones stay on the
	// animation (no physics blend). The ragdoll and the physics layer show the bodies.
	PhysicsControl->SetBodyModifiersInSetPhysicsBlendWeight(TEXT("All"), Profile == ProfileKinematic ? 0.f : 1.f);
	UpdateMeshUpdateRate();
	if (Profile != ProfileRagdoll)
	{
		// The ragdoll's SetSimulatePhysics(true) turns bBlendPhysics on, which blends every body whatever its weight.
		Mesh->bBlendPhysics = false;
	}
	if (Profile == ProfileRagdoll)
	{
		// Ragdoll: the legs collide with each other again and the joints get their limits back.
		CallBodyArraysCollision(TEXT("EnableCollisionBetweenBodyArrays"), Mesh, LeftLegBones, RightLegBones);
		Mesh->SetConstraintProfileForAll(NAME_None, false);
	}
	else
	{
		// Kinematic / PhysicalAnimation: no leg-on-leg collision, "Free" constraint profile (GASP58 calls it too; its
		// physics asset has no such profile, so the joints keep their default limits, as here).
		CallBodyArraysCollision(TEXT("DisableCollisionBetweenBodyArrays"), Mesh, LeftLegBones, RightLegBones);
		Mesh->SetConstraintProfileForAll(TEXT("Free"), false);
		UpdateKinematicBodies(true);
	}
}

void UGMCGASPRagdollComponent::ApplyDefaultPhysicsProfile()
{
	SetPhysicsProfile(DefaultPhysicsProfile);
}

void UGMCGASPRagdollComponent::UpdateKinematicBodies(bool bForce)
{
	if (!PhysicsControl || !bPhysicsControlReady || AppliedPhysicsProfile == ProfileRagdoll || AppliedPhysicsProfile == ProfileKinematic)
	{
		return;
	}
	static const FName FirstPersonView(TEXT("bFirstPersonView"));
	// Only outside the montages (get-up, interactions): there the body plays the montage in the physics layer, as in
	// GASP58.
	const bool bOverrides = LocalPhase == EGMCGASPRagdollPhase::None;
	const bool bHead = bOverrides && bKinematicHeadInFirstPerson && IsLocallyControlledPawn() && GetBool(GetOwner(), FirstPersonView, false);
	if (bForce || bHead != bHeadKinematic)
	{
		bHeadKinematic = bHead;
		PhysicsControl->SetBodyModifiersMovementType(FirstPersonKinematicBodies, bHead ? EPhysicsMovementType::Kinematic : EPhysicsMovementType::Simulated);
		// Kinematic bodies are a step late: their bones show the animation instead.
		PhysicsControl->SetBodyModifiersPhysicsBlendWeight(FirstPersonKinematicBodies, bHead ? 0.f : 1.f);
	}
}

void UGMCGASPRagdollComponent::ApplyPhysicalCollision(bool bForce)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh || bPhysicalCollisionApplied || (!bForce && DefaultPhysicsProfile == ProfileKinematic))
	{
		return;
	}
	bPhysicalCollisionApplied = true;
	if (!bStandingCollisionCaptured)
	{
		bStandingCollisionCaptured = true;
		StandingMeshObjectType = Mesh->GetCollisionObjectType();
		StandingMeshCollisionEnabled = Mesh->GetCollisionEnabled();
		StandingMeshResponses = Mesh->GetCollisionResponseToChannels();
	}
	// GASP58's mesh is in the Ragdoll profile all the time. Ours keeps the hitbox responses of its trace channels
	// (projectiles, lasers only: GMC sweeps, camera, visibility, interaction, terrain IK untouched) and only gains the
	// physical collision of a ragdoll with the object types (world, physics bodies, obstacles), still ignoring pawns.
	Mesh->SetCollisionObjectType(ECC_PhysicsBody);
	for (const ECollisionChannel Channel : {ECC_WorldStatic, ECC_WorldDynamic, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible, ObstacleObjectChannel.GetValue()})
	{
		Mesh->SetCollisionResponseToChannel(Channel, ECR_Block);
	}
	Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
}

void UGMCGASPRagdollComponent::RestorePhysicalCollision()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	if (!Mesh || !bPhysicalCollisionApplied)
	{
		return;
	}
	if (!bStandingCollisionCaptured)
	{
		return;
	}
	bPhysicalCollisionApplied = false;
	Mesh->SetCollisionObjectType(StandingMeshObjectType);
	Mesh->SetCollisionResponseToChannels(StandingMeshResponses);
	Mesh->SetCollisionEnabled(StandingMeshCollisionEnabled);
}

// =====================================================================================================================
//  Character interactions (GASP58 shove / tackle / takedown): server search, every machine plays
// =====================================================================================================================

void UGMCGASPRagdollComponent::OnShoveInput()
{
	RequestInteraction(EGMCGASPInteractionType::Shove);
}

void UGMCGASPRagdollComponent::OnTakedownInput()
{
	RequestInteraction(EGMCGASPInteractionType::Takedown);
}

void UGMCGASPRagdollComponent::RequestInteraction(EGMCGASPInteractionType Type)
{
	if (!GetOwner()->HasAuthority())
	{
		ServerRequestInteraction(Type);
		return;
	}
	const bool bStarted = TryInteraction(Type, nullptr);
	if (bDebug)
	{
		UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s interaction request %d: started %d"), *GetNameSafe(GetOwner()), (int32)Type, bStarted);
	}
}

void UGMCGASPRagdollComponent::ServerRequestInteraction_Implementation(EGMCGASPInteractionType Type)
{
	RequestInteraction(Type);
}

bool UGMCGASPRagdollComponent::CanDoCharacterInteraction() const
{
	// GASP58 CanDoCharacterInteraction: on the ground, no montage in DefaultSlot, not aiming (GMAS State.IsAiming).
	const UGMCMotion* Motion = GetMotion();
	const USkeletalMeshComponent* Mesh = GetMesh();
	const UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (State.Phase != EGMCGASPRagdollPhase::None || !Motion || !Motion->IsMovingOnGround() || !AnimInstance
		|| AnimInstance->IsSlotActive(TEXT("DefaultSlot")))
	{
		return false;
	}
	static const FGameplayTag AimingTag = FGameplayTag::RequestGameplayTag(TEXT("State.IsAiming"), false);
	UGMC_AbilitySystemComponent* Abilities = GetAbilities();
	return !(AimingTag.IsValid() && Abilities && Abilities->HasActiveTag(AimingTag));
}

bool UGMCGASPRagdollComponent::CanBeInteractedWith() const
{
	// GASP58 CanBeInteractedWith returns only "on the ground". The phase check is ours: a knocked-down, dead or already
	// interacting pawn has its own replicated montage / ragdoll.
	const UGMCMotion* Motion = GetMotion();
	return State.Phase == EGMCGASPRagdollPhase::None && Motion && Motion->IsMovingOnGround();
}

TArray<AActor*> UGMCGASPRagdollComponent::FindInteractableCharacters() const
{
	// GASP58 CheckForInteractableCharacters.
	TArray<AActor*> Result;
	const AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World)
	{
		return Result;
	}
	TArray<FOverlapResult> Overlaps;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(RagdollInteraction), false, Owner);
	World->OverlapMultiByObjectType(Overlaps, Owner->GetActorLocation(), FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
		FCollisionShape::MakeSphere(InteractionRange), Params);
	const FTransform OwnerTransform = Owner->GetActorTransform();
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Candidate = Overlap.GetActor();
		const UGMCGASPRagdollComponent* Other = Candidate ? Candidate->FindComponentByClass<UGMCGASPRagdollComponent>() : nullptr;
		if (!Other || Candidate == Owner || Result.Contains(Candidate) || !Other->CanBeInteractedWith())
		{
			continue;
		}
		const FVector Location = Candidate->GetActorLocation();
		if (FMath::Abs(Location.Z - Owner->GetActorLocation().Z) > InteractionMaxHeight)
		{
			continue;
		}
		const FVector Local = OwnerTransform.InverseTransformPositionNoScale(Location);
		if (FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(Local.Y, Local.X))) > InteractionHalfAngle)
		{
			continue;
		}
		// GASP traces the CharacterCapsule profile: only the level may be in between (the held weapons are attached actors
		// that a pawn-blocking trace would hit).
		FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(RagdollInteraction), false, Owner);
		TraceParams.AddIgnoredActor(Candidate);
		FHitResult Hit;
		if (World->SweepSingleByObjectType(Hit, Owner->GetActorLocation(), Location, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
			FCollisionShape::MakeCapsule(20.f, 60.f), TraceParams))
		{
			continue;
		}
		Result.Add(Candidate);
	}
	return Result;
}

bool UGMCGASPRagdollComponent::TryInteraction(EGMCGASPInteractionType Type, AActor* Target)
{
	USkeletalMeshComponent* Mesh = GetMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!GetOwner()->HasAuthority() || !AnimInstance || !CanDoCharacterInteraction())
	{
		return false;
	}
	TArray<AActor*> Targets;
	if (Target)
	{
		const UGMCGASPRagdollComponent* Other = Target->FindComponentByClass<UGMCGASPRagdollComponent>();
		if (Other && Target != GetOwner() && Other->CanBeInteractedWith())
		{
			Targets.Add(Target);
		}
	}
	else
	{
		Targets = FindInteractableCharacters();
	}
	if (Targets.IsEmpty())
	{
		return false;
	}

	// GASP58 Try_MultiCharacterInteraction: the chooser gives the databases (and roles) for the type and the speed, one
	// MotionMatchMulti query each, with this pawn's roles and every target's.
	if (Type == EGMCGASPInteractionType::Tackle)
	{
		Type = EGMCGASPInteractionType::Shove;
	}
	const float Speed = GetOwner()->GetVelocity().Size2D();
	TArray<FPoseSearchMotionMatchMultiQuery> Queries;
	for (const FGMCGASPInteractionDatabase& Row : InteractionDatabases)
	{
		if (Row.Type != Type || !Row.Database || Speed < Row.MinSpeed || Speed >= Row.MaxSpeed)
		{
			continue;
		}
		FPoseSearchMotionMatchMultiQuery& Query = Queries.AddDefaulted_GetRef();
		Query.Database = Row.Database;
		FPoseSearchAnimContextRoles& Initiator = Query.AnimContextsRoles.AddDefaulted_GetRef();
		Initiator.AnimContext = AnimInstance;
		Initiator.Roles = Row.InitiatorRoles;
		for (AActor* Candidate : Targets)
		{
			if (UAnimInstance* CandidateAnim = GetActorAnimInstance(Candidate))
			{
				FPoseSearchAnimContextRoles& Other = Query.AnimContextsRoles.AddDefaulted_GetRef();
				Other.AnimContext = CandidateAnim;
				Other.Roles = Row.TargetRoles;
			}
		}
	}
	if (Queries.IsEmpty())
	{
		return false;
	}
	TArray<FPoseSearchBlueprintResult> Results;
	UPoseSearchInteractionLibrary::MotionMatchMulti(Queries, PoseHistoryName, FPoseSearchContinuingProperties(), Results);

	// Every character of the result plays its role's montage from the matched time (PlayCharacterInteractionAnimation).
	bool bStarted = false;
	for (const FPoseSearchBlueprintResult& Result : Results)
	{
		const UAnimInstance* Context = Result.AnimContexts.IsValidIndex(Result.RoleIndex) ? Cast<UAnimInstance>(Result.AnimContexts[Result.RoleIndex]) : nullptr;
		AActor* Actor = Context ? Context->GetOwningActor() : nullptr;
		AActor* Partner = nullptr;
		for (int32 Index = 0; Index < Result.AnimContexts.Num(); ++Index)
		{
			const UAnimInstance* OtherContext = Cast<UAnimInstance>(Result.AnimContexts[Index]);
			if (Index != Result.RoleIndex && OtherContext)
			{
				Partner = OtherContext->GetOwningActor();
				break;
			}
		}
		UGMCGASPRagdollComponent* Ragdoll = Actor ? Actor->FindComponentByClass<UGMCGASPRagdollComponent>() : nullptr;
		if (!Ragdoll)
		{
			continue;
		}
		Ragdoll->State.bInitiator = Actor == GetOwner();
		const bool bOk = Ragdoll->StartInteractionFromResult(Result, Partner);
		bStarted |= bOk;
		if (bDebug)
		{
			UE_LOG(LogGMCGASPRagdoll, Log, TEXT("%s: %s as %s in %s from %.2f s (cost %.2f, db %s), partner %s, started %d"), *GetNameSafe(GetOwner()),
				*GetNameSafe(Actor), *Result.Role.ToString(), *GetNameSafe(Result.SelectedAnim), Result.SelectedTime, Result.SearchCost,
				*GetNameSafe(Result.SelectedDatabase), *GetNameSafe(Partner), bOk);
		}
	}
	return bStarted;
}

bool UGMCGASPRagdollComponent::StartInteractionFromResult(const FPoseSearchBlueprintResult& Result, AActor* InPartner)
{
	UMultiAnimAsset* Asset = Cast<UMultiAnimAsset>(Result.SelectedAnim);
	UAnimMontage* Montage = Asset ? Cast<UAnimMontage>(Asset->GetAnimationAsset(Result.Role)) : nullptr;
	const USkeletalMeshComponent* Mesh = GetMesh();
	if (!GetOwner()->HasAuthority() || !Montage || !Mesh || State.Phase != EGMCGASPRagdollPhase::None)
	{
		return false;
	}
	const EGMCGASPRagdollPhase Old = State.Phase;
	State.Phase = EGMCGASPRagdollPhase::Interacting;
	State.Injury = EGMCGASPRagdollInjury::None;
	State.Impulse = FVector::ZeroVector;
	State.Montage = Montage;
	State.MontageStartTime = Result.SelectedTime;
	State.InteractionAsset = Asset;
	State.Role = Result.Role;
	State.Partner = InPartner;
	State.bMirrored = Result.bIsMirrored;

	// GASP58 BP_NotifyState_MMI_MotionWarping OnWarpBegin: the fully aligned transform at the end of the warp window, from
	// the result (both characters' transforms at the search). The +90 deg of GASP is the mesh's rotation in the capsule.
	State.bWarp = false;
	float WarpStart = 0.f, WarpEnd = 0.f;
	const URootMotionModifier_Warp* Warp = nullptr;
	if (FindWarpWindow(Montage, WarpStart, WarpEnd, Warp))
	{
		FTransform Aligned;
		UPoseSearchInteractionLibrary::CalculateFullAlignedTransform(Result, WarpEnd - Result.SelectedTime, true, Aligned);
		const FQuat CapsuleRotation = Aligned.GetRotation() * Mesh->GetRelativeRotation().Quaternion().Inverse();
		State.bWarp = true;
		State.WarpLocation = Aligned.GetLocation() - CapsuleRotation.RotateVector(Mesh->GetRelativeLocation());
		State.WarpYaw = CapsuleRotation.Rotator().Yaw;
	}
	++State.Serial;

	InteractionTimeLeft = FMath::Max(0.2f, Montage->GetPlayLength() - Result.SelectedTime - Montage->GetDefaultBlendOutTime());
	InteractionRagdollTime = -1.f;
	EGMCGASPRagdollInjury Injury = EGMCGASPRagdollInjury::None;
	float NotifyTime = 0.f;
	if (FindRagdollNotify(Montage, NotifyTime, Injury) && NotifyTime >= Result.SelectedTime)
	{
		InteractionRagdollTime = NotifyTime;
		// The tackles' notify sits in the blend-out: never end the interaction before the hand-over.
		InteractionTimeLeft = FMath::Max(InteractionTimeLeft, NotifyTime - Result.SelectedTime + 0.1f);
	}
	if (UGMCMotion* Motion = GetMotion())
	{
		Motion->SetRagdollMode(true);
	}
	UGMC_AbilitySystemComponent* Abilities = GetAbilities();
	if (Abilities && RagdollEffect)
	{
		bool bSuccess = false;
		int32 Handle = -1, Id = -1;
		UGMCAbilityEffect* Effect = nullptr;
		Abilities->ApplyAbilityEffectSafe(RagdollEffect, FGMCAbilityEffectData(), EGMCAbilityEffectQueueType::ServerAuth, bSuccess, Handle, Id, Effect);
	}
	ApplyPhase(State.Phase, Old);
	return true;
}

void UGMCGASPRagdollComponent::EnterInteractionLocal()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!AnimInstance || !State.Montage)
	{
		return;
	}
	SetMovementAction(ActionNone);
	AnimInstance->Montage_Play(State.Montage, 1.f, EMontagePlayReturnType::MontageLength, State.MontageStartTime, true);
	InteractionRoot = GetOwner()->GetActorTransform();
	InteractionLastPosition = State.MontageStartTime;

	const URootMotionModifier_Warp* Warp = nullptr;
	bHasWarpWindow = FindWarpWindow(State.Montage, WarpStartTime, WarpEndTime, Warp);
	bWarpStarted = false;
	// Skew warp settings of the notify (GASP: warp translation and rotation, Z ignored except for the tackles).
	bWarpTranslation = Warp ? Warp->bWarpTranslation : true;
	bWarpIgnoreZ = Warp ? Warp->bIgnoreZAxis : true;
	bWarpRotation = Warp ? Warp->bWarpRotation : true;
	WarpRotationTimeMultiplier = Warp ? Warp->WarpRotationTimeMultiplier : 1.f;
	WarpEasing = Warp ? (uint8)Warp->AddTranslationEasingFunc : (uint8)EAlphaBlendOption::Linear;

	// GASP58: the two capsules ignore each other while moving (IgnoreComponentWhenMoving), the initiator's mesh ticks after
	// the other's (its hands' IK reads the other's pose).
	if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(GetOwner()->GetRootComponent()))
	{
		SavedInteractionPawnResponse = Capsule->GetCollisionResponseToChannel(ECC_Pawn);
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}
	InteractionPrerequisiteMesh = nullptr;
	const UGMCGASPRagdollComponent* PartnerRagdoll = State.Partner ? State.Partner->FindComponentByClass<UGMCGASPRagdollComponent>() : nullptr;
	if (USkeletalMeshComponent* PartnerMesh = PartnerRagdoll ? PartnerRagdoll->GetMesh() : nullptr; PartnerMesh && State.bInitiator)
	{
		Mesh->AddTickPrerequisiteComponent(PartnerMesh);
		InteractionPrerequisiteMesh = PartnerMesh;
	}
	InteractionConstraints.Reset();
	bInteractionIKActive = false;
}

void UGMCGASPRagdollComponent::ExitInteractionLocal()
{
	if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(GetOwner()->GetRootComponent()))
	{
		Capsule->SetCollisionResponseToChannel(ECC_Pawn, SavedInteractionPawnResponse);
	}
	// The partner may already be cleared from the state (FinishInteraction, TriggerRagdoll): the mesh was kept.
	USkeletalMeshComponent* Mesh = GetMesh();
	if (USkeletalMeshComponent* PartnerMesh = InteractionPrerequisiteMesh.Get(); Mesh && PartnerMesh)
	{
		Mesh->RemoveTickPrerequisiteComponent(PartnerMesh);
	}
	InteractionPrerequisiteMesh = nullptr;
	ClearInteractionConstraints();
}

void UGMCGASPRagdollComponent::FinishInteraction()
{
	const EGMCGASPRagdollPhase Old = State.Phase;
	State.Phase = EGMCGASPRagdollPhase::None;
	State.Montage = nullptr;
	State.InteractionAsset = nullptr;
	State.bWarp = false;
	++State.Serial;
	InteractionRagdollTime = -1.f;
	if (UGMCMotion* Motion = GetMotion())
	{
		Motion->SetRagdollMode(false);
	}
	if (UGMC_AbilitySystemComponent* Abilities = GetAbilities(); Abilities && RagdollEffectTag.IsValid())
	{
		Abilities->RemoveEffectByTagSafe(RagdollEffectTag, -1, EGMCAbilityEffectQueueType::ServerAuth);
	}
	ApplyPhase(State.Phase, Old);
	State.Partner = nullptr;
}

void UGMCGASPRagdollComponent::UpdateInteractionMotion()
{
	USkeletalMeshComponent* Mesh = GetMesh();
	UGMCMotion* Motion = GetMotion();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	UAnimMontage* Montage = State.Montage;
	if (!Motion || !AnimInstance || !Montage)
	{
		return;
	}
	const float Position = AnimInstance->Montage_IsPlaying(Montage) ? AnimInstance->Montage_GetPosition(Montage) : InteractionLastPosition;
	const float Previous = InteractionLastPosition;
	if (Position > Previous)
	{
		InteractionLastPosition = Position;
		// Root motion is in the mesh's space: work on the root (the mesh component's transform) and back to the capsule.
		const FQuat MeshRotation = Mesh->GetRelativeRotation().Quaternion();
		const FVector MeshOffset = Mesh->GetRelativeLocation();
		const FTransform Root(InteractionRoot.GetRotation() * MeshRotation, InteractionRoot.TransformPositionNoScale(MeshOffset));
		const FTransform RootMotion = Montage->ExtractRootMotionFromTrackRange(Previous, Position, FAnimExtractContext());
		FVector Translation = RootMotion.GetTranslation();
		FQuat Rotation = RootMotion.GetRotation();

		const bool bInWindow = State.bWarp && bHasWarpWindow && Previous < WarpEndTime && Position > WarpStartTime;
		if (bInWindow)
		{
			// URootMotionModifier_SkewWarp::ProcessRootMotion (the modifier becomes active at the first update in its window).
			if (!bWarpStarted)
			{
				bWarpStarted = true;
				WarpActualStartTime = Previous;
				WarpStartTransform = Root;
			}
			const FTransform Total = Montage->ExtractRootMotionFromTrackRange(Previous, WarpEndTime, FAnimExtractContext());
			const FTransform Delta = Montage->ExtractRootMotionFromTrackRange(Previous, FMath::Min(Position, WarpEndTime), FAnimExtractContext());
			const FTransform Extra = Position > WarpEndTime ? Montage->ExtractRootMotionFromTrackRange(WarpEndTime, Position, FAnimExtractContext()) : FTransform::Identity;
			const FTransform Window = Montage->ExtractRootMotionFromTrackRange(WarpStartTime, WarpEndTime, FAnimExtractContext());
			const FVector TargetRootLocation = FTransform(FRotator(0., State.WarpYaw, 0.), FVector(State.WarpLocation)).TransformPositionNoScale(MeshOffset);
			const FQuat TargetRootRotation = FRotator(0., State.WarpYaw, 0.).Quaternion() * MeshRotation;

			if (bWarpTranslation)
			{
				FVector TargetLocation = TargetRootLocation;
				if (bWarpIgnoreZ)
				{
					TargetLocation.Z = Root.GetLocation().Z;
				}
				constexpr double RootMotionThreshold = 2e-4;
				if (!Window.GetTranslation().IsNearlyZero(RootMotionThreshold))
				{
					if (!Delta.GetTranslation().IsNearlyZero(RootMotionThreshold))
					{
						FVector TargetLocal = Root.InverseTransformPositionNoScale(TargetLocation);
						if (bWarpIgnoreZ)
						{
							TargetLocal.Z = 0.;
						}
						Translation = URootMotionModifier_SkewWarp::WarpTranslation(FTransform::Identity, Delta.GetTranslation(), Total.GetTranslation(), TargetLocal)
							+ Extra.GetTranslation();
					}
				}
				else
				{
					// No translation in the animation: added along the way to the target, eased over the window.
					const FVector ToTarget = TargetLocation - Root.GetLocation();
					if (ToTarget.IsNearlyZero())
					{
						Translation = FVector::ZeroVector;
					}
					else
					{
						float Alpha = FMath::Clamp((Position - WarpActualStartTime) / FMath::Max(WarpEndTime - WarpActualStartTime, UE_KINDA_SMALL_NUMBER), 0.f, 1.f);
						Alpha = FAlphaBlend::AlphaToBlendOption(Alpha, (EAlphaBlendOption)WarpEasing, nullptr);
						FVector Next = FMath::Lerp(WarpStartTransform.GetLocation(), TargetLocation, (double)Alpha);
						if (bWarpIgnoreZ)
						{
							Next.Z = Root.GetLocation().Z;
						}
						const double Step = (Next - Root.GetLocation()).Size();
						Translation = Root.InverseTransformVectorNoScale(ToTarget.GetSafeNormal() * Step) + Extra.GetTranslation();
					}
				}
			}
			if (bWarpRotation)
			{
				// URootMotionModifier_Warp::WarpRotation, Slerp method.
				const FQuat TargetRotation = Root.GetRotation().Inverse() * TargetRootRotation;
				const float TimeRemaining = (WarpEndTime - Previous) * WarpRotationTimeMultiplier;
				const float Alpha = TimeRemaining > UE_KINDA_SMALL_NUMBER ? FMath::Clamp((Position - Previous) / TimeRemaining, 0.f, 1.f) : 1.f;
				const FQuat TotalRotation = Total.GetRotation();
				const FQuat TargetThisFrame = FQuat::Slerp(TotalRotation, TargetRotation, Alpha);
				FQuat DeltaOut = TargetThisFrame * TotalRotation.Inverse();
				DeltaOut = DeltaOut.Equals(FQuat::Identity) ? FQuat::Identity : DeltaOut;
				Rotation = Extra.GetRotation() * (DeltaOut * Delta.GetRotation());
			}
		}

		// Apply the root's delta (its space), then back to the capsule.
		const FVector NewRootLocation = Root.GetLocation() + Root.GetRotation().RotateVector(Translation);
		const double NewYaw = InteractionRoot.Rotator().Yaw + Rotation.Rotator().Yaw;
		const FQuat NewCapsuleRotation = FRotator(0., NewYaw, 0.).Quaternion();
		InteractionRoot = FTransform(NewCapsuleRotation, NewRootLocation - NewCapsuleRotation.RotateVector(MeshOffset));
	}
	Motion->SetRagdollCapsuleTarget(InteractionRoot, false);
}

void UGMCGASPRagdollComponent::UpdateInteractionConstraints(float DeltaTime)
{
	// GASP58 SandboxCharacter_Mover_ABP Update_InteractionConstraints: UpdateConstraints at the montage time, then the hands'
	// constraints (GetConstraint) as the full body IK's targets, relative to the mesh.
	USkeletalMeshComponent* Mesh = GetMesh();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	UMultiAnimAsset* Asset = State.InteractionAsset;
	const UGMCGASPRagdollComponent* PartnerRagdoll = State.Partner ? State.Partner->FindComponentByClass<UGMCGASPRagdollComponent>() : nullptr;
	USkeletalMeshComponent* PartnerMesh = PartnerRagdoll ? PartnerRagdoll->GetMesh() : nullptr;
	if (!AnimInstance || !Asset || !PartnerMesh || !State.Montage || !AnimInstance->Montage_IsActive(State.Montage) || Asset->GetNumRoles() != 2)
	{
		ClearInteractionConstraints();
		return;
	}
	const float AssetTime = AnimInstance->Montage_GetPosition(State.Montage);
	const int32 NumRoles = Asset->GetNumRoles();
	auto RoleMesh = [&](FName Role) -> USkeletalMeshComponent* { return Role == State.Role ? Mesh : PartnerMesh; };

	// UpdateConstraints, first part: the constraint notify states of every role's animation at AssetTime, their reach
	// ramped up while seen, cooled down once out of scope.
	TArray<bool> Visited;
	Visited.SetNumZeroed(InteractionConstraints.Num());
	TArray<FInteractionIKConstraint> NewConstraints;
	for (int32 RoleIndex = 0; RoleIndex < NumRoles; ++RoleIndex)
	{
		const UAnimationAsset* Animation = Asset->GetAnimationAsset(Asset->GetRole(RoleIndex));
		if (!Animation)
		{
			continue;
		}
		const UE::PoseSearch::FAnimationAssetSampler NotifySampler(Animation, FTransform::Identity, FVector::ZeroVector, UE::PoseSearch::FAnimationAssetSampler::DefaultRootTransformSamplingRate, false, false);
		FAnimNotifyContext NotifyContext;
		NotifySampler.ExtractAnimNotifyEvents(AssetTime, NotifyContext, [&](const FAnimNotifyEvent* Event)
			{
				const UAnimNotifyState_PoseSearchConstraint* Notify = Cast<const UAnimNotifyState_PoseSearchConstraint>(Event->NotifyStateClass);
				if (!Notify || AssetTime > Event->GetEndTriggerTime() - Notify->CoolDownTime)
				{
					return true;
				}
				auto SameSockets = [Notify](const FInteractionIKConstraint& Other)
				{
					return (Other.FromRole == Notify->FromSocketRole && Other.FromSocket == Notify->FromSocketName && Other.ToRole == Notify->ToSocketRole && Other.ToSocket == Notify->ToSocketName)
						|| (Other.FromRole == Notify->ToSocketRole && Other.FromSocket == Notify->ToSocketName && Other.ToRole == Notify->FromSocketRole && Other.ToSocket == Notify->FromSocketName);
				};
				const int32 Existing = InteractionConstraints.IndexOfByPredicate(SameSockets);
				if (Existing != INDEX_NONE)
				{
					Visited[Existing] = true;
					FInteractionIKConstraint& Constraint = InteractionConstraints[Existing];
					Constraint.DesiredReach = Constraint.RampUpTime > UE_KINDA_SMALL_NUMBER ? FMath::Min(1.f, Constraint.DesiredReach + DeltaTime / Constraint.RampUpTime) : 1.f;
				}
				else if (!NewConstraints.ContainsByPredicate(SameSockets))
				{
					FInteractionIKConstraint& Constraint = NewConstraints.AddDefaulted_GetRef();
					Constraint.FromRole = Notify->FromSocketRole;
					Constraint.FromSocket = Notify->FromSocketName;
					Constraint.ToRole = Notify->ToSocketRole;
					Constraint.ToSocket = Notify->ToSocketName;
					Constraint.TranslationWeight = Notify->TranslationWeight;
					Constraint.RotationWeight = Notify->RotationWeight;
					Constraint.RampUpTime = Notify->RampUpTime;
					Constraint.CoolDownTime = Notify->CoolDownTime;
					Constraint.DesiredReach = Constraint.RampUpTime <= UE_KINDA_SMALL_NUMBER ? 1.f : 0.f;
				}
				return true;
			});
	}
	for (int32 Index = InteractionConstraints.Num() - 1; Index >= 0; --Index)
	{
		if (!Visited[Index])
		{
			FInteractionIKConstraint& Constraint = InteractionConstraints[Index];
			const float Reach = Constraint.CoolDownTime > UE_KINDA_SMALL_NUMBER ? Constraint.DesiredReach - DeltaTime / Constraint.CoolDownTime : 0.f;
			if (Reach > UE_KINDA_SMALL_NUMBER)
			{
				Constraint.DesiredReach = Reach;
			}
			else
			{
				InteractionConstraints.RemoveAt(Index);
			}
		}
	}
	InteractionConstraints.Append(NewConstraints);

	// Second part: the roles' poses sampled at AssetTime in the asset's space (origin * root motion), indexed by skeleton
	// bone; each constraint wants its sockets where the animation puts them relative to each other.
	if (!InteractionConstraints.IsEmpty())
	{
		TArray<FName, TInlineAllocator<2>> Roles;
		TArray<FBoneContainer, TInlineAllocator<2>> Containers;
		TArray<FCompactPose, TInlineAllocator<2>> Poses;
		TArray<FCSPose<FCompactPose>, TInlineAllocator<2>> ComponentPoses;
		TArray<const USkeleton*, TInlineAllocator<2>> Skeletons;
		Roles.SetNum(NumRoles);
		Containers.SetNum(NumRoles);
		Poses.SetNum(NumRoles);
		ComponentPoses.SetNum(NumRoles);
		Skeletons.SetNum(NumRoles);
		for (int32 RoleIndex = 0; RoleIndex < NumRoles; ++RoleIndex)
		{
			Roles[RoleIndex] = Asset->GetRole(RoleIndex);
			const UAnimationAsset* Animation = Asset->GetAnimationAsset(Roles[RoleIndex]);
			const USkeleton* Skeleton = Animation ? Animation->GetSkeleton() : nullptr;
			Skeletons[RoleIndex] = Skeleton;
			if (!Skeleton)
			{
				continue;
			}
			const UE::PoseSearch::FAnimationAssetSampler Sampler(Animation, Asset->GetOrigin(Roles[RoleIndex]), FVector::ZeroVector, UE::PoseSearch::FAnimationAssetSampler::DefaultRootTransformSamplingRate, true, false);
			const int32 NumBones = Skeleton->GetReferenceSkeleton().GetNum();
			TArray<FBoneIndexType> BoneIndices;
			BoneIndices.SetNumUninitialized(NumBones);
			for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
			{
				BoneIndices[BoneIndex] = (FBoneIndexType)BoneIndex;
			}
			Containers[RoleIndex].InitializeTo(BoneIndices, UE::Anim::FCurveFilterSettings(UE::Anim::ECurveFilterMode::DisallowAll), *Skeleton);
			Poses[RoleIndex].SetBoneContainer(&Containers[RoleIndex]);
			Sampler.ExtractPose(AssetTime, Poses[RoleIndex]);
			Poses[RoleIndex][FCompactPoseBoneIndex(0)] = Sampler.ExtractRootTransform(AssetTime);
			ComponentPoses[RoleIndex].InitPose(Poses[RoleIndex]);
		}
		// World transform of a socket (or bone) of a role, current and in the animation (compact index = skeleton index:
		// the containers hold every skeleton bone in order).
		auto SocketTransforms = [&](FName Role, FName Socket, FTransform& OutCurrent, FTransform& OutAnim) -> bool
		{
			const int32 RoleIndex = Roles.IndexOfByKey(Role);
			USkeletalMeshComponent* RoleMeshComponent = RoleMesh(Role);
			if (RoleIndex == INDEX_NONE || !RoleMeshComponent || !Skeletons[RoleIndex])
			{
				return false;
			}
			FTransform SocketLocal = FTransform::Identity;
			FName Bone = Socket;
			if (const USkeletalMeshSocket* MeshSocket = RoleMeshComponent->GetSocketByName(Socket))
			{
				SocketLocal = MeshSocket->GetSocketLocalTransform();
				Bone = MeshSocket->BoneName;
			}
			const int32 SkeletonIndex = Skeletons[RoleIndex]->GetReferenceSkeleton().FindBoneIndex(Bone);
			if (SkeletonIndex == INDEX_NONE || RoleMeshComponent->GetBoneIndex(Bone) == INDEX_NONE)
			{
				return false;
			}
			OutCurrent = RoleMeshComponent->GetSocketTransform(Socket, RTS_World);
			OutAnim = SocketLocal * ComponentPoses[RoleIndex].GetComponentSpaceTransform(FCompactPoseBoneIndex(SkeletonIndex));
			return true;
		};
		for (int32 Index = InteractionConstraints.Num() - 1; Index >= 0; --Index)
		{
			FInteractionIKConstraint& Constraint = InteractionConstraints[Index];
			FTransform CurrentFrom, AnimFrom, CurrentTo, AnimTo;
			if (!SocketTransforms(Constraint.FromRole, Constraint.FromSocket, CurrentFrom, AnimFrom) || !SocketTransforms(Constraint.ToRole, Constraint.ToSocket, CurrentTo, AnimTo))
			{
				InteractionConstraints.RemoveAt(Index);
				continue;
			}
			const FTransform WantedCurrentTo = AnimTo.GetRelativeTransform(AnimFrom) * CurrentFrom;
			const FTransform WantedCurrentFrom = AnimFrom.GetRelativeTransform(AnimTo) * CurrentTo;
			Constraint.FromTransform.SetTranslation(FMath::Lerp(WantedCurrentFrom.GetTranslation(), CurrentFrom.GetTranslation(), (double)Constraint.TranslationWeight));
			Constraint.FromTransform.SetRotation(FQuat::Slerp(WantedCurrentFrom.GetRotation(), CurrentFrom.GetRotation(), Constraint.RotationWeight));
			Constraint.FromTransform.SetScale3D(CurrentFrom.GetScale3D());
			Constraint.ToTransform.SetTranslation(FMath::Lerp(WantedCurrentTo.GetTranslation(), CurrentTo.GetTranslation(), 1. - Constraint.TranslationWeight));
			Constraint.ToTransform.SetRotation(FQuat::Slerp(WantedCurrentTo.GetRotation(), CurrentTo.GetRotation(), 1.f - Constraint.RotationWeight));
			Constraint.ToTransform.SetScale3D(CurrentTo.GetScale3D());
		}
	}

	// GetConstraint(Role, hand): the reach and target of this pawn's hands.
	auto GetHand = [this](FName Socket, float& OutReach, FTransform& OutTarget)
	{
		for (const FInteractionIKConstraint& Constraint : InteractionConstraints)
		{
			if (Constraint.FromRole == State.Role && Constraint.FromSocket == Socket)
			{
				OutReach = Constraint.DesiredReach;
				OutTarget = Constraint.FromTransform;
				return;
			}
			if (Constraint.ToRole == State.Role && Constraint.ToSocket == Socket)
			{
				OutReach = Constraint.DesiredReach;
				OutTarget = Constraint.ToTransform;
				return;
			}
		}
		OutReach = 0.f;
		OutTarget = FTransform::Identity;
	};
	float ReachL = 0.f, ReachR = 0.f;
	FTransform TargetL, TargetR;
	GetHand(InteractionHandL, ReachL, TargetL);
	GetHand(InteractionHandR, ReachR, TargetR);
	const FTransform MeshTransform = Mesh->GetComponentTransform();
	// Enable_MMI_IK is true while the montage is active: blended in over 0.1 s (GASP Blend Poses by bool).
	InteractionIKAlpha = FMath::Min(1.f, InteractionIKAlpha + DeltaTime / 0.1f);
	SetReal(AnimInstance, TEXT("MMI_IK_Alpha"), InteractionIKAlpha);
	SetReal(AnimInstance, TEXT("MMI_IK_HandLAlpha"), ReachL);
	SetReal(AnimInstance, TEXT("MMI_IK_HandRAlpha"), ReachR);
	SetTransform(AnimInstance, TEXT("MMI_IK_HandLTarget"), TargetL.GetRelativeTransform(MeshTransform));
	SetTransform(AnimInstance, TEXT("MMI_IK_HandRTarget"), TargetR.GetRelativeTransform(MeshTransform));
	bInteractionIKActive = true;
}

void UGMCGASPRagdollComponent::ClearInteractionConstraints()
{
	InteractionConstraints.Reset();
	bInteractionIKActive = false;
}

void UGMCGASPRagdollComponent::UpdateInteractionIKBlendOut(float DeltaTime)
{
	// GASP Blend Poses by bool, false: 0.2 s back to the animation once the interaction montage is over.
	if (bInteractionIKActive || InteractionIKAlpha <= 0.f)
	{
		return;
	}
	InteractionIKAlpha = FMath::Max(0.f, InteractionIKAlpha - DeltaTime / 0.2f);
	USkeletalMeshComponent* Mesh = GetMesh();
	if (UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr)
	{
		SetReal(AnimInstance, TEXT("MMI_IK_Alpha"), InteractionIKAlpha);
	}
}
