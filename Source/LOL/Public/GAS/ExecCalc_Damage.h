// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "ExecCalc_Damage.generated.h"

/**
 * ★ 全项目唯一的伤害结算点。
 *
 * 顺序：① 原始伤害 = 攻击者 AttackDamage × Data.DamageMultiplier + Data.FlatDamage
 *       ② 抗性减免（按 Spec 上的 Damage.* 动态资产标签挑护甲/魔抗，先百分比穿透再固定穿透）
 *       ③ 格挡 / 免疫判定（UBlockComponent::TryMitigateIncomingDamage，唯一的减免入口）
 *       ④ 写成目标 Health 的负向 modifier
 *
 * 纪律：伤害公式只存在于这一个文件里。任何「我在旁边自己算一下伤害」的写法都是在绕过格挡判定，
 * 而且是静默绕过（没有日志、没有断言，只是那部分伤害格挡拦不住）。
 */
UCLASS()
class LOL_API UExecCalc_Damage : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()
public:
	UExecCalc_Damage();

	// 注意是 Execute_Implementation（蓝图原生事件的实现），不是 Execute —— 覆写 Execute 会编译不过。
	virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
		FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};
