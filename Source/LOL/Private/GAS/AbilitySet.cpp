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
			Spec.DynamicAbilityTags.AddTag(GA.SlotTag);
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
