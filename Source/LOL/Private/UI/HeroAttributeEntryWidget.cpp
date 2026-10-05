// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroAttributeEntryWidget.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"

void UHeroAttributeEntryWidget::ApplyEntry(const FHeroAttributeEntryView& View)
{
	// 事件只在【真的变了】的时候播，但控件每次照写（写一遍是幂等的、几乎不花钱）。
	// 面板每次重算都会把所有行推一遍，如果无条件播事件，一次属性变化会让十几行各播一次 —— 而其中只有一行真的变了。
	const bool bChanged = !EntryView.EqualsForUI(View);

	EntryView = View;

	// 图标：留空 = 这一行不画图标（收起 IconImage），数值照样显示。
	if (IconImage)
	{
		if (!bAuthoredIconVisibilityCaptured)
		{
			// 只记一次：之后每次 SetBrushFromTexture 都不会改可见性，所以这份值一直是 WBP 里那个。
			AuthoredIconVisibility = IconImage->GetVisibility();
			bAuthoredIconVisibilityCaptured = true;
		}

		if (View.Icon)
		{
			IconImage->SetBrushFromTexture(View.Icon, /*bMatchSize=*/false);
			IconImage->SetVisibility(AuthoredIconVisibility);
		}
		else
		{
			IconImage->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	if (ValueText)
	{
		ValueText->SetText(View.ValueText);
	}

	if (NameText)
	{
		NameText->SetText(View.DisplayName);
	}

	// 形态事件和这条【分开】：数值每变一次就重播一次形态动画的话，
	// 「常驻态是高亮、展开态是灰的」这类表现会在每次属性变化时闪一下。
	if (bChanged)
	{
		BP_OnEntryChanged(EntryView);
	}
}

void UHeroAttributeEntryWidget::SetCompact(bool bInCompact)
{
	if (bCompact == bInCompact)
	{
		return;
	}

	bCompact = bInCompact;

	// 只在【真的翻转】时播：面板每次 ApplyPanelView 都会把形态重算一遍，
	// 无条件播的话展开态下每来一次属性变化，所有行都会重播一遍「变成展开态」。
	BP_OnCompactChanged(bCompact);
}
