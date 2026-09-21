// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_EmpoweredAttack.generated.h"

/**
 * 强化普攻状态：持续型 GE，时长由 Data.EmpowerDuration(SetByCaller) 决定，期间授予 State.EmpoweredAttack。
 *
 * 只承载「状态」和「时长」，不带任何数值——伤害倍率/击退力度统一在 UThreeHitPassiveData 上，
 * 因为唯一的消费者是普攻结算方 GA_ThreeHitPassive。施加方（GA_Stealth 破隐、GA_ThreeHitPassive 完美窗口）
 * 各自决定窗口时长，数值不用重复配。
 *
 * 表现（强化 Montage + 音效）不在这个类里，挂在 GameplayCue.EmpoweredAttack → AGC_EmpoweredAttack。
 */
UCLASS()
class LOL_API UGE_EmpoweredAttack : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_EmpoweredAttack(const FObjectInitializer& ObjectInitializer);
};
