// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "HeroAnimationSet.generated.h"

class UAnimMontage;
class UAnimSequenceBase;
class UBlendSpace;

/**
 * 「身上带着这些标签时，移动层换成这份混合空间」。
 *
 * 条件用 FGameplayTagQuery 而不是两个 FGameplayTagContainer，是因为真实的动画条件
 * 经常要取反或组合（「施法中 且 非隐身」）。只写一条标签时，细节面板里选
 * Any Tags Match + 一条标签就行，不比 Container 麻烦。
 *
 * 只用 UBlendSpace（2D）是跟着现有资产走的：Paragon 的 KuroV2_Jog_Blendspace 和
 * 模板的 BS_Idle_Walk_Run 都是 2D。以后要用 1D 的话这里换成 UBlendSpace1D，
 * ABP 那边同时换成 Blend Space 1D Player 节点 —— 两处必须一起改，光改一处连不上。
 */
USTRUCT(BlueprintType)
struct FLocomotionSwap
{
	GENERATED_BODY()

	/**
	 * 命中条件。
	 *
	 * **留空 = 无条件命中**，当兜底规则用。注意这个语义是 UHeroAnimationSet 自己
	 * 短路出来的，不是引擎白给的：FGameplayTagQuery::Matches 对空 Query 返回 false
	 * （GameplayTagContainer.cpp:1771），直接调的话留空会变成「永远不命中」。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGameplayTagQuery TagQuery;

	/** 命中后换成的移动混合空间。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<UBlendSpace> BlendSpace;

	/**
	 * 这份混合空间里的动画是按多少 cm/s 做的 —— 用来算
	 * PlayRate = 实际水平速度 / 本值（见 UHeroAnimInstance::RefreshAnimSelection）。
	 *
	 * 【为什么必须有这个字段】tag 是布尔的，减速 30% 和减速 50% 身上都是
	 * State.Slowed、都命中同一条规则、落到同一份 BS。不补速率的话两种速度下
	 * 步频一模一样，脚会明显打滑 —— 这是「用 tag 换 BS」这个选型自带的代价，
	 * 靠这个字段还回来：345→242 播 1.21x，345→172 播 0.70x，自动分开。
	 *
	 * 0 = 不补（PlayRate 恒 1），适合混合空间自己已经把速度做进轴的场合。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="cm/s"))
	float SpeedReference = 0.f;

	/** 大了赢。同分时数组里靠前的赢。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	int32 Priority = 0;
};

/**
 * 「身上带着这些标签时，整体盖掉移动层，改播这一个动画」（眩晕 / 束缚 / 死亡）。
 *
 * 用 UAnimSequenceBase 而不是蒙太奇：覆盖是「持续到标签消失」的语义，蒙太奇播完
 * 就结束了，还得自己处理循环；而且 ACharacter::PlayAnimMontage 和
 * UAnimInstance::Montage_Play 的默认 bStopAllMontages=true 会顺手清掉别的蒙太奇槽位
 * —— GA_ThreeHitPassive.cpp:257 的注释就记着这个坑。序列不参与蒙太奇槽位那套，互不干扰。
 */
USTRUCT(BlueprintType)
struct FStateOverride
{
	GENERATED_BODY()

	/** 命中条件。**留空 = 无条件命中**（同 FLocomotionSwap，语义由 C++ 短路实现）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	FGameplayTagQuery TagQuery;

	/** 覆盖播放的动画（Stun_Idle / Bound / Death_A 这种）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	TObjectPtr<UAnimSequenceBase> Anim;

	/** 循环播。死亡这种「一次性摆姿」可以关掉。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	bool bLoop = true;

	/** 大了赢。同分时数组里靠前的赢。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)
	int32 Priority = 0;
};

/** ResolveLocomotion 的结果：用哪份混合空间 + 它的基准速度（给上层算 PlayRate）。 */
USTRUCT(BlueprintType)
struct FHeroLocomotionChoice
{
	GENERATED_BODY()

	/** 选中的移动混合空间。nullptr = 不驱动（AnimBP 自己管）。 */
	UPROPERTY(BlueprintReadOnly)
	TObjectPtr<UBlendSpace> BlendSpace = nullptr;

	/** 这份混合空间的基准速度（cm/s）。0 = 不补 PlayRate。 */
	UPROPERTY(BlueprintReadOnly)
	float SpeedReference = 0.f;
};

/**
 * ResolveStateOverride 的结果。
 * 带上 bLoop 是因为循环与否是每条规则自己配的，不能由调用方一刀切 ——
 * 「束缚」要循环，「死亡」摆完就停。
 */
USTRUCT(BlueprintType)
struct FHeroStateOverrideChoice
{
	GENERATED_BODY()

	/** 要盖的动画。nullptr = 不覆盖。 */
	UPROPERTY(BlueprintReadOnly)
	TObjectPtr<UAnimSequenceBase> Anim = nullptr;

	/** 循环播。Anim 为空时无意义。 */
	UPROPERTY(BlueprintReadOnly)
	bool bLoop = true;

	/** Anim 非空。给蓝图省一次比较。 */
	UPROPERTY(BlueprintReadOnly)
	bool bValid = false;
};

/**
 * 一个英雄的全部动画资源入口。每个英雄配一张，填在 ABP 的 Class Defaults 上。
 *
 * 【为什么要有这个类】动画资源过去散在各处：GA_Block 自己有 BlockMontage、
 * GA_Stealth 自己有 StealthMontage、普攻的蒙太奇在 UThreeHitPassiveData 里、
 * 大招的在 UDeathHarvestData 里。换英雄得把每个技能都改一遍。
 * 收进来之后英雄实例只填这一张表，技能侧只说「播 Anim.Block」。
 *
 * 【ABP 不能共用，这个类可以】每个 Paragon 英雄是独立 Skeleton 资产
 * （Kallari_Skeleton / Sparrow_Skeleton / Wukong_Skeleton），而 AnimBP 和 Skeleton
 * 是硬绑定，没法跨英雄共用一张图。所以「统一」只能落在这一层：
 * 逻辑在 UHeroAnimInstance（C++，完全共用）、资源在这张表（每英雄一张），
 * ABP 退化成薄壳 —— 每个 Skeleton copy 一份一模一样的图，只是绑不同 Skeleton。
 *
 * 【目录】按 CONVENTIONS.md 规则 2 放 GAS/：这个类直接操作 GameplayTag。
 */
UCLASS(BlueprintType)
class LOL_API UHeroAnimationSet : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * 默认移动混合空间。没有任何 LocomotionSwaps 命中时用它。
	 * 留空 = 动画层不驱动移动（退化成接入之前的样子，AnimBP 自己管）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Locomotion")
	TObjectPtr<UBlendSpace> DefaultLocomotion;

	/** 换混合空间的规则（减速 / 潜行 / 重伤）。按 Priority 挑最高的一条。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Locomotion")
	TArray<FLocomotionSwap> LocomotionSwaps;

	/** 全身覆盖（眩晕 / 束缚 / 死亡）。按 Priority 挑最高的一条，没命中就不覆盖。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="State")
	TArray<FStateOverride> StateOverrides;

	/**
	 * 一次性动作：Anim.* 标签 → 蒙太奇。
	 *
	 * 【这里只查表，不播放】。播放留给调用方，因为两类调用的语义不同：
	 *   要等结束的（大招、普攻连段）→ 拿指针交给 UAbilityTask_PlayMontageAndWait，
	 *     能力靠它的长度和回调驱动自己收招（见 GA_DeathHarvest.cpp:1188）；
	 *   播完不管的（格挡、破隐起手）→ 直接 Montage_Play。
	 * 让动画层自己播的话，前一类就拿不到结束回调了 —— 收招没有终点。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Action")
	TMap<FGameplayTag, TObjectPtr<UAnimMontage>> ActionMontages;

	/** 挑移动混合空间。没有任何规则命中 → DefaultLocomotion（可能也是空的）。 */
	UFUNCTION(BlueprintPure, Category="Anim")
	FHeroLocomotionChoice ResolveLocomotion(const FGameplayTagContainer& Tags) const;

	/** 挑全身覆盖动画。没命中 → bValid = false（= 不覆盖）。 */
	UFUNCTION(BlueprintPure, Category="Anim")
	FHeroStateOverrideChoice ResolveStateOverride(const FGameplayTagContainer& Tags) const;

	/** 一次性动作查表。没配 → nullptr，要不要打日志由调用方决定。 */
	UFUNCTION(BlueprintPure, Category="Anim")
	UAnimMontage* FindActionMontage(const FGameplayTag& ActionTag) const;
};
