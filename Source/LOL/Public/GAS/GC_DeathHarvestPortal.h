// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_DeathHarvestPortal.generated.h"

class UParticleSystem;
class UParticleSystemComponent;
class USoundBase;

/**
 * 传送门：开在【服务器算出来的落点】上，人现身后关门。
 *
 * ★ 生命周期是「状态」不是「一次性」：UGA_DeathHarvest 在 P2 用 K2_AddGameplayCueWithParams
 *   挂上（带落点坐标）、P3 用 K2_RemoveGameplayCue 摘掉。别用 ExecuteGameplayCue 发这条 ——
 *   Actor 版 cue 的 Executed 事件走的是 OnExecute（GameplayCueNotify_Actor.cpp:285），
 *   而本类实现的是 OnActive/OnRemove，OnExecute 落到基类那个 `return false` 上：
 *   门一个粒子都不会出现、也不报错（v1 就是这么哑掉的）。
 *
 * ★ 服务器那一端收到的是 WhileActive（运行时 Add 的 cue 在权威端走 WhileActive，
 *   客户端才走 OnActive，见 AbilitySystemComponent.cpp:1601-1605），所以 WhileActive 也要转发过来。
 *
 * ⚠️ 这个类必须自己 SetActorLocation —— Actor 版 cue 的生成位置取的是 TargetActor 的坐标
 * （GameplayCueManager.cpp:528 用的是 TargetActor->GetActorLocation()），不是 Parameters.Location。
 * 不写这一行的表现是"传送门开在自己脚下"，而且不报任何错。
 *
 * 蓝图子类必须命名为 GC_DeathHarvest_Portal。
 */
UCLASS()
class LOL_API AGC_DeathHarvestPortal : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()
public:
	AGC_DeathHarvestPortal();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	/** 运行时 Add 的 cue 在服务器走的是 WhileActive（基类那条是空实现），转给 OnActive。 */
	virtual bool WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 门开着期间的门体循环粒子（Cascade）。Paragon: P_Ult_Teleport_Enter / P_Ultimate_Portal。 */
	UPROPERTY(EditDefaultsOnly, Category="Portal") TObjectPtr<UParticleSystem> OpenParticle;

	/** 关门的一次性粒子。由 OnRemove 以【独立发射器】生成，所以它比本 actor 活得久。 */
	UPROPERTY(EditDefaultsOnly, Category="Portal") TObjectPtr<UParticleSystem> CloseParticle;

	UPROPERTY(EditDefaultsOnly, Category="Portal") TObjectPtr<USoundBase> OpenSound;
	UPROPERTY(EditDefaultsOnly, Category="Portal") TObjectPtr<USoundBase> CloseSound;

	/** 门体的相对偏移（想把门压到地面上时用）。 */
	UPROPERTY(EditDefaultsOnly, Category="Portal") FVector RelativeOffset = FVector::ZeroVector;

private:
	UPROPERTY(Transient) TObjectPtr<UParticleSystemComponent> LoopComp;
	void DestroyLoop();
};
