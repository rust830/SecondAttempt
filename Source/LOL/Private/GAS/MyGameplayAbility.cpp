// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/MyGameplayAbility.h"
#include "GameplayEffect.h"
#include "GAS/LOLGameplayTags.h"

void UMyGameplayAbility::ApplyCooldown(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const
{
	UGameplayEffect* Effect = GetCooldownGameplayEffect();
	if (!Effect) {
		Super::ApplyCooldown(Handle, ActorInfo, ActivationInfo);
		return;
	}
	FGameplayEffectSpecHandle SpecHandle = MakeOutgoingGameplayEffectSpec(Effect->GetClass(), GetAbilityLevel());
	if (SpecHandle.IsValid() && CooldownDuration > 0) {
		SpecHandle.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_Cooldown, CooldownDuration);
	}
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, SpecHandle);
}
