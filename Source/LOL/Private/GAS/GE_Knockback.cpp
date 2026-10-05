// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_Knockback.h"
#include "GAS/GEComponent_Knockback.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/CancelAbilityTagsGameplayEffectComponent.h"

UGE_Knockback::UGE_Knockback(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由施加方填。FSetByCallerFloat 没有默认值字段：谁都不填 = 时长 0 = 挂上即过期。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_KnockbackDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	// UE5.3+：授予标签要通过 TargetTagsGameplayEffectComponent，CDO 构造里只能用 CreateDefaultSubobject。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Knockback);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 硬控打断一切正在施放的东西（和眩晕/死亡同理）。同样只在权威端生效。
	UCancelAbilityTagsGameplayEffectComponent* CancelAbilities =
		ObjectInitializer.CreateDefaultSubobject<UCancelAbilityTagsGameplayEffectComponent>(this, TEXT("CancelAbilityTags"));
	GEComponents.Add(CancelAbilities);
	CancelAbilities->SetAndApplyCanceledAbilityTagChanges(FInheritedTagContainer(), FInheritedTagContainer());

	// 击退表现（击退 Montage）挂在 cue 上，和 UGE_Stun / UGE_Stealth 同一个理由。
	// 用原生标签对象构造，CDO 阶段不做字符串查找。
	GameplayCues.Add(FGameplayEffectCue(LOLGameplayTags::GameplayCue_Knockback, 0.f, 0.f));

	// 位移组件：GE 挂上时在权威端对目标 LaunchCharacter（读 Data.KnockbackImpulse / Data.KnockbackLaunch）。
	// 施加方只要填这两个 SetByCaller，不用自己写 LaunchCharacter —— 击退「硬直 + 蒙太奇 + 位移」全在 GE 里。
	UGEComponent_Knockback* KnockbackComp =
		ObjectInitializer.CreateDefaultSubobject<UGEComponent_Knockback>(this, TEXT("KnockbackComponent"));
	GEComponents.Add(KnockbackComp);
}
