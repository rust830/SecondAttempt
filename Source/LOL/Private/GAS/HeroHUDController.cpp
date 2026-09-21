// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroHUDController.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "TimerManager.h"

#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/HeroHUDSlotConfig.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/MyGameplayAbility.h"
#include "GAS/MyPlayerState.h"

// 构建提示：本模块新增 UCLASS/USTRUCT 之后，必须「关掉编辑器 -> 重新链接 -> 再开编辑器」。
// Live Coding 能热补函数体，但加不了新的反射数据；而且编辑器运行时持有
// Binaries/Win64/UnrealEditor-LOL.dll，链接步骤替换不掉它。
// 自动化入口见 Tools/vs_build.py（--close-editor 会优雅关闭编辑器后再构建）。

// ===========================================================================
// 绑定 / 解绑
// ===========================================================================

UMyAbilitySystemComponent* UHeroHUDController::ResolveSelfASC() const
{
	// 只给本地玩家建 HUD，所以 Outer 一定是本地 PC。
	const APlayerController* PC = Cast<APlayerController>(GetOuter());
	if (!PC)
	{
		return nullptr;
	}

	// ① 本地玩家自己的 PlayerState —— 唯一真值源（ASC 和属性集都挂在它上面）
	if (const AMyPlayerState* PS = PC->GetPlayerState<AMyPlayerState>())
	{
		if (UMyAbilitySystemComponent* ASC = PS->GetMyAbilitySystemComponent())
		{
			return ASC;
		}
	}

	// ② 兜底：Pawn 上找。PS 还没复制到、或者特殊模式（观战）时能救一下。
	//    用项目自己的静态入口，不要用蓝图库那个 —— 理由见
	//    UMyAbilitySystemComponent::FindAbilitySystemComponent 的注释（接口 Cast 成功但返回 nullptr 时它不会往下试）。
	return Cast<UMyAbilitySystemComponent>(UMyAbilitySystemComponent::FindAbilitySystemComponent(PC->GetPawn()));
}

void UHeroHUDController::TryBindHUD()
{
	UMyAbilitySystemComponent* ASC = ResolveSelfASC();

	// ASC 有了但还没 InitAbilityActorInfo：属性读得到、冷却查不了
	// （GetCooldownTimeRemainingAndDuration 需要 ActorInfo）。
	// 这种「半绑」状态（血条在动、冷却不动）比完全没绑更难查，所以整体当成还没好，等重试。
	if (ASC && !ASC->AbilityActorInfo.IsValid())
	{
		ASC = nullptr;
	}

	if (ASC == BoundASC.Get())
	{
		// 没变化。已经绑好 → 只补一次全量（三个调用点会各自走到这里一次，广播三次是幂等的）；
		// 还没绑上 → 确保重试是开着的（首次进来就是这条，不能让重试漏掉）。
		if (ASC)
		{
			BroadcastInitialValues();
		}
		else
		{
			StartBindRetry();
		}
		return;
	}

	// 换人了（或第一次）。先把旧 ASC 上的标签事件 + 属性委托全摘掉 ——
	// 漏一个就是「PS 已经换了但还在收旧 ASC 的事件」，这种症状极难查。
	UnbindSelf();
	BoundASC = ASC;

	if (!ASC)
	{
		ResetToUnboundState();
		StartBindRetry();
		return;
	}

	BindSelf();
	StopBindRetry();

	if (!SlotConfig)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UHeroHUDController: 没有配 UHeroHUDSlotConfig，技能栏会是空的。")
			TEXT("在 BP_LOLPlayerController 上设 HUDSlotConfig。"));
	}

	BroadcastInitialValues();   // 顺手填满缓存 + 通知在场订阅者
	UpdateCooldownTicker();     // 重生 / 重连时可能本来就在冷却
}

void UHeroHUDController::BindSelf()
{
	UMyAbilitySystemComponent* ASC = BoundASC.Get();
	if (!ASC)
	{
		return;
	}

	// ① 属性：血 / 能量。
	//    用 GetGameplayAttributeValueChangeDelegate，【不用 OnRep_*】——
	//    listen server 上主机是权威端，属性是直接写进去的，OnRep 根本不触发。
	//    用 OnRep 的话症状是「主机看不到自己的血条动」，而且只在多人下才暴露。
	HealthHandle = ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetHealthAttribute())
		.AddUObject(this, &UHeroHUDController::OnHealthAttributeChanged);
	MaxHealthHandle = ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxHealthAttribute())
		.AddUObject(this, &UHeroHUDController::OnMaxHealthAttributeChanged);
	EnergyHandle = ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetEnergyAttribute())
		.AddUObject(this, &UHeroHUDController::OnEnergyAttributeChanged);
	MaxEnergyHandle = ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxEnergyAttribute())
		.AddUObject(this, &UHeroHUDController::OnMaxEnergyAttributeChanged);

	// ② 标签：每个槽的冷却标签各一条。
	//    NewOrRemoved = 只在 0→1 / 1→0 触发，中间的 1→2、2→1 不触发 —— 这恰好就是「开关」语义。
	//    不要用 AnyCountChange：同一帧里被两层 GE 挂上时会多抖一次。
	if (SlotConfig)
	{
		for (const FHeroHUDSlotEntry& Entry : SlotConfig->Slots)
		{
			if (!Entry.CooldownTag.IsValid())
			{
				continue;
			}
			const FDelegateHandle Handle = ASC
				->RegisterGameplayTagEvent(Entry.CooldownTag, EGameplayTagEventType::NewOrRemoved)
				.AddUObject(this, &UHeroHUDController::OnCooldownTagChanged);
			BoundTagEvents.Add({ Entry.CooldownTag, Handle });
		}
	}

	// ③ 标签：三条施法侧准入状态。任何一条变，所有槽的灰化都要重算。
	//    死亡读 State.Dead 而不是 Health == 0 —— 和 AHeroCombatCharacter::IsDead() 同一个判据，
	//    否则 HUD 和「技能到底能不能放」会分叉（技能栏亮着但按下去被拒）。
	const FGameplayTag StatusTags[] = {
		LOLGameplayTags::State_Dead,
		LOLGameplayTags::State_Stunned,
		LOLGameplayTags::State_Silenced,
	};
	for (const FGameplayTag& Tag : StatusTags)
	{
		const FDelegateHandle Handle = ASC
			->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved)
			.AddUObject(this, &UHeroHUDController::OnStatusTagChanged);
		BoundTagEvents.Add({ Tag, Handle });
	}
}

void UHeroHUDController::UnbindSelf()
{
	if (UMyAbilitySystemComponent* ASC = BoundASC.Get())
	{
		for (const FHUDBoundTagEvent& Entry : BoundTagEvents)
		{
			if (Entry.Handle.IsValid())
			{
				ASC->UnregisterGameplayTagEvent(Entry.Handle, Entry.Tag, EGameplayTagEventType::NewOrRemoved);
			}
		}

		if (HealthHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetHealthAttribute()).Remove(HealthHandle);
		}
		if (MaxHealthHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxHealthAttribute()).Remove(MaxHealthHandle);
		}
		if (EnergyHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetEnergyAttribute()).Remove(EnergyHandle);
		}
		if (MaxEnergyHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxEnergyAttribute()).Remove(MaxEnergyHandle);
		}
	}
	// ASC 已经被销毁的情况什么都不用做：委托跟着它一起没了。这里唯一的要求是
	// 【不要把句柄留着复用】—— 所以下面无条件 Reset。

	BoundTagEvents.Reset();
	HealthHandle.Reset();
	MaxHealthHandle.Reset();
	EnergyHandle.Reset();
	MaxEnergyHandle.Reset();
}

void UHeroHUDController::UnbindObserved()
{
	if (UAbilitySystemComponent* ASC = ObservedASC.Get())
	{
		for (const FHUDBoundTagEvent& Entry : ObservedTagEvents)
		{
			if (Entry.Handle.IsValid())
			{
				ASC->UnregisterGameplayTagEvent(Entry.Handle, Entry.Tag, EGameplayTagEventType::NewOrRemoved);
			}
		}

		if (ObservedHealthHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetHealthAttribute()).Remove(ObservedHealthHandle);
		}
		if (ObservedMaxHealthHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxHealthAttribute()).Remove(ObservedMaxHealthHandle);
		}
		if (ObservedEnergyHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetEnergyAttribute()).Remove(ObservedEnergyHandle);
		}
		if (ObservedMaxEnergyHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxEnergyAttribute()).Remove(ObservedMaxEnergyHandle);
		}
	}

	ObservedTagEvents.Reset();
	ObservedHealthHandle.Reset();
	ObservedMaxHealthHandle.Reset();
	ObservedEnergyHandle.Reset();
	ObservedMaxEnergyHandle.Reset();

	ObservedTarget = nullptr;
	ObservedASC = nullptr;
}

void UHeroHUDController::UnbindAll()
{
	UnbindSelf();
	UnbindObserved();
}

void UHeroHUDController::ShutdownHUD()
{
	StopBindRetry();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CooldownTickHandle);
	}

	UnbindAll();
	BoundASC = nullptr;

	CachedVitals = FHUDVitalsView();
	CachedSlots.Reset();
	CachedTarget = FTargetFrameView();
}

void UHeroHUDController::StartBindRetry()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (++BindRetryCount > MaxBindRetries)
	{
		BindRetryCount = 0;
		UE_LOG(LogTemp, Warning,
			TEXT("UHeroHUDController: 重试 %d 次后仍然拿不到可用的 ASC，停止重试（观战 / 没有 PS 的情况属正常）。"),
			MaxBindRetries);
		return;
	}

	// 自排程的一次性定时器：触发一次 → TryBindHUD 又失败 → 再排一次。
	// 比 SetTimer(bLoop=true) 好：「成功就停」和「超限就停」分别在 StopBindRetry 和上面这一处，
	// 不需要在循环体里再判断一次。
	World->GetTimerManager().SetTimer(
		BindRetryHandle, this, &UHeroHUDController::TryBindHUD, BindRetryInterval, /*bLoop=*/false);
}

void UHeroHUDController::StopBindRetry()
{
	BindRetryCount = 0;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(BindRetryHandle);
	}
}

// ===========================================================================
// 拉取 / 配置
// ===========================================================================

FHUDSnapshot UHeroHUDController::PullHUDState() const
{
	FHUDSnapshot Snapshot;
	Snapshot.bBound = IsHUDReady();
	Snapshot.Vitals = CachedVitals;
	Snapshot.Slots = CachedSlots;
	Snapshot.Target = CachedTarget;
	return Snapshot;
}

void UHeroHUDController::BroadcastInitialValues()
{
	if (!IsHUDReady())
	{
		ResetToUnboundState();
		return;
	}

	// 强制重放：绕开「变了才推」的判断，保证在场订阅者一定收到一份完整状态。
	// 这也是「订阅即拉取」的兜底路径 —— Widget 在 NativeConstruct 里已经拉过一次，
	// 这里的重放是给「已经建好、正等状态回来」的 Widget 用的。
	RebuildVitals(true);
	RebuildAllSlots(true);
	RefreshTargetFrame(true);
	OnHUDReady.Broadcast(true);
}

void UHeroHUDController::ResetToUnboundState()
{
	EnsureSlotCacheSize();

	CachedVitals = FHUDVitalsView();

	for (int32 Index = 0; Index < CachedSlots.Num(); ++Index)
	{
		const FSkillSlotView Old = CachedSlots[Index];

		FSkillSlotView Reset;
		Reset.SlotIndex = Index;
		// 静态表现留着 —— 它来自配置，跟 ASC 有没有绑上无关。
		// 显示成「图标在、全灰」比显示成空白更像回事，也是更好的加载态。
		Reset.Icon = Old.Icon;
		Reset.KeyLabel = Old.KeyLabel;
		Reset.DisplayName = Old.DisplayName;
		Reset.State = ESkillSlotState::Disabled;
		Reset.BlockReasons = static_cast<int32>(ESkillSlotBlockReason::NoAbility);

		CachedSlots[Index] = Reset;
	}

	// 目标通道和 Self 通道互相独立：Self 没绑上不代表目标框也要收掉。
	RefreshTargetFrame(true);

	// 显式广播一遍，不走「变了才推」—— 这里的场景恰恰是「值没变，但 UI 必须回到未就绪」。
	OnVitalsChanged.Broadcast(CachedVitals);
	for (int32 Index = 0; Index < CachedSlots.Num(); ++Index)
	{
		OnSkillSlotChanged.Broadcast(Index, CachedSlots[Index]);
	}
	OnHUDReady.Broadcast(false);
}

void UHeroHUDController::SetSlotConfig(UHeroHUDSlotConfig* InConfig)
{
	if (SlotConfig == InConfig)
	{
		EnsureSlotCacheSize();
		return;
	}

	// 换配置 = 槽位集合变了：旧的标签事件是按旧配置绑的，依据已经没了，必须整体重来。
	UnbindSelf();
	SlotConfig = InConfig;
	EnsureSlotCacheSize();

	for (int32 Index = 0; Index < CachedSlots.Num(); ++Index)
	{
		CachedSlots[Index] = FSkillSlotView();
		CachedSlots[Index].SlotIndex = Index;
	}

	if (BoundASC.IsValid())
	{
		BindSelf();
		BroadcastInitialValues();
		UpdateCooldownTicker();
	}
}

int32 UHeroHUDController::GetNumSlots() const
{
	return SlotConfig ? SlotConfig->NumSlots() : 0;
}

void UHeroHUDController::EnsureSlotCacheSize()
{
	const int32 Desired = GetNumSlots();
	if (CachedSlots.Num() != Desired)
	{
		CachedSlots.SetNum(Desired);
	}

	for (int32 Index = 0; Index < CachedSlots.Num(); ++Index)
	{
		// 默认构造出来的 SlotIndex 是 INDEX_NONE，所以这里能认出「新格」。
		if (CachedSlots[Index].SlotIndex != Index)
		{
			CachedSlots[Index] = FSkillSlotView();
			CachedSlots[Index].SlotIndex = Index;
			CachedSlots[Index].State = ESkillSlotState::Disabled;
			CachedSlots[Index].BlockReasons = static_cast<int32>(ESkillSlotBlockReason::NoAbility);
		}
	}
}

// ===========================================================================
// 事件回调
// ===========================================================================

void UHeroHUDController::OnHealthAttributeChanged(const FOnAttributeChangeData& Data)
{
	RebuildVitals(false);
}

void UHeroHUDController::OnMaxHealthAttributeChanged(const FOnAttributeChangeData& Data)
{
	RebuildVitals(false);
}

void UHeroHUDController::OnEnergyAttributeChanged(const FOnAttributeChangeData& Data)
{
	// 能量同时喂两条通道：能量条本身 + 「蓝不够」的灰化。
	RebuildVitals(false);
	RebuildAllSlots(false);
}

void UHeroHUDController::OnMaxEnergyAttributeChanged(const FOnAttributeChangeData& Data)
{
	RebuildVitals(false);
	RebuildAllSlots(false);
}

void UHeroHUDController::OnCooldownTagChanged(const FGameplayTag ChangedTag, int32 /*NewCount*/)
{
	if (!SlotConfig)
	{
		return;
	}

	// 由标签反查槽位，而不是每个槽各绑一个 lambda：绑定表就少一份状态，
	// 解绑时也不用额外记住「这个句柄属于哪个槽」。
	const int32 SlotIndex = SlotConfig->IndexOfCooldownTag(ChangedTag);
	if (SlotIndex == INDEX_NONE)
	{
		return;
	}

	// 【状态翻转的唯一入口】
	//
	// 这里刻意不用回调给的 NewCount，而是让 RebuildSlot 重新推导 —— NewCount 只代表「变了」，
	// 状态一律以 HasMatchingGameplayTag 为准，这样回调顺序、同帧多次变化都不会影响结果。
	//
	// 同时也是「倒计时跑完不点亮图标」的落点：心跳只改数字，点亮只发生在这里。
	// 预测回滚时数字会回弹，但标签只会被服务端那份权威 GE 纠正，所以状态不会跟着乱跳。
	RebuildSlot(SlotIndex, false);

	// 有冷却就开心跳、全好了就停。开关只写在这一处。
	UpdateCooldownTicker();
}

void UHeroHUDController::OnStatusTagChanged(const FGameplayTag ChangedTag, int32 NewCount)
{
	// 死亡 / 眩晕 / 沉默。注意死亡只影响「压暗」，不影响冷却数字继续走 ——
	// 两条通道互相独立，所以这里不需要（也不应该）去动冷却。
	RebuildAllSlots(false);
}

void UHeroHUDController::OnObservedHealthChanged(const FOnAttributeChangeData& Data)
{
	RefreshTargetFrame(false);
}

void UHeroHUDController::OnObservedMaxHealthChanged(const FOnAttributeChangeData& Data)
{
	RefreshTargetFrame(false);
}

void UHeroHUDController::OnObservedEnergyChanged(const FOnAttributeChangeData& Data)
{
	RefreshTargetFrame(false);
}

void UHeroHUDController::OnObservedMaxEnergyChanged(const FOnAttributeChangeData& Data)
{
	RefreshTargetFrame(false);
}

void UHeroHUDController::OnObservedStatusTagChanged(const FGameplayTag ChangedTag, int32 NewCount)
{
	RefreshTargetFrame(false);
}

// ===========================================================================
// 重算 / 广播
// ===========================================================================

void UHeroHUDController::RebuildVitals(bool bForceBroadcast)
{
	FHUDVitalsView New;

	if (const UMyAbilitySystemComponent* ASC = BoundASC.Get())
	{
		New.Health = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetHealthAttribute());
		New.MaxHealth = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetMaxHealthAttribute());
		New.Energy = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetEnergyAttribute());
		New.MaxEnergy = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetMaxEnergyAttribute());

		New.HealthPercent = New.MaxHealth > 0.f ? FMath::Clamp(New.Health / New.MaxHealth, 0.f, 1.f) : 0.f;
		New.EnergyPercent = New.MaxEnergy > 0.f ? FMath::Clamp(New.Energy / New.MaxEnergy, 0.f, 1.f) : 0.f;
	}

	if (bForceBroadcast || !New.EqualsForUI(CachedVitals))
	{
		CachedVitals = New;
		OnVitalsChanged.Broadcast(CachedVitals);
	}
}

void UHeroHUDController::RebuildSlot(int32 SlotIndex, bool bForceBroadcast)
{
	EnsureSlotCacheSize();
	if (!CachedSlots.IsValidIndex(SlotIndex))
	{
		return;
	}

	UMyAbilitySystemComponent* ASC = BoundASC.Get();
	const FHeroHUDSlotEntry* Entry = SlotConfig ? SlotConfig->FindEntry(SlotIndex) : nullptr;

	FSkillSlotView New;
	New.SlotIndex = SlotIndex;

	// 静态表现每次都带上：Widget 因此完全不需要认识 UHeroHUDSlotConfig
	// （那份资产持有 FGameplayTag，会把它拖进 GAS 依赖）。
	if (Entry)
	{
		New.Icon = Entry->Icon;
		New.KeyLabel = Entry->KeyLabel;
		New.DisplayName = Entry->DisplayName;
	}

	ESkillSlotBlockReason Reasons = ESkillSlotBlockReason::None;

	if (!ASC || !Entry || !Entry->SlotTag.IsValid())
	{
		Reasons |= ESkillSlotBlockReason::NoAbility;
	}
	else
	{
		const FGameplayAbilitySpecHandle Handle = ASC->GetHandleForSlot(Entry->SlotTag);
		const UMyGameplayAbility* Ability = ASC->GetAbilityForSlot(Entry->SlotTag);

		if (!Handle.IsValid() || !Ability)
		{
			Reasons |= ESkillSlotBlockReason::NoAbility;
		}
		else
		{
			New.bHasAbility = true;

			// ---- ① 冷却：标签是权威 ----
			if (Entry->CooldownTag.IsValid() && ASC->HasMatchingGameplayTag(Entry->CooldownTag))
			{
				Reasons |= ESkillSlotBlockReason::Cooldown;
			}

			// ---- ② 冷却是【现场采样】，不是本地递减 ----
			// 每 30Hz 都重算一遍完整状态，所以数字和状态天然同帧、不会对不上。
			float Remaining = 0.f;
			float Duration = 0.f;
			if (QueryCooldown(SlotIndex, Remaining, Duration))
			{
				New.CooldownRemaining = FMath::Max(0.f, Remaining);
				New.CooldownDuration = Duration;
				// 进度是 UX 需要的那个量，顺手算好，Widget 别自己除。
				New.CooldownPercent = Duration > 0.f ? FMath::Clamp(Remaining / Duration, 0.f, 1.f) : 0.f;
			}

			// ---- ③ 蓝不够 ----
			// 这条现在恒为 false：ManaCost 目前恒为 0（见属性集里 Energy 的注释）。
			// 代码先立好，等成本 GE 接上就自动生效，不用回来改 HUD。
			if (Ability->ManaCost > 0.f)
			{
				const float Energy = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetEnergyAttribute());
				if (Energy < Ability->ManaCost)
				{
					Reasons |= ESkillSlotBlockReason::NotEnoughEnergy;
				}
			}

			// ---- ④ 死亡 / 眩晕 / 沉默 ----
			//
			// 判据是【能力自己声明的】ActivationBlockedTags，不是 HUD 猜的。好处是
			// 「沉默只挡法术不挡普攻」这种语义自动正确：HUD 不需要知道哪些技能算法术，
			// 能力在构造函数里加 State.Silenced，这里就自动跟着灰。
			//
			// DoesAbilitySatisfyTagRequirements 就是 CanActivateAbility 里那道标签检查
			// （GameplayAbility.cpp:408），但【不含冷却 / 消耗 / 共享冷却】，也不吃 Tick ——
			// 它只读标签容器，可以安全地在 CDO 上调用。别改成 CanActivateAbility：
			// 那个是给服务端判定用的，会在客户端对「还没同步过来的状态」给出答案。
			if (!Ability->DoesAbilitySatisfyTagRequirements(*ASC))
			{
				// 具体原因只是给 tooltip 用的。认不出来时落 BlockedByTags —— 宁可灰错不可亮错。
				if (ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Dead))
				{
					Reasons |= ESkillSlotBlockReason::Dead;
				}
				else if (ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Stunned))
				{
					Reasons |= ESkillSlotBlockReason::Stunned;
				}
				else if (ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Silenced))
				{
					Reasons |= ESkillSlotBlockReason::Silenced;
				}
				else
				{
					Reasons |= ESkillSlotBlockReason::BlockedByTags;
				}
			}
		}
	}

	New.BlockReasons = static_cast<int32>(Reasons);
	New.State = ResolveSlotState(New.bHasAbility, Reasons);

	if (bForceBroadcast || !New.EqualsForUI(CachedSlots[SlotIndex]))
	{
		CachedSlots[SlotIndex] = New;
		OnSkillSlotChanged.Broadcast(SlotIndex, CachedSlots[SlotIndex]);
	}
}

void UHeroHUDController::RebuildAllSlots(bool bForceBroadcast)
{
	EnsureSlotCacheSize();

	for (int32 SlotIndex = 0; SlotIndex < CachedSlots.Num(); ++SlotIndex)
	{
		RebuildSlot(SlotIndex, bForceBroadcast);
	}
}

void UHeroHUDController::RefreshTargetFrame(bool bForceBroadcast)
{
	FTargetFrameView New;

	if (AActor* Target = ObservedTarget.Get())
	{
		New.bHasTarget = true;
		New.DisplayName = ResolveTargetDisplayName(Target);

		if (const UAbilitySystemComponent* ASC = ObservedASC.Get())
		{
			New.Health = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetHealthAttribute());
			New.MaxHealth = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetMaxHealthAttribute());
			New.Energy = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetEnergyAttribute());
			New.MaxEnergy = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetMaxEnergyAttribute());
			New.bIsDead = ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Dead);
		}

		New.HealthPercent = New.MaxHealth > 0.f ? FMath::Clamp(New.Health / New.MaxHealth, 0.f, 1.f) : 0.f;
		New.EnergyPercent = New.MaxEnergy > 0.f ? FMath::Clamp(New.Energy / New.MaxEnergy, 0.f, 1.f) : 0.f;
	}

	if (bForceBroadcast || !New.EqualsForUI(CachedTarget))
	{
		CachedTarget = New;
		OnTargetFrameChanged.Broadcast(CachedTarget);
	}
}

// ===========================================================================
// 心跳 / 查询
// ===========================================================================

void UHeroHUDController::UpdateCooldownTicker()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 「有冷却才启、全好了停」的判断依据是【标签】，不是剩余秒数：
	// 秒数可能因为预测回滚、或者客户端本地那份 GE 还没复制到而提前归零，
	// 这时候心跳一停，后面就再也没人把它拉起来了（能拉起来的只有标签事件，而标签没变）。
	bool bAnyCooling = false;
	for (const FSkillSlotView& Slot : CachedSlots)
	{
		if (Slot.IsCoolingDown())
		{
			bAnyCooling = true;
			break;
		}
	}

	FTimerManager& TimerManager = World->GetTimerManager();
	if (bAnyCooling)
	{
		if (!TimerManager.IsTimerActive(CooldownTickHandle))
		{
			TimerManager.SetTimer(
				CooldownTickHandle, this, &UHeroHUDController::SampleCooldowns, CooldownTickInterval, /*bLoop=*/true);
		}
	}
	else
	{
		TimerManager.ClearTimer(CooldownTickHandle);
	}
}

void UHeroHUDController::SampleCooldowns()
{
	// 心跳【只改数字】，绝不改状态。
	//
	// 这是整个 HUD 里最容易写错的一处。如果在这里顺手把「Remaining <= 0」翻成可放，
	// 服务端拒绝激活（预测回滚）之后客户端就会出现「图标亮着但按下去被拒」——
	// 因为标签还在，技能其实还不能放。
	//
	// 反过来说：数字可以回弹（回滚时视觉上跳一下），状态不可以。
	for (int32 SlotIndex = 0; SlotIndex < CachedSlots.Num(); ++SlotIndex)
	{
		if (!CachedSlots[SlotIndex].IsCoolingDown())
		{
			continue;
		}

		// 在旧值基础上改：State / BlockReasons / 静态表现一律不动。
		FSkillSlotView New = CachedSlots[SlotIndex];

		float Remaining = 0.f;
		float Duration = 0.f;
		if (QueryCooldown(SlotIndex, Remaining, Duration))
		{
			New.CooldownRemaining = FMath::Max(0.f, Remaining);
			New.CooldownDuration = Duration;
			New.CooldownPercent = Duration > 0.f ? FMath::Clamp(Remaining / Duration, 0.f, 1.f) : 0.f;
		}
		else
		{
			// 查不到（本地那份冷却 GE 还没到 / 已经没了）：数字归零，状态维持「冷却中」。
			// 保守方向是对的 —— 显示 0 秒而已，等标签落了自然点亮。
			New.CooldownRemaining = 0.f;
			New.CooldownPercent = 0.f;
		}

		if (!New.EqualsForUI(CachedSlots[SlotIndex]))
		{
			CachedSlots[SlotIndex] = New;
			OnSkillSlotChanged.Broadcast(SlotIndex, CachedSlots[SlotIndex]);
		}
		else
		{
			// 数值没变也要存回去：Duration 可能被后台悄悄修正过（急速变化）。
			CachedSlots[SlotIndex] = New;
		}
	}

	UpdateCooldownTicker();
}

bool UHeroHUDController::QueryCooldown(int32 SlotIndex, float& OutRemaining, float& OutDuration) const
{
	OutRemaining = 0.f;
	OutDuration = 0.f;

	UMyAbilitySystemComponent* ASC = BoundASC.Get();
	if (!ASC)
	{
		return false;
	}

	// ActorInfo 必须有：GetCooldownTimeRemainingAndDuration 要靠它拿 ASC，
	// 内部是 ensure(AbilitySystemComponent)，没有的话会报 ensure 而不是安静地返回 0。
	if (!ASC->AbilityActorInfo.IsValid() || !ASC->AbilityActorInfo->AbilitySystemComponent.IsValid())
	{
		return false;
	}

	const FHeroHUDSlotEntry* Entry = SlotConfig ? SlotConfig->FindEntry(SlotIndex) : nullptr;
	if (!Entry || !Entry->SlotTag.IsValid())
	{
		return false;
	}

	const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(ASC->GetHandleForSlot(Entry->SlotTag));
	if (!Spec || !Spec->Ability)
	{
		return false;
	}

	// 【全项目读冷却数值的唯一一处】
	//
	// 走技能而不是 ASC：UE5.8 的 UAbilitySystemComponent 上没有这个函数，只有 UGameplayAbility 有。
	// CDO 上也安全 —— 实现里只读 CooldownGameplayEffectClass / CooldownTags（都来自 CDO）
	// 和传进来的 ActorInfo，不碰任何实例状态。
	//
	// 拿到的 Duration【已经含急速】（UMyGameplayAbility::ApplyCooldown 填入的是
	// ComputeCooldownWithAbilityHaste 的结果），所以 UI 绝对不要再乘一次 100/(100+Haste)。
	// 又因为它每次都是现场查的，冷却中途改急速也会立刻反映出来，不需要监听 AbilityHaste。
	Spec->Ability->GetCooldownTimeRemainingAndDuration(
		Spec->Handle, ASC->AbilityActorInfo.Get(), OutRemaining, OutDuration);

	return true;
}

ESkillSlotState UHeroHUDController::ResolveSlotState(bool bHasAbility, ESkillSlotBlockReason Reasons) const
{
	// 优先级只写这一处。Widget 拿到 State 直接播，不该有第二个优先级判断。
	if (!bHasAbility)
	{
		return ESkillSlotState::Disabled;
	}

	// 灰化（不可用但不是冷却）：死亡 > 眩晕 > 沉默 > 未识别的阻断 > 蓝不够。
	// 这里不用区分先后 —— 旗帜是位组合，全都会带上，顺序只影响 tooltip 怎么念。
	if (EnumHasAnyFlags(Reasons, ESkillSlotBlockReason::Dead)
		|| EnumHasAnyFlags(Reasons, ESkillSlotBlockReason::Stunned)
		|| EnumHasAnyFlags(Reasons, ESkillSlotBlockReason::Silenced)
		|| EnumHasAnyFlags(Reasons, ESkillSlotBlockReason::BlockedByTags)
		|| EnumHasAnyFlags(Reasons, ESkillSlotBlockReason::NotEnoughEnergy))
	{
		return ESkillSlotState::Greyed;
	}

	if (EnumHasAnyFlags(Reasons, ESkillSlotBlockReason::Cooldown))
	{
		return ESkillSlotState::Cooled;
	}

	return ESkillSlotState::Normal;
}

FText UHeroHUDController::ResolveTargetDisplayName(const AActor* Target) const
{
	if (const APawn* Pawn = Cast<APawn>(Target))
	{
		if (const APlayerState* PlayerState = Pawn->GetPlayerState())
		{
			const FString PlayerName = PlayerState->GetPlayerName();
			if (!PlayerName.IsEmpty())
			{
				return FText::FromString(PlayerName);
			}
		}
	}

	// 小兵 / 野怪 / 建筑没有 PlayerState，退回 Actor 名（蓝图里可以再覆盖一遍）。
	return FText::FromString(GetNameSafe(Target));
}

// ===========================================================================
// Observed 通道
// ===========================================================================

void UHeroHUDController::SetObservedTarget(AActor* NewTarget)
{
	// 目标可能已经被销毁 —— 弱引用会自己变 null，这里先归一化，
	// 免得出现「指针非空但 IsValid 为假」的中间态被当成有效目标。
	if (NewTarget && !IsValid(NewTarget))
	{
		NewTarget = nullptr;
	}

	UAbilitySystemComponent* NewASC =
		NewTarget ? UMyAbilitySystemComponent::FindAbilitySystemComponent(NewTarget) : nullptr;

	if (NewTarget == ObservedTarget.Get() && NewASC == ObservedASC.Get())
	{
		// 目标没变：只刷新一次（数值可能变了），不重绑。
		RefreshTargetFrame(false);
		return;
	}

	// 幂等重绑：先摘后挂。目标被销毁 / 切到新目标 / 切回 nullptr，
	// 三种走的是同一条路径 —— 漏了任何一条都会表现成「血条乱跳」。
	UnbindObserved();

	ObservedTarget = NewTarget;
	ObservedASC = NewASC;

	if (NewASC)
	{
		ObservedHealthHandle = NewASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetHealthAttribute())
			.AddUObject(this, &UHeroHUDController::OnObservedHealthChanged);
		ObservedMaxHealthHandle = NewASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxHealthAttribute())
			.AddUObject(this, &UHeroHUDController::OnObservedMaxHealthChanged);
		ObservedEnergyHandle = NewASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetEnergyAttribute())
			.AddUObject(this, &UHeroHUDController::OnObservedEnergyChanged);
		ObservedMaxEnergyHandle = NewASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxEnergyAttribute())
			.AddUObject(this, &UHeroHUDController::OnObservedMaxEnergyChanged);

		// 目标死了要压暗目标框 —— 这是目标通道唯一的标签事件。
		const FDelegateHandle Handle = NewASC
			->RegisterGameplayTagEvent(LOLGameplayTags::State_Dead, EGameplayTagEventType::NewOrRemoved)
			.AddUObject(this, &UHeroHUDController::OnObservedStatusTagChanged);
		ObservedTagEvents.Add({ LOLGameplayTags::State_Dead, Handle });
	}

	// 目标换了：强制推一次，哪怕数值碰巧一样（bHasTarget 变了也必须让 UI 知道）。
	RefreshTargetFrame(true);
}
