// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HeroEvadeAnimDriver.generated.h"

class UAnimInstance;
class FProperty;

/**
 * 闪避 BlendSpace（BS_Evade）的反射驱动。
 *
 * 【为什么是"反射"】角色挂的是库存 Paragon 的 Kallari_AnimBlueprint，它的原生父类就是
 * 纯 `/Script/Engine.AnimInstance` —— 项目没法给那张 ABP 加原生成员。而 BS_Evade 的三条
 * 驱动变量（bIsEvading / EvadeProgress / EvadeDirection）是 ABP 自己的蓝图变量。
 * 于是只能按名字找 FProperty 再逐帧写值。这是【唯一】能驱动它的路径 ——
 * 想走原生就得把那张库存 ABP 改父类，风险远大于收益。
 *
 * 【为什么是组件，不是能力里的 tick】闪避表现天然比能力活得久：GA_GroundDodge 起手后
 * 下一帧就 EndAbility，而整段 BS_Evade 要 1.6s。挂在能力上的话能力一结束驱动就断、
 * 动画停在半路；被打断的能力（眩晕/击退）还会把 bIsEvading 永久留在 true。
 * 组件独立存在，能力只负责"喊一声起手"，生命周期不互相绑架。
 *
 * 【和蒙太奇二选一，不能并存】AnimGraph 里有一个 Slot（DefaultSlot）节点，蒙太奇是从
 * 那里【盖】在底层姿态上的。所以只要还 PlayAnimMontage(AM_Evade_Fwd)，BS_Evade 就被
 * 完全遮住、根本看不见。本组件负责 BS 这条路线，因此 GA_Dodge / GA_GroundDodge 不再播
 * evade 蒙太奇（蒙太奇属性保留，用一个开关就能回退对比）。
 *
 * 【时序】组件 tick 在 TG_PrePhysics，并对骨骼网格加了 tick 前置依赖 —— 必须保证
 * "写变量"发生在网格求值动画【之前】，否则 bool 会晚一帧，起手/收尾各闪一下。
 *
 * 【各端独立】纯本地表现，和蒙太奇一样不复制：本地控制端在本机驱动自己的角色。
 * 远端观察者本来也看不到（改动前蒙太奇同样不复制），行为没有变差。
 */
UCLASS(ClassGroup=(LOL), meta=(BlueprintSpawnableComponent))
class LOL_API UHeroEvadeAnimDriver : public UActorComponent
{
	GENERATED_BODY()

public:
	UHeroEvadeAnimDriver();

	/**
	 * 起手一段 evade 表现：写 bIsEvading=true，并把 EvadeProgress 在 Duration 内从 0 推到 1。
	 * 重复调用 = 重新起手（二段闪避就是这么用的）。
	 *
	 * @param bForward  向前（EvadeDirection=0）/ 向后（=180）。BS 的 Y 轴范围就是 0..180。
	 * @param Duration  整段进度走完的时长（秒）。<=0 时用 DefaultEvadeBlendDuration。
	 *                  它应该等于 BS 里 Fwd 三条片段的合计长度（≈1.6s）——
	 *                  填短了就是"只看到开头一闪"（0.4s 跑完 1.6s 的动画）。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero|Anim")
	void PlayEvade(bool bForward, float Duration = -1.f);

	/**
	 * 立即收尾：只把 bIsEvading 落回 false（progress 停在当前值）。
	 * 能力被打断 / 被取消、或组件 EndPlay 时兜底调用 —— 别把角色永久卡在 evade 姿态。
	 * 幂等，没在闪避时是 no-op。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero|Anim")
	void StopEvade();

	/** 这一趟是否还在驱动（进度没走完）。 */
	UFUNCTION(BlueprintPure, Category="Hero|Anim")
	bool IsEvading() const { return bEvading; }

	/**
	 * 默认段时长（秒）。默认 1.6 = BS_Evade 里 Evade_Fwd 三段合计（1.333 + 0.033 + 0.233）。
	 * 改成和 BS 实际片段合计不一致的值，等于把整段动画播快 / 播慢。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Hero|Anim", meta=(ClampMin="0.01", Units="s"))
	float DefaultEvadeBlendDuration = 1.6f;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** 懒取网格上的 AnimInstance（重生 / 换皮会重建，所以不能只缓存一次）。拿不到返回空。 */
	UAnimInstance* ResolveAnimInstance();

	/** 按名字反射找三个属性；只在 AnimInstance 的类变了时才真正重找。缺变量只吵一次。 */
	void RebuildBinding(UAnimInstance* Anim);

	/** 三条属性都绑上了才可写。 */
	bool HasBinding() const { return PropIsEvading && PropProgress && PropDirection; }

	/** 按名字反射写三个变量。绑定不完整时整段跳过（RebuildBinding 里已经吵过一次）。 */
	void WriteEvadeState(bool bIsEvading, float Progress, float Direction);

	/** 缓存的动画实例。网格重建会失效，靠 IsValid 重新取。 */
	TWeakObjectPtr<UAnimInstance> CachedAnimInstance;

	/** binding 是对哪个类找的。换了类（换英雄 / 换 ABP）才需要重找。 */
	TWeakObjectPtr<UClass> BoundAnimClass;

	// BS_Evade 的三条蓝图变量，按名字反射拿到的 FProperty。
	// 任一为空 = 这张 ABP 没那几个变量，整条链不可用（RebuildBinding 会吵一次）。
	FProperty* PropIsEvading = nullptr;
	FProperty* PropProgress = nullptr;
	FProperty* PropDirection = nullptr;

	/** 这一趟的进度状态。 */
	bool bEvading = false;
	float EvadeElapsed = 0.f;
	float EvadeDuration = 0.f;
	float EvadeProgress = 0.f;
	float EvadeDirection = 0.f;

	/** 缺变量的警告只打一次，避免每帧刷屏。 */
	bool bWarnedMissingVars = false;
};
