// 竞技场 Loading 界面：开局面板，两张斗魂风格的选手卡 + 标题。
//
// 【为什么挂在 ArenaHUD 上而不是单独一个 WBP】加载界面需要的两样东西
// （本地玩家是谁、对手是谁）翻译层的 FArenaMatchView 里已经有 —— 单独再开一条
// 订阅/绑定链等于把"找到 PlayerState"这件事抄第二遍。它就是 HUD 的第四个子面板，
// 和 MatchStatus / RewardScreen / LoadoutBar 平级。
//
// 【显隐规则】RoundNumber == 0 且比赛没结束 = 还在"等待对手"阶段 → 显示；
// 第 1 回合一开始整个收起（不是画一个空面板，同 RewardScreen 的纪律）。
// 真正的地图加载由引擎负责，这个面板只负责"开局摆两张卡"的仪式感。
//
// 【英雄立绘现在没有】FArenaContenderView 里还没有 portrait 字段（PlayerState 上
// 没有英雄形象数据）。WBP 里把立绘 Image 摆好、给一张占位图，等视图加了
// Portrait 字段后这里直接灌 —— 控件名现在就占好位。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaLoadingWidget.generated.h"

class UTextBlock;
class UWidget;

UCLASS(Abstract)
class LOL_API UArenaLoadingWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 灌一份大场状态。全量覆盖；内部自己决定显隐（见类注释的显隐规则）。 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void ApplyMatch(const FArenaMatchView& InMatch);

	/** 蓝图侧的实现入口（入场动画之类）。数据已经在 C++ 填好了。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|UI")
	void BP_OnLoadingChanged(const FArenaMatchView& InMatch);

protected:
	// ---- 可选绑定：WBP 里的控件名字对上就自动接 ----

	/** 顶部标题（"斗魂竞技场"）。文本内容 WBP 静态配即可，这里不覆盖。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> TitleText;

	/** 自己那侧的选手卡容器（整体显隐跟着 bValid 走）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> SelfCard;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> SelfNameText;

	/** 对手那侧的选手卡容器。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> OpponentCard;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> OpponentNameText;

	/** 对局说明（"等待对手进入竞技场…"），只在还没对手时显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> WaitingText;

private:
	/** 一侧选手卡：名字 + 显隐。bValid 为 false 时整卡收起。 */
	static void ApplyContender(
		const FArenaContenderView& Contender,
		UWidget* Card,
		UTextBlock* NameText);
};
