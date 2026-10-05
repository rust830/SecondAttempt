// 比分栏。设计意图全在头文件里。

#include "UI/ArenaMatchStatusWidget.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UI/ArenaHUDLayout.h"

void UArenaMatchStatusWidget::NativePreConstruct()
{
	Super::NativePreConstruct();

	// 留空就什么都不做 —— 这条路径的存在不应该改变"没配布局"时的行为。
	if (!LayoutConfig)
	{
		return;
	}

	const TArray<FName> Skipped = LayoutConfig->ApplyTo(this);

	// 跳过的要能看见。静默跳过的话，"我明明在 DataAsset 里配了 14 个控件，
	// 怎么只动了 6 个"会变成一个要翻源码才能回答的问题。
	// 常见的两种原因：名字拼错，或者那个控件的父容器不是 CanvasPanel。
	if (Skipped.Num() > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[ArenaHUD] 布局配置有 %d 个控件没生效（名字对不上，或父容器不是 CanvasPanel）：%s"),
			Skipped.Num(), *FString::JoinBy(Skipped, TEXT(", "), [](const FName& N) { return N.ToString(); }));
	}
}

void UArenaMatchStatusWidget::ApplyMatch(const FArenaMatchView& InMatch)
{
	// 内容没变就不重画。心跳每 0.25 秒推一次，去重已经在翻译层做过一遍，
	// 这里再来一道是因为 CountdownText 每秒都会变 —— 而这一道挡住的是
	// "同一秒内被推了四次"时的那三次无辜的 SetText。
	if (Match.EqualsForUI(InMatch))
	{
		return;
	}

	Match = InMatch;

	if (RoundText)
	{
		RoundText->SetText(Match.RoundText);
	}

	if (PhaseText)
	{
		PhaseText->SetText(Match.PhaseText);
	}

	if (CountdownText)
	{
		CountdownText->SetText(Match.CountdownText);
		// 没有倒计时的相位（战斗 / 等待对手）把数字收起来。显示一个 0 或者
		// 停在上一次的读数上，都会被当成"卡住了"。
		CountdownText->SetVisibility(Match.bHasCountdown
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	ApplyContender(Match.Self, SelfNameText, SelfHealthBar, SelfHealthText, SelfScoreText, SelfPanel);
	ApplyContender(Match.Opponent, OpponentNameText, OpponentHealthBar, OpponentHealthText,
		OpponentScoreText, OpponentPanel);

	if (ResultText)
	{
		ResultText->SetText(Match.ResultText);
		ResultText->SetVisibility(Match.bMatchEnded
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	if (LastRoundText)
	{
		LastRoundText->SetText(Match.LastRoundText);
		LastRoundText->SetVisibility(Match.LastRoundText.IsEmpty()
			? ESlateVisibility::Collapsed
			: ESlateVisibility::HitTestInvisible);
	}

	BP_OnMatchChanged(Match);
}

void UArenaMatchStatusWidget::ApplyContender(const FArenaContenderView& View, UTextBlock* NameWidget,
	UImage* BarWidget, UTextBlock* HealthWidget, UTextBlock* ScoreWidget, UWidget* RootWidget)
{
	// 这一栏没人（还在等人 / Bot 还没进来）就整栏收起 —— 不画一个 0/100 的空血条。
	// 整栏收起是刻意的：只收起血条的话，屏幕上会留着对手的名字和一个空槽。
	if (RootWidget)
	{
		// Collapsed 而不是 Hidden：Hidden 仍然占位，那一栏会在布局里留一块空白。
		//
		// SelfHitTestInvisible 而不是 Visible：这一栏没有任何交互，但它面积不小 ——
		// 用 Visible 的话它会吃掉鼠标事件，盖在下面的三选一按钮就点不动了
		//（UMG 的命中测试是先命中先得，和谁画在上面无关）。
		RootWidget->SetVisibility(View.bValid
			? ESlateVisibility::SelfHitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	if (NameWidget)
	{
		NameWidget->SetText(View.DisplayName);
	}

	if (BarWidget)
	{
		// 环形血条：百分比走材质的 "Percent" 标量参数（M_ArenaRingBar）。
		// GetDynamicMaterial 拿不到 MID（没挂材质的普通 Image）就是 nullptr —— no-op，
		// 和"没配贴图时回合线走染色兜底"是同一种宽容。
		if (UMaterialInstanceDynamic* MID = BarWidget->GetDynamicMaterial())
		{
			MID->SetScalarParameterValue(TEXT("Percent"), View.HealthPercent);
		}
	}

	if (HealthWidget)
	{
		HealthWidget->SetText(View.HealthText);
	}

	if (ScoreWidget)
	{
		ScoreWidget->SetText(View.ScoreText);
	}
}
