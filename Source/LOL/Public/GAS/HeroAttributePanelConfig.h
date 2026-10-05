// 英雄属性面板的配置：显示哪些属性、按什么顺序、叫什么、怎么格式化。
//
// 放 GAS/ 而不是 UI/：它持有 FGameplayAttribute（判据见 CONVENTIONS.md 规则 2），
// 和 UHeroHUDSlotConfig（持 FGameplayTag）同一类东西。UI 层看不见它 ——
// Controller 在广播前把数值格式化成 FText 塞进 FHeroAttributeEntryView，
// 所以 Widget 既不需要认识这份资产，也不需要认识 GAS 模块。
//
// 【C++ 里没有属性枚举、没有 switch】这是这份资产存在的全部理由：
// 加一条面板行 = 在 DA 里加一行（+ 属性集里加一条属性），C++ 一行不动。
// 和 UHeroHUDSlotConfig 的「数组顺序就是技能栏顺序」完全同构。

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "Engine/DataAsset.h"
#include "GameplayEffectTypes.h"
// 依赖方向是 GAS → UI，不是反过来：EHeroAttributeFormat 定义在 UI/HUDTypes.h（那里零 GAS 类型）。
#include "UI/HUDTypes.h"
#include "HeroAttributePanelConfig.generated.h"

class UTexture2D;

USTRUCT(BlueprintType)
struct FHeroAttributePanelEntry
{
	GENERATED_BODY()

	/**
	 * 要显示的属性，取自 UHeroCombatAttributeSet（编辑器里先选类、再选具体属性）。
	 *
	 * 【留空是合法的但没意义】：这一行会显示 0 且永远不会变。没有任何断言 ——
	 * 需求上「加了一条属性但还没接」是正常的中间状态，让它在 UI 上显示 0 比崩掉更有用。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attribute")
	FGameplayAttribute Attribute;

	/** 小图标。留空 = 该行不画图标，只显示数值。 */
	// EditAnywhere 理由见 HeroHUDSlotConfig.h 的 Icon 注释：结构体只活在 DA 里，
	// 资产编辑器零差异，脚本化批量填图标需要实例可写。
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attribute")
	TObjectPtr<UTexture2D> Icon = nullptr;

	/** 属性名。展开态显示（常驻态一般只看图标 + 数值，所以这个名字通常不显示）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attribute")
	FText DisplayName;

	/** 数值怎么格式化。格式化的实现只有一处：UHeroHUDController::FormatAttributeValue。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attribute")
	EHeroAttributeFormat Format = EHeroAttributeFormat::Integer;

	/**
	 * true = 常驻那一条里就有它；false = 只有按 C 展开之后才看得见。
	 *
	 * 用户要的那 8 条（攻/法强/护甲/魔抗/攻速/技能急速/暴击率/移速）勾上，
	 * 其余属性留在展开态 —— 数据驱动的意义就在这里：哪 8 条常驻是配置，不是代码。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attribute")
	bool bShowInCompact = false;
};

/**
 * 英雄属性面板配置。一个英雄一份（或者所有英雄共用一份）。
 *
 * 【这里是有序 TArray，不是 TMap】—— 数组顺序就是面板从上到下（从左到右）的顺序，
 * 同时也是 Controller 订阅属性的顺序。用 TMap 的话迭代顺序不稳定，面板顺序就丢了，
 * 于是「哪条属性在第几行」就有了两份定义（C++ 一份、蓝图那份顺序一份），迟早对不上。
 */
UCLASS()
class LOL_API UHeroAttributePanelConfig final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 【顺序即 UI 顺序】。数组下标就是 FHeroAttributeEntryView 的下标。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attribute")
	TArray<FHeroAttributePanelEntry> Entries;

	/** 面板总行数（= UI 上要建几个 WBP_AttributeEntry）。 */
	UFUNCTION(BlueprintPure, Category = "Attribute")
	int32 NumEntries() const { return Entries.Num(); }

	/** 按序号取条目。越界返回 nullptr（Controller 遍历时用得上，和自己下标取是一回事）。 */
	const FHeroAttributePanelEntry* FindEntry(int32 Index) const
	{
		return Entries.IsValidIndex(Index) ? &Entries[Index] : nullptr;
	}
};
