// 装备栏。设计意图全在头文件里。

#include "UI/ArenaLoadoutBarWidget.h"

#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"

#include "UI/ArenaLoadoutEntryWidget.h"

#define LOCTEXT_NAMESPACE "ArenaLoadoutBar"

void UArenaLoadoutBarWidget::ApplyLoadout(const FArenaLoadoutView& InLoadout)
{
	// 内容没变就不重画（理由同 UArenaMatchStatusWidget::ApplyMatch）。
	if (Loadout.EqualsForUI(InLoadout))
	{
		return;
	}

	Loadout = InLoadout;

	// 装备和海克斯在同一个数组里（见 FArenaLoadoutView 的注释），所以这里只有一个循环。
	EnsureEntryCount(Loadout.Entries.Num());

	for (int32 Index = 0; Index < Entries.Num() && Loadout.Entries.IsValidIndex(Index); ++Index)
	{
		if (UArenaLoadoutEntryWidget* Entry = Entries[Index])
		{
			Entry->ApplyEntry(Loadout.Entries[Index]);
		}
	}

	if (ForgeChargesText)
	{
		if (Loadout.ForgeCharges.Num() > 0)
		{
			// 拼成一整行（"传说锻造器 ×3　棱彩锻造器 ×1"）而不是每档一个控件：
			// 档位数量是运行期变的，一行文本不用管布局，也不会在花完一档时
			// 让另一档的位置跳一下。
			FString Joined;
			for (const FArenaForgeChargeView& Charge : Loadout.ForgeCharges)
			{
				if (!Joined.IsEmpty())
				{
					// 全角空格分隔，中文排版下比普通空格自然。
					Joined.Append(TEXT("　"));
				}
				Joined.Append(FText::Format(
					LOCTEXT("ArenaForgeChargeLineFmt", "{0} ×{1}"),
					Charge.DisplayName,
					FText::AsNumber(Charge.Count)).ToString());
			}

			ForgeChargesText->SetText(FText::FromString(Joined));
			ForgeChargesText->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			// 一次都没攒到（或者全花完了）就整行收起，不留一个空标签。
			ForgeChargesText->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	BP_OnLoadoutChanged(Loadout);
}

void UArenaLoadoutBarWidget::EnsureEntryCount(int32 DesiredCount)
{
	if (Entries.Num() == DesiredCount)
	{
		return;
	}

	if (!EntryContainer)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 上没有名为 EntryContainer 的容器控件，装备栏画不出任何格子。")
			TEXT("请在 WBP 里放一个 PanelWidget 并命名成 EntryContainer。"),
			*GetName());
		return;
	}

	if (!EntryWidgetClass)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 没配 EntryWidgetClass，装备栏画不出任何格子。")
			TEXT("请在 WBP 的 Details 面板里把 EntryWidgetClass 指向 WBP_ArenaLoadoutEntry。"),
			*GetName());
		return;
	}

	EntryContainer->ClearChildren();
	Entries.Reset();
	Entries.Reserve(DesiredCount);

	for (int32 Index = 0; Index < DesiredCount; ++Index)
	{
		UArenaLoadoutEntryWidget* Entry = CreateWidget<UArenaLoadoutEntryWidget>(GetOwningPlayer(), EntryWidgetClass);
		if (!Entry)
		{
			continue;
		}

		EntryContainer->AddChild(Entry);
		Entries.Add(Entry);
	}

	if (Entries.Num() != DesiredCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 要建 %d 个装备格，实际只建出 %d 个（EntryWidgetClass 是不是配错了？）。"),
			*GetName(), DesiredCount, Entries.Num());
	}
}

#undef LOCTEXT_NAMESPACE
