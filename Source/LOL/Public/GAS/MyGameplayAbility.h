// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "MyGameplayAbility.generated.h"

/** How this ability is triggered. */
UENUM(BlueprintType)
enum class EMyAbilityActivationPolicy : uint8
{
	OnInputTriggered UMETA(DisplayName = "On Input Triggered"),
	OnEvent          UMETA(DisplayName = "On Event"),
	OnGiven          UMETA(DisplayName = "On Given"),
};

/**
 * Common base for all abilities: data-driven cooldown + activation policy.
 */
UCLASS(Blueprintable)
class LOL_API UMyGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()
public:
	/** Cooldown in seconds. Applied via ApplyCooldown + SetByCaller Data.Cooldown. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooldown", meta = (ClampMin = "0"))
	float CooldownDuration = 0.0f;

	/** Mana cost (reserved; hook up a resource attribute + cost GE to enable). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mana", meta = (ClampMin = "0"))
	float ManaCost = 0.0f;

	/** How this ability is triggered. The ASC's button routing only activates OnInputTriggered abilities. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation")
	EMyAbilityActivationPolicy ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	virtual void ApplyCooldown(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const override;
};
