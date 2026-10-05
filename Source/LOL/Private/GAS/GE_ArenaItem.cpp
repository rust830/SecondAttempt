// 斗魂竞技场：通用装备 GE 的实现。

#include "GAS/GE_ArenaItem.h"

#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"
#include "GAS/ArenaItemStats.h"
#include "GAS/HeroCombatAttributeSet.h"

UGE_ArenaItem::UGE_ArenaItem(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 装备是永久的，摘掉靠句柄，不靠时长。
	DurationPolicy = EGameplayEffectDurationType::Infinite;

	// 按 ArenaItemStats 那张表逐条建修正符。
	//
	// 【幅度类型必须是 SetByCaller】理由见头文件 —— 运行时改 Modifiers 只活在服务端。
	// 这里只建「有哪些修正符」，数值一个都不填；填是施加方的事。
	for (const ArenaItemStats::FSupportedStat& Row : ArenaItemStats::GetSupportedStats())
	{
		FGameplayModifierInfo Modifier;
		Modifier.Attribute = Row.Attribute;
		Modifier.ModifierOp = EGameplayModOp::Additive;

		FSetByCallerFloat CallerMagnitude;
		CallerMagnitude.DataTag = Row.SetByCallerTag;
		Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

		Modifiers.Add(Modifier);
	}
}

namespace ArenaItemEffect
{
	FActiveGameplayEffectHandle ApplyStatModifiers(
		UAbilitySystemComponent* ASC,
		const TArray<FArenaItemStatModifier>& Modifiers)
	{
		if (!ASC)
		{
			return FActiveGameplayEffectHandle();
		}

		const TArray<ArenaItemStats::FSupportedStat>& Supported = ArenaItemStats::GetSupportedStats();

		// 先算一遍有效加成有几条 —— 一条都没有就别白挂一个 GE 上去了
		// （挂上去也不会错，但会往 ActiveGameplayEffects 里塞垃圾，查问题时碍眼）。
		int32 NumEffective = 0;
		for (const FArenaItemStatModifier& Modifier : Modifiers)
		{
			if (Modifier.Attribute.IsValid() && ArenaItemStats::IsSupported(Modifier.Attribute))
			{
				++NumEffective;
			}
		}

		if (NumEffective == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[Arena] 装备加成一条都没生效：%d 条里没有一条落在 ArenaItemStats 的支持表里。"
					 "多半是 DataAsset 里那条属性没填，或者填了表外的属性。"),
				Modifiers.Num());
			return FActiveGameplayEffectHandle();
		}

		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		Context.AddSourceObject(ASC->GetOwnerActor());

		FGameplayEffectSpecHandle SpecHandle = ASC->MakeOutgoingSpec(UGE_ArenaItem::StaticClass(), 1.f, Context);
		if (!SpecHandle.IsValid())
		{
			return FActiveGameplayEffectHandle();
		}

		// 【每一条支持的属性都要填，包括这件装备没加的那些（填 0）】
		// GE 上有 18 个 SetByCaller 修正符，不填的那些在求值时会走
		// 「SetByCaller 标签没找到」分支：引擎记 Warning 并按 0 处理。
		// 结果是每件装备刷十几条没用的 Warning，真正的告警被淹掉。
		// 全填一遍 = 一个 Warning 都没有，代价是 18 次 map 写入。
		//
		// ⚠️ 这个 Warning 关不掉：UE 5.8 的 SetSetByCallerMagnitude 只有
		// (FGameplayTag, float) 两个参数（GameplayEffect.h:1117），
		// 早先那个 bWarnIfNotFound 已经删了；而修正符求值那次调用
		// （GameplayEffect.cpp:2040）用的是 AttemptCalculateMagnitude 的默认参数
		// WarnIfSetByCallerFail=true。所以「不填就会吵」，只能靠全填来消音。
		for (const ArenaItemStats::FSupportedStat& Row : Supported)
		{
			SpecHandle.Data->SetSetByCallerMagnitude(Row.SetByCallerTag, 0.f);
		}

		// 再把这件装备真正要加的那些覆盖上去。
		for (const FArenaItemStatModifier& Modifier : Modifiers)
		{
			if (!Modifier.Attribute.IsValid())
			{
				continue;
			}

			const FGameplayTag* Tag = ArenaItemStats::FindSetByCallerTag(Modifier.Attribute);
			if (!Tag)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[Arena] 装备想加属性「%s」，但它不在 ArenaItemStats 的支持表里 —— 这一条被跳过。"
						 "要支持它就在 ArenaItemStats.cpp 的表里加一行。"),
					*Modifier.Attribute.GetName());
				continue;
			}

			SpecHandle.Data->SetSetByCallerMagnitude(*Tag, Modifier.Value);
		}

		const FActiveGameplayEffectHandle Handle = ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());

		// 【MaxHealth 加成要同步抬当前血】GE 修正符只抬上限、不回当前血（见 ArenaItemStats.h
		// 的「MaxHealth 的坑」）。LoL 对齐：拿到 +N 生命 = 上限 +N 且当前血 +N —— 否则奖励
		// 阶段拿到的血量加成要到下一回合重置才看得见。负加成（用血换攻击那类）也走这里，
		// 当前血同步往下掉，Clamp 保证不会掉成负数。
		// 特殊效果型海克斯（SpecialEffect 那份 GE）不经过这里，它想改血量自己负责。
		float MaxHealthDelta = 0.f;
		for (const FArenaItemStatModifier& Modifier : Modifiers)
		{
			if (Modifier.Attribute.IsValid() &&
				Modifier.Attribute == UHeroCombatAttributeSet::GetMaxHealthAttribute())
			{
				MaxHealthDelta += Modifier.Value;
			}
		}

		if (MaxHealthDelta != 0.f)
		{
			// GE 已经生效（同步应用），此刻读到的 MaxHealth 是抬完之后的值。
			const float CurrentHealth = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetHealthAttribute());
			const float NewMaxHealth = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetMaxHealthAttribute());
			ASC->SetNumericAttributeBase(UHeroCombatAttributeSet::GetHealthAttribute(),
				FMath::Clamp(CurrentHealth + MaxHealthDelta, 0.f, NewMaxHealth));
		}

		return Handle;
	}

	void RemoveStatEffect(UAbilitySystemComponent* ASC, FActiveGameplayEffectHandle& Handle)
	{
		if (!Handle.IsValid())
		{
			return;
		}

		if (ASC)
		{
			ASC->RemoveActiveGameplayEffect(Handle);
		}

		// 无论 ASC 在不在都要清 —— 留着旧句柄再摘一次是静默 no-op，
		// 「忘了清」这个错误永远不会自己暴露出来，只会变成「某件装备摘不掉」。
		Handle.Invalidate();
	}
}
