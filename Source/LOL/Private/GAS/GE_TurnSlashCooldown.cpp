// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_TurnSlashCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_TurnSlashCooldown::UGE_TurnSlashCooldown(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由 UMyGameplayAbility::ApplyCooldown 填入（CooldownDuration）。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_Cooldown;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	// UE5.3+：授予冷却标签要通过 TargetTagsGameplayEffectComponent。
	// CDO 构造里不能用 AddComponent，必须用 CreateDefaultSubobject。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Cooldown_TurnSlash);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);
}
