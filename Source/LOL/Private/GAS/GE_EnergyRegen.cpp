// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_EnergyRegen.h"
#include "GAS/HeroCombatAttributeSet.h"

UGE_EnergyRegen::UGE_EnergyRegen()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;

	// 0.5s 一跳：比 1s 平滑（能量条不会一格一格跳），又不至于每帧跑 modifier 聚合。
	Period = 0.5f;

	// 快照=false：每周期现读 EnergyRegen。快照=true 会在挂上那一刻锁死数值，
	// 之后出了回蓝装也不生效（而且快照只在 GE 被施加时抓一次，属性集那时候可能还没 ApplyStats）。
	FAttributeBasedFloat AttributeBased;
	AttributeBased.BackingAttribute = FGameplayEffectAttributeCaptureDefinition(
		UHeroCombatAttributeSet::GetEnergyRegenAttribute(),
		EGameplayEffectAttributeCaptureSource::Source,
		/*bSnapshot=*/false);
	// 引擎公式：(Coefficient × (PreMultiplyAdditiveValue + 属性值)) + PostMultiplyAdditiveValue
	// Pre / Post 保持默认 0 ⇒ 每周期回 EnergyRegen × 0.1。
	// ⚠️ 这三个字段是 FScalableFloat 不是裸 float（能隐式转，但显式写出来免得以后加曲线表时看错类型）。
	AttributeBased.Coefficient = FScalableFloat(0.1f);  // = Period / 5：EnergyRegen 是「每 5 秒」口径

	FGameplayModifierInfo Modifier;
	Modifier.Attribute = UHeroCombatAttributeSet::GetEnergyAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(AttributeBased);
	Modifiers.Add(Modifier);

	// 上限不用管：UHeroCombatAttributeSet::ClampAttribute 已经把 Energy 钳在 [0, MaxEnergy]，
	// 两条钳制路径（PreAttributeChange / PreAttributeBaseChange）都走它。
}
