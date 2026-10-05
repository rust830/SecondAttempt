// 斗魂竞技场：一个玩家的装备栏 + 海克斯 + 锻造器次数。
//
// ===========================================================================
// 【为什么挂在 PlayerState 上，不挂在 Pawn 上】
// 和等级是同一个理由（见 MyPlayerState.h 那段）：回合之间会死人、会重生，
// 装备必须活过 Pawn 的重建。而且施加装备的 ASC 本来就在 PlayerState 上 ——
// 状态和它作用的对象放一起。
//
// 【GE 会活过 Pawn 重建】GE 是挂在 ASC（PlayerState 上那个）上的，
// Pawn 换一个只是重新 InitAbilityActorInfo，ActiveGameplayEffects 不动。
// 所以回合之间【不需要】重新施加一遍装备 —— 重新施加反而会叠两份。
// ===========================================================================
//
// 【下标一一对应的两个数组】被复制的 EquippedItems 和服务端专用的 ItemEffectHandles
// 是同一批装备的两半（前者给 UI 看，后者用来摘 GE）。这种平行数组最容易出的错是
// 「只动了一边」—— 所以增删都封在这个类里，外部拿不到 Handle，也就没法让它俩错位。
//
// 放 GAS/ 的理由：它直接施加 / 摘除 GameplayEffect（判据见 CONVENTIONS.md 规则 2）。

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayEffectTypes.h"
#include "GameplayAbilitySpecHandle.h"
#include "GAS/ArenaItemStats.h"
#include "GAS/ArenaTypes.h"
#include "ArenaLoadoutComponent.generated.h"

class UArenaItemData;
class UArenaAugmentData;
class UAbilitySystemComponent;

/** 装备栏 / 海克斯变了（装上一件、被顶掉、抽到海克斯、锻造器次数变化都算）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FArenaLoadoutChangedSignature);

/** 服务端专用：一个海克斯挂上去的那几个 GE 的句柄。不复制。 */
USTRUCT()
struct FArenaAugmentRuntime
{
	GENERATED_BODY()

	/** UGE_ArenaItem 那一份（纯数值部分）。SpecialEffect 为空时也可能是无效句柄。 */
	FActiveGameplayEffectHandle StatHandle;

	/** UArenaAugmentData::SpecialEffect 那一份。没配特殊效果时是无效句柄。 */
	FActiveGameplayEffectHandle SpecialHandle;

	/**
	 * GrantAbilitySet 授出去的那些【技能】句柄。没配 GrantAbilitySet 时是空数组。
	 *
	 * 刻意不做成 UPROPERTY —— 句柄里没有 UObject 指针，是一对索引；而且这个结构整体
	 * 都不是 UPROPERTY（见下面 AugmentRuntimes 的注释）。做成 UPROPERTY 反而会被误以为可复制。
	 *
	 * 【和上面两个句柄的分工】那两个是「摘 GE 用」，这个是「撤技能用」——
	 * 两条通道的撤回方式完全不同（RemoveActiveGameplayEffect vs ClearAbility），
	 * 所以必须分开存，不能塞进同一个 FActiveGameplayEffectHandle。
	 */
	TArray<FGameplayAbilitySpecHandle> GrantedAbilityHandles;
};

/**
 * 装备栏 + 海克斯 + 锻造器次数。
 *
 * 由 AArenaPlayerState 在构造函数里创建，不往 Pawn 上挂。
 */
UCLASS(ClassGroup = (Arena), meta = (BlueprintSpawnableComponent))
class LOL_API UArenaLoadoutComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UArenaLoadoutComponent();

	// ---------------------------------------------------------------------
	// 增删（只在服务端生效）
	//
	// 这一整组都是【服务端权威】：抽签在服务端、施加 GE 在服务端，
	// 客户端那份靠复制下来。客户端调这些函数会被拒并记一条日志 ——
	// 静默 "成功" 会让两端不一致，而且那种不一致很难查。
	// ---------------------------------------------------------------------

	/**
	 * 装上一件装备。装备栏满了按 FullRackPolicy 处理。
	 *
	 * 返回是否真的装上了（false = 非权威端 / 空指针 / 满了且策略是 Discard）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|Loadout")
	bool EquipItem(UArenaItemData* Item);

	/**
	 * 拿到一个海克斯。海克斯不占装备栏，拿多少个都行。
	 *
	 * 三条通道彼此独立、可任意组合（可以只填其中一条，也可以三条都填）：
	 *   ① Modifiers        → 数值，走 UGE_ArenaItem
	 *   ② SpecialEffect    → 挂一个 GE
	 *   ③ GrantAbilitySet  → 授技能 / 标签 / GE / 属性集（见 UArenaAugmentData）
	 *
	 * 返回是否真的生效（false = 非权威端 / 空指针）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|Loadout")
	bool AddAugment(UArenaAugmentData* Augment);

	/**
	 * 摘掉第 Index 个海克斯（连同它授的技能、挂的 GE、给的标签一起收回去）。
	 *
	 * 【现在还没有调用方】竞技场里海克斯是永久的（拿到就是拿到，见 ApplyStatAnvil
	 * 那段注释的同款约定）。但【能授就必须能撤】是默认能力 ——
	 * 否则一旦出现「先给后撤」的顺序，撤不掉的技能会永久占着槽位
	 *（SlotAbilityMap 里那份指向已回收的 spec），按下去静默无反应，且没有任何日志。
	 *
	 * Index 越界 / 不是权威端都静默返回（和 EquipItem 的返回约定一致）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|Loadout")
	bool RemoveAugmentAt(int32 Index);

	/** 攒锻造器次数。 */
	UFUNCTION(BlueprintCallable, Category = "Arena|Loadout")
	void AddForgeCharges(EArenaItemTier Tier, int32 Count);

	/**
	 * 花掉一次锻造器。返回是否花成功（次数不够返回 false，不扣）。
	 * 调用方拿到 true 之后自己去发属性（stat anvil）—— 这里只管次数。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|Loadout")
	bool ConsumeForgeCharge(EArenaItemTier Tier);

	/**
	 * 使用一次属性锻造器（stat anvil）：把随机抽出的属性包直接灌进 ASC。
	 *
	 * 【和 EquipItem 的三点不同】
	 *   ① 不占装备栏 —— LoL Arena 的 stat anvil 是纯数值，跟装备格数无关；
	 *   ② 不可摘除 —— 不保留句柄，竞技场里拿到就是永久的（FIFO 顶掉也轮不到它）；
	 *   ③ 可以无限叠 —— 每次使用都是一份独立的 Infinite GE。
	 *
	 * 表外的属性会被 ArenaItemEffect::ApplyStatModifiers 跳过并记 Warning。
	 * 返回是否真的生效（false = 非权威端 / 一条有效加成都没有）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|Loadout")
	bool ApplyStatAnvil(EArenaItemTier Tier, const TArray<FArenaItemStatModifier>& StatModifiers);

	// ---------------------------------------------------------------------
	// 读（两端都能调，客户端读的是复制下来的那份）
	// ---------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Arena|Loadout")
	int32 GetForgeCharges(EArenaItemTier Tier) const;

	UFUNCTION(BlueprintPure, Category = "Arena|Loadout")
	int32 GetEquippedItemCount() const { return EquippedItems.Num(); }

	UFUNCTION(BlueprintPure, Category = "Arena|Loadout")
	bool IsItemRackFull() const { return EquippedItems.Num() >= MaxItemSlots; }

	/**
	 * 装备栏内容，按装上的先后顺序。
	 *
	 * 返回的是【软引用】而不是裸指针 —— 这两个数组要复制到客户端，而 DataAsset
	 * 不是 Actor，裸指针在客户端会静默变成 null（理由见 ArenaTypes.h 里 Item 那段）。
	 * 服务端 .Get() 恒有效；客户端在 OnRep_Loadout 里已经预热过，也是直接 .Get()。
	 * UI 拿它去读 DisplayName / Icon，够用。
	 */
	const TArray<TSoftObjectPtr<UArenaItemData>>& GetEquippedItems() const { return EquippedItems; }

	/** 已有海克斯。软引用的理由同上。 */
	const TArray<TSoftObjectPtr<UArenaAugmentData>>& GetEquippedAugments() const { return EquippedAugments; }

	/**
	 * 锻造器次数。UI 画次数徽章用。
	 *
	 * 【含次数已经花到 0 的品质，顺序也稳定】不做「花完就删」是因为那会让数组顺序
	 * 随使用过程变化 —— UI 上两个品质的徽章会互相换位置，看着像出了 bug。
	 * 跳过 0 是 UI 的事（一行 if 就够）。
	 */
	const TArray<FArenaForgeCharge>& GetForgeCharges() const { return ForgeCharges; }

	/**
	 * 身上已经装着的、且标了 bUnique 的装备。
	 * 抽签时把它交给 UArenaRewardPool::RollItems 当排除表，避免抽到重复的唯一装备。
	 */
	UFUNCTION(BlueprintPure, Category = "Arena|Loadout")
	TArray<UArenaItemData*> GetEquippedUniqueItems() const;

	/** 本局已经拿过的海克斯（抽签时排除，避免重复）。 */
	UFUNCTION(BlueprintPure, Category = "Arena|Loadout")
	TArray<UArenaAugmentData*> GetEquippedAugmentList() const;

	/** 装备栏 / 海克斯 / 锻造器次数有任何变化。UI 订阅它重画。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|Loadout")
	FArenaLoadoutChangedSignature OnLoadoutChanged;

	// ---------------------------------------------------------------------
	// 配置（在 BP_ArenaPlayerState 上改）
	// ---------------------------------------------------------------------

	/**
	 * 装备栏几格。默认 6，和 LoL 一样。
	 *
	 * 【改这个数要想一下后面的回合数】需求里第 7 回合起是「装备回合 / 装备回合 / 海克斯回合」
	 * 无限循环，装备只会越抽越多 —— 格子数决定了从第几回合开始触发 FullRackPolicy。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Loadout", meta = (ClampMin = "1"))
	int32 MaxItemSlots = 6;

	/** 装备栏满了还抽到装备时怎么办。需求没写这条，见 EArenaFullRackPolicy 的说明。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Loadout")
	EArenaFullRackPolicy FullRackPolicy = EArenaFullRackPolicy::ReplaceOldest;

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * 装着的装备。复制 —— 本地客户端要靠它画装备栏。
	 * 软引用：DataAsset 不是 Actor，裸指针过不了网络（见 ArenaTypes.h）。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Loadout, BlueprintReadOnly, Category = "Arena|Loadout")
	TArray<TSoftObjectPtr<UArenaItemData>> EquippedItems;

	/** 已有的海克斯。复制。软引用的理由同上。 */
	UPROPERTY(ReplicatedUsing = OnRep_Loadout, BlueprintReadOnly, Category = "Arena|Loadout")
	TArray<TSoftObjectPtr<UArenaAugmentData>> EquippedAugments;

	/** 锻造器次数。复制 —— 客户端要画「还能用几次」。 */
	UPROPERTY(ReplicatedUsing = OnRep_Loadout, BlueprintReadOnly, Category = "Arena|Loadout")
	TArray<FArenaForgeCharge> ForgeCharges;

	/** 三个数组共用一个 OnRep：客户端只关心「变了，重画」，不关心变的是哪个。 */
	UFUNCTION()
	void OnRep_Loadout();

private:
	/**
	 * 服务端专用，和 EquippedItems 【下标一一对应】。
	 * 刻意不做成 UPROPERTY（它不需要被 GC —— 句柄里没有 UObject 指针，
	 * 是一对索引）—— 而且做了 UPROPERTY 反而会被误以为是可复制的东西。
	 */
	TArray<FActiveGameplayEffectHandle> ItemEffectHandles;

	/** 服务端专用，和 EquippedAugments 【下标一一对应】。 */
	TArray<FArenaAugmentRuntime> AugmentRuntimes;

	/** 拿 ASC（在 PlayerState 上）。拿不到返回 nullptr。 */
	UAbilitySystemComponent* ResolveASC() const;

	/**
	 * 摘掉第 Index 件装备：先摘 GE，再把两个数组的同一格挖掉。
	 *
	 * 【只留这一个删入口】平行数组的错位全部来自「有人只删了一边」，
	 * 所以删必须走这里，而且两个数组的 RemoveAt 紧挨着写。
	 */
	void RemoveItemAt(int32 Index);

	/** 告诉 UI 变了。两端都会走到（服务端直接调，客户端在 OnRep 里调）。 */
	void BroadcastLoadoutChanged();
};
