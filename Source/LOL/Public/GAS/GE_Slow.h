// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Slow.generated.h"

/**
 * 减速：持续型 GE，时长和速度倍率都由施加方用 SetByCaller 填。
 *
 *   Data.SlowDuration    —— 持续几秒
 *   Data.SlowMultiplier  —— MoveSpeed 乘多少（0.7 = 减速 30%）
 *
 * 【改的是属性，不是 CharacterMovement】它只动 MoveSpeed 属性。属性到
 * CharacterMovement->MaxWalkSpeed 的那段桥在 AHeroCombatCharacter 里
 * （InitializeAbilityActorInfo 绑 MoveSpeed 的变化回调）。这样减速、以及以后任何
 * 改移速的东西（装备 / buff / 升级）都走同一条路，不用各改各的 ——
 * 接入之前 MoveSpeed 属性一直是「只是数据，没接 MaxWalkSpeed」的状态（见属性集注释）。
 *
 * 【为什么是乘算】LoL 的多个减速是叠乘的（0.7 × 0.8 = 0.56）。加算还得先知道
 * 「基础速度是多少」才能表达一个百分比，等于多养一份真相。
 *
 * ⚠️ 修正符用的是 MultiplyCompound，不是 Multiplicitive —— 这两个在 UE5 里
 * 名字像但语义完全不同：Multiplicitive 是 MultiplyAdditive 的向后兼容别名，
 * 意思是「倍率先相加再乘」（0.7 + 0.7 = 1.4），用它会直接【把减速变成加速】。
 * 详见 GameplayEffectTypes.h:112 的枚举注释。
 *
 * 授予 State.Slowed 是给动画层用的影子：UHeroAnimationSet 读它换移动混合空间。
 * 见 LOLGameplayTags.h 里「减速是属性不是标签」那段。
 */
UCLASS()
class LOL_API UGE_Slow : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Slow(const FObjectInitializer& ObjectInitializer);
};
