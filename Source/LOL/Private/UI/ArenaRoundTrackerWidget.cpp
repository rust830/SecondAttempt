// 回合线的实现。设计意图全在头文件里。

#include "UI/ArenaRoundTrackerWidget.h"

#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Engine/Texture2D.h"

#define LOCTEXT_NAMESPACE "ArenaRoundTracker"

UArenaRoundTrackerWidget::UArenaRoundTrackerWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 默认指向斗魂官方的回合线图标（导入脚本生成的路径）。
	// 资产还没导入时这些软引用解析失败 → 走"染色兜底"，不报错、不空格子。
	// 【为什么在构造函数里填】这是"这套界面长什么样"的一部分，
	// 和 UArenaChoiceCardWidget 的四张卡底同一条规矩：默认皮在 C++ 给好，实例可换。
	// ⚠️ 路径【不带 .png】—— FSoftObjectPath 是资产路径，不是文件名，带扩展名必然解析失败。
	const FString Base = TEXT("/Game/LOL/UI/Arena/Textures/Cherry/StageIcons/");

	auto Set = [&Base](FArenaStageIconSet& Set_, const FString& Stem)
	{
		Set_.Completed = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(*(Base + Stem + TEXT("completed"))));
		Set_.Current   = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(*(Base + Stem + TEXT("current"))));
		Set_.Upcoming  = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(*(Base + Stem + TEXT("upcoming"))));
	};

	FArenaStageIconSet Combat;
	Set(Combat, TEXT("combat"));
	// ⚠ 官方贴图里没有 combatcompleted（战斗回合的"完成"在原作里是队伍胜负图标画的），
	// 这里故意只填两态：完成态走"进行态压灰"兜底。
	StageIcons.Add(EArenaStageKindView::Combat, Combat);

	FArenaStageIconSet Augments;
	Set(Augments, TEXT("augmentsselection"));
	StageIcons.Add(EArenaStageKindView::Augments, Augments);

	FArenaStageIconSet ItemPurchase;
	Set(ItemPurchase, TEXT("itempurchase"));
	StageIcons.Add(EArenaStageKindView::ItemPurchase, ItemPurchase);

	FArenaStageIconSet StatAnvil;
	Set(StatAnvil, TEXT("statanvil"));
	StageIcons.Add(EArenaStageKindView::StatAnvil, StatAnvil);
}

void UArenaRoundTrackerWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// 设计时不跑（WBP 的 Designer 里不需要一排真的 Image）。
	if (IsDesignTime())
	{
		return;
	}
}

void UArenaRoundTrackerWidget::NativeDestruct()
{
	Entries.Reset();
	ResolvedIcons.Reset();

	Super::NativeDestruct();
}

void UArenaRoundTrackerWidget::ApplyTracker(const FArenaRoundTrackerView& InTracker)
{
	Tracker = InTracker;

	const bool bHasContainer = TrackContainer != nullptr;
	if (!bHasContainer || !InTracker.bValid || InTracker.Stages.Num() <= 0)
	{
		// 没容器 / 没计划 → 整条收起。画一排空格子看起来像界面坏了。
		if (bHasContainer)
		{
			TrackContainer->SetVisibility(ESlateVisibility::Collapsed);
		}
		return;
	}

	TrackContainer->SetVisibility(ESlateVisibility::HitTestInvisible);
	EnsureEntryCount(InTracker.Stages.Num());

	for (int32 Index = 0; Index < InTracker.Stages.Num() && Index < Entries.Num(); ++Index)
	{
		UImage* Entry = Entries[Index];
		if (!Entry)
		{
			continue;
		}

		UTexture2D* Texture = nullptr;
		FLinearColor Tint = FLinearColor::White;
		ResolveEntryVisual(InTracker.Stages[Index], Texture, Tint);

		if (Texture)
		{
			// 贴图按原始尺寸画（官方图标本身就是 32~40px 的成品）。
			Entry->SetBrushFromTexture(Texture);
			Entry->SetColorAndOpacity(Tint);
			Entry->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			// 连兜底贴图都没有（三种都空）：占位格子用空刷子画不出来，
			// 收起这一格 —— 一格空白比一排少一格更难解释。
			Entry->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	BP_OnTrackerChanged(InTracker);
}

void UArenaRoundTrackerWidget::EnsureEntryCount(int32 DesiredCount)
{
	if (!TrackContainer)
	{
		return;
	}

	// 数量一样就复用：回合计划整场不变，这里的重建只发生在开局的第一份推送。
	if (Entries.Num() == DesiredCount)
	{
		return;
	}

	for (UImage* Entry : Entries)
	{
		if (Entry)
		{
			Entry->RemoveFromParent();
		}
	}
	Entries.Reset();

	for (int32 Index = 0; Index < DesiredCount; ++Index)
	{
		UImage* Entry = NewObject<UImage>(this);
		TrackContainer->AddChild(Entry);

		// 容器是 HorizontalBox 时给个 4px 的横向间距。
		// 【5.8 API 事实】UHorizontalBoxSlot 没有 SetAutoSize（那是 CanvasPanelSlot 的）——
		// HBox 槽位默认就按子控件的期望尺寸（贴图原始大小）排，不用也不该设。
		// 其他容器（WrapBox / VerticalBox）由容器自己排，不强求。
		if (UHorizontalBoxSlot* HBoxSlot = Cast<UHorizontalBoxSlot>(Entry->Slot))
		{
			HBoxSlot->SetPadding(FMargin(4.f, 0.f));
		}

		Entries.Add(Entry);
	}
}

void UArenaRoundTrackerWidget::ResolveEntryVisual(const FArenaStageEntryView& Entry, UTexture2D*& OutTexture, FLinearColor& OutTint)
{
	OutTexture = ResolveIconTexture(Entry.Kind, Entry.State);
	OutTint = FLinearColor::White;

	if (OutTexture)
	{
		return;   // 本态有贴图，原样画
	}

	// 本态缺失 → 用进行态贴图 + 染色兜底。
	OutTexture = ResolveIconTexture(Entry.Kind, EArenaStageState::Current);
	switch (Entry.State)
	{
	case EArenaStageState::Completed:
		OutTint = FLinearColor(0.42f, 0.42f, 0.42f, 1.f);   // 压灰：打完了
		break;
	case EArenaStageState::Upcoming:
		OutTint = FLinearColor(0.55f, 0.55f, 0.55f, 0.55f); // 压暗半透明：还没到
		break;
	case EArenaStageState::Current:
	default:
		break;
	}
}

UTexture2D* UArenaRoundTrackerWidget::ResolveIconTexture(EArenaStageKindView Kind, EArenaStageState State)
{
	// 键 = Kind<<2 | State（4 × 3 = 12 种组合，uint8 装得下）。
	const uint8 Key = static_cast<uint8>(Kind) << 2 | static_cast<uint8>(State);
	if (TObjectPtr<UTexture2D>* Found = ResolvedIcons.Find(Key))
	{
		return *Found;
	}

	UTexture2D* Texture = nullptr;
	if (const FArenaStageIconSet* SetPtr = StageIcons.Find(Kind))
	{
		const TSoftObjectPtr<UTexture2D>& Soft =
			State == EArenaStageState::Completed ? SetPtr->Completed
			: State == EArenaStageState::Upcoming ? SetPtr->Upcoming
			: SetPtr->Current;

		if (!Soft.IsNull())
		{
			// Get() 命中 = 已加载（皮肤贴图在项目里通常常驻）；
			// miss 才同步加载一次，之后进缓存，不会再走到这行。
			Texture = Soft.Get() ? Soft.Get() : Soft.LoadSynchronous();
		}
	}

	ResolvedIcons.Add(Key, Texture);
	return Texture;
}

#undef LOCTEXT_NAMESPACE
