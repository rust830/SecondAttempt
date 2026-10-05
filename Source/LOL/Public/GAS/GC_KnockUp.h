// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/GC_StatusMontage.h"
#include "GC_KnockUp.generated.h"

/**
 * 击飞表现：升空期间播击飞 Montage。
 * 逻辑全在基类 AGC_StatusMontage（单条 Montage），这里只为拿自己的 GameplayCueTag。
 * 蓝图子类必须命名为 GC_KnockUp。
 */
UCLASS()
class LOL_API AGC_KnockUp : public AGC_StatusMontage
{
	GENERATED_BODY()
public:
	AGC_KnockUp();
};
