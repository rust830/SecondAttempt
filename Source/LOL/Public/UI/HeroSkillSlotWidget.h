// 单个技能槽：图标 / 冷却转圈 / 剩余秒数 / 灰化。
//
// 头文件零 GAS include。它只认两样东西：SlotIndex（第几个槽）和 FSkillSlotView（怎么显示）。
// 图标的来源、冷却的秒数从哪算出来、为什么灰 —— 一概不知道，也不该知道。
//
// 一条容易写错的规则：转圈的开关是 ShouldShowCooldown()（= 标签说在冷却 + 有总时长），
// 【不是】State == Cooled。两者不等价：死亡时 State 是 Greyed，但冷却数字照常要走。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/HUDTypes.h"
#include "HeroSkillSlotWidget.generated.h"

class UImage;
class UProgressBar;
class UTextBlock;

UCLASS(Abstract)
class LOL_API UHeroSkillSlotWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** HUD 建完槽之后调一次，告诉它自己是第几个。 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void SetSlotIndex(int32 InSlotIndex);

	UFUNCTION(BlueprintPure, Category = "HUD")
	int32 GetSlotIndex() const { return SlotIndex; }

	/** 当前视图。返回拷贝而不是 const& —— UFUNCTION 的返回值必须是值类型。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	FSkillSlotView GetSlotView() const { return SlotView; }

	/** 冷却秒数的显示文案。口径统一在 HeroHUD::FormatCooldownSeconds，别在蓝图里另写一套。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	FText GetCooldownLabel() const { return HeroHUD::FormatCooldownSeconds(SlotView.CooldownRemaining); }

	/** 蓝图侧的实现入口。C++ 已经把下面几个控件填好了，这里做额外表现（可用时闪一下、CD 结束音效…）。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnSlotViewChanged(const FSkillSlotView& View);

	/** HUD 调这个。全量覆盖，不做增量 —— 增量是 bug 的温床，而这里的数据量小到没必要。 */
	void ApplySlotView(const FSkillSlotView& View);

protected:
	// ---- 可选绑定：名字对上就自动接 ----

	/** 冷却转圈。用 radial fill 材质，Percent 直接喂给它（1 = 刚进 CD，0 = 好了）。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UProgressBar> CooldownRing;

	/** 剩余秒数文本。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> CooldownText;

	/** 键位标注（Q / W / E / R / D / F），文本来自映射表。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> KeyLabelText;

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> IconImage;

	/** 灰化遮罩（半透明黑）。只在 Greyed / Disabled 时显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> GreyedOverlay;

	// ---- 表现参数 ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FLinearColor ReadyIconTint = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FLinearColor BlockedIconTint = FLinearColor(0.35f, 0.35f, 0.35f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bShowCooldownText = true;

	// ---- 当前视图（蓝图只读） ----

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	FSkillSlotView SlotView;

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	int32 SlotIndex = INDEX_NONE;
};
