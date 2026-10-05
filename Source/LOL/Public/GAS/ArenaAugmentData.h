// 斗魂竞技场：一个海克斯的定义。
//
// 对应需求里的「海克斯：随机三选一」（第 2 / 4 回合，以及第 7 回合起的循环里每三回合一次）。
//
// 【和装备的区别只有两点】① 海克斯有品质（银 / 金 / 棱彩，见 EArenaAugmentTier），
// 回合表决定每次三选一发哪一档；② 海克斯不占装备栏，装多少个都行
// （LoL 斗魂里海克斯本来就不占格）。数值通道和装备【完全一样】：
// 一串 FArenaItemStatModifier → 一个 Infinite 的 UGE_ArenaItem。
//
// 放 GAS/ 的理由同 ArenaItemData.h。

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GAS/ArenaItemStats.h"
#include "GAS/ArenaTypes.h"
#include "ArenaAugmentData.generated.h"

class UGameplayEffect;
class UAbilitySet;

/**
 * 一个海克斯。
 */
UCLASS()
class LOL_API UArenaAugmentData final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 三选一界面上显示的名字。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Augment")
	FText DisplayName;

	/** 三选一界面上的说明文字。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Augment")
	FText Description;

	/** 图标。软引用。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Augment")
	TSoftObjectPtr<UTexture2D> Icon;

	/**
	 * 品质（银 / 金 / 棱彩）。
	 *
	 * 【品质在这里配，不在池子条目上配】同 UArenaItemData::Tier 的纪律：
	 * 避免「池子里写白银、资产上写棱彩」这种对不上的组合。池子按它过滤。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Augment")
	EArenaAugmentTier Tier = EArenaAugmentTier::Silver;

	/**
	 * 纯数值型海克斯加的属性。走和装备同一条通道（UGE_ArenaItem），
	 * 支持哪些属性见 ArenaItemStats.h。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Augment")
	TArray<FArenaItemStatModifier> Modifiers;

	/**
	 * 特殊效果型海克斯的扩展口：填一个自己的 GE 类，施加时会【额外】挂上它。
	 *
	 * 【为什么留这个口】LoL 的海克斯（斗魂竞技场里的强化符文）大多是「改规则」而不是
	 * 「加数值」——「技能命中回血」「大招冷却减半」这类用属性表表达不了。
	 * 只做数值的话，海克斯就退化成一个换皮的装备，所以这里留一条路：
	 * 要写特殊海克斯就做一个 GE 子类填进来，不用回头改这里和 UArenaLoadoutComponent。
	 *
	 * 留空 = 纯数值海克斯（只用上面的 Modifiers）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Augment")
	TSubclassOf<UGameplayEffect> SpecialEffect;

	/**
	 * 这个海克斯【授予的能力】：技能 + 标签 + GE + 属性集，一整套。
	 * 留空 = 只走上面两条通道（数值 + SpecialEffect）。
	 *
	 * 【为什么必须单独一个字段，不让 SpecialEffect 兼职】SpecialEffect 是
	 * TSubclassOf<UGameplayEffect>，而 **GE 不是能力容器，给不了技能**。
	 * 但 LoL 斗魂里有一整类海克斯是「按 1 放一个新技能」（强化治疗、屏障、加速…），
	 * 只能走 ASC->GiveAbility。
	 *
	 * 【为什么用 UAbilitySet 而不是 TArray<FAbilitySet_GrantAbility>】AbilitySet 是项目里
	 * 已经在用的授权资产（英雄技能组 AS_Abilitys_Kallari、召唤师技能 AS_SummonKit 都是它）。
	 * 复用它 = 一个海克斯能同时授技能 + 授标签 + 授 GE + 授属性集，
	 * 不用在这里重造一套平行结构，也不用给「授标签」再单开一个字段。
	 *
	 * 典型配法：GrantAbility[0] 的 SlotTag 填 Ability.Slot.Hex1（按 1 那一格）。
	 * ⚠️ 槽位标签**必须以 "Ability.Slot." 开头** —— UMyAbilitySystemComponent::OnGiveAbility
	 * 用 Tag.ToString().StartsWith(TEXT("Ability.Slot.")) 判，写成 "Slot.Ability.Hex1"
	 * 会静默失效（技能授进来了但按不出来，日志只有一句「槽位未授权」）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Augment")
	TObjectPtr<UAbilitySet> GrantAbilitySet;
};
