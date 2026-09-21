// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_EmpoweredAttack.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_EmpoweredAttack::UGE_EmpoweredAttack(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，由施加方填入：GA_Stealth 填自己的 EmpowerDuration，
	// GA_ThreeHitPassive 填 DA_ThreeHitPassive 的 EmpowerDuration。
	// 注意 FSetByCallerFloat 没有默认值字段：谁都不填的话时长算出来是 0，GE 会立刻过期。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_EmpowerDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	// UE5.3+：授予标签要通过 TargetTagsGameplayEffectComponent，CDO 构造里只能用 CreateDefaultSubobject。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_EmpoweredAttack);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 强化表现（强化 Montage + 音效）挂在 cue 上，和 UGE_Stealth 同一个理由：
	// 「什么时候挂上、什么时候被消耗掉/到期」引擎最清楚，挂在这里 → cue 自动 OnActive/OnRemove，
	// 破隐和完美窗口两条触发路径、以及命中消耗这条移除路径都不用各自操心收尾。
	// 用原生标签对象构造，CDO 阶段不做字符串查找。
	GameplayCues.Add(FGameplayEffectCue(LOLGameplayTags::GameplayCue_EmpoweredAttack, 0.f, 0.f));
}
