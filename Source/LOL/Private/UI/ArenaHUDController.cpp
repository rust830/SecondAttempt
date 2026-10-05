// 竞技场 HUD 翻译层的实现。设计意图全在头文件里。

#include "UI/ArenaHUDController.h"

#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

#include "GAS/ArenaAugmentData.h"
#include "GAS/ArenaGameState.h"
#include "GAS/ArenaItemData.h"
#include "GAS/ArenaLoadoutComponent.h"
#include "GAS/ArenaPlayerState.h"
#include "GAS/ArenaTypes.h"

#define LOCTEXT_NAMESPACE "ArenaHUD"

// ===========================================================================
// 绑定
// ===========================================================================

AArenaPlayerState* UArenaHUDController::ResolveSelfState() const
{
	// Outer 是 AArenaPlayerController（见头文件的约定）。这里退到 APlayerController
	// 而不是 Cast 到派生类：这个类不需要 PC 上的任何竞技场专用东西，
	// 少一个依赖就少一个"改 PC 就编译不过"的理由。
	const APlayerController* PC = Cast<APlayerController>(GetOuter());
	return PC ? PC->GetPlayerState<AArenaPlayerState>() : nullptr;
}

AArenaGameState* UArenaHUDController::ResolveGameState() const
{
	const APlayerController* PC = Cast<APlayerController>(GetOuter());
	const UWorld* World = PC ? PC->GetWorld() : nullptr;
	return World ? World->GetGameState<AArenaGameState>() : nullptr;
}

bool UArenaHUDController::IsBound() const
{
	return BoundSelfState.IsValid() && BoundGameState.IsValid();
}

void UArenaHUDController::TryBindArena()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 已经在跑就不重排 —— 重排会把定时器推后，心跳就停了。
	// （SetTimer 会覆盖旧的那一条，循环标志也是，所以等价于"重置"，不是"再加一条"。）
	if (!World->GetTimerManager().IsTimerActive(HeartbeatHandle))
	{
		World->GetTimerManager().SetTimer(
			HeartbeatHandle, this, &UArenaHUDController::OnHeartbeat, HeartbeatInterval, /*bLoop=*/true);
	}

	// 立刻来一次：不然从"建好"到"第一帧有内容"之间会空 0.25 秒。
	// 订阅即拉取的那一半 —— Widget 绑上来时能拿到东西，靠的就是这一下已经跑过了。
	OnHeartbeat();
}

void UArenaHUDController::ShutdownArena()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(HeartbeatHandle);
	}

	// 显式摘委托，不靠"定时器绑了 UObject 引擎会收"。理由同 UHeroHUDWidget::NativeDestruct：
	// 引擎会收的是定时器，委托是另一回事 —— 留着的话，PlayerState 被销毁之前的
	// 任何一次广播都会打到这个对象上。
	UnbindAll();
}

void UArenaHUDController::BindSelf(AArenaPlayerState* Self)
{
	if (!Self || BoundSelfState.Get() == Self)
	{
		return;
	}

	UnbindSelf();

	BoundSelfState = Self;

	// 【为什么只订自方的 PlayerState，不订对手的】
	// 对手的 PlayerState 是复制过来的，绑定这一刻它可能还不存在；等它到了再去订阅，
	// 就得写一套"对手换人了 → 重新订阅"的逻辑。而对手那点信息（大场血量、战绩）
	// 本来就会被 0.25 秒的心跳重算一遍 —— 不值得为它引一套会漏的订阅。
	Self->OnPromptChanged.AddDynamic(this, &UArenaHUDController::HandlePromptChanged);
	Self->OnMatchStateChanged.AddDynamic(this, &UArenaHUDController::HandleMatchStateChanged);

	// 装备栏组件是在 AArenaPlayerState 的构造函数里建的（不是在 BeginPlay 里），
	// 而且自带复制，所以这个指针在两端的 PS 上都有值。真拿不到也不算什么 ——
	// 心跳照样会重算装备栏，只是慢 0.25 秒。
	if (UArenaLoadoutComponent* Loadout = Self->GetLoadout())
	{
		Loadout->OnLoadoutChanged.AddDynamic(this, &UArenaHUDController::HandleLoadoutChanged);
	}
}

void UArenaHUDController::BindGameState(AArenaGameState* GameState)
{
	if (!GameState || BoundGameState.Get() == GameState)
	{
		return;
	}

	UnbindGameState();

	BoundGameState = GameState;
	GameState->OnArenaPhaseChanged.AddDynamic(this, &UArenaHUDController::HandleArenaPhaseChanged);
	GameState->OnMatchEnded.AddDynamic(this, &UArenaHUDController::HandleMatchEnded);
}

void UArenaHUDController::UnbindSelf()
{
	if (AArenaPlayerState* Self = BoundSelfState.Get())
	{
		Self->OnPromptChanged.RemoveDynamic(this, &UArenaHUDController::HandlePromptChanged);
		Self->OnMatchStateChanged.RemoveDynamic(this, &UArenaHUDController::HandleMatchStateChanged);

		if (UArenaLoadoutComponent* Loadout = Self->GetLoadout())
		{
			Loadout->OnLoadoutChanged.RemoveDynamic(this, &UArenaHUDController::HandleLoadoutChanged);
		}
	}

	BoundSelfState = nullptr;
}

void UArenaHUDController::UnbindGameState()
{
	if (AArenaGameState* GameState = BoundGameState.Get())
	{
		GameState->OnArenaPhaseChanged.RemoveDynamic(this, &UArenaHUDController::HandleArenaPhaseChanged);
		GameState->OnMatchEnded.RemoveDynamic(this, &UArenaHUDController::HandleMatchEnded);
	}

	BoundGameState = nullptr;
}

void UArenaHUDController::UnbindAll()
{
	UnbindSelf();
	UnbindGameState();
}

// ===========================================================================
// 心跳
// ===========================================================================

void UArenaHUDController::OnHeartbeat()
{
	AArenaPlayerState* Self = ResolveSelfState();
	AArenaGameState* GameState = ResolveGameState();

	if (Self && GameState)
	{
		// 两个都到位了才绑。GameState 先到、PlayerState 后到（或反过来）的时候，
		// 半个绑定的状态会让视图里一半是空的 —— 而那种半空的第一屏看起来就像 bug。
		BindSelf(Self);
		BindGameState(GameState);

		// 绑上就归零：之后偶发的短暂拿不到（Pawn 重建、PlayerState 换实例）
		// 不该继续累加到上限上去。
		BindAttempts = 0;
	}
	else if (++BindAttempts > MaxBindAttempts)
	{
		// 10 秒还绑不上就收手。多半是这张图根本没跑 AArenaGameMode
		// （那 PlayerState / GameState 都不是竞技场的类），一直空转只会白烧。
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(HeartbeatHandle);
		}
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] UArenaHUDController 在 %.1f 秒内没能同时拿到 AArenaPlayerState 和 AArenaGameState，已停止刷新。")
			TEXT("正常情况下这只发生在「这张图不是竞技场」或者「GameMode 没配 AArenaPlayerState/AArenaGameState」时 ——")
			TEXT("后者会让整个模式的复制状态全部缺失，见 AArenaGameMode 构造函数里的注释。"),
			HeartbeatInterval * MaxBindAttempts);
		return;
	}

	BroadcastAllChanges();
}

void UArenaHUDController::HandlePromptChanged()
{
	BroadcastAllChanges();
}

void UArenaHUDController::HandleMatchStateChanged()
{
	BroadcastAllChanges();
}

void UArenaHUDController::HandleLoadoutChanged()
{
	BroadcastAllChanges();
}

void UArenaHUDController::HandleArenaPhaseChanged(EArenaPhase NewPhase)
{
	// 换了相位就重算一遍。新相位是什么在这里【没有用】—— 视图是从 GameState 现场读的，
	// 这个参数只是为了让签名对上动态委托。
	BroadcastAllChanges();
}

void UArenaHUDController::HandleMatchEnded(AArenaPlayerState* Winner)
{
	// 同上：Winner 从 GameState 现读，这里不用它。
	BroadcastAllChanges();
}

// ===========================================================================
// 广播（三条路径的唯一出口）
// ===========================================================================

void UArenaHUDController::BroadcastAllChanges()
{
	// 只推【真的变了】的那一路。这是骨架里最重要的一条：
	// 心跳每 0.25 秒跑一次，三个视图每个都无脑 Broadcast 的话，
	// 一千个 Widget 绑定就会被每秒推 12 次 —— 而其中绝大多数是同一份数据。
	{
		const FArenaPromptView Prompt = BuildPromptView();
		if (!Prompt.EqualsForUI(CachedPrompt))
		{
			CachedPrompt = Prompt;
			OnPromptChanged.Broadcast(CachedPrompt);
		}
	}

	{
		const FArenaMatchView Match = BuildMatchView();
		if (!Match.EqualsForUI(CachedMatch))
		{
			CachedMatch = Match;
			OnMatchChanged.Broadcast(CachedMatch);
		}
	}

	{
		const FArenaLoadoutView Loadout = BuildLoadoutView();
		if (!Loadout.EqualsForUI(CachedLoadout))
		{
			CachedLoadout = Loadout;
			OnLoadoutChanged.Broadcast(CachedLoadout);
		}
	}

	{
		const FArenaRoundTrackerView Tracker = BuildRoundTrackerView();
		if (!Tracker.EqualsForUI(CachedTracker))
		{
			CachedTracker = Tracker;
			OnRoundTrackerChanged.Broadcast(CachedTracker);
		}
	}

	// 相位边沿最后推：先让所有"内容"通道落定，再触发"表现"——
	// VS 介绍 / 结果横幅播的时候，比分栏里同一帧的数据已经是新的。
	{
		const EArenaPhaseView Phase = CachedMatch.Phase;
		if (Phase != CachedPhase)
		{
			CachedPhase = Phase;
			OnPhaseChanged.Broadcast(Phase);
		}
	}
}

// ===========================================================================
// 拉取
// ===========================================================================

FArenaPromptView UArenaHUDController::PullPromptState() const
{
	return BuildPromptView();
}

FArenaMatchView UArenaHUDController::PullMatchState() const
{
	return BuildMatchView();
}

FArenaLoadoutView UArenaHUDController::PullLoadoutState() const
{
	return BuildLoadoutView();
}

FArenaRoundTrackerView UArenaHUDController::PullRoundTrackerState() const
{
	return BuildRoundTrackerView();
}

// ===========================================================================
// 构建视图
// ===========================================================================

FArenaPromptView UArenaHUDController::BuildPromptView() const
{
	FArenaPromptView View;

	AArenaPlayerState* Self = BoundSelfState.Get();
	if (!Self)
	{
		// 没绑上 → bActive 保持 false，界面收起。这里【不】造一个假的空面板：
		// 一个"什么都不能选但摆在屏幕上"的三选一，看起来像功能坏了。
		return View;
	}

	const FArenaPendingPrompt& Prompt = Self->GetPendingPrompt();
	View.bActive = Prompt.bActive;
	View.Title = Prompt.Title;
	View.bRerollable = Prompt.bRerollable;
	View.RerollsLeft = Prompt.RerollsLeft;

	if (!Prompt.bActive)
	{
		return View;
	}

	View.Cards.Reserve(Prompt.Options.Num());
	for (int32 Index = 0; Index < Prompt.Options.Num(); ++Index)
	{
		const FArenaChoiceOption& Option = Prompt.Options[Index];

		FArenaChoiceCardView Card;
		// 【下标就是这里定的】服务端只认下标，所以卡片顺序必须原样等于待选顺序 ——
		// 这个数组不排序、不去重、不按品质分组，就是为了保住这件事。
		Card.OptionIndex = Index;
		Card.Label = Option.Label;
		Card.Description = Option.Description;
		Card.Icon = ResolveAsset(Option.Icon);
		// 服务端会把不合法的选项拒掉（并记 Warning），所以这里提前问一次 ——
		// 让玩家点一个"点了没反应"的按钮，比一开始就画成灰的糟糕得多。
		Card.bInteractable = Option.IsValidOption();
		// ⚠️ Finish 目前【没有生产方】（见 EArenaPromptAction::Finish 的注释），
		// 所以这一行恒为 false。保留它是因为卡片边框样式（FinishFrame）已经接好了，
		// 将来恢复「手动结束选择」时零改动。
		Card.bIsFinishAction = Option.Action == EArenaPromptAction::Finish;

		// 卡片上的重随按钮：整份待选可重随、还有次数、这张卡本身可结算，三者都满足才亮。
		// 「进入战斗」那种流程按钮没有"换一批"可言，永远不亮。
		Card.bRerollable = Prompt.bRerollable && Prompt.RerollsLeft > 0 && Card.bInteractable;
		Card.RerollsLeft = Prompt.RerollsLeft;

		// 品质映射：所有"会拿到东西"的选项都带档位上卡（强化符文 / 装备三选一 / 分支礼包），
		// 档位在 GameMode 侧就填进 Option.Tier，这里只做一次枚举转换。
		// 锻造器菜单的按钮单独表现它抽的是哪一档装备（传说→金、棱彩→棱彩）。
		if (Option.Action == EArenaPromptAction::Grant)
		{
			Card.Tier = ArenaAugmentTierToCardTier(Option.Tier);
		}
		else if (Option.Action == EArenaPromptAction::UseForge)
		{
			Card.Tier = ArenaItemTierToCardTier(Option.Branch.Tier);
		}

		View.Cards.Add(MoveTemp(Card));
	}

	return View;
}

FArenaMatchView UArenaHUDController::BuildMatchView() const
{
	FArenaMatchView View;

	AArenaGameState* GameState = BoundGameState.Get();
	AArenaPlayerState* Self = BoundSelfState.Get();
	if (!GameState || !Self)
	{
		return View;
	}

	View.RoundNumber = GameState->GetRoundNumber();
	View.RoundText = View.RoundNumber > 0
		? FText::Format(LOCTEXT("ArenaRoundFmt", "第 {0} 回合"), FText::AsNumber(View.RoundNumber))
		: FText::GetEmpty();

	View.Phase = ArenaPhaseToView(GameState->GetArenaPhase());
	View.PhaseText = ArenaPhaseDisplayName(GameState->GetArenaPhase());

	View.PhaseRemainingSeconds = GameState->GetPhaseRemainingSeconds();
	View.bHasCountdown = View.PhaseRemainingSeconds > 0.f;
	if (View.bHasCountdown)
	{
		// 向上取整：显示 1 的时候还剩不到 1 秒，显示 0 才是真到了。
		// 用 FloorToInt 的话会在还剩 0.9 秒时显示 0，看起来像卡住了。
		View.CountdownText = FText::AsNumber(FMath::CeilToInt(View.PhaseRemainingSeconds));
	}

	BuildContenderView(Self, /*bIsSelf=*/true, View.Self);
	// 对手从 GameState 里取，而不是"Contenders 里不是我的那个"—— 那个判断写在这里
	// 就等于把"1V1"这个前提抄进了 UI 层。GetOpponentOf 在 GameState 上，
	// 那边才是定义参赛关系的地方。
	BuildContenderView(GameState->GetOpponentOf(Self), /*bIsSelf=*/false, View.Opponent);

	// --- 上一回合的结果（√/× + 扣血）---
	//
	// 服务端记的是【赢家的下标】；"我赢了还是我输了"要拿它和自己的下标比。
	// 自己的下标在这里现查一次（一个 2 元素的表，代价为零）。
	int32 SelfSlot = INDEX_NONE;
	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		if (GameState->GetContender(Slot) == Self)
		{
			SelfSlot = Slot;
			break;
		}
	}

	const int32 WinnerSlot = GameState->GetLastRoundWinnerSlot();
	if (WinnerSlot != INDEX_NONE && SelfSlot != INDEX_NONE)
	{
		if (WinnerSlot == SelfSlot)
		{
			// 赢家只报结果。HpLoss 记的是【败方】掉的血，给赢家显示"−15"
			// 等于把自己没受的伤挂在自己头上（真踩过）。
			View.LastRoundText = LOCTEXT("ArenaLastRoundWin", "√ 上回合胜利");
		}
		else
		{
			const FText Outcome = LOCTEXT("ArenaLastRoundLose", "× 上回合失利");
			const int32 HpLoss = GameState->GetLastRoundHpLoss();
			View.LastRoundText = HpLoss > 0
				? FText::Format(LOCTEXT("ArenaLastRoundFmt", "{0}  −{1}"), Outcome, FText::AsNumber(HpLoss))
				: Outcome;
		}
	}

	View.bMatchEnded = GameState->GetArenaPhase() == EArenaPhase::MatchEnd;
	if (View.bMatchEnded)
	{
		AArenaPlayerState* Winner = GameState->GetWinner();
		if (!Winner)
		{
			// nullptr = 平局或中止（有人掉线）。这不是异常：GameMode 的 EndMatch
			// 明确允许 Winner 为空。
			View.ResultText = LOCTEXT("ArenaResultNoWinner", "比赛结束");
		}
		else if (Winner == Self)
		{
			View.ResultText = LOCTEXT("ArenaResultSelfWin", "你赢了");
		}
		else
		{
			View.ResultText = LOCTEXT("ArenaResultSelfLose", "你输了");
		}
	}

	return View;
}

FArenaRoundTrackerView UArenaHUDController::BuildRoundTrackerView() const
{
	FArenaRoundTrackerView View;

	AArenaGameState* GameState = BoundGameState.Get();
	if (!GameState)
	{
		return View;   // bValid = false，回合线收起
	}

	const TArray<EArenaStageKind>& Plan = GameState->GetRoundPlan();
	if (Plan.Num() <= 0)
	{
		return View;   // 服务端还没推（或这张图没有回合计划）
	}

	View.bValid = true;
	View.CurrentRound = GameState->GetRoundNumber();

	View.Stages.Reserve(Plan.Num());
	for (int32 Index = 0; Index < Plan.Num(); ++Index)
	{
		FArenaStageEntryView Entry;
		Entry.Kind = ArenaStageKindToView(Plan[Index]);

		// 回合号 1 起、下标 0 起，所以"第 N 回合"就是下标 N-1。
		// RoundNumber 为 0（还没开局）时全部 Upcoming —— 那正是事实。
		if (View.CurrentRound > 0 && Index + 1 < View.CurrentRound)
		{
			Entry.State = EArenaStageState::Completed;
		}
		else if (View.CurrentRound > 0 && Index + 1 == View.CurrentRound)
		{
			Entry.State = EArenaStageState::Current;
		}
		else
		{
			Entry.State = EArenaStageState::Upcoming;
		}

		View.Stages.Add(MoveTemp(Entry));
	}

	return View;
}

void UArenaHUDController::BuildContenderView(AArenaPlayerState* PS, bool bIsSelf, FArenaContenderView& Out) const
{
	Out = FArenaContenderView();
	Out.bIsSelf = bIsSelf;

	if (!PS)
	{
		// bValid 保持 false：这个位子还空着（还在等人 / Bot 还没进来）。
		// UI 应该收起这一栏，而不是画一个 0/100 的空血条。
		return;
	}

	Out.bValid = true;
	Out.DisplayName = FText::FromString(PS->GetPlayerName());
	Out.MatchHealth = PS->GetMatchHealth();
	// 除零保护：MaxMatchHealth 是 UPROPERTY，可以被配成 0（ClampMin 只在编辑器里拦人手输入，
	// 拦不住代码里 Set 一个 0）。真配成 0 时按 1 算，血条画满，而不是变成 NaN。
	Out.MaxMatchHealth = FMath::Max(1, PS->GetMaxMatchHealth());
	Out.HealthPercent = FMath::Clamp(
		static_cast<float>(Out.MatchHealth) / static_cast<float>(Out.MaxMatchHealth), 0.f, 1.f);
	Out.RoundsWon = PS->GetRoundsWon();
	Out.RoundsLost = PS->GetRoundsLost();

	// 文本和上面的数字由同一次构建填出，不会出现"条是满的、字也是满的、其实血少了一半"。
	Out.HealthText = MakeHealthText(Out.MatchHealth, Out.MaxMatchHealth);
	Out.ScoreText = MakeScoreText(Out.RoundsWon, Out.RoundsLost);

	return;
}

FArenaLoadoutView UArenaHUDController::BuildLoadoutView() const
{
	FArenaLoadoutView View;

	AArenaPlayerState* Self = BoundSelfState.Get();
	UArenaLoadoutComponent* Loadout = Self ? Self->GetLoadout() : nullptr;
	if (!Loadout)
	{
		return View;
	}

	// 装备在前、海克斯在后 —— 这个顺序就是 FArenaLoadoutView::Entries 的约定，
	// 两段紧挨着写，免得以后有人把海克斯那段挪到前面去。
	for (const TSoftObjectPtr<UArenaItemData>& SoftItem : Loadout->GetEquippedItems())
	{
		UArenaItemData* Item = ResolveAsset(SoftItem);
		if (!Item)
		{
			// 资产路径解析不出来（改名 / 删了 / 客户端还没加载到）。跳过这一格，
			// 而不是画一个空白格子 —— 空白格子看起来像"这件装备没有图标"。
			continue;
		}

		FArenaLoadoutEntryView Entry;
		Entry.DisplayName = ResolveDisplayName(Item->DisplayName, Item);
		Entry.Description = Item->Description;
		Entry.Icon = ResolveAsset(Item->Icon);
		Entry.bIsAugment = false;
		View.Entries.Add(MoveTemp(Entry));
	}

	for (const TSoftObjectPtr<UArenaAugmentData>& SoftAugment : Loadout->GetEquippedAugments())
	{
		UArenaAugmentData* Augment = ResolveAsset(SoftAugment);
		if (!Augment)
		{
			continue;
		}

		FArenaLoadoutEntryView Entry;
		Entry.DisplayName = ResolveDisplayName(Augment->DisplayName, Augment);
		Entry.Description = Augment->Description;
		Entry.Icon = ResolveAsset(Augment->Icon);
		Entry.bIsAugment = true;
		Entry.Tier = ArenaAugmentTierToCardTier(Augment->Tier);
		View.Entries.Add(MoveTemp(Entry));
	}

	for (const FArenaForgeCharge& Charge : Loadout->GetForgeCharges())
	{
		// 花完的档位不画（组件那边【刻意】不删 0 次数的条目，见 GetForgeCharges 的注释：
		// 删了会让两个品质的徽章互相换位置）。所以"跳过 0"这一步是 UI 的责任。
		if (Charge.Count <= 0)
		{
			continue;
		}

		FArenaForgeChargeView ChargeView;
		ChargeView.DisplayName = FText::Format(
			LOCTEXT("ArenaForgeChargeFmt", "{0}锻造器"), ArenaTierDisplayName(Charge.Tier));
		ChargeView.Count = Charge.Count;
		View.ForgeCharges.Add(MoveTemp(ChargeView));
	}

	return View;
}

// ===========================================================================
// 提交
// ===========================================================================

void UArenaHUDController::SubmitChoice(int32 OptionIndex)
{
	AArenaPlayerState* Self = BoundSelfState.Get();
	if (!Self)
	{
		return;
	}

	if (!Self->HasPendingPrompt())
	{
		// 本地界面和服务端不同步（比如那一份待选刚好被答完）。这种点击不值得发 RPC，
		// 但也不能静默 —— 静默的话"点了没反应"会被当成 UI 的 bug 去查。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] SubmitChoice(%d) 被忽略：本地没有待选择。多半是界面比状态慢了一拍。"), OptionIndex);
		return;
	}

	// 只发下标。载荷（到底给了哪件装备）由服务端从它自己的待选里取 ——
	// 客户端说的内容一概不信，这样改一个内存就能给自己发装备的路就堵死了。
	Self->ServerSubmitChoice(OptionIndex);
}

void UArenaHUDController::RerollChoice(int32 OptionIndex)
{
	AArenaPlayerState* Self = BoundSelfState.Get();
	if (!Self)
	{
		return;
	}

	if (!Self->HasPendingPrompt())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] RerollChoice(%d) 被忽略：本地没有待选择。多半是界面比状态慢了一拍。"), OptionIndex);
		return;
	}

	// 同 SubmitChoice：只发下标，合法性全部由服务端判（有没有待选、能不能重随、
	// 次数够不够）。客户端算的任何"可重随"都只是提前置灰，不是权威。
	Self->ServerRerollChoice(OptionIndex);
}

// ===========================================================================
// 格式化
// ===========================================================================

FText UArenaHUDController::ArenaPhaseDisplayName(EArenaPhase Phase)
{
	switch (Phase)
	{
	case EArenaPhase::WaitingToStart:
		return LOCTEXT("ArenaPhaseWaiting", "等待对手");
	case EArenaPhase::RewardSelection:
		return LOCTEXT("ArenaPhasePlanning", "备战");
	case EArenaPhase::Combat:
		return LOCTEXT("ArenaPhaseCombat", "战斗中");
	case EArenaPhase::Settlement:
		return LOCTEXT("ArenaPhaseSettlement", "结算");
	case EArenaPhase::MatchEnd:
		return LOCTEXT("ArenaPhaseMatchEnd", "比赛结束");
	}

	// 加了新相位却忘了在这里加一条时走到这里。返回一个明显不对劲的文本，
	// 好过返回空串（空串会被当成"这个相位没有名字"，看起来像排版问题）。
	return LOCTEXT("ArenaPhaseUnknown", "未知相位");
}

FText UArenaHUDController::ArenaTierDisplayName(EArenaItemTier Tier)
{
	switch (Tier)
	{
	case EArenaItemTier::Legendary:
		return LOCTEXT("ArenaTierLegendary", "传说");
	case EArenaItemTier::Prismatic:
		return LOCTEXT("ArenaTierPrismatic", "棱彩");
	}

	return LOCTEXT("ArenaTierUnknown", "未知品质");
}

EArenaCardTier UArenaHUDController::ArenaAugmentTierToCardTier(EArenaAugmentTier Tier)
{
	switch (Tier)
	{
	case EArenaAugmentTier::Silver:
		return EArenaCardTier::Silver;
	case EArenaAugmentTier::Gold:
		return EArenaCardTier::Gold;
	case EArenaAugmentTier::Prismatic:
		return EArenaCardTier::Prismatic;
	}

	return EArenaCardTier::None;
}

EArenaCardTier UArenaHUDController::ArenaItemTierToCardTier(EArenaItemTier Tier)
{
	switch (Tier)
	{
	case EArenaItemTier::Legendary:
		return EArenaCardTier::Gold;
	case EArenaItemTier::Prismatic:
		return EArenaCardTier::Prismatic;
	}

	return EArenaCardTier::None;
}

EArenaPhaseView UArenaHUDController::ArenaPhaseToView(EArenaPhase Phase)
{
	switch (Phase)
	{
	case EArenaPhase::WaitingToStart:   return EArenaPhaseView::WaitingToStart;
	case EArenaPhase::RewardSelection:  return EArenaPhaseView::Planning;
	case EArenaPhase::Combat:           return EArenaPhaseView::Combat;
	case EArenaPhase::Settlement:       return EArenaPhaseView::Settlement;
	case EArenaPhase::MatchEnd:         return EArenaPhaseView::MatchEnd;
	}

	return EArenaPhaseView::Unknown;
}

EArenaStageKindView UArenaHUDController::ArenaStageKindToView(EArenaStageKind Kind)
{
	switch (Kind)
	{
	case EArenaStageKind::Combat:       return EArenaStageKindView::Combat;
	case EArenaStageKind::Augments:     return EArenaStageKindView::Augments;
	case EArenaStageKind::ItemPurchase: return EArenaStageKindView::ItemPurchase;
	case EArenaStageKind::StatAnvil:    return EArenaStageKindView::StatAnvil;
	}

	return EArenaStageKindView::Combat;
}

FText UArenaHUDController::MakeHealthText(int32 Health, int32 MaxHealth)
{
	return FText::Format(
		LOCTEXT("ArenaHealthFmt", "{0} / {1}"), FText::AsNumber(Health), FText::AsNumber(MaxHealth));
}

FText UArenaHUDController::MakeScoreText(int32 Won, int32 Lost)
{
	return FText::Format(
		LOCTEXT("ArenaScoreFmt", "{0} 胜 {1} 负"), FText::AsNumber(Won), FText::AsNumber(Lost));
}

FText UArenaHUDController::ResolveDisplayName(const FText& DisplayName, const UObject* Asset)
{
	if (!DisplayName.IsEmpty())
	{
		return DisplayName;
	}

	// DisplayName 是 FText，忘了填不报错，界面上就是一块空白。退回资产名至少能
	// 一眼看出是哪件装备没填名字（而资产名本身也是"这个名字没配好"的证据）。
	return Asset ? FText::FromString(Asset->GetName()) : FText::GetEmpty();
}

#undef LOCTEXT_NAMESPACE
