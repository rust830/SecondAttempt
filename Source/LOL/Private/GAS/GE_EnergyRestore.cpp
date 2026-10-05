// GE_EnergyRestore 的实现：Instant + 一条 Energy Additive 修正符，幅度走 SetByCaller（Data.EnergyRestore）。

#include "GAS/GE_EnergyRestore.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"

UGE_EnergyRestore::UGE_EnergyRestore(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayModifierInfo Modifier;
	Modifier.Attribute = UHeroCombatAttributeSet::GetEnergyAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;

	FSetByCallerFloat Magnitude;
	Magnitude.DataTag = LOLGameplayTags::Data_EnergyRestore;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Magnitude);

	Modifiers.Add(Modifier);

	// 上限不用管：UHeroCombatAttributeSet::ClampAttribute 已经把 Energy 钳在 [0, MaxEnergy]，
	// 两条钳制路径（PreAttributeChange / PreAttributeBaseChange）都走它（和 GE_EnergyRegen 一致）。
}
