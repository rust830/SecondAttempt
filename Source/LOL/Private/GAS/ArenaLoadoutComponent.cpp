// 斗魂竞技场：装备栏的实现。

#include "GAS/ArenaLoadoutComponent.h"

#include "AbilitySystemComponent.h"
#include "GAS/ArenaAugmentData.h"
#include "GAS/ArenaItemData.h"
#include "GAS/AbilitySet.h"
#include "GAS/GE_ArenaItem.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/MyPlayerState.h"
#include "Net/UnrealNetwork.h"

UArenaLoadoutComponent::UArenaLoadoutComponent()
{
	// 没有任何需要每帧做的事：装备是事件驱动的（装上/摘下那一刻算一次）。
	PrimaryComponentTick.bCanEverTick = false;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	// 组件本身也要复制，不然下面 GetLifetimeReplicatedProps 里登记的那些
	// 属性一个都传不过去（组件不复制 = 它的属性也不复制）。
	SetIsReplicatedByDefault(true);
}

void UArenaLoadoutComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UArenaLoadoutComponent, EquippedItems);
	DOREPLIFETIME(UArenaLoadoutComponent, EquippedAugments);
	DOREPLIFETIME(UArenaLoadoutComponent, ForgeCharges);
}

UAbilitySystemComponent* UArenaLoadoutComponent::ResolveASC() const
{
	// ASC 在 PlayerState 上（见 MyPlayerState.h）。这个组件也挂在同一个 PlayerState 上，
	// 所以 GetOwner() 就是那个 PlayerState。
	if (const AMyPlayerState* PS = Cast<AMyPlayerState>(GetOwner()))
	{
		return PS->GetMyAbilitySystemComponent();
	}

	return nullptr;
}

bool UArenaLoadoutComponent::EquipItem(UArenaItemData* Item)
{
	if (!Item)
	{
		return false;
	}

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		// 装备是权威数据：客户端那份由复制覆盖。静默"生效"只会造成两端不一致，
		// 而且那种不一致查起来很费劲，所以这里要吵。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 非权威端调用了 EquipItem（%s），已忽略。装备只在服务端装。"),
			*Item->GetName());
		return false;
	}

	if (IsItemRackFull())
	{
		if (FullRackPolicy == EArenaFullRackPolicy::Discard)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[Arena] 装备栏已满（%d 格），按策略丢弃了「%s」。"),
				MaxItemSlots, *Item->GetName());
			return false;
		}

		// ReplaceOldest：顶掉最早装上的那件（下标 0）。抽签是按时间顺序装的，
		// 所以下标 0 就是最早那件。
		RemoveItemAt(0);
	}

	// 就算一条修正符都没生效（DataAsset 配错），也照样占一格 ——
	// 那是配置错误，不该表现成「这件装备凭空消失了」。ApplyStatModifiers 已经记过 Warning。
	const FActiveGameplayEffectHandle Handle = ArenaItemEffect::ApplyStatModifiers(ResolveASC(), Item->Modifiers);

	EquippedItems.Add(Item);
	ItemEffectHandles.Add(Handle);

	BroadcastLoadoutChanged();
	return true;
}

bool UArenaLoadoutComponent::AddAugment(UArenaAugmentData* Augment)
{
	if (!Augment)
	{
		return false;
	}

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 非权威端调用了 AddAugment（%s），已忽略。"),
			*Augment->GetName());
		return false;
	}

	FArenaAugmentRuntime Runtime;

	if (UAbilitySystemComponent* ASC = ResolveASC())
	{
		// 只有真配了数值才去挂 —— 纯特殊效果型海克斯（Modifiers 为空）走下面那条路，
		// 这里不调就不会刷出「一条加成都没生效」的假 Warning。
		if (Augment->Modifiers.Num() > 0)
		{
			Runtime.StatHandle = ArenaItemEffect::ApplyStatModifiers(ASC, Augment->Modifiers);
		}

		if (Augment->SpecialEffect)
		{
			FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
			// 来源对象填海克斯本身：以后做「这个效果是谁给的」溯源时用得上，
			// 现在没有消费者，但填错的代价比不填大（改的时候要回头找所有施加点）。
			Context.AddSourceObject(Augment);

			const FGameplayEffectSpecHandle SpecHandle =
				ASC->MakeOutgoingSpec(Augment->SpecialEffect, 1.f, Context);

			if (SpecHandle.IsValid())
			{
				Runtime.SpecialHandle = ASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
			}
		}

		// 通道③：授能力（技能 + 标签 + GE + 属性集）。
		// 和上面两条并列 —— 数值改属性、SpecialEffect 挂状态、这里给能力，三件独立的事。
		//
		// 句柄必须收进 Runtime：否则这个海克斯授出去的能力【收不回来】，
		// 而竞技场将来一定会有「先给后撤」的顺序（换牌 / 局末清空）。
		if (Augment->GrantAbilitySet)
		{
			Augment->GrantAbilitySet->GiveToAbilitySystem(ASC, Runtime.GrantedAbilityHandles);
			UE_LOG(LogTemp, Log, TEXT("[Arena] 海克斯「%s」授予了 %d 个能力"),
				*Augment->GetName(), Runtime.GrantedAbilityHandles.Num());
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 拿到海克斯「%s」时找不到 ASC，数值没生效（海克斯本身记下了）。"),
			*Augment->GetName());
	}

	EquippedAugments.Add(Augment);
	AugmentRuntimes.Add(Runtime);

	BroadcastLoadoutChanged();
	return true;
}

void UArenaLoadoutComponent::AddForgeCharges(EArenaItemTier Tier, int32 Count)
{
	if (Count <= 0)
	{
		return;
	}

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 AddForgeCharges，已忽略。"));
		return;
	}

	for (FArenaForgeCharge& Charge : ForgeCharges)
	{
		if (Charge.Tier == Tier)
		{
			Charge.Count += Count;
			BroadcastLoadoutChanged();
			return;
		}
	}

	ForgeCharges.Add(FArenaForgeCharge{ Tier, Count });
	BroadcastLoadoutChanged();
}

bool UArenaLoadoutComponent::ConsumeForgeCharge(EArenaItemTier Tier)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 ConsumeForgeCharge，已忽略。"));
		return false;
	}

	for (FArenaForgeCharge& Charge : ForgeCharges)
	{
		if (Charge.Tier != Tier)
		{
			continue;
		}

		if (Charge.Count <= 0)
		{
			// 次数不够就【不扣】—— 扣成负数会让「还能用几次」这个显示变成 -1，
			// 而且之后每次判断都得记得处理负数。
			return false;
		}

		--Charge.Count;
		BroadcastLoadoutChanged();
		return true;
	}

	// 这个品质根本没进过表 = 一次都没拿到过。
	return false;
}

bool UArenaLoadoutComponent::ApplyStatAnvil(EArenaItemTier Tier, const TArray<FArenaItemStatModifier>& StatModifiers)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 非权威端调用了 ApplyStatAnvil（%s），已忽略。属性只在服务端发。"),
			*UEnum::GetValueAsString(Tier));
		return false;
	}

	UAbilitySystemComponent* ASC = ResolveASC();
	if (!ASC)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 用 %s 锻造器时找不到 ASC，属性没生效。"),
			*GetNameSafe(GetOwner()), *UEnum::GetValueAsString(Tier));
		return false;
	}

	const FActiveGameplayEffectHandle Handle = ArenaItemEffect::ApplyStatModifiers(ASC, StatModifiers);

	// 句柄刻意丢弃：stat anvil 不可摘除（见头文件）。ApplyStatModifiers 返回无效句柄
	// 说明一条有效加成都没有（池子配错 / 全是表外属性）—— 那要吵出来，别让玩家白花一次次数。
	if (!Handle.IsValid())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 的 %s 锻造器属性包一条都没生效 —— 检查属性池配置（表外属性会被跳过）。"),
			*GetNameSafe(GetOwner()), *UEnum::GetValueAsString(Tier));
		return false;
	}

	for (const FArenaItemStatModifier& Mod : StatModifiers)
	{
		UE_LOG(LogTemp, Log, TEXT("[Arena] %s 的 %s 锻造器：发放 %s %.2f"),
			*GetNameSafe(GetOwner()), *UEnum::GetValueAsString(Tier),
			*Mod.Attribute.AttributeName, Mod.Value);
	}

	BroadcastLoadoutChanged();
	return true;
}

int32 UArenaLoadoutComponent::GetForgeCharges(EArenaItemTier Tier) const
{
	for (const FArenaForgeCharge& Charge : ForgeCharges)
	{
		if (Charge.Tier == Tier)
		{
			return Charge.Count;
		}
	}

	return 0;
}

TArray<UArenaItemData*> UArenaLoadoutComponent::GetEquippedUniqueItems() const
{
	TArray<UArenaItemData*> Result;

	// 服务端专用逻辑（抽签去重用），所以这里的 .Get() 一定命中 ——
	// 这些资产就是从已加载的池子里抽出来的。
	for (const TSoftObjectPtr<UArenaItemData>& SoftItem : EquippedItems)
	{
		if (UArenaItemData* Item = SoftItem.Get())
		{
			if (Item->bUnique)
			{
				Result.Add(Item);
			}
		}
	}

	return Result;
}

TArray<UArenaAugmentData*> UArenaLoadoutComponent::GetEquippedAugmentList() const
{
	TArray<UArenaAugmentData*> Result;
	Result.Reserve(EquippedAugments.Num());

	for (const TSoftObjectPtr<UArenaAugmentData>& SoftAugment : EquippedAugments)
	{
		if (UArenaAugmentData* Augment = SoftAugment.Get())
		{
			Result.Add(Augment);
		}
	}

	return Result;
}

void UArenaLoadoutComponent::RemoveItemAt(int32 Index)
{
	if (!EquippedItems.IsValidIndex(Index))
	{
		return;
	}

	// 先摘 GE，再动数组。顺序反过来的话，下标已经偏了，会摘错人的效果。
	if (ItemEffectHandles.IsValidIndex(Index))
	{
		ArenaItemEffect::RemoveStatEffect(ResolveASC(), ItemEffectHandles[Index]);
	}

	// 两个数组紧挨着删 —— 这是这份代码里唯一会让它们错位的地方。
	EquippedItems.RemoveAt(Index);
	ItemEffectHandles.RemoveAt(Index);
}

bool UArenaLoadoutComponent::RemoveAugmentAt(int32 Index)
{
	if (!EquippedAugments.IsValidIndex(Index))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] RemoveAugmentAt(%d) 越界（现有 %d 个），已忽略。"),
			Index, EquippedAugments.Num());
		return false;
	}

	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 RemoveAugmentAt，已忽略。"));
		return false;
	}

	UAbilitySystemComponent* ASC = ResolveASC();

	// 先摘效果、再动数组 —— 顺序反过来的话下标已经偏了，会摘错人的东西。
	if (AugmentRuntimes.IsValidIndex(Index))
	{
		FArenaAugmentRuntime& Runtime = AugmentRuntimes[Index];

		// ① 数值 GE
		if (Runtime.StatHandle.IsValid())
		{
			ArenaItemEffect::RemoveStatEffect(ASC, Runtime.StatHandle);
		}

		// ② SpecialEffect GE
		if (Runtime.SpecialHandle.IsValid() && ASC)
		{
			ASC->RemoveActiveGameplayEffect(Runtime.SpecialHandle);
		}

		// ③ 撤技能。★ 绝不能用 ClearAllAbilities —— 它把这个英雄身上所有能力
		//    （QWER / 召唤师技能 / 被动 / 形态切换）一起清掉。只能按句柄逐个撤。
		if (ASC)
		{
			for (const FGameplayAbilitySpecHandle& Handle : Runtime.GrantedAbilityHandles)
			{
				if (Handle.IsValid())
				{
					ASC->ClearAbility(Handle);
				}
			}
		}

		// ④ 摘它授的 loose 标签（AbilitySet::GrantTag 走的是 AddLooseGameplayTags）。
		//    ★ 必须显式摘：ClearAbility 只清能力，不会顺手把标签带走 ——
		//    表现是「技能没了但那个状态还在」。
		//    ★ 这里【不】用 RemoveGrantedTagEffects：那个走 GE 的 Granted Tags 查询，
		//    而 loose tag 根本不在任何 GE 上，查询匹配不到（静默不删）。
		if (ASC)
		{
			if (const UArenaAugmentData* Augment = EquippedAugments[Index].Get())
			{
				if (const UAbilitySet* Set = Augment->GrantAbilitySet)
				{
					if (Set->GrantTag.Num() > 0)
					{
						ASC->RemoveLooseGameplayTags(Set->GrantTag);
					}
				}
			}
		}
	}

	// 名字要在 RemoveAt 之前取 —— 删完之后下标 Index 指向的是【下一个】海克斯，
	// 那样打出来的名字是错的，而且刚好在「摘错了」的时候最容易骗人。
	const FString RemovedName = EquippedAugments[Index].Get() ? EquippedAugments[Index].Get()->GetName() : TEXT("<null>");

	// 两个数组紧挨着删（唯一会让它们错位的地方，同 RemoveItemAt）。
	EquippedAugments.RemoveAt(Index);
	AugmentRuntimes.RemoveAt(Index);

	UE_LOG(LogTemp, Log, TEXT("[Arena] 摘掉海克斯「%s」（第 %d 个，剩下 %d 个）"),
		*RemovedName, Index, EquippedAugments.Num());

	BroadcastLoadoutChanged();
	return true;
}

void UArenaLoadoutComponent::OnRep_Loadout()
{
	// 客户端：复制下来的是资产路径，UI 待会儿要读 DisplayName / Icon 就得先把它们加载出来。
	// 在这里一次性预热，而不是等 UI 每次重画时各自 .Get() ——
	// 那样 UI 拿到的会是 null 并画出一排空格子，而且没有任何报错。
	// 数量有上限（装备 ≤ MaxItemSlots，海克斯是几十个里选的几件），不会卡。
	for (const TSoftObjectPtr<UArenaItemData>& SoftItem : EquippedItems)
	{
		SoftItem.LoadSynchronous();
	}

	for (const TSoftObjectPtr<UArenaAugmentData>& SoftAugment : EquippedAugments)
	{
		SoftAugment.LoadSynchronous();
	}

	BroadcastLoadoutChanged();
}

void UArenaLoadoutComponent::BroadcastLoadoutChanged()
{
	OnLoadoutChanged.Broadcast();
}
