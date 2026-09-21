// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_Slow.h"

#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_Slow::UGE_Slow(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长：SetByCaller，实际值由施加方填。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_SlowDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	// MoveSpeed × 倍率。
	// 用 MultiplyCompound 而不是 Multiplicitive，理由见头文件 —— 后者是
	// MultiplyAdditive 的兼容别名，会把多个减速的倍率【相加】，0.7+0.7=1.4 变成加速。
	FGameplayModifierInfo SlowModifier;
	SlowModifier.Attribute = UHeroCombatAttributeSet::GetMoveSpeedAttribute();
	SlowModifier.ModifierOp = EGameplayModOp::MultiplyCompound;

	FSetByCallerFloat SlowMultiplier;
	SlowMultiplier.DataTag = LOLGameplayTags::Data_SlowMultiplier;
	SlowModifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(SlowMultiplier);

	Modifiers.Add(SlowModifier);

	// State.Slowed：纯给动画层看的影子标签，不参与任何 GAS 判定
	// （它不在任何能力的 ActivationBlockedTags 里，减不了速也挡不了技能）。
	// 授予标签要走 TargetTagsGameplayEffectComponent，CDO 构造里必须用
	// CreateDefaultSubobject 而不是 AddComponent。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Slowed);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);
}
