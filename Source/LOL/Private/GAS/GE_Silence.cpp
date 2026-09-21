// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_Silence.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_Silence::UGE_Silence(const FObjectInitializer& ObjectInitializer)
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
	GrantedTags.AddTag(LOLGameplayTags::State_Silenced);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 故意没有 Cancel 组件：沉默不打断已在施放的技能。
}
