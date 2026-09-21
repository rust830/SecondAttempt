// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroHUDSlotConfig.h"

void UHeroHUDSlotConfig::EnsureCache() const
{
	if (!bCacheValid)
	{
		RebuildCache();
	}
}

void UHeroHUDSlotConfig::RebuildCache() const
{
	SlotIndexCache.Reset();
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		const FGameplayTag& SlotTag = Slots[Index].SlotTag;
		if (!SlotTag.IsValid())
		{
			continue;
		}
		// 重复配置时保留第一个 —— 顺序在前的那条说了算，别让后配的偷偷覆盖。
		SlotIndexCache.FindOrAdd(SlotTag, Index);
	}
	bCacheValid = true;
}

int32 UHeroHUDSlotConfig::IndexOfSlot(const FGameplayTag& SlotTag) const
{
	if (!SlotTag.IsValid())
	{
		return INDEX_NONE;
	}

	EnsureCache();
	if (const int32* Found = SlotIndexCache.Find(SlotTag))
	{
		return *Found;
	}

	// 缓存没命中：可能缓存过期（编辑器里改了 Slots 而没走到 PostEditChangeProperty，
	// 比如运行时改资产或热重载）。退化成线性扫描 —— 六个槽，这点开销不值得再维护第二份状态，
	// 而"缓存和数组不一致"是这类代码最典型的静默 bug。
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (Slots[Index].SlotTag == SlotTag)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 UHeroHUDSlotConfig::IndexOfCooldownTag(const FGameplayTag& CooldownTag) const
{
	if (!CooldownTag.IsValid())
	{
		return INDEX_NONE;
	}

	// 不做缓存：这是「标签事件回调」专用的查法，一局里触发次数是按键量级，
	// 而它需要跟 CooldownTag 这个可选字段保持一致（很多槽是空的），
	// 为它再维护一份缓存不划算。
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		if (Slots[Index].CooldownTag.IsValid() && Slots[Index].CooldownTag == CooldownTag)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

const FHeroHUDSlotEntry* UHeroHUDSlotConfig::FindEntry(int32 SlotIndex) const
{
	return Slots.IsValidIndex(SlotIndex) ? &Slots[SlotIndex] : nullptr;
}

#if WITH_EDITOR
void UHeroHUDSlotConfig::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 改了表就丢缓存，否则 PIE 里调完映射要重启编辑器才生效。
	bCacheValid = false;
}
#endif
