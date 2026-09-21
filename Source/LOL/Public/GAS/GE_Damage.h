// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Damage.generated.h"

/**
 * 所有伤害的唯一载体。
 *
 * Instant、**一个 Modifier 都不配**，只挂 UExecCalc_Damage。伤害不是一条固定的 -X 曲线：
 * 它要经过「攻击者攻击力 × 倍率 + 固定值 → 抗性 → 格挡」这一串计算，只有 ExecCalc 拿得到全部输入。
 *
 * 用法（近战/匕首/以后第 N 个技能都一样）：
 *   Spec.Data->SetSetByCallerMagnitude(Data.DamageMultiplier, 倍率);
 *   Spec.Data->SetSetByCallerMagnitude(Data.FlatDamage, 固定值);
 *   Spec.Data->AddDynamicAssetTag(Damage.Physical);   // 不挂 = 物理
 *   ApplyGameplayEffectSpecToTarget(...)
 *
 * 【不要】给这个 GE 建蓝图子类再加 Modifier —— 那等于绕过唯一的结算点，
 * 格挡判定会漏掉那部分伤害，而且是静默漏。
 */
UCLASS()
class LOL_API UGE_Damage : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Damage(const FObjectInitializer& ObjectInitializer);
};
