// 竞技场 HUD 根控件。设计意图全在头文件里。

#include "UI/ArenaHUDWidget.h"

#include "Engine/World.h"
#include "TimerManager.h"

#include "GAS/ArenaPlayerController.h"
#include "UI/ArenaAnnounceWidget.h"
#include "UI/ArenaHUDController.h"
#include "UI/ArenaLoadoutBarWidget.h"
#include "UI/ArenaLoadingWidget.h"
#include "UI/ArenaMatchStatusWidget.h"
#include "UI/ArenaRewardScreenWidget.h"
#include "UI/ArenaRoundTrackerWidget.h"

void UArenaHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (IsDesignTime())
	{
		return;
	}

	// 子控件的点击往上接一跳。子控件不认识翻译层（见头文件），所以这一跳必须在根上接。
	if (RewardScreen)
	{
		RewardScreen->OnOptionChosen.AddDynamic(this, &UArenaHUDWidget::HandleOptionChosen);
		RewardScreen->OnRerollRequested.AddDynamic(this, &UArenaHUDWidget::HandleRerollRequested);
	}

	BindToController();
}

void UArenaHUDWidget::NativeDestruct()
{
	StopControllerRetry();

	if (RewardScreen)
	{
		RewardScreen->OnOptionChosen.RemoveDynamic(this, &UArenaHUDWidget::HandleOptionChosen);
		RewardScreen->OnRerollRequested.RemoveDynamic(this, &UArenaHUDWidget::HandleRerollRequested);
	}

	UnbindFromController();

	Super::NativeDestruct();
}

// ===========================================================================
// 绑定
// ===========================================================================

void UArenaHUDWidget::BindToController()
{
	if (ArenaHUD)
	{
		return;
	}

	AArenaPlayerController* PC = Cast<AArenaPlayerController>(GetOwningPlayer());
	if (!PC)
	{
		// 不是竞技场 PC（比如这个 WBP 被手工 Add 到了别处）。不是错误，直接不管。
		return;
	}

	UArenaHUDController* Controller = PC->GetArenaHUDController();
	if (!Controller)
	{
		// PC 还没建翻译层。AArenaPlayerController 是先建翻译层再 Super::BeginPlay
		//（那一步才建这个 Widget），所以正常路径上不会走到这里 ——
		// 走到这里说明这个 WBP 是被别处手工 Add 出来的，等一下就好。
		StartControllerRetry();
		return;
	}

	ArenaHUD = Controller;
	StopControllerRetry();

	// 顺序：先订阅、再拉取。订阅管"之后的变化"，拉取管"此刻的状态"；
	// 反过来的话，两者之间发出的变化会丢。
	ArenaHUD->OnPromptChanged.AddDynamic(this, &UArenaHUDWidget::HandlePromptChanged);
	ArenaHUD->OnMatchChanged.AddDynamic(this, &UArenaHUDWidget::HandleMatchChanged);
	ArenaHUD->OnLoadoutChanged.AddDynamic(this, &UArenaHUDWidget::HandleLoadoutChanged);
	ArenaHUD->OnRoundTrackerChanged.AddDynamic(this, &UArenaHUDWidget::HandleRoundTrackerChanged);
	ArenaHUD->OnPhaseChanged.AddDynamic(this, &UArenaHUDWidget::HandlePhaseChanged);

	// 【订阅即拉取】这就是"绑定不触发初始值"那个坑的正解：不管翻译层是在这个
	// Widget 之前还是之后绑上 PlayerState 的，这里一定拿得到当前全量。
	RefreshFromController();
}

void UArenaHUDWidget::UnbindFromController()
{
	if (!ArenaHUD)
	{
		return;
	}

	ArenaHUD->OnPromptChanged.RemoveDynamic(this, &UArenaHUDWidget::HandlePromptChanged);
	ArenaHUD->OnMatchChanged.RemoveDynamic(this, &UArenaHUDWidget::HandleMatchChanged);
	ArenaHUD->OnLoadoutChanged.RemoveDynamic(this, &UArenaHUDWidget::HandleLoadoutChanged);
	ArenaHUD->OnRoundTrackerChanged.RemoveDynamic(this, &UArenaHUDWidget::HandleRoundTrackerChanged);
	ArenaHUD->OnPhaseChanged.RemoveDynamic(this, &UArenaHUDWidget::HandlePhaseChanged);

	ArenaHUD = nullptr;
}

void UArenaHUDWidget::RefreshFromController()
{
	if (!ArenaHUD)
	{
		// 当成一次"重来"——覆盖"Widget 建的时候 PC 还没建翻译层"这种情况。
		BindToController();
		return;
	}

	// 三路各拉一次。三条都是现场构建（见 UArenaHUDController 的说明），
	// 所以这里拿到的就是此刻的真值，不是上一次广播留下的缓存。
	HandlePromptChanged(ArenaHUD->PullPromptState());
	HandleMatchChanged(ArenaHUD->PullMatchState());
	HandleLoadoutChanged(ArenaHUD->PullLoadoutState());
	HandleRoundTrackerChanged(ArenaHUD->PullRoundTrackerState());
}

void UArenaHUDWidget::StartControllerRetry()
{
	UWorld* World = GetWorld();
	if (!World || World->GetTimerManager().IsTimerActive(ControllerRetryHandle))
	{
		return;
	}

	World->GetTimerManager().SetTimer(
		ControllerRetryHandle, this, &UArenaHUDWidget::RetryBind, ControllerRetryInterval, /*bLoop=*/true);
}

void UArenaHUDWidget::StopControllerRetry()
{
	ControllerRetryCount = 0;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ControllerRetryHandle);
	}
}

void UArenaHUDWidget::RetryBind()
{
	if (ArenaHUD)
	{
		StopControllerRetry();
		return;
	}

	if (++ControllerRetryCount > MaxControllerRetries)
	{
		StopControllerRetry();
		UE_LOG(LogTemp, Warning,
			TEXT("UArenaHUDWidget: %d 次重试后仍然拿不到 UArenaHUDController。")
			TEXT("这个 WBP 多半不是 AArenaPlayerController 建的。"),
			MaxControllerRetries);
		return;
	}

	BindToController();
}

// ===========================================================================
// 分发
// ===========================================================================

void UArenaHUDWidget::HandlePromptChanged(const FArenaPromptView& Prompt)
{
	// 没在 WBP 里挂 RewardScreen 就什么都不做 —— 这一块是可选的，
	// 但【没有它这个模式就没法玩】（玩家会在奖励阶段一直等下去），
	// 所以翻译层那边该有的日志一条都不会少。
	if (RewardScreen)
	{
		RewardScreen->ApplyPrompt(Prompt);
	}
}

void UArenaHUDWidget::HandleMatchChanged(const FArenaMatchView& Match)
{
	if (MatchStatus)
	{
		MatchStatus->ApplyMatch(Match);
	}

	// Loading 面板吃同一份数据：它只在 RoundNumber == 0 时显示（显隐自己管），
	// 之后每一帧推送对它都是空操作 —— 不需要单独的生命周期。
	if (LoadingScreen)
	{
		LoadingScreen->ApplyMatch(Match);
	}
}

void UArenaHUDWidget::HandleLoadoutChanged(const FArenaLoadoutView& Loadout)
{
	if (LoadoutBar)
	{
		LoadoutBar->ApplyLoadout(Loadout);
	}
}

void UArenaHUDWidget::HandleRoundTrackerChanged(const FArenaRoundTrackerView& Tracker)
{
	if (RoundTracker)
	{
		RoundTracker->ApplyTracker(Tracker);
	}
}

void UArenaHUDWidget::HandlePhaseChanged(EArenaPhaseView NewPhase)
{
	if (!Announce)
	{
		return;
	}

	// VS 介绍要双方的最新名字，结果横幅要 ResultText —— 都从 MatchStatus 拿
	//（它和本函数同帧收到 HandleMatchChanged，数据是新的；它没挂就直接拉一次翻译层）。
	FArenaMatchView Match;
	if (MatchStatus)
	{
		Match = MatchStatus->GetMatch();
	}
	else if (ArenaHUD)
	{
		Match = ArenaHUD->PullMatchState();
	}

	switch (NewPhase)
	{
	case EArenaPhaseView::Combat:
		// 结算里可能还残留上一幕（VS 播到一半人被打死了）—— PlayVsIntro 会
		// 重置计时并盖掉旧幕，公告层自己的互斥保证两幕不叠。
		Announce->PlayVsIntro(Match);
		break;

	case EArenaPhaseView::MatchEnd:
		Announce->PlayResult(Match);
		break;

	case EArenaPhaseView::Planning:
		// 新回合的备战开始：VS / 结果都该收了。
		// （VS 的自动收场是兜底，这里主动收一次—— 备战倒计时如果被配成
		// 短于 VsIntroSeconds，不能让介绍盖着备战界面。）
		Announce->Dismiss();
		break;

	case EArenaPhaseView::WaitingToStart:
	case EArenaPhaseView::Settlement:
	case EArenaPhaseView::Unknown:
	default:
		// 结算阶段什么也不动：这一幕（死亡表现 / VS 尾声）是画面的一部分，
		// 掐掉它会让"赢了但屏幕瞬间全空"。结果横幅在 MatchEnd 才接棒。
		break;
	}
}

void UArenaHUDWidget::HandleOptionChosen(int32 OptionIndex)
{
	if (ArenaHUD)
	{
		// 唯一的提交入口。翻译层会做"当前有没有待选"的检查再发 RPC。
		ArenaHUD->SubmitChoice(OptionIndex);
	}
}

void UArenaHUDWidget::HandleRerollRequested(int32 OptionIndex)
{
	if (ArenaHUD)
	{
		ArenaHUD->RerollChoice(OptionIndex);
	}
}
