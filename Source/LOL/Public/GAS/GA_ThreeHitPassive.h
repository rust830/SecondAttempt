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

	/** 强化那一击的击退 GE（授予 State.Knockback + 位移 + 蒙太奇），默认 UGE_Knockback。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Config") TSubclassOf<UGameplayEffect> KnockbackGE;

	/** 强化那一击击退的硬直时长（秒），作为 Data.KnockbackDuration 填给 KnockbackGE。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Config", meta=(ClampMin="0", Units="s"))
	float KnockbackDuration = 0.5f;

	/**
	 * 蒙太奇里「这一下该结算了」的通知标签（UAnimNotify_SendGameplayEvent）。
	 * 默认 Event.Melee.Impact —— 想换别的标签在编辑器里改这里，同时改蒙太奇上那个 notify。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Tags") FGameplayTag MeleeImpactTag;

protected:
	/**
	 * 连段推进的输入事件标签（ActivateAbility 里拿它建 WaitGameplayEvent 任务）。
	 *
	 * ⚠️ protected 而不是 private：派生类 UGA_BoxingCombo 要把它换成
	 *   Event.Input.ComboAttack（空手四连拳的专用输入标签），见那个类的说明。
	 *
	 * ⚠️ EditDefaultsOnly 意味着【BP 里的值会覆盖构造函数里设的值】。
	 *   派生类如果希望这个标签不可被 BP 改坏（留空=用 C++ 默认），要在
	 *   ActivateAbility 开头再强制设一次 —— UGA_BoxingCombo 就是这么做的。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Tags") FGameplayTag AttackInputTag;

private:
	UFUNCTION() void OnAttackInput(FGameplayEventData Payload);

	/** 蒙太奇上的命中通知到了（只有服务端会发，见 UAnimNotify_SendGameplayEvent::bServerOnly）。 */
	UFUNCTION() void OnAnimImpact(FGameplayEventData Payload);

	void StartStage(int32 NewStage);
	void OpenChainWindow();
	void CloseChainWindow();

	/**
	 * 最后一段的收尾调度（CloseChainWindow 里调）。
	 *
	 * 「连段窗口关闭」和「这一段真的演完了」是两件事：最后一段的窗口关闭时刻
	 * （DS 上配的 ChainWindowCloseTime，通常故意配得很小）可能早于命中时刻、
	 * 更早于蒙太奇播完。在这里直接 EndAbility 的话：
	 *   ① EndAbility 会 ClearTimer(HitTimer) → 命中静默丢失（症状 = 打不出最后一击）；
	 *   ② 收招蒙太奇叠在出拳中途一起播（同 slot 并发）→ 出招画面错乱；
	 *   ③ 能力提前结束 → 玩家再按攻击会立刻重开一段新连招，把还在播的最后一拳顶掉。
	 * 所以收尾统一推迟到 max(命中结算完, 蒙太奇播完)，见 ScheduleLastStageFinish。
	 */
	void ScheduleLastStageFinish();

	/** 推迟收尾的落点：播收招（活着才播）→ 结束能力。 */
	void FinishLastStage();

	/** 兜底：HitTime 定时器到点（本段蒙太奇上没配命中通知时才排，见 StartStage）。 */
	void ConfirmHit();

	/**
	 * 本段结算命中，一段只算一次（bHitAppliedThisStage）。
	 *
	 * 命中时刻的归属是【在 StartStage 里一次定死的】，不是「两个来源抢」：
	 *   蒙太奇上配了 Event.Melee.Impact 通知 → 只认通知，判定跟着动画帧走，不排 HitTime 定时器；
	 *   没配 → 只认 HitTime 定时器，行为和加通知之前完全一样。
	 * 所以现在能跑、加了通知自动升级，中间不存在「配了却被定时器抢掉」的状态。
	 * （闩留着是因为一条蒙太奇上可能被放多个同类通知，以及防御性的重复触发。）
	 *
	 * ⚠️ 配了通知之后有一个行为变化：命中帧【之前】被打断（挥砍中途按 E 播了别的蒙太奇，
	 * Montage_Play 默认会停掉同体的其他蒙太奇）→ 这一击没有伤害。LoL 也是这个行为
	 * （挥砍前摇被打断就不出伤害），但这确实是改动前没有的。不想要就把蒙太奇上的通知删掉，
	 * 判定会退回 HitTime 定时器。
	 */
	void ResolveHit(bool bFromAnimNotify);

	void ApplyServerHit(const FThreeHitAttackStage& Stage);
	float GetAttackPlayRate() const;

	/** 本段连段输入缓冲的时长（世界秒 = ChainInputBufferTime / 攻速倍率）。见 OnAttackInput。 */
	float GetChainInputBufferSeconds() const;

	/**
	 * 把本段配的完美窗口换算成世界时间区间（供 OnAttackInput 判定）。关 <= 开 表示本段没有完美窗口。
	 * Now 由 StartStage 传进来：连段窗口的基准要用同一个时刻，两边不能各取各的 GetTimeSeconds()。
	 */
	void ComputePerfectWindow(const FThreeHitAttackStage& Stage, float Rate, float Now);

	/** 完美窗口触发时挂强化普攻状态（时长取 PassiveData->EmpowerDuration）。 */
	void GrantEmpoweredAttack();

	/**
	 * 打满最后一段之后播收招蒙太奇（PassiveData->RecoveryMontage，空手四连拳用）。
	 *
	 * ⚠️ 走 ACharacter::PlayAnimMontage 而不是 UAbilityTask_PlayMontageAndWait ——
	 *   调用点在 EndAbility 之前一瞬间，而 task 在能力 OnDestroy 时会 StopAnimMontage，
	 *   用 task 播的收招会跟着能力一起被停掉（症状 =「最后一段打完直接回站立，没有收招」）。
	 *
	 * 没配 RecoveryMontage、或 avatar 不是 ACharacter 时什么都不做（静默跳过，不算错配）。
	 */
	void PlayRecoveryMontage();

	/**
	 * 按本段配置重建「命中通知」的订阅任务。在 StartStage 里调（每段一次）。
	 *
	 * 标签来源：Stage.ImpactTag 优先，留空回退 MeleeImpactTag。
	 * 收掉旧任务再新建 —— 不收也不会出错（OnAnimImpact 有 bHitAppliedThisStage 闩），
	 * 但会白留一个空转的 WaitGameplayEvent 到 EndAbility。
	 *
	 * 为什么挪进 StartStage（原来在 ActivateAbility 里建一个固定的）：
	 * 固定订阅就等于「只有 MeleeImpactTag 生效」，每段独立标签（空手四连拳的
	 * Boxing1~4）会全部失效 —— 表现是四段动画都播了、只有一段掉血。
	 */
	/**
	 * 本段起手的【蓄力表现】—— 粒子 + 镜头，按 Stage 上那几个 Charge* 字段配（留空就是普通攻击）。
	 *
	 * 在 StartStage 里、蒙太奇播放【之前】调：起手这一帧就要看到火光，不能等动画跑起来。
	 * 慢放（Stage.ChargeTimeDilation）【不在这里做】，它是并进本段 Rate 的（见 StartStage 那段注释）——
	 * 拆开来的理由：Rate 一变，命中/窗口/播完时刻全跟着变（同一个数）；
	 * 单独去设 Actor 的 CustomTimeDilation 只会让动画变慢、定时器照旧跑。
	 *
	 * 两端各跑各的：客户端预测那一次起手和服务端收到消息那一次起手都会调，
	 * 攒下来的 Niagara 组件由 EndAbility 统一拆（ChargeDuration 那个定时器也算一层兜底）。
	 */
	void StartStageCharge(const FThreeHitAttackStage& Stage, float TimeDilation);

	/** 收掉蓄力表现（粒子 + 镜头）。ChargeDuration 到点、下一段起手、EndAbility 都会调。 */
	void StopChargeVFX();

	void RebuildImpactTask(const FThreeHitAttackStage& Stage);

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
	void PlayAttackSound(bool bEmpowered, int32 InStageIndex);

	/**
	 * 本能力现在打的是【空手形态】（State.Form.Unarmed）还是持剑形态。
	 *
	 * 两套连招（持剑三段 / 空手四段）共用同一个父类和同一个 GameplayCue.MeleeHit，
	 * 音效事件要靠这条判据分叉：空手走 Audio.*Boxing 那两条，持剑走基础那两条。
	 *
	 * 读的是 ASC 上的标签而不是「这个能力是哪个子类」—— 形态是角色身上的状态，
	 * 拿 IsA(UGA_BoxingCombo) 判的话，将来加第三种形态（比如持匕首）就得再改一遍这里，
	 * 而读标签对任何「这段连招是在空手下打出来的」情况都成立（包括空中、电刑之类）。
	 *
	 * ⚠️ 切形态的 GE 复制到客户端有一帧延迟（和 GA_AirAttack 选蒙太奇同一个坑）：
	 * 刚切完形态的头一帧可能还按旧形态取音。挥击音在起手时播，体感上察觉不到。
	 */
	bool IsUnarmedForm() const;

	/** 取角色网格（不是 Character / 没有 mesh 时返回 null）。 */
	USkeletalMeshComponent* GetAvatarMesh() const;

	/**
	 * 强化那一击命中一个目标时，走 GameplayCue.EmpoweredHit 在命中点放额外效果。
	 * 必须走 cue：ApplyServerHit 只在服务端跑，直接 Spawn 的话粒子只有主机看得到。
	 */
	void ExecuteEmpoweredHitCue(const FHitResult& Hit) const;

	/**
	 * 每一击命中一个目标时的基础打击表现：命中点粒子 + 打击音（+ 本地镜头振动），走 GameplayCue.MeleeHit。
	 * 和 ExecuteEmpoweredHitCue 一样必须走 cue：ApplyServerHit 只在服务端跑，
	 * 直接 Spawn 的话粒子只有主机看得到。
	 *
	 * CueWeight 是【本段的打击分量】（Stage.HitCueWeight，0 = 按 1 算），
	 * 塞进 CueParams.NormalizedMagnitude 传给 UGC_MeleeHit，由它乘在冲击波环缩放和镜头振动强度上。
	 * 为什么用 NormalizedMagnitude 这条通道：cue 的参数里只有它是「这个 cue 有多重」的语义，
	 * 而且项目里没有别的地方用它（GameplayCue 的时长/强度在 GE 上，不在直接 Execute 的 cue 上）。
	 *
	 * StageIndex（0 起）走 RawMagnitude。段号必须一起传：三连击共用一个 GameplayCue.MeleeHit，
	 * 而 FGameplayCueParameters 里没有一个「往下游传音效」的槽位，命中音只能由 UGC_MeleeHit
	 * 自己按段号来查表。三下都查同一句的话，连击听起来就是同一声重复播三遍。
	 */
	void ExecuteMeleeHitCue(const FHitResult& Hit, float CueWeight, int32 InStageIndex) const;

	/**
	 * 本段命中那一下的顿帧（全世界慢下来）。只在服务端调，且只在 Stage.bHitStopOnImpact 时才做。
	 *
	 * 写法和 UGA_DeathHarvest::BeginHitStop 同一套（那个是大招上的顿帧，已经跑通了）：
	 * 存原始 TimeDilation → SetGlobalTimeDilation → 排一个【按膨胀后世界秒折算】的还原定时器。
	 * 不能直接存 GetEffectiveTimeDilation()：它把 CinematicTimeDilation 那一路也乘了进去，
	 * 拿它回写等于把那层缩放永久烙进世界时间。
	 */
	void BeginImpactHitStop(const FThreeHitAttackStage& Stage);

	/** 还原世界流速。幂等：EndAbility 里还会兜底再调一次。 */
	void EndHitStop();

	/** 把「真实秒」折算成当前流速下的世界秒（世界计时器读的是膨胀后的 delta）。 */
	float ScaledWorldSeconds(float RealSeconds) const;

	/** 当前正在播的提示光（没有就是 null）。 */
	UPROPERTY(Transient) TObjectPtr<UNiagaraComponent> PerfectWindowVFX;

	/** 本段起手时生成的蓄力特效（挂在某只手上的插槽，跟着角色走）。没有就是 null。 */
	UPROPERTY(Transient) TObjectPtr<UNiagaraComponent> ChargeVFX;

	/** 蓄力特效的自动拆除定时器（Stage.ChargeDuration <= 0 时不排）。 */
	FTimerHandle ChargeVFXStopTimer;

	FTimerHandle PerfectVFXOpenTimer;
	FTimerHandle PerfectVFXCloseTimer;
	FTimerHandle PerfectVFXTickTimer;

	/** 提示光是不是正在跑。也是每帧自续的那个定时器的停止开关。 */
	bool bPerfectWindowVFXActive = false;

	/** 「NS / 音效没配」的警告各只打一次，避免每次连段都刷屏（和 GC_EmpoweredAttack 同一个套路）。 */
	bool bLoggedMissingPerfectWindowVFX = false;
	bool bLoggedMissingPerfectWindowSound = false;
	bool bLoggedMissingAttackSound = false;

	// （AttackInputTag 已移到上面的 protected 段 —— UGA_BoxingCombo 要继承并改写它。）

	// 伤害值的 SetByCaller 标签不在这里配了：伤害统一由 GE_Damage + UExecCalc_Damage 结算，
	// 本能力只负责喂 Data.DamageMultiplier / Data.FlatDamage（见 GAS_Block_Setup.md §3.6）。
	TObjectPtr<UAbilityTask_WaitGameplayEvent> InputTask;
	TObjectPtr<UAbilityTask_WaitGameplayEvent> ImpactTask;
	FTimerHandle HitTimer, OpenTimer, CloseTimer;

	/** 顿帧的还原定时器（BeginImpactHitStop 排、EndHitStop 清）。 */
	FTimerHandle HitStopTimer;

	/** 顿帧前世界的原始 TimeDilation（不是 GetEffectiveTimeDilation，理由见 BeginImpactHitStop）。 */
	float SavedWorldTimeDilation = 1.f;

	/** 顿帧是不是还挂着。幂等还原 + 防止两次命中叠两层顿帧。 */
	bool bHitStopApplied = false;
	/** 最后一段的推迟收尾定时器（ScheduleLastStageFinish 排、EndAbility 清）。 */
	FTimerHandle FinishTimer;
	/** 完美窗口挂/摘 State.PerfectWindow* 的两个定时器（SchedulePerfectWindowTags 排、EndAbility 清）。 */
	FTimerHandle PerfectTagOpenTimer;
	FTimerHandle PerfectTagCloseTimer;
	int32 StageIndex = INDEX_NONE;

	/**
	 * 本段蒙太奇播完的世界时刻（StartStage 里算，按攻速换算）。
	 * 只给最后一段的收尾用：收招必须等出招演完，不能叠在半截出拳上。
	 * 没播蒙太奇时为 0。
	 */
	float StageMontageEndWorldTime = 0.f;

	/**
	 * 本段是否已经结算过命中。命中时刻的来源在 StartStage 里就定死了（通知 / HitTime 定时器二选一），
	 * 这个闩只是防御性的「一段只结算一次」。每段起手时在 StartStage 里清掉。
	 */
	bool bHitAppliedThisStage = false;

	/**
	 * 「本段是不是强化普攻」。在 StartStage 起手时从 State.EmpoweredAttack 现读一次并锁住，
	 * 命中结算（ApplyServerHit）用这个值而不是再读标签 —— 因为强化在起手那一刻就被消耗掉了（用掉即没），
	 * 到命中结算时标签已经没了。客户端/服务端各自锁各自的，两边起手都会跑到 StartStage。
	 */
	bool bStageEmpowered = false;

	bool bWindowOpen = false;
	bool bQueuedNextStage = false;

	/**
	 * 把「连段窗口开着」这件事挂成 loose 标签 State.ComboWindow（bOpen=false 就摘）。
	 *
	 * 【为什么是标签而不是给技能加个 getter】读者在技能外面（AArenaBotController），
	 * 而且它手上只有 Pawn / ASC —— 拿标签是现成的（HasMatchingGameplayTag），
	 * 拿技能实例要走 FindAbilitySpecFromClass 再 Cast，还得处理"技能没在跑"的空指针。
	 * 和 GA_Dodge 把派生窗口挂成 State.Dodge.Active 是同一套路子，见 LOLGameplayTags.h。
	 *
	 * 幂等：AddLooseGameplayTag 重复挂、RemoveLooseGameplayTag 没挂过时都是 no-op，
	 * 所以每个"窗口状态变了"的地方（含 EndAbility 兜底）都可以无条件调，不用自己记状态。
	 */
	void SetComboWindowTag(bool bOpen);

	/**
	 * 把完美窗口也挂成 loose 标签：State.PerfectWindowArmed（本段有窗口、还没结束）+
	 * State.PerfectWindow（窗口正开着）。两个都是幂等的，无条件调。
	 *
	 * 【为什么要两个】"要不要等"和"现在按"是两件事：只有第 1 段（剑套）配了完美窗口，
	 * 其余段一个都没有 —— 只有一个标签的话 AI 分不清"等一等就有"和"永远不会有"。
	 * 见 LOLGameplayTags.h 里两个标签各自的注释。
	 */
	void SetPerfectWindowTags(bool bArmed, bool bOpen);

	/** 完美窗口到点：挂 State.PerfectWindow。 */
	void OpenPerfectWindow();

	/** 完美窗口到点收：摘 State.PerfectWindow 和 State.PerfectWindowArmed。 */
	void ClosePerfectWindow();

	/**
	 * 排两个定时器把完美窗口挂/摘成标签。时刻直接用 ComputePerfectWindow 算好的那两个世界时间，
	 * 和提示光的定时器同一个来源。
	 *
	 * 【为什么不塞进 SchedulePerfectWindowVFX】那个函数在专用服务器 / 提示光资产没配时会提前返回
	 * （没渲染就没表现），而判定窗口是玩法状态、必须照挂 —— 表现没了不能连 AI 的依据也没了。
	 */
	void SchedulePerfectWindowTags();

	/**
	 * 本段完美窗口的 [开, 关]（世界时间，已按攻速把蒙太奇秒换算过来）。
	 * 关 <= 开 表示本段没有完美窗口（没勾标记 / 没配时间 / 已经是最后一段 / 和连段窗口没交集）。
	 */
	float PerfectWindowOpenWorldTime = 0.f;
	float PerfectWindowCloseWorldTime = 0.f;

	/**
	 * 本段连段窗口的 [开, 关]（世界时间，已按攻速把蒙太奇秒换算过来）。
	 * 由 OnAttackInput 的输入缓冲判定用：窗口没开但离它不到 ChainInputBufferTime 的那一下也算数。
	 * 和三个定时器共用同一套换算（同一个 Now、同一个 Rate）。
	 */
	float ChainWindowOpenWorldTime = 0.f;
	float ChainWindowCloseWorldTime = 0.f;

	/** 连段窗口内的第一次输入有没有踩中完美窗口 —— 只认第一次，见 OnAttackInput。 */
	bool bPerfectQueued = false;

	/**
	 * 起手那一按的「吞一次」闩。
	 *
	 * 激活能力的那一次按键，客户端和服务端都会各发一遍 Event.Input.BasicAttack
	 * （见 AHeroCombatCharacter::RouteBasicAttackInput），它和本段起手落在同一帧 →
	 * 会被 OnAttackInput 的输入缓冲当成「窗口开之前就按了」收下 → 一次按键连出两段。
	 *
	 * 只有第 0 段有这一按（后面几段是被排队推进来的，没有对应的新按键），所以在 ActivateAbility
	 * 里起手后置位；收到这次输入时清位，连段窗口打开时兜底也清一次（万一那次输入没到，
	 * 不能把玩家真的按键吞掉）。
	 */
	bool bSwallowOpeningInput = false;
};
