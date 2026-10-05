// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "GAS/MyGameplayAbility.h"
#include "GA_Dodge.generated.h"

class UAbilityTask_WaitGameplayEvent;
class UAnimMontage;
class UGameplayEffect;

/**
 * W 技能「闪避 + 派生 JumpKick」。
 *
 * 流程：
 *   按 W → jet deploy 蒙太奇（跃起前启动动作）→ 喷射粒子 cue → 按【移动输入方向】跃向空中 evade
 *        → 打开一个短暂的「派生窗口」（挂本地标签 State.Dodge.Active）
 *   窗口内按普攻 → 左键被输入层拦截、改派生成 JumpKick（发 Event.Input.DodgeKick）
 *        → 踢向准心方向，带一点索敌容差（附近有敌人允许小幅偏差，锁定时吸附、不扩大命中范围）
 *   隐身中 dodge → 破隐（bBreaksStealthOnCast），强化保留给下一次普通普攻；踢本身【不】强化
 *
 * 位移和伤害：evade 位移、踢的伤害/击退都只在权威端结算，客户端预测蒙太奇和喷射表现。
 *
 * 【闪避表现走蒙太奇】Fwd / Bwd 各一条完整蒙太奇（Start/Mid/End 三段已烘进去），
 * 走 AnimGraph 的 DefaultSlot。
 *
 * 曾经试过让 UHeroEvadeAnimDriver 反射驱动 BS_Evade 做无缝过渡，已弃用 —— ABP 里的
 * AnimGraphNode_BlendSpaceGraph_1 是个空壳（BlendSpace=None，指向的资产已丢），
 * 它会把一个未定义姿势混进最终输出，表现为抽搐 / 偶尔播错片段。组件本身留着，
 * 将来若要重做 BS 可以直接捡回来。
 *
 * 踢的结算时机是【到达即结算】，不是固定时长：飞行中每 KickHitCheckInterval 检查一次
 * 「和目标胶囊表面的距离」，够近（≤ KickHitMargin）才结算；超时 KickMaxFlightTime 还没够到
 * （被墙挡了 / 目标闪现跑了 / 目标死了）按【扑空】收尾 —— 收拖尾、结束能力，不结算伤害。
 * 为什么不能用一个固定时长：射程 300~1200cm 之间变，而 D = I·t + ½g·t² 三个量只能定两个，
 * 定死 I(1500) 和 t 就必然让中远距离「隔空打人」。
 *
 * 【落地也是收尾】落地代表这一脚飞完了，所以 OnLanded 里必判一次：够进 KickLandingMargin
 * 算踢中，否则算扑空。两条路都会打日志 —— 漏掉这一步就会出现既没伤害、也没有任何日志的静默丢击。
 */
UCLASS(Blueprintable)
class LOL_API UGA_Dodge : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_Dodge();
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	// ---------------------------------------------------------------------
	// 闪避
	// ---------------------------------------------------------------------
	/** 闪避水平冲量（cm/s 量级）。和 EvadeVerticalImpulse 合成「跃向空中 + 方向位移」，数值越大水平跃得越远。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0"))
	float EvadeHorizontalImpulse = 600.f;

	/** 闪避跃起的竖直冲量（cm/s 量级）。数值越大跃得越高。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0"))
	float EvadeVerticalImpulse = 500.f;

	/** 向前闪避蒙太奇（Evade_Fwd Start/Mid/End）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge")
	TObjectPtr<UAnimMontage> EvadeForwardMontage;

	/** 向后闪避蒙太奇（Evade_Bwd Start/Mid/End）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge")
	TObjectPtr<UAnimMontage> EvadeBackwardMontage;

	/**
	 * jet deploy 蒙太奇（跃起前的启动动作）。可空。
	 * 它长 1.7s 且走 DefaultSlot，紧随其后的 evade 蒙太奇会以 0.25s 淡出把它顶掉。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge")
	TObjectPtr<UAnimMontage> JetDeployMontage;

	/**
	 * 空中派生窗口的【兜底上限】（秒）。正常由「落地」关闭（LandedDelegate → OnLanded）。
	 * 跃起后在空中按普攻 = 向下踢，落地后窗口关闭、普攻恢复正常。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0", Units="s"))
	float KickWindowDuration = 3.f;

	/**
	 * 二段 evade 的冲量倍率（比第一段略强一点）。窗口内按空格派生，一次闪避最多补一段。
	 * 1.0 = 和第一段一样，>1 = 略强。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0"))
	float SecondDodgeRangeMultiplier = 1.2f;

	// ---------------------------------------------------------------------
	// 完美闪避（无敌窗口）
	// ---------------------------------------------------------------------
	/**
	 * 无敌窗口时长（秒），从闪避起手算起。窗口内挨到的伤害被完全吃掉，并且算【完美闪避】
	 *（回蓝 + 子弹时间 + 镜头推近，见 UDodgeComponent）。0 = 关掉，闪避不带无敌。
	 *
	 * 默认 0.35：只覆盖跃起那一下。窗口必须【短于】整个闪避 —— 无敌帧比动作还长的话，
	 * 闪避就从「躲开」变成了「免伤技能」，和它的 10 秒冷却对不上。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge|Perfect", meta=(ClampMin="0", Units="s"))
	float PerfectDodgeWindow = 0.35f;

	/** 无敌窗口 GE。默认 UGE_DodgeWindow（授予会复制的 State.Dodge.Window）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge|Perfect")
	TSubclassOf<UGameplayEffect> DodgeWindowEffect;

	// ---------------------------------------------------------------------
	// JumpKick
	// ---------------------------------------------------------------------
	/** JumpKick 蒙太奇（可空）。Kallari 的 JumpKick 是 Start/Loop 序列，需先包成蒙太奇。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick")
	TObjectPtr<UAnimMontage> KickMontage;

	/**
	 * 向下踢的【贴地】索敌半径（cm）：站在地上时索敌球半径就是这个值。
	 * 空中会按「准心容差锥打到地面的距离」自动延长（见 KickAcquireRange），
	 * 因为角色能被两段 evade 抬到 10m 高空，固定 300 在那里必然锁不到人。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="cm"))
	float KickRange = 300.f;

	/**
	 * 索敌球在【容差锥方向】上的补偿余量（cm）：把「正下方那片地面」圈进索敌球时多算出来的半径，
	 * 大约是半个身位 —— 目标贴着地面站时，角色的球心到它的距离比到地面还多一点。
	 * 它不参与命中判定（命中用 CapsuleSurfaceDistance 的胶囊表面距离）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="cm"))
	float KickRadius = 90.f;

	/**
	 * 空中索敌长度的【上限】（cm）—— 也是索敌球的半径。
	 *
	 * 索敌是「以角色为中心、半径 = 这个长度（贴地时是 KickRange）的球重叠」，
	 * 所以这个值就是「最远能锁到多远的人」。
	 *
	 * 贴地 = KickRange（地面手感不变）；空中 = 离地高度 / cos(索敌容差) + KickRadius，
	 * 夹在 [KickRange, 这个值] 之间 —— 让高空飞踢也能覆盖到下方的敌人。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="cm"))
	float KickAcquireRange = 1200.f;

	/**
	 * 索敌容差（度）：【准心方向】这个半角锥内的敌人才有资格被锁定（吸附）。
	 * 是【锁敌时允许偏差】，不是扩大命中范围（命中半径还是 KickRadius）。
	 *
	 * ⚠️ 这个容差必须作用在【候选集】上（球重叠捞出来的所有人），不能只在
	 * 「已经扫中的东西」上过滤 —— 否则它形同虚设，见 KickTargetAngleWeight 的注释。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="deg"))
	float KickAimAssistAngle = 30.f;

	/**
	 * 索敌排序时【距离 / 角度】的权衡系数。
	 *
	 * 容差锥里可能有多个候选，打分 = 距离 × (1 + 角度/容差 × 这个系数)，取最小者：
	 *   0 = 只看距离（最近的赢，哪怕它在锥的边缘）
	 *   1 = 锥边缘的候选要「近一倍」才能赢正中间的候选（默认）
	 * 越大越「认准心」。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0"))
	float KickTargetAngleWeight = 1.f;

	/** 踢的伤害倍率（乘攻击力）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0"))
	float KickDamageMultiplier = 1.2f;

	/**
	 * 踢中后【水平】把目标推出去的冲量（cm/s），作为 Data.KnockbackImpulse 填给 KnockbackGE。
	 *
	 * 单位是 cm/s，可以直接换算位移：
	 *   · 在空中被打中（KnockbackLaunch=0）→ 速度被 FallingLateralFriction(默认 0) 保留，
	 *     约等于整个硬直时长都在以这个速度平移，位移 ≈ KickKnockback × 硬直时长。
	 *   · 站在地上被打中（KnockbackLaunch>0）→ 地面 GroundFriction=8 会很快吃掉水平速度，
	 *     实际位移小得多，这时候主要看竖直的弹起感。
	 * 所以「砸下去」和「推出去」是两笔账，改一个别指望另一个跟着变。
	 *
	 * 方向不是这里定的，由 UGEComponent_Knockback 按命中法线/相对位置算（见那个类的注释）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="cm/s"))
	float KickKnockbackHorizontal = 600.f;

	/**
	 * 踢中后【竖直】冲量（cm/s），作为 Data.KnockbackLaunch 填给 KnockbackGE。
	 *
	 * 【正负决定"砸"还是"抛"】组件里是 UpVector * 这个值：
	 *   · 正数 = 往上弹。目标站着被打中会跳一下；空中被打中会先被顶高再落下。
	 *   · 负数 = 往下砸。这个值不能超过「目标离地高度对应的落地速度」，否则目标会先穿到地下
	 *     再被地面弹回来（看起来像穿地诈尸）。
	 *
	 * 常用换算（GravityScale=1.0、世界 GravityZ=-980）：
	 *   从高度 H 自由落体到地面的速度 ≈ 44.3 × √H（cm/s）。H=200cm 时约 626、H=800cm 时约 1253。
	 *   ⇒ 想"把目标砸到地上"填 -(那个数) 附近即可；填小了就是「没砸下去」。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick")
	float KickKnockbackVertical = -600.f;

	/**
	 * 下砸击退的【高度加成上限】（倍率，1 = 关闭）。
	 *
	 * 实际倍率 = clamp(攻击者落地速度 / KickDiveSpeed, 1, 这个值)：
	 *   贴地铲一脚 → 1.0（用 KickKnockbackVertical 原值）
	 *   从 2 倍 KickDiveSpeed 的高度砸下来 → 2.0（击退更狠）
	 * 下限固定为 1 而不是 0：低空不该比配置值更弱，配置值就是"最低强度"。
	 *
	 * 这是让"高度"最终【加到力量上】而不是减掉力量的最后一环 ——
	 * 前面把飞行冲量拆成水平/竖直保证了下砸速度恒定，这里再让击退强度随之放大。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="1"))
	float KickKnockbackHeightBonusMax = 2.f;

	// ---------------------------------------------------------------------
	// 飞行模型：竖直（下砸）与水平（前进）是【两个互相独立的量】
	//
	// 【为什么不写成 KickDir × 一个冲量】这是「高处的飞踢反而看着慢、力量弱」的根因。
	// KickDir 是单位向量，水平分量 = cos(pitch)。高空飞踢的 pitch 会到 73°~79°，
	// 于是水平速度只剩 29%~18% —— 鸟瞰视角下【几乎看不到任何水平位移】，看起来
	// 就是「直直地慢慢掉下去」，而不是「扑过去砸下来」。
	//   高差    pitch   水平速度(原模型)
	//     0cm    0.0°     1500 (100%)
	//   200cm   53.1°      900 ( 60%)
	//   500cm   73.3°      431 ( 29%)
	//   800cm   79.4°      276 ( 18%)
	// 而且耗时也在变长（净空 150cm 用 0.10s → 高空 900cm 用 0.47s，慢 5 倍）：
	// 竖直那段是纯匀加速滑行。两个效应叠加，越高的飞踢越像"飘"，调大 KickFlyImpulse
	// 救不了（那会让低空变得离谱）。
	//
	// 【拆开之后】竖直只负责"以多快的速度砸到地面"，水平只负责"以多快的速度扑过去"，
	// 两者都不再随 pitch 衰减。竖直分量吃掉了下砸距离 ⇒ 耗时变短且不再是纯滑行；
	// 水平分量全程满值 ⇒ 高空也有横向残影，"扑过去"的观感回来了。
	// ---------------------------------------------------------------------

	/**
	 * 飞踢【水平】前进冲量（cm/s）。方向 = 索敌/准心的水平投影（KickDir 清掉 Z 再归一化），
	 * 【不随俯视角度衰减】—— 这是这个属性存在的全部意义（见上面那段）。
	 *
	 * 手感换算：水平 1500 → 净空 150cm 的近距离约 0.10s 撞上（"唰"一下就贴脸）；
	 * 1500cm 外的远距离约 0.24s。想要更有"扑"的力度就往上加。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="cm/s"))
	float KickFlyImpulse = 1500.f;

	/**
	 * 飞踢【下砸】的初速（cm/s）—— 走完整个下砸过程的起始竖直速度（向下）。
	 *
	 * 取 max(这个值, 当前离地高度所需的落地速度)：
	 *   · 低空（离地很矮）：用这个下限。矮高度本来就不需要砸得多快，避免贴地飞踢变得暴烈。
	 *   · 高空：自动升到"从头砸到底"的速度 ⇒ 【落地速度恒定】，和高度无关。
	 *     结果就是低空/高空的耗时差被压到约 2 倍（原来是 5 倍），力量感不再被高度稀释。
	 *   · 顺带天然封了穿地：冲刺速度不可能超过落地所需速度，不会先穿到地下再弹回来。
	 *
	 * 换算（GravityScale=1.0、世界 GravityZ=-980）：落地速度 V ↔ 下坠距离 H = V²/(2g)。
	 *   900 → 413cm   1200 → 735cm   1500 → 1148cm   1800 → 1653cm
	 * 默认 1200 大约覆盖"从 7 米高空砸下来"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="cm/s"))
	float KickDiveSpeed = 1200.f;

	/**
	 * 飞行耗时（秒）换算成【蒙太奇播放速率】的基准。
	 *
	 * 蒙太奇是固定长度播放的（AM_JumpKick 0.533s），而飞行耗时随距离变 ——
	 * 不缩放的话，高空长距离飞行时会出现"出脚动作已经播完、人还在空中飘"，
	 * 落地那一下的接触帧对不上，观感就是"没劲"。
	 *
	 * 速率 = clamp(这个基准 / 实际飞行耗时, 下限, 上限)：
	 *   · 飞行耗时 = 基准 → 原速（1.0）
	 *   · 飞得慢（耗时长）→ 速率 < 1，动画跟着放慢，接触帧正好落在命中那一刻
	 *   · 飞得快（耗时短）→ 速率 > 1，动画加速，不会"人已撞上、动作还没出完"
	 *
	 * 默认 0.35 ≈ 净空飞行撞上的典型耗时（1500cm/s 打 400~500cm 的目标）。
	 * 填 0 = 关闭缩放，蒙太奇固定原速（退回旧行为）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="s"))
	float KickMontageRefFlightTime = 0.35f;

	/** 蒙太奇播放速率的下限（飞行很远、耗时很长时用）。防止动画慢到像定格。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0.05"))
	float KickMontageMinRate = 0.5f;

	/** 蒙太奇播放速率的上限（飞行极短时用）。防止动画快到看不清出脚。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0.05"))
	float KickMontageMaxRate = 2.0f;

	/**
	 * 踢中后给目标的硬直时长（秒），作为 Data.KnockbackDuration 填给 KnockbackGE。
	 *
	 * ⚠️ 这个值要和 GC_Knockback 上那条蒙太奇的长度对得上。库存的
	 * `AM_KnockBack_Kallari` / `AM_KnockBackBack_Kallari` 都是 **2.0s** —— 填 0.5 的话
	 * 硬直 0.5s 就结束了（能走能放技能），人却还躺在地上演剩下 1.5s 的"倒地"姿势，
	 * 观感是「被打飞了但一秒后又自己站起来接着打」，比击退本身更破坏打击感。
	 *
	 * 【不要靠加长这个值去对齐】2 秒不能动在 MOBA 里是长控级别的强度（对比：LoL 里
	 * 2s 的硬控基本都是大招）。正确做法是在 GC_Knockback 的 BP 上把 **MontagePlayRate**
	 * 提到约 4，让 2.0s 的蒙太奇放完只需要 0.5s —— 保持硬控强度不变，把动画压进来。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="s"))
	float KickStunDuration = 0.5f;

	// ---------------------------------------------------------------------
	// 空手形态派生【直拳】（W 闪避 → 派生窗口内按普攻）
	//
	// 【为什么不是同一脚】持刀/默认的派生是 JumpKick（从天而降砸下来）；空手改成一记直拳：
	// 索敌/飞行/到达即结算的整套模型原样复用（表现跟 JumpKick 差不多），只换三样：
	//   ① 蒙太奇（PunchMontage，全身槽位，出拳的接触帧对上命中那一刻）
	//   ② 命中 cue（PunchHitCueTag → GC_DodgePunchHit，拳的特效语言和脚分开配）
	//   ③ 击退方向（KickKnockbackVertical 是负数=往下砸，拳改成 PunchKnockbackVertical 正数=往上弹）
	// 起手充能/彗星拖尾（GameplayCue.DodgeKick）是"脚"的表现，空手直拳不挂。
	// ---------------------------------------------------------------------

	/**
	 * 空手形态派生用的直拳蒙太奇（可空 = 空手仍然用踢）。
	 * 必须是全身槽位（DefaultSlot / FullBody）：飞行中下半身要跟着飞，
	 * UpperBody 槽位的拳动画会让腿继续跑步 —— 那是"上半身打拳、下半身逛街"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick|Punch")
	TObjectPtr<UAnimMontage> PunchMontage;

	/**
	 * 直拳蒙太奇的【接触帧时刻】（秒）—— 和 KickMontageRefFlightTime 同一个语义：
	 * 速率 = 接触帧时刻 / 预估飞行耗时，让拳头甩出去那一刻正好撞上目标。
	 * 填 0 = 关闭缩放（原速播）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick|Punch", meta=(ClampMin="0", Units="s"))
	float PunchMontageRefFlightTime = 0.45f;

	/**
	 * 空手直拳【命中】的 cue 标签（→ BP: GC_DodgePunchHit）。
	 * 留空 = 复用飞踢的 GameplayCue.DodgeKickHit（特效跟脚共用一份）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick|Punch")
	FGameplayTag PunchHitCueTag;

	/**
	 * 直拳命中后给目标的【竖直】冲量（cm/s，正数 = 往上弹）。
	 * 直拳是"打上天一拳"而不是"砸到地上"，所以默认正的小弹（对比 KickKnockbackVertical 的 -600 下砸）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick|Punch")
	float PunchKnockbackVertical = 250.f;

	/**
	 * 飞踢落地的特效 / 震屏强度缩放基准（cm/s）。按【实际落地速度】相对它来缩放。
	 *
	 * 和 KickDiveSpeed 那套配合：竖直初速被抬到"恒定落地速度"之后，低空与高空的落地速度
	 * 基本一致 ⇒ 落地反馈天然就是恒定的，这个缩放只是留一个旋钮给"特别矮的飞踢别太夸张"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="1", Units="cm/s"))
	float KickImpactSpeedRef = 1200.f;


	/** 飞踢"到达即结算"的检查间隔（秒）。0.03 ≈ 每 2 帧判一次，够密又不用每帧跑。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0.005", Units="s"))
	float KickHitCheckInterval = 0.03f;

	/**
	 * 命中余量（cm）：两人【胶囊表面】的距离小于它就结算。0 = 皮肤贴皮肤才算踢到。
	 * 留余量是为了照顾低 tick 率下的一帧位移（1500cm/s ÷ 30fps = 50cm/帧）和网络延迟。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="cm"))
	float KickHitMargin = 30.f;

	/**
	 * 落地结算余量（cm）：落地那一刻还没进 KickHitMargin，但只要在 KickLandingMargin 之内也算踢中。
	 * 落地 = 这一脚飞完了，判据要比飞行中宽松 —— 踩到人身上、贴着脸落到地上显然算踢到。
	 * 不这么做就会出现「明明落在目标身上却没伤害也没日志」的静默丢击。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="cm"))
	float KickLandingMargin = 120.f;

	/**
	 * 飞行超时兜底（秒）：一直没够到目标（被墙挡住 / 目标跑了 / 一直没落地）就按【扑空】收尾。
	 * 最远的 1200cm 在 KickFlyImpulse=1500 + 重力下约 0.55s 到达，1.2s 留了一倍余量。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick", meta=(ClampMin="0", Units="s"))
	float KickMaxFlightTime = 1.2f;

	// ---------------------------------------------------------------------
	// 起手充能（雷欧飞踢的第一段）
	//
	// 出脚那一刻脚上先聚能量（0.15~0.2s 渐强），飞出去之后转成全功率彗星态。
	// 两段共用【同一个 actor cue】（GameplayCue.DodgeKick）—— 它一挂上就"换挡"：
	//   t < ChargeLeadTime  → GC 只开 ChargeNiagara（脚上聚能）
	//   t >= ChargeLeadTime → GC 开 TrailNiagara + AirFlowNiagara（彗星拖尾）
	//   GC 用 ChargeLeadTime=0 表示"不起手充能"（旧行为，直接全开）。
	//
	// 【为什么不挂两个 cue】actor cue 是"一个 GE/一次 Add 对应一个 notify 实例"，
	// 挂两个就得管两个生命周期（两处 Add / 两处 Remove），而这两段在时间上是【连续的】——
	// 用同一个实例换挡，既不会在切换时闪断（Niagara 组件是常驻的，只改开关），
	// 也天然保证"没有起手就没有后续"（见 PerformKick 里 else 分支的说明）。
	// ---------------------------------------------------------------------

	/**
	 * 飞踢起手到"全力出脚"的间隔（秒）。表演上是「脚上聚能 → 蹬地射出去」的那一下延迟。
	 *
	 * 【它同时是两件事的时间轴】：① GC 里充能 → 拖尾的换挡时刻；② 这里决定
	 * 拖尾 cue 之后要不要额外补一次"发射"的本地表现（目前只做 ①）。
	 *
	 * 填 0 = 不起手（GC 直接开全功率），和其它技能的表现节奏一致。默认 0.16s。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick|Charge", meta=(ClampMin="0", Units="s"))
	float KickChargeLeadTime = 0.16f;

	/**
	 * 踢中的击退 GE：授予 State.Knockback（硬控 → 基类 ActivationBlockedTags 挡掉目标一切技能，
	 * 含普攻）+ 打断目标正在放的技能 + 播 GC_Knockback 蒙太奇 + 向下砸的位移。默认 UGE_Knockback。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick")
	TSubclassOf<UGameplayEffect> KnockbackGE;

	/** 踢的伤害 GE。默认 GE_Damage（唯一的伤害入口）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick")
	TSubclassOf<UGameplayEffect> DamageEffect;

	// ---------------------------------------------------------------------
	// 飞踢的「扑空」表现
	//
	// 踢空（没锁到人 / 飞过头 / 被墙挡住）也是玩家的常见体验，给一点反馈比静默消失好。
	// 不打伤害、不改任何状态，纯本地表现。
	// ---------------------------------------------------------------------

	/**
	 * 扑空时在【脚落点】放一下的 cue。可空 = 什么都不做。
	 *
	 * 用 cue 而不是在这里直接生成粒子，是为了和项目里其它表现一致：
	 * cue 走 ASC 多播，各端都看得到，而且素材可以在 BP 里换（不用重编译）。
	 *
	 * 建议配一个「踢空的风声 + 一小团尘土」的表现；不配也完全没问题。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick|Whiff")
	FGameplayTag WhiffCueTag;

	/**
	 * 扑空的判定下落点相对角色的偏移（cm，角色局部空间）：
	 * X = 前方，Y = 右方，Z = 下方。默认「身前下方一点」——
	 * 踢空时脚是在往斜下方扫过去的，粒子落在那儿最自然。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Kick|Whiff")
	FVector WhiffCueOffset = FVector(120.f, 0.f, -80.f);

private:
	/** 闪避位移 + 喷射表现 + 蒙太奇（bSecond=true 时冲量乘 SecondDodgeRangeMultiplier）。 */
	void PerformEvade(class ACharacter* Character, bool bSecond);

	/** 派生踢：Event.Input.DodgeKick 回调。 */
	UFUNCTION() void OnKickInput(FGameplayEventData Payload);

	/** 二段 evade：Event.Input.DodgeEvade 回调。 */
	UFUNCTION() void OnEvadeInput(FGameplayEventData Payload);

	/** 落地回调：闪避跃起落地后关闭空中派生窗口（空中才能派生向下踢）。 */
	UFUNCTION() void OnLanded(const FHitResult& Hit);

	/** 踢：蒙太奇 + 权威端结算 + 收尾。 */
	void PerformKick();

	/** 索敌：准心方向附近容差内最靠近准心的敌人。命中则 OutDirection 指向该目标（吸附）。 */
	AActor* FindKickTarget(FVector& OutDirection) const;

	/**
	 * 索敌扫描长度：贴地 = KickRange（地面手感不变）；空中按「容差锥打到地面的距离」延长，
	 * 上限 KickAcquireRange。让高空飞踢也能锁到下方的敌人。
	 */
	float ComputeAcquireLength(const class ACharacter* Character, const FVector& Start) const;

	/**
	 * 估算飞踢从当前位置飞到目标需要多久（秒）。只用于定蒙太奇播放速率，
	 * 不参与命中判定（判定走 TickKickFlight 的实际距离检查）。
	 * FlyImpulse 传飞行冲量（权威端）或当前速度（模拟端），用不到就传 Velocity。
	 */
	float EstimateKickFlightTime(const AActor* Avatar, const AActor* Target, const FVector& FlyImpulse) const;

	/**
	 * 播踢蒙太奇，播放速率按预估飞行耗时缩放（见 KickMontageRefFlightTime 的注释）。
	 * ExpectedFlightTime <= 0 表示调用方没算（模拟端），这时自己在内部估一次。
	 */
	void ApplyKickMontageRate(AActor* Avatar, float ExpectedFlightTime);

	/** 权威端：对目标施加踢的伤害 + 击退 GE（State.Knockback 硬直 + 向下砸）。 */
	void ApplyKickDamage(AActor* Target);

	/** 飞行心跳：够近 → OnKickHit；超时 → OnKickWhiff。踢中/扑空都由它收尾。 */
	void TickKickFlight();

	/** 飞行结束 → 踢中：结算伤害 + 命中 cue + 收掉拖尾。 */
	void OnKickHit();

	/** 一直没够到目标（被墙挡 / 目标跑了）→ 扑空收尾：不打伤害，只收尾。 */
	void OnKickWhiff();

	/**
	 * 起手充能的表现：挂 GameplayCue.DodgeKick（Actor cue），走 Normal / RawMagnitude 把
	 * 【飞行方向】和【水平飞行速度】带出去，让 GC 里的拖尾/气流朝对的方向喷、按速度调浓淡。
	 *
	 * ★ 只在权威端调用（乘机在 PerformKick 的 if (Target) 分支里）—— cue 走 ASC 多播，
	 *   权威端 Add 一次，各端都会收到。模拟端调会重复挂。
	 */
	void StartKickTrail(ACharacter* Character, const FVector& FlyDir2D, float FlySpeed);

	/**
	 * 起手充能的表现收掉：没锁到目标时用。和 StartKickTrail 一一对应，
	 * 【只能在权威端调】（对应 StartTrail 也只在那里挂）。
	 *
	 * 单独一个函数的理由：这个 Remove 必须和 Add 严格配对，写成一个具名函数
	 * 才好一眼看出"哪儿挂的就哪儿摘"，不至于像旧代码那样被后来的无条件 Add 覆盖掉。
	 */
	void CancelKickCharge(ACharacter* Character);

	/** 结束能力 + 摘派生窗口标签 + 清定时器。 */
	void EndDodge();

	/** 等 Event.Input.DodgeKick 的任务。 */
	UPROPERTY() TObjectPtr<UAbilityTask_WaitGameplayEvent> KickInputTask;

	/** 等 Event.Input.DodgeEvade 的任务（二段 evade）。 */
	UPROPERTY() TObjectPtr<UAbilityTask_WaitGameplayEvent> EvadeInputTask;

	/** 派生窗口超时定时器。 */
	FTimerHandle KickWindowTimer;

	/** 飞踢飞行 → 踢中/扑空 的心跳定时器（循环）。 */
	FTimerHandle KickHitTimer;

	/** 飞踢起飞的世界时刻（秒），用来和 KickMaxFlightTime 比。 */
	float KickFlightStartTime = 0.f;

	/** 飞踢起手锁定的目标（飞行中够近了就结算）。 */
	UPROPERTY() TWeakObjectPtr<AActor> KickTarget;

	/** 这一脚已经收尾了（踢中或扑空）。闩：心跳和 OnLanded 都可能来收尾，第二次必须让路。 */
	bool bKickSettled = false;

	/**
	 * 这次派生走的是【直拳】而不是踢（PerformKick 时按 State.Form.Unarmed + PunchMontage 判定）。
	 * 两端各自判（标签会复制，判得一致）；蒙太奇/命中 cue/击退方向都跟着它分叉。
	 */
	bool bPunchMode = false;

	/** 已经踢出去过一次（闩：一次闪避最多派生一脚）。 */
	bool bKickPerformed = false;

	/** 二段 evade 还没用掉（一次闪避最多补一段）。 */
	bool bSecondEvadeAvailable = false;

	/**
	 * 这一趟的 evade 已经起过手（闩：LocalPredicted 在 ListenServer 主机上「预测 + 权威」会
	 * 各调一次 ActivateAbility，用它挡掉第二趟，避免 LaunchCharacter 位移翻倍）。
	 */
	bool bEvadeStarted = false;
};
