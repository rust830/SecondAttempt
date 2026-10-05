// 竞技场 Loading 界面。设计意图全在头文件里。

#include "UI/ArenaLoadingWidget.h"

#include "Components/TextBlock.h"

void UArenaLoadingWidget::ApplyMatch(const FArenaMatchView& InMatch)
{
	ApplyContender(InMatch.Self, SelfCard, SelfNameText);
	ApplyContender(InMatch.Opponent, OpponentCard, OpponentNameText);

	if (WaitingText)
	{
		// 对手没就位（或者比赛已经打完）时才说话；双方都到位了这张卡就有内容了。
		const bool bWaiting = !InMatch.Opponent.bValid && !InMatch.bMatchEnded;
		WaitingText->SetVisibility(bWaiting
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	// 显隐规则（见头文件）：第 1 回合还没开始 → 显示；开打 / 打完 → 收起。
	// bMatchEnded 单独判一次：万一有人把回合表配出 0 回合的战斗，别把结算画面盖住。
	const bool bShouldShow = InMatch.RoundNumber == 0 && !InMatch.bMatchEnded;
	SetVisibility(bShouldShow
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);

	BP_OnLoadingChanged(InMatch);
}

void UArenaLoadingWidget::ApplyContender(
	const FArenaContenderView& Contender,
	UWidget* Card,
	UTextBlock* NameText)
{
	if (Card)
	{
		// 位子还空着 → 整卡收起（同"不画空血条"的纪律）。
		Card->SetVisibility(Contender.bValid
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	if (NameText)
	{
		NameText->SetText(Contender.bValid ? Contender.DisplayName : FText::GetEmpty());
	}
}
