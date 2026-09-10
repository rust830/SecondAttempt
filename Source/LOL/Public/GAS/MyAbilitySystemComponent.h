// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "MyAbilitySystemComponent.generated.h"

/**
 * 
 */
UCLASS()
class LOL_API UMyAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere,BlueprintReadWrite)
	float MyLeastInterval = 0.1f;

	void AbilityInputTagPressed(const FGameplayTag& SlotTag);
	void AbilityInputTagHeld(const FGameplayTag& SlotTag);
	void AbilityInputTagReleased(const FGameplayTag& SlotTag);

	FGameplayAbilitySpecHandle GetHandleForSlot(const FGameplayTag& SlotTag)const;
	int32 GetAbilityLevelForSlot(const FGameplayTag& SlotTag)const;
	void SetAbilityLevelForSlot(const FGameplayTag& SlotTag,int32 NewLevel);

	virtual void OnGiveAbility(FGameplayAbilitySpec& AbilitySpec)override;
	virtual void OnRemoveAbility(FGameplayAbilitySpec& AbilitySpec)override;
private:
	TMap<FGameplayTag, FGameplayAbilitySpecHandle> SlotAbilityMap;
};
