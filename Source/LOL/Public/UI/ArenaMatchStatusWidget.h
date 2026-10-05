// 比分栏：第几回合、什么相位、倒计时、双方的大场血量和战绩。
//
// 【这里画的血条不是角色血条】角色血量每回合回满（那是 WBP_HealthBar 那套 FHUDVitalsView），
// 这一条只减不增（需求里的 15/30/40/50）。两条条同时在屏幕上，各自有各自的含义，
// 所以这个 Widget 不复用 UHeroHealthBarWidget —— 复用的话就得先给那个类加一个
// "这次画的是大场血量"的开关，而那个开关迟早会被用错。
//
// 【对手那一栏也是复制数据】对手的 PlayerState 是复制过来的，所以他的大场血量在
// 两台机器上都算得出来 —— 这里不需要任何"同步对手血量"的逻辑。
// 数据到不到位由 FArenaContenderView::bValid 表示，false 时收起那一栏。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaMatchStatusWidget.generated.h"

class UImage;
class UProgressBar;
class UTextBlock;
class UWidget;
class UArenaHUDLayoutConfig;

UCLASS(Abstract)
class LOL_API UArenaMatchStatusWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 灌一份比赛状态。全量覆盖。 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void ApplyMatch(const FArenaMatchView& InMatch);

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	FArenaMatchView GetMatch() const { return Match; }

	/** 蓝图侧的实现入口（回合切换的过场、血量变化的抖动…）。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|UI")
	void BP_OnMatchChanged(const FArenaMatchView& InMatch);

protected:
	/**
	 * 套用布局配置。
	 *
	 * 选 PreConstruct 而不是 Construct：**PreConstruct 在设计时也会跑** ——
	 * WBP 的 Designer 里就能看到套用后的效果，改一个数字立刻反映，不用进 PIE。
	 * Construct 只在运行时跑，那就得先跑一局才知道摆对没有。
	 *
	 * 重复调用是安全的（每次都是全量覆盖，不累加）。
	 */
	virtual void NativePreConstruct() override;

	/**
	 * 布局配置。指向 DA_ArenaHUDLayout_MatchStatus。
	 *
	 * 留空 = 完全沿用 WBP 里排好的位置（这套逻辑就相当于不存在）。
	 * 所以"想临时回到手摆的版本"不需要改代码，把这里的引用清掉就行。
	 *
	 * 坐标从 LoL 斗魂竞技场的真实 UX 数据转来（1600×1200 画布，见
	 * Arena_HUD_UMG_Setup.md 与 arena_ui/out/umg_spec.md）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|UI|Layout")
	TObjectPtr<UArenaHUDLayoutConfig> LayoutConfig;

	/**
	 * 一份数据灌进某一栏（自己 / 对手各一次）。共用一个实现，免得两栏慢慢写歪。
	 *
	 * 【血条是环形材质的 Image】规格书要求环形大场血量：BarWidget 是一个
	 * 挂了环形材质（带 "Percent" 标量参数）的 Image，百分比走 MID 参数。
	 * 没挂材质时这条更新是 no-op（不至于崩）。
	 */
	void ApplyContender(const FArenaContenderView& View, UTextBlock* NameWidget, UImage* BarWidget,
		UTextBlock* HealthWidget, UTextBlock* ScoreWidget, UWidget* RootWidget);

	// ---- 顶部：回合 / 相位 / 倒计时 ----

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RoundText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> PhaseText;

	/** 相位倒计时。没有倒计时的相位（战斗 / 等待）把它收起来。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> CountdownText;

	// ---- 自己那一栏 ----

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelfNameText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UImage> SelfHealthBar;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelfHealthText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelfScoreText;

	/** 自己那一栏的根，用来整栏收起。留空 = 找不到可收起的控件，只清空文本。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> SelfPanel;

	// ---- 对手那一栏 ----

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> OpponentNameText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UImage> OpponentHealthBar;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> OpponentHealthText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> OpponentScoreText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> OpponentPanel;

	// ---- 结果 ----

	/** "你赢了" / "你输了"。比赛没结束时收起来。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ResultText;

	/**
	 * 上一回合的结果（"√ 上回合胜利 −30"）。规格书的 outcome_display + team_health_loss。
	 * 文本为空（还没打过任何一回合）时收起。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> LastRoundText;

	/** 当前视图。蓝图只读。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FArenaMatchView Match;
};
