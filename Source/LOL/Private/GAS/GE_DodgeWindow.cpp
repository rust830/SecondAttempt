// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_DodgeWindow.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_DodgeWindow::UGE_DodgeWindow(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由闪避技能填入（PerfectDodgeWindow）。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_DodgeWindow;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	// UE5.3+：授予标签要通过 TargetTagsGameplayEffectComponent（CDO 构造里不能用 AddComponent）。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Dodge_Window);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);
}
