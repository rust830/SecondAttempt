// 目标框（当前选中的敌人 / 单位）。头文件零 GAS include —— 数据由 UHeroHUDController 推过来。
//
// 这里只管「怎么显示」，不管「值从哪来」：名字、血、能量、死亡压暗、显隐全在 C++，
// 蓝图只负责摆控件和配布局（放哪、多宽、什么字体）。
//
// 血条 / 能量条复用 UHeroHealthBarWidget —— 和 WBP_HUD 里的是同一套 FHUDVitalsView 契约，
// bIsEnergyBar 决定它们各自显示哪一对数值。目标框不需要重新发明一条渲染路径。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/HUDTypes.h"
#include "HeroTargetFrameWidget.generated.h"

class UHeroHealthBarWidget;
class UImage;
class UTextBlock;

UCLASS(Abstract)
class LOL_API UHeroTargetFrameWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** HUD 调这个。全量覆盖，不做增量 —— 目标框一次变化就是一次完整状态。 */
	void ApplyTargetFrame(const FTargetFrameView& View);

	/** 当前视图。返回拷贝而不是 const& —— UFUNCTION 的返回值必须是值类型。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	FTargetFrameView GetTargetFrame() const { return TargetFrame; }

	/** 蓝图侧的实现入口。C++ 已经把名字 / 血 / 能量 / 死亡压暗填好了，这里做额外表现（选中动画、边框…）。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnTargetFrameChanged(const FTargetFrameView& View);

protected:
	/**
	 * 【按类型兜底】把树里那几个还没接上的控件找回来。
	 *
	 * 【为什么需要】BindWidgetOptional 的规则是"名字对上才接"，这对"改个名字不该报错"是对的，
	 * 但它有一个静默的代价：WBP 里拖进去的血条默认叫 `WBP_HealthBar`（不是 `HealthBar`），
	 * 于是控件明明在树里、视图也推过来了，就是没人接 —— 表现是"目标框出来了，里面空的"。
	 * 名字对不上不报错，所以这件事没有任何提示，只能靠这里补一条路。
	 *
	 * 【为什么按"血 / 能量"分而不是随便挑一个】这个类的两个条是同一个 WBP 类的两个实例，
	 * 靠的就是 bIsEnergyBar 那一 bit（见 UHeroHealthBarWidget）。按它分，
	 * "WBP 里摆了血条 + 能量条但名字都没对上"这种情况也能各归各位。
	 *
	 * 【优先级】名字绑定【优先】：某个属性已经被 BindWidgetOptional 接上了就一个字都不动。
	 * 所以 WBP 里名字起对了的控件永远说了算，这条只是补漏。
	 */
	void ResolveOptionalChildren();

	// ---- 可选绑定：WBP 里的控件名字对上就自动接 ----

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> NameText;

	/** 复用 WBP_HealthBar 那个类。bIsEnergyBar 在 WBP 里配成 false。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroHealthBarWidget> HealthBar;

	/** 复用 WBP_HealthBar 那个类。bIsEnergyBar 在 WBP 里配成 true。留空 = 目标框不显示能量条。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroHealthBarWidget> EnergyBar;

	/** 死亡压暗遮罩。目标处于 State.Dead 时显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> DeadOverlay;

	// ---- 表现参数 ----

	/** 是否显示能量条。留空 EnergyBar 时这里也会被忽略。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bShowEnergyBar = true;

	// ---- 当前视图（蓝图只读） ----

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	FTargetFrameView TargetFrame;

	/** 按类型兜底扫过一遍没有（见 ResolveOptionalChildren）。控件树不变，所以只扫一次。 */
	bool bOptionalChildrenResolved = false;
};
