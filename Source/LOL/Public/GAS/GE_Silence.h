// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Silence.generated.h"

/**
 * 沉默：持续型 GE，时长由施加方用 Data.ControlDuration(SetByCaller) 填。
 *
 * 只授予 State.Silenced，那个标签只写在【法术】技能的 ActivationBlockedTags 上，
 * 普攻（UGA_ThreeHitPassive）刻意不加 —— 沉默挡技能不挡平 A 是 LoL 语义。
 *
 * 【不取消已激活的技能】：沉默是「不让开新的」，不是「打断正在放的」。
 * 打断是眩晕/死亡的事（见 UGE_Stun / UGE_Death 的 Cancel 组件）。
 * 给沉默也加 Cancel 组件的表现是「被沉默瞬间大招自己断了」，和 LoL 不一样。
 */
UCLASS()
class LOL_API UGE_Silence : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Silence(const FObjectInitializer& ObjectInitializer);
};
