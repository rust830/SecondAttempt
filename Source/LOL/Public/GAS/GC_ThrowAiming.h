// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_ThrowAiming.generated.h"

class UParticleSystem;
class UParticleSystemComponent;
class USoundBase;

/**
 * 匕首瞄准轮廓表现：进瞄准态时把粒子挂到右手匕首 socket 上，随抬刀动画走；退出时销毁。
 *
 * 用 Actor 变体是因为它跨一段时间存在、需要在 OnRemove 里回收自己生成的组件。
 * 只对本地控制端生成——轮廓是「我在瞄准」的操作反馈，敌人不该看到。
 *
 * 蓝图子类必须命名为 GC_ThrowAiming。
 */
UCLASS()
class LOL_API AGC_ThrowAiming : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()

public:
	AGC_ThrowAiming();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;

	/** 匕首轮廓粒子。 */
	UPROPERTY(EditDefaultsOnly, Category = "Aiming")
	TObjectPtr<UParticleSystem> ReticleFX;

	/** 轮廓要挂的 socket（右手匕首根部）。找不到会自动回退到 mesh 根。 */
	UPROPERTY(EditDefaultsOnly, Category = "Aiming")
	FName AttachSocketName = FName("dagger_a_r");


private:
	/** 本 cue 生成的轮廓组件，OnRemove 里销毁。 */
	UPROPERTY(Transient)
	TObjectPtr<UParticleSystemComponent> ReticleComponent;

	/** 本次瞄准是否已经放过抬手声。OnRemove 复位，见 OnActive 里的说明。 */
	bool bPlayedAimingSound = false;
};
