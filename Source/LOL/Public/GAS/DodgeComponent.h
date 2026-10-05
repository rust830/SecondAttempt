// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "GameplayEffectTypes.h"
#include "DodgeComponent.generated.h"

class UAbilitySystemComponent;
class UGameplayEffect;
class UCameraModifier;

/**
 * 完美闪避：闪避的无敌窗口真的挡掉一击时，回蓝 + 子弹时间 + 镜头推近。
 *
 * 【窗口状态不在这里】窗口是 GE 标签 State.Dodge.Window，由 GE_DodgeWindow 授予，
 * 时长 = 闪避技能上的 PerfectDodgeWindow。本组件只做三件事：
 *   判定（TryNegateIncomingDamage）+ 结算奖励（回蓝 / 表现）+ 收尾（子弹时间还原）。
 * 结构和 UBlockComponent 是刻意对称的 —— 同一类判定不要有两套写法。
 *
 * 【唯一调用者是 UExecCalc_Damage】和格挡一样。刻意不做成 UFUNCTION：在唯一结算点之外
 * 另开一条伤害路径的话，那条路上的伤害这里永远拦不到，而且不报错。
 *
 * 【为什么判定必须在服务端】伤害只在服务端结算（见 ExecCalc 里的说明），
 * 所以窗口标签必须【会复制】—— 这也是它不像 State.Dodge.Active 那样用 loose 标签的原因。
 */
UCLASS(ClassGroup=(LOL), meta=(BlueprintSpawnableComponent))
class LOL_API UDodgeComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UDodgeComponent();

	// ---------------------------------------------------------------------
	// 奖励：回蓝
	// ---------------------------------------------------------------------
	/** 完美闪避回多少能量。基础能量上限 200、基础回复 44/5s ⇒ 40 约等于 4.5 秒的自然回复。 */
	UPROPERTY(EditDefaultsOnly, Category="Dodge|Perfect", meta=(ClampMin="0"))
	float PerfectDodgeEnergyRestore = 40.f;

	/** 回蓝用的瞬时 GE。默认 UGE_EnergyRestore。 */
	UPROPERTY(EditDefaultsOnly, Category="Dodge|Perfect")
	TSubclassOf<UGameplayEffect> EnergyRestoreEffect;

	// ---------------------------------------------------------------------
	// 表现：子弹时间（世界流速）
	// ---------------------------------------------------------------------
	/** 完美闪避时是否慢放世界。 */
	UPROPERTY(EditDefaultsOnly, Category="Dodge|Perfect|Feel")
	bool bBulletTimeEnabled = true;

	/**
	 * 子弹时间的世界流速（1 = 不慢，0.35 = 世界慢到 35%）。
	 * 会被 WorldSettings 的 Min/MaxGlobalTimeDilation 夹一下，日志里打的是生效值。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Dodge|Perfect|Feel",
		meta=(ClampMin="0.05", ClampMax="1", EditCondition="bBulletTimeEnabled"))
	float BulletTimeScale = 0.35f;

	/**
	 * 子弹时间持续多久（【真实】秒，不是世界秒）。
	 * 0.4 秒：够看清「这一下擦着我过去了」，又不至于长到打断连招节奏。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Dodge|Perfect|Feel",
		meta=(ClampMin="0.05", Units="s", EditCondition="bBulletTimeEnabled"))
	float BulletTimeRealSeconds = 0.4f;

	// ---------------------------------------------------------------------
	// 表现：镜头（只有本地控制端会挂）
	// ---------------------------------------------------------------------
	/** 镜头推近用的修改器类。默认 UDodgeCameraModifier。 */
	UPROPERTY(EditDefaultsOnly, Category="Dodge|Perfect|Camera")
	TSubclassOf<UCameraModifier> CameraModifierClass;

	/**
	 * 这一击被完全吃掉时返回 true（调用方据此跳过落地扣血）。
	 * 就地改写 InOutDamage。
	 */
	static bool TryNegateIncomingDamage(UAbilitySystemComponent* TargetASC, AActor* DamageSource, float& InOutDamage);

	/**
	 * 把 ASC 交给组件。由 AHeroCombatCharacter::InitializeAbilityActorInfo 调用
	 *（和 UBlockComponent::BindToAbilitySystem 同一处、同一个理由：组件自己 BeginPlay 时 ASC 还没就绪）。
	 */
	void BindToAbilitySystem(UAbilitySystemComponent* InASC);

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 真的躲掉了：摘窗口、归零伤害、回蓝、放表现。只在权威端生效。 */
	bool OnPerfectDodge(AActor* DamageSource, float& InOutDamage);

	/** 子弹时间到点：世界流速还原 + 放掉镜头修改器（它会自己淡出）。 */
	void EndBulletTime();

	/** 在本机（且本机就是闪避者）挂镜头修改器。不是本地控制就什么都不做。 */
	void ApplyCameraEffect();

	/** 「等 RealSeconds 那么久的真实时间」= 等 RealSeconds × 当前流速 那么多的世界秒。 */
	float ScaledWorldSeconds(float RealSeconds) const;

	/** 注意是 AvatarActor 不是 OwnerActor：ASC 挂在 PlayerState 上，组件挂在角色身上。 */
	static UDodgeComponent* FindOn(UAbilitySystemComponent* ASC);

	UPROPERTY(Transient) TObjectPtr<UAbilitySystemComponent> CachedASC;

	/** 子弹时间的收尾定时器。EndPlay 兜底清。 */
	FTimerHandle BulletTimeTimer;

	/** 还挂着的镜头修改器（弱引用：管理器可能已经把它摘了）。 */
	TWeakObjectPtr<UCameraModifier> ActiveCameraModifier;

	/** 慢放前的世界流速（还原用）。 */
	float SavedTimeDilation = 1.f;

	/** 子弹时间还挂着（幂等还原 + 防止两次完美闪避叠两层）。 */
	bool bBulletTimeApplied = false;
};
