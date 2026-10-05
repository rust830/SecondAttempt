// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"   // FGameplayTag —— 不在 CoreMinimal 里，必须显式引
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Sound/SoundBase.h"
#include "UObject/SoftObjectPtr.h"

#include "HeroAudioLibrary.generated.h"

class UHeroAudioConfig;

/**
 * 音效解析 / 播放的统一入口（2026-10-04，见 CodeReview/12_音效层.md）。
 *
 * 【取值顺序】SoundOverride（调用点自己钉的）→ UHeroAudioConfig::SoundByTag（表）
 * → UHeroAudioConfig::FallbackSound（兜底）→ 都没有：打一次 warning 然后安静返回。
 *
 * ⚠️ 最后那条「安静返回」是有意的：音效缺失不能让表现层崩，
 * 但必须留痕 —— 所以按 tag 去重，同一个缺失事件的文件里只报一次，
 * 不然一次连击能刷屏几百行。
 *
 * 【谁该用】任何想播「一个事件音」的地方：GameplayCue、Ability、将来的 UI 点击音。
 * 别再直接调 UGameplayStatics::PlaySoundAtLocation —— 那样又变成散落的硬编码点了。
 *
 * 【GC 保护】GetConfig() 的缓存（GHeroAudioConfig）在 .cpp 里是 TStrongObjectPtr ——
 * 5.8 的 TStrongObjectPtr 走 UObject 自己的引用计数（UObjectBase::AddRef），GC 当根，
 * 不依赖这个类的 AddReferencedObjects 会不会被调到。历史上这里试过
 * 「TObjectPtr + 手写 AddReferencedObjects 静态钩子」，UHT 确实把它接进了类的
 * CppClassStaticFunctions（Editor / Game 两个 target 的 gen.cpp 逐字节相同），
 * 但打包版仍然没钉住、崩在 GetConfig 的 IsValid 里 —— 所以别退回那条路。
 * 也别给这个 UCLASS 挂 FGCObject 基类：它只给非 UObject 类用。
 */
UCLASS()
class LOL_API UHeroAudioLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** 显式注册配置（比如某个 Subsystem 在 Init 时指过来）。传 null = 清掉，下次懒加载回默认路径。 */
	UFUNCTION(BlueprintCallable, Category = "Hero Audio")
	static void SetConfig(UHeroAudioConfig* InConfig);

	/** 当前生效的配置（懒加载默认路径上的 DA_HeroAudioConfig）。 */
	UFUNCTION(BlueprintPure, Category = "Hero Audio")
	static UHeroAudioConfig* GetConfig();

	/**
	 * 解析出这个事件最终该播哪个音。返回 null 表示「这个事件就是没声」（已打过 warning）。
	 * @param bOutOverridden 这个音是从调用点的 SoundOverride 来的（true）还是表里查的（false）。
	 */
	UFUNCTION(BlueprintPure, Category = "Hero Audio")
	static USoundBase* ResolveSound(FGameplayTag EventTag, TSoftObjectPtr<USoundBase> Override, bool& bOutOverridden);

	/**
	 * 播一个事件音。世界坐标 Location，发音目标是 WorldContext 所在的 World。
	 * 和 UGameplayStatics::PlaySoundAtLocation 的区别：灵魂参数是个 Audio.* 标签，
	 * 音源是查表来的，缺失会留日志。
	 *
	 * 「这个事件走表」的调用点用三参版：
	 *   PlayAt(this, LOLGameplayTags::Audio_Dodge, Location);
	 * 只有某个点想自己钉死一个音时才带上第四参：
	 *   PlayAt(World, LOLGameplayTags::Audio_ThrowDaggerThrow, Loc, ThrowSound);
	 */
	UFUNCTION(BlueprintCallable, Category = "Hero Audio")
	static void PlayAt(const UObject* WorldContext, FGameplayTag EventTag, TSoftObjectPtr<USoundBase> Override, const FVector& Location);

	/**
	 * 三参便利版（override 留空 = 纯走表）。故意不做成第四参的默认值：
	 * UFUNCTION 的默认值要能被 UHT 解析成常量表达式，TSoftObjectPtr 的构造式过不了，
	 * 直接在 C++ 侧加个重载更省事，也不给蓝图多加一个带默认值的入口。
	 */
	static void PlayAt(const UObject* WorldContext, FGameplayTag EventTag, const FVector& Location)
	{
		PlayAt(WorldContext, EventTag, TSoftObjectPtr<USoundBase>(), Location);
	}

	/**
	 * 播一个事件音的【第 StageIndex 段】（0 起）—— 连段专用入口。
	 *
	 * 查 UHeroAudioConfig::ComboSoundByTag[EventTag][StageIndex]；这一段没配（越界 /
	 * 软引用失效 / 表里根本没这条）→ 一路退回 PlayAt(EventTag)，也就是事件表里的基础条目，
	 * 保证「忘了给第 2 段配音」不会变成静默无声，而是退回一句通用打击音。
	 *
	 * 段号从哪来：GameplayCue 的参数里没有「传音效资产」的槽位，命中音只能由 cue 自己查表，
	 * 所以施加方（GA_ThreeHitPassive::ExecuteMeleeHitCue）把段号塞进 CueParameters.RawMagnitude，
	 * UGC_MeleeHit 解出来传到这里。
	 */
	UFUNCTION(BlueprintCallable, Category = "Hero Audio")
	static void PlayAtStage(const UObject* WorldContext, FGameplayTag EventTag, int32 StageIndex, const FVector& Location);

	/**
	 * 按名字要一个 Audio.* 事件标签。
	 *
	 * ⚠️ 存在的理由：UE 5.8 的 Python 侧造不出 FGameplayTag（request_gameplay_tag 没了，
	 * GetPlayLevel... 相关的 BlueprintGameplayTagLibrary 也没暴露「按名字造标签」这一条），
	 * 而填 DA_HeroAudioConfig 这种「TMap<FGameplayTag, ...>」属性的脚本必须有 tag 对象当键。
	 * 于是给这条只留的口子 —— 它走的是引擎自己的 RequestGameplayTag，不是另搞一套命名规则。
	 *
	 * @param EventName "Audio.MeleeHit" 这种（只认完整名字，不做层级匹配）
	 * @return 找不到就返回空标签，调用方自己判断。
	 */
	UFUNCTION(BlueprintPure, Category = "Hero Audio")
	static FGameplayTag MakeEventTag(FName EventName);

	/**
	 * 启动时（PIE / 编辑器）体检：把表里没填、或填了但指向空的条目全列出来。
 * （表值是硬引用，资产被删会在 cook/编译期就暴露；运行时体检主要防手工填漏。）
	 * 收敛前「哪个槽位是空的」只能靠人肉记，现在一张表就能问清楚。
	 */
	UFUNCTION(BlueprintCallable, Category = "Hero Audio")
	static void ValidateConfig();
};
