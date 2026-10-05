// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_GroundDodgeCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_GroundDodgeCooldown::UGE_GroundDodgeCooldown(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_Cooldown;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Cooldown_GroundDodge);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);
}
