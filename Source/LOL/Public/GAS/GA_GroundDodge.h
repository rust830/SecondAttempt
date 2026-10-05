// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GA_GroundDodge.generated.h"

class UAnimMontage;
class ACharacter;
class UGameplayEffect;

/**
 * 地面闪避：朝【移动输入方向】在地面冲刺一段距离。
 *
 * 和 GA_Dodge（空中飞踢）是【两个独立技能】：这个纯地面、不跃向空中、不派生飞踢。
 * 表现（喷射粒子）复用 GC_Dodge。
 *
 * 【闪避动画走蒙太奇】Fwd / Bwd 各一条完整蒙太奇（Start/Mid/End 三段已烘进去），
 * 走 AnimGraph 的 DefaultSlot。
 *
 * 曾经试过让 UHeroEvadeAnimDriver 反射驱动 BS_Evade 做无缝过渡，已弃用 —— ABP 里的
 * AnimGraphNode_BlendSpaceGraph_1 是个空壳（BlendSpace=None，指向的资产已丢），
 * 它会把一个未定义姿势混进最终输出，表现为抽搐 / 偶尔播错片段。
 */
UCLASS(Blueprintable)
class LOL_API UGA_GroundDodge : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_GroundDodge();
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	// -------------------------------------------------------------------------
	// 位移参数
	// -------------------------------------------------------------------------

	/**
	 * 地面闪避要走的【水平距离】（cm）—— 这是主参数，调参直接看它。
	 *
	 * 【为什么不直接填冲量（cm/s）】因为那样「配置值」和「实际走多远」是两回事：
	 * 冲量只是一次性的初速，之后完全交给 CharacterMovementComponent 用地面摩擦
	 * （GroundFriction=8）+ BrakingDecelerationWalking 削掉，实测走出来的距离远小于
	 * 用「冲量 × 时间」估的值。填距离 + 内部反解速度，配置值就是最终结果。
	 *
	 * 参考尺寸：Kallari 胶囊半径 42cm ⇒ 一个身位约 90cm（位移里有一部分被对手胶囊挡掉）。
	 * LoL 里闪现/位移类技能普遍是 400~500cm，默认 450。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0", Units="cm"))
	float DodgeDistance = 450.f;

	/**
	 * 位移的【最短时长】（秒）。距离 ÷ 这个值 = 需要的最低平均速度。
	 *
	 * 它决定"冲得多快"：同样的距离，时长越短越像瞬移，越长越像滑步。
	 * LoL 的老闪现约等于瞬移（无位移过程），新闪现是 0.25s 滑过去 —— 这里默认取
	 * 一个偏"利落"的值，别做得像在冰面上滑。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0.02", Units="s"))
	float DodgeDuration = 0.18f;

	/**
	 * 位移的【最长时长】兜底（秒）：超过它就收手，不管走没走够。
	 *
	 * 什么时候会触发：贴着墙冲（速度被几何吃掉，一直走不够距离）、
	 * 或者撞进凹角原地顶住。没有这个兜底就会"卡在墙里一直往前拱"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0.05", Units="s"))
	float DodgeMaxDuration = 0.35f;

	// -------------------------------------------------------------------------
	// 完美闪避
	// -------------------------------------------------------------------------

	/**
	 * 完美闪避窗口（秒）：按下闪避后的这段时间里被打中，判定为完美闪避。
	 *
	 * 【为什么比 GA_Dodge 的 0.35 短】地面闪避本身就是 450cm 的位移，无敌帧再给满
	 * 就等于"位移 + 长时间免伤"白送；空中飞踢没有位移收益，可以给宽一点。
	 * 这个值要 ≥ DodgeDuration（0.18），否则位移还没走完窗口就关了，
	 * "躲开了"和"免伤了"在观感上会对不上。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge|Perfect", meta=(ClampMin="0", Units="s"))
	float PerfectDodgeWindow = 0.25f;

	/**
	 * 窗口效果：给自身挂 State.Dodge.Window 的 HasDuration GE（时长由上面的秒数走
	 * SetByCaller 传进去）。伤害执行 UExecCalc_Damage 靠这个标签判定完美闪避。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge|Perfect")
	TSubclassOf<UGameplayEffect> DodgeWindowEffect;

	// -------------------------------------------------------------------------
	// 反卡地参数
	// -------------------------------------------------------------------------

	/**
	 * 位移期间的【离地抬升】（cm）—— 治"胶囊顶住地面导致滑动被吃掉"。
	 *
	 * 【为什么会卡地】胶囊是立起来的圆柱，站着时底面贴着地面。`LaunchCharacter` 的
	 * 水平冲刺在 PhysWalking 里走 `SafeMoveUpdatedComponent`（扫掠）——整个胶囊一起撞地面，
	 * 地面不平 / 有微小台阶 / 斜坡时，胶囊会被"顶住"，向前那一步只走了很少一点，
	 * 看起来就是"冲了一下就停下"。
	 *
	 * 给一个很小的 Z 速度让胶囊【短暂离地】几毫米到一两厘米，扫掠就不再和地面纠缠，
	 * 水平位移能完整跑完。落地由重力自然完成，不需要额外处理。
	 * 太大就变成"跳起来闪避"了，所以这个值要小 —— 默认 40（约 0.08s 离地 0.8cm）。
	 *
	 * 填 0 = 不抬升（退回旧行为，会卡地）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0", Units="cm/s"))
	float DodgeLiftOffSpeed = 40.f;

	/**
	 * 收尾时把剩余速度【抹掉】的比例（0~1）。
	 *
	 * 走够距离后如果还剩速度，人会继续往前溜一段 —— 那正是"配置 450 实际走 600"的另一半原因。
	 * 这里按剩余距离的比例把速度插值到 0：剩余越多削得越狠，走完那一刻速度正好接近 0。
	 * 1 = 走到就立刻停住（最精确，略生硬）；0.6 左右留一点余韵。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0", ClampMax="1"))
	float DodgeStopDamping = 0.9f;

	/** 位移的补速度间隔（秒）。0.01 ≈ 每帧，比 Tick 便宜且不受 Actor tick 频率影响。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge", meta=(ClampMin="0.005", Units="s"))
	float DodgeUpdateInterval = 0.01f;

	/** 向前 Evade 蒙太奇（AM_Evade_Fwd）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge")
	TObjectPtr<UAnimMontage> EvadeForwardMontage;

	/** 向后 Evade 蒙太奇（AM_Evade_Bwd）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Dodge")
	TObjectPtr<UAnimMontage> EvadeBackwardMontage;

private:
	/**
	 * 这一趟的 evade 已经起过手（闩：LocalPredicted 在 ListenServer 主机上「预测 + 权威」会
	 * 各调一次 ActivateAbility，用它挡掉第二趟，避免位移/表现跑两次）。
	 */
	bool bEvadeStarted = false;

	/** 起手位置（权威端），用来算"已经走多远"。 */
	FVector DodgeStartLocation = FVector::ZeroVector;

	/** 起手时刻（秒），用来判超时。 */
	float DodgeStartTime = 0.f;

	/** 补速度的定时器。 */
	FTimerHandle DodgeUpdateTimer;

	/**
	 * 已经在收尾流程里（防止 FinishDodge → EndAbility → FinishDodge 重入）。
	 */
	bool bDodgeFinishing = false;

	/** 这一趟的冲刺方向（水平单位向量）。 */
	FVector DodgeDirection = FVector::ZeroVector;

	/** 收尾（停表 + 清计时器 + 排 EndAbility）。幂等。 */
	void FinishDodge();

	/** 补速度：按剩余距离反解当前应有速度，并处理撞墙/超时兜底。 */
	void TickDodgeStep();
};
