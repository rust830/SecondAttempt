// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_EmpoweredAttack.generated.h"

class UAnimInstance;
class UAnimMontage;
class UParticleSystem;
class USceneComponent;
class USoundBase;

/**
 * 强化普攻状态的表现：挂上时播一个强化 Montage + 音效 + 刀上粒子，收掉时停 Montage（可选再给个音效/粒子）。
 *
 * 粒子挂在两把刀的刀根插槽上（SwordSocketLeft / SwordSocketRight），和 GC_Stealth 的刀根粒子同一套做法：
 * 左右各一份、朝向分开配（右手插槽是镜像的）。
 * 和隐身那套的区别是不加 OnlyOwnerSee —— 强化状态是在 State.Stealth 归零之后才挂上的
 * （见 UGA_Stealth::OnStealthTagChanged），这时候人已经重新露面了，敌人看得见才对。
 *
 * 生命周期挂在 UGE_EmpoweredAttack 的 GameplayCues 上：GE 挂上 → OnActive，
 * GE 被移除（下次普攻命中消耗掉 / 窗口自然到期）→ OnRemove，两条触发路径
 * （GA_Stealth 破隐、GA_ThreeHitPassive 完美窗口）都不用各自操心表现。
 *
 * 用 Actor 变体而不是 Static：它要跨一段时间存在，并且得在 OnRemove 里把自己播的 Montage 收掉。
 * cue 在每个客户端各跑一次，所以敌我双方都看得到强化动作，不需要复制 Montage。
 * 只想给本人看的话，在蓝图子类里自己加 IsLocallyControlled 判断。
 *
 * 蓝图子类必须命名为 GC_EmpoweredAttack，见 LOLGameplayTags.h 里的说明。
 */
UCLASS()
class LOL_API AGC_EmpoweredAttack : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()

public:
	AGC_EmpoweredAttack();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;

	/** 强化状态期间播的 Montage（可空：只出声不出动作）。Paragon: Montage_EnhancedAttack。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack")
	TObjectPtr<UAnimMontage> EmpowerMontage;

	/** Montage 播放速率（1 = 原速，可以配攻速倍率）。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack")
	float MontagePlayRate = 1.f;

	/**
	 * 播 Montage 时是否停掉角色身上其它 Montage。
	 * 默认 false：完美窗口那条路径是在普攻命中后才挂状态的，正打着的那段普攻 Montage 不该被切断。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack")
	bool bStopOtherMontages = false;

	/** OnRemove 时停 Montage 的淡出时间（秒）。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack", meta = (ClampMin = "0"))
	float MontageStopBlendOut = 0.25f;

	/** 挂上强化状态时的一次性音效（可空）。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack")
	TObjectPtr<USoundBase> EnterSound;

	/** 状态被消耗 / 到期时的一次性音效（可空）。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack")
	TObjectPtr<USoundBase> ExitSound;


	/** 挂上强化状态时，在两把刀根上各放一份的一次性 Cascade（可空）。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack|VFX")
	TObjectPtr<UParticleSystem> EnterParticle;

	/** 强化被消耗 / 到期时，在两把刀根上各放一份的一次性 Cascade（可空）。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack|VFX")
	TObjectPtr<UParticleSystem> ExitParticle;

	/** 上面两个粒子挂的两个插槽（Kallari 是双刀），和 GC_Stealth 用同一组。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack|VFX")
	FName SwordSocketLeft = TEXT("sword_base_l");

	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack|VFX")
	FName SwordSocketRight = TEXT("sword_base_r");

	/**
	 * 粒子相对插槽的朝向。
	 *
	 * SnapToTarget 时这个值直接落成组件的相对旋转（相对插槽），跟着刀走。
	 * 左右分开配是因为右手插槽一般是左手的镜像：同一个粒子挂上去，「沿刀面」的那个方向会翻过来
	 * （朝着刀背而不是刀刃）。右手反了就调 ParticleRotationRight —— 通常是绕刀刃长轴转 180°。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack|VFX")
	FRotator ParticleRotationLeft = FRotator::ZeroRotator;

	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack|VFX")
	FRotator ParticleRotationRight = FRotator::ZeroRotator;

	/** 粒子缩放（1 = 原大小）。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredAttack|VFX")
	FVector ParticleScale = FVector(1.f);

private:
	/** 取目标角色的 AnimInstance（不是 Character / 没有 mesh 时返回 null）。 */
	UAnimInstance* GetTargetAnimInstance(AActor* Target) const;

	/** 在目标身上播 EmpowerMontage（没配则只打一条警告）。 */
	void PlayEmpowerMontage(AActor* Target);

	/** 停掉本 cue 播的 EmpowerMontage（没在播时是 no-op）。 */
	void StopEmpowerMontage(AActor* Target);

	/** 在目标位置播一个一次性音效（可空，静默跳过）。 */
	void PlaySoundAt(AActor* Target, USoundBase* Sound) const;

	/** 粒子统一挂在角色网格上跟着身体走；没有网格就退回 root。 */
	USceneComponent* ResolveAttachComponent(AActor* Target) const;

	/** 在 SwordSocketLeft / SwordSocketRight 上各放一份一次性 Cascade（可空，静默跳过；播完自毁）。 */
	void SpawnSwordParticles(AActor* Target, UParticleSystem* Particle) const;

	/** 「Montage 没配」的警告只打一次，避免反复挂状态刷屏。 */
	bool bLoggedMissingMontage = false;
};
