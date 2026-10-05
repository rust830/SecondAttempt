// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/HeroStatConfig.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

namespace
{
	/**
	 * 兜底数值表的一行：1 级基础值 + 每级成长。
	 *
	 * 这张表就是「默认英雄」（= 没配 UHeroStatConfig 时用的那一份），构造函数和
	 * ApplyStats 都从这里读。给某个英雄一份 UHeroStatConfig 就会盖掉它。
	 *
	 * 【为什么不用 FHeroStatEntry（那份 DA 的行）】：那一行存 FGameplayAttribute，
	 * 而 FGameplayAttribute 要查 UClass 的属性表 —— CDO 构造阶段不能碰（见构造函数里的注释）。
	 * 这里存成员函数指针，CDO 和运行期都能用。
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
		// 蓝量回复：50 → 44，给「完美闪避回蓝」腾出价值空间（一次完美闪避回 40，约等于
		// 少掉的 6/s 攒 6.7 秒）。只动基础值，装备/符文那套成长加成（Growth 列）不受影响。
		{ &UHeroCombatAttributeSet::GetEnergyRegenAttribute,           &UHeroCombatAttributeSet::InitEnergyRegen,            44.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetAttackDamageAttribute,          &UHeroCombatAttributeSet::InitAttackDamage,           62.f,     3.3f },
		{ &UHeroCombatAttributeSet::GetAbilityPowerAttribute,          &UHeroCombatAttributeSet::InitAbilityPower,            0.f,     0.f  },
		{ &UHeroCombatAttributeSet::GetBaseAttackSpeedAttribute,       &UHeroCombatAttributeSet::InitBaseAttackSpeed,         0.625f,   0.f  },
		{ &UHeroCombatAttributeSet::GetAttackSpeedRatioAttribute,      &UHeroCombatAttributeSet::InitAttackSpeedRatio,        0.625f,   0.f  },
		{ &UHeroCombatAttributeSet::GetBonusAttackSpeedPercentAttribute, &UHeroCombatAttributeSet::InitBonusAttackSpeedPercent, 0.f,  0.032f },
		{ &UHeroCombatAttributeSet::GetCritChanceAttribute,            &UHeroCombatAttributeSet::InitCritChance,              0.f,     0.f  },
		// 暴击伤害是【倍率】：2.0 = 200%。基础值不随等级涨（属性集.txt 里没有成长这一说）。
		{ &UHeroCombatAttributeSet::GetCritDamageAttribute,            &UHeroCombatAttributeSet::InitCritDamage,              2.f,     0.f  },
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

	/**
	 * 「当前值」属性：Health / Energy。它们是**运行期状态**，不是基础数值。
	 *
	 * 【为什么不参与 ApplyStats 的重算】伤害和消耗写的就是它们的 base。按表重算
	 * （Health = 600 + 119 × (Level-1)）等于每次升级自动满血满蓝 —— 表里那两行只该被
	 * 构造函数用一次（出生满血），之后由 ApplyStats 用「上限差值」的方式推着走。
	 *
	 * 名字带 HeroStat 前缀而不是叫 IsCurrentValueAttribute：unity build 会把多个 .cpp
	 * 合进同一个翻译单元，匿名 namespace 挡不住同签名的自由函数重定义（见 CONVENTIONS.md）。
	 */
	bool HeroStatIsCurrentValueAttribute(const FGameplayAttribute& Attribute)
	{
		return Attribute == UHeroCombatAttributeSet::GetHealthAttribute()
			|| Attribute == UHeroCombatAttributeSet::GetEnergyAttribute();
	}
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

const TArray<FGameplayAttribute>& UHeroCombatAttributeSet::GetVitalsAttributes()
{
	static const TArray<FGameplayAttribute> Vitals = {
		GetHealthAttribute(),
		GetMaxHealthAttribute(),
		GetEnergyAttribute(),
		GetMaxEnergyAttribute(),
	};
	return Vitals;
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
		Attribute == GetAbilityPowerAttribute() || Attribute == GetCritDamageAttribute() ||
		Attribute == GetFlatArmorPenAttribute() || Attribute == GetFlatMagicPenAttribute() ||
		Attribute == GetOmnivampAttribute()     || Attribute == GetLifeStealAttribute() ||
		Attribute == GetHealShieldPowerAttribute())
	{
		NewValue = FMath::Max(0.f, NewValue);
		return;
	}

	// 百分比类：0~1。穿透（先百分比再固定）和韧性 (1-韧性)×时长 都要求落在这个区间，
	// 超出区间算出来的免伤/控制时长没有意义（负的控制时长反而是「加速」）。
	// 暴击率同理：> 1 是「必定暴击」，留着它只会让人写出「叠加暴击率超过 100%」的配置，
	// 结算那边还要再钳一次 —— 在这里钳掉，属性面板上显示的数字也就是真实生效的数字。
	if (Attribute == GetPercentArmorPenAttribute() || Attribute == GetPercentMagicPenAttribute() ||
		Attribute == GetTenacityAttribute() || Attribute == GetCritChanceAttribute())
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
	// 非 const 是因为下面要 ExecuteGameplayCue（那个接口不是 const 的）。
	UAbilitySystemComponent* ASC = GetOwningAbilitySystemComponent();
	if (!ASC || !ASC->IsOwnerActorAuthoritative()) return;

	const FGameplayEffectContextHandle& Context = Data.EffectSpec.GetContext();

	// ---------------------------------------------------------------------
	// 受击表现：发一条 cue，各端各自按「伤害从哪来」播受击动画。
	//
	// 【为什么在这道门之前发】下面那句「Health > 0 就 return」会把致命的那一下赶出去，
	// 而致命那一下恰恰是客户端最需要的一下 —— 死亡蒙太奇要靠它算出「向前扑还是向后倒」
	// （凶手的坐标不进网络，客户端拿不到，只能在这里捎过去）。见 UGC_HitReact。
	//
	// 【仍然只在权威端发】ExecuteGameplayCue 会先在本端跑一次、再多播到各客户端各自跑一次。
	// 预测的那一份在上面已经被门掉了，所以不会出现「挨打的人自己播两遍」。
	// ---------------------------------------------------------------------
	// 【只对掉血发】PostGameplayEffectExecute 在 Health 被【任何方向】改动时都会跑 ——
	// 加血同样走这里。不判符号的话，回血会播一次受击动画。
	// 本项目的伤害一律是 Additive + 负值（见 UExecCalc_Damage），所以判 Magnitude < 0 就够。
	//
	// 已经死透的也不再发：一次多段伤害的每一跳都会走到这儿。
	//（下面还有一道同样判据是给死亡广播用的，两处各管各的。）
	if (Data.EvaluatedData.Magnitude < 0.f
		&& !ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Dead))
	{
		FGameplayCueParameters CueParams;
		CueParams.Instigator = Context.GetInstigator();
		CueParams.EffectCauser = Context.GetEffectCauser();

		// 致命的那一下只记方向、不播受击动画，靠这个标签告诉 cue。
		if (GetHealth() <= 0.f)
		{
			CueParams.AggregatedSourceTags.AddTag(LOLGameplayTags::Data_Lethal);
		}

		ASC->ExecuteGameplayCue(LOLGameplayTags::GameplayCue_HitReact, CueParams);
	}

	// 到这里 Health 已经是扣完并钳过的值（PostGameplayEffectExecute 的语义就是「改完之后」），
	// 不用自己去减 Damage。
	if (GetHealth() > 0.f) return;

	// 已经死了就别再广播：一次多段伤害的每一跳、或者同一帧里的两下，都会走到这儿。
	// 监听方（AHeroCombatCharacter::HandleOutOfHealth）自己还有一道同样的判断，两处都留着 ——
	// 广播本身是廉价的，重复挂 GE_Death 不是。
	if (ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Dead)) return;

	UE_LOG(LogTemp, Warning, TEXT("[Death] %s 生命归零（凶手=%s 施加者=%s）→ 广播 OnOutOfHealth"),
		*GetNameSafe(ASC->GetAvatarActor()),
		*GetNameSafe(Context.GetEffectCauser()), *GetNameSafe(Context.GetInstigator()));

	OnOutOfHealth.Broadcast(Context.GetInstigator(), Context.GetEffectCauser());
}

void UHeroCombatAttributeSet::ApplyStats(const UHeroStatConfig* Config, int32 Level)
{
	UAbilitySystemComponent* ASC = GetOwningAbilitySystemComponent();
	if (!ASC)
	{
		// CDO / 还没 InitAbilityActorInfo 时会走到这里。静默 return 的话，调用方会以为数值生效了。
		UE_LOG(LogTemp, Warning, TEXT("[Attributes] ApplyStats(Level=%d) 时拿不到 ASC → 数值未生效"), Level);
		return;
	}

	// 等级从 1 起：1 级 = 只吃 Base，和构造函数刚建出来的结果完全一致。
	// 【幂等的落点】：每次都是 Base + PerLevel × (Level-1)，不是在上一次的结果上累加。
	const int32 LevelsGained = FMath::Max(0, Level - 1);

	// 重算之前先记下上限的【base】，末尾算差值要用。
	//
	// ⚠️ 必须记 base 而不是当前值（GetMaxHealth()）。当前值里含 GE 修正符
	//（装备 / 海克斯给的「最大生命 +200」），拿它做算术会把那 200 算成
	//「本次重算带来的增量」，再写进 Health 的 base 永久写死 ——
	// 症状是「卸掉加血装备后血量不缩回去」。写回的是 base，算术也得用 base。
	const float PreviousMaxHealthBase = ASC->GetNumericAttributeBase(GetMaxHealthAttribute());
	const float PreviousMaxEnergyBase = ASC->GetNumericAttributeBase(GetMaxEnergyAttribute());

	if (Config && !Config->IsEmpty())
	{
		for (const FHeroStatEntry& Row : Config->Stats)
		{
			// 空行跳过：Attribute 没填，写进去是静默无效的。吵一声 —— 「配了却没生效」是错误，
			// 不是像面板 DA 那样的合法中间状态。
			if (!Row.Attribute.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("[Attributes] UHeroStatConfig「%s」里有一行没填属性 → 已跳过，该属性保持构造值"),
					*GetNameSafe(Config));
				continue;
			}

			// 当前值不重算，理由见 HeroStatIsCurrentValueAttribute。
			if (HeroStatIsCurrentValueAttribute(Row.Attribute)) continue;

			ASC->SetNumericAttributeBase(Row.Attribute, Row.Base + Row.PerLevel * LevelsGained);
		}
	}
	else
	{
		// 没配英雄 / 配了一份空表 → 退回内置兜底表。兜底是【整份】的，不做逐条合并：
		// 逐条合并会让「某几条忘了配」表现成「悄悄用了默认值」，而不是一眼看得出来的 0。
		if (Config)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Attributes] UHeroStatConfig「%s」里一行都没有 → 退回内置兜底表"), *GetNameSafe(Config));
		}

		for (const FHeroStatRow& Row : GHeroStatTable)
		{
			if (HeroStatIsCurrentValueAttribute(Row.GetAttribute())) continue;
			ASC->SetNumericAttributeBase(Row.GetAttribute(), Row.Base + Row.PerLevel * LevelsGained);
		}
	}

	// 当前值只跟着上限的【差值】走 —— LoL 口径：升级时 MaxHealth 涨多少，当前生命就到账多少，
	// 但不是回满（残血升级之后还是残血，只是血上限多出来的那截拿到了）。
	//
	// 上限变小（改表、降级）时差值也是负的，当前值跟着往下走，不会超出新上限。
	//
	// 【为什么 PreAttributeBaseChange 的钳制管不了这件事】：它只管「正在写的那一条」属性，
	// 管不到「另一条属性变小了，我这条要跟着收敛」这种跨属性的关系。
	{
		// 差值取【base 之差】：MaxHealth 的基础值涨了多少，Health 的基础值就跟着涨多少。
		// 修正符（+200 装备之类）不参与 —— 它会在每次重算时原样重新叠加上，不会被写死。
		const float MaxHealthDelta =
			ASC->GetNumericAttributeBase(GetMaxHealthAttribute()) - PreviousMaxHealthBase;
		const float MaxEnergyDelta =
			ASC->GetNumericAttributeBase(GetMaxEnergyAttribute()) - PreviousMaxEnergyBase;

		// 钳制上限反过来用【当前值】：当前值才是「这一刻真实能到的血量上限」
		//（base + 修正符）。身上有负修正时它比 base 小，正好能把越界截住。
		const float NewHealthBase = FMath::Clamp(
			ASC->GetNumericAttributeBase(GetHealthAttribute()) + MaxHealthDelta,
			0.f, GetMaxHealth());
		ASC->SetNumericAttributeBase(GetHealthAttribute(), NewHealthBase);

		const float NewEnergyBase = FMath::Clamp(
			ASC->GetNumericAttributeBase(GetEnergyAttribute()) + MaxEnergyDelta,
			0.f, GetMaxEnergy());
		ASC->SetNumericAttributeBase(GetEnergyAttribute(), NewEnergyBase);
	}
}

void UHeroCombatAttributeSet::ApplyChampionLevel(int32 Level)
{
	ApplyStats(nullptr, Level);
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
HERO_DEFINE_REPNOTIFY(AbilityPower)
HERO_DEFINE_REPNOTIFY(CritChance)
HERO_DEFINE_REPNOTIFY(CritDamage)
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
	HERO_REPLICATE(AbilityPower);
	HERO_REPLICATE(BaseAttackSpeed);
	HERO_REPLICATE(AttackSpeedRatio);
	HERO_REPLICATE(BonusAttackSpeedPercent);
	HERO_REPLICATE(FinalAttackSpeed);
	HERO_REPLICATE(CritChance);
	HERO_REPLICATE(CritDamage);
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
