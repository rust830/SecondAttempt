// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroAttributePanelWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"

#include "UI/HeroAttributeEntryWidget.h"

// ===========================================================================
// 形态
// ===========================================================================

void UHeroAttributePanelWidget::ToggleExpanded()
{
	SetExpanded(!bExpanded);
}

void UHeroAttributePanelWidget::SetExpanded(bool bInExpanded)
{
	if (bExpanded == bInExpanded)
	{
		return;
	}

	bExpanded = bInExpanded;

	// 形态变了要重排行的可见性（收起态只留常驻那几条）+ 把每行的形态也换掉。
	ApplyRowVisibility();

	BP_OnExpandedChanged(bExpanded);

	// C++ 侧也广播一条：HUD 拿它去同步属性区背景框的高度（收起 96 / 展开 168）。
	// 放在 BP 事件之后，顺序上让蓝图先做自己的表现。
	OnExpandedChanged.Broadcast(bExpanded);
}

// ===========================================================================
// 应用视图
// ===========================================================================

void UHeroAttributePanelWidget::ApplyPanelView(const FHeroAttributePanelView& View)
{
	PanelView = View;

	if (!bHasAppliedView)
	{
		bHasAppliedView = true;

		// 第一次拿视图时采用 WBP 里配的初始形态，且【不播】BP_OnExpandedChanged ——
		// 从「默认值」到「真实形态」不是一次翻转，播了会在开局闪一下
		//（和技能槽的 bHasAppliedView 是同一个理由）。
		bExpanded = bStartExpanded;
	}

	// 行：懒建 + 逐个填。下标 = 条目下标，中间不会有空洞（条目是连续的）。
	//
	// 【bShowInCompact 改了不用重建行】分组已经不进容器了 —— 它只影响发号顺序
	//（见 ReflowRows 的两趟发号），所以配置改完下次刷新会自己归位。
	for (int32 Index = 0; Index < View.Entries.Num(); ++Index)
	{
		if (UHeroAttributeEntryWidget* Row = EnsureEntryWidget(Index))
		{
			Row->ApplyEntry(View.Entries[Index]);
		}
	}

	// 数据填完再排行：bValid 和 bShowInCompact 都要等视图进来才知道。
	ApplyRowVisibility();

	BP_OnPanelChanged(PanelView);
}

void UHeroAttributePanelWidget::ApplyRowVisibility()
{
	// 展开态才显示全部；未就绪（bValid=false）时整块收起 —— 不显示一排 0。
	// 注意 bExpanded 是【用户意图】，bValid 是【数据可用性】：意图要留着，
	// 所以未就绪时不把 bExpanded 改掉，等属性集绑上来自动回到用户选的那个形态。
	const bool bShowAll = bExpanded && PanelView.bValid;

	// 行形态【默认不随展开变化】：展开只多出一块行，所有行保持一样宽。
	// 想回到"展开时所有行一起变宽"（显示属性名那种）就把 bUseRelaxedRowsWhenExpanded 打开。
	const bool bCompactRows = !(bShowAll && bUseRelaxedRowsWhenExpanded);

	for (int32 Index = 0; Index < EntryWidgets.Num(); ++Index)
	{
		UHeroAttributeEntryWidget* Row = EntryWidgets[Index];
		if (!Row)
		{
			continue;
		}

		// 配置里删过行的话，多出来的旧行会留在 EntryWidgets 里 —— 一并收起来，
		// 否则改完 DA 重启 PIE 会看到几行永远不更新的幽灵行。
		const bool bHasEntry = PanelView.Entries.IsValidIndex(Index);

		// 【可见性规则还是老一条，分组由"行在哪个容器里"承担】
		//   常驻行（bShowInCompact）：常驻态就该看得见 —— 它就是常驻条；
		//   展开行：              只有展开态才出现，收起时 Collapsed，不画也不占位。
		// 两个容器的行各自被这条规则筛出可见集合，所以收起态天然只剩常驻条、展开态
		// 上块才出现 —— 不需要额外去折叠/展开容器的可见性（Collapsed 的孩子在
		// 流式容器里不占位，在 UniformGridPanel 里由 ReflowRows 挤到 (0,0) 也不占格）。
		const bool bVisible = PanelView.bValid
			&& bHasEntry
			&& (bShowAll || PanelView.Entries[Index].bShowInCompact);

		// 可见性由【面板】写：这是布局决策（哪些行现在占屏幕上的位置），不是行的外观。
		// 行的外观（常驻态长什么样）走 SetCompact → BP_OnCompactChanged，由 WBP 自己决定。
		Row->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);

		Row->SetCompact(bCompactRows);
	}

	// 可见性定完才能发行/列号（它读的就是上面刚设的 Visibility）。
	ReflowRows();
}

void UHeroAttributePanelWidget::ReflowRows()
{
	if (EntryGridSlots.Num() == 0)
	{
		return;
	}

	// 【第一趟：把收起的那几行挤到 (0,0)】
	//
	// 留在原位是不行的：行/列号是显式定位，收起只是不画出来，格子照样被它占着。
	// SUniformGridPanel 只会挤掉「整行/整列都没有可见孩子」的行列
	//（ComputeDesiredSize 里的 CollapsedRows / CollapsedColumns），
	// 而常驻那几条在 DA 里的下标是散点 —— 一个可见行和一堆收起行共处一行时，
	// 那一行不算空行，空洞就留在面板上了。
	//
	// 挤到 (0,0) 是安全的：Collapsed 的孩子不参与单元格尺寸计算
	//（同一个函数里 continue 掉了），所以它不会把 (0,0) 那格撑大，
	// 也不会和同样落在 (0,0) 的可见行打架 —— 可见的那个才是唯一画出来的。
	for (int32 Index = 0; Index < EntryGridSlots.Num(); ++Index)
	{
		UUniformGridSlot* GridSlot = EntryGridSlots[Index].Get();
		if (!GridSlot)
		{
			continue;
		}

		const bool bRowVisible = EntryWidgets.IsValidIndex(Index)
			&& EntryWidgets[Index]
			&& EntryWidgets[Index]->GetVisibility() != ESlateVisibility::Collapsed;

		if (!bRowVisible)
		{
			GridSlot->SetRow(0);
			GridSlot->SetColumn(0);
		}
	}

	// 【第二趟：给可见行发号，两趟 —— "展开的那些"排在常驻条之前】
	//
	// 网格是显式定位的，所以"谁在上面"完全由发号顺序决定：先给「只在展开态才出现的那些」
	//（bShowInCompact == false）发号，再给常驻那几条发号 —— 于是展开行占住上面的行、
	// 常驻条被推到下面，看起来就是"在常驻条上方多出一块"。
	//
	// 不能拿条目下标单趟发号：常驻那几条在 DA 里的下标是 0/1/3/5… 这种散点，
	// 单趟发号会让展开行插在它们中间。
	//
	// 【收起态是空跑的】第一趟那一组全是 Collapsed，会整个跳过 —— 所以发号结果和
	// 加这个功能之前完全一致，不会动到已有的表现。
	const int32 Columns = FMath::Max(1, NumColumns);
	int32 VisibleOrdinal = 0;

	for (int32 Group = 0; Group < 2; ++Group)
	{
		const bool bResidentGroup = (Group == 1);

		for (int32 Index = 0; Index < EntryWidgets.Num(); ++Index)
		{
			UHeroAttributeEntryWidget* Row = EntryWidgets[Index];
			UUniformGridSlot* GridSlot = EntryGridSlots.IsValidIndex(Index) ? EntryGridSlots[Index].Get() : nullptr;
			if (!Row || !GridSlot || Row->GetVisibility() == ESlateVisibility::Collapsed)
			{
				continue;
			}

			// 分组依据和 ApplyRowVisibility 用的是同一个字段，两边不会分叉。
			const bool bResident = PanelView.Entries.IsValidIndex(Index)
				&& PanelView.Entries[Index].bShowInCompact;
			if (bResident != bResidentGroup)
			{
				continue;
			}

			// 只有可见行吃号：这样常驻 8 条就是干净的 4 行 × 2 列。
			GridSlot->SetRow(VisibleOrdinal / Columns);
			GridSlot->SetColumn(VisibleOrdinal % Columns);
			++VisibleOrdinal;
		}
	}
}

// ===========================================================================
// 建行
// ===========================================================================

UHeroAttributeEntryWidget* UHeroAttributePanelWidget::EnsureEntryWidget(int32 EntryIndex)
{
	if (EntryIndex == INDEX_NONE)
	{
		return nullptr;
	}

	if (EntryWidgets.IsValidIndex(EntryIndex) && EntryWidgets[EntryIndex])
	{
		return EntryWidgets[EntryIndex];
	}

	if (!EntryContainer || !EntryWidgetClass)
	{
		return nullptr;
	}

	UHeroAttributeEntryWidget* NewWidget = CreateWidget<UHeroAttributeEntryWidget>(GetOwningPlayer(), EntryWidgetClass);
	if (!NewWidget)
	{
		return nullptr;
	}

	// ---------------------------------------------------------------------
	// 尺寸：配了 EntrySize 就包一层 SizeBox，然后按容器的种类挂上去。
	//
	// 和 UHeroHUDWidget::EnsureSlotWidget 里那段【完全同源】—— 为什么不能直接 AddChild：
	// 容器的槽位规则是 Automatic（用子控件自报的尺寸），而 WBP_AttributeEntry 的根如果是
	// CanvasPanel 这类控件，自报尺寸是 0 —— 表现是所有行挤成一条线（宽度为 0）。
	// 详见 EntrySize 的注释和 GAS_HUD_Setup.md §12.5。
	// ---------------------------------------------------------------------
	UWidget* WidgetToAdd = NewWidget;

	if (!EntrySize.IsNearlyZero() && WidgetTree)
	{
		USizeBox* SizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		if (SizeBox)
		{
			// 分维判断：只想约束宽度时另一维传 0 即可，不会被写成 0 高。
			if (EntrySize.X > 0.f)
			{
				SizeBox->SetWidthOverride(EntrySize.X);
			}
			if (EntrySize.Y > 0.f)
			{
				SizeBox->SetHeightOverride(EntrySize.Y);
			}
			SizeBox->AddChild(NewWidget);
			WidgetToAdd = SizeBox;
		}
	}

	if (UVerticalBox* Box = Cast<UVerticalBox>(EntryContainer))
	{
		// 竖向排列是属性面板的默认形态：上下各留一半间距，相邻两行之间正好是 EntrySpacing。
		// 只给一边的话整列会朝另一边偏半个间距，看起来像容器没对齐，而不像「有间距」。
		if (UVerticalBoxSlot* BoxSlot = Box->AddChildToVerticalBox(WidgetToAdd))
		{
			BoxSlot->SetSize(ESlateSizeRule::Automatic);
			BoxSlot->SetHorizontalAlignment(HAlign_Fill);

			const float HalfSpacing = FMath::Max(EntrySpacing, 0.f) * 0.5f;
			if (HalfSpacing > 0.f)
			{
				BoxSlot->SetPadding(FMargin(0.f, HalfSpacing));
			}
		}
	}
	else if (UUniformGridPanel* Grid = Cast<UUniformGridPanel>(EntryContainer))
	{
		// 【这里必须走 AddChildToUniformGrid，不能走 AddChild】
		// AddChild 也会建出一个 UUniformGridSlot（UPanelWidget 用 GetSlotClass 建的），
		// 但它的 Row / Column 是 0 —— 那两个 int32 在头文件里没有默认值，UObject 零初始化，
		// 于是所有行都叠在 (0,0) 那一格。真正的行列号由 ReflowRows() 按【各容器自己的可见顺序】发，
		// 这里先落在 (0,0) 就行。
		UUniformGridSlot* GridSlot = Grid->AddChildToUniformGrid(WidgetToAdd, /*Row=*/0, /*Column=*/0);

		if (GridSlot)
		{
			// 格子等宽等高（UniformGrid 的单元格尺寸取所有孩子的最大期望尺寸），
			// Fill 让每行铺满格子 —— 两列的宽度因此天然对齐。
			GridSlot->SetHorizontalAlignment(HAlign_Fill);
			GridSlot->SetVerticalAlignment(VAlign_Fill);
		}

		// 间距走【容器级】的 SlotPadding：UniformGrid 没有 per-slot padding。
		// 给一半 —— 每格四周各留半格，相邻两格之间正好是 EntrySpacing
		//（同 VerticalBox 分支的算法，只给一边的话整块会朝另一边偏）。
		const float HalfSpacing = FMath::Max(EntrySpacing, 0.f) * 0.5f;
		if (HalfSpacing > 0.f)
		{
			Grid->SetSlotPadding(FMargin(HalfSpacing));
		}

		// 用 SetNum 而不是 Add：条目号必须等于数组下标（同 EntryWidgets）。
		// 越界的行会被跳过，所以和 EntryWidgets 长度不一致也不会错位。
		if (!EntryGridSlots.IsValidIndex(EntryIndex))
		{
			EntryGridSlots.SetNum(EntryIndex + 1);
		}
		EntryGridSlots[EntryIndex] = GridSlot;   // 可能是 null，ReflowRows 会跳过
	}
	else
	{
		// 其他容器（WrapBox / HorizontalBox…）走通用路径：
		// 槽位规则由各自的容器决定，C++ 不替它们猜（同 EnsureSlotWidget 的 else 分支）。
		EntryContainer->AddChild(WidgetToAdd);
	}

	// 用 SetNum 而不是 Add：条目号必须等于数组下标。
	// 一旦用 Add，中间的空位就会把下标整体左移，「按条目下标找行」全部错位。
	if (!EntryWidgets.IsValidIndex(EntryIndex))
	{
		EntryWidgets.SetNum(EntryIndex + 1);
	}
	EntryWidgets[EntryIndex] = NewWidget;

	// Grid 槽只有 UniformGridPanel 分支会填；容器不是网格时这个数组一直是空的，
	// ReflowRows 会提前返回（流式容器不需要行/列号）。
	return NewWidget;
}
