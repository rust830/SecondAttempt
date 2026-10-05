// 斗魂竞技场：抽签池 —— 三选一就是从这里面抽三个。
//
// ===========================================================================
// 需求里所有「随机三选一」都走这里，一共四处：
//   ① 第 1 / 5 回合的「装备」分支  → RollItems(Tier)
//   ② 第 1 / 5 回合的锻造器，每次使用 → RollItems(Tier)
//   ③ 第 7 回合起「装备回合」的装备分支 / 锻造器 → RollItems(Tier)
//   ④ 第 2 / 4 回合及「海克斯回合」  → RollAugments()
//
// 装备和锻造器抽的是【同一个池子、同一条规则】—— 需求里两者的差别只在
// 「当场三选一」还是「攒次数、要用的时候再三选一」，抽签本身没有区别。
// ===========================================================================
//
// 放 GAS/ 的理由：它引用 UArenaItemData / UArenaAugmentData（那两个持有 GAS 类型）。

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GAS/ArenaTypes.h"
#include "ArenaRewardPool.generated.h"

class UArenaItemData;
class UArenaAugmentData;

/** 池子里的一件装备 + 它的权重。 */
USTRUCT(BlueprintType)
struct FArenaItemPoolEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Pool")
	TObjectPtr<UArenaItemData> Item;

	/**
	 * 相对权重。0 = 抽不到（等价于从池子里拿掉，但留着方便临时停用）。
	 * 品质不在这里配 —— 直接读 Item->Tier，避免「池子里写传说、资产上写棱彩」这种对不上的组合。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Pool", meta = (ClampMin = "0"))
	float Weight = 1.f;
};

/** 池子里的一个海克斯 + 它的权重。 */
USTRUCT(BlueprintType)
struct FArenaAugmentPoolEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Pool")
	TObjectPtr<UArenaAugmentData> Augment;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Pool", meta = (ClampMin = "0"))
	float Weight = 1.f;
};

/**
 * 一个抽签池。整局共用一个资产。
 *
 * 【只在服务端抽】抽签结果要广播给双方（对手也得知道对面拿了什么），
 * 两端各抽一次会抽出不同结果。服务端抽完把结果放进复制数据里。
 */
UCLASS()
class LOL_API UArenaRewardPool final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Pool")
	TArray<FArenaItemPoolEntry> Items;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Pool")
	TArray<FArenaAugmentPoolEntry> Augments;

	/**
	 * 按品质抽 Count 件装备，**不重复**。
	 *
	 * Exclude 里的是「已经装在身上且 bUnique 的」和「本次已经抽出来的」，
	 * 前者保证不重复抽到唯一装备，后者保证同一次三选一里不出现两件一样的。
	 *
	 * 【抽不满 Count 是正常情况】池子里符合条件的不足三件时就有多少给多少。
	 * 调用方必须能处理「少于三个选项」—— 直接当崩处理的话，池子没配全就会炸在开局第一回合。
	 * 池子里一件都没有时返回空数组（调用方会记 Warning 并跳过这次奖励）。
	 */
	void RollItems(
		EArenaItemTier Tier,
		int32 Count,
		const TArray<UArenaItemData*>& Exclude,
		TArray<UArenaItemData*>& OutItems) const;

	/**
	 * 按品质抽 Count 个海克斯，**不重复**。Exclude 是本局已经拿过的。
	 * 品质直接读 Augment->Tier（同 RollItems 的纪律）。抽不满 Count 的处理同 RollItems。
	 */
	void RollAugments(
		EArenaAugmentTier Tier,
		int32 Count,
		const TArray<UArenaAugmentData*>& Exclude,
		TArray<UArenaAugmentData*>& OutAugments) const;

	/** 池子里有没有配这个品质的装备。开局自检用 —— 没配的话第 1 回合就发不出奖励。 */
	bool HasAnyItemOfTier(EArenaItemTier Tier) const;

	/** 池子里有没有配这个品质的海克斯。开局自检用。 */
	bool HasAnyAugmentOfTier(EArenaAugmentTier Tier) const;

	/** 池子里有没有配海克斯。 */
	bool HasAnyAugment() const { return Augments.Num() > 0; }
};
