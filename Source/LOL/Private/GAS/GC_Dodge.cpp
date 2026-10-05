// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_Dodge.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/LOLGameplayTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameplayEffectTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"

UGC_Dodge::UGC_Dodge()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_Dodge;

	// 默认指向 Kallari 的背喷尾迹 burst（jet deploy）。软引用：默认值只是路径，编辑器里随时换。
	JetParticle = TSoftObjectPtr<UParticleSystem>(
		FSoftObjectPath(TEXT("/Game/ParagonKallari/FX/Particles/Kallari/Abilities/Dodge/FX/P_BackJets_Trail_Burst.P_BackJets_Trail_Burst")));
}

bool UGC_Dodge::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// 喷射粒子挂在角色 mesh 的喷射插槽上跟着人走（背喷 booster）；没有 mesh（非角色）就退回世界位置。
	UParticleSystem* FX = JetParticle.LoadSynchronous();
	if (FX)
	{
		if (const ACharacter* Character = Cast<ACharacter>(MyTarget))
		{
			if (Character->GetMesh())
			{
				// bAutoDestroy=true：一次性 burst，播完自毁。挂在 JetSocketName（默认 FX_Thruster_Center）上。
				UGameplayStatics::SpawnEmitterAttached(FX, Character->GetMesh(), JetSocketName,
					FVector::ZeroVector, FRotator::ZeroRotator, FVector(1.f),
					EAttachLocation::SnapToTarget, /*bAutoDestroy=*/true);
			}
			else
			{
				UGameplayStatics::SpawnEmitterAtLocation(World, FX, FTransform(FRotator::ZeroRotator, MyTarget->GetActorLocation()));
			}
		}
		else
		{
			UGameplayStatics::SpawnEmitterAtLocation(World, FX, FTransform(FRotator::ZeroRotator, Parameters.Location));
		}
	}

	// 音效从事件表里取（Audio.Dodge），这一类身上不再留音效属性。
	UHeroAudioLibrary::PlayAt(World, LOLGameplayTags::Audio_Dodge, Parameters.Location);

	return true;
}
