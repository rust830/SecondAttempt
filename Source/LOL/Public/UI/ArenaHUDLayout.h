// 竞技场 HUD 的布局配置：把「控件名 → 锚点 / 对齐 / 偏移 / 尺寸」做成数据资产。
//
// ===========================================================================
// 【为什么不做成"编辑器工具直接改 WBP"】
// UE 没有把 UMG 的 WidgetTree 编辑能力开放出来 —— Python 侧实测：
//   * UWidgetBlueprint 的 WidgetTree 属性读不到（"Failed to find property"）；
//   * unreal.WidgetTree 这个类存在，但没有任何可调用的操作方法
//     （FindWidget / GetAllWidgets / ConstructWidget 都不是 UFUNCTION）；
//   * SubobjectDataSubsystem 只管 Actor / Component，对 WidgetBlueprint
//     调 k2_gather_subobject_data_for_blueprint 返回 0 条。
// 所以「自动摆控件」只能落在 C++ 里。这里选的是**运行时应用**：不动 WBP 资产
// 本身，而是在 NativePreConstruct 里按配置改各控件的 Slot。
//
// 【为什么是 NativePreConstruct 而不是 NativeConstruct】
// PreConstruct 在**设计时也会被调用** —— 于是 WBP 的 Designer 里就能直接看到
// 套用后的效果：在 DataAsset 里改一个数字，切回 WBP 立刻反映，不用进 PIE。
// 而 NativeConstruct 只在运行时跑，改完得先 PIE 才知道对不对。
//
// 【局限（有意保留的）】
// 只能调整「父容器是 CanvasPanel」的控件 —— 它们的 Slot 是 UCanvasPanelSlot。
// 嵌在 VerticalBox / HorizontalBox / SizeBox / Overlay 里的控件由容器布局管，
// 这里**跳过**，并把它记进返回的 Skipped 列表（不静默吞掉）。
// 想让某个控件也能被摆，把它挪到 CanvasPanel 直下即可。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ArenaHUDLayout.generated.h"

class UUserWidget;

/** 一个控件的布局。四个字段一一对应 UCanvasPanelSlot 的同名参数。 */
USTRUCT(BlueprintType)
struct FArenaWidgetLayout
{
	GENERATED_BODY()

	/** WBP 里的控件变量名 —— 就是 BindWidgetOptional 认的那个名字。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FName WidgetName;

	/**
	 * 锚点（归一化 0-1）。Min/Max 分开给是为了支持"锚定一整条边"的做法；
	 * 只做点定位时两个填一样就行。
	 *
	 * 默认 (0.5, 0.5) 是刻意的：LoL 那边每一个元素的 Anchor 都是 (0.5, 0.5)，
	 * 含义是"分辨率变化时以画布中心为基准等比缩放"。跟着它走，
	 * 21:9 和 4:3 下的表现会和原作最接近。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector2D AnchorMin = FVector2D(0.5, 0.5);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector2D AnchorMax = FVector2D(0.5, 0.5);

	/** 控件自身的哪个点对齐到锚点。(0.5, 0.5) = 用中心对齐，位置即控件中心。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector2D Alignment = FVector2D(0.5, 0.5);

	/** 相对锚点的像素偏移。配合 Alignment(0.5,0.5) 时，这就是"离屏幕中心多远"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector2D Position = FVector2D::ZeroVector;

	/** 像素尺寸。bApplySize 为 false 时忽略这一项。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	FVector2D Size = FVector2D(200.0, 48.0);

	/**
	 * 要不要覆盖尺寸。
	 *
	 * 关掉它的典型场合：控件里是自适应内容（一段会换行的文本、一排长度不定的
	 * 名字），WBP 里排好的尺寸比这里的估值准 —— 那就只摆位置、不动大小。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	bool bApplySize = true;
};

/**
 * 一个界面的布局表。
 *
 * 坐标的原点约定：Position 是「相对锚点的像素偏移」，不是绝对坐标。
 * 从 LoL 的 UX 数据（1600×1200 设计画布，绝对像素）转过来时：
 *
 *     Position = (x + w/2 − 画布宽/2,  y + h/2 − 画布高/2)
 *
 * 也就是"控件中心相对画布中心的偏移"。配 Anchor/Alignment 都是 (0.5,0.5) 使用。
 */
UCLASS(BlueprintType)
class LOL_API UArenaHUDLayoutConfig : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 要应用布局的控件。名字对不上、或父容器不是 CanvasPanel 的会被跳过。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout")
	TArray<FArenaWidgetLayout> Widgets;

	/**
	 * 把本配置应用到 Widget。
	 *
	 * 返回**被跳过的控件名**：没找到同名控件、或者它的 Slot 不是
	 * UCanvasPanelSlot（父容器是 VerticalBox / SizeBox 之类）。
	 * 返回而不是只打日志，是为了能在 PIE 里一眼看出来"我配的这 14 个里
	 * 哪几个其实没生效"。
	 */
	TArray<FName> ApplyTo(UUserWidget* Widget) const;
};
