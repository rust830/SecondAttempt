// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "GameplayEffectTypes.h"
#include "BlockComponent.generated.h"

class UAbilitySystemComponent;
class UGameplayEffect;

/**
 * 格挡 / 免疫的减免判定（方案文档 §3.4）。
 *
 * 挂在角色身上（AHeroCombatCharacter），状态本身【不在这里】—— 窗口和免疫都是 GE 标签：
 *   State.Blocking    由 GE_Blocking 授予，时长 = 窗口时长
 *   State.BlockImmune 由 GE_BlockImmune 授予，时长 = 免疫时长
 * 本组件只做两件事：判定（TryMitigateIncomingDamage）+ 记录窗口关闭时刻（联网宽限用）。
 *
 * 【唯一调用者是 UExecCalc_Damage】。刻意不做成 UFUNCTION：蓝图里调用它就等于在唯一结算点
 * 之外另开一条伤害路径，那部分伤害格挡永远拦不到，而且不报错。
 */
UCLASS(ClassGroup=(LOL), meta=(BlueprintSpawnableComponent))
class LOL_API UBlockComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UBlockComponent();

	/** 格挡成功时挂的免疫 GE。默认 UGE_BlockImmune。 */
	UPROPERTY(EditDefaultsOnly, Category="Block") TSubclassOf<UGameplayEffect> ImmuneEffect;

	/** 免疫持续时间（秒）。 */
	UPROPERTY(EditDefaultsOnly, Category="Block", meta=(ClampMin="0")) float ImmuneDuration = 1.5f;

	/** 被格挡那一击的伤害倍率（作为减免系数乘上去）。0 = 完全抵消（默认）。 */
	UPROPERTY(EditDefaultsOnly, Category="Block", meta=(ClampMin="0")) float BlockedDamageMultiplier = 0.f;

	/** 免疫期间的伤害倍率。0 = 完全免伤（默认）。 */
	UPROPERTY(EditDefaultsOnly, Category="Block", meta=(ClampMin="0")) float ImmuneDamageMultiplier = 0.f;

	/** 是否只挡正面来的伤害。默认关（宽松 > 误判）。 */
	UPROPERTY(EditDefaultsOnly, Category="Block|Facing") bool bRequireFacingAttacker = false;
	UPROPERTY(EditDefaultsOnly, Category="Block|Facing",
		meta=(ClampMin="0", ClampMax="180", EditCondition="bRequireFacingAttacker"))
	float FacingHalfAngleDeg = 90.f;

	// ---- 联网宽限（方案文档 §1.1）----
	// 判定跑在服务端，而服务端的窗口从「ServerTryActivateAbility 到达」才开始，比客户端本地预测晚半个 RTT。
	// 补偿方式：窗口关闭后再多认 RTT/2 内到达的伤害，且只认一次。
	// 【注意】宽限加在判定层，不是把 GE_Blocking 的时长加长 —— 那个时长是两端各自施加的，
	// 加长它会制造两端不一致的预测副本，而且客户端根本不判伤害，多留半秒没有意义。

	/** 服务端判定宽限：窗口关闭后再多认 RTT/2。关掉 = 严格按 GE 时长判（局域网调试用）。 */
	UPROPERTY(EditDefaultsOnly, Category="Block|Net") bool bCompensatePing = true;

	/** 宽限上限（秒）。防止高延迟/异常 ping 把「窗口」拉成一个能常驻的状态。 */
	UPROPERTY(EditDefaultsOnly, Category="Block|Net", meta=(ClampMin="0", Units="s"))
	float MaxGraceSeconds = 0.15f;

	/**
	 * ★ 唯一减免入口。就地改写 InOutDamage（乘减免系数），返回 true = 这一击被完全吃掉。
	 *
	 * 静态入口的理由和 UMyAbilitySystemComponent::RemoveGrantedTagEffects 一样：
	 * 手里只有裸 ASC 指针也能用（ExecCalc 拿到的就是 ExecutionParams 里的 ASC，拿不到组件引用）。
	 */
	static bool TryMitigateIncomingDamage(UAbilitySystemComponent* TargetASC, AActor* DamageSource, float& InOutDamage);

	/**
	 * 把 ASC 交给组件并开始跟踪窗口开关时刻。
	 * 由 AHeroCombatCharacter::InitializeAbilityActorInfo 调用 —— 组件自己 BeginPlay 时 ASC 可能还没就绪
	 * （ASC 挂在 PlayerState 上，PossessedBy / OnRep_PlayerState 的时序不保证）。
	 */
	void BindToAbilitySystem(UAbilitySystemComponent* InASC);

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 真的挡到了：摘窗口、乘减免、挂免疫。只在权威端生效。 */
	bool OnBlockSucceeded(AActor* DamageSource, float& InOutDamage);

	bool IsInFront(AActor* DamageSource) const;

	/** 本次宽限秒数 = 格挡者 ping / 2，上限 MaxGraceSeconds。没有 PlayerState（AI/木桩）时为 0。 */
	float GetGraceSeconds() const;

	/** 注意是 AvatarActor 不是 OwnerActor：ASC 挂在 PlayerState 上，组件挂在角色身上。 */
	static UBlockComponent* FindOn(UAbilitySystemComponent* ASC);

	/** State.Blocking 计数变化：0→1 开窗（作废宽限），1→0 记下关闭时刻并放开一次宽限。 */
	void OnBlockingTagChanged(const FGameplayTag Tag, int32 NewCount);

	UPROPERTY(Transient) TObjectPtr<UAbilitySystemComponent> CachedASC;
	FDelegateHandle BlockingTagDelegateHandle;

	/** 窗口关闭的服务端世界时刻。宽限判定的起点。 */
	float WindowCloseServerTime = -FLT_MAX;

	/** 这次关窗之后的那份宽限还没被用掉。用掉即作废，防止一次窗口连挡两下。 */
	bool bGraceAvailable = false;
};
