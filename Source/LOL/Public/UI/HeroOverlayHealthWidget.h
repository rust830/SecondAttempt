// 敌人头顶的横条血条。头文件零 GAS include —— 数据由 UHeroOverlayHealthComponent 推过来。
//
// 它跟 WBP_HUD 里的血条【共用同一个 UHeroHealthBarWidget】：复用的是那套 UI 语义契约
// （FHUDVitalsView），不是某个具体对象。所以同一份 WBP_HealthBar 两边都能用。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/HUDTypes.h"
#include "HeroOverlayHealthWidget.generated.h"

class UHeroHealthBarWidget;

UCLASS(Abstract)
class LOL_API UHeroOverlayHealthWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * UHeroOverlayHealthComponent 调这个。
	 *
	 * 两个布尔是【显示策略】，由组件传进来而不是 Widget 自己配：
	 * 同一个 WBP_OverlayHealth 要用在英雄 / 小兵 / 建筑上，隐藏规则是每个实例的事，
	 * 不是这套 Widget 的事。策略只留一个出处。
	 */
	void ApplyOverlayVitals(const FHUDVitalsView& Vitals, bool bIsDead, bool bHideWhenFullHealth, bool bHideWhenDead);

	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnOverlayVitalsChanged(const FHUDVitalsView& Vitals, bool bIsDead);

protected:
	/** 可选绑定：名字对上就用内置的血条渲染（WBP_HealthBar 那个类）。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroHealthBarWidget> HealthBar;
};
