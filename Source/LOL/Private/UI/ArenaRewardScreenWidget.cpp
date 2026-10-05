// 奖励选择界面。设计意图全在头文件里。

#include "UI/ArenaRewardScreenWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"

#include "UI/ArenaChoiceCardWidget.h"

void UArenaRewardScreenWidget::NativeConstruct()
{
	Super::NativeConstruct();

	if (IsDesignTime())
	{
		return;
	}

	// 一开始必须收起：这个界面在"还没轮到我选"的时候也在屏幕外等着，
	// 而 WBP 里的默认可见性通常是 Visible —— 不主动关掉的话，开局第一帧
	// 会闪一个空壳三选一。第一次 ApplyPrompt 会把它调回正确的状态。
	SetVisibility(ESlateVisibility::Collapsed);

	if (HideShowButton)
	{
		HideShowButton->OnClicked.AddDynamic(this, &UArenaRewardScreenWidget::HandleHideShowClicked);
	}

	// 按钮文本只在 Toggle 时更新，WBP 里的默认文本是空的 —— 第一次进 PIE
	// 会看到一个没有字的按钮。这里按初始状态（内容展开）补上。
	if (HideShowButtonText)
	{
		HideShowButtonText->SetText(NSLOCTEXT("ArenaHUD", "RewardHide", "隐藏选择"));
	}
}

void UArenaRewardScreenWidget::NativeDestruct()
{
	// 卡片是 CreateWidget 出来的，跟着本 Widget 一起没。这里显式清一遍，
	// 是因为 Cards 里那些卡片的 OnCardClicked 绑在【这个对象】上 ——
	// 清掉之后即使有个别卡片被别处留着，回调也打不到一个正在析构的对象上。
	for (UArenaChoiceCardWidget* Card : Cards)
	{
		if (Card)
		{
			Card->OnCardClicked.RemoveDynamic(this, &UArenaRewardScreenWidget::HandleCardClicked);
			Card->OnRerollClicked.RemoveDynamic(this, &UArenaRewardScreenWidget::HandleCardRerolled);
		}
	}
	Cards.Reset();

	if (HideShowButton)
	{
		HideShowButton->OnClicked.RemoveDynamic(this, &UArenaRewardScreenWidget::HandleHideShowClicked);
	}

	Super::NativeDestruct();
}

void UArenaRewardScreenWidget::HandleCardClicked(int32 OptionIndex)
{
	// 原样转发。不判合法性（服务端会判）、不记日志（翻译层和 GameMode 都会记）。
	OnOptionChosen.Broadcast(OptionIndex);
}

void UArenaRewardScreenWidget::HandleCardRerolled(int32 OptionIndex)
{
	// 同 HandleCardClicked：原样转发，服务端判合法性（可不可重随、还有没有次数）。
	OnRerollRequested.Broadcast(OptionIndex);
}

void UArenaRewardScreenWidget::HandleHideShowClicked()
{
	ToggleContentVisibility();
}

void UArenaRewardScreenWidget::ToggleContentVisibility()
{
	bContentHidden = !bContentHidden;
	ApplyContentVisibility();

	if (HideShowButtonText)
	{
		HideShowButtonText->SetText(bContentHidden
			? NSLOCTEXT("ArenaHUD", "RewardShow", "显示选择")
			: NSLOCTEXT("ArenaHUD", "RewardHide", "隐藏选择"));
	}
}

void UArenaRewardScreenWidget::ApplyContentVisibility()
{
	// 展开态必须是 SelfHitTestInvisible 而不是 HitTestInvisible：
	// HitTestInvisible 会把【子控件一起】变成不可点击 —— 卡片全在 PromptContent
	// 里，用它整个三选一就"看得见点不着"（真踩过）。SelfHitTestInvisible 只让
	// 容器自己不挡点击（空画布区域穿透到世界），卡片/按钮照常可交互。
	const ESlateVisibility TargetVis = bContentHidden
		? ESlateVisibility::Collapsed
		: ESlateVisibility::SelfHitTestInvisible;

	// 有内容根就整块收/放；没有就逐个收（两个路径行为一致，只是效率差别）。
	if (PromptContent)
	{
		PromptContent->SetVisibility(TargetVis);
	}
	else
	{
		if (TitleText)
		{
			TitleText->SetVisibility(TargetVis);
		}
		if (CardContainer)
		{
			CardContainer->SetVisibility(TargetVis);
		}
	}
}

void UArenaRewardScreenWidget::ApplyPrompt(const FArenaPromptView& InPrompt)
{
	Prompt = InPrompt;

	// -----------------------------------------------------------------------
	// ① 显隐。收起时把卡片一并清掉 —— 留着它们在下次开出来时会先闪一帧旧内容。
	// -----------------------------------------------------------------------
	if (!Prompt.bActive)
	{
		if (CardContainer)
		{
			CardContainer->ClearChildren();
		}
		Cards.Reset();

		SetVisibility(ESlateVisibility::Collapsed);
		BP_OnPromptChanged(Prompt);
		return;
	}

	SetVisibility(ESlateVisibility::Visible);

	if (TitleText)
	{
		TitleText->SetText(Prompt.Title);
	}

	// -----------------------------------------------------------------------
	// ② 卡片：数量变了才重建，否则只灌视图。
	//
	// 【为什么不是每次都重建】三选一答完会立刻来下一份待选（选完装备 → 锻造器菜单），
	// 每次重建等于把玩家刚点过的那张卡的动画和悬停状态全部丢掉 ——
	// 表现是"点一下，整排卡片闪一下"。数量没变时逐张 ApplyCard 就够了，
	// 而 ApplyCard 自己还会跳过内容没变的卡片。
	// -----------------------------------------------------------------------
	EnsureCardCount(Prompt.Cards.Num());

	for (int32 Index = 0; Index < Cards.Num() && Prompt.Cards.IsValidIndex(Index); ++Index)
	{
		if (UArenaChoiceCardWidget* Card = Cards[Index])
		{
			Card->ApplyCard(Prompt.Cards[Index]);
		}
	}

	// 内容的收/放状态以本地按钮为准 —— 换待选不改变"玩家收起了界面"这个事实。
	ApplyContentVisibility();

	// 放最后：让蓝图在"已经知道全部数据"之后再决定额外表现。
	BP_OnPromptChanged(Prompt);
}

void UArenaRewardScreenWidget::EnsureCardCount(int32 DesiredCount)
{
	// 数量对得上就什么都不做 —— 这一条同时是"重建过没有"的判据，
	// 所以不需要另外记一个"上次建了几张"。建失败（CreateWidget 返回 null）时
	// Cards.Num() 会小于 DesiredCount，于是下一次推送会再试一遍。
	if (Cards.Num() == DesiredCount)
	{
		return;
	}

	if (!CardContainer)
	{
		// WBP 里没放容器（或名字没对上）。这不是"画不出来"那么简单 —— 玩家会卡在
		// 奖励阶段永远进不了战斗，所以这里必须吵。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 上没有名为 CardContainer 的容器控件，三选一界面画不出任何卡片。")
			TEXT("请在 WBP 里放一个 PanelWidget（HorizontalBox / WrapBox / Grid…）并命名成 CardContainer。"),
			*GetName());
		return;
	}

	if (!CardWidgetClass)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 没配 CardWidgetClass，三选一界面画不出任何卡片。")
			TEXT("请在 WBP 的 Details 面板里把 CardWidgetClass 指向 WBP_ArenaChoiceCard。"),
			*GetName());
		return;
	}

	CardContainer->ClearChildren();
	Cards.Reset();
	Cards.Reserve(DesiredCount);

	for (int32 Index = 0; Index < DesiredCount; ++Index)
	{
		// 用 OwningPlayer 而不是 GetWorld()：CreateWidget 需要一个 PlayerController
		// 来定 UI 的所属玩家（决定本地化、输入路由）。拿不到就什么都不建。
		UArenaChoiceCardWidget* Card = CreateWidget<UArenaChoiceCardWidget>(GetOwningPlayer(), CardWidgetClass);
		if (!Card)
		{
			continue;
		}

		Card->OnCardClicked.AddDynamic(this, &UArenaRewardScreenWidget::HandleCardClicked);
		Card->OnRerollClicked.AddDynamic(this, &UArenaRewardScreenWidget::HandleCardRerolled);

		// AddChild 而不是 AddChildToHorizontalBox：容器类型是 UPanelWidget（见头文件），
		// 槽位规则由 WBP 里那个具体容器自己决定，C++ 不替它猜。
		CardContainer->AddChild(Card);

		// 卡与卡之间留空隙（用户反馈：卡片贴在一起）。FMargin 左右对称：
		// 每张卡左右各 12px ⇒ 相邻卡间隙 24px，整行左右外边距各 12px。
		// 卡框外发光会向两侧各溢出 ~6px，间隙小于这个数发光会糊到邻卡上。
		// 只有 HBox 才管间距；WBP 换成别的容器时这里自动不生效。
		if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(Card->Slot))
		{
			HBoxSlot->SetPadding(FMargin(12.f, 0.f));
		}

		Cards.Add(Card);
	}

	if (Cards.Num() != DesiredCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 要建 %d 张卡片，实际只建出 %d 张（CardWidgetClass 是不是配错了？）。"),
			*GetName(), DesiredCount, Cards.Num());
	}
}
