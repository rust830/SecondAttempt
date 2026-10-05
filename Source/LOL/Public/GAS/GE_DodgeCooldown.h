// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_DodgeCooldown.generated.h"

/** 闪避冷却：持续型 GE，时长由 Data.Cooldown(SetByCaller) 决定，期间持有 State.Cooldown.Dodge。 */
UCLASS()
class LOL_API UGE_DodgeCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_DodgeCooldown(const FObjectInitializer& ObjectInitializer);
};
