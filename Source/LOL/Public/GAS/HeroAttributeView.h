// 英雄属性集的「只读数值快照」：24 条属性的当前值，一个字段一条，全是裸数值。
//
// 【为什么不带百分比】
// 这个结构体回答的是「数值是多少」，不是「进度条画多长」。两件事分开：
//   · 画进度条 → FHUDVitalsView（UI/HUDTypes.h）带 HealthPercent / EnergyPercent，
//                因为那是 UI 语义（1 = 满，直接喂 ProgressBar），而且是事件推的。
//   · 问数值   → 这里。谁需要比率谁自己除一次（Armor/(Armor+100) 之类的公式本来也在别处）。
//
//   一旦给每个资源都挂一个 *Percent，就多出 6 个必须跟数值同步维护的字段，
//   而它们的含义还会随「分母是什么」漂移（当前血 / 最大血？还是当前血 / 满级血？）。
//
// 【为什么不是 24 个独立 getter】
// 一次调用拿全量，蓝图侧只多一个 Break 节点。换成 24 个函数就是 24 个节点、
// 24 次 ASC 子对象查找，而且每加一条属性都要补一个接口。
//
// 【零 GAS 类型】
// 这个头文件不 include 任何 GAS 头，字段只有 float / bool，所以 UI 层可以安全引用 ——
// 和 HUDTypes.h 的纪律不冲突：那边管的是「UI 的接口里不出现 FGameplayAttribute /
// FGameplayTag」，不是「UI 不能碰 float」。
//
// 字段顺序对齐 UHeroCombatAttributeSet 的声明顺序（资源 → 输出 → 防御 → 通用），
// 加属性时两边一起改，方便对照。
//
// 拿数据的入口只有一个：UHeroAttributeLibrary::GetHeroAttributes()。别绕过它自己
// 去 GetNumericAttribute —— 那样「怎么从 Actor 找到 ASC」这件事就有了第二份定义。

#pragma once

#include "CoreMinimal.h"
#include "HeroAttributeView.generated.h"

USTRUCT(BlueprintType)
struct LOL_API FHeroAttributeView
{
	GENERATED_BODY()

	/**
	 * false = 这个 Actor 上读不到属性（没有 ASC / ASC 上没挂 UHeroCombatAttributeSet /
	 * Actor 为空）。此时其余字段【全是 0】，不是「血是 0」—— 用法上应当直接跳过显示，
	 * 而不是画一条空血条。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Attributes")
	bool bValid = false;

	// -----------------------------------------------------------------------
	// 资源（Hero|Vital）
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "Hero|Vital") float Health = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Vital") float MaxHealth = 0.f;
	/** 每 5 秒回复量（属性集口径），换算成每秒要 /5。 */
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Vital") float HealthRegen = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Vital") float Energy = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Vital") float MaxEnergy = 0.f;
	/** 每 5 秒回复量，同上。 */
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Vital") float EnergyRegen = 0.f;

	// -----------------------------------------------------------------------
	// 输出（Hero|Offense）
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float AttackDamage = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float BaseAttackSpeed = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float AttackSpeedRatio = 0.f;
	/** 攻速加成百分比（0.5 = +50%）。它本身就是个「数值」，不是我们派生出来的比率。 */
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float BonusAttackSpeedPercent = 0.f;
	/** 派生值：BaseAttackSpeed + AttackSpeedRatio × BonusAttackSpeedPercent。 */
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float FinalAttackSpeed = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float FlatArmorPen = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float PercentArmorPen = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float FlatMagicPen = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float PercentMagicPen = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float Omnivamp = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float LifeSteal = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Offense") float HealShieldPower = 0.f;

	// -----------------------------------------------------------------------
	// 防御（Hero|Defense）
	// -----------------------------------------------------------------------

	/** 免伤率是派生量（Armor/(Armor+100)），本结构体【故意不给】—— 见文件头。 */
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Defense") float Armor = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Defense") float MagicResist = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Defense") float Tenacity = 0.f;

	// -----------------------------------------------------------------------
	// 通用（Hero|Utility）
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "Hero|Utility") float MoveSpeed = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Utility") float AttackRange = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Hero|Utility") float AbilityHaste = 0.f;
};
