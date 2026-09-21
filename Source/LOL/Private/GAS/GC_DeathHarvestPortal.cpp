// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestPortal.h"
#include "GAS/LOLGameplayTags.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"

AGC_DeathHarvestPortal::AGC_DeathHarvestPortal()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Portal;

	// P3 现身时服务器 K2_RemoveGameplayCue → 本 actor 销毁（技能被打断则由
	// Super::EndAbility 按 bRemoveOnAbilityEnd 兜底摘掉）。关门的表现已经在这一刻用
	// 独立发射器放出去了（见 OnRemove），所以这里可以立刻销毁。
	bAutoDestroyOnRemove = true;

	// 门开在世界坐标上，不跟着人走。
	bAutoAttachToOwner = false;
}

bool AGC_DeathHarvestPortal::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return false;
	}

	// 先清旧的（cue actor 会被回收复用，OnActive 可能不是第一次）。
	DestroyLoop();

	// ★ 这一行是这个类存在的理由：把门挪到服务器算出来的落点。
	//   参数由 UGA_DeathHarvest::AddLocationCue 填（Location = 落点，Normal = 落点到目标的朝向）。
	SetActorLocation(Parameters.Location + RelativeOffset);
	if (!Parameters.Normal.IsNearlyZero())
	{
		SetActorRotation(Parameters.Normal.Rotation());
	}

	if (OpenParticle)
	{
		LoopComp = UGameplayStatics::SpawnEmitterAttached(
			OpenParticle, GetRootComponent(), NAME_None, FVector::ZeroVector, FRotator::ZeroRotator, FVector(1.f),
			EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
	}

	if (OpenSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, OpenSound, Parameters.Location);
	}

	return LoopComp != nullptr || OpenSound != nullptr;
}

bool AGC_DeathHarvestPortal::WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// 服务器那一端走的是这条（运行时 Add 的 cue）。转给 OnActive，理由见头文件。
	return OnActive_Implementation(MyTarget, Parameters);
}

bool AGC_DeathHarvestPortal::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	DestroyLoop();

	// 关门的一次性粒子。用独立发射器（不是挂在自己身上的组件）：
	// 本 actor 马上就要销毁了，挂在它身上的粒子会跟着一起没。
	if (CloseParticle)
	{
		UGameplayStatics::SpawnEmitterAtLocation(GetWorld(), CloseParticle, GetActorTransform());
	}
	if (CloseSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, CloseSound, GetActorLocation());
	}

	return true;
}

void AGC_DeathHarvestPortal::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyLoop();
	Super::EndPlay(EndPlayReason);
}

void AGC_DeathHarvestPortal::DestroyLoop()
{
	if (LoopComp)
	{
		LoopComp->Deactivate();
		LoopComp->bAutoDestroy = true;
		LoopComp->DestroyComponent();
		LoopComp = nullptr;
	}
}
