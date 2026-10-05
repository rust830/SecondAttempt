// GE_Heal 的实现：Instant + 一条 Health Additive 修正符，幅度走 SetByCaller（Data.Heal）。

#include "GAS/GE_Heal.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"

UGE_Heal::UGE_Heal(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	// 瞬时加血。正负都合法（负值 = 掉血），但现在的施加点只会填正数。
	FGameplayModifierInfo Modifier;
	Modifier.Attribute = UHeroCombatAttributeSet::GetHealthAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;

	FSetByCallerFloat Magnitude;
	Magnitude.DataTag = LOLGameplayTags::Data_Heal;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Magnitude);

	Modifiers.Add(Modifier);
}
