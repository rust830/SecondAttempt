// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectComponent.h"
#include "GEComponent_Knockback.generated.h"

/**
 * GE_Knockback 的位移组件：GE 挂上时，在权威端对目标 LaunchCharacter。
 *
 * 把「击退位移」从施加方（技能里那行 LaunchCharacter）搬进 GE 组件，让 GE_Knockback 自成一体：
 *   施加方只要「挂 GE + 填两个 SetByCaller（水平冲量 / 上抛冲量）」，硬直（State.Knockback）、
 *   蒙太奇（GameplayCue.Knockback）、位移（本组件）就都齐了，不用各技能各写一遍 LaunchCharacter。
 *
 * 方向优先级：
 *   ① 命中法线（EffectContext 里的 HitResult.ImpactNormal，取反 = 远离命中面，
 *      但只取水平分量 —— 法线若原本是竖直的（砸胶囊顶/底），这一级会退化成零向量）
 *   ② （目标 - 施加者）水平方向
 *   ③ 施加者的水平朝向（打推力方向：专治「上下叠着」的砸击，见下）
 *   ④ 目标朝向兜底
 *
 * ③ 是补上的：飞踢是自上而下砸，目标与施加者的水平差接近 0 ⇒ ②退化；而目标自己的朝向
 * 由它自己/它的玩家决定，可能正背对攻击者 ⇒ ④方向是错的。用【施加者朝哪打】才是
 * 「被这一下往哪个方向推」，这也是经典动作游戏里击退方向的常规取法。
 *
 * 两个冲量都允许负值：水平为负 = 往施加者拉，竖直为负 = 向下砸。
 * 竖直为负时的额外纪律（见 GE_Knockback.h）：把水平冲量填成 0 就等于放弃了水平位移 ——
 * 目标只会在原地上下弹一下。想让砸击有位移就必须给水平冲量，方向由上表 ①~④ 定。
 *
 * 位移只在权威端执行（同伤害/死亡），客户端靠位置复制。
 */
UCLASS()
class LOL_API UGEComponent_Knockback : public UGameplayEffectComponent
{
	GENERATED_BODY()

public:
	virtual void OnGameplayEffectApplied(FActiveGameplayEffectsContainer& ActiveGEContainer, FGameplayEffectSpec& GESpec, FPredictionKey& PredictionKey) const override;
};
