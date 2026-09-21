// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_BlockImmune.generated.h"

/**
 * 格挡成功后的免疫：持续型 GE，时长由 Data.BlockImmuneDuration(SetByCaller) 决定，
 * 期间授予 State.BlockImmune 并挂 GameplayCue.Block。
 *
 * 防护罩表现挂在这个 GE 的 cue 上而不是手动 Spawn：
 * 免疫挂上 → OnActive → 生成防护罩；免疫到期/被驱散 → OnRemove → 收掉。
 * 三者天然对齐，不需要任何外部清理调用，而且每个客户端各跑一遍、天然多端一致。
 */
UCLASS()
class LOL_API UGE_BlockImmune : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_BlockImmune(const FObjectInitializer& ObjectInitializer);
};
