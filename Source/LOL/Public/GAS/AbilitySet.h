// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "AbilitySet.generated.h"

class UGameplayAbility;
class UGameplayEffect;
class UAttributeSet;
class UAbilitySystemComponent;
/**
 * 
 */
USTRUCT(BlueprintType)
struct FAbilitySet_GrantAbility {
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly) FGameplayTag SlotTag;
	UPROPERTY(EditDefaultsOnly) TSubclassOf<UGameplayAbility> Ability;
	UPROPERTY(EditDefaultsOnly, meta = (ClampMin = "1")) int32 AbilityLevel = 1;
};
USTRUCT(BlueprintType)
struct FAbilitySet_GrantEffect {
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly) TSubclassOf<UGameplayEffect> Effect;
	UPROPERTY(EditDefaultsOnly, meta = (ClampMin = "0.01")) float EffectLevel = 1.f;
};
UCLASS()
class LOL_API UAbilitySet final: public UPrimaryDataAsset
{
	GENERATED_BODY()
public:
	void GiveToAbilitySystem(UAbilitySystemComponent* ASC)const;
	UPROPERTY(EditDefaultsOnly,Category="Abilities")
	TArray<FAbilitySet_GrantAbility> GrantAbility;
	UPROPERTY(EditDefaultsOnly, Category = "Abilities")
	TArray<FAbilitySet_GrantEffect> GrantEffect;
	UPROPERTY(EditDefaultsOnly, Category = "Abilities")
	TArray<TSubclassOf<UAttributeSet>> GrantAttribute;
	UPROPERTY(EditDefaultsOnly, Category = "Abilities")
	FGameplayTagContainer GrantTag;
};
