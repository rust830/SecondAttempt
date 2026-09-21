// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_ThrowDaggerHit.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"
#include "UObject/SoftObjectPath.h"

UGC_ThrowDaggerHit::UGC_ThrowDaggerHit()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_ThrowDagger_Hit;

	// 默认指向 Paragon 的 E 技能命中语音。软引用：默认值只是路径，编辑器里随时换
	// （和 UAnimNotifyState_BladeTrail::TrailSystem / UGC_EmpoweredHit::HitSystem 同一种写法）。
	HitSound = TSoftObjectPtr<USoundBase>(
		FSoftObjectPath(TEXT("/Game/ParagonKallari/Audio/Cues/Kallari_Ability_E_Engage.Kallari_Ability_E_Engage")));
}

bool UGC_ThrowDaggerHit::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// 命中类型由投射物塞进 AggregatedTargetTags；逐个查，第一个命中的生效。
	// 用 GetGameplayTagArray 而不是直接 range-for 容器，避免依赖容器的迭代器接口。
	UParticleSystem* FX = DefaultFX;
	TArray<FGameplayTag> TargetTags;
	Parameters.AggregatedTargetTags.GetGameplayTagArray(TargetTags);
	for (const FGameplayTag& Tag : TargetTags)
	{
		if (const TObjectPtr<UParticleSystem>* Found = HitFXMap.Find(Tag))
		{
			FX = Found->Get();
			break;
		}
	}

	// 音效先取：粒子没配但音效配了，这一击也不算「没有表现」。
	USoundBase* Sound = HitSound.LoadSynchronous();

	// Location / Normal 由投射物从 FHitResult 填好：命中点 + 命中面法线。
	// 用 Normal 决定朝向，粒子才会贴着墙面/地面朝外喷，而不是永远世界朝前。
	const FRotator Rotation = Parameters.Normal.IsNearlyZero()
		? FRotator::ZeroRotator
		: Parameters.Normal.Rotation();

	if (FX)
	{
		UGameplayStatics::SpawnEmitterAtLocation(World, FX, FTransform(Rotation, Parameters.Location));
	}
	if (Sound)
	{
		UGameplayStatics::PlaySoundAtLocation(World, Sound, Parameters.Location);
	}

	// 两个都留空 = 这次命中没有任何表现，和加音效之前一样返回 false（不是错误，只是没东西可播）。
	return FX != nullptr || Sound != nullptr;
}
