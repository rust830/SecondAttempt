// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_TurnSlashCooldown.generated.h"

/** TurnSlash（回身击退）冷却：持续型 GE，时长由 Data.Cooldown(SetByCaller) 决定，期间持有 State.Cooldown.TurnSlash。 */
UCLASS()
class LOL_API UGE_TurnSlashCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_TurnSlashCooldown(const FObjectInitializer& ObjectInitializer);
};
