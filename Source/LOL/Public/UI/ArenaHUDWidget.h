// 竞技场 HUD 的根 Widget：唯一持有 UArenaHUDController 的那一个。
//
// ===========================================================================
// 【为什么只有它有翻译层】
// 和 WBP_HUD 那套一样（见 UHeroHUDWidget）：只有根控件去订阅翻译层、拿全量快照，
// 再把每一块分给子控件。子控件（三选一界面 / 比分栏 / 装备栏）一律不认识
// UArenaHUDController，只认识自己的 ApplyXxx(View)。
//
// 好处是具体的：新增一个"显示回合奖励历史"的面板时，它不需要知道
// PlayerState 在哪、不需要重试绑定、也不会在翻译层没建好时各写一套兜底。
//
// 【点击是怎么变成一次提交的】三选一界面自己不提交 —— 它把 OnOptionChosen 报上来，
// 由这里调 UArenaHUDController::SubmitChoice。中间这一跳保证了"谁能提交选择"
// 只有一个答案（就是持有翻译层的这个对象）。
//
// 【输入模式不在这里管】奖励界面要能用鼠标点，靠的是 AArenaPlayerController
// 把输入模式切成 UIOnly（它照复制下来的相位做，见那边的注释）——
// 这个类只负责把界面画出来。两个职责分开，是因为"该不该吃鼠标"取决于
// 服务端说的相位，而不是取决于本地有没有弹出某个 WBP。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaHUDWidget.generated.h"

class AArenaPlayerController;
class UArenaAnnounceWidget;
class UArenaHUDController;
class UArenaLoadoutBarWidget;
class UArenaLoadingWidget;
class UArenaMatchStatusWidget;
class UArenaRewardScreenWidget;
class UArenaRoundTrackerWidget;

UCLASS(Abstract)
class LOL_API UArenaHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 翻译层（Widget / 蓝图要手动推一遍时用）。没建好时返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	UArenaHUDController* GetArenaHUDController() const { return ArenaHUD; }

	/**
	 * 强行拉一次全量并分发。PIE 里改了资产 / 想手动刷新时用。
	 * 平时不需要调 —— 订阅即拉取在 BindToController 里已经做过了。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void RefreshFromController();

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	// ---- 翻译层的三条通道 ----

	UFUNCTION()
	void HandlePromptChanged(const FArenaPromptView& Prompt);

	UFUNCTION()
	void HandleMatchChanged(const FArenaMatchView& Match);

	UFUNCTION()
	void HandleLoadoutChanged(const FArenaLoadoutView& Loadout);

	/** 回合计划变了（开局一次）。转发给回合线。 */
	UFUNCTION()
	void HandleRoundTrackerChanged(const FArenaRoundTrackerView& Tracker);

	/**
	 * 相位边沿。VS 介绍 / 结果横幅在这里触发 —— 表现的决策点在根上，
	 * 公告层（UArenaAnnounceWidget）只负责演。
	 */
	UFUNCTION()
	void HandlePhaseChanged(EArenaPhaseView NewPhase);

	/** 某张卡被点了。转发给翻译层（唯一的提交入口）。 */
	UFUNCTION()
	void HandleOptionChosen(int32 OptionIndex);

	/** 某张卡的重随按钮被点了。转发给翻译层 → 服务端（合法性在服务端判）。 */
	UFUNCTION()
	void HandleRerollRequested(int32 OptionIndex);

	// ---- 子控件（全部可选：只做比分栏也能跑）----

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UArenaRewardScreenWidget> RewardScreen;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UArenaMatchStatusWidget> MatchStatus;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UArenaLoadoutBarWidget> LoadoutBar;

	/** 开局的 Loading 面板（斗魂选手卡）。显隐自己管（见 UArenaLoadingWidget）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UArenaLoadingWidget> LoadingScreen;

	/**
	 * 回合线（顶部中央的回合计划图标排）。没挂 = 不画回合线，其余功能不受影响。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UArenaRoundTrackerWidget> RoundTracker;

	/**
	 * 公告层（VS 介绍 / 结果横幅）。没挂 = 不播公告，其余功能不受影响。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UArenaAnnounceWidget> Announce;

private:
	/** 顺着 OwningPlayer 找翻译层。找不到就排一次有界重试。 */
	void BindToController();
	void UnbindFromController();

	void StartControllerRetry();
	void StopControllerRetry();

	/** 拿不到翻译层时的重试（和 UHeroHUDWidget 同一套：0.2 秒一次，最多 25 次）。 */
	UFUNCTION()
	void RetryBind();

	UPROPERTY(Transient)
	TObjectPtr<UArenaHUDController> ArenaHUD;

	FTimerHandle ControllerRetryHandle;
	int32 ControllerRetryCount = 0;

	static constexpr float ControllerRetryInterval = 0.2f;
	static constexpr int32 MaxControllerRetries = 25;
};
