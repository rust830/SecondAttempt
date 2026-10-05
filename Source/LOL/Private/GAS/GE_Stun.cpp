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

	// 眩晕表现（眩晕 Montage）挂在 cue 上：GE 挂上 → OnActive 播眩晕蒙太奇，GE 到期/被打断 → OnRemove 收掉。
	// 和 UGE_Stealth / UGE_EmpoweredAttack 同一个理由：什么时候挂上、什么时候被移除引擎最清楚，
	// 挂这里 → cue 自动 OnActive/OnRemove，不管是谁、以什么方式结束了眩晕都不会漏还原。
	// 用原生标签对象构造，CDO 阶段不做字符串查找。
	GameplayCues.Add(FGameplayEffectCue(LOLGameplayTags::GameplayCue_Stun, 0.f, 0.f));
}
