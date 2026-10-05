// 装备栏里的一格。设计意图全在头文件里。

#include "UI/ArenaLoadoutEntryWidget.h"

#include "Components/Image.h"
#include "Components/TextBlock.h"

void UArenaLoadoutEntryWidget::ApplyEntry(const FArenaLoadoutEntryView& InEntry)
{
	// 内容没变就不重画（理由同 UArenaChoiceCardWidget::ApplyCard）。
	if (Entry.EqualsForUI(InEntry))
	{
		return;
	}

	Entry = InEntry;

	if (NameText)
	{
		NameText->SetText(Entry.DisplayName);
	}

	if (DescriptionText)
	{
		DescriptionText->SetText(Entry.Description);
	}

	if (IconImage)
	{
		IconImage->SetBrushFromTexture(Entry.Icon, /*bMatchSize=*/false);
		IconImage->SetVisibility(Entry.Icon
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	if (AugmentMark)
	{
		// 海克斯角标。HitTestInvisible 而不是 Visible：这一格整体不该吃鼠标事件
		//（它没有任何交互），角标也不该开一个例外。
		AugmentMark->SetVisibility(Entry.bIsAugment
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);

		// 品质配色：角标染成对应档位的颜色（WBP 里的角标本体给一张白色图形，
		// 颜色完全由这里定 —— 换品质配色不用动资产）。
		if (UImage* MarkImage = Cast<UImage>(AugmentMark))
		{
			FLinearColor Tint = FLinearColor::White;
			switch (Entry.Tier)
			{
			case EArenaCardTier::Silver:
				Tint = FLinearColor(0.62f, 0.68f, 0.76f);
				break;
			case EArenaCardTier::Gold:
				Tint = FLinearColor(0.96f, 0.72f, 0.22f);
				break;
			case EArenaCardTier::Prismatic:
				Tint = FLinearColor(0.55f, 0.62f, 0.95f);
				break;
			case EArenaCardTier::None:
			default:
				break;
			}
			MarkImage->SetColorAndOpacity(Tint);
		}
	}

	BP_OnEntryChanged(Entry);
}
