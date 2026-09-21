// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Death.generated.h"

/**
 * 死亡状态：持续型 GE，时长 = 复活等待时间（Data.RespawnDelay）。
 *
 * 它做三件事，全靠 GE 组件，没有任何一处需要 C++ 主动调用：
 *
 *  1. 授予 State.Dead —— 所有技能的 ActivationBlockedTags 里都有它（基类默认），挂上即无法激活，普攻也一起挡。
 *  2. 取消当前所有已激活技能 —— 大招放到一半被打死必须断在这里。
 *     两个空容器 = CancelAbilities(nullptr, nullptr) = 取消全部，见引擎
 *     CancelAbilityTagsGameplayEffectComponent.cpp 里 "Empty container will fail the HasAny check" 那段。
 *     ⚠️ 该组件内部第一行就是 `if (!ActiveGEContainer.OwnerIsNetAuthority) return;`，
 *     所以取消【只在权威端真的发生】。挂 GE 的地方因此必须是服务端（见 AHeroCombatCharacter::HandleOutOfHealth）。
 *  3. 到期自动摘掉 State.Dead —— 复活的触发点就是「这个标签计数归零」。
 *     时长只有这一个来源（AHeroCombatCharacter::OnDeadTagChanged），别再另开一个重生计时器，
 *     两份计时迟早对不上。
 */
UCLASS()
class LOL_API UGE_Death : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Death(const FObjectInitializer& ObjectInitializer);
};
