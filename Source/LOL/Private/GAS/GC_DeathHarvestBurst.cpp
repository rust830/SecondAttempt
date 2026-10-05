// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestBurst.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/CueCameraShake.h"
#include "GAS/LOLGameplayTags.h"
#include "Camera/CameraShakeBase.h"
#include "Engine/World.h"
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

	// 音效走事件表（Audio.DeathHarvest.Burst），这一类的表现不往 GC 上塞音效属性 ——
	// 见 UHeroAudioConfig 的注释。
	UParticleSystem* FX = Particle.LoadSynchronous();

	if (FX)
	{
		UGameplayStatics::SpawnEmitterAtLocation(World, FX, FTransform(Rotation, Location));
	}
	UHeroAudioLibrary::PlayAt(World, LOLGameplayTags::Audio_DeathHarvestBurst, Location);

	// ★ 用 MyTarget 判"是不是施法者本机"，不是 Parameters.Instigator —— 理由见
	//   CueCameraShake.h。死亡收割的三个 burst（Appear / Hit / TeleportOut）都用这一份实现。
	const bool bShaken = HeroCueCameraShake::PlayLocalHitShake(MyTarget, CameraShake);

	// 三个都留空 = 这次没有表现，返回 false（不是错误，只是没东西可播）。
	// 注意这个返回值不影响任何逻辑：UGA_DeathHarvest 不看它。
	return FX != nullptr || bShaken;
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
