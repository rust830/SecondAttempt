// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Blocking.generated.h"

/**
 * 格挡窗口：持续型 GE，时长由 Data.BlockWindow(SetByCaller) 决定，期间授予 State.Blocking。
 *
 * 窗口就是这一个 GE 的时长 —— 不开定时器。定时器两端各跑一份会漂移、能力结束后要记得清、
 * 预测回滚时状态还对不上；GE 时长是网络同步的，到期引擎自己摘。
 *
 * ⚠️ 这个时长【两端完全一致】（都填 BlockWindow）。联网宽限加在判定层
 * （UBlockComponent 的 WindowCloseServerTime），不要把 RTT/2 加到这里，原因见 BlockComponent.h。
 */
UCLASS()
class LOL_API UGE_Blocking : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Blocking(const FObjectInitializer& ObjectInitializer);
};
