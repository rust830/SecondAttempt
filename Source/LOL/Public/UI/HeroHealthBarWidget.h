// 一条资源条（血条或能量条）。同一个类两个实例，bIsEnergyBar 决定显示哪一对数值。
//
// 头文件零 GAS include：只认 FHUDVitalsView。数值从哪来、怎么算的，它一概不知道。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/HUDTypes.h"
#include "HeroHealthBarWidget.generated.h"

class UProgressBar;
class UTextBlock;

UCLASS(Abstract)
class LOL_API UHeroHealthBarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 蓝图侧的实现入口。C++ 已经填好了 Bar / NumericText，这里只做额外表现（描边、脉冲、分段线…）。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnVitalsChanged(const FHUDVitalsView& Vitals, float Percent, float Current, float Max);

	/** HUD 调这个。 */
	void ApplyVitals(const FHUDVitalsView& Vitals);

	/** 兼容「血条和自己不是同一个 Widget 树」的用法：直接问当前百分比。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	float GetLastPercent() const;

protected:
	/** true = 显示能量，false = 显示生命。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bIsEnergyBar = false;

	/** 低于这个值就换成 LowFillColor。<= 0 表示不启用（能量条一般不开）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	float LowValueThreshold = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FLinearColor FillColor = FLinearColor(0.20f, 0.72f, 0.28f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FLinearColor LowFillColor = FLinearColor(0.88f, 0.18f, 0.14f, 1.f);

	/** 是否让 C++ 直接往 NumericText 写 "123 / 456"。关掉的话蓝图自己排。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bShowNumericText = true;

	// -----------------------------------------------------------------------
	// 可选绑定：WBP 里的控件名字对上就自动接上，对不上也不报错。
	// 用 BindWidgetOptional 而不是 BindWidget —— HUD 的排版会反复改，
	// 强制绑定会让「改名字」变成「编译期报错 + 打不开蓝图」，代价比收益大。
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UProgressBar> Bar;

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> NumericText;

private:
	FHUDVitalsView LastVitals;
};
