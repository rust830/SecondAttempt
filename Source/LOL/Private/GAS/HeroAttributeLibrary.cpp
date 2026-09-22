// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroAttributeLibrary.h"

#include "AbilitySystemComponent.h"
#include "GameFramework/Actor.h"

#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/MyAbilitySystemComponent.h"

namespace
{
	/**
	 * 从 Actor 找到英雄属性集子对象。找不到返回 nullptr。
	 *
	 * 用 GetSet<T>() 而不是逐个 GetNumericAttribute()：前者是按类型取 subobject（一次），
	 * 后者每次都要拿 FGameplayAttribute 去查一遍映射表（24 次）。而且 GetSet 的语义正好是
	 * 「这个 ASC 上确实注册了这类属性集」—— 不做任何猜测，读不到就是读不到。
	 */
	const UHeroCombatAttributeSet* ResolveHeroAttributeSet(const AActor* Source)
	{
		if (!Source)
		{
			return nullptr;
		}

		const UAbilitySystemComponent* ASC = UMyAbilitySystemComponent::FindAbilitySystemComponent(Source);
		if (!ASC)
		{
			return nullptr;
		}

		return ASC->GetSet<UHeroCombatAttributeSet>();
	}
}

FHeroAttributeView UHeroAttributeLibrary::GetHeroAttributes(const AActor* Source)
{
	FHeroAttributeView View;

	const UHeroCombatAttributeSet* Set = ResolveHeroAttributeSet(Source);
	if (!Set)
	{
		// bValid 保持 false —— 调用方据此跳过显示，而不是把 0 当成「血是 0」。
		return View;
	}

	View.bValid = true;

	// 顺序与 FHeroAttributeView 的声明保持一致，两边对照着改。
	View.Health = Set->GetHealth();
	View.MaxHealth = Set->GetMaxHealth();
	View.HealthRegen = Set->GetHealthRegen();
	View.Energy = Set->GetEnergy();
	View.MaxEnergy = Set->GetMaxEnergy();
	View.EnergyRegen = Set->GetEnergyRegen();

	View.AttackDamage = Set->GetAttackDamage();
	View.BaseAttackSpeed = Set->GetBaseAttackSpeed();
	View.AttackSpeedRatio = Set->GetAttackSpeedRatio();
	View.BonusAttackSpeedPercent = Set->GetBonusAttackSpeedPercent();
	View.FinalAttackSpeed = Set->GetFinalAttackSpeed();

	View.FlatArmorPen = Set->GetFlatArmorPen();
	View.PercentArmorPen = Set->GetPercentArmorPen();
	View.FlatMagicPen = Set->GetFlatMagicPen();
	View.PercentMagicPen = Set->GetPercentMagicPen();

	View.Omnivamp = Set->GetOmnivamp();
	View.LifeSteal = Set->GetLifeSteal();
	View.HealShieldPower = Set->GetHealShieldPower();

	View.Armor = Set->GetArmor();
	View.MagicResist = Set->GetMagicResist();
	View.Tenacity = Set->GetTenacity();

	View.MoveSpeed = Set->GetMoveSpeed();
	View.AttackRange = Set->GetAttackRange();
	View.AbilityHaste = Set->GetAbilityHaste();

	return View;
}

bool UHeroAttributeLibrary::HasHeroAttributes(const AActor* Source)
{
	return ResolveHeroAttributeSet(Source) != nullptr;
}
