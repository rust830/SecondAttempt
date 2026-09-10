// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_ThrowDaggerCooldown.generated.h"

/** 投掷匕首冷却：持续型 GE，时长由 Data.Cooldown(SetByCaller) 决定，期间持有 State.Cooldown.ThrowDagger。 */
UCLASS()
class LOL_API UGE_ThrowDaggerCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_ThrowDaggerCooldown(const FObjectInitializer& ObjectInitializer);
};
