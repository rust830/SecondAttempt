// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "GameplayTagContainer.h"
#include "HeroAnimInstance.generated.h"

class ACharacter;
class UAbilitySystemComponent;
class UAnimSequenceBase;
class UBlendSpace;
class UHeroAnimationSet;

/**
 * 所有英雄共用的动画实例基类。项目里第一个 UAnimInstance 子类。
 *
 * 【它解决什么】选择逻辑（读标签 → 挑资源）只写这一份，每个英雄的 ABP 只填一张
 * UHeroAnimationSet，图里只做「读变量 → 播」。ABP 资产本身没法跨英雄共用
 * （AnimBP 和 Skeleton 硬绑定，而 Paragon 每个英雄一个 Skeleton），
 * 所以能共用的部分全部下沉到这里。详见 UHeroAnimationSet 的类注释。
 *
 * 【三层，别混】
 *   1. 移动      每帧连续      → GroundSpeed / CurrentLocomotion / LocomotionPlayRate
 *   2. 状态覆盖  持续到标签消失 → bOverrideActive / CurrentOverride / bOverrideLoop
 *   3. 一次性动作 有长度、要等结束 → 【故意不在这里】，走 UHeroAnimationSet::ActionMontages
 *      查表，播放交给能力或 GameplayCue。
 *      第 3 类不做进来的原因：播放交给能力，能力才能用
 *      UAbilityTask_PlayMontageAndWait 拿到长度和结束回调来收招
 *      （GA_DeathHarvest.cpp:1188 就是这么用的）。让动画层自己播，收招就没有终点了。
 *
 * 【状态从哪来】读 ASC 上的标签。本项目的 ASC 挂在 PlayerState 上而不是 Pawn 上
 * （见 AHeroCombatCharacter::InitializeAbilityActorInfo），而且客户端上 PlayerState
 * 复制到位的时刻晚于 AnimInstance 的创建 —— 所以拿 ASC 这件事必须允许失败并重试，
 * 见 NativeUpdateAnimation。
 *
 * 【为什么不每帧重算】标签变化用 ASC 的泛型标签事件做脏标记，只在标签真的变了时
 * 才重新挑资源。每帧遍历规则表在英雄多了以后是纯浪费。
 */
UCLASS(Blueprintable)
class LOL_API UHeroAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;
	virtual void NativeUninitializeAnimation() override;

	/**
	 * 拔刀 montage 期间强制亮剑。
	 *
	 * 【为什么需要】空手形态下剑是被 HideBone 藏掉的（见 NativeUpdateAnimation）。
	 * 切回持刀时 FormUnarmed 要按 FormBlendSpeed 平滑，蒙太奇前半段权重还没过 0.5，
	 * 剑还是隐形的 —— 「拔刀」却看不见剑，非常怪。GA_FormSwitch 在拔刀 montage
	 * 启动时置 true，结束回调/EndAbility 里清 false。
	 * 注意：这只是绕过本进程的视觉平滑，remote client 上 GA 激活比蒙太奇复制晚，
	 * 会晚一拍亮剑（listen server 单人测试无此问题）。
	 */
	void SetSwordForceVisible(bool bVisible) { bSwordForceVisible = bVisible; }

protected:
	/**
	 * 这个英雄的动画资源表。填在 ABP 的 Class Defaults 上。
	 * 每个英雄填自己那张（DA_Kallari_Anims 之类）。
	 * 留空 = 整个动画层不驱动，退化成接入之前的样子。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Anim")
	TObjectPtr<UHeroAnimationSet> AnimSet;

	// ---------------------------------------------------------------------
	// 给 AnimGraph 读的。全部 Transient：每帧算出来的，不该被序列化，
	// 否则打包时会把「上次编辑器里预览到的那份 BS」烤进资产里。
	// ---------------------------------------------------------------------

	/**
	 * 实际水平速度（cm/s）。BS 的横轴喂它。
	 * 用实际速度而不是 MaxWalkSpeed —— 要的是「这一帧真的在以多快移动」，
	 * 沿墙滑、被击退、减速生效中这三种情况下两个数完全不一样。
	 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	float GroundSpeed = 0.f;

	/**
	 * 移动方向（速度相对角色朝向的夹角，-180..180，正 = 身体右侧）。方向 BS 的横轴喂它。
	 *
	 * 角色平时 bOrientRotationToMovement 朝移动方向，稳态跑动这一值 ≈ 0（前进列）；
	 * 非零值出现在转身 / 侧移 / 被击退的过渡帧 —— 正是八向 BS 要吃的那部分
	 * （转身时从前进列平滑扫过侧向列，而不是硬切）。
	 * 速度 < 5cm/s 时保持上一帧的值：速度趋零时方向是噪声，而且 BS 已经落在 idle 行，
	 * 但快速减速的那段还能继续朝原方向混合，不会闪一下正前方。
	 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	float MovementDirection = 0.f;

	/** 当前移动混合空间。AnimSet 没配 / 没命中任何规则时是 nullptr，图里必须能接受空。 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	TObjectPtr<UBlendSpace> CurrentLocomotion;

	/** 移动混合空间的播放速率 = GroundSpeed / 基准速度。不补时恒 1。 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	float LocomotionPlayRate = 1.f;

	/** 当前要盖的全身动画。nullptr = 不覆盖。 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	TObjectPtr<UAnimSequenceBase> CurrentOverride;

	/** 覆盖是否生效。图里用它做「移动 ↔ 覆盖」的状态切换。 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	bool bOverrideActive = false;

	/** 覆盖动画要不要循环。覆盖生效时有效。 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	bool bOverrideLoop = true;

	/**
	 * 最近一次算选择时用的标签快照。
	 * 标成 BlueprintReadOnly 是为了在 ABP 的预览窗口里能直接看到「它到底认为我身上有什么」——
	 * 「为什么挑了这份 BS」这类问题，看一眼这个比猜快得多。
	 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	FGameplayTagContainer ActiveTags;

	/**
	 * 形态权重（0=持刀，1=空手），已按 FormBlendSpeed 平滑。
	 *
	 * 【图里怎么用】喂给 ModifyCurve 节点覆写名为 "Unarmed" 的姿势曲线，
	 * PoseBlend（Kallari_Skeleton_PoseAsset，additive）读到这条曲线就把
	 * 「空手 − 持刀」的姿势差值按权重叠到当前动画上 —— 只改姿势不改动作。
	 * 不直接用标签布尔硬切，是留给形态切换一个可见的过渡。
	 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	float FormUnarmed = 0.f;

	/** FormUnarmed 追向目标值的速度（FInterpTo 的_interpSpeed_）。0 = 不平滑，瞬间切换。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Anim", meta=(ClampMin="0"))
	float FormBlendSpeed = 8.f;

	/**
	 * LegIK（脚步 IK）的启用权重，喂给 AnimGraph 里 Leg IK 节点的 Alpha。
	 *
	 * 【为什么需要门控，而不是常开】
	 * LegIK 会把脚【钉在 Effector 指向的位置】。空中时脚本该离地（跳跃/下落/闪避飞行），
	 * 一旦被钉住就是"顶髋"——这个项目已经吃过一次（LegIK 被旁路掉了，链变成
	 * `Slot → LocalToComponent → AnimDynamics`，LegIK 成了孤岛节点）。
	 *
	 * 【现在的策略：只管地面跑动】
	 * `bIsFalling || Speed <= FootIKMinSpeed` 时输出 0 ⇒ 空中/静止一律不接管，
	 * 只在「地面且确实在移动」时钉脚 ⇒ 专治跑动滑步，且不重现顶髋。
	 *
	 * 滑步的来源：出拳前冲（PunchLunge）和 Walk/Jog 动画的位移与实际移动不同步，
	 * 脚在地上蹭。LegIK 每帧把脚贴回地面即可消除。
	 *
	 * 0 = 完全不启用（等价于旁路 LegIK 节点）。
	 */
	UPROPERTY(BlueprintReadOnly, Transient, Category="Anim")
	float FootIKAlpha = 0.f;

	/** 地面速度超过这个值才启用脚步 IK（cm/s）。低于它视为站立/起步，不接管。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Anim", meta=(ClampMin="0", Units="cm/s"))
	float FootIKMinSpeed = 120.f;

private:
	/** 缓存的 Pawn。TryGetPawnOwner 每帧也能调，但存一份省事，也方便 Pawn 还没绑上时重试。 */
	TWeakObjectPtr<ACharacter> OwnerCharacter;

	/**
	 * 缓存的 ASC。允许为空 —— 客户端上它比 AnimInstance 创建得晚（挂在 PlayerState 上）。
	 * 弱引用：Pawn/PS 被销毁时不悬挂。
	 */
	TWeakObjectPtr<UAbilitySystemComponent> CachedASC;

	/** 泛型标签事件的句柄。先摘后挂 + NativeUninitializeAnimation 里清理。 */
	FDelegateHandle TagChangedDelegateHandle;

	/** 标签变过 → 下一帧重算。泛型回调不告诉我们是哪个标签，统一置脏。 */
	bool bTagsDirty = true;

	/** 当前选中混合空间的基准速度（cm/s），来自 ResolveLocomotion。0 = 不补 PlayRate。 */
	float CachedSpeedReference = 0.f;

	/** FormUnarmed 要追的目标（0/1），由 State.Form.Unarmed 标签决定，只在标签变化时重算。 */
	float FormUnarmedTarget = 0.f;

	/** 拔刀 montage 期间置 true：藏剑判定被短路，剑全程可见。见 SetSwordForceVisible。 */
	bool bSwordForceVisible = false;

	/** 试着拿 ASC；拿到了就顺手绑标签事件。返回 ASC 是否可用。 */
	bool TryResolveAbilitySystem();

	/** 标签事件回调：只置脏。重算留给 NativeUpdateAnimation，避免同一帧里连着算好几次。 */
	void OnAnyTagChanged(const FGameplayTag Tag, int32 NewCount);

	/**
	 * 空手形态下藏起「Kallari 独有、重定向动画驱不动」的那批装饰骨
	 * （背后兜帽 / 触须 / 背部推进器）。bHide=false 时恢复。
	 *
	 * 为什么必须藏：IK Retargeter 只驱动源骨架（Manny）有的骨，Kallari 独有的这些
	 * 在重定向动画里一条轨道都没有 ⇒ 停在 ref pose ⇒ 从背后支棱出来。
	 * 持刀形态用 Kallari 自带动画（驱动全部 141 根骨）所以看不出来。
	 *
	 * ⚠️ 扭转骨（upperarm_twist_* / thigh_twist_* / calf_twist_01_*）**故意不藏** ——
	 *   它们是身体内部骨，藏了会破坏蒙皮（手/腿变形）。它们同样不被驱动，
	 *   这是「动作僵硬」的来源，要解决只能给动画补轨道，不是藏能解决的。
	 *
	 * 详见 .cpp 里同名函数的长注释。
	 */
	void HideUnarmedDecorationBones(USkeletalMeshComponent* MeshComp, bool bHide);

	/** 按 ActiveTags 重新挑一遍资源。只在标签变化时调。 */
	void RefreshAnimSelection();

	/** 每帧更新 GroundSpeed。 */
	void UpdateGroundSpeed();

	/** 每帧更新 MovementDirection（方向 BS 的横轴）。见属性上的注释。 */
	void UpdateMovementDirection();

	/** 每帧按 GroundSpeed / 基准速度更新 LocomotionPlayRate。 */
	void UpdateLocomotionPlayRate();

	/** 解绑标签事件。幂等。 */
	void UnbindTagEvent();
};
