// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_DodgeCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_DodgeCooldown::UGE_DodgeCooldown(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由 UMyGameplayAbility::ApplyCooldown 填入（CooldownDuration）。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_Cooldown;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Cooldown_Dodge);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);
}
