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

	if (IconImage && View.Icon)
	{
		// 只在有图标时设置：Icon 为空（还没绑上 ASC / 配置没填）时保留 WBP 里的占位图，
		// 不然加载期会先闪一下空白。
		IconImage->SetBrushFromTexture(View.Icon, /*bMatchSize=*/false);
	}

	if (KeyLabelText && !View.KeyLabel.IsEmpty())
	{
		KeyLabelText->SetText(View.KeyLabel);
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
