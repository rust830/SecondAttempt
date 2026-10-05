// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_AbilityCost.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/HeroCombatAttributeSet.h"

UGE_AbilityCost::UGE_AbilityCost()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_Cost;

	FGameplayModifierInfo Modifier;
	Modifier.Attribute = UHeroCombatAttributeSet::GetEnergyAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;
	// 施加方填负值。默认值不在这里写 —— SetByCaller 没有「默认值」字段，
	// 谁都不填就是 0（= 不扣蓝），和「消耗没接上」的表现一致，不会静默抽干能量。
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);
	Modifiers.Add(Modifier);
}
