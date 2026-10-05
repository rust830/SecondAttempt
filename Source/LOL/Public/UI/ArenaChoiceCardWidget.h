// 三选一里的一张卡。
//
// 【职责只有两件】把 FArenaChoiceCardView 画出来、被点时把 OptionIndex 报上去。
// 它不认识 GameMode、不认识 PlayerState、不知道这张卡是装备还是海克斯 ——
// "点了之后给什么"完全由服务端按下标决定（见 ArenaHUDController::SubmitChoice）。
//
// 【为什么它是一个独立的 Widget 类】奖励界面上的卡片数量是运行期定的（2 张或 3 张，
// 池子不够时还可能更少），所以卡片必须是"可以按份数生成"的东西。
// 做成一个类 + 一个 CardWidgetClass，美术只需要把这一张卡的样子做好。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaChoiceCardWidget.generated.h"

class UButton;
class UImage;
class UTextBlock;

/** 这张卡被点了。参数是 FArenaChoiceCardView::OptionIndex。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaCardClickedSignature, int32, OptionIndex);

/** 这张卡的重随按钮被点了。参数同上 —— 服务端按下标换掉待选里的那一项。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaCardRerollSignature, int32, OptionIndex);

UCLASS(Abstract)
class LOL_API UArenaChoiceCardWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UArenaChoiceCardWidget(const FObjectInitializer& ObjectInitializer);

	/**
	 * 灌一份视图。全量覆盖 —— 卡片没有"部分状态"。
	 *
	 * 【同一份视图重复灌是空操作】奖励界面的 ApplyPrompt 会在每次待选变化时重跑一遍，
	 * 而其中大部分卡片其实没变（比如锻造器菜单里多了"使用棱彩锻造器"那一项，
	 * 前面的「进入战斗」原样不动）。不判一下就重复 SetText / SetBrushFromTexture，
	 * 白掉一遍布局，还会把正在播的动画打断。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void ApplyCard(const FArenaChoiceCardView& InCard);

	/** 当前这张卡的下标。没灌过视图时是 INDEX_NONE。 */
	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	int32 GetOptionIndex() const { return Card.OptionIndex; }

	/** 蓝图侧的实现入口（悬停动画 / 品质描边…）。数据已经在 C++ 填好了。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|UI")
	void BP_OnCardChanged(const FArenaChoiceCardView& InCard);

	/** 玩家点了这张卡。奖励界面订阅它。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaCardClickedSignature OnCardClicked;

	/** 玩家点了这张卡的重随按钮。奖励界面订阅它。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaCardRerollSignature OnRerollClicked;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	UFUNCTION()
	void HandleCardButtonClicked();

	UFUNCTION()
	void HandleRerollButtonClicked();

	// ---- 可选绑定：WBP 里的控件名字对上就自动接 ----

	/**
	 * 卡片的按钮。
	 *
	 * 【为什么把整套显隐/禁用都挂在它上面】bInteractable 为 false 的卡（服务端会拒掉的那种）
	 * 必须点不动，而"点不动"最可靠的实现是让按钮自己变成 disabled —— 在 OnClicked 里
	 * 判一下再 return 的话，按钮仍然会有按下反馈，玩家会以为"点了但没生效"。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UButton> CardButton;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> LabelText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> DescriptionText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UImage> IconImage;

	/**
	 * 卡片底框（斗魂卡贴图铺满整张卡）。
	 *
	 * 【为什么由 C++ 换贴图而不是蓝图图表】品质只有"换一张底图"这一件事，
	 * 三档各配一张软引用就够了 —— 不值得为它写一段蓝图图表（程序化搭建时
	 * 图表是最难生成的部分）。WBP 里只要有一个叫 FrameImage 的 Image。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UImage> FrameImage;

	/** 品质名（"白银"/"黄金"/"棱彩"）。None 档收起。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> TierText;

	/**
	 * 这张卡的重随按钮（规格书：每张卡下方一个，花 1 次重随换掉这个选项）。
	 * 可点条件（整份待选可重随 + 还有次数）由 ApplyCard 按视图设 enabled；
	 * 显示的次数文本放 RerollCountText。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UButton> RerollButton;

	/** 重随按钮上的次数文本（"重随 (3)"的括号部分由这里单独显示也行，整句也行）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> RerollCountText;

	/**
	 * 三档品质的卡片底图 + 「进入战斗」按钮底图。默认指向斗魂风格贴图
	 * （/Game/LOL/UI/Arena/Textures/），在 WBP 或实例上可以换皮。
	 *
	 * 【为什么是软引用】这个类会被 RewardScreen 按份数 CreateWidget，构造函数里
	 * 硬引用会把三张卡图拖进每一次的引用链；软引用 + 按需加载，首次 ApplyCard 才取。
	 */
	UPROPERTY(EditAnywhere, Category = "Arena|UI|Skin")
	TSoftObjectPtr<UTexture2D> SilverFrame;

	UPROPERTY(EditAnywhere, Category = "Arena|UI|Skin")
	TSoftObjectPtr<UTexture2D> GoldFrame;

	UPROPERTY(EditAnywhere, Category = "Arena|UI|Skin")
	TSoftObjectPtr<UTexture2D> PrismaticFrame;

	UPROPERTY(EditAnywhere, Category = "Arena|UI|Skin")
	TSoftObjectPtr<UTexture2D> FinishFrame;

	/** 当前视图。蓝图只读。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FArenaChoiceCardView Card;

private:
	/** 按品质档取底图（带缓存：软引用只解析一次）。返回 nullptr = 这档没配图。 */
	UTexture2D* ResolveTierTexture(EArenaCardTier Tier);

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> CachedSilver = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> CachedGold = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> CachedPrismatic = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> CachedFinish = nullptr;
};
