// 公告层的实现。设计意图全在头文件里。

#include "UI/ArenaAnnounceWidget.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "TimerManager.h"

#define LOCTEXT_NAMESPACE "ArenaAnnounce"

UArenaAnnounceWidget::UArenaAnnounceWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 官方贴图（导入脚本生成的路径）。⚠️ 资产路径不带 .png —— 带扩展名必然解析失败。
	// 没导入时解析失败，Image 留空不报错。
	VsTextTexture = TSoftObjectPtr<UTexture2D>(
		FSoftObjectPath(TEXT("/Game/LOL/UI/Arena/Textures/Cherry/MatchupIntro/cherry_hud_vs_text")));
	ResultFrameTexture = TSoftObjectPtr<UTexture2D>(
		FSoftObjectPath(TEXT("/Game/LOL/UI/Arena/Textures/Cherry/Elimination/elimination_background_frame")));
}

void UArenaAnnounceWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// VS 艺术字 / 结果底框只在拿到贴图时才铺，缺失时这块就是空的。
	// （两块贴图整场不变，解析一次挂缓存就够。）
	if (VsImage && !VsTextTexture.IsNull())
	{
		CachedVsTexture = VsTextTexture.Get() ? VsTextTexture.Get() : VsTextTexture.LoadSynchronous();
		if (CachedVsTexture)
		{
			VsImage->SetBrushFromTexture(CachedVsTexture);
		}
	}

	if (ResultFrameImage && !ResultFrameTexture.IsNull())
	{
		CachedResultFrame = ResultFrameTexture.Get() ? ResultFrameTexture.Get() : ResultFrameTexture.LoadSynchronous();
		if (CachedResultFrame)
		{
			ResultFrameImage->SetBrushFromTexture(CachedResultFrame);
		}
	}

	// 公告层开局必然是 None：第一个"有内容"的时刻是第一场战斗开始。
	ApplyMode(EArenaAnnounceMode::None);
}

void UArenaAnnounceWidget::NativeDestruct()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(VsIntroExpireHandle);
	}

	Super::NativeDestruct();
}

void UArenaAnnounceWidget::PlayVsIntro(const FArenaMatchView& Match)
{
	if (VsSelfText)
	{
		VsSelfText->SetText(Match.Self.DisplayName);
	}
	if (VsOpponentText)
	{
		// 对手还没就位（名字为空）时画个占位：VS 屏上少一个名字看起来像 bug。
		VsOpponentText->SetText(Match.Opponent.bValid
			? Match.Opponent.DisplayName
			: LOCTEXT("VsUnknownOpponent", "???"));
	}

	ApplyMode(EArenaAnnounceMode::VsIntro);

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	World->GetTimerManager().ClearTimer(VsIntroExpireHandle);
	if (VsIntroSeconds > 0.f)
	{
		World->GetTimerManager().SetTimer(VsIntroExpireHandle, this,
			&UArenaAnnounceWidget::HandleVsIntroExpire, VsIntroSeconds, /*bLoop=*/false);
	}
}

void UArenaAnnounceWidget::PlayResult(const FArenaMatchView& Match)
{
	if (!Match.bMatchEnded)
	{
		// 还没打完就被调 = 调用方把"内容变化"当成了"相位边沿"。
		// 不动画面 —— 结果屏上出现"你赢了"然后比赛继续，是整层最大的事故。
		return;
	}

	if (ResultText)
	{
		ResultText->SetText(Match.ResultText);
	}

	// 结果横幅不自动收：MatchEnd 之后没有下一幕，收起它反而像"什么都没发生"。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(VsIntroExpireHandle);
	}

	ApplyMode(EArenaAnnounceMode::Result);
}

void UArenaAnnounceWidget::Dismiss()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(VsIntroExpireHandle);
	}

	ApplyMode(EArenaAnnounceMode::None);
}

void UArenaAnnounceWidget::ApplyMode(EArenaAnnounceMode NewMode)
{
	Mode = NewMode;

	const bool bShowRoot = Mode != EArenaAnnounceMode::None;
	if (AnnounceRoot)
	{
		AnnounceRoot->SetVisibility(bShowRoot ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}

	if (VsPanel)
	{
		VsPanel->SetVisibility(Mode == EArenaAnnounceMode::VsIntro
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	if (ResultPanel)
	{
		ResultPanel->SetVisibility(Mode == EArenaAnnounceMode::Result
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	BP_OnAnnounceChanged(Mode);
}

void UArenaAnnounceWidget::HandleVsIntroExpire()
{
	// 只收 VS 这一幕：如果结果横幅已经接棒（极端时序下结算快于 VS 收场），
	// 不能把别人的幕也撤了。
	if (Mode == EArenaAnnounceMode::VsIntro)
	{
		ApplyMode(EArenaAnnounceMode::None);
	}
}

#undef LOCTEXT_NAMESPACE
