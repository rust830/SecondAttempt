// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_Stealth.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_Stealth::UGE_Stealth(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由 GA_Stealth::ActivateAbility 填入（StealthDuration）。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_StealthDuration;   // 直接用原生注册的标签对象
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	//授予标签要通过 TargetTagsGameplayEffectComponent。
	// CDO 构造里不能用 AddComponent，必须用 CreateDefaultSubobject。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Stealth);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 隐身表现（角色涂层 + 本地屏幕边缘框）挂在 cue 上，而不是写在 GA_Stealth 里：
	// GE 是本效果的「状态」，什么时候挂上、什么时候被移除（破隐/到期/被驱散）引擎最清楚。
	// 挂在这里 → cue 自动 OnActive/OnRemove，不管是谁、以什么方式结束了隐身都不会漏还原。
	// 用原生标签对象构造，CDO 阶段不做字符串查找。
	// MinLevel/MaxLevel 留 0：只影响 GetLevelPercentage，不参与「cue 是否触发」的判断。
	GameplayCues.Add(FGameplayEffectCue(LOLGameplayTags::GameplayCue_Stealth, 0.f, 0.f));
}
