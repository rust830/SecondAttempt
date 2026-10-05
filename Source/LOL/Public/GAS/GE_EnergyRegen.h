// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_EnergyRegen.generated.h"

/**
 * 能量回复：Infinite GE，周期性回 Energy。
 *
 * 【为什么要有它】接上消耗之后能量只出不进 —— 没有回复的话放几个技能就永久哑火，
 * 「接消耗」这件事本身就是残缺的。Kallari 在 LoL 里是能量型英雄，能量本来就自动回。
 *
 * 【数值口径】EnergyRegen 是「每 5 秒回复量」（和 HealthRegen 同口径，见属性集.txt），
 * 所以每秒回复 = EnergyRegen / 5。GE 的周期是 0.5s ⇒ 每个周期回 EnergyRegen × 0.5/5 = ×0.1。
 * 系数直接写死在 Coefficient 里，改周期就要连带改这个 0.1 —— 两处绑死，别只改一处。
 *
 * 【为什么用 AttributeBased 而不是 SetByCaller】回复量跟着 EnergyRegen 属性走，
 * 以后出「回蓝装」加的也是那条属性，GE 不用动。SetByCaller 则要每个施加点自己算。
 *
 * 【谁来挂】UAbilitySet 上的 Effect 数组（冠军套件资产里加一条即可），不用改角色类。
 * Infinite 且没有标签 ⇒ 不挡任何东西，也不用摘。
 */
UCLASS()
class LOL_API UGE_EnergyRegen : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_EnergyRegen();
};
