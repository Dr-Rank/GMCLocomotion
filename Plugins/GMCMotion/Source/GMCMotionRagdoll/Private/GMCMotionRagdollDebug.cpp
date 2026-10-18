// GMCMotionRagdoll - gmc.Ragdoll.Knockdown [self|others|all] [Impulse]: knocks pawns down through
// UGMCMotionRagdollComponent::TriggerRagdoll (the server-authoritative entry point any gameplay rule should call), pushed
// backwards by Impulse (default 20000). Bots get up by themselves once still; players get up with GetUpAction, or by
// themselves with bPlayersGetUpAutomatically. Runs on the authority world only. Ported from CLEAR (CLEAR.Debug.Knockdown).

#include "GMCMotionRagdollComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"

static FAutoConsoleCommandWithWorldAndArgs GGMCMotionRagdollKnockdown(
	TEXT("gmc.Ragdoll.Knockdown"),
	TEXT("gmc.Ragdoll.Knockdown [self|others|all] [Impulse=20000]: knock pawns down (networked ragdoll)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		if (!World || World->GetNetMode() == NM_Client)
		{
			UE_LOG(LogTemp, Warning, TEXT("gmc.Ragdoll.Knockdown: needs the server world"));
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
