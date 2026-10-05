// 属性面板上的【一行】：小图标 + 数值（+ 可选的属性名）。
//
// 头文件零 GAS include。它只认一样东西：FHeroAttributeEntryView —— 图标选好了、数值已经是
// 格式化好的文本了。属性叫什么、值怎么算、百分号从哪来，一概不知道，也不该知道。
//
// 【两种形态】常驻态（紧凑）和展开态。C++ 只告诉你「现在是哪种」，长什么样是 WBP 的事 ——
// 见 SetCompact 的注释（这里踩过一次可见性的坑）。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/HUDTypes.h"
#include "HeroAttributeEntryWidget.generated.h"

class UImage;
class UTextBlock;

UCLASS(Abstract)
class LOL_API UHeroAttributeEntryWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 当前视图。返回拷贝而不是 const& —— UFUNCTION 的返回值必须是值类型。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	FHeroAttributeEntryView GetEntryView() const { return EntryView; }

	/** 现在是紧凑（常驻）形态吗。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	bool IsCompact() const { return bCompact; }

	/**
	 * 形态切换。**C++ 不改任何控件的可见性**，只广播一条事件让 WBP 自己决定。
	 *
	 * 【为什么】和技能槽那边的 AuthoredVisibility 是同一条教训：控件的可见性是设计师在
	 * WBP 里配的（可能是 HitTestInvisible / SelfHitTestInvisible，可能故意留空位），
	 * C++ 写死 Visible 会把设计意图冲掉，而且这种冲突只在运行时看得见、编辑器里一切正常。
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnCompactChanged(bool bInCompact);

	/** 视图变化（数值、图标、名字）。值没变就不播 —— 一次属性变化只该有一行响。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnEntryChanged(const FHeroAttributeEntryView& View);

	/**
	 * 全量覆盖，不做增量 —— 增量是 bug 的温床，而这里的数据量小到没必要。
	 * 面板调这个，不要在蓝图里自己去 SetText。
	 */
	void ApplyEntry(const FHeroAttributeEntryView& View);

	/** 面板调这个。值没变就不会重复播事件（形态切换同理）。 */
	void SetCompact(bool bInCompact);

protected:
	// ---- 可选绑定：名字对上就自动接 ----

	/** 小图标。留空的话这一行完全不画图标（蓝图自己摆的东西不受影响）。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> IconImage;

	/** 数值文本。C++ 只写这里，且写的是已经格式化好的 FText。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ValueText;

	/** 属性名文本（"攻击力"）。**可以留空** —— 常驻态一般只看图标 + 数值。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> NameText;

	/** 当前视图（蓝图只读）。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	FHeroAttributeEntryView EntryView;

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	bool bCompact = false;

	/**
	 * WBP 里给 IconImage 配的可见性（首次应用时记下来）。
	 *
	 * 图标留空时要把它收起来，但【不能】反过来无条件写 Visible：
	 * 那个值属于 WBP，和上面 SetCompact 是同一条纪律。
	 */
	ESlateVisibility AuthoredIconVisibility = ESlateVisibility::Visible;

	bool bAuthoredIconVisibilityCaptured = false;
};
