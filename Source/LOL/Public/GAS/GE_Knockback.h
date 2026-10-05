// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Knockback.generated.h"

/**
 * 击退/击飞：硬控，持续型 GE，时长由施加方用 Data.KnockbackDuration(SetByCaller) 填。
 *
 * 它做两件事，全靠 GE 组件，没有一处需要 C++ 主动调用：
 *  1. 授予 State.Knockback —— 基类 ActivationBlockedTags 默认含它，挂上即无法激活任何技能（含普攻）。
 *  2. 取消当前所有已激活技能 —— 读条/持续施法被打断（和眩晕/死亡同理）。
 *  3. 击退表现（击退 Montage）挂在 cue 上 —— GE 挂上 → OnActive 播击退蒙太奇，到期/被打断 → OnRemove 收掉。
 *
 * 【位移不在这里做】击退的位移（LaunchCharacter）是施加方（技能/命中点）在打中那一刻同步做的瞬时冲量，
 * 这个 GE 只负责「击退之后的硬直状态 + 击退 Montage 表现」。两者职责分开 —— 位移是瞬时的，
 * 硬直是持续的，混在一个 GE 里会让「被击退多远」变成 GE 的属性、反而难调。
 *
 * 和 UGE_Stun 的差别只有语义：眩晕站定不动、击退带位移。要不要「击退期间还叠加眩晕」由施加方决定
 * （两个 GE 可以同时挂，ActivationBlockedTags 里两个标签都会挡技能，互不影响）。
 */
UCLASS()
class LOL_API UGE_Knockback : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Knockback(const FObjectInitializer& ObjectInitializer);
};
