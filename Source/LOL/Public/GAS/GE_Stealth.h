// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Stealth.generated.h"

/** 隐身效果：持续型 GE，时长由 Data.StealthDuration(SetByCaller) 决定，期间授予 State.Stealth。 */
UCLASS()
class LOL_API UGE_Stealth : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Stealth(const FObjectInitializer& ObjectInitializer);
};
