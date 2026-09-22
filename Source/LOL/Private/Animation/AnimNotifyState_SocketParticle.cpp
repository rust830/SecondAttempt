// Fill out your copyright notice in the Description page of Project Settings.

#include "Animation/AnimNotifyState_SocketParticle.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "TimerManager.h"

namespace
{
	/**
	 * 插槽和骨骼都算数：GetSocketLocation 对骨骼名同样有效。
	 *
	 * ⚠ 名字里的文件前缀是必须的，别改回 HasSocket：unity build 会把多个 .cpp 合进同一个
	 * Module.LOL.N.cpp，匿名 namespace 只挡【跨 TU】冲突，合进一个 TU 之后同签名的函数
	 * 就是重定义（C2084）。同样的实现还有 AnimNotifyState_BladeTrail / GA_ThreeHitPassive 两份。
	 */
	bool SocketParticleHasSocket(const USkeletalMeshComponent* MeshComp, const FName& SocketName)
	{
		return MeshComp->DoesSocketExist(SocketName) || MeshComp->GetBoneIndex(SocketName) != INDEX_NONE;
	}
}

void UAnimNotifyState_SocketParticle::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	if (!MeshComp)
	{
		return;
	}

	// 角色在 notify 期间被销毁时 NotifyEnd 不一定跑得到，顺手清掉失效的键（弱引用，不会解野指针）。
	for (auto It = ActiveParticles.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// 专用服务器没有渲染，不生成。
	UWorld* World = MeshComp->GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	UParticleSystem* System = Particle.LoadSynchronous();
	if (!System)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SocketParticle] Particle 没配（或软引用加载失败）→ %s 上不生成"),
			*GetNameSafe(MeshComp->GetSkeletalMeshAsset()));
		return;
	}

	FActiveSocketParticles& Entry = ActiveParticles.Add(MeshComp);
	if (Side == ESocketParticleSide::Left || Side == ESocketParticleSide::Both)
	{
		if (UParticleSystemComponent* Comp = SpawnOne(MeshComp, System, /*bRight=*/false))
		{
			Entry.Comps.Add(Comp);
		}
	}
	if (Side == ESocketParticleSide::Right || Side == ESocketParticleSide::Both)
	{
		if (UParticleSystemComponent* Comp = SpawnOne(MeshComp, System, /*bRight=*/true))
		{
			Entry.Comps.Add(Comp);
		}
	}

	if (Entry.Comps.Num() == 0)
	{
		ActiveParticles.Remove(MeshComp);
	}
}

UParticleSystemComponent* UAnimNotifyState_SocketParticle::SpawnOne(USkeletalMeshComponent* MeshComp, UParticleSystem* System, bool bRight) const
{
	const FName SocketName = bRight ? SocketRight : SocketLeft;
	if (!SocketParticleHasSocket(MeshComp, SocketName))
	{
		// 插槽不存在时 SpawnEmitterAttached 会静默把粒子挂在组件原点 —— 看不出是配置错的，必须当场报出来。
		UE_LOG(LogTemp, Warning, TEXT("[SocketParticle] %s 上找不到插槽 %s → 这一侧不生成"),
			*GetNameSafe(MeshComp->GetSkeletalMeshAsset()), *SocketName.ToString());
		return nullptr;
	}

	const FRotator Rotation = bRight ? RelativeRotationRight : RelativeRotationLeft;

	// SnapToTarget：Location/Rotation 落成【相对插槽】的变换（跟着刀走，不跟着角色转身）。
	// bAutoDestroy=false：生命周期由 NotifyEnd 的 Teardown 统一管，不给引擎留一个"自己决定什么时候死"的分支。
	UParticleSystemComponent* Comp = UGameplayStatics::SpawnEmitterAttached(
		System, MeshComp, SocketName, RelativeOffset, Rotation, FVector(1.f),
		EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false, EPSCPoolMethod::None, /*bAutoActivate=*/true);

	if (!Comp)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SocketParticle] SpawnEmitterAttached 返回空（系统无效，或被可扩展性预剔除挡掉）"));
		return nullptr;
	}

	// ★ 转圈时是 false：人已经现身了，敌人该看到刀上的光。
	// （SCS_Stealth 的 SwordParticle 是 true —— 那一段抄过来时最容易抄错这个值。）
	Comp->bOnlyOwnerSee = bOnlyOwnerSee;

	return Comp;
}

void UAnimNotifyState_SocketParticle::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (!MeshComp)
	{
		return;
	}

	FActiveSocketParticles Entry;
	if (!ActiveParticles.RemoveAndCopyValue(MeshComp, Entry))
	{
		return;
	}

	for (UParticleSystemComponent* Comp : Entry.Comps)
	{
		Teardown(Comp, TeardownDelay);
	}
}

void UAnimNotifyState_SocketParticle::Teardown(UParticleSystemComponent* Component, float DelaySeconds)
{
	if (!Component)
	{
		return;
	}

	// 先停发射：已经生成的那部分按粒子自己的寿命自然收掉，比直接 Destroy 好看。
	Component->Deactivate();
	Component->bAutoDestroy = true;

	// 兜底：emitter 要是设成无限循环，auto destroy 永远等不到 —— 每转一次泄漏一个常驻组件。
	if (UWorld* World = Component->GetWorld())
	{
		TWeakObjectPtr<UParticleSystemComponent> WeakComponent(Component);
		FTimerHandle TeardownTimer;
		World->GetTimerManager().SetTimer(TeardownTimer, FTimerDelegate::CreateWeakLambda(Component, [WeakComponent]()
		{
			if (UParticleSystemComponent* StillAlive = WeakComponent.Get())
			{
				StillAlive->DestroyComponent();
			}
		}), DelaySeconds, /*bLoop=*/false);
	}
}

FString UAnimNotifyState_SocketParticle::GetNotifyName_Implementation() const
{
	switch (Side)
	{
	case ESocketParticleSide::Left:  return TEXT("SocketParticle (左手)");
	case ESocketParticleSide::Both:  return TEXT("SocketParticle (双手)");
	default:                         return TEXT("SocketParticle (右手)");
	}
}
