// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GC_Flash.generated.h"

class UParticleSystem;
class USoundBase;

/**
 * 闪现表现：一次性的落点粒子 + 音效。
 *
 * 用 Static（不是 Actor）变体：它没有生命周期、没有 Tick、不占 actor，正适合「放一个 burst 就完事」。
 * 落点不是起手点，由 GA_Flash 通过 Parameters.Location 传进来。
 *
 * 蓝图子类必须命名为 GC_Flash。
 */
UCLASS()
class LOL_API UGC_Flash : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_Flash();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	/** 闪现粒子（Cascade，可空）。本项目默认用 Cascade。 */
	UPROPERTY(EditDefaultsOnly, Category = "Flash")
	TObjectPtr<UParticleSystem> FlashParticle;

	/** 闪现音效（可空）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Flash")
	TObjectPtr<USoundBase> FlashSound;
};
