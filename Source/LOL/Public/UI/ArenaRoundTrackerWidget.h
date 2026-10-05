// 回合线（round tracker）：顶部中央那一排回合计划图标。
//
// ===========================================================================
// 【对齐 LoL 斗魂竞技场的 CherryRounds】一排小图标，从左到右是整场比赛的
// 回合计划：已经打完的画完成态、正在打的画进行态（稍大）、还没到的画未开始态。
// 格子是什么（战斗 / 海克斯 / 装备 / 锻造）由服务端按回合表推下来
// （AArenaGameState::RoundPlan），这个 Widget 只画不猜 —— 它不知道
// "第 7 回合之后是循环"，也不需要知道。
//
// 【为什么条目是程序化 UImage 而不是子 WBP】一个格子就是一个换贴图的 Image，
// 没有按钮、没有文本、没有自己的状态。为它做一个 WidgetBlueprint 类 +
// CardWidgetClass 那套配置，等于给"画一张贴图"上了三道工序。
// 运行时 NewObject<UImage> 挂进容器是 UMG 的正常用法，蓝图里还能继续覆盖样式。
//
// 【贴图缺失的兜底】三态 × 四种回合 = 最多 12 张图标，关卡/皮肤只配了一部分时
// 不该画成空白：缺"完成态"用进行态贴图压灰、缺"未开始态"用进行态贴图压暗。
// 这样只配一张 combatcurrent.png 也能跑出可读的回合线。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaRoundTrackerWidget.generated.h"

class UImage;
class UPanelWidget;
class UTexture2D;

/** 一种回合的三态贴图。任何一档都可以留空 —— 留空的档位用进行态贴图加染色兜底。 */
USTRUCT(BlueprintType)
struct FArenaStageIconSet
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|UI|Skin")
	TSoftObjectPtr<UTexture2D> Completed;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|UI|Skin")
	TSoftObjectPtr<UTexture2D> Current;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|UI|Skin")
	TSoftObjectPtr<UTexture2D> Upcoming;
};

UCLASS(Abstract)
class LOL_API UArenaRoundTrackerWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UArenaRoundTrackerWidget(const FObjectInitializer& ObjectInitializer);

	/**
	 * 灌一份回合线。bValid 为 false 时整条收起。
	 *
	 * 【格子数量对不上就重建，对得上就只换贴图】和 UArenaRewardScreenWidget 的
	 * EnsureCardCount 同一条纪律：回合计划整场不变，绝大多数调用只是
	 * "当前回合 +1"，重建一排 Image 会打断正在播的过渡动画。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void ApplyTracker(const FArenaRoundTrackerView& InTracker);

	/** 蓝图侧的实现入口（当前格的呼吸动画、回合切换的闪烁…）。数据已在 C++ 填好。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|UI")
	void BP_OnTrackerChanged(const FArenaRoundTrackerView& InTracker);

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	/**
	 * 格子往这里塞。推荐 HorizontalBox：格子自报尺寸（贴图原始大小），
	 * 间距用 HorizontalBoxSlot 的 Padding 调。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UPanelWidget> TrackContainer;

	/**
	 * 四种回合的三态贴图。默认指向斗魂官方图标
	 * （/Game/LOL/UI/Arena/Textures/Cherry/StageIcons/，由导入脚本生成）；
	 * 没导入贴图时全部解析失败，走"单色染色"兜底，回合线照样可读。
	 *
	 * 【为什么是 TMap 而不是 12 个独立属性】三态 × 四类是同一个"格子图标"的
	 * 四个实例，平铺 12 个 UPROPERTY 没法一眼看出配的是哪一态；
	 * 按 kind 一组，缺哪一态、补哪一态在 Details 面板里是同一个小节。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|UI|Skin")
	TMap<EArenaStageKindView, FArenaStageIconSet> StageIcons;

	/** 当前视图。蓝图只读。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FArenaRoundTrackerView Tracker;

private:
	/** 格子数量对不上就重建（新建 Image 挂进容器），对得上就复用。 */
	void EnsureEntryCount(int32 DesiredCount);

	/**
	 * 解析一格的贴图 + 染色。
	 *
	 * 【染色规则】贴图按"优先本态 → 退进行态"取；退了进行态时用颜色区分：
	 *   完成态压灰、未开始态压暗半透明。三态贴图都配齐时染色是白色（原样）。
	 */
	void ResolveEntryVisual(const FArenaStageEntryView& Entry, UTexture2D*& OutTexture, FLinearColor& OutTint);

	UTexture2D* ResolveIconTexture(EArenaStageKindView Kind, EArenaStageState State);

	/** 软引用 → 贴图（带缓存：软引用只解析一次）。 */
	UTexture2D* GetCachedTexture(const TSoftObjectPtr<UTexture2D>& Soft, TObjectPtr<UTexture2D>& CacheSlot);

	/** 生成的格子。下标 = 回合下标。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UImage>> Entries;

	/**
	 * 缓存的贴图指针（UPROPERTY 防 GC）。键 = Kind<<2 | State（枚举一共 4×3 种组合）。
	 * 【为什么缓存】心跳每次变化都会走一遍这里；软引用每帧 LoadSynchronous
	 * 等于反复查资产注册表 —— 先查缓存，miss 才解析。
	 */
	UPROPERTY(Transient)
	TMap<uint8, TObjectPtr<UTexture2D>> ResolvedIcons;
};
