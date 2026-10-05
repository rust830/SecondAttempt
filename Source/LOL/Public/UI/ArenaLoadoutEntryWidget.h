// 装备栏里的一格：一件装备或一个海克斯。
//
// 【为什么和 UArenaChoiceCardWidget 不合成一个】两者长得像，但语义不同：
// 卡片是"可点的选项"（有按钮、有能不能点、点完要报下标），
// 这一格是"已经到手的东西"（没有任何交互，纯展示）。
// 合成一个类就要在里面放一个 bClickable 开关，然后到处判它 ——
// 那种类迟早会有人给一件已装备的东西绑上点击。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaLoadoutEntryWidget.generated.h"

class UImage;
class UTextBlock;

UCLASS(Abstract)
class LOL_API UArenaLoadoutEntryWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 灌一份视图。全量覆盖，同一份重复灌是空操作（理由同 UArenaChoiceCardWidget::ApplyCard）。 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void ApplyEntry(const FArenaLoadoutEntryView& InEntry);

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	FArenaLoadoutEntryView GetEntry() const { return Entry; }

	/** 蓝图侧的实现入口（海克斯加一圈光效、新到手的播一个弹入…）。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|UI")
	void BP_OnEntryChanged(const FArenaLoadoutEntryView& InEntry);

protected:
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> NameText;

	/** 说明文字（鼠标悬停的浮层也归它，怎么做在蓝图里）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> DescriptionText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UImage> IconImage;

	/** 海克斯角标之类。bIsAugment 为 true 时显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> AugmentMark;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FArenaLoadoutEntryView Entry;
};
