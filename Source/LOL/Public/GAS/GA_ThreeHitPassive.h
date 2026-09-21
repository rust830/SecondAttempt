// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GA_ThreeHitPassive.generated.h"
class UThreeHitPassiveData;
class UAbilityTask_WaitGameplayEvent;
class UGameplayEffect;
class UNiagaraComponent;
class USkeletalMeshComponent;
class USoundBase;
struct FHitResult;
struct FThreeHitAttackStage;

/** Predicted presentation, server-authoritative hit confirmation and damage application. */
UCLASS(Blueprintable)
class LOL_API UGA_ThreeHitPassive : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_ThreeHitPassive();
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Config") TObjectPtr<UThreeHitPassiveData> PassiveData;

	/** 完美窗口武装的强化普攻状态 GE（授予 State.EmpoweredAttack），默认 UGE_EmpoweredAttack。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Config") TSubclassOf<UGameplayEffect> EmpoweredAttackGE;

private:
	UFUNCTION() void OnAttackInput(FGameplayEventData Payload);
	void StartStage(int32 NewStage);
	void OpenChainWindow();
	void CloseChainWindow();
	void ConfirmHit();
	void ApplyServerHit(const FThreeHitAttackStage& Stage);
	float GetAttackPlayRate() const;

	/** 把本段配的完美窗口换算成世界时间区间（供 OnAttackInput 判定）。关 <= 开 表示本段没有完美窗口。 */
	void ComputePerfectWindow(const FThreeHitAttackStage& Stage, float Rate);

	/** 完美窗口触发时挂强化普攻状态（时长取 PassiveData->EmpowerDuration）。 */
	void GrantEmpoweredAttack();

	// ---------------------------------------------------------------------
	// 完美窗口的 QTE 表现（NS 和插槽配在 UThreeHitPassiveData 上）
	// 时刻直接用 ComputePerfectWindow 算出来的那两个世界时间，不另算一套：提示光和判定错开一帧
	// 就会变成「光还没扫到刀尖就已经算完美」。
	// ---------------------------------------------------------------------

	/** 本段完美窗口的长度（世界秒）。<= 0 表示本段没有完美窗口，表现整段跳过。 */
	float GetPerfectWindowDuration() const;

	/** 按算好的完美窗口排两个定时器：开的时候生成提示光，关的时候收掉。 */
	void SchedulePerfectWindowVFX();

	/** 窗口打开：在 Base→Tip 之间生成提示光，并按窗口长度缩放 NS 的播放速度。 */
	void ShowPerfectWindowVFX();

	/** 每帧把两个插槽的世界坐标喂给提示光，然后续下一帧（UGameplayAbility 没有 Tick）。 */
	void TickPerfectWindowVFX();

	/** 窗口关闭：停掉提示光。 */
	void HidePerfectWindowVFX();

	/** 踩中完美窗口那一刻：放一声 QTE 反馈音（PerfectWindowSuccessSound）。 */
	void PlayPerfectSuccessSound();

	/** 本段起手时放挥击音效，强化那一击换 EmpoweredAttackSound。 */
	void PlayAttackSound(bool bEmpowered);

	/** 取角色网格（不是 Character / 没有 mesh 时返回 null）。 */
	USkeletalMeshComponent* GetAvatarMesh() const;

	/**
	 * 强化那一击命中一个目标时，走 GameplayCue.EmpoweredHit 在命中点放额外效果。
	 * 必须走 cue：ApplyServerHit 只在服务端跑，直接 Spawn 的话粒子只有主机看得到。
	 */
	void ExecuteEmpoweredHitCue(const FHitResult& Hit) const;

	/** 当前正在播的提示光（没有就是 null）。 */
	UPROPERTY(Transient) TObjectPtr<UNiagaraComponent> PerfectWindowVFX;

	FTimerHandle PerfectVFXOpenTimer;
	FTimerHandle PerfectVFXCloseTimer;
	FTimerHandle PerfectVFXTickTimer;

	/** 提示光是不是正在跑。也是每帧自续的那个定时器的停止开关。 */
	bool bPerfectWindowVFXActive = false;

	/** 「NS / 音效没配」的警告各只打一次，避免每次连段都刷屏（和 GC_EmpoweredAttack 同一个套路）。 */
	bool bLoggedMissingPerfectWindowVFX = false;
	bool bLoggedMissingPerfectWindowSound = false;
	bool bLoggedMissingAttackSound = false;

	UPROPERTY(EditDefaultsOnly, Category="Tags") FGameplayTag AttackInputTag;
	// 伤害值的 SetByCaller 标签不在这里配了：伤害统一由 GE_Damage + UExecCalc_Damage 结算，
	// 本能力只负责喂 Data.DamageMultiplier / Data.FlatDamage（见 GAS_Block_Setup.md §3.6）。
	TObjectPtr<UAbilityTask_WaitGameplayEvent> InputTask;
	FTimerHandle HitTimer, OpenTimer, CloseTimer;
	int32 StageIndex = INDEX_NONE;

	/**
	 * 「本段是不是强化普攻」。在 StartStage 起手时从 State.EmpoweredAttack 现读一次并锁住，
	 * 命中结算（ApplyServerHit）用这个值而不是再读标签 —— 因为强化在起手那一刻就被消耗掉了（用掉即没），
	 * 到命中结算时标签已经没了。客户端/服务端各自锁各自的，两边起手都会跑到 StartStage。
	 */
	bool bStageEmpowered = false;

	bool bWindowOpen = false;
	bool bQueuedNextStage = false;

	/**
	 * 本段完美窗口的 [开, 关]（世界时间，已按攻速把蒙太奇秒换算过来）。
	 * 关 <= 开 表示本段没有完美窗口（没勾标记 / 没配时间 / 已经是最后一段 / 和连段窗口没交集）。
	 */
	float PerfectWindowOpenWorldTime = 0.f;
	float PerfectWindowCloseWorldTime = 0.f;

	/** 连段窗口内的第一次输入有没有踩中完美窗口 —— 只认第一次，见 OnAttackInput。 */
	bool bPerfectQueued = false;
};
