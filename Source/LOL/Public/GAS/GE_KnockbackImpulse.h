// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_KnockbackImpulse.generated.h"

/**
 * 纯冲量击退：Instant GE，【不带任何控制标签】，只推一下。
 *
 * 【和 UGE_Knockback 的分工】
 * UGE_Knockback 是「硬控」：HasDuration + 授 State.Knockback（挡技能/挡移动）+ 击退蒙太奇 + 位移。
 * 拳击普攻每一下都要把人推开一点，但每一下都挂硬控就太强了（等于平 A 自带软控，
 * 连打四段就是 4 次打断），所以这里拆一个只要位移的版本。
 *
 * 位移复用 UGEComponent_Knockback：它读 Data.KnockbackImpulse / Data.KnockbackLaunch，
 * 内部 LaunchCharacter，方向四级回退（命中法线 → 目标-施加者 → 施加者朝向 → 目标朝向）。
 *
 * ⚠️ **Instant GE 也会触发 GE 组件的 OnGameplayEffectApplied**（这是能不能这么写的关键）：
 * AbilitySystemComponent.cpp 里 `Spec.Def->OnApplied(...)` 在
 * `if (DurationPolicy == Instant) ExecuteGameplayEffect(...)` 那个分支【之后】无条件调用，
 * 到 UGameplayEffect::OnApplied 再转发给 GEComponents。
 * 所以 Instant + 只挂这一个组件 = 推一下就结束，ActiveGameplayEffects 里不留任何东西。
 *
 * ⚠️ 组件内部自己门了 `ActiveGEContainer.OwnerIsNetAuthority`，客户端不会重复推。
 */
UCLASS()
class LOL_API UGE_KnockbackImpulse : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_KnockbackImpulse(const FObjectInitializer& ObjectInitializer);
};
