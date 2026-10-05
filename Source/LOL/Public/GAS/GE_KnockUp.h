// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_KnockUp.generated.h"

/**
 * 击飞：硬控，持续型 GE，时长由施加方用 Data.KnockUpDuration(SetByCaller) 填。
 *
 * 它做的事和 UGE_Knockback 完全同构：
 *  1. 授予 State.KnockUp —— 基类 ActivationBlockedTags 默认含它，挂上即挡一切技能（含普攻）。
 *  2. 取消当前所有已激活技能。
 *  3. 击飞表现（升空 Montage）挂在 cue 上。
 *
 * 和 UGE_Knockback 分开的原因见 LOLGameplayTags.h 里 State.KnockUp 的注释：击退是水平位移
 * （正面/背面方向驱动动画）、击飞是垂直升空，两者动画/手感/韧性减免都不同。
 *
 * 【位移不在这里做】和击退一样，升空的冲量（LaunchCharacter 的 Z 向）是施加方打中那一刻同步做的，
 * 这个 GE 只负责「升空后的硬直状态 + 击飞 Montage 表现」。
 */
UCLASS()
class LOL_API UGE_KnockUp : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_KnockUp(const FObjectInitializer& ObjectInitializer);
};
