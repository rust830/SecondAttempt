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

	/**
	 * 能量消耗（扣 Energy）。0 = 免费（默认，行为和接上之前完全一致）。
	 *
	 * 【这条链怎么跑起来的】CommitAbility → CommitCheck → CheckCost（不够就直接失败、不进冷却）
	 * → CommitExecute → ApplyCost（扣）。项目里所有技能都调了 CommitAbility，所以填个数就生效，
	 * 不用在蓝图里再加节点。
	 *
	 * ⚠️ 没调 CommitAbility 的技能【不会】扣蓝：GA_DeathHarvest 在客户端那条分支是刻意不 Commit 的
	 * （见它的注释：本端再挂一份冷却会导致 CD 比服务端长一个 RTT），那条路径上消耗也就跟着不扣 ——
	 * 这是对的，真正施法是服务端那一次，扣一次就够。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mana", meta = (ClampMin = "0"))
	float ManaCost = 0.0f;

	/**
	 * 消耗 GE。默认 UGE_AbilityCost（扣 Energy，数值走 SetByCaller Data.Cost）。
	 *
	 * 留成可配是为了「消耗别的资源」的技能（比如耗血的技能）只换这一个 GE，
	 * 不用再改能力基类。留空 = 这个技能不扣任何东西（即便 ManaCost > 0）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mana")
	TSubclassOf<UGameplayEffect> CostGameplayEffect;

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
	 * 消耗够不够。ManaCost 为 0 / 没配 CostGameplayEffect 时一律通过（默认免费）。
	 *
	 * 【为什么直接比 Energy >= ManaCost，而不是走引擎的 CanApplyAttributeModifiers】
	 * 那条路会先把 Instant GE 的 modifier 全算一遍再判「有没有属性被钳到 0 以下」，
	 * 中间还夹着 Period / Stacking 的处理 —— 对「扣一个固定数」这种最简单的消耗是绕远路，
	 * 而且它对「刚好等于」的边界（Energy 正好 == Cost）判据不直观。
	 * 直接读属性一眼能看懂，失败时也能打出「当前多少 / 需要多少」。
	 */
	virtual bool CheckCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

	/**
	 * 真的扣。施加路径和 ApplyCooldown 完全一致（ApplyGameplayEffectSpecToOwner）：
	 * 它内部按预测键决定是本地预测还是权威施加，客户端不用额外写代码。
	 */
	virtual void ApplyCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const override;

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
