// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_Damage.h"
#include "GAS/ExecCalc_Damage.h"

UGE_Damage::UGE_Damage(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	// 唯一的作用：把伤害交给 UExecCalc_Damage 结算。
	// 这里【不】配任何 Modifier —— 见头文件的说明。
	FGameplayEffectExecutionDefinition Execution;
	Execution.CalculationClass = UExecCalc_Damage::StaticClass();
	Executions.Add(Execution);
}
