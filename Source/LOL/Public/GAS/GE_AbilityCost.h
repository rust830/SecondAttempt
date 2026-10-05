// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_AbilityCost.generated.h"

/**
 * 技能消耗：Instant GE，扣 Energy，数值由 SetByCaller Data.Cost 决定。
 *
 * 【为什么走 GE 而不是直接 SetNumericAttributeBase】
 * 消耗要能被「减耗/免费施法」这类 GE 改到，直接写属性就绕开了整条 modifier 聚合链，
 * 而且 Instant 的聚合结果不会进属性集的复制路径（客户端看到的能量会和服务端不一致）。
 * 走 GE 的话 Modifier 还能被 AttributeBased 替换（以后做「按最大能量百分比计费」只改这一个 GE）。
 *
 * 【谁填 Data.Cost】只有 UMyGameplayAbility::ApplyCost，统一取 -ManaCost（负号在那一处写）。
 * 别的施加点不要自己填 —— 符号写反的消耗是「放技能回蓝」，而且只在配了正值的技能上暴露。
 *
 * ⚠️ 施加方是【自己】：ApplyGameplayEffectSpecToOwner。消耗不进目标 ASC。
 */
UCLASS()
class LOL_API UGE_AbilityCost : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_AbilityCost();
};
