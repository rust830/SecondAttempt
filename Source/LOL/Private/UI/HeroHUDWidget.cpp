// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroHUDWidget.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

#include "GAS/HeroHUDController.h"
#include "LOLPlayerController.h"
#include "UI/HeroAttributePanelWidget.h"
#include "UI/HeroHealthBarWidget.h"
#include "UI/HeroSkillSlotWidget.h"
#include "UI/HeroTargetFrameWidget.h"

namespace
{
	/**
	 * 兜底目标框的资产路径。
	 *
	 * 【为什么单独拎出来】构造里的 FClassFinder 和 EnsureTargetFrameWidget 里的运行时兜底
	 * 找的是同一个资产，路径写两遍迟早会只改一处。
	 */
	const TCHAR* const GHeroTargetFrameWidgetPath = TEXT("/Game/LOL/UI/WBP_HeroTarget.WBP_HeroTarget_C");
}

// ===========================================================================
// 生命周期
// ===========================================================================

UHeroHUDWidget::UHeroHUDWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// -----------------------------------------------------------------------
	// 目标框兜底类的【默认值】：写在这里 = "这个 HUD 天生就带目标框"。
	//
	// 【为什么带一个默认值，而不是"谁要谁去 WBP 上配"】这条兜底的全部意义就是"不用配也能看到
	// 目标框"（见 TargetFrameWidgetClass 的注释）。留成空的属性就退化回"必须记得去 Class
	// Defaults 里配一下，忘了就是屏幕上什么都没有、而且不报错"—— 正是这个功能一直没生效的原因。
	// 想换一个（换个 WBP）就在 WBP_HUDWidget 的 Class Defaults 里覆盖这个属性，蓝图值优先。
	//
	// 【为什么不是 UPROPERTY 的默认值直接写路径】TSubclassOf 的属性默认值只能是 nullptr，
	// 类引用得在构造里用 FClassFinder 解出来。这也让它是【硬引用】：打包时 WBP_HeroTarget
	// 一定会被 cook 进去，不会出现"编辑器里好好的、打包后目标框不见了"。
	// -----------------------------------------------------------------------
	static ConstructorHelpers::FClassFinder<UHeroTargetFrameWidget> DefaultTargetFrameFinder(
		GHeroTargetFrameWidgetPath);
	if (DefaultTargetFrameFinder.Succeeded())
	{
		TargetFrameWidgetClass = DefaultTargetFrameFinder.Class;
	}
}

void UHeroHUDWidget::ApplyAttributeBackdropHeight(bool bExpanded)
{
	if (!AttributeBackdrop)
	{
		return;                       // WBP 里没放这个控件（或名字没对上）就不做
	}
	if (UCanvasPanelSlot* BackdropSlot = Cast<UCanvasPanelSlot>(AttributeBackdrop->Slot))
	{
		const float Height = bExpanded ? AttributeBackdropHeight_Expanded
		                               : AttributeBackdropHeight_Collapsed;
		// 左下锚定 ⇒ 只写高度，底边钉在 WBP 里摆好的那个位置，顶边往上长。
		BackdropSlot->SetOffsets(FMargin(0.f, -Height, 117.f, Height));
	}
}

void UHeroHUDWidget::HandleAttributePanelExpanded(bool bInExpanded)
{
	ApplyAttributeBackdropHeight(bInExpanded);
}

void UHeroHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// 设计器里一行逻辑都不要跑：否则每打开一次 WBP_HUD 就是一堆空指针。
	if (IsDesignTime())
	{
		return;
	}

	// 属性区背景框：面板展开/收起时跟着变高（收起 96 / 展开 168，见 ApplyAttributeBackdropHeight）。
	if (AttributePanel)
	{
		AttributePanel->OnExpandedChanged.AddDynamic(this, &UHeroHUDWidget::HandleAttributePanelExpanded);
		ApplyAttributeBackdropHeight(AttributePanel->IsExpanded());
	}

	// 【必须在 BindToController 之前】绑定末尾那次 ApplySnapshot 就会推第一份目标框视图，
	// 那时控件还不存在的话，第一帧的目标框会被静默丢掉（下一个属性变化才补上）。
	EnsureTargetFrameWidget();

	BindToController();
}

void UHeroHUDWidget::NativeDestruct()
{
	StopControllerRetry();
	UnbindFromController();

	Super::NativeDestruct();
}

void UHeroHUDWidget::EnsureTargetFrameWidget()
{
	// WBP 里挂了控件 → 设计师说了算，C++ 一个字都不插手（位置 / 层级 / 边框全在设计器里）。
	if (TargetFrame)
	{
		return;
	}

	// 兜底类的来源有两处，按优先级：
	//   ① 属性上的值（构造里 FClassFinder 给的默认，或者 WBP_HUDWidget 上覆盖的）；
	//   ② 属性为空时按路径【运行时】再解一次。
	//
	// 【为什么②这条必须有】①在编辑器里会莫名其妙地失效：BP 类的 CDO 在热重载 / 类重新实例化时
	// 会被"旧值"覆盖一遍，构造里设的默认值就这么没了 —— 属性读出来是 None，于是目标框不建、
	// 屏幕上什么都没有、还不报错（这个功能已经这样静默失效过一次）。LoadClass 走的是普通资产加载，
	// 不受那套 CDO 语义影响。（构造里那次仍然保留：它给 Class Defaults 面板一个可见的默认值，
	// 也是一条【硬引用】，打包时保证 WBP_HeroTarget 被 cook 进去。）
	TSubclassOf<UHeroTargetFrameWidget> WidgetClass = TargetFrameWidgetClass;
	if (!WidgetClass)
	{
		WidgetClass = LoadClass<UHeroTargetFrameWidget>(nullptr, GHeroTargetFrameWidgetPath);
	}

	// 两条路都没拿到 = 这个 HUD 不要兜底目标框（或者路径写错了）。不是错误，不打日志。
	if (!WidgetClass)
	{
		return;
	}

	APlayerController* OwningPC = GetOwningPlayer();
	if (!OwningPC)
	{
		return;
	}

	TargetFrame = CreateWidget<UHeroTargetFrameWidget>(OwningPC, WidgetClass);
	if (!TargetFrame)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UHeroHUDWidget: 目标框类 %s 建不出来，目标框不会有任何显示。"), *GetNameSafe(WidgetClass));
		return;
	}

	// 【为什么是 AddToViewport，而不是塞进本 Widget 的控件树】控件树是【资产里那张图】，
	// 运行期往里加控件就得自己找容器、自己写槽位（锚点 / 对齐 / 偏移），而且和设计器里
	// 那张图是两套坐标 —— 设计师在 WBP 里挪不动它。WBP_HeroTarget 的根是一张铺满屏幕的
	// CanvasPanel，让它自己进视口，它内部设计好的锚点就直接生效：要挪位置改 WBP 就行。
	//
	// 【为什么 ZOrder 要高出本 HUD 一层】本 HUD 是被 AddToPlayerScreen(0) 加进来的，
	// 而本函数跑在本 HUD 的 NativeConstruct 里 —— 也就是【目标框先进视口、HUD 后进】。
	// 同一层的时候谁在上面就看添加顺序，结果是 HUD 压住目标框。高一层把这个顺序问题抹掉。
	static constexpr int32 FallbackTargetFrameZOrder = 1;
	TargetFrame->AddToViewport(FallbackTargetFrameZOrder);

	// 刚建出来还没有任何数据，先收起 —— 否则它会在屏幕上闪一帧空的（根控件的默认可见性）。
	// 紧接着 BindToController → ApplySnapshot 就会推第一份真实视图上来。
	TargetFrame->SetVisibility(ESlateVisibility::Collapsed);
}

void UHeroHUDWidget::RefreshFromController()
{
	// 没绑上就当成一次「重来」——覆盖「Widget 建的时候 PC 还没建 Controller」这种情况。
	if (!HUDController)
	{
		BindToController();
		return;
	}

	// 先让 Controller 按【当前】配置重算一遍每个槽位的静态表现（图标 / 键位 / 名字 / 种类 /
	// SlotSizeScale），再拉全量。
	//
	// 少了这一步，PIE 运行中改 DataAsset 是看不到反应的：PullHUDState 端出来的是一份
	// 【早就算好并缓存起来的】视图，配置改了它也不知道。见 RefreshSlotViewsFromConfig 的注释。
	HUDController->RefreshSlotViewsFromConfig();

	ApplySnapshot(HUDController->PullHUDState());
}

void UHeroHUDWidget::ToggleAttributePanel()
{
	// 没挂面板就是空操作 —— 按键处理器那边不用先判一次（判两处迟早会分叉）。
	if (AttributePanel)
	{
		AttributePanel->ToggleExpanded();
	}
}

// ===========================================================================
// 绑定
// ===========================================================================

void UHeroHUDWidget::BindToController()
{
	if (HUDController)
	{
		return;
	}

	ALOLPlayerController* PC = Cast<ALOLPlayerController>(GetOwningPlayer());
	if (!PC)
	{
		// 不是玩家 HUD（比如挂在 WidgetComponent 上给 AI 用）。不是错误，直接不管。
		return;
	}

	UHeroHUDController* Controller = PC->GetHUDController();
	if (!Controller)
	{
		// PC 还没建 Controller。PC 会先建 Controller 再建 Widget，所以正常路径上不会走到这里；
		// 走到这里说明这个 WBP_HUD 是被别处手工 Add 出来的，等一下就好。
		StartControllerRetry();
		return;
	}

	HUDController = Controller;
	StopControllerRetry();

	// 顺序：先订阅、再拉取。
	// 订阅管「之后的变化」，拉取管「此刻的状态」；反过来的话，两者之间发出的变化会丢。
	HUDController->OnVitalsChanged.AddDynamic(this, &UHeroHUDWidget::HandleVitalsChanged);
	HUDController->OnSkillSlotChanged.AddDynamic(this, &UHeroHUDWidget::HandleSkillSlotChanged);
	HUDController->OnTargetFrameChanged.AddDynamic(this, &UHeroHUDWidget::HandleTargetFrameChanged);
	HUDController->OnAttributesChanged.AddDynamic(this, &UHeroHUDWidget::HandleAttributesChanged);
	HUDController->OnHUDReady.AddDynamic(this, &UHeroHUDWidget::HandleHUDReady);

	// 【订阅即拉取】。这就是「绑定不触发初始值」那个坑的正解：
	// 不管 Controller 是在这个 Widget 之前还是之后绑上 ASC 的，这里一定拿得到当前全量。
	// 所以 Controller 那边【不】需要「记得在正确时机 BroadcastInitialValues()」。
	ApplySnapshot(HUDController->PullHUDState());
}

void UHeroHUDWidget::UnbindFromController()
{
	if (!HUDController)
	{
		return;
	}

	HUDController->OnVitalsChanged.RemoveDynamic(this, &UHeroHUDWidget::HandleVitalsChanged);
	HUDController->OnSkillSlotChanged.RemoveDynamic(this, &UHeroHUDWidget::HandleSkillSlotChanged);
	HUDController->OnTargetFrameChanged.RemoveDynamic(this, &UHeroHUDWidget::HandleTargetFrameChanged);
	HUDController->OnAttributesChanged.RemoveDynamic(this, &UHeroHUDWidget::HandleAttributesChanged);
	HUDController->OnHUDReady.RemoveDynamic(this, &UHeroHUDWidget::HandleHUDReady);

	HUDController = nullptr;
}

void UHeroHUDWidget::StartControllerRetry()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (++ControllerRetryCount > MaxControllerRetries)
	{
		ControllerRetryCount = 0;
		UE_LOG(LogTemp, Warning,
			TEXT("UHeroHUDWidget: %d 次重试后仍然拿不到 UHeroHUDController。")
			TEXT("这个 WBP_HUD 多半不是 ALOLPlayerController 建的。"),
			MaxControllerRetries);
		return;
	}

	World->GetTimerManager().SetTimer(
		ControllerRetryHandle, this, &UHeroHUDWidget::RefreshFromController, ControllerRetryInterval, /*bLoop=*/false);
}

void UHeroHUDWidget::StopControllerRetry()
{
	ControllerRetryCount = 0;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ControllerRetryHandle);
	}
}

// ===========================================================================
// 应用状态
// ===========================================================================

void UHeroHUDWidget::ApplySnapshot(const FHUDSnapshot& Snapshot)
{
	HandleVitalsChanged(Snapshot.Vitals);
	HandleTargetFrameChanged(Snapshot.Target);
	HandleAttributesChanged(Snapshot.Attributes);

	for (const FSkillSlotView& View : Snapshot.Slots)
	{
		HandleSkillSlotChanged(View.SlotIndex, View);
	}

	// 放最后：让蓝图在「已经知道全部状态」之后再决定显示/隐藏，不会先闪一下。
	HandleHUDReady(Snapshot.bBound);
}

void UHeroHUDWidget::HandleVitalsChanged(const FHUDVitalsView& Vitals)
{
	// 一个视图推给两条条：哪个是血、哪个是能量由条自己做主（bIsEnergyBar）。
	if (HealthBar)
	{
		HealthBar->ApplyVitals(Vitals);
	}
	if (EnergyBar)
	{
		EnergyBar->ApplyVitals(Vitals);
	}

	BP_OnVitalsChanged(Vitals);
}

void UHeroHUDWidget::HandleSkillSlotChanged(int32 SlotIndex, const FSkillSlotView& View)
{
	if (UHeroSkillSlotWidget* SlotWidget = EnsureSlotWidget(SlotIndex))
	{
		// 尺寸 / 对齐 / 间距先落，再灌视图。
		//
		// 【为什么必须在这里调、而且每次都要调】这条路径是数据到达 Widget 的唯一入口，
		// 而视图里的 SlotSizeScale 是 DataAsset 决定的、运行期会变。只在建格时算一次的话，
		// 后来的变化就没有任何人消费 —— 表现就是「改了 SlotSizeScale，size 没变」。
		ApplySlotLayout(SlotIndex, View);
		SlotWidget->ApplySlotView(View);
	}

	BP_OnSkillSlotChanged(SlotIndex, View);
}

void UHeroHUDWidget::HandleTargetFrameChanged(const FTargetFrameView& View)
{
	// 目标框现在有内置子 Widget：名字 / 血 / 能量 / 死亡压暗 / 显隐全在 C++（UHeroTargetFrameWidget）。
	// 蓝图只负责布局和额外表现（选中动画、边框等）。没在 WBP 里挂 TargetFrame 控件就退回旧行为。
	if (TargetFrame)
	{
		TargetFrame->ApplyTargetFrame(View);
	}

	BP_OnTargetFrameChanged(View);
}

void UHeroHUDWidget::HandleAttributesChanged(const FHeroAttributePanelView& Panel)
{
	// 面板的建行 / 形态 / 收起全在 C++（UHeroAttributePanelWidget），这里只管转交。
	// 没在 WBP 里挂 AttributePanel 控件就退回「蓝图自己搭」的旧行为（什么都不做）。
	if (AttributePanel)
	{
		AttributePanel->ApplyPanelView(Panel);
	}

	BP_OnAttributesChanged(Panel);
}

void UHeroHUDWidget::HandleHUDReady(bool bBound)
{
	BP_OnHUDReady(bBound);
}

UHeroSkillSlotWidget* UHeroHUDWidget::EnsureSlotWidget(int32 SlotIndex)
{
	if (SlotIndex == INDEX_NONE)
	{
		return nullptr;
	}

	// 已经建过就直接返回。尺寸【不在这里补】—— 调用方（HandleSkillSlotChanged）紧接着会调
	// ApplySlotLayout，那才是尺寸的唯一出口。
	if (SlotWidgets.IsValidIndex(SlotIndex) && SlotWidgets[SlotIndex])
	{
		return SlotWidgets[SlotIndex];
	}

	if (!SkillSlotContainer || !SkillSlotWidgetClass)
	{
		return nullptr;
	}

	UHeroSkillSlotWidget* NewWidget = CreateWidget<UHeroSkillSlotWidget>(GetOwningPlayer(), SkillSlotWidgetClass);
	if (!NewWidget)
	{
		return nullptr;
	}

	NewWidget->SetSlotIndex(SlotIndex);

	// ---------------------------------------------------------------------
	// 【恒包两层壳】SizeBox（定**实际尺寸**）→ ScaleBox（把设计画面等比缩放过去）→ 槽位 widget
	//
	// 【为什么必须有 ScaleBox —— 这是整条链上最容易漏的一环】
	// 槽位 WBP 里的实际画面（IconImage / 技能框 / 冷却数字…）挂在 CanvasPanel 上，而
	// SConstraintCanvas 给子控件定的尺寸【就是 WBP 里填的那两个 offset】：
	//     SlotSize = FVector2D(Offset.Right, Offset.Bottom)
	//     Size     = AutoSize ? 子控件自报尺寸 : SlotSize
	//     （Slate/Private/Widgets/Layout/SConstraintCanvas.cpp:249-251）
	// 也就是说：【容器给画布多大，跟画布里图标多大完全无关】。只有锚点被拉成
	// "左右/上下都有跨度"（Anchors.Min.X != Max.X）时才反过来用画布实测尺寸（同文件 260-281）。
	//
	// 后果：只缩外面的盒子，图标原样不动 —— HorizontalBox 按小尺寸排、画面按大尺寸画，
	// 相邻两格互相压，看起来就是"错位 / 后面的图标前移了"。同理，改任何尺寸都不可能
	// 只靠外面的 SizeBox 传到画布里去。
	//
	// 想让整块设计画面【等比缩放】，UMG 里只有 ScaleBox 这一条路：它用
	// `AllottedGeometry.MakeChild(Child, Offset, DesiredSize, FinalScale)` 给子控件加一层
	// layout scale，子控件自己仍然以为是设计尺寸（Slate/.../SScaleBox.cpp:282-287）。
	//   · Stretch = ScaleToFit        → FinalScale = min(区域 / 子控件设计尺寸)（同文件 184-187）
	//   · StretchDirection = Both     → 不夹到 1，放大缩小都能（同文件 210-220）
	// 这两个值刚好就是 UScaleBox 构造函数的默认值（UMG/.../ScaleBox.cpp:21-22），
	// 这里仍然显式写一遍：免得以后引擎改默认值把行为悄悄改掉。
	//
	// 【契约】ScaleToFit 的"设计尺寸"取自子控件自报的 desired size ⇒ WBP_SkillSlot 的根
	// 必须自报一个尺寸（现在就是那个 Width/Height Override = 64）。那个 64 从此【只表示设计基准】，
	// 屏幕上的实际大小 = SkillSlotSize × SlotSizeScale。根不自报尺寸（= 0）的话 SScaleBox 会
	// 退化成 scale 1（同文件 180），等于没缩放 —— 那种情况要先修 WBP 的根。
	//
	// 【为什么不能"配了才包"】尺寸是运行期会变的（ApplySlotLayout 一直同步它），
	// 若"有没有壳"也跟着配置变，就得出运行期往 HorizontalBox 里插控件这种脏活。
	// 正确做法是让【结构一次定死，只让尺寸变】。
	//
	// 【为什么不能直接 AddChild 完事】HorizontalBox 的槽位规则是 Automatic（按子控件自报的
	// 尺寸排），而 WBP_SkillSlot 的根如果是 CanvasPanel 这类控件，自报尺寸是 0 —— 表现就是
	// 六格挤成一条线。SizeBox 的宽高覆盖是「从外面定死尺寸」，和根是什么控件无关。
	// 详见 SkillSlotSize 的注释（含 s1mpleFps 里踩过的同款坑）。
	// ---------------------------------------------------------------------
	UWidget* WidgetToAdd = NewWidget;
	if (WidgetTree)
	{
		USizeBox* SizeBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		UScaleBox* ScaleBox = WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass());
		// 两层壳都建出来了才用它们；只建出一个（理论上不会发生）就退回"什么都不包"，
		// 不做半套 —— 缺 SizeBox 的话尺寸覆盖就没地方写，缺 ScaleBox 的话画面又不会缩放。
		if (SizeBox && ScaleBox)
		{
			ScaleBox->SetStretch(EStretch::ScaleToFit);
			ScaleBox->SetStretchDirection(EStretchDirection::Both);
			ScaleBox->AddChild(NewWidget);

			SizeBox->AddChild(ScaleBox);
			WidgetToAdd = SizeBox;
		}
	}

	if (UHorizontalBox* Box = Cast<UHorizontalBox>(SkillSlotContainer))
	{
		// 槽位规则显式给 Automatic（按子控件自报尺寸排，不拉伸不塌缩），
		// 和 s1mpleFps 的手雷槽位同一套写法 —— 那里验证过。
		//
		// 对齐 / 尺寸 / 间距【不在这里给】：统一由 ApplySlotLayout 负责。两处都写就是两个真相，
		// 迟早分叉 —— 这次「改了 SlotSizeScale 没反应」就是分叉的一种形态。
		if (UHorizontalBoxSlot* BoxSlot = Box->AddChildToHorizontalBox(WidgetToAdd))
		{
			BoxSlot->SetSize(ESlateSizeRule::Automatic);
			BoxSlot->SetHorizontalAlignment(HAlign_Center);
		}
	}
	else
	{
		// 其他容器（UniformGridPanel / WrapBox…）走通用路径：
		// 槽位规则由各自的容器决定，C++ 不替它们猜。
		SkillSlotContainer->AddChild(WidgetToAdd);
	}

	// 用 SetNum 而不是 Add：槽位号必须等于数组下标。
	// 一旦用 Add，中间的空位就会把下标整体左移，「按 SlotIndex 找 Widget」全部错位。
	if (!SlotWidgets.IsValidIndex(SlotIndex))
	{
		SlotWidgets.SetNum(SlotIndex + 1);
	}
	SlotWidgets[SlotIndex] = NewWidget;

	return NewWidget;
}

UPanelSlot* UHeroHUDWidget::FindContainerSlot(const UHeroSkillSlotWidget& SlotWidget) const
{
	// 【局部变量名要避开 UWidget 的成员】`Slot` 和 `Cursor` 都是 UWidget 上的成员变量，
	// 用它们当局部名会遮住成员 —— C4458，而 UBT 把这条当错误（不是警告）。
	// 这个函数正好两个都踩过。换个不相干的名字就没事。
	UPanelSlot* WalkSlot = SlotWidget.Slot.Get();

	// 一路往上走，直到某个槽位的 Parent 就是 SkillSlotContainer 为止。
	//
	// 【为什么是"走"而不是"上一层"】槽位外面的壳是实现细节（现在是 SizeBox → ScaleBox），
	// 而 SlotWidget->Slot 拿到的是【最内层】壳的槽 —— 写死层数的话，加/减一层壳就会静默失配
	//（Cast 返回 null → 什么都不做 → 表现成"改了没反应"）。
	//
	// 8 层是纯防呆上限：控件树不可能这么深，真走到这个数说明有环、或者这一格根本没挂在容器下。
	constexpr int32 MaxShellDepth = 8;
	for (int32 Depth = 0; WalkSlot && WalkSlot->Parent != SkillSlotContainer && Depth < MaxShellDepth; ++Depth)
	{
		UPanelWidget* Wrapper = WalkSlot->Parent;
		WalkSlot = Wrapper ? Wrapper->Slot.Get() : nullptr;
	}

	// 走到顶都没碰上 SkillSlotContainer（比如这一格被挂到别处去了）就明确返回空，
	// 让调用方什么都不做 —— 好过把尺寸/对齐写到一个不相干的容器的槽位上。
	return (WalkSlot && WalkSlot->Parent == SkillSlotContainer) ? WalkSlot : nullptr;
}

// ===========================================================================
// 槽位布局：尺寸 / 纵向对齐 / 间距
//
// 【为什么单独一个函数，而不是建格时算一次】
// 这三样都取决于 Widget 上的配置，其中尺寸还乘了 FSkillSlotView::SlotSizeScale（DataAsset
// 决定的）。建格时算一次写死的话，改完配置已有的格子就没有任何人会去更新它 ——
// 用户看到的现象就是「改了 SlotSizeScale，实际 size 没变」。
//
// 所以做成【幂等 + 每次视图变化都调】。幂等不是优化，是必须的：
// 冷却期间 UHeroHUDController::SampleCooldowns 的 30Hz 心跳走的正是这条广播路径
//（OnSkillSlotChanged → HandleSkillSlotChanged），不先读一次现值就无脑写的话，
// 六个格子每秒会被打一百多次 Layout 失效。
// ===========================================================================

void UHeroHUDWidget::ApplySlotLayout(int32 SlotIndex, const FSkillSlotView& View)
{
	if (!SlotWidgets.IsValidIndex(SlotIndex))
	{
		return;
	}

	UHeroSkillSlotWidget* SlotWidget = SlotWidgets[SlotIndex];
	if (!SlotWidget)
	{
		return;
	}

	// 【落点：容器槽 + 最外层那个 SizeBox】
	// 用 Slot 反查而不是另外存一份指针，是为了让状态只有一个来源 —— 另存一份
	//（比如 TArray<TObjectPtr<USizeBox>>）迟早会和控件树上的真实结构对不上。
	UPanelSlot* ContainerSlot = FindContainerSlot(*SlotWidget);

	// 容器槽的 Content 就是我们包的最外层那个壳。现在链子是 SizeBox → ScaleBox → 槽位 widget，
	// 所以它就是尺寸落点（UPanelWidget::AddChild 同时设好 Content / Parent，PanelWidget.cpp:170-173）。
	// 一个壳都没包上时 Content 是槽位 widget 自己，Cast 失败 → 尺寸那一段自然跳过。
	USizeBox* SizeBox = ContainerSlot ? Cast<USizeBox>(ContainerSlot->Content) : nullptr;

	// -------------------------------------------------------------------
	// ① 尺寸：写进包裹它的 SizeBox。
	// 没包上 → 跳过（这一格不是我们建的，或走了 WidgetTree 不可用时的降级路径）。
	// -------------------------------------------------------------------
	if (SizeBox)
	{
		// 每格的倍率乘在全局基准尺寸上 —— 「被动小一点」走的就是这条，这里【不判 Kind】。
		// ≤ 0 当成 1：倍率配错了应该退化成「和其他格一样大」，而不是让格子消失
		//（消失是 bHideWhenUnavailable 的职责，两个字段各管一件事，别互相兜底）。
		const float Scale = View.SlotSizeScale > 0.f ? View.SlotSizeScale : 1.f;
		const FVector2D SlotSize = SkillSlotSize * Scale;

		// 分维判断：只想约束宽度时另一维传 0 即可，那一维就【没有覆盖】（透传子控件自报尺寸）。
		const bool bWantWidth = SlotSize.X > 0.f;
		if (SizeBox->IsWidthOverride() != bWantWidth
			|| (bWantWidth && !FMath::IsNearlyEqual(SizeBox->GetWidthOverride(), static_cast<float>(SlotSize.X))))
		{
			if (bWantWidth)
			{
				SizeBox->SetWidthOverride(static_cast<float>(SlotSize.X));
			}
			else
			{
				// Clear 而不是 Set(0)：SetWidthOverride(0) 是「宽度锁死为 0」（格子会消失），
				// ClearWidthOverride() 才是「这一维不干预」。
				SizeBox->ClearWidthOverride();
			}
		}

		const bool bWantHeight = SlotSize.Y > 0.f;
		if (SizeBox->IsHeightOverride() != bWantHeight
			|| (bWantHeight && !FMath::IsNearlyEqual(SizeBox->GetHeightOverride(), static_cast<float>(SlotSize.Y))))
		{
			if (bWantHeight)
			{
				SizeBox->SetHeightOverride(static_cast<float>(SlotSize.Y));
			}
			else
			{
				SizeBox->ClearHeightOverride();
			}
		}

		// 【外壳的显隐要跟着视图走】隐藏的槽位（FSkillSlotView::bHidden）由 ApplySlotView 设成
		// Collapsed，但 HorizontalBox 跳过的是它的【直接子控件】—— 现在是这个 SizeBox，不是里面的
		// SlotWidget。不同步的话，Collapsed 的槽位仍会留下左右各半个 SkillSlotSpacing 的空隙
		//（尺寸本身是 0：SBox 先看子控件的 Collapsed 再决定用不用覆盖值，SBox.cpp:126-139，
		//  所以这里只有 padding 漏出来）。
		//
		// SelfHitTestInvisible 是 USizeBox 构造函数自己设的值 —— 外壳不参与命中测试，但不挡子控件。
		const ESlateVisibility DesiredShellVisibility =
			View.bHidden ? ESlateVisibility::Collapsed : ESlateVisibility::SelfHitTestInvisible;
		if (SizeBox->GetVisibility() != DesiredShellVisibility)
		{
			SizeBox->SetVisibility(DesiredShellVisibility);
		}
	}

	// -------------------------------------------------------------------
	// ② 纵向对齐 + 间距：作用在【容器槽】上（不是 SizeBox 内部那层），且只对 HorizontalBox 做。
	// 其他容器（UniformGridPanel / WrapBox…）的槽位语义各不相同，C++ 不替它们猜。
	// -------------------------------------------------------------------
	if (UHorizontalBoxSlot* BoxSlot = Cast<UHorizontalBoxSlot>(ContainerSlot))
	{
		// 只有真的不同才写 —— SetVerticalAlignment 会让 Slate 重排一遍。
		if (BoxSlot->GetVerticalAlignment() != SkillSlotVerticalAlignment.GetValue())
		{
			BoxSlot->SetVerticalAlignment(SkillSlotVerticalAlignment.GetValue());
		}

		// 间距：左右各一半，相邻两格之间正好是 SkillSlotSpacing。
		// 只给一边的话，整排会朝另一边偏半个间距（末格右边空一截 / 首格左边空一截），
		// 看起来像是容器没对齐，而不是「有间距」。
		const FMargin DesiredPadding(FMath::Max(SkillSlotSpacing, 0.f) * 0.5f, 0.f);
		if (BoxSlot->GetPadding() != DesiredPadding)
		{
			BoxSlot->SetPadding(DesiredPadding);
		}
	}
}
