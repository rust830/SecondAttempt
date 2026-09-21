// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_DeathHarvestCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_DeathHarvestCooldown::UGE_DeathHarvestCooldown(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由 UMyGameplayAbility::ApplyCooldown 填入（CooldownDuration，
	// 已经按技能急速换算过）。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_Cooldown;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	// ⚠️ 冷却标签必须走 UTargetTagsGameplayEffectComponent：GetGrantedTags 只从 GEComponents 聚合，
	// 老的 InheritableOwnedTagsContainer 已经废弃（见 ue58-gas-granted-tags-component 那条）。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Cooldown_DeathHarvest);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);
}
