// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestBurst.h"
#include "GAS/LOLGameplayTags.h"
#include "Camera/CameraShakeBase.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"

bool UGC_DeathHarvestBurst::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Location / Normal 由 UGA_DeathHarvest::ExecuteLocationCue 填好：位置 + 朝向。
	// 用 Normal 决定朝向，粒子才会贴着墙面/地面朝外喷，而不是永远世界朝前。
	const FRotator Rotation = Parameters.Normal.IsNearlyZero() ? FRotator::ZeroRotator : Parameters.Normal.Rotation();
	const FVector Location = Parameters.Location + LocationOffset;

	// 音效先取：粒子没配但音效配了，这一下也不算"没有表现"。
	UParticleSystem* FX = Particle.LoadSynchronous();
	USoundBase* SoundAsset = Sound.LoadSynchronous();

	if (FX)
	{
		UGameplayStatics::SpawnEmitterAtLocation(World, FX, FTransform(Rotation, Location));
	}
	if (SoundAsset)
	{
		UGameplayStatics::PlaySoundAtLocation(World, SoundAsset, Location);
	}

	bool bShaken = false;
	if (CameraShake)
	{
		// ★ 用 MyTarget 判"是不是施法者本机"，不是 Parameters.Instigator：
		//   cue 的 MyTarget 就是这条 cue 那个 ASC 的 avatar（AbilitySystemComponent.cpp:1507-1511
		//   InvokeGameplayCueEvent 里取 AbilityActorInfo->AvatarActor），而多播 RPC 是在
		//   【施法者的 ASC】上跑的（GameplayCueManager::FlushPendingCues → Call_InvokeGameplayCueExecuted_WithParams），
		//   所以每一台机器上 MyTarget 都是施法者本人 —— 不依赖参数里那些 weak ptr 的复制。
		APawn* CasterPawn = Cast<APawn>(MyTarget);
		if (CasterPawn && CasterPawn->IsLocallyControlled())
		{
			if (APlayerController* PC = Cast<APlayerController>(CasterPawn->GetController()))
			{
				PC->ClientStartCameraShake(CameraShake);
				bShaken = true;
			}
		}
	}

	// 三个都留空 = 这次没有表现，返回 false（不是错误，只是没东西可播）。
	// 注意这个返回值不影响任何逻辑：UGA_DeathHarvest 不看它。
	return FX != nullptr || SoundAsset != nullptr || bShaken;
}

// --- 三个子类：只有标签不同 ---
// CDO 构造阶段字符串查标签拿不到（返回 None），所以必须直接用原生标签对象。

UGC_DeathHarvestAppear::UGC_DeathHarvestAppear()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Appear;
}

UGC_DeathHarvestHit::UGC_DeathHarvestHit()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Hit;
}

UGC_DeathHarvestTeleportOut::UGC_DeathHarvestTeleportOut()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_TeleportOut;
}
