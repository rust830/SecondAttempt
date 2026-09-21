// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_BlockCooldown.generated.h"

/** 格挡冷却：时长由 Data.Cooldown(SetByCaller) 填，期间授予 State.Cooldown.Block。 */
UCLASS()
class LOL_API UGE_BlockCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_BlockCooldown(const FObjectInitializer& ObjectInitializer);
};
