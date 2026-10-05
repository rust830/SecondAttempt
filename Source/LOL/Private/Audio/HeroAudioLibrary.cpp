// Fill out your copyright notice in the Description page of Project Settings.

#include "Audio/HeroAudioLibrary.h"

#include "Audio/HeroAudioConfig.h"
#include "GameplayTagsManager.h"   // UGameplayTagsManager —— 见 MakeEventTag
#include "Sound/SoundBase.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtrTemplates.h"
#include "UObject/UObjectGlobals.h"

// 默认配置资产路径。写死在代码里是有意的：它是「找不到配置」这一路径本身的兜底，
// 不能再依赖另一份可配置的东西去指它。
static const TCHAR* const HeroAudioDefaultConfigPath = TEXT("/Game/LOL/Audio/DA_HeroAudioConfig.DA_HeroAudioConfig");

// 配置缓存。DA_HeroAudioConfig 全项目没有任何 UPROPERTY 硬引用它（这也是当初 cook 漏掉它
// 的同一个原因），所以必须由这里钉住，否则打包版第一次 GC 就把它回收、指针悬空。
//
// ⚠️ 钉它的方式：TStrongObjectPtr —— 5.8 里它底层是 UObject 自己的引用计数
// （TStrongObjectPtr::Reset → UObjectBase::AddRef → FUObjectItem::AddRef），
// GC 把它当根，不依赖「类的 AddReferencedObjects 会不会被调用」，也不依赖 TObjectPtr 的
// 句柄解析状态。前一版用的是 TObjectPtr + 手写 AddReferencedObjects 静态钩子（UHT 那条
// UOBJECT_CPPCLASS_STATICFUNCTIONS_FORCLASS 确实接上了，Editor/Game 两个 target 的 gen.cpp
// 也一模一样），但打包版实测仍然没钉住：2026-10-05 崩在 GetConfig 的 IsValid 里，
// 对象数组查表时踩到未分配的 chunk（FUObjectItem 里的下标是垃圾值）——即那个指针已经是
// 被回收/复用的内存。别再退回「裸静态 + 手写 ARO」那条路。
static TStrongObjectPtr<UHeroAudioConfig> GHeroAudioConfig;

/** 已经报过「没配」的 tag —— 见 .h 里关于刷屏的说明。 */
static TSet<FGameplayTag> GHeroAudioMissingReported;

// ---------------------------------------------------------------- 内部工具

static void PlaySoundAtVolume(UWorld* World, USoundBase* Sound, const FVector& Location, float Volume)
{
	if (World && Sound)
	{
		UGameplayStatics::PlaySoundAtLocation(World, Sound, Location, Volume);
	}
}

/**
 * 主层播完之后补一层「角色语气词」（UHeroAudioConfig::SecondaryLayerByTag）。
 * 主次关系在配置里定（打击音 1.0 / 语气词 0.4），这里只负责照着播；
 * 事件没登记次层就什么都不发生，行为和以前完全一样。
 */
static void TryPlaySecondaryLayer(UWorld* World, FGameplayTag EventTag, const FVector& Location)
{
	const UHeroAudioConfig* Config = UHeroAudioLibrary::GetConfig();
	if (!Config)
	{
		return;
	}
	if (const FHeroAudioSecondaryLayer* Layer = Config->SecondaryLayerByTag.Find(EventTag))
	{
		if (Layer->Volume > 0.f)
		{
			PlaySoundAtVolume(World, Layer->Sound.Get(), Location, Layer->Volume);
		}
	}
}

void UHeroAudioLibrary::SetConfig(UHeroAudioConfig* InConfig)
{
	GHeroAudioConfig.Reset(InConfig);
	GHeroAudioMissingReported.Reset();
}

UHeroAudioConfig* UHeroAudioLibrary::GetConfig()
{
	// IsValid = 判空 + 挡 PendingKill。正常路径 TStrongObjectPtr 已把引用计数加上、
	// 永远走不进重载分支；留着它是给「指针被作废」的边缘情况一条自愈路径。
	UHeroAudioConfig* Cached = GHeroAudioConfig.Get();
	if (!IsValid(Cached))
	{
		GHeroAudioConfig.Reset(Cast<UHeroAudioConfig>(StaticLoadObject(UHeroAudioConfig::StaticClass(), nullptr, HeroAudioDefaultConfigPath)));
		if (!GHeroAudioConfig)
		{
			UE_LOG(LogTemp, Warning, TEXT("[HeroAudio] 找不到默认配置 %s —— 所有事件音都会落空（音效请补到 /Game/LOL/Audio/DA_HeroAudioConfig）"), HeroAudioDefaultConfigPath);
		}
		else
		{
			// 走到这里说明缓存里的配置被回收过。以前这条是静默的，结果打包版崩了半年都不知道
			// 是 GC；现在留一行日志，下次再崩能直接看出是不是又没钉住。
			UE_LOG(LogTemp, Warning, TEXT("[HeroAudio] 配置缓存失效后重新加载成功 —— 上一次那份被 GC 回收了，检查 GHeroAudioConfig 的钉法"));
		}
	}
	return GHeroAudioConfig.Get();
}

USoundBase* UHeroAudioLibrary::ResolveSound(FGameplayTag EventTag, TSoftObjectPtr<USoundBase> Override, bool& bOutOverridden)
{
	bOutOverridden = false;

	if (USoundBase* Direct = Override.LoadSynchronous())
	{
		bOutOverridden = true;
		return Direct;
	}

	if (UHeroAudioConfig* Config = GetConfig())
	{
		if (const TObjectPtr<USoundBase>* Found = Config->SoundByTag.Find(EventTag))
		{
			if (USoundBase* Sound = Found->Get())
			{
				return Sound;
			}
		}

		if (USoundBase* Fallback = Config->FallbackSound.Get())
		{
			return Fallback;
		}
	}

	if (!GHeroAudioMissingReported.Contains(EventTag))
	{
		GHeroAudioMissingReported.Add(EventTag);
		UE_LOG(LogTemp, Warning, TEXT("[HeroAudio] 事件音没配：%s（表里这条是空的，或资产没进包 —— 编辑器里好着、打包后没声就是后者）→ 这个事件没声音"), *EventTag.ToString());
	}

	return nullptr;
}

void UHeroAudioLibrary::PlayAt(const UObject* WorldContext, FGameplayTag EventTag, TSoftObjectPtr<USoundBase> Override, const FVector& Location)  // 三参便利版见 .h，转发到这里
{
	// const 是为了让 OnExecute_Implementation 这类 const 播放点能直接传 this（之前散落调用点
	// 有的传 this、有的传 World，正是这种不一致让「为什么这里没声」更难查）。
	if (!WorldContext)
	{
		return;
	}

	UWorld* World = WorldContext->GetWorld();
	if (!World)
	{
		return;
	}

	bool bOverridden = false;
	USoundBase* Sound = ResolveSound(EventTag, Override, bOverridden);
	if (Sound)
	{
		PlaySoundAtVolume(World, Sound, Location, 1.f);
	}

	// 次层（角色语气词）在主层之后立即补上；主层没配也要播 —— 命中点的「人声点缀」
	// 不该因为打击音缺配而整个消失。TryPlaySecondaryLayer 自己按表判断。
	TryPlaySecondaryLayer(World, EventTag, Location);
}

FGameplayTag UHeroAudioLibrary::MakeEventTag(FName EventName)
{
	// 引擎自带的按名字取标签（会走 GameplayTag.ini 里的重定向 / 已删除标签的兜底）。
	return UGameplayTagsManager::Get().RequestGameplayTag(EventName, /*bTagNotFoundShouldBeZero=*/false);
}

void UHeroAudioLibrary::PlayAtStage(const UObject* WorldContext, FGameplayTag EventTag, int32 StageIndex, const FVector& Location)
{
	if (!WorldContext || StageIndex < 0)
	{
		return;
	}

	UWorld* World = WorldContext->GetWorld();
	if (!World)
	{
		return;
	}

	USoundBase* Sound = nullptr;
	if (const UHeroAudioConfig* Config = GetConfig())
	{
		if (const FHeroAudioComboStage* Combo = Config->ComboSoundByTag.Find(EventTag))
		{
			if (Combo->Sounds.IsValidIndex(StageIndex))
			{
				Sound = Combo->Sounds[StageIndex].Get();
			}
		}
	}

	// 这一段没配（下标越界 / 软引用失效 / 表里没这条事件）→ 退回事件表的基础条目。
	// 刻意不打新日志：连段哪一段缺音是配置问题，走下面 PlayAt 的按 tag 去重 warning 就够了。
	if (!Sound)
	{
		bool bOverridden = false;
		Sound = ResolveSound(EventTag, TSoftObjectPtr<USoundBase>(), bOverridden);
	}

	PlaySoundAtVolume(World, Sound, Location, 1.f);
	TryPlaySecondaryLayer(World, EventTag, Location);
}

void UHeroAudioLibrary::ValidateConfig()
{
	UHeroAudioConfig* Config = GetConfig();
	if (!Config)
	{
		UE_LOG(LogTemp, Warning, TEXT("[HeroAudio] 无法体检：配置文件没加载起来"));
		return;
	}

	int32 EmptyCount = 0;
	for (const TPair<FGameplayTag, TObjectPtr<USoundBase>>& Pair : Config->SoundByTag)
	{
		if (!Pair.Value.Get())
		{
			UE_LOG(LogTemp, Warning, TEXT("[HeroAudio] 空音效条目：%s"), *Pair.Key.ToString());
			++EmptyCount;
		}
	}

	if (!Config->FallbackSound.Get())
	{
		UE_LOG(LogTemp, Warning, TEXT("[HeroAudio] FallbackSound 没填 —— 表里没登记的事件会直接落空"));
		++EmptyCount;
	}

	UE_LOG(LogTemp, Log, TEXT("[HeroAudio] 体检完成：%d 个事件，%d 条空/无效"), Config->SoundByTag.Num(), EmptyCount);
}
