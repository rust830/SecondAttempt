// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/GC_StatusMontage.h"
#include "GC_Stun.generated.h"

/**
 * 眩晕表现：被眩晕期间播眩晕 Montage。
 * 逻辑全在基类 AGC_StatusMontage，这里只为拿自己的 GameplayCueTag。
 * 蓝图子类必须命名为 GC_Stun（见 LOLGameplayTags.h 里的命名约定）。
 */
UCLASS()
class LOL_API AGC_Stun : public AGC_StatusMontage
{
	GENERATED_BODY()
public:
	AGC_Stun();
};
