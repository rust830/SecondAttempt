// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GC_DeathHarvestBurst.generated.h"

class UCameraShakeBase;
class UParticleSystem;
class USoundBase;

/**
 * 三个一次性 burst（现身 / 命中 / 原地消失）共用的实现：在 Parameters.Location 放一个粒子 + 一声音效，
 * 朝向用 Parameters.Normal（命中点贴着墙面/地面朝外喷）。
 *
 * 为什么三个类都留着而不是共用一个：cue 的标签是按【类名】推导的
 * （LOLGameplayTags.h:70-72），同一个 C++ 类派出来的三个蓝图会被引擎重新推导成同一个无效标签。
 * 所以子类必须存在，只是它们每个只有四行（构造函数里设自己的 GameplayCueTag）。
 *
 * 这些 cue 都由 UGA_DeathHarvest 在服务器 ExecuteGameplayCue → 多播到各客户端各自播一次
 * （和 GC_ThrowDaggerHit 同一条路：命中结算只在服务端跑，就地 Spawn 的话粒子只有主机看得到）。
 */
UCLASS(Abstract)
class LOL_API UGC_DeathHarvestBurst : public UGameplayCueNotify_Static
{
	GENERATED_BODY()
public:
	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	/** 一次性粒子（Cascade）。 */
	UPROPERTY(EditDefaultsOnly, Category="Burst")
	TSoftObjectPtr<UParticleSystem> Particle;

	/** 一次性音效（可空）。用软引用：默认值只是路径，编辑器里随时换。 */

	/** 粒子相对命中点抬高多少（有些特效原点在脚底）。 */
	UPROPERTY(EditDefaultsOnly, Category="Burst")
	FVector LocationOffset = FVector::ZeroVector;

	/**
	 * 镜头震动（可空）。只在【施法者本机】真的摇 —— 这条 cue 每台机器都会跑一遍
	 *（服务器 ExecuteGameplayCue 是多播），不判的话挨打的那个人的屏幕也会跟着震，
	 * 而「被打的反馈」是另一条 cue 的事。
	 *
	 * 打在 GC_DeathHarvest_Hit 上就是"每一跳命中都震一下"，那是最直接的手感来源；
	 * 一刀不砍地用同一个值，所以想区分"起手命中"和"后续命中"得拆成两条 cue（现在没拆）。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Burst")
	TSubclassOf<UCameraShakeBase> CameraShake;
};

/** 现身：她从门里出来那一下（落点）。BP: GC_DeathHarvest_Appear。 */
UCLASS()
class LOL_API UGC_DeathHarvestAppear : public UGC_DeathHarvestBurst
{
	GENERATED_BODY()
public:
	UGC_DeathHarvestAppear();
};

/** 每跳命中：范围伤害打到人身上（命中点 + 法线）。BP: GC_DeathHarvest_Hit。 */
UCLASS()
class LOL_API UGC_DeathHarvestHit : public UGC_DeathHarvestBurst
{
	GENERATED_BODY()
public:
	UGC_DeathHarvestHit();
};

/** 原地消失：开门的同一时刻她在老位置消失那一下。BP: GC_DeathHarvest_TeleportOut。 */
UCLASS()
class LOL_API UGC_DeathHarvestTeleportOut : public UGC_DeathHarvestBurst
{
	GENERATED_BODY()
public:
	UGC_DeathHarvestTeleportOut();
};
