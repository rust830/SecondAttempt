// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_BlockImmune.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_BlockImmune::UGE_BlockImmune(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由 UBlockComponent 填入（ImmuneDuration）。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_BlockImmuneDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_BlockImmune);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 防护罩表现（材质 + 粒子 + 音效）全在这个 cue 里 → BP 必须命名为 GC_Block，
	// 否则引擎会把继承来的 GameplayCueTag 按类名重推成无效标签、cue 静默不触发。
	// MinLevel/MaxLevel 留 0：只影响 GetLevelPercentage，不参与「cue 是否触发」的判断。
	GameplayCues.Add(FGameplayEffectCue(LOLGameplayTags::GameplayCue_Block, 0.f, 0.f));
}
