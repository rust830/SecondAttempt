// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayTagContainer.h"
#include "GC_ThrowDaggerHit.generated.h"

class UParticleSystem;
class USoundBase;

/**
 * 匕首命中表现：按命中目标类型播不同 burst。
 *
 * 命中类型（Target.Hero / Target.Void / Target.Terrain）不可能塞进 cue 标签本身——
 * GameplayCueSet 只按弹射出的那个标签查表，不会给子标签各跑一份 cue。
 * 所以这里统一用 GameplayCue.ThrowDagger.Hit 一个 cue，类型走 Parameters.AggregatedTargetTags。
 *
 * 这个 cue 走服务端 ExecuteGameplayCue → 多播到各客户端，修掉了原先「命中特效只在服务端播」的问题。
 *
 * 蓝图子类必须命名为 GC_ThrowDagger_Hit。
 */
UCLASS()
class LOL_API UGC_ThrowDaggerHit : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_ThrowDaggerHit();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	/** 命中类型 → 粒子。键用 Target.Hero / Target.Void / Target.Terrain。 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit")
	TMap<FGameplayTag, TObjectPtr<UParticleSystem>> HitFXMap;

	/** HitFXMap 没匹配上时的兜底粒子（可空：留空则该命中不放特效）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit")
	TObjectPtr<UParticleSystem> DefaultFX;

};
