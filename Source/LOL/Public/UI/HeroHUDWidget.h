// HUD 根容器。只做四件事：订阅 Controller、拉一次快照、按快照建槽、把变化转给子 Widget 和蓝图。
//
// 头文件零 GAS include。这里连 Controller 的头文件都不 include（只前置声明），
// 因为 UHeroHUDController 在 GAS/ 下、携带 FGameplayTag 等类型 —— 那个依赖只允许出现在 .cpp 里。
//
// 「订阅即拉取」的落点就在这里：NativeConstruct 先订阅、再 PullHUDState()。
// 于是 Widget 的创建时机和 Controller 的绑定时机彻底解耦，
// 不依赖「Controller 记得在正确时机 BroadcastInitialValues()」——
// 那条路一定会漏，因为两个时间轴是独立的（Widget 可能建在绑定之前，也可能建在之后）。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Types/SlateEnums.h"
#include "UI/HUDTypes.h"
#include "HeroHUDWidget.generated.h"

class UHeroAttributePanelWidget;
class UHeroHealthBarWidget;
class UHeroHUDController;
class UHeroSkillSlotWidget;
class UHeroTargetFrameWidget;
class UPanelSlot;
class UCanvasPanelSlot;
class UImage;
class UPanelWidget;

UCLASS(Abstract)
class LOL_API UHeroHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UHeroHUDWidget(const FObjectInitializer& ObjectInitializer);

	/**
	 * 手动重拉一次（比如切关卡后重新 AddToViewport，或者 Controller 当时还没就绪）。
	 *
	 * 【它同时是「配置改了立刻生效」的入口】会先让 Controller 按【当前】配置重算一遍每个
	 * 槽位的静态表现（图标 / 键位 / 名字 / 种类 / SlotSizeScale），再拉一份全量快照。
	 * 于是在 PIE 运行中改 UHeroHUDSlotConfig、或者改这个 Widget 上的 SkillSlotSize /
	 * SkillSlotVerticalAlignment / SkillSlotSpacing，调一次它就能看到结果，不用重启。
	 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void RefreshFromController();

	/**
	 * 展开 / 收起属性面板。按键（ALOLPlayerController）调它，蓝图也能调。
	 *
	 * 【为什么入口开在 HUD 根 Widget 而不是 PC 直接调面板】面板是 WBP_HUD 里的一个子控件，
	 * 外面看不见它（BindWidgetOptional 是 protected）。让根 Widget 转一手，
	 * 「按键 → 谁」这条线就只有一处，而且没挂面板时这里是安全的空操作。
	 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void ToggleAttributePanel();

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	// -----------------------------------------------------------------------
	// 蓝图实现入口。C++ 已经把内置的子 Widget 推好了，这几个只做额外表现。
	// -----------------------------------------------------------------------

	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnVitalsChanged(const FHUDVitalsView& Vitals);

	/** 目标框额外表现。C++ 已把名字 / 血 / 能量 / 死亡压暗 / 显隐填好，这里做选中动画、边框等。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnTargetFrameChanged(const FTargetFrameView& View);

	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnHUDReady(bool bBound);

	/** 槽位变化。六槽的 C++ 子 Widget 已经在更新了，这里是补充表现。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnSkillSlotChanged(int32 SlotIndex, const FSkillSlotView& View);

	/** 属性面板变化。面板的 C++ 子 Widget 已经在更新了，这里是补充表现。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnAttributesChanged(const FHeroAttributePanelView& Panel);

	// -----------------------------------------------------------------------
	// 可选绑定：名字对上就自动接。血条 / 能量条可以留空，蓝图自己处理。
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroHealthBarWidget> HealthBar;

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroHealthBarWidget> EnergyBar;

	/** 目标框。留空的话目标框完全由蓝图自己搭（退回旧行为）。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroTargetFrameWidget> TargetFrame;

	/**
	 * 【兜底路径】WBP 里没挂 TargetFrame 控件时，用这个类在运行时补一个目标框。
	 *
	 * 【为什么需要它】BindWidgetOptional 的规则是"名字对上就接"—— 也就是说目标框要出现在
	 * 屏幕上，必须有人往 WBP_HUDWidget 的控件树里摆一个 WBP_HeroTarget，而控件树只能手搭
	 * （Python 建不出来）。于是很容易出现"目标框的 C++ 全都在、数据也推过去了，屏幕上却
	 * 什么都没有"：缺的不是代码，是那一个控件。配了这个类，HUD 建起来时 C++ 自己补上。
	 *
	 * 【优先级】WBP 里挂了就以 WBP 为准（设计师要摆位 / 加边框都随意），这里一个字都不做。
	 * 所以这条兜底和"以后在 WBP 里正经摆一个"【不冲突】，只是让现在没有它也能用。
	 *
	 * 【配在哪】WBP_HUDWidget 上：TargetFrameWidgetClass = WBP_HeroTarget。
	 * 留空 = 不要兜底（目标框只认 WBP 里的控件）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	TSubclassOf<UHeroTargetFrameWidget> TargetFrameWidgetClass;

	/**
	 * 属性面板（常驻条 + 按 C 展开）。**WBP_HUDWidget 里那个控件名必须精确叫 AttributePanel**。
	 * 留空 = 没有属性面板（不报错，其余部分照常）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroAttributePanelWidget> AttributePanel;

	/**
	 * 【属性区背景框】—— 属性展开时它要跟着变高（收起 96 / 展开 168）。
	 * 在 CanvasPanel_16 下、childIndex 0（画在最底层）；左下锚定 + auto_size=False，
	 * 高度由 ApplyAttributeBackdropHeight() 按展开状态写进槽位的 Offsets。
	 * **WBP_HUDWidget 里那个控件名必须精确叫 AttributeBackdrop**。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> AttributeBackdrop;

	/** 属性区背景框的收起高度（= 常驻 4 行 × 每行 24）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	float AttributeBackdropHeight_Collapsed = 96.f;

	/** 属性区背景框的展开高度（= 7 行 × 24；上限跟面板 SizeBox 的 MaxDesiredHeight 对齐）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	float AttributeBackdropHeight_Expanded = 168.f;

	/**
	 * 按展开状态把背景框的高度写进它的 CanvasPanel 槽位。
	 * 【为什么只写高度】背景框和面板都是【左下锚定、底边钉在同一处】，所以只要高度相等就永远齐平
	 * —— 底边不动、上边一起抬。宽度/左右位置不归它管（那是 WBP 里摆好的）。
	 */
	void ApplyAttributeBackdropHeight(bool bExpanded);

	/** 收到面板的展开状态变化 → 同步背景框高度（面板的 OnExpandedChanged 绑到它）。 */
	UFUNCTION()
	void HandleAttributePanelExpanded(bool bInExpanded);

	/** 技能槽的父容器（HorizontalBox / UniformGridPanel 都行）。留空就不自动建槽。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> SkillSlotContainer;

	/** 槽位 Widget 类（WBP_SkillSlot）。留空就不自动建槽。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	TSubclassOf<UHeroSkillSlotWidget> SkillSlotWidgetClass;

	/**
	 * 每个槽位的尺寸（像素）。**零 = 不干预**，完全按 WBP_SkillSlot 根控件的自报尺寸走。
	 *
	 * 【为什么需要它 —— 这是 HorizontalBox 上最容易踩的一个坑】
	 * WBP_SkillSlot 的根如果是 CanvasPanel / Overlay / SizeBox 之类的「不自报尺寸」控件，
	 * 塞进 HorizontalBox 之后它报的 desired size 是 0：表现是**六格挤成 0 宽**（或者被拉伸得
	 * 完全对不上设计）。这是 UMG 的既定行为，不是 bug —— HorizontalBox 的槽位规则
	 *（Automatic = 用子控件自报的尺寸）拿不到尺寸时就是 0。
	 *
	 * 两个解法，任选：
	 *   ① 在 WBP_SkillSlot 里把根做成自报尺寸的控件（Image 用 brush 尺寸、或 SizeBox 定死宽高）；
	 *   ② 在这里填一个尺寸，C++ 会替每一格包一层 SizeBox（宽 / 高分开判，<=0 的那一维不覆盖）。
	 *
	 * ②的好处是**改布局不用动 WBP**，而且是运行时建槽这条路上唯一能在 C++ 侧控制的点
	 *（容器是 WBP 里画的，但槽位是 C++ 建的，设计师在设计器里看不到它们）。
	 * 这个坑在 s1mpleFps 的手雷槽位上已经踩过一次，那里用的就是同样的 SizeBox 包法。
	 *
	 * 【这个值 = 屏幕上的实际尺寸】它和每个槽位的倍率（FSkillSlotView::SlotSizeScale）一起
	 * 由 ApplySlotLayout 在每次视图变化时同步一次 —— 所以 PIE 里改它、或者改完调
	 * RefreshFromController()，已有的格子会立刻跟着变，不需要重启。
	 *
	 * 【和 WBP 里那个 64 的关系】槽位 WBP 的根是一个 SizeBox（Width/Height Override = 64）。
	 * 那 64 从此【只表示设计基准】—— C++ 会在外面再包一层 `ScaleBox(ScaleToFit)`，
	 * 把整块设计画面【等比缩放】到这里给的实际尺寸。所以两个数不相等也没关系（这个值改大了
	 * 画面跟着放大）：一个说"画多大"，另一个说"按什么比例画"。见 EnsureSlotWidget 里那段注释。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	FVector2D SkillSlotSize = FVector2D::ZeroVector;

	/**
	 * 技能槽之间的间距（像素）。**零 = 挨着**。
	 *
	 * 【为什么也只能在 C++ 里给】同 SkillSlotSize：槽位是运行时建的，设计器里根本看不到它们，
	 * HorizontalBox 的槽位 padding 没有地方可填 —— 你现在看到「六格贴在一起」就是这个原因
	 * （WBP 里的 HorizontalBox 是空的，摆位全靠 C++ 建槽时给）。
	 *
	 * 实现是每格左右各留一半：相邻两格的间隙 = SkillSlotSpacing（左右两半相加），
	 * 首尾两格各多出半个 —— 那半个就是「离外面那圈边框的距离」的一部分，
	 * 边框自己还能再加一层（见 WBP 那边 Border 的 Padding）。
	 *
	 * 和 SkillSlotSize 一样会回流（见 ApplySlotLayout），改完不用重启。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	float SkillSlotSpacing = 8.f;

	/**
	 * 槽位在容器里的【纵向对齐】。默认居中，也就是「按格子中心线对齐」。
	 *
	 * 【为什么这是个要暴露出来的选项 —— 它不是"好不好看"，是语义选择】
	 * HBox 槽位的纵向对齐由引擎这么算（`LayoutUtils.h` → `AlignChild<Orient_Vertical>`）：
	 *   上边距 = (容器高 - 槽高) / 2        ← VAlign_Center
	 *   上边距 = 0                          ← VAlign_Top
	 *   上边距 = 容器高 - 槽高              ← VAlign_Bottom
	 * 注意分母是【容器高】、分子是【槽高】，跟 FSlateChildSize（SetSize(Automatic)）无关 ——
	 * 那个只管主轴（横向）。
	 *
	 * 于是倍率不同的格子（比如被动填了 0.8）在默认居中下**中心线是齐的、上下边缘必然不齐**：
	 * 小的那格上下各缩进 10%。要「小格子的下边缘和大格子对齐」（很多 MOBA 的角色栏是这么摆的），
	 * 就得选 VAlign_Bottom，不是 Center。
	 *
	 * 其他容器（UniformGridPanel / WrapBox…）的槽位语义各不相同，C++ 不替它们猜 —— 这个值
	 * 只在容器是 HorizontalBox 时生效。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	TEnumAsByte<EVerticalAlignment> SkillSlotVerticalAlignment = VAlign_Center;

	// ---- 委托回调。必须是 UFUNCTION 才能 AddDynamic。 ----

	UFUNCTION()
	void HandleVitalsChanged(const FHUDVitalsView& Vitals);

	UFUNCTION()
	void HandleSkillSlotChanged(int32 SlotIndex, const FSkillSlotView& View);

	UFUNCTION()
	void HandleTargetFrameChanged(const FTargetFrameView& View);

	UFUNCTION()
	void HandleAttributesChanged(const FHeroAttributePanelView& Panel);

	UFUNCTION()
	void HandleHUDReady(bool bBound);

private:
	void BindToController();
	void UnbindFromController();
	void ApplySnapshot(const FHUDSnapshot& Snapshot);

	/**
	 * TargetFrame 为空、而 TargetFrameWidgetClass 有配时，运行时建一个加进视口。
	 * 必须在第一次 HandleTargetFrameChanged（也就是 ApplySnapshot）之前调，见属性上的注释。
	 */
	void EnsureTargetFrameWidget();

	/**
	 * 槽位 Widget 的懒创建：不存在就建一个、包上壳子塞进容器。
	 *
	 * 【只管建，不管尺寸】壳子是 `SizeBox`（定实际尺寸）→ `ScaleBox`（把 WBP 里的设计画面
	 * 等比缩放到那个尺寸），里面的值统一由 ApplySlotLayout 每次视图变化时同步
	 * ——「建结构」和「定尺寸」彻底分开，才不会出现「配置改了但结构没重建，于是新值没人去用」
	 * 这种只在一半路径上生效的 bug。
	 */
	UHeroSkillSlotWidget* EnsureSlotWidget(int32 SlotIndex);

	/**
	 * 从槽位 widget 往上走到【挂在 SkillSlotContainer 上】的那个槽位。
	 *
	 * 【为什么必须"往上走"而不是写死层数】槽位外面包了几层壳是实现细节（现在是
	 * SizeBox → ScaleBox），而 `SlotWidget->Slot` 拿到的是【最内层】壳的槽，不是容器槽 ——
	 * 这个假设已经错过两次，而且每次都是静默失败（Cast 返回 null，if 体永远不执行，
	 * 表现就是"改了没反应"）。所以改成一路往上走到 `Parent == SkillSlotContainer` 为止，
	 * 层数怎么变都成立，一个壳都没包上（直接挂在容器上）也成立。
	 *
	 * @return 容器槽；槽位不是挂在 SkillSlotContainer 下时返回 nullptr。
	 */
	UPanelSlot* FindContainerSlot(const UHeroSkillSlotWidget& SlotWidget) const;

	/**
	 * 把视图里的【尺寸 / 纵向对齐 / 间距】落到已经建好的那个槽位上。
	 *
	 * 【为什么不能在建格时算一次就完事】
	 * 这三样都取决于 Widget 上的配置（SkillSlotSize / SkillSlotVerticalAlignment /
	 * SkillSlotSpacing），其中尺寸还额外乘了 FSkillSlotView::SlotSizeScale（来自
	 * UHeroHUDSlotConfig 这个 DataAsset）。DataAsset 是运行期会被改的 —— 建格时算一次写死，
	 * 改完配置已有的格子就没有任何人会去更新它，表现就是「改了没反应」。
	 *
	 * 【和外面那层 ScaleBox 的分工】这个函数只决定"盒子多大"（外层 SizeBox 的宽高覆盖）；
	 * 盒子里那块设计画面【等比缩放】到盒子大小是 ScaleBox 的事（`Stretch=ScaleToFit`），
	 * 不需要每次重算 —— 见 EnsureSlotWidget 里那段注释。
	 *
	 * 【必须幂等】冷却心跳（UHeroHUDController::SampleCooldowns）的 30Hz 广播走的是同一条
	 * 派发路径，所以冷却期间这个函数每秒会被调一百多次。因此它每次都先读 Slate 上当前的值，
	 * 只有真的不一样才写回去 —— SetWidthOverride / SetVerticalAlignment 都会打一次 Layout
	 * 失效，白写就是白让 Slate 重排一遍。
	 */
	void ApplySlotLayout(int32 SlotIndex, const FSkillSlotView& View);

	/** Controller 还没就绪时的有界重试（PC 还没建 Controller 的情况）。 */
	void StartControllerRetry();
	void StopControllerRetry();

	UPROPERTY(Transient)
	TObjectPtr<UHeroHUDController> HUDController;

	/** 下标 = 槽位号，中间的空位留 nullptr —— 不要用 Add 之外的方式打乱这个对应关系。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UHeroSkillSlotWidget>> SlotWidgets;

	FTimerHandle ControllerRetryHandle;
	int32 ControllerRetryCount = 0;

	/** 0.1s × 20 次 = 2 秒还拿不到 Controller，就认为这个 Widget 不是 PC 建的。 */
	static constexpr float ControllerRetryInterval = 0.1f;
	static constexpr int32 MaxControllerRetries = 20;
};
