#include "Combat/GMCGASPRagdollTriggers.h"

#include "Components/BoxComponent.h"
#include "Components/GMCMotion.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Pawn.h"

void UGMCGASPAnimNotify_TriggerRagdoll::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);
	AActor* Owner = MeshComp ? MeshComp->GetOwner() : nullptr;
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}
	if (UGMCGASPRagdollComponent* Ragdoll = Owner->FindComponentByClass<UGMCGASPRagdollComponent>())
	{
		Ragdoll->TriggerRagdoll(Injury, FVector::ZeroVector, FVector::ZeroVector);
	}
}

AGMCGASPZeroGZone::AGMCGASPZeroGZone()
{
	Zone = CreateDefaultSubobject<UBoxComponent>(TEXT("Zone"));
	Zone->SetBoxExtent(FVector(500.f, 500.f, 300.f));
	Zone->SetCollisionProfileName(TEXT("Trigger"));
	RootComponent = Zone;
	Zone->OnComponentBeginOverlap.AddDynamic(this, &AGMCGASPZeroGZone::OnZoneBeginOverlap);
	Zone->OnComponentEndOverlap.AddDynamic(this, &AGMCGASPZeroGZone::OnZoneEndOverlap);
}

void AGMCGASPZeroGZone::OnZoneBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	ApplyGravity(OtherActor, GravityScale);
}

void AGMCGASPZeroGZone::OnZoneEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	ApplyGravity(OtherActor, 1.f);
}

void AGMCGASPZeroGZone::ApplyGravity(AActor* Pawn, float Scale) const
{
	if (!Cast<APawn>(Pawn))
	{
		return;
	}
	if (UGMCMotion* Motion = Pawn->FindComponentByClass<UGMCMotion>())
	{
		Motion->GravityScale = Scale;
	}
	if (UGMCGASPRagdollComponent* Ragdoll = Pawn->FindComponentByClass<UGMCGASPRagdollComponent>())
	{
		Ragdoll->RagdollGravityMultiplier = Scale;
	}
}
