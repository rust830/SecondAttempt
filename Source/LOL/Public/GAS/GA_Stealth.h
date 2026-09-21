// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GA_Stealth.generated.h"

class UGameplayEffect;
class UAnimMontage;

/**
 * Q 技能「隐身」：按 Q 进入持续一段时间的隐身，普攻或施放其他技能时提前破除。状态由 GE 承载。
 *
 * 隐身结束（被打破或自然到期）时挂一个 State.EmpoweredAttack 状态，让破隐的那一击（或到期后的下一击）
 * 打出强化伤害 + 击退。数值统一在 UThreeHitPassiveData 上，能力只负责「什么时候挂、挂多久」。
 */
UCLASS(Blueprintable)
class LOL_API UGA_Stealth : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_Stealth();
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	/** 隐身效果 GE（授予 State.Stealth）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stealth")
	TSubclassOf<UGameplayEffect> StealthGE;

	/** 隐身持续时间（秒），作为 SetByCaller 填入 GE。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stealth", meta = (ClampMin = "0"))
	float StealthDuration = 8.f;

	/** 破隐后挂的强化普攻状态 GE（授予 State.EmpoweredAttack）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stealth|Empower")
	TSubclassOf<UGameplayEffect> EmpoweredAttackGE;

	/** 破隐后强化普攻的窗口时长（秒）—— 空挥没打中时，这段时间内还能再打一次。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stealth|Empower", meta = (ClampMin = "0"))
	float EmpowerDuration = 3.f;

	/**
	 * 进入隐身 Montage（可选）。
	 * 留在能力里没搬去 cue：它和别的表现不一样，是「施法动作」，跟能力的预测时机绑在一起，
	 * 交给 cue 反而会因为 cue 只在服务端触发而丢掉本地的手感。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stealth|Anim")
	TObjectPtr<UAnimMontage> StealthMontage;

	// 粒子、音效、隐身涂层、屏幕框全部搬到 GameplayCue（见 AGC_Stealth），
	// 挂在 UGE_Stealth 的 GameplayCues 上——GE 何时被移除，表现就何时收，不需要能力操心。

private:
	/** State.Stealth 计数变化回调：归零 = 隐身结束。 */
	void OnStealthTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	/** 挂强化普攻状态（仅权威侧）。 */
	void GrantEmpoweredAttack();

	/** 解绑标签计数回调，避免能力结束后还留悬垂委托。 */
	void UnregisterStealthTagEvent();

	/** State.Stealth 计数回调的句柄。 */
	FDelegateHandle StealthTagDelegateHandle;
};
