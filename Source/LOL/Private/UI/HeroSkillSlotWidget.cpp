// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroSkillSlotWidget.h"

#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"

void UHeroSkillSlotWidget::SetSlotIndex(int32 InSlotIndex)
{
	SlotIndex = InSlotIndex;
}

void UHeroSkillSlotWidget::ApplySlotView(const FSkillSlotView& View)
{
	SlotView = View;

	// ---------------------------------------------------------------------
	// 可见性：预留位（被动 / 格挡，配了 bHideWhenUnavailable 而能力还没授予）整格收起。
	//
	// 只在【第一次】记下 WBP 里配的可见性，恢复时回到它，而不是硬写 Visible ——
	// 否则会把美术在 WBP 里设的 HitTestInvisible 之类冲掉。
	//
	// 判据是 Controller 算好的 View.bHidden，这里不做第二次判断（同 ResolveSlotState 的理由）。
	// ---------------------------------------------------------------------
	if (!bAuthoredVisibilityCaptured)
	{
		AuthoredVisibility = GetVisibility();
		bAuthoredVisibilityCaptured = true;
	}
	SetVisibility(View.bHidden ? ESlateVisibility::Collapsed : AuthoredVisibility);

	if (IconImage && View.Icon)
	{
		// 只在有图标时设置：Icon 为空（还没绑上 ASC / 配置没填）时保留 WBP 里的占位图，
		// 不然加载期会先闪一下空白。
		IconImage->SetBrushFromTexture(View.Icon, /*bMatchSize=*/false);
	}

	if (KeyLabelText)
	{
		// 被动【按不出来】，所以不画键位：不管 WBP 模板里留了什么占位文字，一律清掉。
		// 主动 / 格挡走原来的规则 —— 只在 KeyLabel 非空时写，空的时候保留模板占位，
		// 免得到配置加载完之前先闪一下空白（见 IconImage 那条同款注释）。
		if (View.Kind == EHeroHUDSlotKind::Passive)
		{
			KeyLabelText->SetText(FText::GetEmpty());
		}
		else if (!View.KeyLabel.IsEmpty())
		{
			KeyLabelText->SetText(View.KeyLabel);
		}
	}

	// ---------------------------------------------------------------------
	// 转圈：判断条件是 ShouldShowCooldown()，不是 State == Cooled。
	//
	// 两者不等价，而且差别正好是最容易漏的那个：死亡时 State 是 Greyed，
	// 但冷却数字和转圈都要照常走（死亡和冷却是两条独立通道）。
	// ---------------------------------------------------------------------
	const bool bShowCooldown = View.ShouldShowCooldown();

	if (CooldownRing)
	{
		CooldownRing->SetVisibility(bShowCooldown ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowCooldown)
		{
			CooldownRing->SetPercent(View.CooldownPercent);
		}
	}

	if (CooldownText)
	{
		CooldownText->SetText(
			bShowCooldown && bShowCooldownText ? HeroHUD::FormatCooldownSeconds(View.CooldownRemaining) : FText::GetEmpty());
	}

	const bool bBlocked = View.State == ESkillSlotState::Greyed || View.State == ESkillSlotState::Disabled;

	if (GreyedOverlay)
	{
		GreyedOverlay->SetVisibility(bBlocked ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}

	if (IconImage)
	{
		IconImage->SetColorAndOpacity(bBlocked ? BlockedIconTint : ReadyIconTint);
	}

	BP_OnSlotViewChanged(View);
}
