// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "InputConfig.generated.h"

class UInputAction;
/**
 * 
 */
USTRUCT(BlueprintType)
struct FAbilityInputAction
{
    GENERATED_BODY()
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TObjectPtr<UInputAction> InputAction;
    /** 该按键对应的槽位标签：Ability.Slot.Q / W / E / R / D / F。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FGameplayTag SlotTag;
};

UCLASS()
class LOL_API UInputConfig final: public UDataAsset
{
	GENERATED_BODY()
public:
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TArray<FAbilityInputAction> AbilityInputActions;
	
};
