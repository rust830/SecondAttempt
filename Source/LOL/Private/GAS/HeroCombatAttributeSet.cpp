// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/HeroCombatAttributeSet.h"
#include "Net/UnrealNetwork.h"

UHeroCombatAttributeSet::UHeroCombatAttributeSet()
{
	InitAttackDamage(60.f);
	InitBaseAttackSpeed(.658f);
	InitAttackSpeedRatio(.658f);
	InitBonusAttackSpeedPercent(0.f);
	// Attribute setters notify the owning ASC; there is no owner while a CDO/default subobject is constructed.
	InitFinalAttackSpeed(.658f);
}

void UHeroCombatAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	if (Attribute == GetAttackDamageAttribute()) NewValue = FMath::Max(0.f, NewValue);
	if (Attribute == GetBaseAttackSpeedAttribute() || Attribute == GetAttackSpeedRatioAttribute()) NewValue = FMath::Max(.01f, NewValue);
	if (Attribute == GetBonusAttackSpeedPercentAttribute()) NewValue = FMath::Max(-.99f, NewValue);
}

void UHeroCombatAttributeSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	Super::PostAttributeChange(Attribute, OldValue, NewValue);
	if (Attribute == GetBaseAttackSpeedAttribute() || Attribute == GetAttackSpeedRatioAttribute() || Attribute == GetBonusAttackSpeedPercentAttribute()) RecalculateFinalAttackSpeed();
}

void UHeroCombatAttributeSet::RecalculateFinalAttackSpeed()
{
	SetFinalAttackSpeed(FMath::Max(.01f, GetBaseAttackSpeed() + GetAttackSpeedRatio() * GetBonusAttackSpeedPercent()));
}

void UHeroCombatAttributeSet::OnRep_AttackDamage(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, AttackDamage, OldValue);
}

void UHeroCombatAttributeSet::OnRep_BaseAttackSpeed(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, BaseAttackSpeed, OldValue); RecalculateFinalAttackSpeed();
}

void UHeroCombatAttributeSet::OnRep_AttackSpeedRatio(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, AttackSpeedRatio, OldValue); RecalculateFinalAttackSpeed();
}

void UHeroCombatAttributeSet::OnRep_BonusAttackSpeedPercent(const FGameplayAttributeData& OldValue) { 
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, BonusAttackSpeedPercent, OldValue); RecalculateFinalAttackSpeed();
}

void UHeroCombatAttributeSet::OnRep_FinalAttackSpeed(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, FinalAttackSpeed, OldValue);
}

void UHeroCombatAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroCombatAttributeSet, AttackDamage, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroCombatAttributeSet, BaseAttackSpeed, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroCombatAttributeSet, AttackSpeedRatio, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroCombatAttributeSet, BonusAttackSpeedPercent, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UHeroCombatAttributeSet, FinalAttackSpeed, COND_None, REPNOTIFY_Always);
}
