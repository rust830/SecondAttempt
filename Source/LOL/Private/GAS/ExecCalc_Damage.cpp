// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/ExecCalc_Damage.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/BlockComponent.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"

/**
 * 捕获定义表。
 *
 * ⚠️ 加/删属性必须【两处一起改】：这里的 DECLARE/DEFINE，以及构造函数里的 RelevantAttributesToCapture.Add。
 * 漏掉后者的表现是「读出来恒为 0、不报错」—— 攻击力恒 0 就是「只有固定值那部分伤害」，
 * 抗性恒 0 就是「打谁都是满伤害」。两种都不崩、不报警告。
 */
struct FDamageStatics
{
	DECLARE_ATTRIBUTE_CAPTUREDEF(AttackDamage);
	DECLARE_ATTRIBUTE_CAPTUREDEF(Armor);
	DECLARE_ATTRIBUTE_CAPTUREDEF(MagicResist);
	DECLARE_ATTRIBUTE_CAPTUREDEF(FlatArmorPen);
	DECLARE_ATTRIBUTE_CAPTUREDEF(PercentArmorPen);
	DECLARE_ATTRIBUTE_CAPTUREDEF(FlatMagicPen);
	DECLARE_ATTRIBUTE_CAPTUREDEF(PercentMagicPen);
	DECLARE_ATTRIBUTE_CAPTUREDEF(Health);
	DECLARE_ATTRIBUTE_CAPTUREDEF(MaxHealth);

	FDamageStatics()
	{
		// 攻击力/穿透从【施加者】身上抓，抗性从【目标】身上抓。
		// 施加者是谁由 Spec 的 EffectContext 决定：MakeEffectContext 已经把 instigator 设成
		// 施加者的 PlayerState（causer = 角色），所以近战/匕首不用额外做任何事。
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, AttackDamage, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, FlatArmorPen, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, PercentArmorPen, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, FlatMagicPen, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, PercentMagicPen, Source, false);

		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, Armor, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, MagicResist, Target, false);

		// 已损失生命加成从【目标】身上抓：目标是敌人，残血打得疼（GAS_DeathHarvest_Setup.md §5.5）。
		// 想改成"施法者残血加伤"，把这两行的 Target 改成 Source 就行，别的地方不用动。
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, Health, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, MaxHealth, Target, false);
	}
};

static const FDamageStatics& DamageStatics()
{
	static FDamageStatics Statics;
	return Statics;
}

UExecCalc_Damage::UExecCalc_Damage()
{
	RelevantAttributesToCapture.Add(DamageStatics().AttackDamageDef);
	RelevantAttributesToCapture.Add(DamageStatics().ArmorDef);
	RelevantAttributesToCapture.Add(DamageStatics().MagicResistDef);
	RelevantAttributesToCapture.Add(DamageStatics().FlatArmorPenDef);
	RelevantAttributesToCapture.Add(DamageStatics().PercentArmorPenDef);
	RelevantAttributesToCapture.Add(DamageStatics().FlatMagicPenDef);
	RelevantAttributesToCapture.Add(DamageStatics().PercentMagicPenDef);
	RelevantAttributesToCapture.Add(DamageStatics().HealthDef);
	RelevantAttributesToCapture.Add(DamageStatics().MaxHealthDef);
}

void UExecCalc_Damage::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
	FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	UAbilitySystemComponent* SourceASC = ExecutionParams.GetSourceAbilitySystemComponent();
	UAbilitySystemComponent* TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();
	if (!SourceASC || !TargetASC)
	{
		// 走到这里说明 Spec 的 EffectContext 里没有 instigator（比如有人自己 new 了个 Spec 直接施加）。
		// 静默返回的话表现是「这一下完全没伤害」，很难往这里想。
		UE_LOG(LogTemp, Warning, TEXT("[Damage] 缺 SourceASC(%d) 或 TargetASC(%d) → 本次伤害未结算"),
			SourceASC ? 1 : 0, TargetASC ? 1 : 0);
		return;
	}

	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();

	FAggregatorEvaluateParameters EvalParams;
	EvalParams.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	EvalParams.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

	auto Capture = [&ExecutionParams, &EvalParams](const FGameplayEffectAttributeCaptureDefinition& Def) -> float
	{
		float Value = 0.f;
		ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(Def, EvalParams, Value);
		return Value;
	};

	// ① 原始伤害 = 攻击者攻击力 × 攻击力加成 + 固定伤害
	//    两个 SetByCaller 都传 WarnIfNotFound=false：伤害点上「只填其中一个」是合法的
	//    （近战只用倍率、匕首只用固定值），不该每次都刷警告。
	const float AttackDamage = Capture(DamageStatics().AttackDamageDef);
	const float Multiplier   = Spec.GetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, /*WarnIfNotFound=*/false, 0.f);
	const float FlatDamage   = Spec.GetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, /*WarnIfNotFound=*/false, 0.f);
	float Damage = AttackDamage * Multiplier + FlatDamage;

	// ①b 已损失生命加成（斩杀型）。满血 = 0 加成，残血最多 ×(1 + MissingHealthBonus)。
	//
	// 【乘法不加法】：和上面 AttackDamage × Multiplier + Flat 的公式不打架，
	// 同一个技能以后还能被别的系数接着叠。
	// WarnIfNotFound=false 和现有三个 SetByCaller 一致 —— 近战/匕首那两条老路径根本不填这个标签，
	// 填 true 的话它们每次命中都会刷一条警告。
	const float MissingHealthBonus = Spec.GetSetByCallerMagnitude(LOLGameplayTags::Data_MissingHealthBonus, /*WarnIfNotFound=*/false, 0.f);
	if (MissingHealthBonus > 0.f)
	{
		// Health 是【这次结算之前】的值：ExecCalc 跑的时候负向 modifier 还没落地。
		const float TargetHealth = Capture(DamageStatics().HealthDef);
		const float TargetMaxHealth = Capture(DamageStatics().MaxHealthDef);
		if (TargetMaxHealth > 0.f)
		{
			const float MissingPct = 1.f - FMath::Clamp(TargetHealth / TargetMaxHealth, 0.f, 1.f);
			Damage *= 1.f + MissingPct * MissingHealthBonus;
		}
	}

	// ② 抗性减免。伤害类型由施加方打在 Spec 的动态资产标签上（同一个 GE 服务所有伤害，靠标签分流），
	//    没打任何类型标签 = 物理（近战/匕首的默认）。
	const FGameplayTagContainer& AssetTags = Spec.GetDynamicAssetTags();
	const bool bTrueDamage = AssetTags.HasTag(LOLGameplayTags::Damage_True);
	const bool bMagic = AssetTags.HasTag(LOLGameplayTags::Damage_Magic);
	if (!bTrueDamage)
	{
		float Resist   = Capture(bMagic ? DamageStatics().MagicResistDef : DamageStatics().ArmorDef);
		const float FlatPen = Capture(bMagic ? DamageStatics().FlatMagicPenDef : DamageStatics().FlatArmorPenDef);
		const float PctPen  = Capture(bMagic ? DamageStatics().PercentMagicPenDef : DamageStatics().PercentArmorPenDef);

		// 先百分比、后固定（属性集.txt 的口径）。
		Resist = Resist * (1.f - FMath::Clamp(PctPen, 0.f, 1.f)) - FlatPen;

		// x/(x+100) 的免伤 → 承伤系数。抗性为负时用 LoL 的延伸公式：
		// Resist = -100 时分母是 100-(-100)=200，不会除爆（直接拿 x/(x+100) 才会）。
		const float DamageTaken = (Resist >= 0.f) ? 100.f / (100.f + Resist) : 2.f - 100.f / (100.f - Resist);
		Damage *= FMath::Max(0.f, DamageTaken);
	}

	// ③ 格挡 / 免疫 —— 全项目唯一的减免入口，就这一行。
	//    传 causer 而不是 avatar：朝向判定要比的是「这一击从哪来」。
	UBlockComponent::TryMitigateIncomingDamage(TargetASC, Spec.GetContext().GetEffectCauser(), Damage);

	// ④ 落地。Health 被扣成负的没关系：PreAttributeBaseChange 里会钳到 0 —— 别在这里再钳一次，
	//    两处钳制迟早对不上（比如以后加了「最低保留 1 点血」的机制）。
	if (Damage > 0.f)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
			UHeroCombatAttributeSet::GetHealthAttribute(), EGameplayModOp::Additive, -Damage));
	}

	UE_LOG(LogTemp, Warning, TEXT("[Damage] 攻=%.1f 倍率=%.2f 固定=%.1f 斩杀=%.2f → 结算=%.1f 类型=%s 目标=%s"),
		AttackDamage, Multiplier, FlatDamage, MissingHealthBonus, Damage,
		bTrueDamage ? TEXT("真实") : (bMagic ? TEXT("魔法") : TEXT("物理")),
		*GetNameSafe(TargetASC->GetAvatarActor()));
}
