// 近战形态技的共同基类：SpinSlash（轻挥→上挑击飞）和 TurnSlash（转身→回身击退）。
//
// ===========================================================================
// 【为什么抽这个基类】
// 两个技能要做的事完全同构，差别只在「转多少度」和「命中挂什么控制」：
//   ① 锁移动、关自动转向
//   ② 播一段 montage（FullBody slot）
//   ③ 在动画的命中帧上（UAnimNotify_SendGameplayEvent）接住 → 球形扫描 → 每个目标：
//      伤害（GE_Damage + UExecCalc_Damage，SetByCaller 喂参数）
//      + 可选控制 GE（SetByCaller 喂时长/冲量）
//      + 命中 cue（GameplayCue.MeleeHit，多播到各端）
//   ④ 收招：恢复移动/转向、EndAbility
// 不抽基类的话上面这些要在两个文件里各写一遍，改平衡（比如命中半径）要动两个类。
//
// 【判定为什么走动画 notify 而不是球形扫描定时器】
// 和 UGA_ThreeHitPassive 同一个理由：判定时刻跟着【动画帧】走，改攻速/换蒙太奇时
// 不会错位。UAnimNotify_SendGameplayEvent 默认 bServerOnly=true —— 只有服务端会发，
// 而命中结算本来就只该在服务端跑（客户端没有权威的 HitResult）。
//
// 【★ 蒙太奇 slot 的选择】
// 走 AbilityTask_PlayMontageAndWait 并显式指定 FullBody slot，
// 不和 DefaultSlot 那一组（死亡 / 硬控 / 闪避 / 强化普攻）混 ——
// 那一组是「谁都能打断谁」的一组，本组是「独立的一组」。
// 另有一条项目踩过的坑：引擎 Montage_Play / PlayAnimMontage 的第 5 参
// bStopAllMontages 默认【true】＝停掉同体所有其他蒙太奇；同组播蒙太奇必须显式 false。
// PlayMontageAndWait 走的是按 slot 播放，不做这个全局停止。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "Engine/HitResult.h"
#include "GA_FormMelee.generated.h"

class UAnimMontage;
class UGameplayEffect;
class UAbilityTask_PlayMontageAndWait;
class UAbilityTask_WaitGameplayEvent;

/**
 * 近战形态技的【一个伤害段】。
 *
 * 段与段的区别只在「结算什么」，动画时长/转向这些是整条技能共用的
 * （都在 UGA_FormMelee 上）—— 因为两段在**同一条** Montage 里连着播。
 */
USTRUCT(BlueprintType)
struct FFormMeleeStage
{
	GENERATED_BODY()

	/**
	 * 这一段的命中通知标签（蒙太奇上那个 UAnimNotify_SendGameplayEvent 填同一个）。
	 *
	 * ★ 每段必须用**不同**的标签，否则无法区分是哪一段触发了 ——
	 *   而 WaitGameplayEvent 是按标签订阅的，同一个标签发两次只会唤醒一次
	 *   （代码侧有「已结算过就不再结算」的闩，第二次会被吃掉，
	 *   症状是「两段动作播完只有第一下有伤害」）。
	 *
	 * 命名建议：Event.Melee.SpinSlash / Event.Melee.SpinSlash2 …
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Tags")
	FGameplayTag ImpactTag;

	/** 本段用哪个伤害 GE。留空 = 用技能顶层的 DamageGE。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	TSubclassOf<UGameplayEffect> DamageGE;

	/** 本段攻击力倍率。留 -1 = 用技能顶层的 DamageMultiplier。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = "-1"))
	float DamageMultiplier = -1.f;

	/** 本段固定伤害。留 -1 = 用技能顶层的 FlatDamage。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = "-1"))
	float FlatDamage = -1.f;

	/**
	 * 本段命中扫描半径。留 <= 0 = 用技能顶层的 HitRadius。
	 * 两段半径可以不同（比如第一段范围大、第二段是贴身的上挑）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hit", meta = (ClampMin = "0", Units = "cm"))
	float HitRadius = 0.f;

	/**
	 * 本段挂的控制 GE。留空 = 【不加控制】（不是「用顶层的」）。
	 *
	 * ⚠️ 这个「留空 = 不加」和 DamageGE 的「留空 = 用顶层」是**故意相反**的：
	 *   数值类（伤害/半径）填一个「中和值」不自然，留空就该继承；
	 *   而「这一段要不要控制」是一个明确的二元选择，SpinSlash 第 1 段
	 *   要的就是「不挂控制」—— 如果留空会继承顶层的 UGE_KnockUp，
	 *   平砍那一下就也把人挑飞了。所以这里留空的语义必须是「无」。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Control")
	TSubclassOf<UGameplayEffect> ControlGE;

	/** 本段控制时长。留 <= 0 = 用技能顶层的 ControlDuration。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Control", meta = (ClampMin = "0", Units = "s"))
	float ControlDuration = 0.f;

	/** 本段的击退水平冲量。留 0 = 用技能顶层。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Control", meta = (ClampMin = "0"))
	float KnockbackImpulse = 0.f;

	/** 本段的击退上抛冲量。留 0 = 用技能顶层。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Control", meta = (ClampMin = "0"))
	float KnockbackLaunch = 0.f;

	/** 本段的击飞上抛冲量。留 0 = 用技能顶层。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Control", meta = (ClampMin = "0"))
	float KnockUpLaunch = 0.f;

	/**
	 * 本段的命中表现 Cue。留空 = 用技能顶层的 HitCueTag。
	 *
	 * ★ 镜头效果挂在这里的 Cue 上：照 UGC_DeathHarvestBurst 的做法
	 *   （Cue 里配一个 UCameraShakeBase + ClientStartCameraShake），
	 *   而 ClientStartCameraShake 只在施法者本机生效 —— 正是「只有我自己被震」。
	 *   所以「第二段要镜头效果」= 配一个带 CameraShake 的 Cue 填在这里。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "FX")
	FGameplayTag HitCueTag;
};

/**
 * 近战形态技基类。不直接使用 —— 派生 UGA_SpinSlash / UGA_TurnSlash。
 *
 * 子类要做的：填 Montage / ImpactTag / 控制 GE 和数值，以及（TurnSlash）转多少度。
 */
UCLASS(Abstract)
class LOL_API UGA_FormMelee : public UMyGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_FormMelee();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

protected:
	// ---------------------------------------------------------------------
	// 配置（子类在 BP 里填）
	// ---------------------------------------------------------------------

	/**
	 * 本技能的蒙太奇。播在 FullBody slot（见 MyGameplayAbility 槽位约定）。
	 *
	 * ★ 【多段技能也只用一个 Montage】—— 两段动作在【同一条】montage 里，
	 *   靠下面 Stages 里各自的 ImpactTag 区分「哪一段结算了」。
	 *   刻意不做「每段一条 montage」：那样两段之间要自己接续播放，
	 *   而一条里连着播天然同步、也不会在段间被别的蒙太奇插进来。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Anim")
	TObjectPtr<UAnimMontage> Montage;

	/**
	 * 每一个「伤害段」。
	 *
	 * 【为什么是数组而不是一份数值】两段攻击的每段效果可以完全不同 ——
	 *   SpinSlash：第 1 段平砍（普通伤害、无控制），第 2 段上挑（更高伤害 + 击飞）。
	 *   TurnSlash：第 1 段正常挥砍，第 2 段转满 180° 后击退。
	 * 每段各配各的伤害/控制/半径/通知标签，一段只结算一次。
	 *
	 * 【和 UGA_ThreeHitPassive 的 FThreeHitAttackStage 同构】那边是
	 * 「每段一条 montage + 靠输入推进」，这边是「一条 montage 内靠 notify 区分段」——
	 * 前者要玩家手动接段，后者是一气呵成。数值组织的思路一致。
	 *
	 * **只填 1 段 = 单段技能**（旧行为，属性含义完全不变）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Stages")
	TArray<FFormMeleeStage> Stages;

	/**
	 * 默认伤害 GE（Instant，靠 SetByCaller 吃 Data.DamageMultiplier / Data.FlatDamage）。
	 * 某段的 DamageGE 留空时用这个。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Damage")
	TSubclassOf<UGameplayEffect> DamageGE;

	/** 默认攻击力倍率（某段 DamageMultiplier 留 -1 时用这个）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Damage", meta = (ClampMin = "0"))
	float DamageMultiplier = 1.f;

	/** 默认固定伤害（某段 FlatDamage 留 -1 时用这个）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Damage", meta = (ClampMin = "0"))
	float FlatDamage = 0.f;

	/** 命中扫描半径（cm）。球形 —— 近战够用，且不依赖角色朝向。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Hit", meta = (ClampMin = "0", Units = "cm"))
	float HitRadius = 200.f;

	/**
	 * 命中时额外挂的控制 GE（配 UGE_KnockUp / UGE_Knockback / UGE_Stun）。
	 * 留空 = 这一击只加伤害。
	 *
	 * ⚠️ **这是所有段的【兜底值】** —— 每段可以用自己的 ControlGE 覆盖它
	 *   （SpinSlash 就是：第 1 段留空=无控制，第 2 段填 UGE_KnockUp）。
	 *   保留这个顶层字段是为了「只有一个控制 GE、想填一次」的简单场景。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Control")
	TSubclassOf<UGameplayEffect> ControlGE;

	/**
	 * 控制时长（秒），按 ControlGE 需要的键填：
	 *   UGE_Stun      → Data.ControlDuration
	 *   UGE_Knockback → Data.KnockbackDuration
	 *   UGE_KnockUp   → Data.KnockUpDuration
	 * 三个键都填一遍：多余的键被对应的 GE 忽略（它只读自己那个），
	 * 这样换控制 GE 时不用回来改这里。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Control", meta = (ClampMin = "0", Units = "s"))
	float ControlDuration = 0.6f;

	/**
	 * 击退/击飞的水平冲量（cm/s）。UGE_Knockback 的 UGEComponent_Knockback 读它做位移。
	 *
	 * ⚠️ UGE_KnockUp **没有**位移组件 —— 升空那一下的 Z 冲量要施加方自己
	 *   LaunchCharacter（见 LaunchTarget）。所以这个值只对 Knockback 有效。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Control", meta = (ClampMin = "0"))
	float KnockbackImpulse = 900.f;

	/**
	 * 击退的垂直上抛冲量（cm/s）。UGE_Knockback 的 UGEComponent_Knockback 读它
	 *（Data.KnockbackLaunch）⇒ 击退时想要一点浮空就填这里，填 0 = 纯水平推。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Control", meta = (ClampMin = "0"))
	float KnockbackLaunch = 0.f;

	/**
	 * 击飞的垂直上抛冲量（cm/s）。★ 只有 UGE_KnockUp 用得上。
	 *
	 * ⚠️ UGE_KnockUp **没有**位移组件（只有 State.KnockUp + 升空蒙太奇），
	 *   所以升空那一下必须由 UGA_SpinSlash::LaunchTarget 自己 LaunchCharacter，
	 *   用不到这个值的话人只是站着挂 State.KnockUp（看起来是定身，不是击飞）。
	 *
	 * 和 KnockbackImpulse 的分工：击退走 GE 组件读 KnockbackImpulse/KnockbackLaunch；
	 * 击飞走 LaunchTarget 读 KnockUpLaunch（水平分量为 0，纯垂直起跳）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Control", meta = (ClampMin = "0"))
	float KnockUpLaunch = 700.f;

	/** 命中表现。配 GC_MeleeHit（命中点粒子 + 打击音 + 攻击者本地镜头振动）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Melee|FX")
	FGameplayTag HitCueTag;

	// ---------------------------------------------------------------------
	// 子类可覆盖的钩子
	// ---------------------------------------------------------------------

	/**
	 * 起手那一刻锁定的朝向。
	 *
	 * 默认不转（沿用角色当前朝向）。TurnSlash 覆盖它来插值转到背后。
	 * @param OutFacing 返回算出来的朝向
	 * @param Seconds   已经过的时长（TurnSlash 用它算插值进度）
	 */
	virtual void ComputeFacingRotation(float Seconds, FVector& OutLocation, FRotator& OutRotation) const;

	/**
	 * 控制 GE 需要额外由施加方做的位移。
	 *
	 * 击飞为什么特殊：UGE_Knockback 自带 UGEComponent_Knockback 会 LaunchCharacter，
	 * 而 UGE_KnockUp **只有状态没有位移**（GE_KnockUp.h 里写明了「位移不在这里做」）。
	 * 所以升空那一下必须由施加方自己 LaunchCharacter，否则目标只是站着挂 State.KnockUp
	 * —— 看起来像「被定住了」而不是「被打飞了」。
	 *
	 * ★ 传的是本段的冲量（Stage 解析后的值），不是顶层的 —— 上挑那一段
	 *   和平砍那一段的 KnockUpLaunch 可以在 BP 里配不同。
	 */
	virtual void LaunchTarget(AActor* Target, const FHitResult& Hit, const FFormMeleeStage& Stage);

	/** 本次激活要转多少度（0 = 不转）。TurnSlash 覆盖。 */
	virtual float GetTurnDegrees() const { return 0.f; }

	/**
	 * 本次激活要不要锁移动（关掉移动组件）。
	 *
	 * 地面近战一律要锁：出手时人定住，伤害判定和视觉才对得上。
	 * 但空中绝对不能锁 —— ApplyStanceLock 会 StopMovementImmediately + DisableMovement，
	 * 在空中那等于**把角色从跳跃轨迹上拽下来悬停**（症状：空中按一下攻击，人就定在半空）。
	 * 所以 UGA_AirAttack 覆盖它返回 false。
	 *
	 * 默认 true = 地面技能的原有行为，一个字都不用改。
	 */
	virtual bool ShouldLockMovement() const { return true; }

	/** 转到位所用的时长（秒）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Anim", meta = (ClampMin = "0", Units = "s"))
	float TurnDuration = 0.4f;

	/** 本次激活锁定的目标（起手时按「离角色最近的敌人」选的）。 */
	TWeakObjectPtr<AActor> LockedTarget;

	/**
	 * 起手时选中的目标是否在【背后】。TurnSlash 靠它决定转 180 还是 0，
	 * SpinSlash 不用（始终面对目标）。
	 */
	bool bTargetBehind = false;

private:
	/** 蒙太奇上的命中通知到了（只有服务端会发）。按标签认出是哪一段。 */
	UFUNCTION()
	void OnAnimImpact(FGameplayEventData Payload);

	/**
	 * 收招：蒙太奇播完 → 结束技能。
	 *
	 * ★ 必须是一个【无参】的 UFUNCTION，不能直接绑 EndAbility ——
	 *   EndAbility 的签名带 4 个参数，和 FPlayMontageDelegateDynamic（无参）不匹配。
	 *   绑错能编译过但运行时不触发（委托签名不兼容时 UE 的 AddDynamic 是静默的空操作），
	 *   症状是「蒙太奇播完了技能不结束、人物一直锁着不能动」。
	 */
	UFUNCTION()
	void OnMontageFinished();

	/** 逐帧把朝向从起点插到终点（TurnSlash 转身）。 */
	void TickTurn();
	void ApplyTurnRotation();

	/** 对一个目标结算：伤害 + 控制 + cue。Stage 是本段的配置。 */
	void ApplyServerHit(AActor* Target, const FHitResult& Hit, const FFormMeleeStage& Stage);

	/**
	 * 把一段的配置和顶层默认值【合并】成一份可直接用的数值。
	 *
	 * 合并规则（每一条都在 FFormMeleeStage 的字段注释里写明了「留 X = 用顶层」）：
	 *   伤害类（DamageGE / 倍率 / 固定伤害 / 半径）留空或负值 = 继承顶层；
	 *   控制类【ControlGE】留空 = 不加控制（故意不继承，见该字段注释）；
	 *   时长/冲量留 <= 0 = 继承顶层。
	 *
	 * 为什么要合并而不是直接在 ApplyServerHit 里到处写 if：
	 *   LaunchTarget 是虚函数、子类要读 Stage 里的冲量，合并成一份结构体
	 *   子类就只管取字段，不用知道哪条是继承的。
	 */
	FFormMeleeStage ResolveStage(int32 StageIndex) const;

	/** 起手时选目标：HitRadius 内、活着的敌方 Pawn 里离角色最近的那个。 */
	AActor* FindTarget() const;

	/** 锁移动 + 关自动转向。收尾在 EndAbility。 */
	void ApplyStanceLock(bool bLock);

	UPROPERTY(Transient) TObjectPtr<UAbilityTask_PlayMontageAndWait> MontageTask;
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitGameplayEvent> ImpactTask;

	/**
	 * 转向的逐帧 timer。EndAbility 里必须 ClearTimer ——
	 * 被打断时那是唯一能走到的地方，漏清的话这个 0.016s 的 timer 会一直转到 World 销毁，
	 * 而它回调里还在 SetActorRotation（表现为「死亡后尸体还在缓慢转向」）。
	 */
	FTimerHandle TurnTimerHandle;

	/**
	 * 本次命中是否已结算。
	 *
	 * ⚠️ 现在是 **TSet<段下标>** 而不是单个 bool —— 多段技能每段各结算一次，
	 *   这个集合的作用变成「防御性的重复触发去重」（一条 montage 上
	 *   同一个标签可能被放多个 notify）。
	 */
	TSet<int32> HitAppliedStages;

	/** 转向：起点 / 终点 / 已用时长。 */
	FRotator TurnStartRotation = FRotator::ZeroRotator;
	FRotator TurnTargetRotation = FRotator::ZeroRotator;
	float TurnElapsed = 0.f;
	bool bTurning = false;

	/** 关自动转向前的原值（只在真关过时恢复）。 */
	bool bWasOrientRotationToMovement = true;
	bool bOrientRotationOverridden = false;

	/** 锁移动前的模式。 */
	EMovementMode SavedMovementMode = MOVE_Walking;
	bool bMovementLocked = false;
};
