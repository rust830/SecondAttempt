// 斗魂竞技场：抽签池的实现。

#include "GAS/ArenaRewardPool.h"

#include "GAS/ArenaAugmentData.h"
#include "GAS/ArenaItemData.h"

namespace
{
	/**
	 * 按权重从 Candidates 里挑一个下标，挑完把它从数组里挖掉（保证不重复）。
	 *
	 * 【权重全为 0 时退化成等概率】这时「按权重抽」没有定义（总和为 0，除不了）。
	 * 与其返回 -1 让上层去处理一种几乎不会发生的分支，不如退回等概率 ——
	 * 那个行为更好猜，而且不会让一整池子装备因为忘填权重而全部抽不出来。
	 *
	 * 实现是「先累加、再在总和里取一个点、二分不出就线性扫」——
	 * 池子是几十条的量级，线性扫足够，不值得为它排序建前缀和表。
	 */
	int32 TakeWeightedIndex(TArray<float>& Weights)
	{
		const int32 Num = Weights.Num();
		if (Num <= 0)
		{
			return INDEX_NONE;
		}

		float Total = 0.f;
		for (const float Weight : Weights)
		{
			Total += FMath::Max(0.f, Weight);
		}

		int32 Picked = INDEX_NONE;

		if (Total <= 0.f)
		{
			// 全 0（或全负）：等概率。
			Picked = FMath::RandRange(0, Num - 1);
		}
		else
		{
			const float Roll = FMath::FRandRange(0.f, Total);
			float Accumulated = 0.f;
			for (int32 Index = 0; Index < Num; ++Index)
			{
				Accumulated += FMath::Max(0.f, Weights[Index]);
				if (Roll <= Accumulated)
				{
					Picked = Index;
					break;
				}
			}

			// 浮点误差可能让 Roll 落在最后一个之后（Roll 恰好等于 Total 时上面已覆盖，
			// 但累加过程中的舍入仍可能让循环走完都没命中）。兜底取最后一个有权重的。
			if (Picked == INDEX_NONE)
			{
				for (int32 Index = Num - 1; Index >= 0; --Index)
				{
					if (Weights[Index] > 0.f)
					{
						Picked = Index;
						break;
					}
				}
			}
		}

		if (Picked != INDEX_NONE)
		{
			Weights.RemoveAt(Picked);
		}

		return Picked;
	}
}

void UArenaRewardPool::RollItems(
	EArenaItemTier Tier,
	int32 Count,
	const TArray<UArenaItemData*>& Exclude,
	TArray<UArenaItemData*>& OutItems) const
{
	OutItems.Reset();

	if (Count <= 0)
	{
		return;
	}

	// 先按品质 + 排除表筛出候选，权重跟着一起搬（两个数组下标一一对应）。
	TArray<UArenaItemData*> Candidates;
	TArray<float> CandidateWeights;

	for (const FArenaItemPoolEntry& Entry : Items)
	{
		UArenaItemData* Item = Entry.Item;
		if (!Item || Item->Tier != Tier)
		{
			continue;
		}

		if (Exclude.Contains(Item))
		{
			continue;
		}

		Candidates.Add(Item);
		CandidateWeights.Add(FMath::Max(0.f, Entry.Weight));
	}

	const int32 Wanted = FMath::Min(Count, Candidates.Num());
	OutItems.Reserve(Wanted);

	for (int32 Pick = 0; Pick < Wanted; ++Pick)
	{
		const int32 Index = TakeWeightedIndex(CandidateWeights);
		if (Index == INDEX_NONE)
		{
			break;
		}

		OutItems.Add(Candidates[Index]);
		// 两个数组同步挖掉，下标继续一一对应。
		Candidates.RemoveAt(Index);
	}
}

void UArenaRewardPool::RollAugments(
	EArenaAugmentTier Tier,
	int32 Count,
	const TArray<UArenaAugmentData*>& Exclude,
	TArray<UArenaAugmentData*>& OutAugments) const
{
	OutAugments.Reset();

	if (Count <= 0)
	{
		return;
	}

	// 同 RollItems：先按品质 + 排除表筛出候选，权重跟着一起搬。
	TArray<UArenaAugmentData*> Candidates;
	TArray<float> CandidateWeights;

	for (const FArenaAugmentPoolEntry& Entry : Augments)
	{
		UArenaAugmentData* Augment = Entry.Augment;
		if (!Augment || Augment->Tier != Tier || Exclude.Contains(Augment))
		{
			continue;
		}

		Candidates.Add(Augment);
		CandidateWeights.Add(FMath::Max(0.f, Entry.Weight));
	}

	const int32 Wanted = FMath::Min(Count, Candidates.Num());
	OutAugments.Reserve(Wanted);

	for (int32 Pick = 0; Pick < Wanted; ++Pick)
	{
		const int32 Index = TakeWeightedIndex(CandidateWeights);
		if (Index == INDEX_NONE)
		{
			break;
		}

		OutAugments.Add(Candidates[Index]);
		Candidates.RemoveAt(Index);
	}
}

bool UArenaRewardPool::HasAnyAugmentOfTier(EArenaAugmentTier Tier) const
{
	for (const FArenaAugmentPoolEntry& Entry : Augments)
	{
		if (Entry.Augment && Entry.Augment->Tier == Tier && Entry.Weight > 0.f)
		{
			return true;
		}
	}

	return false;
}

bool UArenaRewardPool::HasAnyItemOfTier(EArenaItemTier Tier) const
{
	for (const FArenaItemPoolEntry& Entry : Items)
	{
		if (Entry.Item && Entry.Item->Tier == Tier && Entry.Weight > 0.f)
		{
			return true;
		}
	}

	return false;
}
