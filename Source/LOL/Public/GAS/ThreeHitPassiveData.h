// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
// FGameplayTag ImpactTag（每段命中标签）要用它。UHT 解析 UPROPERTY 时需要【完整类型】
// （要取它的 sizeof/Serialize），只靠 CoreMinimal 传递进来的前置声明会报
// 「'ImpactTag' : unknown override specifier」这种完全对不上号的错。
#include "GameplayTagContainer.h"
// Stage.ChargeCameraShake 用的是 UCameraShakeBase，UHT 解析 UPROPERTY 时要【完整类型】
// （和上面 GameplayTagContainer 一个道理，只有前置声明会报莫名其妙的错）。
#include "Camera/CameraShakeBase.h"
#include "ThreeHitPassiveData.generated.h"
class UAnimMontage;
class UGameplayEffect;
class UNiagaraSystem;
class USoundBase;
class UCameraShakeBase;

USTRUCT(BlueprintType)
struct FThreeHitAttackStage
{
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TObjectPtr<UAnimMontage> Montage;

	/**
	 * 本段命中通知的标签（蒙太奇上那个 UAnimNotify_SendGameplayEvent 填同一个）。
	 * 留空 = 用能力上的 MeleeImpactTag（三连普攻的旧行为，3 段共用一个 Event.Melee.Impact）。
	 *
	 * 为什么留「每段一个」的选项：连段推进本身靠【阶段索引】区分（StartStage 里换订阅），
	 * 所以严格说一个标签就够。但每段独立标签有两个实际好处：
	 *   ① 一次按键打两段时（两个能力同时激活等异常场景），日志能直接看出是哪一段结算的；
	 *   ② 允许「同一段挂两种用途不同的通知」——比如伤害帧和击飞帧用不同标签，
	 *      配在同一个 Stage 里是不可能的。
	 * 空手四连拳用的是每段一个（Event.Melee.Boxing1~4）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Tags") FGameplayTag ImpactTag;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0")) float DamageMultiplier = 1.f;

	/**
	 * 本段蒙太奇的【额外】播放倍率 —— 在攻速倍率之上再乘一次。
	 * 1 = 跟攻速走（默认，和加这个字段之前完全一样）；1.3 = 这一段比同攻速下的其它段快 30%。
	 * 想让四连拳里的某一段（比如右腿那段 1.667s 的 MM_Attack_03）单独提速，就在这儿填。
	 *
	 * 【为什么放在 Stage 上，而不是改蒙太奇资产自己的 Rate Scale】
	 * 蒙太奇的 RateScale 确实生效，但只在播放那一侧：
	 *   AnimMontage.cpp → `PlayRate = MontageInstance->PlayRate * Montage->RateScale`
	 * 而能力里所有「蒙太奇秒 → 世界秒」的换算（HitTime / ChainWindowOpen/Close / PerfectWindow）
	 * 用的都是 GetAttackPlayRate()，它【看不到】资产上的 RateScale ⇒
	 * 改 RateScale 会让动画变快、但连段窗口仍按旧速率换算 ⇒ 窗口相对动画整体【延后】，
	 * 手感变成「拳都打完了还接不上 / 提前按才算数」。
	 * 倍率放在 Stage 上并乘进那个 Rate，播放和换算共用同一个数，两边不会各算各的。
	 *
	 * ⚠️ 命中判定不受影响：四段都配了独立 ImpactTag（Event.Melee.Boxing1~4），
	 *    命中走蒙太奇上的 notify，跟着动画帧走，不依赖 HitTime 定时器。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0.1", ClampMax="4")) float PlayRateMultiplier = 1.f;

	/**
	 * 本段出拳时的前冲距离（cm/s，喂给 LaunchCharacter）。0 = 不前冲。
	 *
	 * 【为什么需要它】源角色 Crunch 的上肢比例是 Kallari 的 1.63 倍
	 *（手到 spine_01 的最远距离：Crunch 94.7cm / Kallari 58.0cm，静态骨架实测）。
	 * 重定向把动作搬过来了，但 Kallari 的手臂物理上伸不到那么远，
	 * 视觉上就是「拳头没打出去」—— 尤其配上重定向动画自带的
	 * 56°~103° 的大臂摆幅，反差很明显。
	 * 加一点前冲，拳头的世界位置就能到该去的距离。
	 *
	 * 【填多少】这是**速度**不是距离：LaunchCharacter 给的是初速度(cm/s)，
	 * 实际位移 ≈ 初速度 × 到命中帧的时间。四段 0.93s 的动画，建议 180~320：
	 * 命中在起手后约 0.15s ⇒ 200cm/s ≈ 前冲 30cm，刚好补上 36cm 的差值的大部分。
	 * ⚠️ 太大会让角色像在滑步（脚步动画和实际位移对不上），先从 220 试。
	 *
	 * 只在服务端施加（位移必须权威），客户端靠位置复制。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="cm/s")) float PunchLunge = 0.f;

	// ---------------------------------------------------------------------
	// 本段命中的【纯冲量击退】（无控制标签，只推一下）
	//
	// 为什么放在【段】上而不是整个 Data 资产一份：连打四段时每一段的推力本来就不该一样 ——
	// 「前三段推一下、第四段（蓄力那一拳）推得最远」才是手感，一份全局数值做不到这个。
	// 留 0 / 留空 = 回退到 UThreeHitPassiveData 上那份全局 HitImpulse*（老资产不用重填），
	// 两边都留 0 = 这一段不推人。
	// ---------------------------------------------------------------------

	/** 本段命中的推力 GE。留空 = 用 PassiveData->HitImpulseGE。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impulse") TSubclassOf<UGameplayEffect> HitImpulseGE;

	/** 本段的水平推力（cm/s）。0 = 回退 PassiveData 那份。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impulse", meta=(ClampMin="0", Units="cm/s")) float HitImpulse = 0.f;

	/** 本段的垂直上抛（cm/s）。0 = 不上抛（= 回退 PassiveData 那份）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impulse", meta=(ClampMin="0", Units="cm/s")) float HitLaunch = 0.f;

	// ---------------------------------------------------------------------
	// 本段命中的【控制】（眩晕）
	//
	// 做成两个可配字段而不是在代码里写死「第 3 段眩晕」：段序是资产里的一行配置，
	// 写死第 3 段的话哪天改段数、或者持刀形态也想在某一段眩晕，就得回头动 C++。
	// StunGE 留空 / StunDuration <= 0 = 这一段不眩晕（默认，平 A 不该每段都把人定住）。
	//
	// ⚠️ 眩晕是硬控：UGE_Stun 授 State.Stunned → 目标挡一切技能 + 取消正在施放的技能。
	//    「平 A 第三段把人定住 0.6 秒」在 LoL 里属于较重的打击感设计，别给每一段都开。
	// ---------------------------------------------------------------------

	/** 本段命中挂的眩晕 GE（UGE_Stun 系：HasDuration + 时长读 Data.ControlDuration）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Control") TSubclassOf<UGameplayEffect> StunGE;

	/** 眩晕时长（秒），填进 SetByCaller Data.ControlDuration。0 = 不眩晕（= 回退到「没配」）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Control", meta=(ClampMin="0", Units="s")) float StunDuration = 0.f;

	// ---------------------------------------------------------------------
	// 本段命中的【打击分量】与【顿帧】
	//
	// 三段轻拳和第四段蓄力拳的命中表现走的是同一个 GameplayCue.MeleeHit，而那个 cue 上的
	// ShockwaveScale / HitCameraShakeScale 是【每个 cue 资产一份】的常量 ——
	// 不额外给一个分量的话，收尾那一记最重的拳和第一下轻拳在屏幕上完全一样重。
	// 分量通过 CueParams.NormalizedMagnitude 传给 UGC_MeleeHit（项目里没有别处用它），
	// 乘在冲击波环的缩放和镜头振动的强度上。
	// ---------------------------------------------------------------------

	/**
	 * 本段命中的打击分量倍率（乘在 cue 的冲击波环缩放 / 镜头振动强度上）。
	 * 0（默认）= 不覆盖，按 1.0 处理 —— 老资产不用重填，行为和加这个字段之前完全一样。
	 * 空手四连拳靠这个把第四段（蓄力拳）推到 2~3 倍：动作大、命中帧晚，表现也该更重。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impact", meta=(ClampMin="0", ClampMax="10")) float HitCueWeight = 0.f;

	/**
	 * 本段【命中那一下】把全世界慢下来（顿帧）。
	 *
	 * 【为什么这个可以开，而普攻平时不能开】见 UGC_MeleeHit 末尾那段：普攻一秒两下，
	 * 每一击都顿帧会让全场一起卡，所以默认全关。但蓄力拳是【一套连段只出一发】，
	 * 一次 0.1s 的顿帧正是收尾该有的分量，代价完全可以接受。
	 *
	 * ⚠️ 只在服务端调（UWorld 的流速由 WorldSettings 复制到全场，客户端自己设会被覆盖回去）。
	 * ⚠️ 世界慢下来时【所有】世界计时器都跟着慢 —— 本段剩下的连段窗口/收尾调度会一起被拉长。
	 *    对收尾那一击这是想要的（顿帧期间接不上就该接不上），但如果哪天要给中间段开，
	 *    记得窗口会跟着变宽。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impact") bool bHitStopOnImpact = false;

	/**
	 * 顿帧时长（秒，【真实时间】不是世界时间）。
	 * 0.06~0.12 比较合适：低于 0.06 肉眼基本看不出来，超过 0.15 会像卡顿而不是打击感。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impact", meta=(ClampMin="0", Units="s")) float HitStopTime = 0.10f;

	/**
	 * 顿帧期间的世界流速。0.35 = 慢到三成五。
	 * 别低于 0.2：再低就接近「停住」，配合时长很容易看着像掉帧/卡死。
	 * WorldSettings 上还有 Min/MaxGlobalTimeDilation 会夹一次（超范围不报错，只是生效的不是填的值），
	 * 实际生效值由能力的日志打出来。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impact", meta=(ClampMin="0.05", ClampMax="1")) float HitStopScale = 0.35f;

	// ---------------------------------------------------------------------
	// 本段【起手蓄力】的表现：粒子 + 镜头 + 时间流速
	//
	// 第四段（蓄力拳）这类「有明显蓄力动作」的段靠这一组字段配：
	//   起手 → 在手上生成蓄力特效 + 改镜头 + 慢放 → 特效到点/下一段起手自动收掉。
	// 全部留空 / 留 1 = 本段就是普通攻击，行为和加这几行之前完全一样（默认 1 就是「不慢」）。
	// ---------------------------------------------------------------------

	/**
	 * 本段起手时在手上生成的蓄力特效（NS_Hand_Charge 那类「能量汇聚到拳头」的紫焰）。
	 * 留空 = 本段没有蓄力粒子。它挂在 ChargeSocket 上、跟着角色走，不是世界坐标。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Charge|VFX") TSoftObjectPtr<UNiagaraSystem> ChargeSystem;

	/** 蓄力特效挂哪个插槽。默认 hand_l（空手攻击那双手充能用的就是这一对插槽）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Charge|VFX") FName ChargeSocket = TEXT("hand_l");

	/**
	 * 蓄力特效的时长（世界秒）。到点自动收掉；留 0 = 不自动收（只有 EndAbility /
	 * 再起一段时才由代码兜底拆掉，适合本身带 auto-destroy 的无限循环 emitter）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Charge|VFX", meta=(ClampMin="0", Units="s")) float ChargeDuration = 0.f;

	/**
	 * 蓄力特效要不要跟着一起慢（和 ChargeTimeDilation 同一个数）。
	 * 关掉（false）的话粒子按原速烧、角色在慢放 —— 只有在 NS 没配成 Tick Delta Time 时才有必要关。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Charge|VFX") bool bChargeSystemSlowsWithTime = true;

	/**
	 * 慢放倍率（1 = 不慢，0.35 = 明显子弹时间）。
	 *
	 * 【它乘进本段的播放速率，不是单独去改 Actor 的 CustomTimeDilation】——
	 * 这一点是时序能不能对的关键：本能力里命中定时器、连段窗口、蒙太奇播完时刻
	 * 全部走同一个 Rate（都是 `蒙太奇秒 / Rate`），把倍率并进 Rate 之后
	 * 「动画变慢 / 判定变慢 / 窗口变长」是同一个数推出来的，慢放期间时序天然同步。
	 * 反过来要是只设 Actor 的 CustomTimeDilation：动画确实慢了，但定时器还按世界秒跑，
	 * 命中会【提前】于动作帧（打在拳还没挥出去的时候）。
	 *
	 * ⚠️ 副作用（想要的也是这个）：慢放会把本段的连段窗口一起拉长 —— 慢放期间接段更宽容。
	 * ⚠️ 必须留够真实时间：0.35 倍速下 0.2s 的蓄力只剩真实 0.06s，肉眼看不见。建议 0.3~0.5s。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Charge|Time", meta=(ClampMin="0.05", ClampMax="1")) float ChargeTimeDilation = 1.f;

	/**
	 * 起手瞬间的镜头（UCameraShakeBase 子类资产，配在 cue 上就能跨端复制）。
	 * 这里配在段上、由能力在本机播：起手这一下要「看自己蓄力」，镜头是【表演】不是【状态】，
	 * 走 cue 反而要等 GE 挂上那一瞬才触发（时机对不上）。和 GC_DeathHarvestBurst 一样
	 * 只震控制者本人那台机器（ClientStartCameraShake）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Charge|Camera") TSoftObjectPtr<UCameraShakeBase> ChargeCameraShake;

	/** 上面那个镜头的强度倍率。0 = 只震一下不位移（很多 shake 资产是纯噪声抖动的用法）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Charge|Camera", meta=(ClampMin="0")) float ChargeCameraShakeScale = 1.f;

	/** Times are in authored montage seconds, before attack-speed play-rate scaling. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float HitTime = .15f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float ChainWindowOpenTime = .25f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float ChainWindowCloseTime = .50f;
	/**
	 * On stage 2, the first input inside the perfect window (PerfectWindow* below, inside the chain
	 * window) arms the next hit as an empowered attack (bonus damage multiplier + knockback, values
	 * from UThreeHitPassiveData::Empower*).  An input elsewhere in the chain window still advances
	 * the combo — the next hit just comes out normal.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) bool bPerfectWindowEnablesNextHitKnockback = false;

	/**
	 * 完美窗口（蒙太奇秒，和上面几个时间同一口径，同样按攻速缩放）。
	 * 只有落在 [开, 关] 内的那一次连段输入才会武装下一段强化；连段窗口内的其余输入照样推进连段，
	 * 只是下一段是普通攻击。两者都留 0（或关 <= 开）表示「没配」→ 回退成整段连段窗口，
	 * 也就是老行为（连段窗口内按键即强化），日志里会打出来，不会静默。
	 * 实际有效区间取它和连段窗口的交集（连段窗口开启前/关闭后的输入本来就不收）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float PerfectWindowOpenTime = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float PerfectWindowCloseTime = 0.f;
};

/** Per-hero tuning asset.  The delivery GE reads Data.Damage via SetByCaller. */
UCLASS(BlueprintType)
class LOL_API UThreeHitPassiveData final : public UDataAsset
{
	GENERATED_BODY()
public:
	UThreeHitPassiveData();
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack") TArray<FThreeHitAttackStage> Stages;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0.01")) float ReferenceAttackSpeed = .658f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0", Units="cm")) float TraceDistance = 180.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0", Units="cm")) float TraceRadius = 55.f;

	/**
	 * 打满【最后一段】之后的收招蒙太奇（空手四连拳 = AM_Combo_Recovery_Kallari，1.467s）。
	 * 留空 = 最后一段播完直接回状态机（Paragon 原版就是这样：Combo_01_Montage 播完 0.933s 就结束）。
	 *
	 * 【为什么不是「最后一段」的一个 Stage】
	 * Recovery 是 1.467s，比攻击段（0.933s）本身还长，塞进 Stages 里的话：
	 *   ① 它会占掉一个连段位（打空一拳才能继续），而它压根不是攻击；
	 *   ② 它没有 ImpactTag，ResolveHit 会因为「本段没配通知」退化成 HitTime 兜底，
	 *      白白打一次伤害判定。
	 * 单独一个字段更贴合语义：它是【收招表演】不是【伤害段】。
	 *
	 * ⚠️ 只在「打满最后一段」时播。中途连段超时（玩家没接上）不播 ——
	 *   否则打一拳就要等 1.5 秒才回状态机，手感会变得很钝。
	 *   被打断（受击/死亡/被击飞）也不播：那时候该由打断动画接管。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack") TObjectPtr<UAnimMontage> RecoveryMontage;

	/**
	 * 连段输入缓冲（蒙太奇秒，和其他几个时间同一口径，同样按攻速缩放）。
	 *
	 * 连段窗口【开之前】这么久以内的按键也算数 —— 但只当作「普通下一段」，不给完美窗口。
	 * 三连击是一段一段手动接的，窗口没开就按的那一下原来是被整段丢掉的（见 OnAttackInput），
	 * 玩家的体感就是「我明明按了却没接上」，尤其是攻速快起来之后窗口只有零点几秒。
	 *
	 * 为什么是蒙太奇秒而不是世界秒：攻速 4 倍时窗口本身只有四分之一长，
	 * 固定世界秒的缓冲会相对变得极长（等于把窗口往前扩了一大截）→ 等于没有门槛。
	 * 除以攻速倍率之后，缓冲和窗口的【比例】恒定，攻速再快也不会烂掉。
	 *
	 * 判定放在窗口开启之前，所以永远不可能算成完美（完美窗口和连段窗口的交集必然不早于窗口开启）。
	 * 填 0 = 关掉缓冲，回到「窗口没开就丢掉」的老行为。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0", Units="s")) float ChainInputBufferTime = .15f;
	/** Instant GE with a SetByCaller magnitude named Data.Damage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage") TSubclassOf<UGameplayEffect> DamageEffect;

	// ---------------------------------------------------------------------
	// 每一击都带的推力（区别于下面 Empower* 那一套「只有强化命中才有」的控制）
	// ---------------------------------------------------------------------

	/**
	 * 每一击命中的【纯冲量】击退 GE（UGE_KnockbackImpulse：Instant、不带任何控制标签、只推一下）。
	 *
	 * 【为什么单独做一个 GE 而不是复用 UGE_Knockback】
	 * UGE_Knockback 是硬控：HasDuration + 授 State.Knockback（挡技能、挡移动、播击退蒙太奇）。
	 * 空手四连拳每一下都带前冲位移（Stage.PunchLunge），按手感应该每一下都把人推开一点，
	 * 但每一下都挂硬控就太强了 —— 平 A 自带软控，连打四段等于连控四次。
	 * 所以这里只要位移，不要标签：推一下就结束，ActiveGameplayEffects 里不留东西。
	 *
	 * 留空 = 普攻不推人（持刀三连击的老行为）。拳击资产 DS_Passive_Boxing 填 UGE_KnockbackImpulse。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impulse") TSubclassOf<UGameplayEffect> HitImpulseGE;

	/** 每一击的水平推力（cm/s）。0 = 不推。建议 150~300：够看出「被打退了半步」，又不至于把人推出攻击范围。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impulse", meta=(ClampMin="0", Units="cm/s")) float HitImpulse = 0.f;

	/** 每一击的垂直上抛（cm/s）。0 = 不上抛。留着给「上勾拳」那种段位单独配。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage|Impulse", meta=(ClampMin="0", Units="cm/s")) float HitLaunch = 0.f;

	// ---------------------------------------------------------------------
	// 强化普攻（破隐 / 三连击完美窗口共用同一套数值）
	// 谁挂 State.EmpoweredAttack 谁决定窗口时长，但强度只在这里配一份。
	// ---------------------------------------------------------------------

	/**
	 * 强化那一击替换 Stage.Montage 播放的蒙太奇。
	 * slot 必须和普攻同槽（UpperBody），否则 AnimBP 里没有对应 slot 节点，播了也看不见姿势。
	 * 留空则强化那一击仍播本段原来的蒙太奇（只是伤害/击退变强）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower") TObjectPtr<UAnimMontage> EmpowerMontage;

	/** 强化命中在 Stage.DamageMultiplier 之上再乘的倍率。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0")) float EmpowerDamageMultiplier = 2.f;

	/** 强化命中的水平击退（沿攻击者朝向，cm/s）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0", Units="cm/s")) float EmpowerKnockback = 800.f;

	/** 强化命中的垂直上抛（cm/s）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0", Units="cm/s")) float EmpowerLaunch = 120.f;

	/**
	 * 强化命中挂的【控制 GE】（可空 = 用上面的击退旧行为）。
	 *
	 * 【为什么做成可配而不是直接改】击退（UGE_Knockback，水平推+硬直）和击飞（UGE_KnockUp，
	 * 垂直升空+硬控到落地）是两种不同强度的控制：持刀形态的完美窗口强化是「把人推开」，
	 * 空手的破隐上勾拳是「把人打上天」—— 两种手感都合理，每个 Data 资产自己选。
	 * 拳击资产（DS_Passive_Boxing）配 UGE_KnockUp = 强化击变成击飞。
	 *
	 * ⚠️ UGE_KnockUp 没有位移组件（只给 State.KnockUp + 升空蒙太奇），上抛那一下由
	 * GA_ThreeHitPassive 在打中时用 EmpowerKnockUpLaunch 自己 LaunchCharacter 补 ——
	 * 和 SpinSlash::LaunchTarget 同一条纪律。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower") TSubclassOf<UGameplayEffect> EmpowerControlGE;

	/** 强化击飞的时长（秒），填进 Data.KnockUpDuration(SetByCaller)。仅在 EmpowerControlGE 配了击飞时有意义。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0", Units="s")) float EmpowerKnockUpDuration = 0.75f;

	/** 强化击飞的上抛冲量（cm/s，纯垂直）。UGE_KnockUp 没有位移组件，由施加方 LaunchCharacter 补。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0", Units="cm/s")) float EmpowerKnockUpLaunch = 700.f;

	/** 三连击完美窗口武装的强化窗口时长（秒），填进 GE 的 SetByCaller Data.EmpowerDuration。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0", Units="s")) float EmpowerDuration = 3.f;

	// ---------------------------------------------------------------------
	// 完美窗口的 QTE 表现（由 UGA_ThreeHitPassive 播）
	// 窗口打开时刀上扫一道光当进度条，踩中窗口时出一声。
	// 放在这个资产上而不是能力类上：NS 和插槽都是每个英雄一份（蒙太奇也在这里）。
	// 而「窗口什么时候开、什么时候关」由能力算（见 ComputePerfectWindow），两边共用同一组时刻，
	// 提示光和判定不会各算各的。
	//
	// 「踩中窗口」和「强化那一击命中」是两回事：
	//   踩中 → 只给本人一声 QTE 反馈（下面的 PerfectWindowSuccessSound），此刻还没有命中信息；
	//   命中 → 才是在目标身上炸开的额外效果，走 GameplayCue.EmpoweredHit（见 UGC_EmpoweredHit），
	//          因为命中结算只在服务端跑，粒子必须在 cue 里生成才能复制到各客户端。
	// ---------------------------------------------------------------------

	/** 完美窗口打开时刀上扫过的那道光（NS_PerfectWindow）。留空 = 不生成提示。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") TSoftObjectPtr<UNiagaraSystem> PerfectWindowSystem;

	/** 提示光的两端：光从 Base 扫到 Tip（Kallari：sword_base_l → sword_tip_l）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") FName PerfectWindowBaseSocket = TEXT("sword_base_l");
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") FName PerfectWindowTipSocket = TEXT("sword_tip_l");

	/**
	 * 踩中完美窗口那一刻的音效（QTE 的「按对了」反馈）。默认 Kallari_Ability_ScoredCrit。
	 * 留空 = 踩中窗口没有任何声音。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") TSoftObjectPtr<USoundBase> PerfectWindowSuccessSound;

	// ---------------------------------------------------------------------
	// 普攻音效（由 UGA_ThreeHitPassive 在每段起手时播）
	// 三连击的蒙太奇里没有配任何 AnimNotify_PlaySound，所以不在这儿补的话普攻是完全没声的。
	// ---------------------------------------------------------------------

	/** 每段普攻起手的挥击音效。默认 Kallari_Effort_Swing。留空 = 普攻无声。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Sound") TSoftObjectPtr<USoundBase> AttackSound;

	/** 强化那一击起手的音效（有配就用它顶掉 AttackSound）。默认 Kallari_Effort_Ability_Primary_Strike。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Sound") TSoftObjectPtr<USoundBase> EmpoweredAttackSound;

	/**
	 * NS_PerfectWindow 按原速播完要多久（秒）—— 也就是光从刀根走到刀尖的标准时长。
	 * 能力拿它和完美窗口的实际长度算一个播放倍率（SetCustomTimeDilation）：窗口比它短就快放、
	 * 比它长就慢放，光正好在窗口关闭那一刻扫到刀尖（进度条的观感全靠这个）。
	 * 填 0 = 不缩放，NS 按原速播。
	 * ⚠️ 只对 Age Update Mode = Tick Delta Time（默认值）的 NS 有效：DesiredAge 模式下
	 * CustomTimeDilation 不参与推进，改了也没反应。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX", meta=(ClampMin="0", Units="s")) float PerfectWindowSystemDuration = 1.f;

	/**
	 * NS_PerfectWindow 的 emitter 勾了 Local Space 就打开：喂进去的坐标会先转成 Niagara 组件的
	 * 局部坐标（组件挂在 mesh 上，所以是角色网格空间），光才会跟着角色走而不是糊在世界里。
	 * 和 NS_BladeTrail 上那个 bLocalSpace 一个意思。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") bool bPerfectWindowSystemLocalSpace = false;

	/** 窗口关闭后等多久强拆提示光。NS 的 emitter 要是设成无限循环，auto destroy 永远等不到。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX", meta=(ClampMin="0.05", Units="s")) float PerfectWindowTeardownDelay = 1.f;
};
