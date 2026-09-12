// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GameplayEffectTypes.h"
#include "TimerManager.h"
#include "ThrowDaggerAbility.generated.h"

class AThrowDaggerProjectile;
class UAbilityTask_PlayMontageAndWait;
class UGameplayEffect;
class UAnimMontage;
class UParticleSystem;
class UParticleSystemComponent;
class ACharacter;

/**
 * 投掷匕首：E 进瞄准 → 左键投掷 / E 再按取消。直线飞行、空中翻滚。
 */
UCLASS(Blueprintable)
class LOL_API UThrowDaggerAbility : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UThrowDaggerAbility();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CancelAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateCancelAbility) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool WasCanceled) override;

protected:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Projectile")
	TSubclassOf<AThrowDaggerProjectile> Dagger;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Projectile")
	float ThrowSpeed = 1800.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Damage")
	TSubclassOf<UGameplayEffect> DamageGE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Damage")
	float BaseDamage = 80.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Aim")
	TSubclassOf<UGameplayEffect> AimingGE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Anim")
	TObjectPtr<UAnimMontage> ThrowDaggerMontage;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|VFX")
	TObjectPtr<UParticleSystem> AimingReticleFX;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|VFX")
	TObjectPtr<UParticleSystemComponent> AimingReticleFXComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|VFX")
	float AimReticleDistance = 300.f;

	// 出手 socket（右手），匕首从这个 socket 的世界坐标射出。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Projectile")
	FName ThrowSocketName = FName("Melee_Impact_R");

	// 左键确认 → 真正出手的延迟（秒），要匹配 Cast 动画的出手帧，不能超过 Cast 段时长。
	// 同时就是「人物转向瞄准方向」的时长：出手帧正好转到位。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Anim")
	float CastThrowDelay = 0.35f;

	FActiveGameplayEffectHandle GEHandle;
	UPROPERTY() TObjectPtr<UAbilityTask_PlayMontageAndWait> MontageTask = nullptr;
	UPROPERTY() TObjectPtr<UParticleSystemComponent> AimReticleComponent = nullptr;
	FTimerHandle TurnTimer;        // 出手前平滑转向的逐帧 timer
	FTimerHandle ThrowDelayTimer;  // 左键确认 → 真正出手的延迟 timer

	// 左键确认后置 true；等 Cast 段播完（OnBlendOut/OnCompleted）才真正 End，否则 EndAbility 会立刻停掉 Montage。
	bool bThrowCommitted = false;

	// 确认时的瞄准方向（纯鼠标方向）与出手时人物的目标朝向，确认时锁定。
	FVector ThrowDirection = FVector::ForwardVector;
	FRotator TargetFacingRotation = FRotator::ZeroRotator;
	FRotator TurnStartRotation = FRotator::ZeroRotator; // 转向起点（确认时的朝向）
	float TurnElapsed = 0.f;                             // 转向已用时长（秒）

	// 出手期间临时关掉 bOrientRotationToMovement，让角色一直朝瞄准方向（只转一次），
	// 能力结束（ExitAimingState）才恢复，避免移动组件中途把角色转回移动方向造成「转两下/镜头抖动」。
	// bOrientRotationOverridden 标记是否真的关过，防止 ExitAimingState 在还没转向时误恢复成 false。
	bool bWasOrientRotationToMovement = false;
	bool bOrientRotationOverridden = false;

	// 出手后角色保持瞄准朝向的时长（秒）。这段时间内不恢复 bOrientRotationToMovement，
	// 避免移动组件在出手瞬间立刻把角色转回移动方向（"小修正"）。过了这段时间再恢复。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Anim")
	float PostCastHoldDuration = 0.3f;

	// 延迟恢复 bOrientRotationToMovement 的一次性 timer。
	FTimerHandle PostRestoreTimer;

	void EnterAimingState();
	void ExitAimingState();
	void StartTurnToFace(const FVector& AimDir);
	void TickTurnToFace();
	void RestoreOrientRotationToMovement();
	void FireDagger();
	FVector GetThrowSocketLocation(ACharacter* Character) const;
	UFUNCTION() void OnThrowPressed(FGameplayEventData Payload);
	UFUNCTION() void OnCancelPressed(FGameplayEventData Payload);
	UFUNCTION() void OnThrowMontageFinished();
	FVector GetAimingDirection() const;
	void SpawnProjectile(const FVector& AimDir);
};
