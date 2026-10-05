// 斗魂竞技场：本地玩家输入模式的实现。

#include "GAS/ArenaPlayerController.h"

#include "Blueprint/UserWidget.h"
#include "Engine/World.h"

#include "GAS/ArenaGameState.h"
#include "GAS/ArenaPlayerState.h"
#include "GAS/HeroHUDController.h"
#include "UI/ArenaHUDController.h"

AArenaPlayerController::AArenaPlayerController()
{
	// 默认不碰鼠标：和引擎默认一致（GameOnly + 无光标）。真正的切换只在相位变化时发生，
	// 所以"这个 PC 放在非竞技场关卡里"也不会有任何副作用。
	// primary tick 不用管：APlayerController 本来就 tick，PlayerTick 只对本地玩家跑。
}

void AArenaPlayerController::BeginPlay()
{
	// -----------------------------------------------------------------------
	// 【翻译层必须建在 Super::BeginPlay 之前】
	// 基类的 BeginPlay 会 CreateHUDForLocalPlayer()，而那里面 CreateWidget 会
	// 【同步】触发控件的 NativeConstruct —— 竞技场 HUD 就是在那里向这个 PC 要翻译层的。
	// 顺序反过来的话，第一次绑定必然落空，只能等 0.2 秒后的重试才连上
	//（表现是"开局第一秒三选一是空的"）。
	// -----------------------------------------------------------------------
	CreateArenaHUDControllerForLocalPlayer();

	Super::BeginPlay();   // 基类在这里建英雄 HUD，不能省

	CreateArenaHUDForLocalPlayer();

	// 开局对齐一次。开局相位是 WaitingToStart（GameOnly），所以这一次通常什么都不做 ——
	// 但"通常"不算数：如果是无缝切关卡进来，第一次读到的可能已经是 RewardSelection 了。
	SyncInputModeToPhase();
}

void AArenaPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 翻译层自己收尾：停心跳 + 摘委托。它是个 UObject，不会走 Actor 的 EndPlay，
	// 所以这一句是它唯一的收尾机会。
	if (ArenaHUDController)
	{
		ArenaHUDController->ShutdownArena();
	}

	// 离开时把输入还回去。PIE 里这无所谓（编辑器会重置），
	// 但无缝切关卡 / 打包后切地图时，留着一个 IgnoreInput 的 viewport 会让下一张图
	// 完全没有输入 —— 而且看起来像"新地图是坏的"。
	if (bInputModeTouched && CurrentIntent != EArenaInputIntent::GameOnly)
	{
		ApplyInputMode(EArenaInputIntent::GameOnly);
	}

	Super::EndPlay(EndPlayReason);
}

// ---------------------------------------------------------------------------
// 竞技场 HUD
// ---------------------------------------------------------------------------

void AArenaPlayerController::CreateArenaHUDControllerForLocalPlayer()
{
	// 幂等。三次绑定时机（BeginPlay / AcknowledgePossession / OnRep_PlayerState）里
	// 只有 BeginPlay 会真的建，但幂等这一条不能省 —— 建两个翻译层的话会有两条心跳、
	// 两份缓存，而且两个都会往同一批 Widget 上推。
	if (ArenaHUDController)
	{
		return;
	}

	// 【只给本地玩家建】listen server 上远端客户端的 PC 也会跑 BeginPlay，
	// 不门住的话主机会给每个玩家各建一份（还会看到别人的三选一）。
	// 同 ALOLPlayerController::CreateHUDForLocalPlayer 的理由。
	if (!IsLocalPlayerController())
	{
		return;
	}

	// Outer 传 this：UArenaHUDController 是靠 Outer 找本地 PlayerState / GameState 的
	//（见那边的 ResolveSelfState）。传别的对象当 Outer，它一个都找不到。
	ArenaHUDController = NewObject<UArenaHUDController>(this);

	// 立刻试一次。GameState / PlayerState 这会儿多半还没到（客户端上是复制过来的），
	// 所以这一下通常是失败的 —— 不要紧，翻译层自己排了 0.25 秒一次的重试。
	ArenaHUDController->TryBindArena();
}

void AArenaPlayerController::CreateArenaHUDForLocalPlayer()
{
	if (ArenaHUDWidget)
	{
		return;
	}

	if (!IsLocalPlayerController())
	{
		return;
	}

	if (!ArenaHUDWidgetClass)
	{
		// 没配类 = 这个模式没有界面。不是每张图都需要它（比如纯调试用的竞技场关卡），
		// 所以只说一次，不每帧吵。
		UE_LOG(LogTemp, Log,
			TEXT("[Arena] %s 没有配 ArenaHUDWidgetClass，竞技场不会显示三选一 / 比分 / 装备栏。")
			TEXT("要界面的话在 BP_ArenaPlayerController 上配上 WBP_ArenaHUD。"),
			*GetName());
		return;
	}

	ArenaHUDWidget = CreateWidget<UUserWidget>(this, ArenaHUDWidgetClass);
	if (!ArenaHUDWidget)
	{
		return;
	}

	ArenaHUDWidget->AddToViewport(ArenaHUDZOrder);

	// 焦点交给竞技场 HUD 根控件：键盘 / 手柄导航要靠它落到按钮上。
	// 鼠标点击不依赖焦点（Slate 的命中测试是另一条路），所以这一步失败也不影响玩法。
	SetScreenFocusWidget(ArenaHUDWidget);
}

// ---------------------------------------------------------------------------
// 相位 → 输入模式
// ---------------------------------------------------------------------------

void AArenaPlayerController::PlayerTick(float DeltaTime)
{
	// 父类这里是空的（ALOLPlayerController 没有重写 PlayerTick），但照调不误：
	// 以后谁在父类里加了东西，这一行能保证不被静默跳过。
	Super::PlayerTick(DeltaTime);

	SyncInputModeToPhase();
	SyncObservedTarget();
}

// ---------------------------------------------------------------------------
// 目标框：对手
// ---------------------------------------------------------------------------

void AArenaPlayerController::SyncObservedTarget()
{
	// 英雄 HUD 的翻译层。非本地玩家（服务端上别人的 PC）没有它，这里自然什么都不做。
	UHeroHUDController* HeroHUD = GetHUDController();
	if (!HeroHUD)
	{
		return;
	}

	const UWorld* World = GetWorld();
	AArenaGameState* GameState = World ? World->GetGameState<AArenaGameState>() : nullptr;
	AArenaPlayerState* MyPlayerState = GetPlayerState<AArenaPlayerState>();

	// 不是竞技场关卡（GameState 类型对不上），或者状态还没复制过来 —— 两种都是"现在没得看"。
	if (!GameState || !MyPlayerState)
	{
		return;
	}

	// 对手是"另一个参赛者"。GetOpponentOf 在候选人表里找，找不到（还没排进来 / 已经出局）就是空，
	// 空会让目标框收起 —— 这比留着上一个人的血条正确得多。
	AArenaPlayerState* OpponentState = GameState->GetOpponentOf(MyPlayerState);
	APawn* OpponentPawn = OpponentState ? OpponentState->GetPawn() : nullptr;

	// 【幂等】目标没换就什么都不做（尤其别每帧调 SetObservedTarget：
	// 虽然它自己会短路成"只刷新不重绑"，但那是给"数值变了要补一帧"用的，不是给逐帧轮询用的）。
	// 这里拿控制器当前的观察目标当缓存，所以不需要在本类里再存一份（存了反而多一个会和控制器
	// 不同步的地方：控制器会在目标被销毁时自己清空，本类那份不会知道）。
	if (HeroHUD->GetObservedTarget() == OpponentPawn)
	{
		return;
	}

	HeroHUD->SetObservedTarget(OpponentPawn);
}

EArenaInputIntent AArenaPlayerController::PhaseInputIntent(EArenaPhase Phase, bool bRewardSelectionDone)
{
	switch (Phase)
	{
	case EArenaPhase::RewardSelection:
		// 备战：玩家在备战地图上【自由移动】，同时要用鼠标点三选一 / 锻造器菜单。
		// 游戏输入和 UI 输入必须同时活着 —— 切 UIOnly 会把人钉死在原地。
		//
		// 但【选完之后】就不用鼠标了：这一相位还要等备战倒计时跑完（以及等对手选完），
		// 那段时间光标留在屏幕中间只会挡视角，而且视角还锁不住（GameAndUI 的代价，
		// 见 ApplyInputMode 里那段）。所以选完就收回光标，走 GameOnly。
		// 下一回合 StartRound → ResetRewardSelection 会把 RewardStep 打回非 None，
		// 光标自己就回来了 —— 不需要谁手动还原。
		return bRewardSelectionDone ? EArenaInputIntent::GameOnly : EArenaInputIntent::GameAndUI;

	case EArenaPhase::MatchEnd:
		// 结果界面。大场已经结束，之后不会再回战斗相位，所以可以放心把输入一直交给界面。
		return EArenaInputIntent::UIOnly;

	case EArenaPhase::WaitingToStart:
		// 还没有比赛，但也没有界面。留在 GameOnly 让玩家能转头看看场地，
		// 切 UIOnly 的话会亮出一个谁也点不动、也不知道在等什么的光标。
		return EArenaInputIntent::GameOnly;

	case EArenaPhase::Combat:
		// 要打。
		return EArenaInputIntent::GameOnly;

	case EArenaPhase::Settlement:
		// 这一回合已经判完了，下一回合的备战是定时器排的。留在 GameOnly：
		// 输的那一方正在播死亡动画、赢的那一方可以走两步，都不影响结果
		// （伤害只在战斗相位里被计入，见 AArenaGameMode::PollCombatOutcome）。
		// 切 UIOnly 会在这几秒里闪一下光标，比不切更烦。
		return EArenaInputIntent::GameOnly;
	}

	return EArenaInputIntent::GameOnly;
}

void AArenaPlayerController::SyncInputModeToPhase()
{
	const UWorld* World = GetWorld();
	const AArenaGameState* GameState = World ? World->GetGameState<AArenaGameState>() : nullptr;

	if (!GameState)
	{
		// 不是竞技场关卡，或者 GameState 还没复制过来。
		// 【刻意不打日志】GameState 后到是正常的开局瞬间，而"这个 PC 被放在别的关卡里"
		// 也不该每帧刷一条。两种情况的表现都是"这个类什么都不做"，这本来就是对的。
		return;
	}

	// 选完奖励没有会翻转的 bool，所以上面那条"没有相位变化就什么都不做"的短路
	// 挡不住它 —— 这里显式把这个 bool 算进判断，它一变就重切一次输入模式。
	const AArenaPlayerState* MyPlayerState = GetPlayerState<AArenaPlayerState>();
	const bool bRewardSelectionDone = MyPlayerState && MyPlayerState->IsRewardSelectionDone();

	const EArenaInputIntent Want = PhaseInputIntent(GameState->GetArenaPhase(), bRewardSelectionDone);
	if (Want == CurrentIntent)
	{
		return;   // 没变。见头文件：绝不重复照调，计数器会被卡住
	}

	ApplyInputMode(Want);
}

void AArenaPlayerController::ApplyInputMode(EArenaInputIntent Intent)
{
	CurrentIntent = Intent;
	bInputModeTouched = true;

	// SetIgnoreMoveInput / SetIgnoreLookInput 只在离开 GameOnly 时兜一层，
	// 回 GameOnly 时必须成对解开 —— 它们是计数器，多加不解就永远动不了。
	const bool bLeaveGameOnly = Intent != EArenaInputIntent::GameOnly;

	switch (Intent)
	{
	case EArenaInputIntent::UIOnly:
	{
		// FInputModeUIOnly 是唯一能让"游戏收不到输入、widget 收得到"的模式：
		// 它内部做的是 SetFocusAndLocking + ReleaseMouseCapture +
		// GameViewportClient.SetIgnoreInput(true) + SetMouseCaptureMode(NoCapture)。
		FInputModeUIOnly Mode;

		if (UUserWidget* FocusWidget = ScreenFocusWidget.Get())
		{
			// 只有界面自己交过 widget 才有焦点。没有也不影响鼠标点击，
			// 影响的只是键盘 / 手柄导航的落点。
			Mode.SetWidgetToFocus(FocusWidget->TakeWidget());
		}
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);

		SetInputMode(Mode);
		SetShowMouseCursor(true);

		// 第二层兜底。UIOnly 已经把整条 viewport 输入路堵死了，
		// 但这两句能挡住"某个输入路径绕过 viewport"（比如直接读按键状态的代码）。
		SetIgnoreMoveInput(true);
		SetIgnoreLookInput(true);
		return;
	}

	case EArenaInputIntent::GameAndUI:
	{
		// 备战形态：游戏输入照常（走动），光标放出来点三选一。
		//
		// 【5.8 的 API 事实】FInputModeGameAndUI 只有三件可调的：
		// SetWidgetToFocus / SetLockMouseToViewportBehavior / SetHideCursorDuringCapture
		//（旧版的 SetMouseCaptureMode 已删）。点世界空白处会有一次"按下期间"的
		// 临时捕获 —— SetHideCursorDuringCapture(false) 保证光标不会因此消失，
		// 抬起即释放，不会出现"点一下地面三选一就点不动"的永久抓走。
		// 【代价】备战阶段鼠标不锁视角。备战区里不交战，这是可接受的取舍；
		// 开战瞬间切回 GameOnly，视角照旧。
		FInputModeGameAndUI Mode;

		if (UUserWidget* FocusWidget = ScreenFocusWidget.Get())
		{
			Mode.SetWidgetToFocus(FocusWidget->TakeWidget());
		}
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);

		SetInputMode(Mode);
		SetShowMouseCursor(true);

		// 兜底层保持【解开】：这个形态下走动是玩法的一部分。
		SetIgnoreMoveInput(false);
		SetIgnoreLookInput(false);
		return;
	}

	case EArenaInputIntent::GameOnly:
	default:
		SetIgnoreMoveInput(false);
		SetIgnoreLookInput(false);
		SetShowMouseCursor(false);
		SetInputMode(FInputModeGameOnly());
		return;
	}
}

void AArenaPlayerController::SetScreenFocusWidget(UUserWidget* InWidget)
{
	ScreenFocusWidget = InWidget;

	// 界面是在游戏跑起来之后才建的，所以设焦点这一下要立刻生效 ——
	// 只在 SyncInputModeToPhase 里用的话，等下一次相位变化就太晚了。
	// 【只在已经处于"界面参与"的形态时才动引擎】否则会把 GameOnly 的玩家
	// 莫名其妙地拽到 UI 模式。GameAndUI 和 UIOnly 用各自的 Mode 重建，
	// 保证焦点和光标行为与当前形态一致。
	if (!InWidget || !bInputModeTouched || CurrentIntent == EArenaInputIntent::GameOnly)
	{
		return;
	}

	if (CurrentIntent == EArenaInputIntent::GameAndUI)
	{
		FInputModeGameAndUI Mode;
		Mode.SetWidgetToFocus(InWidget->TakeWidget());
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		SetInputMode(Mode);
		return;
	}

	FInputModeUIOnly Mode;
	Mode.SetWidgetToFocus(InWidget->TakeWidget());
	Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(Mode);
}
