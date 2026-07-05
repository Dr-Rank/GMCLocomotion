#include "Components/GMCRagdollComponent.h"
#include "GMCRagdollLog.h"
#include "GMCOrganicMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Animation/AnimInstance.h"
#include "Net/UnrealNetwork.h"

UGMCRagdollComponent::UGMCRagdollComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	SetIsReplicatedByDefault(true);
}

void UGMCRagdollComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UGMCRagdollComponent, bRagdollActive);
}

void UGMCRagdollComponent::OnRep_RagdollActive()
{
	if (bRagdollActive)
	{
		EnableRagdollLocal();
	}
	else
	{
		DisableRagdollLocal();
	}
}

bool UGMCRagdollComponent::CacheOwnerReferences()
{
	if (CachedMesh && CachedMovementComp)
	{
		return true;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return false;
	}

	CachedMovementComp = Owner->FindComponentByClass<UGMC_OrganicMovementCmp>();
	if (CachedMovementComp)
	{
		CachedMesh = CachedMovementComp->GetSkeletalMeshReference();
	}

	if (!CachedMesh || !CachedMovementComp)
	{
		UE_LOG(LogGMCRagdoll, Warning, TEXT("[GMCRagdoll] CacheOwnerReferences failed on %s — Mesh=%s MovComp=%s"),
			*Owner->GetName(),
			CachedMesh ? TEXT("Y") : TEXT("N"),
			CachedMovementComp ? TEXT("Y") : TEXT("N"));
		return false;
	}

	return true;
}

void UGMCRagdollComponent::EnableRagdoll(FVector InitialBoneVelocity)
{
	if (RagdollState != EGMCRagdollState::None)
	{
		return;
	}

	if (GetOwner() && GetOwner()->HasAuthority())
	{
		// Server/Authority: set replicated bool (for late joiners) + multicast to all instances
		bRagdollActive = true;
		EnableRagdollLocal(InitialBoneVelocity);
		Multicast_EnableRagdoll();
	}
	else
	{
		// Non-authority: enable locally for immediate visual feedback
		// The server's multicast will also fire (EnableRagdollLocal guards against double-enable)
		EnableRagdollLocal(InitialBoneVelocity);
	}
}

void UGMCRagdollComponent::Multicast_EnableRagdoll_Implementation()
{
	// Skip on Authority — already enabled locally in EnableRagdoll()
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}

	EnableRagdollLocal();
}

void UGMCRagdollComponent::EnableRagdollLocal(FVector InitialBoneVelocity)
{
	if (RagdollState != EGMCRagdollState::None)
	{
		return;
	}

	if (!CacheOwnerReferences())
	{
		UE_LOG(LogGMCRagdoll, Error, TEXT("[GMCRagdoll] EnableRagdoll failed — missing mesh or movement component on %s"),
			GetOwner() ? *GetOwner()->GetName() : TEXT("NULL"));
		return;
	}

	UE_LOG(LogGMCRagdoll, Log, TEXT("[GMCRagdoll] EnableRagdoll on %s | Role=%d | BoneVelocity=%s"),
		*GetOwner()->GetName(), (int32)GetOwner()->GetLocalRole(), *InitialBoneVelocity.ToString());

	// ── Save mesh state ──
	SavedMeshCollisionProfileName = CachedMesh->GetCollisionProfileName();
	SavedMeshCollisionEnabled = CachedMesh->GetCollisionEnabled();
	SavedAnimTickOption = CachedMesh->VisibilityBasedAnimTickOption;

	// ── Save capsule state ──
	UPrimitiveComponent* CapsuleComp = Cast<UPrimitiveComponent>(CachedMovementComp->UpdatedComponent);
	if (CapsuleComp)
	{
		SavedCapsuleResponses.Empty();
		// Save the Pawn channel response specifically (we only modify this one)
		SavedCapsuleResponses.Add(ECC_Pawn, CapsuleComp->GetCollisionResponseToChannel(ECC_Pawn));
	}

	// ── Disable mesh smoothing ──
	CachedMovementComp->SetComponentToSmooth(nullptr);

	// ── Modify capsule collision ──
	if (bDisableCapsulePawnCollision && CapsuleComp)
	{
		CapsuleComp->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	// ── Shrink capsule if configured ──
	if (bShrinkCapsule)
	{
		SavedCapsuleHalfHeight = CachedMovementComp->GetRootCollisionHalfHeight(true);
		CachedMovementComp->SetRootCollisionHalfHeight(ShrunkCapsuleHalfHeight, true, false);
		bCapsuleWasShrunk = true;
	}

	// ── Enable ragdoll physics on mesh ──
	CachedMesh->SetCollisionProfileName(TEXT("Ragdoll"));
	// SetAllBodiesBelowSimulatePhysics internally sets PhysicsBlendWeight = 1.0 on each body
	CachedMesh->SetAllBodiesBelowSimulatePhysics(RagdollBoneName, true, true);
	CachedMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	// ── Apply physics body tuning (mass, damping, gravity) ──
	if (CachedMesh->Bodies.Num() > 0)
	{
		SavedBodyMassScales.Reset();
		SavedBodyLinearDamping.Reset();
		SavedBodyAngularDamping.Reset();

		for (FBodyInstance* Body : CachedMesh->Bodies)
		{
			if (!Body) continue;

			// Save original values for restore
			SavedBodyMassScales.Add(Body->MassScale);
			SavedBodyLinearDamping.Add(Body->LinearDamping);
			SavedBodyAngularDamping.Add(Body->AngularDamping);

			// Apply ragdoll overrides
			if (!FMath::IsNearlyEqual(RagdollMassScale, 1.0f))
			{
				Body->SetMassScale(Body->MassScale * RagdollMassScale);
				Body->UpdateMassProperties();
			}
			Body->LinearDamping = RagdollLinearDamping;
			Body->AngularDamping = RagdollAngularDamping;

		}
	}

	// ── Apply initial bone velocity ──
	if (!InitialBoneVelocity.IsNearlyZero())
	{
		CachedMesh->SetAllBodiesBelowLinearVelocity(RagdollBoneName, InitialBoneVelocity, true);
	}

	BlendTimer = 0.0f;

	if (BlendInDuration > SMALL_NUMBER)
	{
		// Gradual blend: override the automatic blend weight back to 0 and ramp up
		const FName SkeletonRoot = CachedMesh->GetBoneName(0);
		BlendAlpha = 0.0f;
		CachedMesh->SetAllBodiesBelowPhysicsBlendWeight(SkeletonRoot, BlendAlpha, false, true);
		RagdollState = EGMCRagdollState::BlendingIn;
	}
	else
	{
		// Instant (default): leave the automatic blend weight at 1.0 (full physics)
		BlendAlpha = 1.0f;
		RagdollState = EGMCRagdollState::Active;
	}

	SettleFrameCounter = 0;

	// Enable tick for blend/settle updates
	SetComponentTickEnabled(true);

	OnRagdollStarted.Broadcast();
}

void UGMCRagdollComponent::DisableRagdoll()
{
	if (RagdollState == EGMCRagdollState::None ||
		RagdollState == EGMCRagdollState::BlendingOut)
	{
		return;
	}

	if (GetOwner() && GetOwner()->HasAuthority())
	{
		// Server/Authority: clear replicated bool + multicast to all instances
		bRagdollActive = false;
		DisableRagdollLocal();
		Multicast_DisableRagdoll();
	}
	else
	{
		DisableRagdollLocal();
	}
}

void UGMCRagdollComponent::Multicast_DisableRagdoll_Implementation()
{
	// Skip on Authority — already disabled locally in DisableRagdoll()
	if (GetOwner() && GetOwner()->HasAuthority())
	{
		return;
	}

	DisableRagdollLocal();
}

void UGMCRagdollComponent::DisableRagdollLocal()
{
	if (RagdollState == EGMCRagdollState::None ||
		RagdollState == EGMCRagdollState::BlendingOut)
	{
		return;
	}

	if (!CachedMesh)
	{
		return;
	}

	// If settled, physics simulation was stopped — re-enable for blend-out
	if (RagdollState == EGMCRagdollState::Settled)
	{
		CachedMesh->SetAllBodiesBelowSimulatePhysics(RagdollBoneName, true, true);
	}

	// Capture ragdoll bone velocity before stopping physics
	RagdollRecoveryVelocity = CachedMesh->GetBoneLinearVelocity(RagdollBoneName);

	// Begin blend-out
	RagdollState = EGMCRagdollState::BlendingOut;
	BlendTimer = 0.0f;

	// Re-enable tick (may have been disabled on settle)
	SetComponentTickEnabled(true);
}

void UGMCRagdollComponent::HandleRevived()
{
	if (RagdollState == EGMCRagdollState::None)
	{
		return;
	}

	// Capture face-up state before blend-out moves bones
	const bool bFaceUp = IsRagdollFaceUp();

	// Save pose snapshot so the AnimGraph can blend from the ragdoll pose
	// into the get-up animation (use a Pose Snapshot node with this name)
	if (CachedMesh)
	{
		if (UAnimInstance* AnimInstance = CachedMesh->GetAnimInstance())
		{
			AnimInstance->SavePoseSnapshot(RagdollPoseSnapshotName);
		}
	}

	// Begin ragdoll blend-out
	DisableRagdoll();

	// Signal revive with face-up info for get-up animation selection
	OnRevived.Broadcast(bFaceUp);
}

bool UGMCRagdollComponent::IsRagdollFaceUp() const
{
	if (RagdollState == EGMCRagdollState::None || !CachedMesh)
	{
		return true;
	}

	const FTransform BoneTransform = CachedMesh->GetSocketTransform(RagdollBoneName, RTS_World);
	const FVector PelvisUp = BoneTransform.GetUnitAxis(EAxis::Z);
	return FVector::DotProduct(PelvisUp, FVector::UpVector) > 0.0f;
}

void UGMCRagdollComponent::ApplyCapsuleImpulse(FVector Impulse)
{
	if (!CacheOwnerReferences())
	{
		return;
	}

	// Add velocity to the GMC movement component so the capsule coasts
	CachedMovementComp->SetLinearVelocity_GMC(
		CachedMovementComp->GetLinearVelocity_GMC() + Impulse
	);
}

void UGMCRagdollComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (RagdollState == EGMCRagdollState::None)
	{
		SetComponentTickEnabled(false);
		return;
	}

	UpdateRagdollBlend(DeltaTime);
}

void UGMCRagdollComponent::UpdateRagdollBlend(float DeltaTime)
{
	if (!CachedMesh)
	{
		return;
	}

	BlendTimer += DeltaTime;

	// Use skeleton root bone (index 0) so blend weight applies to ALL bodies
	const FName BlendRoot = CachedMesh->GetBoneName(0);

	switch (RagdollState)
	{
	case EGMCRagdollState::BlendingIn:
	{
		if (BlendInDuration > SMALL_NUMBER)
		{
			BlendAlpha = FMath::Clamp(BlendTimer / BlendInDuration, 0.0f, 1.0f);
		}
		else
		{
			BlendAlpha = 1.0f;
		}

		CachedMesh->SetAllBodiesBelowPhysicsBlendWeight(BlendRoot, BlendAlpha, false, true);

		KeepMeshOnCapsule();

		if (BlendAlpha >= 1.0f)
		{
			RagdollState = EGMCRagdollState::Active;
			SettleFrameCounter = 0;
		}
		break;
	}

	case EGMCRagdollState::Active:
	{
		KeepMeshOnCapsule();
		CheckRagdollSettled();
		break;
	}

	case EGMCRagdollState::Settled:
	{
		// Stay in settled state until DisableRagdoll() is called externally
		break;
	}

	case EGMCRagdollState::BlendingOut:
	{
		if (BlendOutDuration > SMALL_NUMBER)
		{
			BlendAlpha = FMath::Clamp(1.0f - (BlendTimer / BlendOutDuration), 0.0f, 1.0f);
		}
		else
		{
			BlendAlpha = 0.0f;
		}

		CachedMesh->SetAllBodiesBelowPhysicsBlendWeight(BlendRoot, BlendAlpha, false, true);

		if (BlendAlpha <= 0.0f)
		{
			FinishBlendOut();
		}
		break;
	}

	default:
		break;
	}
}

void UGMCRagdollComponent::CheckRagdollSettled()
{
	if (!CachedMesh)
	{
		return;
	}

	const FVector BoneVelocity = CachedMesh->GetBoneLinearVelocity(RagdollBoneName);

	if (BoneVelocity.Size() < SettleVelocityThreshold)
	{
		SettleFrameCounter++;
		if (SettleFrameCounter >= SettleFrameCount)
		{
			RagdollState = EGMCRagdollState::Settled;

			// Stop physics simulation entirely to prevent micro-twitches.
			// PutAllRigidBodiesToSleep alone isn't enough — UE physics can
			// wake sleeping bodies from gravity or numerical instability.
			CachedMesh->SetAllBodiesBelowSimulatePhysics(RagdollBoneName, false, true);

			// Disable tick — nothing to update while settled
			SetComponentTickEnabled(false);

			OnRagdollSettled.Broadcast();
		}
	}
	else
	{
		SettleFrameCounter = 0;
	}
}

void UGMCRagdollComponent::KeepMeshOnCapsule()
{
	if (!CachedMesh || !CachedMovementComp)
	{
		return;
	}

	UPrimitiveComponent* CapsuleComp = Cast<UPrimitiveComponent>(CachedMovementComp->UpdatedComponent);
	if (!CapsuleComp)
	{
		return;
	}

	const FVector PelvisLocation = CachedMesh->GetSocketLocation(RagdollBoneName);

	// Capsule base = capsule center minus half-height
	FVector CapsuleBase = CapsuleComp->GetComponentLocation();
	CapsuleBase.Z -= CachedMovementComp->GetRootCollisionHalfHeight(true);

	// Direction from pelvis toward capsule base
	FVector Direction = (CapsuleBase - PelvisLocation).GetSafeNormal();

	// Apply tether impulse — halved on Z to prevent excessive vertical bouncing
	FVector Impulse = Direction * CapsuleTetherForce;
	Impulse.Z *= 0.5f;

	CachedMesh->AddImpulse(Impulse, RagdollBoneName, false);
}

void UGMCRagdollComponent::FinishBlendOut()
{
	if (!CachedMesh)
	{
		RagdollState = EGMCRagdollState::None;
		SetComponentTickEnabled(false);
		return;
	}

	// ── Disable physics simulation ──
	CachedMesh->SetAllBodiesSimulatePhysics(false);
	CachedMesh->ResetAllBodiesSimulatePhysics();

	// ── Restore physics body properties ──
	if (SavedBodyMassScales.Num() > 0)
	{
		const int32 NumBodies = FMath::Min(CachedMesh->Bodies.Num(), SavedBodyMassScales.Num());
		for (int32 i = 0; i < NumBodies; ++i)
		{
			FBodyInstance* Body = CachedMesh->Bodies[i];
			if (!Body) continue;

			Body->SetMassScale(SavedBodyMassScales[i]);
			Body->UpdateMassProperties();
			Body->LinearDamping = SavedBodyLinearDamping[i];
			Body->AngularDamping = SavedBodyAngularDamping[i];
		}
		SavedBodyMassScales.Reset();
		SavedBodyLinearDamping.Reset();
		SavedBodyAngularDamping.Reset();
	}

	// ── Restore mesh collision ──
	if (SavedMeshCollisionProfileName != NAME_None)
	{
		CachedMesh->SetCollisionProfileName(SavedMeshCollisionProfileName);
	}
	CachedMesh->SetCollisionEnabled(SavedMeshCollisionEnabled);

	// ── Restore anim tick option ──
	CachedMesh->VisibilityBasedAnimTickOption = SavedAnimTickOption;

	// ── Restore capsule collision ──
	if (CachedMovementComp)
	{
		UPrimitiveComponent* CapsuleComp = Cast<UPrimitiveComponent>(CachedMovementComp->UpdatedComponent);
		if (CapsuleComp)
		{
			for (const auto& Pair : SavedCapsuleResponses)
			{
				CapsuleComp->SetCollisionResponseToChannel(Pair.Key, Pair.Value);
			}
		}

		// ── Restore capsule height ──
		if (bCapsuleWasShrunk)
		{
			CachedMovementComp->SetRootCollisionHalfHeight(SavedCapsuleHalfHeight, true, false);
			bCapsuleWasShrunk = false;
		}

		// ── Re-enable mesh smoothing for roles that need it ──
		if (CachedMovementComp->IsSimulatedProxy() || CachedMovementComp->IsSmoothedListenServerPawn())
		{
			CachedMovementComp->SetComponentToSmooth(CachedMovementComp->GetSkeletalMeshReference());
		}
	}

	// ── Transition to None ──
	RagdollState = EGMCRagdollState::None;
	BlendAlpha = 0.0f;

	SetComponentTickEnabled(false);

	OnRagdollEnded.Broadcast();
}
