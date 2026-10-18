// GMCMotionRagdoll - gmc.Ragdoll.Knockdown [self|others|all] [Impulse]: knocks pawns down through
// UGMCMotionRagdollComponent::TriggerRagdoll (the server-authoritative entry point any gameplay rule should call), pushed
// backwards by Impulse (default 20000). Bots get up by themselves once still; players get up with GetUpAction, or by
// themselves with bPlayersGetUpAutomatically. Runs on the authority world only. Ported from CLEAR (CLEAR.Debug.Knockdown).

#include "GMCMotionRagdollComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Containers/Ticker.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

static FAutoConsoleCommandWithWorldAndArgs GGMCMotionRagdollKnockdown(
	TEXT("gmc.Ragdoll.Knockdown"),
	TEXT("gmc.Ragdoll.Knockdown [self|others|all] [Impulse=20000] [DelaySeconds=0]: knock pawns down (networked ragdoll)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World || World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTemp, Warning, TEXT("gmc.Ragdoll.Knockdown: needs the server world"));
			return;
		}
		const float Delay = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.f;
		if (Delay > 0.f)
		{
			TArray<FString> Now = {Args[0], Args[1]};
			FTimerHandle Handle;
			World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(World, [World, Now]()
			{
				IConsoleManager::Get().ProcessUserConsoleInput(*FString::Printf(TEXT("gmc.Ragdoll.Knockdown %s %s"), *Now[0], *Now[1]), *GLog, World);
			}), Delay, false);
			return;
		}
		const FString Who = Args.Num() > 0 ? Args[0].ToLower() : TEXT("self");
		const float Impulse = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 20000.f;
		const APlayerController* LocalPC = World->GetFirstPlayerController();
		const APawn* Self = LocalPC ? LocalPC->GetPawn() : nullptr;
		int32 Count = 0;
		for (TObjectIterator<UGMCMotionRagdollComponent> It; It; ++It)
		{
			APawn* Pawn = Cast<APawn>(It->GetOwner());
			if (!Pawn || Pawn->GetWorld() != World || It->IsDead())
			{
				continue;
			}
			const bool bSelf = Pawn == Self;
			if ((Who == TEXT("self") && !bSelf) || (Who == TEXT("others") && bSelf))
			{
				continue;
			}
			It->TriggerRagdoll(EGMCMotionRagdollInjury::None, -Pawn->GetActorForwardVector() * Impulse, Pawn->GetActorLocation() + FVector(0., 0., 60.));
			++Count;
		}
		UE_LOG(LogTemp, Log, TEXT("gmc.Ragdoll.Knockdown %s: %d pawn(s) knocked down"), *Who, Count);
	}));

// gmc.Ragdoll.GetUp [self|others|all] [DelaySeconds]: forces knocked-down pawns back up (ExitRagdoll(true)), after
// DelaySeconds of game time when given (so a single command line can knock down, let the body fall, and get up).
static void GMCMotionRagdollGetUp(UWorld* World, const FString& Who)
{
	const APlayerController* LocalPC = World->GetFirstPlayerController();
	const APawn* Self = LocalPC ? LocalPC->GetPawn() : nullptr;
	int32 Count = 0;
	for (TObjectIterator<UGMCMotionRagdollComponent> It; It; ++It)
	{
		const APawn* Pawn = Cast<APawn>(It->GetOwner());
		if (!Pawn || Pawn->GetWorld() != World || It->IsDead() || It->GetPhase() != EGMCMotionRagdollPhase::Ragdoll)
		{
			continue;
		}
		const bool bSelf = Pawn == Self;
		if ((Who == TEXT("self") && !bSelf) || (Who == TEXT("others") && bSelf))
		{
			continue;
		}
		It->ExitRagdoll(true);
		++Count;
	}
	UE_LOG(LogTemp, Log, TEXT("gmc.Ragdoll.GetUp %s: %d pawn(s) getting up"), *Who, Count);
}

static FAutoConsoleCommandWithWorldAndArgs GGMCMotionRagdollGetUp(
	TEXT("gmc.Ragdoll.GetUp"),
	TEXT("gmc.Ragdoll.GetUp [self|others|all] [DelaySeconds=0]: force knocked-down pawns to get up."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World || World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTemp, Warning, TEXT("gmc.Ragdoll.GetUp: needs the server world"));
			return;
		}
		const FString Who = Args.Num() > 0 ? Args[0].ToLower() : TEXT("self");
		const float Delay = Args.Num() > 1 ? FCString::Atof(*Args[1]) : 0.f;
		if (Delay <= 0.f)
		{
			GMCMotionRagdollGetUp(World, Who);
			return;
		}
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(World, [World, Who]()
		{
			GMCMotionRagdollGetUp(World, Who);
		}), Delay, false);
	}));

// gmc.Ragdoll.Report [DelaySeconds]: logs each ragdoll pawn's phase and its mesh's attachment and relative transform,
// e.g. to compare the mesh before a knockdown and after the get-up.
static void GMCMotionRagdollReport(UWorld* World)
{
	for (TObjectIterator<UGMCMotionRagdollComponent> It; It; ++It)
	{
		const APawn* Pawn = Cast<APawn>(It->GetOwner());
		if (!Pawn || Pawn->GetWorld() != World)
		{
			continue;
		}
		const USkeletalMeshComponent* Mesh = Pawn->FindComponentByClass<USkeletalMeshComponent>();
		UE_LOG(LogTemp, Log, TEXT("gmc.Ragdoll.Report %s: phase %s, mesh %s attached to %s, relative %s, simulating %d"),
			*Pawn->GetName(), *UEnum::GetValueAsString(It->GetPhase()), *GetNameSafe(Mesh),
			Mesh ? *GetNameSafe(Mesh->GetAttachParent()) : TEXT("-"),
			Mesh ? *Mesh->GetRelativeTransform().ToString() : TEXT("-"),
			Mesh && Mesh->IsSimulatingPhysics() ? 1 : 0);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GGMCMotionRagdollReport(
	TEXT("gmc.Ragdoll.Report"),
	TEXT("gmc.Ragdoll.Report [DelaySeconds=0]: log ragdoll phase and mesh attachment of every pawn."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}
		const float Delay = Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.f;
		if (Delay <= 0.f)
		{
			GMCMotionRagdollReport(World);
			return;
		}
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(World, [World]()
		{
			GMCMotionRagdollReport(World);
		}), Delay, false);
	}));

// gmc.Ragdoll.FirstPerson [0|1]: sets (or with no argument flips) bFirstPersonRagdoll on the local player's pawn in this
// world. Applies from the next knockdown.
static FAutoConsoleCommandWithWorldAndArgs GGMCMotionRagdollFirstPerson(
	TEXT("gmc.Ragdoll.FirstPerson"),
	TEXT("gmc.Ragdoll.FirstPerson [0|1]: first-person ragdoll on the local pawn (no argument flips it); next knockdown."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		const APlayerController* LocalPC = World ? World->GetFirstPlayerController() : nullptr;
		const APawn* Pawn = LocalPC ? LocalPC->GetPawn() : nullptr;
		UGMCMotionRagdollComponent* Ragdoll = Pawn ? Pawn->FindComponentByClass<UGMCMotionRagdollComponent>() : nullptr;
		if (!Ragdoll)
		{
			UE_LOG(LogTemp, Warning, TEXT("gmc.Ragdoll.FirstPerson: no local pawn with a ragdoll component"));
			return;
		}
		Ragdoll->bFirstPersonRagdoll = Args.Num() > 0 ? FCString::Atoi(*Args[0]) != 0 : !Ragdoll->bFirstPersonRagdoll;
		UE_LOG(LogTemp, Log, TEXT("gmc.Ragdoll.FirstPerson: %d on %s (from the next knockdown)"),
			Ragdoll->bFirstPersonRagdoll ? 1 : 0, *Pawn->GetName());
	}));

// gmc.Ragdoll.Trace [Seconds=10]: logs, every frame for Seconds, the local pawn's ragdoll phase, whether the view is first
// person (the owner's bFirstPersonView), its facing, the view (control) yaw, the camera yaw window and the camera's
// distance from the head bone - for tuning the knockdown camera and first-person ragdoll (bFirstPersonRagdoll).
static FAutoConsoleCommandWithWorldAndArgs GGMCMotionRagdollTrace(
	TEXT("gmc.Ragdoll.Trace"),
	TEXT("gmc.Ragdoll.Trace [Seconds=10]: log the local pawn's ragdoll phase, view and camera every frame."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}
		const double Seconds = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 10.;
		const double EndTime = FPlatformTime::Seconds() + Seconds;
		TWeakObjectPtr<UWorld> WeakWorld = World;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakWorld, EndTime](float)
		{
			UWorld* W = WeakWorld.Get();
			if (!W || FPlatformTime::Seconds() > EndTime)
			{
				return false;
			}
			APlayerController* PC = W->GetFirstPlayerController();
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			const UGMCMotionRagdollComponent* Ragdoll = Pawn ? Pawn->FindComponentByClass<UGMCMotionRagdollComponent>() : nullptr;
			if (!Ragdoll)
			{
				return true;
			}
			const FBoolProperty* FirstPerson = FindFProperty<FBoolProperty>(Pawn->GetClass(), TEXT("bFirstPersonView"));
			const bool bFirstPerson = FirstPerson && FirstPerson->GetPropertyValue_InContainer(Pawn);
			const APlayerCameraManager* Camera = PC->PlayerCameraManager;
			const USkeletalMeshComponent* Mesh = Pawn->FindComponentByClass<USkeletalMeshComponent>();
			const double HeadDistance = Camera && Mesh && Mesh->GetBoneIndex(TEXT("head")) != INDEX_NONE
				? FVector::Dist(Camera->GetCameraLocation(), Mesh->GetBoneLocation(TEXT("head"))) : -1.;
			UE_LOG(LogTemp, Log, TEXT("gmc.Ragdoll.Trace t=%.3f phase=%s fp=%d fpRagdoll=%d facing=%.1f view=%.1f window=[%.1f,%.1f] camYaw=%.1f camHead=%.1f"),
				W->GetTimeSeconds(), *UEnum::GetValueAsString(Ragdoll->GetPhase()), bFirstPerson ? 1 : 0,
				Ragdoll->IsFirstPersonRagdollActive() ? 1 : 0, Pawn->GetActorRotation().Yaw, PC->GetControlRotation().Yaw,
				Camera ? Camera->ViewYawMin : 0.f, Camera ? Camera->ViewYawMax : 0.f,
				Camera ? Camera->GetCameraRotation().Yaw : 0., HeadDistance);
			return true;
		}));
	}));

// gmc.Ragdoll.TraceBones [Seconds=10]: logs, every frame for Seconds, every pawn in a game world that is ragdolling or
// getting up: net role, phase, whether any body is awake, the mesh component's location and the world positions of the
// pelvis, head, right hand and left foot - for finding twitching (large frame-to-frame accelerations of a body that is
// otherwise still) on the server, the owning client and simulated proxies.
static FAutoConsoleCommandWithWorldAndArgs GGMCMotionRagdollTraceBones(
	TEXT("gmc.Ragdoll.TraceBones"),
	TEXT("gmc.Ragdoll.TraceBones [Seconds=10]: log ragdolling pawns' bones and mesh every frame."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			return;
		}
		const double Seconds = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 10.;
		const double EndTime = FPlatformTime::Seconds() + Seconds;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([EndTime](float)
		{
			if (FPlatformTime::Seconds() > EndTime)
			{
				return false;
			}
			static const FName Bones[] = {TEXT("pelvis"), TEXT("head"), TEXT("hand_r"), TEXT("foot_l")};
			for (TObjectIterator<UGMCMotionRagdollComponent> It; It; ++It)
			{
				const APawn* Pawn = Cast<APawn>(It->GetOwner());
				UWorld* W = Pawn ? Pawn->GetWorld() : nullptr;
				if (!W || !W->IsGameWorld() || It->GetPhase() == EGMCMotionRagdollPhase::None)
				{
					continue;
				}
				USkeletalMeshComponent* Mesh = Pawn->FindComponentByClass<USkeletalMeshComponent>();
				if (!Mesh)
				{
					continue;
				}
				FString Line = FString::Printf(TEXT("t=%.4f pawn=%s role=%d phase=%d awake=%d mesh=%s"), W->GetTimeSeconds(),
					*Pawn->GetName(), (int32)Pawn->GetLocalRole(), (int32)It->GetPhase(), Mesh->IsAnyRigidBodyAwake() ? 1 : 0,
					*Mesh->GetComponentLocation().ToCompactString());
				for (const FName& Bone : Bones)
				{
					const FVector P = Mesh->GetBoneLocation(Bone);
					Line += FString::Printf(TEXT(" %s=%.2f,%.2f,%.2f"), *Bone.ToString(), P.X, P.Y, P.Z);
				}
				UE_LOG(LogTemp, Log, TEXT("gmc.Ragdoll.TraceBones %s"), *Line);
			}
			return true;
		}));
	}));
