// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Stun.generated.h"

/**
 * 眩晕等硬控：持续型 GE，时长由施加方用 Data.ControlDuration(SetByCaller) 填。
 *
 * 授予 State.Stunned（基类 ActivationBlockedTags 默认含它 → 挡一切技能，含普攻）
 * 并取消当前所有已激活技能（读条/持续施法被打断）。
 * 移动的封锁不在这里做：要动 CharacterMovement 的话得是 Character 自己听标签，
 * 现在项目里还没有会走路的硬控，先只做施法侧。
 *
 * 韧性（Tenacity）换算时长还没接 —— 属性集里有 Tenacity，等真做控制链时在这里乘 (1-Tenacity)。
 */
UCLASS()
class LOL_API UGE_Stun : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Stun(const FObjectInitializer& ObjectInitializer);
};
