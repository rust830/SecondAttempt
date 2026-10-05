// 斗魂竞技场：装备 / 海克斯能改哪些属性 —— 属性 ↔ SetByCaller 标签的对应表。
//
// ===========================================================================
// 这份表是【唯一真相源】，两处同时依赖它，改的时候两处一起动：
//
//   ① UGE_ArenaItem 的构造函数 —— 按表逐条建修正符（每条一个 SetByCaller）。
//   ② UArenaLoadoutComponent —— 施加时按表逐条填数值。
//
// 两处都由这张表生成，所以不存在「GE 上加了修正符但这边忘了加标签」的漂移。
// 反过来说：**要支持一条新属性，只需要在这张表里加一行**，GE 和施加方自动跟上。
// ===========================================================================
//
// 【为什么全是加算（Additive）】属性集把「百分比类」的东西各自单列成了 0~1 的属性
// （CritChance / Omnivamp / LifeSteal / 各种百分比穿透 / HealShieldPower / Tenacity），
// 攻速也单列了 BonusAttackSpeedPercent（0.15 = +15%）。所以装备想表达的任何加成，
// 落到属性上都是「加一个数」，没有一条需要乘法。
//
// ⚠️ 因此这里【没有 ModifierOp 字段】—— 加了它也是个没人读的字段，那种字段比没有更坏。
// 真需要乘算的装备，去做一个 UGE_ArenaItem 的子类，别往这张表里塞。
//
// 【MaxHealth 的坑】装备加的最大生命是 GE 修正符，只抬上限，**不会回当前血量**。
// LoL 里买到 +200 生命的装备是当场回 200 血的，这里不会 —— 想对齐得在装备生效后
// 补一次回血。属性集那边 ApplyStats 用「当前值跟 Max 的差值走」处理升级，
// 但那条路只认 base 值，不走修正符（见 HeroCombatAttributeSet.cpp 的 ApplyStats）。
//
// 放 GAS/ 的理由同 HeroStatConfig.h：持有 FGameplayAttribute。

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "ArenaItemStats.generated.h"

/**
 * 装备 / 海克斯身上的一条属性加成。
 *
 * 【为什么存 FGameplayAttribute 而不是自制枚举】和 HeroStatConfig.h 的边界 1 同一个道理：
 * 属性名是 schema，值得编译期/编辑器检查。自制枚举等于把「支持哪些属性」这个决定
 * 从属性集那边抄一份过来，两边迟早对不上。
 *
 * 但注意：**填了表里没有的属性 = 这条不生效**（会在施加时记一条 Warning）。
 * 能加什么，仍然由 ArenaItemStats 那张表说了算 —— 这是有意的，
 * 因为 UGE_ArenaItem 的修正符表是 C++ 建的死表，表外的属性它根本没有修正符可挂。
 */
USTRUCT(BlueprintType)
struct FArenaItemStatModifier
{
	GENERATED_BODY()

	/** 加哪条属性。留空 = 这一行无效（施加时跳过并记日志）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Item")
	FGameplayAttribute Attribute;

	/**
	 * 加多少。口径跟属性走：攻速填 0.15（= +15%），暴击填 0.25（= +25%），
	 * 攻击力/护甲/生命这些就是字面值。可以为负（做「用血量换攻击力」那类装备）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|Item")
	float Value = 0.f;
};

namespace ArenaItemStats
{
	/** 表里的一行：一条属性，以及它在 UGE_ArenaItem 上对应的 SetByCaller 键。 */
	struct FSupportedStat
	{
		FGameplayAttribute Attribute;
		FGameplayTag SetByCallerTag;
	};

	/**
	 * 全部支持的属性，顺序即 UGE_ArenaItem 建修正符的顺序。
	 * 静态表，DLL 加载后第一次调用时构建一次。
	 */
	LOL_API const TArray<FSupportedStat>& GetSupportedStats();

	/**
	 * 查一条属性对应的 SetByCaller 键。不在表里返回 nullptr。
	 *
	 * 调用方拿到 nullptr 应该记日志并跳过 —— 静默跳过会让「装备没生效」
	 * 变成一个只能靠读 C++ 才能回答的问题。
	 */
	LOL_API const FGameplayTag* FindSetByCallerTag(const FGameplayAttribute& Attribute);

	/** 一条属性是否在支持列表里。 */
	LOL_API bool IsSupported(const FGameplayAttribute& Attribute);
}
