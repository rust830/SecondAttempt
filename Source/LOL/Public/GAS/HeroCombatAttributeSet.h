// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "HeroCombatAttributeSet.generated.h"

class UHeroStatConfig;

#define HERO_ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/**
 * 血量掉到 0 时广播一次。
 *
 * 参数是「谁打的」：DamageInstigator 是施加者的 PlayerState，DamageCauser 是施加者的角色
 * （口径和 UExecCalc_Damage 里的捕获一致）。以后要接击杀提示/记分板，就在这里取。
 *
 * 只在【权威端】广播 —— PostGameplayEffectExecute 在本地预测的客户端上也会跑，
 * 不门住的话每个客户端都会各自挂一份 UGE_Death。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FHeroOutOfHealthSignature, AActor*, DamageInstigator, AActor*, DamageCauser);

/**
 * 英雄主属性集：数值口径见 Desktop/属性集.txt。
 *
 * 三条纪律：
 *  1. **基础值只有一个来源** —— 优先 UHeroStatConfig（每英雄一份，见 HeroStatConfig.h），
 *     没配英雄时退回 .cpp 顶部的 GHeroStatTable（= 默认英雄）。构造函数和 ApplyStats
 *     都读这两个来源，不要在任何地方再写一遍数字。
 *  2. **钳制写在 PreAttributeBaseChange + PreAttributeChange 两处**：
 *     前者管「直接写基础值」（升级、ExecCalc 的 Instant 输出都走这条），
 *     后者管「聚合后的最终值」。只写一处会漏掉另一条路径（引擎注释里明说了要两处都钳）。
 *  3. **Health 是伤害的唯一落点** —— UExecCalc_Damage 往它写负向 modifier。
 */
UCLASS()
class LOL_API UHeroCombatAttributeSet final : public UAttributeSet
{
	GENERATED_BODY()
public:
	UHeroCombatAttributeSet();
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * 死亡判定就在这里面。全项目【唯一】一处「血到 0 之后干什么」的入口：
	 * 之前 Health 归零只被 ClampAttribute 钳到 0，没有任何人知道这件事发生过。
	 */
	virtual void PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data) override;

	/** 血量归零。挂 UGE_Death 的是监听方（AHeroCombatCharacter::HandleOutOfHealth）。 */
	UPROPERTY(BlueprintAssignable, Category="Hero|Vital")
	FHeroOutOfHealthSignature OnOutOfHealth;

	// ---------------------------------------------------------------------
	// 属性集合（「一组属性」的定义，不是某一条属性）
	// ---------------------------------------------------------------------

	/**
	 * 血条 / 能量条读哪几条 —— **唯一定义**。
	 *
	 * Self（本地玩家自己）和 Observed（目标框）两条通道原先是各写一份一模一样的 4 条。
	 * 以后加一条资源（护盾、怒气）时改了一处漏一处，症状是「自己挨打血条动、目标框的不动」，
	 * 而且只在有目标的时候才暴露。两处都调它，这类漏改就不存在了。
	 *
	 * 【头顶血条不走这里，那是有意的】：它只要血、不要能量，见
	 * UHeroOverlayHealthComponent::Bind 里那份两条的列表。别顺手改成调用这里 —— 那样
	 * 每个小兵头顶都会多绑两条永远不看的属性。
	 *
	 * 返回 const 引用而不是值：调用方都是拿去绑委托，复制一份 TArray 没有意义。
	 * 用函数内 static 局部量（不是文件作用域）—— 避开「静态初始化期去问 UPROPERTY」的顺序问题。
	 */
	static const TArray<FGameplayAttribute>& GetVitalsAttributes();

	// ---------------------------------------------------------------------
	// 资源
	// ---------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Health, Category="Hero|Vital") FGameplayAttributeData Health;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, Health)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_MaxHealth, Category="Hero|Vital") FGameplayAttributeData MaxHealth;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, MaxHealth)
	/** 每 5 秒回复量（属性集口径），换算成每秒 = /5。没有回复逻辑，先只存数值。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_HealthRegen, Category="Hero|Vital") FGameplayAttributeData HealthRegen;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, HealthRegen)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Energy, Category="Hero|Vital") FGameplayAttributeData Energy;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, Energy)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_MaxEnergy, Category="Hero|Vital") FGameplayAttributeData MaxEnergy;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, MaxEnergy)
	/** 每 5 秒回复量。ManaCost 目前恒为 0，等成本 GE 接上再启用。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_EnergyRegen, Category="Hero|Vital") FGameplayAttributeData EnergyRegen;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, EnergyRegen)

	// ---------------------------------------------------------------------
	// 输出
	// ---------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_AttackDamage, Category="Hero|Offense") FGameplayAttributeData AttackDamage;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, AttackDamage)
	/**
	 * 法术强度。基础值 0、无成长（属性集.txt 里没有这一条，LoL 的法强也全靠装备/技能堆）。
	 *
	 * 【目前只是数据】：魔法伤害的公式还是「攻击力 × 倍率」，没有把法强接进去 ——
	 * 接了法强的技能出现时，改的是 UExecCalc_Damage 里的 ① 那一行，不是这里。
	 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_AbilityPower, Category="Hero|Offense") FGameplayAttributeData AbilityPower;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, AbilityPower)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_BaseAttackSpeed, Category="Hero|Offense") FGameplayAttributeData BaseAttackSpeed;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, BaseAttackSpeed)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_AttackSpeedRatio, Category="Hero|Offense") FGameplayAttributeData AttackSpeedRatio;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, AttackSpeedRatio)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_BonusAttackSpeedPercent, Category="Hero|Offense") FGameplayAttributeData BonusAttackSpeedPercent;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, BonusAttackSpeedPercent)
	/** 派生值，不进数值表：BaseAttackSpeed + AttackSpeedRatio × BonusAttackSpeedPercent。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_FinalAttackSpeed, Category="Hero|Offense") FGameplayAttributeData FinalAttackSpeed;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, FinalAttackSpeed)

	/**
	 * 暴击率，0~1（0.25 = 25%）。默认 0 = 永远不暴击。
	 *
	 * 【谁能暴击由施加方声明】：UExecCalc_Damage 只在 Spec 上带了 Data.CanCrit 时才 roll，
	 * 普攻（GA_ThreeHitPassive::ApplyServerHit）会带，技能不带 —— 和 LoL 一致。
	 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_CritChance, Category="Hero|Offense") FGameplayAttributeData CritChance;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, CritChance)
	/**
	 * 暴击伤害【倍率】，默认 2.0 = 200%。暴击时 Damage × 它。
	 *
	 * 存倍率而不是存百分数（和 BonusAttackSpeedPercent 存 0.5 一个口径）：
	 * 「+10% 暴击伤害」的 GE 因此就是一条 Additive +0.1，不用在两边各写一次 /100。
	 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_CritDamage, Category="Hero|Offense") FGameplayAttributeData CritDamage;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, CritDamage)

	/** 固定穿透（先算百分比再算固定，见 属性集.txt）。穿透本身没接进伤害公式，先存数值。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_FlatArmorPen, Category="Hero|Offense") FGameplayAttributeData FlatArmorPen;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, FlatArmorPen)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_PercentArmorPen, Category="Hero|Offense") FGameplayAttributeData PercentArmorPen;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, PercentArmorPen)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_FlatMagicPen, Category="Hero|Offense") FGameplayAttributeData FlatMagicPen;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, FlatMagicPen)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_PercentMagicPen, Category="Hero|Offense") FGameplayAttributeData PercentMagicPen;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, PercentMagicPen)

	/**
	 * 全能吸血：所有伤害都按比例回血（0.15 = 造成的伤害回 15%）。
	 * 结算在 UExecCalc_Damage ⑤ —— 按【最终伤害】（减抗/暴击/格挡之后）算。
	 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Omnivamp, Category="Hero|Offense") FGameplayAttributeData Omnivamp;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, Omnivamp)
	/** 生命偷取：只有普攻（挂了 Data.BasicAttack 的伤害）回血，其余同全能吸血。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_LifeSteal, Category="Hero|Offense") FGameplayAttributeData LifeSteal;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, LifeSteal)
	/** 治疗与护盾强度加成。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_HealShieldPower, Category="Hero|Offense") FGameplayAttributeData HealShieldPower;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, HealShieldPower)

	// ---------------------------------------------------------------------
	// 防御
	// ---------------------------------------------------------------------

	/** 物理免伤 = Armor/(Armor+100)。UExecCalc_Damage 读它。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Armor, Category="Hero|Defense") FGameplayAttributeData Armor;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, Armor)
	/** 魔法免伤 = MagicResist/(MagicResist+100)。UExecCalc_Damage 读它。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_MagicResist, Category="Hero|Defense") FGameplayAttributeData MagicResist;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, MagicResist)
	/** 控制时长变为 (1 - Tenacity) × 原时长。没有控制效果，先存数值。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_Tenacity, Category="Hero|Defense") FGameplayAttributeData Tenacity;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, Tenacity)

	// ---------------------------------------------------------------------
	// 通用
	// ---------------------------------------------------------------------

	/** 目前只是数据，没有接 CharacterMovement->MaxWalkSpeed。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_MoveSpeed, Category="Hero|Utility") FGameplayAttributeData MoveSpeed;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, MoveSpeed)
	/** 目前只是数据。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_AttackRange, Category="Hero|Utility") FGameplayAttributeData AttackRange;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, AttackRange)
	/** 技能急速：CD = 原CD × 100/(100+AbilityHaste)。由 UMyGameplayAbility::ApplyCooldown 读。 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing=OnRep_AbilityHaste, Category="Hero|Utility") FGameplayAttributeData AbilityHaste;
	HERO_ATTRIBUTE_ACCESSORS(UHeroCombatAttributeSet, AbilityHaste)

	/**
	 * 按【英雄数值表 + 等级】把基础值重算一遍。1 级 = 只吃 Base，和构造函数刚建出来的结果一致。
	 *
	 * 两个性质，动它的时候必须保住：
	 *  1. **幂等**：每次都是 `Base + PerLevel × (Level-1)` 从表重算，不是 `+=` 增量。
	 *     升级、复活、重连、切关卡都会重放它 —— 写成增量就会一遍遍叠加。
	 *  2. **只写基础值**：不动 GE 修正符。所以装备、减速这些不受影响（它们改的是聚合后的值）。
	 *
	 * 写成普通成员函数而不是 GE：升级是「重设基础值」，不是可被驱散/叠加的状态。
	 *
	 * @param Config  这个英雄的数值表。nullptr / 空表 → 退回 GHeroStatTable（默认英雄）。
	 * @param Level   英雄等级，1 起。0 或负数按 1 处理。
	 *
	 * 谁来调：只有 AHeroCombatCharacter::ApplyChampionStats（服务端、授予技能组的同一处）。
	 * 构造函数【不能】调 —— 属性集的构造函数读不到英雄配置（那一刻没有 Pawn、没有英雄身份）。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero")
	void ApplyStats(const UHeroStatConfig* Config, int32 Level);

	/**
	 * 没配英雄数值表时的手动入口：等价于 ApplyStats(nullptr, Level)，用内置兜底表。
	 *
	 * 留着它是因为 BP 里可能有节点在调（它是 BlueprintCallable）。新代码一律走 ApplyStats。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero")
	void ApplyChampionLevel(int32 Level);

	UFUNCTION() void OnRep_Health(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_HealthRegen(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_Energy(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_MaxEnergy(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_EnergyRegen(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_AttackDamage(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_AbilityPower(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_BaseAttackSpeed(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_AttackSpeedRatio(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_BonusAttackSpeedPercent(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_FinalAttackSpeed(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_CritChance(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_CritDamage(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_FlatArmorPen(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_PercentArmorPen(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_FlatMagicPen(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_PercentMagicPen(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_Omnivamp(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_LifeSteal(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_HealShieldPower(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_Armor(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_MagicResist(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_Tenacity(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_MoveSpeed(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_AttackRange(const FGameplayAttributeData& OldValue);
	UFUNCTION() void OnRep_AbilityHaste(const FGameplayAttributeData& OldValue);

private:
	/** 攻速那三条改了就重算派生的 FinalAttackSpeed。 */
	void RecalculateFinalAttackSpeed();

	/** FinalAttackSpeed 的唯一公式（构造函数和 Recalculate 共用，别写成两份）。 */
	float ComputeFinalAttackSpeed() const;

	/** 两条钳制路径共用的规则表。 */
	void ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const;
};
