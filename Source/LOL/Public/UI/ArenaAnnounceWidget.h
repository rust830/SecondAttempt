// 公告层：VS 介绍（开打瞬间）+ 结果横幅（大场结束）。
//
// ===========================================================================
// 【它吃的是 FArenaMatchView，不是相位】播什么由根 Widget 决定
// （根订阅翻译层的相位边沿），这个类只负责"把这一幕演出来 + 演完收场"。
// 于是"VS 介绍要不要在回放里播""结果横幅要不要常驻"这类决策全在根上改，
// 这里只留时长这一个旋钮。
//
// 【为什么 VS 和结果共用一个 Widget】两幕用的是同一块屏幕区域、同一个
// 全屏根、同一批入场资产（matchupintro 边框 + 淘汰底框），拆成两个 WBP
// 意味着根控件要管两个全屏面板的互斥显隐 —— 那是"模式"该干的事，
// 收进一个类里用一个 Mode 枚举表达，互斥天然成立。
//
// 【自动收场只在 VS】结果横幅之后永远是 MatchEnd（终态），没有"下一幕"
// 会盖上来，常驻反而是对的；VS 介绍不收就会一直糊在画面上。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaAnnounceWidget.generated.h"

class UImage;
class UTextBlock;
class UWidget;

/** 公告层的当前一幕。None = 整层收起。 */
UENUM(BlueprintType)
enum class EArenaAnnounceMode : uint8
{
	None		UMETA(DisplayName = "无"),
	VsIntro		UMETA(DisplayName = "VS 介绍"),
	Result		UMETA(DisplayName = "结果横幅"),
};

UCLASS(Abstract)
class LOL_API UArenaAnnounceWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UArenaAnnounceWidget(const FObjectInitializer& ObjectInitializer);

	/**
	 * 播 VS 介绍：左名 vs 右名 + VS 艺术字。VsIntroSeconds 后自动收起。
	 *
	 * 【重复调用会重置计时】进战斗的边沿理论上一场只来一次，
	 * 但翻译层的重试/心跳可能在极端时序下补发 —— 重置而不是叠加是对的。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void PlayVsIntro(const FArenaMatchView& Match);

	/**
	 * 播结果横幅（"你赢了 / 你输了"）。不自动收起 —— MatchEnd 是终态。
	 * bMatchEnded 为 false 时是 no-op（还没打完就没什么可宣布的）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void PlayResult(const FArenaMatchView& Match);

	/** 收起一切（回到 None）。切关卡 / 手动重置用。 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void Dismiss();

	/** 蓝图侧的实现入口（VS 的滑入、结果的闪白…）。显隐已在 C++ 定好。 */
	UFUNCTION(BlueprintImplementableEvent, Category = "Arena|UI")
	void BP_OnAnnounceChanged(EArenaAnnounceMode NewMode);

	/** 现在演到哪一幕。 */
	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	EArenaAnnounceMode GetMode() const { return Mode; }

protected:
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	/** 整个公告层的根。Mode == None 时收起它。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> AnnounceRoot;

	/** VS 介绍那一幕（左名 / VS 图 / 右名）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> VsPanel;

	/** 结果横幅那一幕。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UWidget> ResultPanel;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> VsSelfText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> VsOpponentText;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> ResultText;

	/**
	 * VS 艺术字（官方贴图 cherry_hud_vs_text）。
	 * 贴图没导入时整张 Image 留空 —— 名字文本不受影响，VS 介绍照样可读。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UImage> VsImage;

	/**
	 * 结果横幅的底框（官方贴图 elimination_background_frame）。
	 * 没导入时留空 —— 结果文本直接浮在画面上，可读性还在。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI", meta = (BindWidgetOptional))
	TObjectPtr<UImage> ResultFrameImage;

	/** VS 介绍的停留时长（秒）。0 = 不自动收（调试摆位时方便）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|UI", meta = (ClampMin = "0", Units = "s"))
	float VsIntroSeconds = 2.5f;

private:
	/** 应用一个模式：互斥显隐 + 收起/展开根 + 广播给蓝图。 */
	void ApplyMode(EArenaAnnounceMode NewMode);

	/** Mode == VsIntro 时排在收场时间上的那一条。 */
	void HandleVsIntroExpire();

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> CachedVsTexture = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> CachedResultFrame = nullptr;

	FTimerHandle VsIntroExpireHandle;

	/** 软引用贴图。构造函数里指向 /Game/LOL/UI/Arena/Textures/Cherry/ 下的官方资产。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|UI|Skin", meta = (AllowPrivateAccess = "true"))
	TSoftObjectPtr<UTexture2D> VsTextTexture;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|UI|Skin", meta = (AllowPrivateAccess = "true"))
	TSoftObjectPtr<UTexture2D> ResultFrameTexture;

	EArenaAnnounceMode Mode = EArenaAnnounceMode::None;
};
