// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_DeathHarvestCooldown.generated.h"

/** 大招冷却：持续型 GE，时长由 Data.Cooldown(SetByCaller) 决定，期间持有 State.Cooldown.DeathHarvest。 */
UCLASS()
class LOL_API UGE_DeathHarvestCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_DeathHarvestCooldown(const FObjectInitializer& ObjectInitializer);
};
