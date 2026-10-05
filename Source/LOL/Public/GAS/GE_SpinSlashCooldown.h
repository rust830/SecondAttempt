// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_SpinSlashCooldown.generated.h"

/**
 * SpinSlash（上挑击飞）冷却：持续型 GE，时长由 Data.Cooldown(SetByCaller) 决定，
 * 期间持有 State.Cooldown.SpinSlash。
 *
 * 【为什么每个技能一个自己的冷却 GE，不共用一个通用 GE】CheckCooldown 是【按标签】判的 ——
 * 它读 CooldownGameplayEffectClass 的 Granted Tags（GetCooldownTags）。两个技能共用一个
 * GE 就等于共用一个冷却标签 ⇒ 玩家同时拿到这两个海克斯时，放其中任意一个会把另一个
 * 也一起锁住。LoL 里每个技能有自己的 CD，这里就照做。
 */
UCLASS()
class LOL_API UGE_SpinSlashCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_SpinSlashCooldown(const FObjectInitializer& ObjectInitializer);
};
