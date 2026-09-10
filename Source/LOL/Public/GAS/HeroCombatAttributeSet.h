// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "HeroCombatAttributeSet.generated.h"

#define HERO_ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/** League style: Final AS = Base AS + AS Ratio * Bonus AS%. 0.35 means +35% bonus AS. */
UCLASS()
class LOL_API UHeroCombatAttributeSet final : public UAttributeSet
{
	GENERATED_BODY()
public:
	UHeroCombatAttributeSet();
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_AttackDamage, Category="Hero|Offense") FGameplayAttributeData AttackDamage;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, AttackDamage)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_BaseAttackSpeed, Category="Hero|Offense") FGameplayAttributeData BaseAttackSpeed;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, BaseAttackSpeed)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_AttackSpeedRatio, Category="Hero|Offense") FGameplayAttributeData AttackSpeedRatio;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, AttackSpeedRatio)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_BonusAttackSpeedPercent, Category="Hero|Offense") FGameplayAttributeData BonusAttackSpeedPercent;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, BonusAttackSpeedPercent)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_FinalAttackSpeed, Category="Hero|Offense") FGameplayAttributeData FinalAttackSpeed;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, FinalAttackSpeed)

	UFUNCTION() void OnRep_AttackDamage(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_BaseAttackSpeed(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_AttackSpeedRatio(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_BonusAttackSpeedPercent(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_FinalAttackSpeed(const FGameplayAttributeData& OldValue);
private:
	void RecalculateFinalAttackSpeed();
};
