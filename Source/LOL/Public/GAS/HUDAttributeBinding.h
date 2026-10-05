// 「属性委托 + 标签事件」的绑定记账器。
//
// 【为什么单独抽出来】
// HUD 里同一套「绑一组属性委托 + N 条标签事件，解绑时原样摘掉」的代码本来有三份：
// UHeroHUDController 的 Self 通道 / Observed 通道，和 UHeroOverlayHealthComponent。
// 三份手写记账 = 三处漏解绑的机会，而漏解绑的症状（PS 已经换了还在收旧 ASC 的事件、
// 句柄被复用）是整个项目里最难查的一类。收在一处之后，解绑的正确性只需要读一遍。
//
// 【为什么是模板】
// 回调是各调用方自己的成员函数（Self 通道重算 vitals、Observed 通道刷目标框、
// 头顶血条刷自己那条），所以绑定时要把「谁的哪个函数」传进来。顺带一个副作用：
// 一组属性的回调体本来就是同一个（都是「重算一遍，再决定要不要推」），
// 于是原来 8 个各写一遍的空壳回调（OnHealthChanged / OnMaxHealthChanged / …）收成了 2 个。
//
// 【纪律】（和 UHeroHUDController 文件头那两条一致，这里是落点）
//   · 属性用 GetGameplayAttributeValueChangeDelegate，【不用 OnRep_*】——
//     listen server 上主机是权威端，属性直接写进去，OnRep 不触发。
//   · 标签用 NewOrRemoved —— 只在 0→1 / 1→0 触发，正是「开关」语义；
//     不要换成 AnyCountChange：同一帧被两层 GE 挂上时会多抖一次。
//
// 【不持有 ASC】只存弱引用。强引用会把 PlayerState 一起钉住。
//
// 类型名带 HUD 前缀：这个头会被多个 .cpp include，unity build 把多个 .cpp 合进同一个 TU 时，
// 同名的自由函数/类型会撞（见 CONVENTIONS.md 的「unity build 重名」一节）。

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"

/**
 * 一条已注册的标签事件。
 * 解绑要 (Handle, Tag) 成对给回去（ASC 的 UnregisterGameplayTagEvent 需要标签才能定位到那张表），
 * 所以两个字段必须一起存 —— 这也是「句柄单独一个 TArray 就够」不成立的原因。
 */
struct FHUDBoundTagEvent
{
	FGameplayTag Tag;
	FDelegateHandle Handle;
};

/** 一条已注册的属性委托。解绑同样要拿属性再取一次那条委托，所以也要成对存。 */
struct FHUDBoundAttributeEvent
{
	FGameplayAttribute Attribute;
	FDelegateHandle Handle;
};

/**
 * 一个 ASC 上的绑定集合。用法永远是「先 Unbind，再 Bind…」——
 * 重绑不清旧的会得到两份回调，症状是数值跳变两下、或者日志打两遍。
 */
struct FHUDAttributeBinding
{
	/**
	 * 把一组属性绑到【同一个】回调上。
	 * 同一个回调是刻意的：这几种资源的处理逻辑完全一样（重算 + 变了才推），
	 * 分成多个回调只会多出「忘了绑其中一条」的机会。
	 */
	template <typename TOwner>
	void BindAttributes(
		UAbilitySystemComponent* ASC,
		const TArray<FGameplayAttribute>& Attributes,
		TOwner* Owner,
		void (TOwner::*Handler)(const FOnAttributeChangeData&))
	{
		if (!ASC || !Owner)
		{
			return;
		}

		BoundASC = ASC;

		for (const FGameplayAttribute& Attribute : Attributes)
		{
			if (!Attribute.IsValid())
			{
				continue;
			}

			const FDelegateHandle Handle =
				ASC->GetGameplayAttributeValueChangeDelegate(Attribute).AddUObject(Owner, Handler);
			BoundAttributes.Add({ Attribute, Handle });
		}
	}

	/**
	 * 绑一条标签事件（NewOrRemoved 语义）。标签无效时安静跳过 ——
	 * 「这个槽没有冷却标签」是合法配置（被动），不是错误。
	 */
	template <typename TOwner>
	void BindTag(
		UAbilitySystemComponent* ASC,
		const FGameplayTag& Tag,
		TOwner* Owner,
		void (TOwner::*Handler)(const FGameplayTag, int32))
	{
		if (!ASC || !Owner || !Tag.IsValid())
		{
			return;
		}

		BoundASC = ASC;

		const FDelegateHandle Handle = ASC
			->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved)
			.AddUObject(Owner, Handler);
		TagEvents.Add({ Tag, Handle });
	}

	/** 摘掉一切并清空句柄。调完还能再 Bind。 */
	void Unbind()
	{
		if (UAbilitySystemComponent* ASC = BoundASC.Get())
		{
			for (const FHUDBoundAttributeEvent& Entry : BoundAttributes)
			{
				if (Entry.Handle.IsValid())
				{
					ASC->GetGameplayAttributeValueChangeDelegate(Entry.Attribute).Remove(Entry.Handle);
				}
			}

			for (const FHUDBoundTagEvent& Entry : TagEvents)
			{
				if (Entry.Handle.IsValid())
				{
					ASC->UnregisterGameplayTagEvent(Entry.Handle, Entry.Tag, EGameplayTagEventType::NewOrRemoved);
				}
			}
		}

		// ASC 已经没了的情况什么都不用做：委托跟着它一起没了。唯一的要求是
		// 【不要把句柄留着复用】—— 所以无条件清空（和原来两份手写实现的行为一致）。
		Reset();
	}

	/** 只清句柄，不碰 ASC（给「反正要换了」的场景省一次查找）。 */
	void Reset()
	{
		BoundAttributes.Reset();
		TagEvents.Reset();
		BoundASC = nullptr;
	}

	bool IsBound() const { return BoundASC.IsValid(); }

private:
	/** 弱引用：强引用会把 PlayerState 一起钉住。只用于 Unbind 时定位到那份委托。 */
	TWeakObjectPtr<UAbilitySystemComponent> BoundASC;

	TArray<FHUDBoundAttributeEvent> BoundAttributes;
	TArray<FHUDBoundTagEvent> TagEvents;
};
