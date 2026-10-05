// 英雄数值表：这个英雄 1 级各项属性是多少、每级涨多少。
//
// 【为什么要有这份资产】数值原来是写死在 HeroCombatAttributeSet.cpp 顶部那张
// GHeroStatTable 里的 —— 所有英雄共用一份，改一个英雄的攻速就要动 C++ 重编译。
// 现在那张表退化成「兜底表」（= 没配英雄时的默认英雄），每个英雄一份 DA。
//
// 三条边界，越界就会出问题：
//  1. **属性名是 schema，属性值是数据。** 加一条新属性（比如「怒气」）仍然要改 C++：
//     属性集里加一个 FGameplayAttributeData + 钳制规则 + Replicate。给某个英雄一个数字
//     才走这份 DA。不要把「任意属性」做成 TMap<FGameplayTag, float> —— 那等于把属性名
//     变成运行期字符串，FGameplayAttribute 的编译期检查全丢，所有消费者改成查表。
//  2. **这里只有「基础值」。** 装备、buff、减速都是 GE 修正符，不在这份表里，也不该在这里
//     做加减 —— 写基础值会覆盖掉它们（`ApplyStats` 用的是 SetNumericAttributeBase）。
//  3. **当前值（Health / Energy）不是这一表的行。** 它们的 base 是伤害和消耗的落点，
//     按表重算 = 每次升级自动满血满蓝。它们只跟着 Max 的差值走，见 ApplyStats。
//
// 放 GAS/ 而不是别处：它持有 FGameplayAttribute（判据见 CONVENTIONS.md 规则 2），
// 和 UHeroAttributePanelConfig 同一类东西。

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "Engine/DataAsset.h"
#include "GameplayEffectTypes.h"
#include "HeroStatConfig.generated.h"

USTRUCT(BlueprintType)
struct FHeroStatEntry
{
	GENERATED_BODY()

	/**
	 * 要写哪一条属性，取自 UHeroCombatAttributeSet（编辑器里先选类、再选具体属性）。
	 *
	 * 【留空是合法的，但那一行等于不存在】—— 空属性绑不到任何东西上，写进去静默无效。
	 * 和面板 DA 不同（那边空行显示 0），这边空行会被跳过并记一条日志，因为「配了却没生效」
	 * 在这里是个真正的错误，不是中间状态。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stat")
	FGameplayAttribute Attribute;

	/** 1 级时的基础值。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stat")
	float Base = 0.f;

	/**
	 * 每升一级涨多少。0 = 不随等级成长（暴击率、移速这些）。
	 *
	 * 单位口径跟属性走：攻速那是 0.032（= 每级 +3.2%），暴击伤害那边是倍率不是百分数。
	 * 属性集里的 ClampAttribute 会兜住离谱的值。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stat")
	float PerLevel = 0.f;
};

/**
 * 一个英雄的数值表。数组顺序无所谓 —— 和面板 DA 不一样，这里没有「顺序即 UI」的约束。
 *
 * 【一行一个属性的写法是有意的】：不写成「一个结构体 27 个 float」是因为那样加属性要改
 * 结构体定义（C++），而这里加一行只是在编辑器里多一条。
 */
UCLASS()
class LOL_API UHeroStatConfig final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Stat")
	TArray<FHeroStatEntry> Stats;

	/** 空表 = 没配。ApplyStats 会退回属性集里的内置兜底表，并记一条日志。 */
	bool IsEmpty() const { return Stats.Num() == 0; }
};
