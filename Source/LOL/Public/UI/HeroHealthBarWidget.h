// 一条资源条（血条或能量条）。同一个类两个实例，bIsEnergyBar 决定显示哪一对数值。
//
// 头文件零 GAS include：只认 FHUDVitalsView。数值从哪来、怎么算的，它一概不知道。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/HUDTypes.h"
#include "HeroHealthBarWidget.generated.h"

class UImage;
class UMaterialInstanceDynamic;
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

	/**
	 * 这条是血条还是能量条。
	 *
	 * 存在的理由是「同一个类两个实例」这件事需要一个【从外面问得出来】的答案：
	 * 目标框（UHeroTargetFrameWidget）要在一棵别人的控件树里认出血条和能量条，
	 * 而它既不是这个类的子类、也不是朋友，读不到 protected 的 bIsEnergyBar。
	 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	bool IsEnergyBar() const { return bIsEnergyBar; }

	/** 每帧把白条残影向当前比例缓动（Ghost 追 Fill），并写进 FillImage 的材质。 */
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

protected:
	/** true = 显示能量，false = 显示生命。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bIsEnergyBar = false;

	/**
	 * 低于这个比例就换成 LowFillColor。0~1 的比例，不是绝对数值 —— 0.3 表示血量低于 30%。
	 * <= 0 表示不启用（能量条一般不开）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD",
		meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
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

	/**
	 * 承载血条/蓝条材质的 Image（Brush = M_BarFill，材质里用 Fill / Ghost 两个标量参数）。
	 * **WBP 里那个控件名必须精确叫 FillImage**。
	 * 留空 = 没有材质条，只显示 ProgressBar 自己的填充。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> FillImage;

	/** 白条残影的当前比例（0~1）。它在 NativeTick 里向 Percent 缓动 —— 这是 LoL 掉血的手感。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	float GhostPercent = 1.f;

private:
	FHUDVitalsView LastVitals;

	/**
	 * 数字文本上一次写进去的那两个整数。
	 *
	 * 存在的唯一理由是省掉重复的文本构造：`FText::Format` + 两次 `FText::AsNumber`
	 * 是每次属性变化都要跑一整遍的（ICU 数字格式化 + 新建 FText）。头顶血条是
	 * 「每个敌人一个」，小兵群吃一发 AOE 就是 N 条同时刷 —— 而血条的整数显示
	 * 绝大多数时候根本没变（1 点伤害改不动整数部分）。所以先比整数，一样就整块跳过。
	 *
	 * INDEX_NONE = 还没写过（第一次必须写）。
	 */
	int32 LastNumericCurrent = INDEX_NONE;
	int32 LastNumericMax = INDEX_NONE;

	/** "x / y" 的格式模板。NSLOCTEXT 查表 + 解析也只做一次。 */
	FText NumericFormatPattern;
};
