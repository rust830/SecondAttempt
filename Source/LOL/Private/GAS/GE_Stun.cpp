// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_Stun.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/CancelAbilityTagsGameplayEffectComponent.h"

UGE_Stun::UGE_Stun(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_ControlDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Stunned);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 硬控打断一切正在施放的东西（和死亡同理）。同样只在权威端生效。
	UCancelAbilityTagsGameplayEffectComponent* CancelAbilities =
		ObjectInitializer.CreateDefaultSubobject<UCancelAbilityTagsGameplayEffectComponent>(this, TEXT("CancelAbilityTags"));
	GEComponents.Add(CancelAbilities);
	CancelAbilities->SetAndApplyCanceledAbilityTagChanges(FInheritedTagContainer(), FInheritedTagContainer());
}
