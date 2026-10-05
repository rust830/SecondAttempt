// 斗魂竞技场：一件装备的定义。
//
// 对应需求里的「装备来源：回合奖励，随机三选一」—— 池子里抽的就是这些东西。
// 一份资产 = 一件装备，含它加的所有属性。
//
// 【加一件新装备不用写 C++】新建一个这个类的 DataAsset，填名字 / 图标 / 品质 / 几条属性，
// 再挂进 UArenaRewardPool 的池子即可。数值怎么变成 GE 见 GE_ArenaItem.h。
//
// 放 GAS/ 的理由同 HeroStatConfig.h：持有 FGameplayAttribute 与 GE 相关的类型。

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GAS/ArenaItemStats.h"
#include "GAS/ArenaTypes.h"
#include "ArenaItemData.generated.h"

/**
 * 一件装备。
 *
 * 【为什么没有「装备类型 / 合成路线 / 价格」这些字段】需求里商店是关掉的
 * （shop.装备购买 = false，金币购买 = 关闭），装备只有「回合奖励」一个来源，
 * 所以价格和合成路线没有任何消费者。等真要做商店时再加，别现在先摆着。
 */
UCLASS()
class LOL_API UArenaItemData final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 三选一界面上显示的名字。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Item")
	FText DisplayName;

	/** 三选一界面上的说明文字。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Item")
	FText Description;

	/** 品质。只影响「从哪个池子里抽得出来」，不影响数值怎么生效。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Item")
	EArenaItemTier Tier = EArenaItemTier::Legendary;

	/** 图标。软引用 —— 抽到之前不需要加载。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Item")
	TSoftObjectPtr<UTexture2D> Icon;

	/**
	 * 加的属性。加法叠加，多条会一起生效。
	 *
	 * 空数组 = 一件什么都不加的装备（合法，但不该出现在池子里 —— 抽到它的玩家会觉得亏）。
	 * 支持哪些属性见 ArenaItemStats.h 那张表；表外的属性会被跳过并记 Warning。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Item")
	TArray<FArenaItemStatModifier> Modifiers;

	/**
	 * 同一件装备能不能重复装。
	 *
	 * 默认 false = 能重复（LoL 里同名装备一般只能买一件，但斗魂这边直说更省事）。
	 * 打开它之后，抽签会把「已经装在身上的」这一件排除掉 —— 见 UArenaRewardPool::RollItems。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Item")
	bool bUnique = false;
};
