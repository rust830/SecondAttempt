// 斗魂竞技场：装备属性对应表的实现。表本身在 GetSupportedStats() 里。

#include "GAS/ArenaItemStats.h"

#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"

namespace ArenaItemStats
{
	const TArray<FSupportedStat>& GetSupportedStats()
	{
		// 函数内静态：第一次调用时构建，之后直接返回。
		// 【不要挪成文件级全局】—— 属性 getter 和原生标签都要等 GAS 注册完才好使，
		// 文件级全局会在 DLL 加载期就跑，那时原生标签还没注册，拿到的是空标签。
		static const TArray<FSupportedStat> Table = []()
		{
			TArray<FSupportedStat> Rows;

			auto Add = [&Rows](const FGameplayAttribute& Attribute, const FGameplayTag& Tag)
			{
				Rows.Add(FSupportedStat{ Attribute, Tag });
			};

			// 顺序 = UGE_ArenaItem 建修正符的顺序，也是施加时填数值的顺序。
			// 常用属性放前面，纯为了读起来顺眼 —— 顺序本身没有语义。
			Add(UHeroCombatAttributeSet::GetAttackDamageAttribute(),           LOLGameplayTags::Data_Item_AttackDamage);
			Add(UHeroCombatAttributeSet::GetAbilityPowerAttribute(),           LOLGameplayTags::Data_Item_AbilityPower);
			Add(UHeroCombatAttributeSet::GetMaxHealthAttribute(),              LOLGameplayTags::Data_Item_MaxHealth);
			Add(UHeroCombatAttributeSet::GetArmorAttribute(),                  LOLGameplayTags::Data_Item_Armor);
			Add(UHeroCombatAttributeSet::GetMagicResistAttribute(),            LOLGameplayTags::Data_Item_MagicResist);
			Add(UHeroCombatAttributeSet::GetBonusAttackSpeedPercentAttribute(), LOLGameplayTags::Data_Item_AttackSpeed);
			Add(UHeroCombatAttributeSet::GetCritChanceAttribute(),             LOLGameplayTags::Data_Item_CritChance);
			Add(UHeroCombatAttributeSet::GetAbilityHasteAttribute(),           LOLGameplayTags::Data_Item_AbilityHaste);
			Add(UHeroCombatAttributeSet::GetMoveSpeedAttribute(),              LOLGameplayTags::Data_Item_MoveSpeed);
			Add(UHeroCombatAttributeSet::GetOmnivampAttribute(),               LOLGameplayTags::Data_Item_Omnivamp);
			Add(UHeroCombatAttributeSet::GetLifeStealAttribute(),              LOLGameplayTags::Data_Item_LifeSteal);
			Add(UHeroCombatAttributeSet::GetFlatArmorPenAttribute(),           LOLGameplayTags::Data_Item_FlatArmorPen);
			Add(UHeroCombatAttributeSet::GetPercentArmorPenAttribute(),        LOLGameplayTags::Data_Item_PercentArmorPen);
			Add(UHeroCombatAttributeSet::GetFlatMagicPenAttribute(),           LOLGameplayTags::Data_Item_FlatMagicPen);
			Add(UHeroCombatAttributeSet::GetPercentMagicPenAttribute(),        LOLGameplayTags::Data_Item_PercentMagicPen);
			Add(UHeroCombatAttributeSet::GetHealShieldPowerAttribute(),        LOLGameplayTags::Data_Item_HealShieldPower);
			Add(UHeroCombatAttributeSet::GetHealthRegenAttribute(),            LOLGameplayTags::Data_Item_HealthRegen);
			Add(UHeroCombatAttributeSet::GetTenacityAttribute(),               LOLGameplayTags::Data_Item_Tenacity);

			return Rows;
		}();

		return Table;
	}

	const FGameplayTag* FindSetByCallerTag(const FGameplayAttribute& Attribute)
	{
		// 属性可能是空属性（DataAsset 里那一行没填）。空属性不该撞上表里任何一行，
		// 但 FGameplayAttribute 的 operator== 对「两个空属性」返回 true —— 会撞上吗？
		// 不会：表里没有空属性，所以下面这个循环对空属性一定走到底、返回 nullptr。
		if (!Attribute.IsValid())
		{
			return nullptr;
		}

		for (const FSupportedStat& Row : GetSupportedStats())
		{
			if (Row.Attribute == Attribute)
			{
				return &Row.SetByCallerTag;
			}
		}

		return nullptr;
	}

	bool IsSupported(const FGameplayAttribute& Attribute)
	{
		return FindSetByCallerTag(Attribute) != nullptr;
	}
}
