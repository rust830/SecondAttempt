// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_StatusMontage.generated.h"

class UAnimInstance;
class UAnimMontage;

/**
 * 「挂一个持续状态、期间播一个 Montage」这类硬控表现（眩晕 / 击退 / 击飞 / 以及以后的恐惧、定身…）的共用基类。
 *
 * 生命周期挂在对应 GE 的 GameplayCues 上：GE 挂上 → OnActive 播蒙太奇，GE 到期/被打断 → OnRemove 收掉。
 * 和 UGE_EmpoweredAttack 那套同一个理由 —— 什么时候挂上、什么时候被移除引擎最清楚，不用各状态各自操心收尾。
 *
 * 每个状态一个子类，只为拿不同的 GameplayCueTag（引擎按类名推导标签，同一 C++ 类派出的蓝图会被重新推导成
 * 同一个无效标签，见 LOLGameplayTags.h 里的说明）。子类可以再 override PickMontage 按需换蒙太奇
 * （比如击退按受击方向挑正面/背面两条）。
 *
 * 用 Actor 变体而不是 Static：它要跨一段时间存在，并且得在 OnRemove 里把播的 Montage 收掉。
 * cue 在每个客户端各跑一次，所以敌我双方都看得到状态动作，不需要复制 Montage。
 */
UCLASS(Abstract)
class LOL_API AGC_StatusMontage : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()

public:
	AGC_StatusMontage();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;

	/** 状态期间播的 Montage（可空：只出声/只变色不出动作，但硬控建议都配）。 */
	UPROPERTY(EditDefaultsOnly, Category = "StatusMontage")
	TObjectPtr<UAnimMontage> Montage;

	/** Montage 播放速率（1 = 原速）。 */
	UPROPERTY(EditDefaultsOnly, Category = "StatusMontage")
	float MontagePlayRate = 1.f;

	/**
	 * 播 Montage 时是否停掉角色身上其它 Montage。默认 true：硬控应当打断当前正在播的攻击/移动 Montage。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "StatusMontage")
	bool bStopOtherMontages = true;

	/** OnRemove 时停 Montage 的淡出时间（秒）。 */
	UPROPERTY(EditDefaultsOnly, Category = "StatusMontage", meta = (ClampMin = "0"))
	float MontageStopBlendOut = 0.25f;

protected:
	/**
	 * 挑这条 cue 要播哪条 Montage。默认返回 Montage。
	 * 子类 override 它按状态上下文换蒙太奇（GC_Knockback 按受击方向挑正面/背面）。
	 * 返回值会记进 ActiveMontage，OnRemove 用它来停，保证「停的就是播的那条」。
	 */
	virtual UAnimMontage* PickMontage(AActor* Target, const FGameplayCueParameters& Parameters) const;

private:
	/** 取目标角色的 AnimInstance（不是 Character / 没有 mesh 时返回 null）。 */
	UAnimInstance* GetTargetAnimInstance(AActor* Target) const;

	/** 在目标身上播指定 Montage（没配则只打一条警告）。 */
	void PlayStatusMontage(AActor* Target, UAnimMontage* InMontage);

	/** 停掉本 cue 播的 Montage（没在播时是 no-op）。 */
	void StopStatusMontage(AActor* Target);

	/** 本 cue 实际播的那条 Montage（OnRemove 靠它停对）。 */
	UPROPERTY(Transient)
	TObjectPtr<UAnimMontage> ActiveMontage;

	/** 「Montage 没配」的警告只打一次，避免反复挂状态刷屏。 */
	bool bLoggedMissingMontage = false;
};
