// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/AbilitySet.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"

void UAbilitySet::GiveToAbilitySystem(UAbilitySystemComponent* ASC) const
{	
	if (!ASC)return;
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
			// 槽位标签就是靠它被 UMyAbilitySystemComponent::OnGiveAbility 认出来的。
			Spec.GetDynamicSpecSourceTags().AddTag(GA.SlotTag);
		}
		ASC->GiveAbility(Spec);
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
			ASC->AddLooseGameplayTags(GrantTag);
	}
}
