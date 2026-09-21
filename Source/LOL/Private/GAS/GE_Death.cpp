// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_Death.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/CancelAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/RemoveOtherGameplayEffectComponent.h"

UGE_Death::UGE_Death(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由 AHeroCombatCharacter::HandleOutOfHealth 填入（RespawnDelay）。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_RespawnDelay;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	// UE5.3+：授予标签要走 TargetTagsGameplayEffectComponent（GetGrantedTags 只从 GEComponents 聚合）。
	// CDO 构造里不能用 AddComponent，必须用 CreateDefaultSubobject。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Dead);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 死亡打断一切：两个容器都留空 = 取消全部已激活技能（含大招）。
	// 想改成「只断大招」的话，往 CanceledWithTags 里放 Ability_Type_Ultimate 即可。
	UCancelAbilityTagsGameplayEffectComponent* CancelAbilities =
		ObjectInitializer.CreateDefaultSubobject<UCancelAbilityTagsGameplayEffectComponent>(this, TEXT("CancelAbilityTags"));
	GEComponents.Add(CancelAbilities);
	CancelAbilities->SetAndApplyCanceledAbilityTagChanges(FInheritedTagContainer(), FInheritedTagContainer());

	// 死亡顺手剥掉身上的状态 GE。取消组件只管「正在施放的技能」，管不到「已经挂上、没人再管的状态」——
	// GE_Blocking / GE_EmpoweredAttack 都是这种：授予它们的能力早就结束了（GA_Block 是薄能力），
	// 光靠取消没人会去摘。不摘的表现是「举着盾复活」「带着强化普攻复活」。
	//
	// ⚠️ 顺序有讲究：Cancel 必须排在 Remove 前面（OnApplied 是按 GEComponents 的数组顺序跑的）。
	// 反过来的话，State.Stealth 被摘 → GA_Stealth 的标签归零回调触发 → 给死人挂一份强化普攻。
	//
	// State.Stealth 刻意【不在】这个列表里，它归 GA_Stealth::EndAbility 自己清（那个 GE 的
	// 生命周期本来就由能力持有）。冷却（State.Cooldown.*）也刻意不动：LoL 里冷却在死亡期间照走。
	URemoveOtherGameplayEffectComponent* RemoveOther =
		ObjectInitializer.CreateDefaultSubobject<URemoveOtherGameplayEffectComponent>(this, TEXT("RemoveOtherEffects"));
	GEComponents.Add(RemoveOther);

	FGameplayTagContainer StatesToStrip;
	StatesToStrip.AddTag(LOLGameplayTags::State_Blocking);
	StatesToStrip.AddTag(LOLGameplayTags::State_BlockImmune);
	StatesToStrip.AddTag(LOLGameplayTags::State_EmpoweredAttack);
	RemoveOther->RemoveGameplayEffectQueries.Add(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(StatesToStrip));
}
