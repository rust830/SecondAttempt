// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_Flash.h"
#include "GAS/LOLGameplayTags.h"
#include "Engine/World.h"
#include "GameplayEffectTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"

UGC_Flash::UGC_Flash()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_Flash;
}

bool UGC_Flash::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// 闪现是「落点」特效，不是起手点：位置由 GA_Flash 填进 Location。
	const FVector Location = Parameters.Location;

	if (FlashParticle)
	{
		// Cascade：UParticleSystem + SpawnEmitterAtLocation（和 GC_ThrowDaggerHit 同一套写法）。
		UGameplayStatics::SpawnEmitterAtLocation(World, FlashParticle, FTransform(FRotator::ZeroRotator, Location));
	}
	if (FlashSound)
	{
		UGameplayStatics::PlaySoundAtLocation(World, FlashSound, Location);
	}

	return true;
}
