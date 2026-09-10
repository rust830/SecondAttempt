// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ThreeHitPassiveData.generated.h"
class UAnimMontage;
class UGameplayEffect;

USTRUCT(BlueprintType)
struct FThreeHitAttackStage
{
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TObjectPtr<UAnimMontage> Montage;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0")) float DamageMultiplier = 1.f;
	/** Times are in authored montage seconds, before attack-speed play-rate scaling. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float HitTime = .15f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float ChainWindowOpenTime = .25f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float ChainWindowCloseTime = .50f;
	/** On stage 2, a successful input in this short window makes stage 3 knock back. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) bool bPerfectWindowEnablesNextHitKnockback = false;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(EditCondition="bPerfectWindowEnablesNextHitKnockback", ClampMin="0", Units="cm/s")) float NextHitKnockback = 800.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(EditCondition="bPerfectWindowEnablesNextHitKnockback", ClampMin="0", Units="cm/s")) float NextHitLaunch = 120.f;
};

/** Per-hero tuning asset.  The delivery GE reads Data.Damage via SetByCaller. */
UCLASS(BlueprintType)
class LOL_API UThreeHitPassiveData final : public UDataAsset
{
	GENERATED_BODY()
public:
	UThreeHitPassiveData();
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack") TArray<FThreeHitAttackStage> Stages;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0.01")) float ReferenceAttackSpeed = .658f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0", Units="cm")) float TraceDistance = 180.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0", Units="cm")) float TraceRadius = 55.f;
	/** Instant GE with a SetByCaller magnitude named Data.Damage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage") TSubclassOf<UGameplayEffect> DamageEffect;
};
