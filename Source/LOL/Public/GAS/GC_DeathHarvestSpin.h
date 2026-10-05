// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_DeathHarvestSpin.generated.h"

class UParticleSystem;
class UParticleSystemComponent;
class USoundBase;

/**
 * 转圈期间挂在身上的循环表现。
 *
 * 生命周期由 UGA_DeathHarvest 用 K2_AddGameplayCueWithParams(..., bRemoveOnAbilityEnd=true) 挂上，
 * 技能一结束（正常结束或被取消）引擎自动摘 —— 所以这里不需要任何"技能结束时清一下"的代码，
 * OnRemove / EndPlay 只需要管自己生成的那个组件。
 *
 * ★ WhileActive 必须转发到 OnActive：运行时 Add 的 cue 在【服务器那一端】收到的是 WhileActive、
 *   客户端收到的才是 OnActive（AbilitySystemComponent.cpp:1601-1605）。而基类的
 *   WhileActive_Implementation 是空实现、不会转到 OnActive —— 只写 OnActive 的后果是
 *   主机自己屏幕上转圈特效是空的，客户端却看得到，且引擎不报错。
 *
 * 蓝图子类必须命名为 GC_DeathHarvest_Spin。
 */
UCLASS()
class LOL_API AGC_DeathHarvestSpin : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()
public:
	AGC_DeathHarvestSpin();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	/** 服务器那一端走的是这条（运行时 Add 的 cue），转给 OnActive。理由见类注释。 */
	virtual bool WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 转圈期间绕身的循环粒子（Cascade）。Paragon: P_Ultimate_Rush_Wind。左右手各生成一份。 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") TObjectPtr<UParticleSystem> LoopParticle;

	/** 循环粒子挂的两个手部插槽（Kallari 是双刀，和 GC_Stealth 的刀根粒子同一套插槽名）。 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") FName LoopSocketLeft = TEXT("sword_base_l");
	UPROPERTY(EditDefaultsOnly, Category="Spin") FName LoopSocketRight = TEXT("sword_base_r");

	/**
	 * 相对各自插槽的偏移。左右分开配：右手插槽是左手的镜像，
	 * 「沿刀面往外推一点」这种偏移在两边往往不是同一个方向。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") FVector RelativeOffsetLeft = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category="Spin") FVector RelativeOffsetRight = FVector::ZeroVector;

	/** 两个粒子的缩放（共用）。 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") FVector Scale = FVector(1.f);

	/**
	 * 粒子相对插槽的朝向。
	 *
	 * 用的是 EAttachLocation::SnapToTarget —— 这个值直接落成组件的相对旋转，也就是【相对插槽】，
	 * 不是世界朝向，所以跟着刀走、不用管角色转不转身。
	 *
	 * 左右分开配是因为右手插槽一般是左手的镜像：同一个粒子挂上去，「沿刀面」的那个方向会翻过来
	 * （朝着刀背而不是刀刃）。右手反了就调 ParticleRotationRight —— 通常是绕刀刃长轴转 180°。
	 * （和 GC_Stealth 的 SwordParticleRotationLeft/Right 是同一套道理。）
	 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") FRotator ParticleRotationLeft = FRotator::ZeroRotator;
	UPROPERTY(EditDefaultsOnly, Category="Spin") FRotator ParticleRotationRight = FRotator::ZeroRotator;

	/** 起转的龙吼/音效（可空）。 */

private:
	/** 两只手各一份（找不到插槽的那一侧不生成）。 */
	UPROPERTY(Transient) TArray<TObjectPtr<UParticleSystemComponent>> LoopComps;
	void DestroyLoop();
};
