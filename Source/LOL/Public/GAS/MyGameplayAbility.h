// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "MyGameplayAbility.generated.h"

class UGameplayEffect;

/** How this ability is triggered. */
UENUM(BlueprintType)
enum class EMyAbilityActivationPolicy : uint8
{
	OnInputTriggered UMETA(DisplayName = "On Input Triggered"),
	OnEvent          UMETA(DisplayName = "On Event"),
	OnGiven          UMETA(DisplayName = "On Given"),
};

/**
 * Common base for all abilities: data-driven cooldown + activation policy.
 */
UCLASS(Blueprintable)
class LOL_API UMyGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()
public:
	UMyGameplayAbility();

	/** Cooldown in seconds. Applied via ApplyCooldown + SetByCaller Data.Cooldown. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cooldown", meta = (ClampMin = "0"))
	float CooldownDuration = 0.0f;

	/** Mana cost (reserved; hook up a resource attribute + cost GE to enable). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mana", meta = (ClampMin = "0"))
	float ManaCost = 0.0f;

	/** How this ability is triggered. The ASC's button routing only activates OnInputTriggered abilities. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation")
	EMyAbilityActivationPolicy ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	/** 施放本技能是否破除隐身（State.Stealth）。隐身技能自身设 false，避免再按误破。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Stealth")
	bool bBreaksStealthOnCast = true;

	/**
	 * 「按住槽位键选目标，左键点中才放」的能力标记（GA_DeathHarvest 的大招就是这条链）。
	 *
	 * true 时槽位按键【不激活本能力】：AHeroCombatCharacter::AbilityInputTagPressed 会拦下这次按键，
	 * 只开一个本地的选择窗口（State.DeathHarvest.Selecting）；左键点在谁身上，就把谁当载荷发到服务端
	 * 去激活（UMyAbilitySystemComponent::SubmitManualTargetOnServer）—— 按下这一刻不占冷却、不锁移动、
	 * 不发 cue、也不破隐，真正施法要等点中目标那一下。
	 *
	 * 所以这类能力的 ActivationPolicy 必须配 OnEvent（槽位按键路由会跳过非 OnInputTriggered 的能力），
	 * 并在 AbilityTriggers 里声明一条 GameplayEvent 触发（标签订 Event.DeathHarvest.CastAt 这种）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Activation")
	bool bManualTargetSelect = false;

	/**
	 * 取本能力声明的事件触发标签（AbilityTriggers 里第一条 GameplayEvent）。
	 *
	 * 给「外部要把载荷送进来激活它」的调用方用：UMyAbilitySystemComponent 读不到 protected 的
	 * AbilityTriggers，而且它也不该认识任何具体技能 —— 让能力自己声明「我要听哪个标签」。
	 * 没声明时返回无效标签（调用方自己判断并打日志）。
	 */
	FGameplayTag GetGameplayEventTriggerTag() const;

	virtual void ApplyCooldown(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const override;

	/**
	 * 给自己挂「强化普攻」状态（State.EmpoweredAttack），持续 Duration 秒。
	 *
	 * 破隐（GA_Stealth）和三连击完美窗口（GA_ThreeHitPassive）共用这一段：两者只是「什么时候挂、挂多久」
	 * 不同，施加方式一样。强度倍率/击退力度不在 GE 上，统一在 UThreeHitPassiveData（唯一的消费者是普攻结算方）。
	 *
	 * 两端都施加，用的是「调用方能力的激活预测键」（GetCurrentActivationInfo().GetActivationPredictionKey()）：
	 * 客户端本地预测挂上 → tag 当帧有效，起手判断和表现不用等 GE 复制过来；服务端用同一个键挂，
	 * 复制回客户端时预测实例被 catch up 合并，不会叠成两份。
	 * 不能用 ApplyGameplayEffectSpecToOwner：它取 GetPredictionKeyForNewAction()，而这里的两个调用点
	 * （隐身归零回调、连段窗口定时器）都在预测窗口外，客户端会拿到无效键 → 静默不生效。
 *
 * 重复挂按「刷新窗口」处理：先摘掉旧的同名状态再挂新的，任何时刻最多一层 GE。
 * （叠加的话时长各算各的，而且 cue 的 OnRemove 会在第一层到期时提前触发。）
	 */
	void ApplyEmpoweredAttack(TSubclassOf<UGameplayEffect> InEmpoweredAttackGE, float Duration);

	/**
	 * 把 CooldownDuration 按技能急速换算成实际 CD：CD × 100/(100+AbilityHaste)（属性集.txt 的口径）。
	 *
	 * 拿不到属性集时【返回原值】：召唤师技能在 PlayerState 的 BeginPlay 里授予，那一刻
	 * InitAbilityActorInfo 还没跑、GetSet 是空的 —— 返回 0 会让「PlayerState 还没就绪」变成「技能没 CD」。
	 */
	float ComputeCooldownWithAbilityHaste(const FGameplayAbilityActorInfo* ActorInfo) const;
};
