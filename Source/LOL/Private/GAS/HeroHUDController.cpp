// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroHUDController.h"

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "TimerManager.h"

#include "GAS/HeroAttributePanelConfig.h"
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

	if (!AttributePanelConfig)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UHeroHUDController: 没有配 UHeroAttributePanelConfig，属性面板会是空的。")
			TEXT("在 BP_LOLPlayerController 上设 AttributePanelConfig。"));
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

	// ① 属性：血 / 能量。四条绑到【同一个】回调上（见 OnSelfVitalsAttributeChanged）。
	//    用 GetGameplayAttributeValueChangeDelegate，【不用 OnRep_*】——
	//    listen server 上主机是权威端，属性是直接写进去的，OnRep 根本不触发。
	//    用 OnRep 的话症状是「主机看不到自己的血条动」，而且只在多人下才暴露。
	//
	//    这四条的定义在属性集里（GetVitalsAttributes），和下面 Observed 通道共用一份 ——
	//    以前是两处各写一份同样的列表，加一条资源时漏改一处，症状是「目标框的血条不动」。
	SelfBinding.BindAttributes(ASC, UHeroCombatAttributeSet::GetVitalsAttributes(), this, &UHeroHUDController::OnSelfVitalsAttributeChanged);

	// ①b 属性：属性面板上配了哪些就订阅哪些。
	//
	// 【订阅列表来自配置，不是硬编码的 27 条】—— 这是「面板完全数据驱动」的最后一块：
	// 加一行面板 = 在 DA 里加一条，订阅自动跟上，不用回来改这里。
	//
	// 代价：血/能量的 4 条会被订阅两次（vitals 一次、面板一次）。
	// 这是【刻意接受】的 —— FHUDAttributeBinding 的记账本来就支持叠加（每个句柄独立摘），
	// 而为了省这 4 个委托去写「跳过已经绑过的属性」的判断，会把「订阅集合」变成两处推导，
	// 那正是这份配置想避免的东西。
	if (AttributePanelConfig)
	{
		TArray<FGameplayAttribute> PanelAttributes;
		PanelAttributes.Reserve(AttributePanelConfig->Entries.Num());
		for (const FHeroAttributePanelEntry& Entry : AttributePanelConfig->Entries)
		{
			// 只收有效的：空条目（配了行但没选属性）留着的话会绑到 GetAllAttribute 之外的空属性上，
			// 那种委托永远不会触发，纯属白白多一条需要维护的句柄。
			if (Entry.Attribute.IsValid())
			{
				PanelAttributes.Add(Entry.Attribute);
			}
		}

		if (PanelAttributes.Num() > 0)
		{
			SelfBinding.BindAttributes(ASC, PanelAttributes, this, &UHeroHUDController::OnSelfAttributesAttributeChanged);
		}
	}

	// ② 标签：每个槽的冷却标签各一条。
	//    NewOrRemoved = 只在 0→1 / 1→0 触发，中间的 1→2、2→1 不触发 —— 这恰好就是「开关」语义。
	//    不要用 AnyCountChange：同一帧里被两层 GE 挂上时会多抖一次。
	//    没有冷却标签的槽（被动）由 BindTag 自己跳过，这里不用先判一次。
	if (SlotConfig)
	{
		for (const FHeroHUDSlotEntry& Entry : SlotConfig->Slots)
		{
			SelfBinding.BindTag(ASC, Entry.CooldownTag, this, &UHeroHUDController::OnCooldownTagChanged);
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
		SelfBinding.BindTag(ASC, Tag, this, &UHeroHUDController::OnStatusTagChanged);
	}
}

void UHeroHUDController::UnbindSelf()
{
	// 摘句柄的记账全在 FHUDAttributeBinding 里（三处绑定共用一份，就不存在「漏摘一个」）。
	// ASC 已经被销毁的情况它自己会跳过：委托跟着 ASC 一起没了，唯一的要求是把句柄清掉。
	SelfBinding.Unbind();
}

void UHeroHUDController::UnbindObserved()
{
	ObservedBinding.Unbind();

	// 目标身上那条 OnDestroyed 要单独摘：它挂在 Actor 上，不归 ASC 那份记账管。
	// 读的是【当前】的 ObservedTarget —— 所以本函数必须在 ObservedTarget 被改写之前调
	//（SetObservedTarget 正是这个顺序：先 UnbindObserved，再赋新目标）。
	if (AActor* Previous = ObservedTarget.Get())
	{
		Previous->OnDestroyed.RemoveDynamic(this, &UHeroHUDController::OnObservedTargetDestroyed);
	}

	// 解绑和「忘了这个目标」写在同一个函数里，是为了让三条路径
	//（切到别的目标 / 切回 nullptr / 被 SetObservedTarget 归一化成 nullptr）
	// 不可能只做一半 —— 只解绑不清引用，症状是「血条乱跳」；只清引用不解绑，症状是「收到已销毁目标的事件」。
	// 目标框本身的收起由 SetObservedTarget 末尾那次强制刷新负责。
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
	CachedAttributes = FHeroAttributePanelView();
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
	Snapshot.Attributes = CachedAttributes;
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
	RebuildAttributes(true);
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
		Reset.Kind = Old.Kind;
		Reset.State = ESkillSlotState::Disabled;
		Reset.BlockReasons = static_cast<int32>(ESkillSlotBlockReason::NoAbility);

		// 未绑上 = 这个槽上一定没有能力 → 预留位该隐身就隐身。判据走同一个函数。
		const FHeroHUDSlotEntry* Entry = SlotConfig ? SlotConfig->FindEntry(Index) : nullptr;
		Reset.bHidden = IsReservedSlotHidden(Entry, /*bHasAbility=*/false);

		CachedSlots[Index] = Reset;
	}

	// 目标通道和 Self 通道互相独立：Self 没绑上不代表目标框也要收掉。
	RefreshTargetFrame(true);

	// 属性面板：RebuildAttributes 自己在「拿不到 ASC」时会把 bValid 置 false 并广播，
	// 面板据此整块收起 —— 静态表现（图标 / 名字）照样留着，只是不显示 0。
	RebuildAttributes(true);

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
		FSkillSlotView New;
		New.SlotIndex = Index;

		const FHeroHUDSlotEntry* Entry = SlotConfig ? SlotConfig->FindEntry(Index) : nullptr;
		ApplyEntryStatic(New, Entry);
		// 刚换配置、还没重算能力归属：按「暂时没有能力」算，预留位先隐身。
		// 真要是在场就重算，下面 BindSelf + BroadcastInitialValues 会覆盖成准确值。
		New.bHidden = IsReservedSlotHidden(Entry, /*bHasAbility=*/false);

		CachedSlots[Index] = New;
	}

	if (BoundASC.IsValid())
	{
		BindSelf();
		BroadcastInitialValues();
		UpdateCooldownTicker();
	}
}

void UHeroHUDController::RefreshSlotViewsFromConfig()
{
	EnsureSlotCacheSize();

	for (int32 Index = 0; Index < CachedSlots.Num(); ++Index)
	{
		// 【在旧值的基础上改，而不是重新构造一个】—— 冷却秒数、可用性状态、能力归属都是
		// 运行期算出来的，配置刷新不该把它们冲掉（那会表现成"改个尺寸，冷却条全清空了"）。
		FSkillSlotView New = CachedSlots[Index];
		New.SlotIndex = Index;

		const FHeroHUDSlotEntry* Entry = SlotConfig ? SlotConfig->FindEntry(Index) : nullptr;
		ApplyEntryStatic(New, Entry);

		// 隐藏与否 = 配置（bHideWhenUnavailable）× 运行期（这一刻有没有能力），
		// 所以要拿【保留下来的】bHasAbility 重算一遍，不能只刷配置那一半。
		New.bHidden = IsReservedSlotHidden(Entry, New.bHasAbility);

		CachedSlots[Index] = New;
	}

	// 强制重播：这里变的可能只是"配置那一侧"（视图的字段值恰好没变），
	// 走「变了才推」会被挡掉，UI 就永远看不到新尺寸。
	RebuildAllSlots(/*bForceBroadcast=*/true);
}

int32 UHeroHUDController::GetNumSlots() const
{
	return SlotConfig ? SlotConfig->NumSlots() : 0;
}

void UHeroHUDController::SetAttributePanelConfig(UHeroAttributePanelConfig* InConfig)
{
	if (InConfig && InConfig->Entries.Num() == 0)
	{
		// 空配置【不静默】：面板会是空的，而没有任何提示的话，看到的现象是
		// 「HUD 上就是没有属性条」—— 排查时第一个会怀疑的是 Widget 没绑上。
		UE_LOG(LogTemp, Warning,
			TEXT("UHeroHUDController: UHeroAttributePanelConfig(%s) 的 Entries 是空的，属性面板不会有任何行。"),
			*GetNameSafe(InConfig));
	}

	if (AttributePanelConfig == InConfig)
	{
		return;
	}

	// 换配置 = 订阅的属性集合变了（见 BindSelf ①b），旧的句柄依据已经没了，必须整体重来。
	// 和 SetSlotConfig 同一个形状：先解绑、再赋值、再重绑 + 重放一份全量。
	UnbindSelf();
	AttributePanelConfig = InConfig;

	if (BoundASC.IsValid())
	{
		BindSelf();
		BroadcastInitialValues();
	}
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
			// 新格先按「没绑上 ASC」填一份：这样即使第一个 RebuildSlot 还没跑，
			// PullHUDState() 拿到的也是一份自洽的投影（预留位该隐身的已经隐身）。
			FSkillSlotView New;
			New.SlotIndex = Index;
			New.State = ESkillSlotState::Disabled;
			New.BlockReasons = static_cast<int32>(ESkillSlotBlockReason::NoAbility);

			const FHeroHUDSlotEntry* Entry = SlotConfig ? SlotConfig->FindEntry(Index) : nullptr;
			ApplyEntryStatic(New, Entry);
			New.bHidden = IsReservedSlotHidden(Entry, /*bHasAbility=*/false);

			CachedSlots[Index] = New;
		}
	}
}

void UHeroHUDController::ApplyEntryStatic(FSkillSlotView& View, const FHeroHUDSlotEntry* Entry) const
{
	if (!Entry)
	{
		return;
	}

	View.Icon = Entry->Icon;
	View.KeyLabel = Entry->KeyLabel;
	View.DisplayName = Entry->DisplayName;
	View.Kind = Entry->Kind;
	View.SlotSizeScale = Entry->SlotSizeScale;
}

bool UHeroHUDController::IsReservedSlotHidden(const FHeroHUDSlotEntry* Entry, bool bHasAbility)
{
	// 「预留位」= 配置里声明了「没能力就别出现」，而这一刻确实还没有能力。
	//
	// 反过来不成立的地方要注意：没有 Entry（越界 / 配置为空）时【不隐藏】——
	// 那是配置坏了，应该看得见（好排查），而不是安静地少一格。
	return Entry && Entry->bHideWhenUnavailable && !bHasAbility;
}

// ===========================================================================
// 事件回调
// ===========================================================================

void UHeroHUDController::OnSelfVitalsAttributeChanged(const FOnAttributeChangeData& Data)
{
	RebuildVitals(false);

	// 能量同时喂两条通道：能量条本身 + 「蓝不够」的灰化。
	//
	// 按属性分流而不是四条都重算槽位：槽位重算要查冷却（FindAbilitySpecFromHandle +
	// GetCooldownTimeRemainingAndDuration），而血量变化跟技能可用性一点关系都没有 ——
	// 挨打时把六个槽白算一遍没有意义。
	const bool bEnergy = Data.Attribute == UHeroCombatAttributeSet::GetEnergyAttribute()
		|| Data.Attribute == UHeroCombatAttributeSet::GetMaxEnergyAttribute();
	if (bEnergy)
	{
		RebuildAllSlots(false);
	}
}

void UHeroHUDController::OnSelfAttributesAttributeChanged(const FOnAttributeChangeData& Data)
{
	// 不分流：整块重算（理由见头文件）。这是和 vitals 那条【唯一的】区别 ——
	// vitals 会为了「别在挨打时白算六个槽」而按属性分流，面板这边没有那种代价，
	// 而且属性之间会互相影响，分流反而要维护一张「谁影响谁」的表。
	RebuildAttributes(false);
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

void UHeroHUDController::OnObservedVitalsAttributeChanged(const FOnAttributeChangeData& Data)
{
	RefreshTargetFrame(false);
}

void UHeroHUDController::OnObservedStatusTagChanged(const FGameplayTag ChangedTag, int32 NewCount)
{
	RefreshTargetFrame(false);
}

void UHeroHUDController::OnObservedTargetDestroyed(AActor* DestroyedActor)
{
	// 走 SetObservedTarget(nullptr) 而不是直接 RefreshTargetFrame：
	// 「被销毁」和「切回没有目标」必须是同一条路径 —— 目标框收起、句柄摘干净、
	// 缓存里 bHasTarget 归 false，三件事一起做，才有唯一的落点。
	//
	// 注意这里【不能】假设 ObservedTarget 还是非空：Actor 销毁时弱引用可能已经失效，
	// 那种情况下 SetObservedTarget 会走到「前后都是空」那条分支，照样强制刷新一次。
	SetObservedTarget(nullptr);
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

FText UHeroHUDController::FormatAttributeValue(float Value, EHeroAttributeFormat Format)
{
	// 无千分位：属性面板上一格只有几十像素宽，"1,234" 会挤爆，而且 LoL 也不这么显示。
	const FNumberFormattingOptions NoGrouping = FNumberFormattingOptions::DefaultNoGrouping();

	switch (Format)
	{
	case EHeroAttributeFormat::OneDecimal:
	{
		// 最少 / 最多都锁一位：0.8 显示成 "0.8" 而不是 "0.8"、"1" 两种形态混着。
		FNumberFormattingOptions Options = NoGrouping;
		Options.SetMinimumFractionalDigits(1);
		Options.SetMaximumFractionalDigits(1);
		return FText::AsNumber(Value, &Options);
	}

	case EHeroAttributeFormat::Percent:
	{
		// 属性存的是比率（0.25 = 25%），显示口径在这里 ×100 —— 转换只有这一处。
		// 用 FText::Format 而不是拼字符串：% 号是可本地化的，别写死进 Printf。
		return FText::Format(
			NSLOCTEXT("HeroHUD", "AttributePercentFormat", "{0}%"),
			FText::AsNumber(FMath::RoundToInt(Value * 100.f), &NoGrouping));
	}

	case EHeroAttributeFormat::Integer:
	default:
		return FText::AsNumber(FMath::RoundToInt(Value), &NoGrouping);
	}
}

void UHeroHUDController::RebuildAttributes(bool bForceBroadcast)
{
	FHeroAttributePanelView New;
	const UMyAbilitySystemComponent* ASC = BoundASC.Get();
	New.bValid = (ASC != nullptr);

	// 【长度和配置一一对应】，即使拿不到 ASC 也要建出这些行：
	// Widget 按下标认行（同技能槽），少一行会让后面所有行错位，而「收起」是靠 bValid 表达的，
	// 不是靠「数组是空的」。
	if (AttributePanelConfig)
	{
		New.Entries.Reserve(AttributePanelConfig->Entries.Num());

		for (const FHeroAttributePanelEntry& Source : AttributePanelConfig->Entries)
		{
			FHeroAttributeEntryView& Entry = New.Entries.AddDefaulted_GetRef();

			// 静态表现每次都带上：Widget 因此完全不需要认识 UHeroAttributePanelConfig
			//（那份资产持有 FGameplayAttribute，会把它拖进 GAS 依赖）。
			Entry.Icon = Source.Icon;
			Entry.DisplayName = Source.DisplayName;
			Entry.bShowInCompact = Source.bShowInCompact;

			// 未就绪时数值留 0（但 UI 因为 bValid=false 整块收起了，看不见）。
			const float Value = (ASC && Source.Attribute.IsValid())
				? ASC->GetNumericAttribute(Source.Attribute)
				: 0.f;
			Entry.ValueText = FormatAttributeValue(Value, Source.Format);
		}
	}

	if (bForceBroadcast || !New.EqualsForUI(CachedAttributes))
	{
		CachedAttributes = New;
		OnAttributesChanged.Broadcast(CachedAttributes);
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
	ApplyEntryStatic(New, Entry);

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

	// ---- ⑤ 可见性 ----
	//
	// 放在最后算（要等 bHasAbility 定下来），且【和 State 分开】：State 说的是「能不能放」，
	// bHidden 说的是「要不要出现在屏幕上」，两件事不该互相污染 ——
	// 否则「隐身槽位」的语义会随着灰化优先级的调整被顺手改掉。
	New.bHidden = IsReservedSlotHidden(Entry, New.bHasAbility);

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

	// 目标被销毁时没有任何属性/标签事件会打进来（ASC 跟着一起没了），
	// 所以要额外听一条 Actor 自己的销毁通知，否则目标框会冻在最后一帧。
	// sparse 动态委托：只能 AddDynamic，且必须先判空。
	if (NewTarget)
	{
		NewTarget->OnDestroyed.AddDynamic(this, &UHeroHUDController::OnObservedTargetDestroyed);
	}

	if (NewASC)
	{
		// 和 Self 通道用的是【同一份】列表（GetVitalsAttributes 是唯一定义），
		// 和 Self 那四条绑的是两个不同的回调、两套句柄 —— 叠加订阅是合法的，见 §3.3。
		ObservedBinding.BindAttributes(
			NewASC, UHeroCombatAttributeSet::GetVitalsAttributes(), this, &UHeroHUDController::OnObservedVitalsAttributeChanged);

		// 目标死了要压暗目标框 —— 这是目标通道唯一的标签事件。
		ObservedBinding.BindTag(
			NewASC, LOLGameplayTags::State_Dead, this, &UHeroHUDController::OnObservedStatusTagChanged);
	}

	// 目标换了：强制推一次，哪怕数值碰巧一样（bHasTarget 变了也必须让 UI 知道）。
	RefreshTargetFrame(true);
}
