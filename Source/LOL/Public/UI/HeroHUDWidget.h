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
#include "UI/HUDTypes.h"
#include "HeroHUDWidget.generated.h"

class UHeroHealthBarWidget;
class UHeroHUDController;
class UHeroSkillSlotWidget;
class UPanelWidget;

UCLASS(Abstract)
class LOL_API UHeroHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 手动重拉一次（比如切关卡后重新 AddToViewport，或者 Controller 当时还没就绪）。 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void RefreshFromController();

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	// -----------------------------------------------------------------------
	// 蓝图实现入口。C++ 已经把内置的子 Widget 推好了，这几个只做额外表现。
	// -----------------------------------------------------------------------

	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnVitalsChanged(const FHUDVitalsView& Vitals);

	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnTargetFrameChanged(const FTargetFrameView& View);

	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnHUDReady(bool bBound);

	/** 槽位变化。目标框没有内置子 Widget，所以「目标框长什么样」完全由蓝图决定。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnSkillSlotChanged(int32 SlotIndex, const FSkillSlotView& View);

	// -----------------------------------------------------------------------
	// 可选绑定：名字对上就自动接。血条 / 能量条可以留空，蓝图自己处理。
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroHealthBarWidget> HealthBar;

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UHeroHealthBarWidget> EnergyBar;

	/** 技能槽的父容器（HorizontalBox / UniformGridPanel 都行）。留空就不自动建槽。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> SkillSlotContainer;

	/** 槽位 Widget 类（WBP_SkillSlot）。留空就不自动建槽。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
	TSubclassOf<UHeroSkillSlotWidget> SkillSlotWidgetClass;

	// ---- 委托回调。必须是 UFUNCTION 才能 AddDynamic。 ----

	UFUNCTION()
	void HandleVitalsChanged(const FHUDVitalsView& Vitals);

	UFUNCTION()
	void HandleSkillSlotChanged(int32 SlotIndex, const FSkillSlotView& View);

	UFUNCTION()
	void HandleTargetFrameChanged(const FTargetFrameView& View);

	UFUNCTION()
	void HandleHUDReady(bool bBound);

private:
	void BindToController();
	void UnbindFromController();
	void ApplySnapshot(const FHUDSnapshot& Snapshot);

	/** 槽位 Widget 的懒创建：不存在就建一个塞进容器。 */
	UHeroSkillSlotWidget* EnsureSlotWidget(int32 SlotIndex);

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
