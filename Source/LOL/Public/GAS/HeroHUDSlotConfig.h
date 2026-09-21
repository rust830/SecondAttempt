// 槽位标签 → 冷却标签的映射表 + 技能栏顺序 + 静态表现。数据驱动，不写死在 C++。
//
// 放 GAS/ 而不是 UI/：它持有 FGameplayTag（判据见 CONVENTIONS.md 规则 2），跟 UAbilitySet /
// UInputConfig 同一类东西。UI 层看不见它 —— Controller 在广播前把 Icon / KeyLabel / DisplayName
// 摘出来塞进 FSkillSlotView，所以 Widget 既不需要认识这份资产，也不需要认识 GameplayTags 模块。

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "HeroHUDSlotConfig.generated.h"

class UTexture2D;
struct FPropertyChangedEvent;

USTRUCT(BlueprintType)
struct FHeroHUDSlotEntry
{
	GENERATED_BODY()

	/** 槽位标签：Ability.Slot.Q / W / E / R / D / F。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	FGameplayTag SlotTag;

	/**
	 * 对应的冷却标签：State.Cooldown.Flash 等。
	 * 【留空是合法的】= 这个槽不显示冷却转圈（比如被动）。现状只有五条冷却标签，
	 * 六个槽里必然有映射不到的空位，不要为了让表"填满"而乱配。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	FGameplayTag CooldownTag;

	/** 静态表现。Controller 会把这三个字段原样带进 FSkillSlotView。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	TObjectPtr<UTexture2D> Icon = nullptr;

	/** 键位标签，蓝图里配 "Q" / "W" / "E" / "R" / "D" / "F"。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	FText KeyLabel;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	FText DisplayName;
};

/**
 * 英雄 HUD 槽位配置。一个英雄一份（或者所有英雄共用一份，视 UI 是否统一而定）。
 *
 * 【这里是有序 TArray，不是 TMap】—— 数组顺序就是技能栏从左到右的顺序。
 * 用 TMap 的话迭代顺序不稳定，槽位顺序就丢了，UI 还得在蓝图里再排一次序，
 * 于是「SlotTag → CooldownTag 的映射」就有了两份定义（C++ 一份、蓝图那份顺序一份），迟早对不上。
 * 排序也是这份资产的职责。查找走内部缓存，对外只暴露数组。
 */
UCLASS()
class LOL_API UHeroHUDSlotConfig final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 【顺序即 UI 顺序】。数组下标就是 FSkillSlotView::SlotIndex。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	TArray<FHeroHUDSlotEntry> Slots;

	/** 槽位总数（= UI 上要建几个 WBP_SkillSlot）。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	int32 NumSlots() const { return Slots.Num(); }

	/** 槽位标签 → 序号。没配返回 INDEX_NONE。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	int32 IndexOfSlot(const FGameplayTag& SlotTag) const;

	/** 冷却标签 → 序号。没配返回 INDEX_NONE。标签事件回调只用它认人。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	int32 IndexOfCooldownTag(const FGameplayTag& CooldownTag) const;

	/** 按序号取条目。越界返回 nullptr。 */
	const FHeroHUDSlotEntry* FindEntry(int32 SlotIndex) const;

#if WITH_EDITOR
	/** 编辑器里改了 Slots 就丢掉缓存 —— 否则 PIE 里改了映射要重启才生效。 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void EnsureCache() const;
	void RebuildCache() const;

	/** SlotTag → 下标。非 UPROPERTY（里面没有 UObject，不需要 GC 追踪）。 */
	mutable TMap<FGameplayTag, int32> SlotIndexCache;
	mutable bool bCacheValid = false;
};
