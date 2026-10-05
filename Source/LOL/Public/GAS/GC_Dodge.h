// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GC_Dodge.generated.h"

class UParticleSystem;
class USoundBase;

/**
 * 闪避表现：施法瞬间的喷射粒子（jet deploy）+ 可选音效。
 *
 * 用 Static 变体：放一个 burst 就完事，没有生命周期。
 * 粒子挂在角色 mesh 上（跟着人走）——喷射是背上的 booster，得跟身体一起动。
 * 由 GA_Dodge 在闪避起手时 ExecuteGameplayCue，多播到各客户端各播一次。
 *
 * 蓝图子类必须命名为 GC_Dodge。
 */
UCLASS()
class LOL_API UGC_Dodge : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_Dodge();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	/** 喷射粒子（Cascade）。默认 Kallari 的 BackJets 尾迹 burst。软引用：默认值只是路径，编辑器里随时换。 */
	UPROPERTY(EditDefaultsOnly, Category = "Dodge")
	TSoftObjectPtr<UParticleSystem> JetParticle;

	/** 喷射粒子挂的插槽（背喷 booster）。Kallari 的喷射点在 FX_Thruster_Center。 */
	UPROPERTY(EditDefaultsOnly, Category = "Dodge")
	FName JetSocketName = TEXT("FX_Thruster_Center");

	/** 喷射音效（可空）。 */
};
