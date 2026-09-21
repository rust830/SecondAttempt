// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GA_Block.generated.h"

class UGameplayEffect;
class UAnimMontage;

/**
 * 右键「格挡」：开一个由 GE 时长承载的窗口，窗口内挨打算格挡成功（吃掉这一击 + 挂免疫）。
 *
 * 这是个**薄能力**：只做「提交 CD → 挂 GE_Blocking → 播施法动作 → 立刻结束」。
 * 窗口的状态全在 GE 上（State.Blocking），判定在 UExecCalc_Damage → UBlockComponent 里，
 * 防护罩表现在 GE_BlockImmune 的 cue 上。能力不需要活到窗口关闭。
 *
 * 和隐身那套同构：状态是 GE 标签（唯一事实来源），能力只负责提交 CD + 挂 GE，表现走 cue。
 */
UCLASS(Blueprintable)
class LOL_API UGA_Block : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_Block();
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	/** 格挡窗口 GE（授予 State.Blocking）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Block")
	TSubclassOf<UGameplayEffect> BlockingGE;

	/** 窗口时长（秒），作为 SetByCaller 填入 GE。⚠️ 两端必须填同一个值，联网宽限在判定层。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Block", meta = (ClampMin = "0"))
	float BlockWindow = 0.4f;

	/**
	 * 施法动作（可选）。
	 * 留在能力里没搬去 cue，理由和 GA_Stealth 一样：施法动作要跟着预测时机在本地立刻播，
	 * 交给 cue 会丢掉本地手感。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Block|Anim")
	TObjectPtr<UAnimMontage> BlockMontage;
};
