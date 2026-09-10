// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GA_ThreeHitPassive.generated.h"
class UThreeHitPassiveData;
class UAbilityTask_WaitGameplayEvent;
struct FThreeHitAttackStage;

/** Predicted presentation, server-authoritative hit confirmation and damage application. */
UCLASS(Blueprintable)
class LOL_API UGA_ThreeHitPassive : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_ThreeHitPassive();
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Config") TObjectPtr<UThreeHitPassiveData> PassiveData;

private:
	UFUNCTION() void OnAttackInput(FGameplayEventData Payload);
	void StartStage(int32 NewStage);
	void OpenChainWindow();
	void CloseChainWindow();
	void ConfirmHit();
	void ApplyServerHit(const FThreeHitAttackStage& Stage, bool bKnockback);
	float GetAttackPlayRate() const;

	UPROPERTY(EditDefaultsOnly, Category="Tags") FGameplayTag AttackInputTag;
	UPROPERTY(EditDefaultsOnly, Category="Tags") FGameplayTag DamageSetByCallerTag;
	TObjectPtr<UAbilityTask_WaitGameplayEvent> InputTask;
	FTimerHandle HitTimer, OpenTimer, CloseTimer;
	int32 StageIndex = INDEX_NONE;
	bool bWindowOpen = false;
	bool bQueuedNextStage = false;
	bool bEmpowerNextStage = false;
	bool bCurrentStageKnocksBack = false;
};
