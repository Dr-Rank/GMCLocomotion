#include "GMCMotionRagdollTriggers.h"

#include "Components/BoxComponent.h"
#include "Components/GMCMotion.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Pawn.h"

void UGMCMotionAnimNotify_TriggerRagdoll::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);
	AActor* Owner = MeshComp ? MeshComp->GetOwner() : nullptr;
	if (!Owner || !Owner->HasAuthority())
	{
		return;
	}
	if (UGMCMotionRagdollComponent* Ragdoll = Owner->FindComponentByClass<UGMCMotionRagdollComponent>())
	{
		Ragdoll->TriggerRagdoll(Injury, FVector::ZeroVector, FVector::ZeroVector);
	}
}

AGMCMotionZeroGZone::AGMCMotionZeroGZone()
{
	Zone = CreateDefaultSubobject<UBoxComponent>(TEXT("Zone"));
	Zone->SetBoxExtent(FVector(500.f, 500.f, 300.f));
	Zone->SetCollisionProfileName(TEXT("Trigger"));
	RootComponent = Zone;
	Zone->OnComponentBeginOverlap.AddDynamic(this, &AGMCMotionZeroGZone::OnZoneBeginOverlap);
	Zone->OnComponentEndOverlap.AddDynamic(this, &AGMCMotionZeroGZone::OnZoneEndOverlap);
}

void AGMCMotionZeroGZone::OnZoneBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp,
	int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	ApplyGravity(OtherActor, GravityScale);
}

void AGMCMotionZeroGZone::OnZoneEndOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex)
{
	ApplyGravity(OtherActor, 1.f);
}

void AGMCMotionZeroGZone::ApplyGravity(AActor* Pawn, float Scale) const
{
	if (!Cast<APawn>(Pawn))
	{
		return;
	}
	if (UGMCMotion* Motion = Pawn->FindComponentByClass<UGMCMotion>())
	{
		Motion->GravityScale = Scale;
	}
	if (UGMCMotionRagdollComponent* Ragdoll = Pawn->FindComponentByClass<UGMCMotionRagdollComponent>())
	{
		Ragdoll->RagdollGravityMultiplier = Scale;
	}
}
