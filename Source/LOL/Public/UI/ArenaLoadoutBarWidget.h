// 装备栏：一排装备 / 海克斯图标 + 攒着的锻造器次数。
//
// 【需求里的"装备栏"在这个模式里是什么】商店是关掉的（shop 全关），所以装备只有
// 「回合奖励」一个来源。这个界面就是唯一能看见"我现在身上有什么"的地方 ——
// 它不提供任何操作（没有拖拽、没有出售），只显示。
//
// 【为什么锻造器次数和装备挤在同一个 Widget 里】它们回答的是同一个问题：
// "我这一回合拿到了什么、还有什么能用"。分成两个 Widget 的话，奖励阶段结束后
// 玩家得看两个地方才能决定进不进战斗 —— 而那个决定只需要一眼。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaLoadoutBarWidget.generated.h"

class UArenaLoadoutEntryWidget;
class UPanelWidget;
class UTextBlock;

UCLASS(Abstract)
class LOL_API UArenaLoadoutBarWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 灌一份装备栏。全量覆盖。 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void ApplyLoadout(const FArenaLoadoutView& InLoadout);

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	FArenaLoadoutView GetLoadout() const { return Loadout; }

	/** 蓝图侧的实现入口（新装备弹入、被顶掉的那件播放消失…）。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|UI")
	void BP_OnLoadoutChanged(const FArenaLoadoutView& InLoadout);

protected:
	/** 格子的类。在 WBP 里配。不配就一格都画不出来（会记一条 Warning）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|UI")
	TSubclassOf<UArenaLoadoutEntryWidget> EntryWidgetClass;

	/**
	 * 格子的容器。
	 *
	 * ⚠️ 和奖励界面同一条注意事项：容器是 HorizontalBox 时，格子 WBP 的根必须自报尺寸
	 *（根上用 SizeBox 定宽高），否则会被挤成 0 宽 —— 详见 ArenaRewardScreenWidget.h 顶部。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> EntryContainer;

	/** 锻造器次数那一行。没有次数（全花完了）时整行收起。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ForgeChargesText;

	/** 当前视图。蓝图只读。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FArenaLoadoutView Loadout;

private:
	/** 格子数量对不上就重建（理由同 UArenaRewardScreenWidget::EnsureCardCount）。 */
	void EnsureEntryCount(int32 DesiredCount);

	/** 造出来的格子。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UArenaLoadoutEntryWidget>> Entries;
};
