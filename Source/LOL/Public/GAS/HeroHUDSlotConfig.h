// 槽位标签 → 冷却标签的映射表 + 技能栏顺序 + 静态表现。数据驱动，不写死在 C++。
//
// 放 GAS/ 而不是 UI/：它持有 FGameplayTag（判据见 CONVENTIONS.md 规则 2），跟 UAbilitySet /
// UInputConfig 同一类东西。UI 层看不见它 —— Controller 在广播前把 Icon / KeyLabel / DisplayName
// 摘出来塞进 FSkillSlotView，所以 Widget 既不需要认识这份资产，也不需要认识 GameplayTags 模块。

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
// 依赖方向是 GAS → UI，不是反过来：EHeroHUDSlotKind 定义在 UI/HUDTypes.h（那里零 GAS 类型）。
// 方向必须是这个 —— GAS 层知道「UI 怎么分类槽位」，UI 层不知道「标签叫什么」。
#include "UI/HUDTypes.h"
#include "HeroHUDSlotConfig.generated.h"

class UTexture2D;
struct FPropertyChangedEvent;

USTRUCT(BlueprintType)
struct FHeroHUDSlotEntry
{
	GENERATED_BODY()

	/** 槽位标签：Ability.Slot.Q / W / E / R / D / F。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FGameplayTag SlotTag;

	/**
	 * 对应的冷却标签：State.Cooldown.Flash 等。
	 * 【留空是合法的】= 这个槽不显示冷却转圈（比如被动）。现状六条冷却标签，
	 * 六个槽里必然有映射不到的空位，不要为了让表"填满"而乱配。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FGameplayTag CooldownTag;

	/**
	 * 种类：主动 / 被动 / 格挡。纯分类，唯一的影响是 Widget 不画被动的键位标注。
	 * 加被动技能不用改 C++ —— 在数组里插一条 Kind=Passive 就行。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	EHeroHUDSlotKind Kind = EHeroHUDSlotKind::Active;

	/**
	 * 【预留位的开关】没能力时整格收起，而不是画一个灰格子。
	 *
	 * 为什么需要它：被动 / 格挡这类槽位现在可能还没实装，以后才加。要是现在就往数组里
	 * 占好位置，UI 上会多出两个永远灰着的空格子。勾上这个，没授权时自动隐身，
	 * 等哪天能力真的授予了（比如被动挂上 CD 系统）就自己出现 —— **不用回来改这份资产**。
	 *
	 * 默认 false：六个主动槽的既有行为一个字节都没变。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bHideWhenUnavailable = false;

	/**
	 * 这一格的【尺寸倍率】，乘在 UHeroHUDWidget::SkillSlotSize 上。1 = 和其他格一样大。
	 * 想让被动那一格小一点就填 0.8。**≤ 0 当成 1**（想让它不出现请用 bHideWhenUnavailable）。
	 *
	 * 【为什么尺寸要在数据里配，而不是在 Widget 里判 Kind】
	 * 槽位是运行时建的（UHeroHUDWidget::EnsureSlotWidget），设计器里根本点不到具体哪一格 ——
	 * 六格共用同一个 SkillSlotSize，在 WBP 里怎么调都只会影响全部。所以 per-slot 的尺寸必须
	 * 跟着 Entry 走到算尺寸那一步（UHeroHUDWidget::ApplySlotLayout）。而拿 Kind == Passive
	 * 当判据会把两件事绑死：Kind 现在的唯一职责是「被动不画键位标注」，以后想让某个被动
	 * 跟主动一样大就得改代码。这里是个独立的数，任何槽都能单独调。
	 *
	 * 【改完立刻生效，不用重启】尺寸是【每次视图变化都重算一次】的（UHeroHUDWidget::ApplySlotLayout），
	 * 所以 PIE 运行中改这个值，再调一次 UHeroHUDWidget::RefreshFromController() 就能看到结果。
	 * （以前是建格时算一次写死，运行期改它不会回流到已有的格子上 —— 那个限制已经去掉了。）
	 *
	 * 【倍率同时作用于"盒子"和"画面"】盒子由外层 SizeBox 定尺寸，画面由外面那层 ScaleBox
	 * 等比缩放到盒子大小 —— 因为 WBP 画布里的图标尺寸是写死的 offset，不缩放它就会
	 * "格子小了但图标没小"，相邻两格互相压。原理见 GAS_HUD_Setup.md §12.5。
	 *
	 * 【前提】SkillSlotSize 得先配。它是 0 时 SizeBox 的宽高都不设覆盖（透传子控件的自报尺寸）：
	 * 0 乘任何倍率还是 0，倍率没有作用对象。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	float SlotSizeScale = 1.f;

	/** 静态表现。Controller 会把这四个字段原样带进 FSkillSlotView。 */
	// 【本结构体全部字段统一 EditAnywhere】这个结构体只存在于 DataAsset 里（不上 Actor），
	// 对 DA 资产编辑器和 EditDefaultsOnly 零差异；但编辑器 Python / 自动化工具批量填图标
	// 需要实例可写 —— EditDefaultsOnly 会被引擎属性标志连整个数组一起拦下（实测）。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	TObjectPtr<UTexture2D> Icon = nullptr;

	/** 键位标签，蓝图里配 "Q" / "W" / "E" / "R" / "D" / "F"。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FText KeyLabel;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
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
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
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
