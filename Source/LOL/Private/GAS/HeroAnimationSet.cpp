// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroAnimationSet.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"

namespace
{
	/**
	 * 一条规则是否命中。
	 *
	 * 【必须先把空 Query 短路掉】引擎的 FGameplayTagQuery::Matches 对空 Query 返回的是
	 * false 而不是 true（`if (IsEmpty()) return false;`，GameplayTagContainer.cpp:1771），
	 * 直接调的话「条件留空 = 兜底规则」这个直觉语义会变成「永远不命中」。
	 *
	 * 这个坑难查的地方在于它【完全静默】：不报错、不打日志，表现只是
	 * 「我明明配了默认混合空间却一直不换」，很难往空 Query 上想。
	 * 所以这个语义由这里显式给出，不依赖引擎的默认行为。
	 */
	bool MatchesRule(const FGameplayTagQuery& Query, const FGameplayTagContainer& Tags)
	{
		return Query.IsEmpty() || Query.Matches(Tags);
	}
}

FHeroLocomotionChoice UHeroAnimationSet::ResolveLocomotion(const FGameplayTagContainer& Tags) const
{
	FHeroLocomotionChoice Choice;
	Choice.BlendSpace = DefaultLocomotion;

	// 线性扫描取 Priority 最大的一条，用「严格大于 + 已找到」而不是「大于等于」：
	// 同分时数组里靠前的那条保持胜出，既不用排序也不用额外存下标。
	// 规则条数是个位数，不值得为它上排序。
	int32 BestPriority = 0;
	bool bFound = false;

	for (const FLocomotionSwap& Swap : LocomotionSwaps)
	{
		// 条件配了但资源没配 → 当这条不存在，不能拿 nullptr 把默认顶掉
		if (!Swap.BlendSpace)
		{
			continue;
		}
		if (!MatchesRule(Swap.TagQuery, Tags))
		{
			continue;
		}
		if (bFound && Swap.Priority <= BestPriority)
		{
			continue;
		}

		bFound = true;
		BestPriority = Swap.Priority;
		Choice.BlendSpace = Swap.BlendSpace;
		Choice.SpeedReference = Swap.SpeedReference;
	}

	return Choice;
}

FHeroStateOverrideChoice UHeroAnimationSet::ResolveStateOverride(const FGameplayTagContainer& Tags) const
{
	const FStateOverride* Best = nullptr;

	for (const FStateOverride& Override : StateOverrides)
	{
		if (!Override.Anim)
		{
			continue;
		}
		if (!MatchesRule(Override.TagQuery, Tags))
		{
			continue;
		}
		if (Best && Override.Priority <= Best->Priority)
		{
			continue;
		}

		Best = &Override;
	}

	FHeroStateOverrideChoice Choice;
	if (Best)
	{
		Choice.Anim = Best->Anim;
		Choice.bLoop = Best->bLoop;
		Choice.bValid = true;
	}
	return Choice;
}

UAnimMontage* UHeroAnimationSet::FindActionMontage(const FGameplayTag& ActionTag) const
{
	// 查不到就返回 nullptr，不在这一层打日志：调用方可能故意允许某英雄没有某个动作
	// （比如没有格挡动画的英雄），由调用方判断该不该 warn。
	const TObjectPtr<UAnimMontage>* Found = ActionMontages.Find(ActionTag);
	return Found ? Found->Get() : nullptr;
}
