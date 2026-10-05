// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/AbilitySet.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"

void UAbilitySet::GiveToAbilitySystem(UAbilitySystemComponent* ASC) const
{
	// 英雄技能组 / 召唤师技能走这条：授完跟着角色一辈子，没有「撤」这个动作，句柄直接丢弃。
	// 别把下面的循环体复制一份到这儿 —— 出参版才是唯一实现。
	TArray<FGameplayAbilitySpecHandle> Discarded;
	GiveToAbilitySystem(ASC, Discarded);
}

void UAbilitySet::GiveToAbilitySystem(UAbilitySystemComponent* ASC, TArray<FGameplayAbilitySpecHandle>& OutGrantedAbilityHandles) const
{
	if (!ASC) return;

	// Reset 而不是 Add：调用方（ArenaLoadoutComponent）传的容器可能是复用的，
	// 追加两次就会带着上一批的句柄去 ClearAbility —— 摘掉别人还需要的技能。
	OutGrantedAbilityHandles.Reset();

	for (const TSubclassOf<UAttributeSet>& ASClass : GrantAttribute) {
		if (!ASClass || ASC->GetAttributeSet(ASClass))continue;
		UAttributeSet* AS = NewObject<UAttributeSet>(ASC->GetOwner(), ASClass);
		ASC->AddAttributeSetSubobject(AS);
	}
	for (const FAbilitySet_GrantAbility& GA : GrantAbility) {
		if (!GA.Ability)continue;
		FGameplayAbilitySpec Spec(GA.Ability, GA.AbilityLevel);
		if (GA.SlotTag.IsValid()) {
			// 5.5 起 DynamicAbilityTags 改名成 GetDynamicSpecSourceTags()（同一个容器，只是名字更准）。
			// 槽位标签就是靠它被 UMyAbilitySystemComponent::OnGiveAbility 认出来的 ——
			// 而那个函数只认【以 "Ability.Slot." 开头】的标签，写成别的前缀会静默失效
			//（技能授进来了但按不出来，日志只有一句「槽位未授权」）。
			Spec.GetDynamicSpecSourceTags().AddTag(GA.SlotTag);
		}
		// ★ 收句柄。撤回时只能用 ClearAbility(Handle)，不能用 spec 数组下标
		//   （下标会随别的技能授予/移除而移位）。
		OutGrantedAbilityHandles.Add(ASC->GiveAbility(Spec));
	}
	for (const FAbilitySet_GrantEffect& GE : GrantEffect) {
		if (!GE.Effect)continue;
		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(GE.Effect, GE.EffectLevel, Context);
		if (Spec.IsValid()) {
			ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
		}
	}
	if (GrantTag.Num() > 0) {
		// ⚠️ 5.8 的默认 TagRepState = None ⇒ 不复制。头文件里那条注释有详细说明。
		//   要复制就显式传 EGameplayTagReplicationState::TagOnly。
		ASC->AddLooseGameplayTags(GrantTag);
	}
}
