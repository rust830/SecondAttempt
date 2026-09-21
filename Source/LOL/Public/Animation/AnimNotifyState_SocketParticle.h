// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AnimNotifyState_SocketParticle.generated.h"

class UParticleSystem;
class UParticleSystemComponent;
class USkeletalMeshComponent;

/** 挂哪只手（和 BladeTrail 的 EBladeTrailSide 同义，只是这里没有"拖尾"的语义）。 */
UENUM()
enum class ESocketParticleSide : uint8
{
	Left  UMETA(DisplayName = "Left (左手)"),
	Right UMETA(DisplayName = "Right (右手)"),
	Both  UMETA(DisplayName = "Both (双手)"),
};

/**
 * 在动画区间里往插槽上挂一个 Cascade 粒子（起 → 挂上，止 → 摘掉）。
 *
 * 为什么是 notify 而不是 GameplayCue：蒙太奇在【每一台机器】上都会播（见
 * GAS_DeathHarvest_Setup.md §1.2），notify 于是天然在每台机器各跑一次 ——
 * 正好是"每个人都该看到刀上挂特效"想要的，不需要任何复制代码。
 *
 * ⚠️ 这个对象是蒙太奇资产里的实例，**所有角色共用同一个**，所以生成出来的组件必须按
 * MeshComp 分开存（ActiveParticles），不能放成员变量里 —— 否则两个人同时转圈会互相顶掉。
 *
 * ⚠️ bOnlyOwnerSee 默认 false（敌人也该看到刀光）。这和 GC_Stealth 的 SwordParticle 正好相反：
 * 隐身时刀上的特效必须只有主人可见，转圈时人已经现身了。抄那一段的时候最容易抄错的就是这个值。
 */
UCLASS(meta = (DisplayName = "Socket Particle (Cascade)"))
class LOL_API UAnimNotifyState_SocketParticle : public UAnimNotifyState
{
	GENERATED_BODY()
public:
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;

	/** 挂在插槽上的 Cascade 粒子（武器蓄能那套：P_Ultimate_Weapon_Charge）。 */
	UPROPERTY(EditAnywhere, Category="SocketParticle")
	TSoftObjectPtr<UParticleSystem> Particle;

	UPROPERTY(EditAnywhere, Category="SocketParticle")
	ESocketParticleSide Side = ESocketParticleSide::Both;

	/** Kallari 是双刀，插槽名和 GC_Stealth 里那对保持一致。 */
	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FName SocketLeft = TEXT("sword_base_l");

	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FName SocketRight = TEXT("sword_base_r");

	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FVector RelativeOffset = FVector::ZeroVector;

	/**
	 * 粒子相对插槽的朝向（EAttachLocation::SnapToTarget：直接落成组件的相对旋转，跟着刀走）。
	 * 左右分开配，因为右手插槽一般是左手的镜像 —— 右手反了就绕刀刃长轴转 180°。
	 * 和 GC_Stealth.h 里 SwordParticleRotation{Left,Right} 是同一个坑、同一套解法。
	 */
	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FRotator RelativeRotationLeft = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FRotator RelativeRotationRight = FRotator::ZeroRotator;

	/** true = 只有本人看得见。转圈期间【必须】是 false（人已经现身了，敌人该看到刀光）。 */
	UPROPERTY(EditAnywhere, Category="SocketParticle")
	bool bOnlyOwnerSee = false;

	/**
	 * NotifyEnd 之后等多久强制销毁组件。
	 * 正常路径是 Deactivate() + bAutoDestroy 让粒子自然收掉；但 Cascade 的 emitter 要是设成
	 * 无限循环，auto destroy 永远等不到 —— 那样每转一次就泄漏一个常驻组件，所以留个兜底。
	 */
	UPROPERTY(EditAnywhere, Category="SocketParticle", meta=(ClampMin="0.1", Units="s"))
	float TeardownDelay = 1.f;

private:
	/** 一次 notify 在某条 mesh 上生成的所有组件（Both 时两个）。 */
	struct FActiveSocketParticles
	{
		TArray<TObjectPtr<UParticleSystemComponent>> Comps;
	};

	/** 按 mesh 分开存：notify 实例是所有角色共用的。键用弱引用，mesh 没了自动失效。 */
	TMap<TWeakObjectPtr<USkeletalMeshComponent>, FActiveSocketParticles> ActiveParticles;

	/** 生成一侧。插槽不存在时打日志并返回 nullptr（GetSocketLocation 会静默返回组件位置）。 */
	UParticleSystemComponent* SpawnOne(USkeletalMeshComponent* MeshComp, UParticleSystem* System, bool bRight) const;

	/** 停掉一个组件并挂上兜底销毁的定时器。 */
	static void Teardown(UParticleSystemComponent* Component, float DelaySeconds);
};
