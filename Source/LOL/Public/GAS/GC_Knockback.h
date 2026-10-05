// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/GC_StatusMontage.h"
#include "GC_Knockback.generated.h"

class UAnimMontage;

/**
 * 击退表现：被击退期间播击退 Montage，方向（正面打退 / 背面打飞）由受击方向决定。
 *
 * 正面/背面共用一个 State.Knockback 标签（见 LOLGameplayTags.h 里 State.KnockUp 的注释），
 * 动画的区分靠 PickMontage 里 ResolveHitDirection 挑：
 *   - 从正面打来（Front）→ Montage（向后倒）
 *   - 从背面打来（Back） → BackMontage（向前扑）
 * 左/右归到最近的 Front/Back（暂不做左右两套受击）。
 *
 * 蓝图子类必须命名为 GC_Knockback。
 */
UCLASS()
class LOL_API AGC_Knockback : public AGC_StatusMontage
{
	GENERATED_BODY()
public:
	AGC_Knockback();

protected:
	virtual UAnimMontage* PickMontage(AActor* Target, const FGameplayCueParameters& Parameters) const override;

	/** 从背面打来（向前扑）的蒙太奇。留空 = 回退用 Montage（向后倒那套）。 */
	UPROPERTY(EditDefaultsOnly, Category = "StatusMontage")
	TObjectPtr<UAnimMontage> BackMontage;
};
