// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GA_Flash.generated.h"

class ACharacter;
class UNiagaraSystem;
class USoundBase;

/** 召唤师技能「闪现」：朝相机朝向水平瞬移一段距离，带墙体安全检测（第三人称）。 */
UCLASS(Blueprintable)
class LOL_API UGA_Flash : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_Flash();
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	/** 闪现最大距离（单位 cm）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Flash", meta=(ClampMin="0", Units="cm"))
	float FlashRange = 425.f;

	/** 闪现粒子特效（表现层，可选）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Flash|Effects")
	TObjectPtr<UNiagaraSystem> FlashNiagara;

	/** 闪现音效（表现层，可选）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Flash|Effects")
	TObjectPtr<USoundBase> FlashSound;

private:
	FVector ComputeFlashDirection(const ACharacter* Character) const;
	bool TryFindBlinkDestination(const ACharacter* Character, const FVector& Start, const FVector& Target, FVector& OutDestination) const;
	void PlayFlashEffects(const FVector& Location) const;
};
