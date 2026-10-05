// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_DodgeWindow.generated.h"

/**
 * 闪避的无敌窗口：持续型 GE，时长由 Data.DodgeWindow(SetByCaller) 决定，期间授予 State.Dodge.Window。
 *
 * 【和 GE_Blocking 是同一套路，故意做成对称的】格挡的窗口也是「一个 GE 的时长」而不是定时器：
 * 定时器两端各跑一份会漂移、能力结束后要记得清、预测回滚时状态还对不上；GE 时长是网络同步的，
 * 到期引擎自己摘。
 *
 * ⚠️ 和格挡不同的是，这个标签【必须复制】：判定跑在服务端（UExecCalc_Damage），
 * 而被闪避的那一方通常是远端玩家 —— 服务端要能独立知道「他这一刻在无敌窗口里」，
 * 不能靠客户端本地挂的 loose 标签（State.Dodge.Active 就是纯本地的，它只给输入层用）。
 * 走 GE 授予标签就自动复制，这一点和 State.Blocking 一致。
 */
UCLASS()
class LOL_API UGE_DodgeWindow : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_DodgeWindow(const FObjectInitializer& ObjectInitializer);
};
