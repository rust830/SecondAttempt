// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_GroundDodgeCooldown.generated.h"

/** 地面闪避冷却：持续型 GE，时长由 Data.Cooldown(SetByCaller) 决定，期间持有 State.Cooldown.GroundDodge。 */
UCLASS()
class LOL_API UGE_GroundDodgeCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_GroundDodgeCooldown(const FObjectInitializer& ObjectInitializer);
};
