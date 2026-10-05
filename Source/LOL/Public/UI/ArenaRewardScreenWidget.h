// 奖励选择界面：把 FArenaPromptView 画成一排卡片。
//
// ===========================================================================
// 【它不知道自己在选什么】
// 这个类里没有一处判断"这是装备还是海克斯"、"这是第几回合"、
// 甚至没有一处判断"点了会得到什么"。它只做两件事：
//   ① 有几张卡就生成几个 UArenaChoiceCardWidget；
//   ② 某张卡被点了，把它的 OptionIndex 转发出去（OnOptionChosen）。
// 于是"第 1 回合是二选一、第 2 回合是三选一海克斯、锻造器菜单长什么样"
// 这些事全部不用改这个文件 —— 它们本来就是服务端算好的待选。
// ===========================================================================
//
// 【点了之后谁提交】不是它。根 Widget（UArenaHUDWidget）订阅 OnOptionChosen，
// 再调 UArenaHUDController::SubmitChoice。中间多一跳是有意的：
// 子 Widget 一律不持有翻译层（同 WBP_HUD 里那些子控件的做法），
// 这样"谁能提交选择"永远只有一个答案。
//
// 【WBP 侧必须注意的一件事】卡片是塞进 CardContainer 的，如果那个容器是
// HorizontalBox，卡片 WBP 的【根控件必须自报尺寸】（根上用 SizeBox 定宽高）——
// 不自报尺寸的根（比如 CanvasPanel）在 HBox 里会被挤成 0 宽，表现是"三张卡叠在一起"。
// 这个坑本项目在技能栏上踩过，见 HeroHUDWidget.cpp 里 EnsureSlotWidget 那段长注释。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaRewardScreenWidget.generated.h"

class UArenaChoiceCardWidget;
class UButton;
class UPanelWidget;
class UTextBlock;

/** 玩家点了某张卡。参数是 FArenaChoiceCardView::OptionIndex，原样转发给翻译层。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaOptionChosenSignature, int32, OptionIndex);

/** 玩家点了某张卡的重随按钮。参数同上，由根 Widget 转给翻译层 → 服务端。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaRerollRequestedSignature, int32, OptionIndex);

UCLASS(Abstract)
class LOL_API UArenaRewardScreenWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * 灌一份待选。bActive 为 false 时整个界面收起。
	 *
	 * 【收起而不是画一个空面板】没有待选的时候（战斗阶段、结算阶段）屏幕上不该留一块
	 * "什么都没有"的板子 —— 那看起来像界面坏了。这条纪律和"未就绪不画空血条"是同一个。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void ApplyPrompt(const FArenaPromptView& InPrompt);

	/** 当前视图。返回拷贝 —— UFUNCTION 的返回值必须是值类型。 */
	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	FArenaPromptView GetPrompt() const { return Prompt; }

	/** 蓝图侧的实现入口（入场动画、翻牌…）。数据已经在 C++ 填好了。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|UI")
	void BP_OnPromptChanged(const FArenaPromptView& InPrompt);

	/** 某张卡被点了。根 Widget 订阅它。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaOptionChosenSignature OnOptionChosen;

	/** 某张卡的重随按钮被点了。根 Widget 订阅它。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaRerollRequestedSignature OnRerollRequested;

	/**
	 * 切换选择内容的显示/隐藏（规格书的 hide_show_button）。
	 * 只收内容（标题 + 卡片），底部那颗按钮本身保留 —— 收掉之后还得有地方点回来。
	 * 按钮文本由 HandleHideShowClicked 同步换成"显示"/"隐藏"。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void ToggleContentVisibility();

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	bool IsContentHidden() const { return bContentHidden; }

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	/** 所有卡片共用的这一个回调 —— 卡片自己的下标就在参数里。 */
	UFUNCTION()
	void HandleCardClicked(int32 OptionIndex);

	UFUNCTION()
	void HandleCardRerolled(int32 OptionIndex);

	UFUNCTION()
	void HandleHideShowClicked();

	/** 把 bContentHidden 应用到内容控件上。ApplyPrompt 和 Toggle 共用它，保证只有一个真相。 */
	void ApplyContentVisibility();

	/**
	 * 收/放的内容根。WBP 里放一个包住标题+卡片的容器并命名为 PromptContent。
	 * 没放的话退而收 TitleText / CardContainer 两个（都能用，行为一致）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> PromptContent;

	/**
	 * 底部的隐藏/显示按钮（规格书的 hide_show_button，屏幕底部居中）。
	 * 没绑 = 没有这个功能，界面行为退回"始终显示"。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UButton> HideShowButton;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> HideShowButtonText;

	/** 当前内容是否被收起。 */
	UPROPERTY(Transient)
	bool bContentHidden = false;

	/**
	 * 卡片类。在 WBP 里配（EditDefaultsOnly 而不是 EditAnywhere：
	 * 这是"这个界面长什么样"的一部分，不该在实例上被改掉）。
	 * 不配就一张卡都画不出来，会记一条 Warning。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|UI")
	TSubclassOf<UArenaChoiceCardWidget> CardWidgetClass;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> TitleText;

	/**
	 * 卡片往这里塞。
	 *
	 * 类型是 UPanelWidget 而不是 UHorizontalBox：竖排、WrapBox、Grid 都行，
	 * 这个类不替美术决定怎么摆 —— 它只需要"有个能加子控件的地方"。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> CardContainer;

	/** 当前视图。蓝图只读。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FArenaPromptView Prompt;

private:
	/** 卡片数量对不上就重建。数量对得上时什么都不做（见实现里的注释）。 */
	void EnsureCardCount(int32 DesiredCount);

	/** 生成的第 Index 张卡。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UArenaChoiceCardWidget>> Cards;
};
