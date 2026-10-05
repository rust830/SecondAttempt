// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_KnockUp.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/CancelAbilityTagsGameplayEffectComponent.h"

UGE_KnockUp::UGE_KnockUp(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_KnockUpDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_KnockUp);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 硬控打断一切正在施放的东西（和眩晕/击退/死亡同理）。同样只在权威端生效。
	UCancelAbilityTagsGameplayEffectComponent* CancelAbilities =
		ObjectInitializer.CreateDefaultSubobject<UCancelAbilityTagsGameplayEffectComponent>(this, TEXT("CancelAbilityTags"));
	GEComponents.Add(CancelAbilities);
	CancelAbilities->SetAndApplyCanceledAbilityTagChanges(FInheritedTagContainer(), FInheritedTagContainer());

	// 击飞表现（升空 Montage）挂在 cue 上。
	GameplayCues.Add(FGameplayEffectCue(LOLGameplayTags::GameplayCue_KnockUp, 0.f, 0.f));
}
