// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

namespace
{
	/**
	 * 数值表的一行：1 级基础值 + 每级成长。
	 *
	 * Base 和 PerLevel 就是 Desktop/属性集.txt 里那两列。改数值只改这张表 ——
	 * 构造函数和 ApplyChampionLevel 都从这里读，不存在「基础值写在两个地方」的问题。
	 */
	struct FHeroStatRow
	{
		/** 例如 &UHeroCombatAttributeSet::GetHealthAttribute。 */
		FGameplayAttribute (*GetAttribute)();
		/** 例如 &UHeroCombatAttributeSet::InitHealth（GAMEPLAYATTRIBUTE_VALUE_INITTER 生成的）。 */
		void (UHeroCombatAttributeSet::*InitBase)(float);
		float Base;
		float PerLevel;
	};

	/** 属性集.txt 的落表。FinalAttackSpeed 不在表里：它是派生值，见 UHeroCombatAttributeSet()。 */
	const FHeroStatRow GHeroStatTable[] =
	{
		{ &UHeroCombatAttributeSet::GetHealthAttribute,                &UHeroCombatAttributeSet::InitHealth,                600.f,   119.f  },
		{ &UHeroCombatAttributeSet::GetMaxHealthAttribute,             &UHeroCombatAttributeSet::InitMaxHealth,             600.f,   119.f  },
		{ &UHeroCombatAttributeSet::GetHealthRegenAttribute,           &UHeroCombatAttributeSet::InitHealthRegen,             9.f,     0.9f },
		{ &UHeroCombatAttributeSet::GetEnergyAttribute,                &UHeroCombatAttributeSet::InitEnergy,                200.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetMaxEnergyAttribute,             &UHeroCombatAttributeSet::InitMaxEnergy,             200.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetEnergyRegenAttribute,           &UHeroCombatAttributeSet::InitEnergyRegen,            50.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetAttackDamageAttribute,          &UHeroCombatAttributeSet::InitAttackDamage,           62.f,     3.3f },
		{ &UHeroCombatAttributeSet::GetBaseAttackSpeedAttribute,       &UHeroCombatAttributeSet::InitBaseAttackSpeed,         0.625f,   0.f  },
		{ &UHeroCombatAttributeSet::GetAttackSpeedRatioAttribute,      &UHeroCombatAttributeSet::InitAttackSpeedRatio,        0.625f,   0.f  },
		{ &UHeroCombatAttributeSet::GetBonusAttackSpeedPercentAttribute, &UHeroCombatAttributeSet::InitBonusAttackSpeedPercent, 0.f,  0.032f },
		{ &UHeroCombatAttributeSet::GetFlatArmorPenAttribute,          &UHeroCombatAttributeSet::InitFlatArmorPen,            0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetPercentArmorPenAttribute,       &UHeroCombatAttributeSet::InitPercentArmorPen,         0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetFlatMagicPenAttribute,          &UHeroCombatAttributeSet::InitFlatMagicPen,            0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetPercentMagicPenAttribute,       &UHeroCombatAttributeSet::InitPercentMagicPen,         0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetOmnivampAttribute,              &UHeroCombatAttributeSet::InitOmnivamp,                0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetLifeStealAttribute,             &UHeroCombatAttributeSet::InitLifeSteal,               0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetHealShieldPowerAttribute,       &UHeroCombatAttributeSet::InitHealShieldPower,         0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetArmorAttribute,                 &UHeroCombatAttributeSet::InitArmor,                  23.f,     4.7f },
		{ &UHeroCombatAttributeSet::GetMagicResistAttribute,           &UHeroCombatAttributeSet::InitMagicResist,            37.f,     2.05f },
		{ &UHeroCombatAttributeSet::GetTenacityAttribute,              &UHeroCombatAttributeSet::InitTenacity,                0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetMoveSpeedAttribute,             &UHeroCombatAttributeSet::InitMoveSpeed,             345.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetAttackRangeAttribute,           &UHeroCombatAttributeSet::InitAttackRange,           125.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetAbilityHasteAttribute,          &UHeroCombatAttributeSet::InitAbilityHaste,            0.f,     0.f  },
	};

	/** 攻速下限：0.01 而不是 0，避免用它做分母/乘除时炸掉。 */
	constexpr float MinAttackSpeed = 0.01f;
}

UHeroCombatAttributeSet::UHeroCombatAttributeSet()
{
	// 基础值只有这一处来源：上面的 GHeroStatTable。
	// 注意这里【不能】用 SetXxx()：那是走 ASC 的（GetOwningAbilitySystemComponent 在 CDO 构造
	// 阶段是 null，引擎的 SetNumericAttributeBase 会直接 ensure 失败）。Init 是直接写字段的。
	for (const FHeroStatRow& Row : GHeroStatTable)
	{
		(this->*Row.InitBase)(Row.Base);
	}

	// 派生值：用和 RecalculateFinalAttackSpeed 同一个公式算（读的是刚写进去的字段，不碰 ASC）。
	InitFinalAttackSpeed(ComputeFinalAttackSpeed());
}

float UHeroCombatAttributeSet::ComputeFinalAttackSpeed() const
{
	return FMath::Max(MinAttackSpeed, GetBaseAttackSpeed() + GetAttackSpeedRatio() * GetBonusAttackSpeedPercent());
}

void UHeroCombatAttributeSet::ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const
{
	// 血量/能量上下限：伤害扣血靠的就是 Health 的下限 0。
	// 不钳的话负血会一路传下去，HUD 上难看，以后做死亡判定也会判错。
	if (Attribute == GetHealthAttribute())        { NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth()); return; }
	if (Attribute == GetEnergyAttribute())        { NewValue = FMath::Clamp(NewValue, 0.f, GetMaxEnergy()); return; }
	if (Attribute == GetMaxHealthAttribute())     { NewValue = FMath::Max(0.f, NewValue); return; }
	if (Attribute == GetMaxEnergyAttribute())     { NewValue = FMath::Max(0.f, NewValue); return; }
	if (Attribute == GetHealthRegenAttribute())   { NewValue = FMath::Max(0.f, NewValue); return; }
	if (Attribute == GetEnergyRegenAttribute())   { NewValue = FMath::Max(0.f, NewValue); return; }

	// 输出/防御不该是负的。
	if (Attribute == GetAttackDamageAttribute() ||
		Attribute == GetFlatArmorPenAttribute() || Attribute == GetFlatMagicPenAttribute() ||
		Attribute == GetOmnivampAttribute()     || Attribute == GetLifeStealAttribute() ||
		Attribute == GetHealShieldPowerAttribute())
	{
		NewValue = FMath::Max(0.f, NewValue);
		return;
	}

	// 百分比类：0~1。穿透（先百分比再固定）和韧性 (1-韧性)×时长 都要求落在这个区间，
	// 超出区间算出来的免伤/控制时长没有意义（负的控制时长反而是「加速」）。
	if (Attribute == GetPercentArmorPenAttribute() || Attribute == GetPercentMagicPenAttribute() ||
		Attribute == GetTenacityAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, 1.f);
		return;
	}

	if (Attribute == GetBaseAttackSpeedAttribute() || Attribute == GetAttackSpeedRatioAttribute())
	{
		NewValue = FMath::Max(MinAttackSpeed, NewValue);
		return;
	}
	if (Attribute == GetBonusAttackSpeedPercentAttribute()) { NewValue = FMath::Max(-0.99f, NewValue); return; }

	if (Attribute == GetMoveSpeedAttribute() || Attribute == GetAttackRangeAttribute() ||
		Attribute == GetAbilityHasteAttribute())
	{
		NewValue = FMath::Max(0.f, NewValue);
		return;
	}
}

void UHeroCombatAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	ClampAttribute(Attribute, NewValue);
}

void UHeroCombatAttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);
	// 直接写基础值的路径（升级、Instant GE 的输出 modifier）走的是这条，不是上面那条。
	// 引擎注释里明说了要两处都钳 —— 只写一处会漏掉另一条路径。
	ClampAttribute(Attribute, NewValue);
}

void UHeroCombatAttributeSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	Super::PostAttributeChange(Attribute, OldValue, NewValue);
	if (Attribute == GetBaseAttackSpeedAttribute() || Attribute == GetAttackSpeedRatioAttribute() || Attribute == GetBonusAttackSpeedPercentAttribute()) RecalculateFinalAttackSpeed();
}

void UHeroCombatAttributeSet::RecalculateFinalAttackSpeed()
{
	SetFinalAttackSpeed(ComputeFinalAttackSpeed());
}

void UHeroCombatAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	// 只有 Health 会死人。Energy/攻速那些走到这里直接出去，不做任何判断。
	if (Data.EvaluatedData.Attribute != GetHealthAttribute()) return;

	// 【只认权威端】。PostGameplayEffectExecute 在本地预测的客户端上也会跑（客户端预测的那一下伤害），
	// 不门住的话每个客户端都会各自挂一份 UGE_Death、各自跑一次重生。
	const UAbilitySystemComponent* ASC = GetOwningAbilitySystemComponent();
	if (!ASC || !ASC->IsOwnerActorAuthoritative()) return;

	// 到这里 Health 已经是扣完并钳过的值（PostGameplayEffectExecute 的语义就是「改完之后」），
	// 不用自己去减 Damage。
	if (GetHealth() > 0.f) return;

	// 已经死了就别再广播：一次多段伤害的每一跳、或者同一帧里的两下，都会走到这儿。
	// 监听方（AHeroCombatCharacter::HandleOutOfHealth）自己还有一道同样的判断，两处都留着 ——
	// 广播本身是廉价的，重复挂 GE_Death 不是。
	if (ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Dead)) return;

	const FGameplayEffectContextHandle& Context = Data.EffectSpec.GetContext();

	UE_LOG(LogTemp, Warning, TEXT("[Death] %s 生命归零（凶手=%s 施加者=%s）→ 广播 OnOutOfHealth"),
		*GetNameSafe(ASC->GetAvatarActor()),
		*GetNameSafe(Context.GetEffectCauser()), *GetNameSafe(Context.GetInstigator()));

	OnOutOfHealth.Broadcast(Context.GetInstigator(), Context.GetEffectCauser());
}

void UHeroCombatAttributeSet::ApplyChampionLevel(int32 Level)
{
	// 等级从 1 起：1 级 = 只吃基础值，和构造函数刚建出来的结果完全一致。
	const int32 LevelsGained = FMath::Max(0, Level - 1);

	UAbilitySystemComponent* ASC = GetOwningAbilitySystemComponent();
	if (!ASC)
	{
		// CDO / 还没 InitAbilityActorInfo 时会走到这里。静默 return 的话，调用方会以为升级生效了。
		UE_LOG(LogTemp, Warning, TEXT("[Attributes] ApplyChampionLevel(%d) 时拿不到 ASC → 升级未生效"), Level);
		return;
	}

	for (const FHeroStatRow& Row : GHeroStatTable)
	{
		ASC->SetNumericAttributeBase(Row.GetAttribute(), Row.Base + Row.PerLevel * LevelsGained);
	}

	// 上面把 MaxHealth 也重算了。当前 Health 的【基础值】跟着表走了，但如果 MaxHealth 变小
	// （改表、降级），当前值必须一起压下来 —— PreAttributeBaseChange 里的钳制只管「正在写的那一条」属性，
	// 管不到「另一条属性变小了，我这条要跟着收敛」这种情况。
	ASC->SetNumericAttributeBase(GetHealthAttribute(), FMath::Min(GetHealth(), GetMaxHealth()));
	ASC->SetNumericAttributeBase(GetEnergyAttribute(), FMath::Min(GetEnergy(), GetMaxEnergy()));
}

#define HERO_DEFINE_REPNOTIFY(PropertyName) \
	void UHeroCombatAttributeSet::OnRep_##PropertyName(const FGameplayAttributeData& OldValue) \
	{ \
		GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, PropertyName, OldValue); \
	}

HERO_DEFINE_REPNOTIFY(Health)
HERO_DEFINE_REPNOTIFY(MaxHealth)
HERO_DEFINE_REPNOTIFY(HealthRegen)
HERO_DEFINE_REPNOTIFY(Energy)
HERO_DEFINE_REPNOTIFY(MaxEnergy)
HERO_DEFINE_REPNOTIFY(EnergyRegen)
HERO_DEFINE_REPNOTIFY(FlatArmorPen)
HERO_DEFINE_REPNOTIFY(PercentArmorPen)
HERO_DEFINE_REPNOTIFY(FlatMagicPen)
HERO_DEFINE_REPNOTIFY(PercentMagicPen)
HERO_DEFINE_REPNOTIFY(Omnivamp)
HERO_DEFINE_REPNOTIFY(LifeSteal)
HERO_DEFINE_REPNOTIFY(HealShieldPower)
HERO_DEFINE_REPNOTIFY(Armor)
HERO_DEFINE_REPNOTIFY(MagicResist)
HERO_DEFINE_REPNOTIFY(Tenacity)
HERO_DEFINE_REPNOTIFY(MoveSpeed)
HERO_DEFINE_REPNOTIFY(AttackRange)
HERO_DEFINE_REPNOTIFY(AbilityHaste)

// 攻速那三条的 OnRep 要顺带重算派生值，所以不套上面的宏。
void UHeroCombatAttributeSet::OnRep_AttackDamage(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, AttackDamage, OldValue);
}

void UHeroCombatAttributeSet::OnRep_BaseAttackSpeed(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, BaseAttackSpeed, OldValue); RecalculateFinalAttackSpeed();
}

void UHeroCombatAttributeSet::OnRep_AttackSpeedRatio(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, AttackSpeedRatio, OldValue); RecalculateFinalAttackSpeed();
}

void UHeroCombatAttributeSet::OnRep_BonusAttackSpeedPercent(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, BonusAttackSpeedPercent, OldValue); RecalculateFinalAttackSpeed();
}

void UHeroCombatAttributeSet::OnRep_FinalAttackSpeed(const FGameplayAttributeData& OldValue) {
	GAMEPLAYATTRIBUTE_REPNOTIFY(UHeroCombatAttributeSet, FinalAttackSpeed, OldValue);
}

#undef HERO_DEFINE_REPNOTIFY

void UHeroCombatAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 全部 COND_None + REPNOTIFY_Always：属性是两端都要看的权威值，
	// 而且要保证「值没变但语义变了」（比如 GE 覆盖）时客户端也收得到通知。
#define HERO_REPLICATE(PropertyName) DOREPLIFETIME_CONDITION_NOTIFY(UHeroCombatAttributeSet, PropertyName, COND_None, REPNOTIFY_Always)

	HERO_REPLICATE(Health);
	HERO_REPLICATE(MaxHealth);
	HERO_REPLICATE(HealthRegen);
	HERO_REPLICATE(Energy);
	HERO_REPLICATE(MaxEnergy);
	HERO_REPLICATE(EnergyRegen);
	HERO_REPLICATE(AttackDamage);
	HERO_REPLICATE(BaseAttackSpeed);
	HERO_REPLICATE(AttackSpeedRatio);
	HERO_REPLICATE(BonusAttackSpeedPercent);
	HERO_REPLICATE(FinalAttackSpeed);
	HERO_REPLICATE(FlatArmorPen);
	HERO_REPLICATE(PercentArmorPen);
	HERO_REPLICATE(FlatMagicPen);
	HERO_REPLICATE(PercentMagicPen);
	HERO_REPLICATE(Omnivamp);
	HERO_REPLICATE(LifeSteal);
	HERO_REPLICATE(HealShieldPower);
	HERO_REPLICATE(Armor);
	HERO_REPLICATE(MagicResist);
	HERO_REPLICATE(Tenacity);
	HERO_REPLICATE(MoveSpeed);
	HERO_REPLICATE(AttackRange);
	HERO_REPLICATE(AbilityHaste);

#undef HERO_REPLICATE
}
