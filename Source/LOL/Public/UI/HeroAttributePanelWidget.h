// 属性面板：一排「图标 + 数值」的小格，按一个键展开显示全部属性。
//
// 头文件零 GAS include。它只认 FHeroAttributePanelView —— 哪条属性、值怎么算、按什么顺序，
// 全部由 Controller 翻译好。面板自己只做两件事：建行、切形态。
//
// 【两个形态：同一个容器，靠"发号顺序"把展开的那些排到上面】
//   常驻：只显示 bShowInCompact 的那几条（用户要的 8 条）。
//   展开：另外那些也显示出来，并且【排在常驻那几条之前】—— 于是它们出现在常驻条上方，
//         常驻条原地不动（只是被推到下面）。
// 分组依据是 FHeroAttributeEntryView::bShowInCompact。形态切换是 ToggleExpanded()。
//
// 【为什么是"两趟发号"而不是"加第二个容器"】
//   网格是【显式定位】的（行/列号说了算），不是流式顺序 —— 所以"谁在上面"完全由发号顺序决定，
//   两趟就够：展开组先吃号，常驻组后吃号。
//   加第二个容器看着更"结构清晰"，但代价是根得从网格换成一个 Box（面板自身的尺寸/锚定跟着变），
//   换来的"两组各配列数/尺寸"又正好和"所有行一样宽"的需求相抵 —— 不划算。
//   收起态下第一趟是空跑（那些行都是 Collapsed），发号结果和加这功能之前【完全一致】。
//
// 【行是运行时建的】和技能槽一样：设计师在设计器里看不到它们，所以行尺寸只能由
// EntrySize（C++ 包一层 SizeBox）或 WBP 里根控件的 SizeBox 给 —— 完整原理见
// GAS_HUD_Setup.md §12.5「槽位尺寸」（HorizontalBox 上根控件不自报尺寸就挤成 0 宽那个坑）。
//
// 【分列】常驻那几条要分两列的话，把 EntryContainer 换成 UniformGridPanel（列数由 NumColumns
// 给）。VerticalBox 做不到 —— 它的语义就是「一个孩子 = 一整行」，槽位只有 Size / Padding /
// HAlign 三个旋钮，没有列的概念。WrapBox 也能分列，但列数由宽度决定，不固定。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/HUDTypes.h"
#include "HeroAttributePanelWidget.generated.h"

class UHeroAttributeEntryWidget;
class UPanelWidget;
class UUniformGridSlot;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnAttributePanelExpandedChanged, bool, bInExpanded);

UCLASS(Abstract)
class LOL_API UHeroAttributePanelWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 展开 / 收起。按键和蓝图都走这个入口（幂等地翻转）。 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void ToggleExpanded();

	UFUNCTION(BlueprintCallable, Category = "HUD")
	void SetExpanded(bool bInExpanded);

	UFUNCTION(BlueprintPure, Category = "HUD")
	bool IsExpanded() const { return bExpanded; }

	/** 展开状态翻转（真翻转才响）。HUD 拿它去同步背景框高度。 */
	UPROPERTY(BlueprintAssignable, Category = "HUD")
	FOnAttributePanelExpandedChanged OnExpandedChanged;

	/** 当前视图。返回拷贝而不是 const& —— UFUNCTION 的返回值必须是值类型。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	FHeroAttributePanelView GetPanelView() const { return PanelView; }

	/**
	 * 面板整体换了一份数据（C++ 已经把内置的行更新完了）。
	 *
	 * 需要额外表现（展开时的一个动画、某一行高亮）挂这条。注意它是【每次属性变化】都会调的，
	 * 不是只在展开时调 —— 想只在展开瞬间做事就挂 BP_OnExpandedChanged。
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnPanelChanged(const FHeroAttributePanelView& View);

	/** 展开状态翻转。首次拿到视图时如果和 WBP 默认一致就不播（避免开局闪一下）。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnExpandedChanged(bool bInExpanded);

	/**
	 * 全量覆盖。HUD 根 Widget 调它（订阅即拉取的那一份快照也走这里）。
	 *
	 * 不叫 RefreshFromHUD 之类的：面板【不主动去拉数据】—— 那是 HUD 根 Widget 的职责，
	 * 面板连 Controller 是哪来的都不知道（同技能槽：只认视图）。
	 */
	void ApplyPanelView(const FHeroAttributePanelView& View);

protected:
	/** 行 Widget 的父容器（VerticalBox / UniformGridPanel / WrapBox 都行）。留空就只更新数据、不建行。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> EntryContainer;

	/** 行 Widget 类（WBP_AttributeEntry）。留空就不自动建行。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	TSubclassOf<UHeroAttributeEntryWidget> EntryWidgetClass;

	/**
	 * 每一行的尺寸（像素）。**零 = 不干预**，完全按 WBP_AttributeEntry 根控件的自报尺寸走。
	 *
	 * 和 UHeroHUDWidget::SkillSlotSize 是同一个坑、同一个解法（宽 / 高分开判，<=0 的那一维不覆盖）：
	 * 行的根控件如果是 CanvasPanel / Overlay 这类不自报尺寸的，塞进容器后会挤成 0。
	 * 详细原理见 GAS_HUD_Setup.md §12.5。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	FVector2D EntrySize = FVector2D::ZeroVector;

	/** 行之间的间距（像素）。零 = 挨着。同样因为行是运行时建的，只能在 C++ 侧给。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	float EntrySpacing = 4.f;

	/**
	 * 面板按几列排。**仅当 EntryContainer 是 UniformGridPanel 时有效**，默认 2
	 *（= 常驻那几条分成两列）。别的容器（VerticalBox 一列到底 / WrapBox 按宽度自己流）不看它。
	 *
	 * 【展开态和常驻态共用这一个值】—— 两组本来就在同一个网格里、是同一套格子。
	 * 想让两组各配一套列数就得拆成两个容器，那要改根结构（见文件头那段说明）。
	 *
	 * 【为什么列数只能在 C++ 侧给】
	 *   1. 行是运行时建的，设计器里看不到它们，也就没地方填；
	 *   2. UE 5.8 的 UUniformGridPanel **没有列数这个属性** —— 面板上只有 SlotPadding /
	 *      MinDesiredSlotWidth / MinDesiredSlotHeight。「几列」是靠每个孩子自己的行/列号
	 *      隐含出来的（ColumnFill 是另一个控件 UGridPanel 上的东西，别搞混）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD", meta = (ClampMin = "1"))
	int32 NumColumns = 2;

	/**
	 * 展开时要不要把每行切成「宽松形态」（一般会显示属性名）。
	 *
	 * **默认 false**：展开【只多出一块行】，行自己的宽高一点不变 —— 也就是"所有行一样宽"。
	 * 形态固定还顺带避掉一个副作用：常驻条本来就在屏幕上，展开时它跟着变宽，看起来像整块在跳。
	 *
	 * 设 true 就是旧行为：展开时所有行一起变宽（走 UHeroAttributeEntryWidget::SetCompact(false)）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	bool bUseRelaxedRowsWhenExpanded = false;

	/** 当前视图（蓝图只读）。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	FHeroAttributePanelView PanelView;

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	bool bExpanded = false;

	/** WBP 里配的初始展开状态。第一次拿到视图时用它决定要不要播 BP_OnExpandedChanged。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	bool bStartExpanded = false;

private:
	/** 行 Widget 的懒创建：不存在就建一个塞进容器。返回 null 表示这条建不出来（配置坏了）。 */
	UHeroAttributeEntryWidget* EnsureEntryWidget(int32 EntryIndex);

	/** 按当前 PanelView + bExpanded 决定每一行的可见性和形态。 */
	void ApplyRowVisibility();

	/**
	 * 按【可见顺序】给行重新发行号 / 列号。非 UniformGridPanel 容器是空操作。
	 *
	 * 为什么必须重排：格子是显式定位的（行/列号说了算），不是流式排的。直接拿条目下标算
	 * 行列（Row = Index / Columns）会得到一片空洞 —— 收起的那几行还在，只是不画出来，
	 * 格子照样被它们占着。
	 *
	 * 【两趟发号 = "展开的在上面"】第一趟只给「展开才出现的那些」（bShowInCompact == false）
	 * 发号，第二趟才给常驻那几条发号 —— 于是展开行占住上面的行、常驻条被推到下面。
	 * 收起态第一趟是空跑，结果和没有这个功能时完全一致。
	 */
	void ReflowRows();

	/** 下标 = 条目下标，中间的空位留 nullptr —— 不要用 Add 之外的方式打乱这个对应关系。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UHeroAttributeEntryWidget>> EntryWidgets;

	/**
	 * 下标 = 条目下标；**只有 UniformGridPanel 容器会填**（其他容器的位置由流式排列决定，
	 * 没有行/列号可发）。和 EntryWidgets 一样是平行数组，同样不要打乱。
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UUniformGridSlot>> EntryGridSlots;

	/** 有没有拿到过视图。第一次不播 BP_OnExpandedChanged（没有「上一个形态」可言）。 */
	bool bHasAppliedView = false;
};
