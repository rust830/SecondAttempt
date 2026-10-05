// 布局配置的应用逻辑。设计意图全在头文件里。

#include "UI/ArenaHUDLayout.h"

#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"

TArray<FName> UArenaHUDLayoutConfig::ApplyTo(UUserWidget* Widget) const
{
	TArray<FName> Skipped;

	if (!Widget)
	{
		return Skipped;
	}

	for (const FArenaWidgetLayout& Layout : Widgets)
	{
		if (Layout.WidgetName.IsNone())
		{
			continue;
		}

		// 用 GetWidgetFromName 而不是 BindWidget 的指针：这一层只认名字，
		// 于是"这个 WBP 到底有没有那个控件"是一个可以被单独回答的问题 ——
		// 而且 NativePreConstruct 里跑也没问题（WidgetTree 在 Initialize 就建好了）。
		UWidget* Target = Widget->GetWidgetFromName(Layout.WidgetName);
		if (!Target)
		{
			Skipped.Add(Layout.WidgetName);
			continue;
		}

		// 只有 CanvasPanel 的子控件才能这样摆。别的容器（VerticalBox / SizeBox…）
		// 自己管孩子的排布，硬塞坐标没有意义 —— 记下来交给调用方。
		UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Target->Slot);
		if (!CanvasSlot)
		{
			Skipped.Add(Layout.WidgetName);
			continue;
		}

		CanvasSlot->SetAnchors(FAnchors(Layout.AnchorMin.X, Layout.AnchorMin.Y,
			Layout.AnchorMax.X, Layout.AnchorMax.Y));
		CanvasSlot->SetAlignment(Layout.Alignment);
		CanvasSlot->SetPosition(Layout.Position);

		if (Layout.bApplySize)
		{
			CanvasSlot->SetSize(Layout.Size);
		}
	}

	return Skipped;
}
