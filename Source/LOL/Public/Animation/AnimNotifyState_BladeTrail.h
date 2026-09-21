// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AnimNotifyState_BladeTrail.generated.h"

class UNiagaraComponent;
class UNiagaraSystem;
class USkeletalMeshComponent;

/** 拖尾挂哪只手。三连击 1/3 段右手、2 段左手，强化那一击双手。 */
UENUM()
enum class EBladeTrailSide : uint8
{
	Right UMETA(DisplayName = "Right (右手)"),
	Left  UMETA(DisplayName = "Left (左手)"),
	Both  UMETA(DisplayName = "Both (双手)"),
};

/**
 * User.SwordBasePos / User.SwordTipPos 取哪一组插槽。
 * Kallari 的骨骼是外部资源，两组都在，先换着看哪一组贴刀刃。
 */
UENUM()
enum class EBladeTrailSocketSet : uint8
{
	/** sword_base_* → sword_tip_*，和 NS 的参数名一一对应。 */
	SwordBaseTip UMETA(DisplayName = "sword_base / sword_tip"),
	/** pivotA / pivotB 骨骼下的 extensionA → extensionB（能量刃的延伸段）。 */
	ExtensionAB UMETA(DisplayName = "extensionA / extensionB"),
	/** 用下面四个自定义名字。 */
	Custom UMETA(DisplayName = "Custom"),
};

/**
 * 在挥砍区间里挂一条 NS_BladeTrail，每帧把两个插槽的世界坐标喂给 User.SwordBasePos / User.SwordTipPos。
 *
 * 为什么走 AnimNotifyState 而不是让能力播：起停时机由动画自己定（只有挥出去那一段有拖尾，
 * 蓄力和收招没有），而且拖尾跟着 montage 走 —— 强化那一击 `GA_ThreeHitPassive::StartStage`
 * 会换播 EmpowerMontage，A/B/C 上的 notify 不会误触发，强化 montage 自己的 notify 自然接上，
 * 能力那边一行都不用改。
 *
 * ⚠️ 这个对象是 montage 资产里的实例，**所有角色共用同一个**，所以「生成出来的组件」必须按
 * MeshComp 分开存（ActiveTrails），不能放成员变量里 —— 否则两个角色同时挥砍会互相顶掉。
 *
 * ⚠️ 插槽名对不上时 GetSocketLocation 会静默返回组件位置、参数名对不上时 SetVariablePosition
 * 也是静默 no-op（见 NiagaraComponent.cpp 的 GetPositionParameterValue 兜底）。两个坑都不报错、
 * 只会「什么都没发生」，所以这里都当场核一遍并打日志。
 */
UCLASS(meta = (DisplayName = "Blade Trail"))
class LOL_API UAnimNotifyState_BladeTrail : public UAnimNotifyState
{
	GENERATED_BODY()
public:
	UAnimNotifyState_BladeTrail();

	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;

	/** 拖尾用的 Niagara 系统，默认指向 /Game/LOL/Niagara/NS_BladeTrail。 */
	UPROPERTY(EditAnywhere, Category="BladeTrail") TSoftObjectPtr<UNiagaraSystem> TrailSystem;

	/** 挂哪只手。 */
	UPROPERTY(EditAnywhere, Category="BladeTrail") EBladeTrailSide Side = EBladeTrailSide::Right;

	/** SwordBasePos / SwordTipPos 取哪一组插槽。 */
	UPROPERTY(EditAnywhere, Category="BladeTrail") EBladeTrailSocketSet SocketSet = EBladeTrailSocketSet::SwordBaseTip;

	UPROPERTY(EditAnywhere, Category="BladeTrail|Custom", meta=(EditCondition="SocketSet==EBladeTrailSocketSet::Custom", EditConditionHides))
	FName CustomBaseSocketRight = TEXT("sword_base_r");
	UPROPERTY(EditAnywhere, Category="BladeTrail|Custom", meta=(EditCondition="SocketSet==EBladeTrailSocketSet::Custom", EditConditionHides))
	FName CustomTipSocketRight = TEXT("sword_tip_r");
	UPROPERTY(EditAnywhere, Category="BladeTrail|Custom", meta=(EditCondition="SocketSet==EBladeTrailSocketSet::Custom", EditConditionHides))
	FName CustomBaseSocketLeft = TEXT("sword_base_l");
	UPROPERTY(EditAnywhere, Category="BladeTrail|Custom", meta=(EditCondition="SocketSet==EBladeTrailSocketSet::Custom", EditConditionHides))
	FName CustomTipSocketLeft = TEXT("sword_tip_l");

	/** NS 里那两个位置参数的完整名字（含 User. 前缀）。 */
	UPROPERTY(EditAnywhere, Category="BladeTrail|Parameters") FName BaseUserParameter = TEXT("User.SwordBasePos");
	UPROPERTY(EditAnywhere, Category="BladeTrail|Parameters") FName TipUserParameter = TEXT("User.SwordTipPos");

	/**
	 * NS_BladeTrail 的 emitter 勾了 Local Space 就打开：喂进去的坐标会先转成 Niagara 组件的局部坐标
	 * （组件挂在 mesh 上，所以局部坐标 = 角色网格空间），拖尾才会跟着角色走而不是糊在世界里。
	 * 没勾（World Space，默认）就保持关：直接喂世界坐标。
	 */
	UPROPERTY(EditAnywhere, Category="BladeTrail|Parameters") bool bLocalSpace = false;

	/**
	 * NotifyEnd 之后等多久强制销毁组件。
	 * 正常路径是 Deactivate() + bAutoDestroy，粒子放完自己销毁；但 NS 的 emitter 要是设成无限循环，
	 * auto destroy 就永远等不到 —— 那样每次挥砍会泄漏一个组件常驻运行，所以留个定时器兜底。
	 */
	UPROPERTY(EditAnywhere, Category="BladeTrail|Parameters", meta=(ClampMin="0.1", Units="s")) float TeardownDelay = 2.f;

private:
	/** 一次挥砍给同一条 mesh 生成的两条拖尾（Side=Both 时两条都有）。 */
	struct FActiveTrail
	{
		TObjectPtr<UNiagaraComponent> Right;
		TObjectPtr<UNiagaraComponent> Left;
	};

	/** 按 mesh 分开存：notify 实例是所有角色共用的，状态不能放成员变量。键用弱引用，mesh 没了就自动失效。 */
	TMap<TWeakObjectPtr<USkeletalMeshComponent>, FActiveTrail> ActiveTrails;

	/** 解析出实际要用的插槽名。 */
	FName ResolveSocket(bool bTip, bool bRight) const;

	/** 生成一侧的拖尾。插槽/参数校验不过就打日志并返回 nullptr。 */
	UNiagaraComponent* SpawnTrail(USkeletalMeshComponent* MeshComp, UNiagaraSystem* System, bool bRight) const;

	/** 每帧把两个插槽的坐标写进 NS。 */
	void UpdateTrail(UNiagaraComponent* Component, USkeletalMeshComponent* MeshComp, bool bRight) const;

	/** 停掉一条拖尾，并挂上兜底销毁的定时器。 */
	static void TeardownTrail(UNiagaraComponent* Component, float DelaySeconds);
};
