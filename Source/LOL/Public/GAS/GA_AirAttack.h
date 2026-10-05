// 空中攻击（跳跃后按左键）。双形态共用一个能力，动画在起手时按形态选。
// ===========================================================================
// 【为什么单独一个类，而不是两个能力装两个槽位】
// 需求是「空手和持剑都能在跳跃后按左键打一拳」，但两种形态的差别**只有动画**：
// 伤害、控制、命中判定、收招全部一样。所以做成
// 「一个能力 + 两个 Montage 属性」，而不是「两个能力 + 两个槽位 + 形态切换时换装配」。
// 后者要动 GA_FormSwitch 的装配逻辑（GE 复制时序还有坑），收益为零。
//
// 【和 UGA_ThreeHitPassive 的分工】
//   地面：持剑 = 三连普攻（GA_ThreeHitPassive + 3 段数据）
//         空手 = 四连拳（GA_ThreeHitPassive + 4 段数据，同一个 C++ 类，只是数据不同）
//   空中：两形态都是本能力（单段、无完美窗口）
// 路由在 AHeroCombatCharacter::RouteBasicAttackInput 里按「是否在空中 + 当前形态」选。
//
// 【为什么继承 UGA_FormMelee 而不是 UGA_ThreeHitPassive】
// 空中是【单段、不接输入、不进连段窗口】，而 ThreeHitPassive 的整套机制（阶段推进、
// 输入缓冲、完美窗口 QTE、破隐强化）在这里全都用不上。而 FormMelee 恰好就是
// 「单段近战」的形状：锁姿态 → 播 montage → 等 notify → 球形扫描 → 伤害+控制+cue → 收招。
// 继承它只需要覆盖两件事：选哪个 montage、要不要锁移动（见 ShouldLockMovement）。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GAS/GA_FormMelee.h"
#include "GA_AirAttack.generated.h"

class UAnimMontage;

/**
 * 空中攻击。派生 BP 里配 Stages（1 段）+ 伤害/控制数值。
 *
 * 激活方式：AHeroCombatCharacter::RouteBasicAttackInput 判到角色在空中时
 * `TryActivateAbility(Ability.Slot.AirAttack 的句柄)`。所以 ActivationPolicy 是
 * OnInputTriggered（走显式激活），不是 OnEvent —— 它不订阅 Event.Input.BasicAttack。
 */
UCLASS(Abstract)
class LOL_API UGA_AirAttack : public UGA_FormMelee
{
	GENERATED_BODY()

public:
	UGA_AirAttack();

	// ⚠️ 参数一律【按值】传，必须和基类 UGA_FormMelee::ActivateAbility 的签名逐字一致。
	//   写成 const 引用（const FGameplayAbilitySpecHandle& Handle）会编译失败，
	//   报错是「包含重写说明符"override"的方法没有重写任何基类方法」——
	 //   那个报错完全看不出是签名不匹配，很容易误以为是头文件没 include。
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	/**
	 * 【必须返回 false】空中锁移动会把角色从跳跃轨迹上拽下来。
	 *
	 * 父类的 ApplyStanceLock 会 StopMovementImmediately + DisableMovement ——
	 * 地面那套是对的（出手定住），空中那套就是「按一下攻击，人悬停半空」。
	 * 父类 EndAbility 那侧不用管：ApplyStanceLock(false) 有 bMovementLocked 闩，
	 * 没锁过时它自己什么都不做。
	 */
	virtual bool ShouldLockMovement() const override { return false; }

protected:
	/**
	 * 空手形态的空中攻击（AM_Combo_Air_Kallari，ParagonCrunch 的 Ability_Attack_Air 重定向，0.933s）。
	 * 留空 = 空手形态这一下没有动画（仍然会结算伤害，只是没有视觉）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Anim")
	TObjectPtr<UAnimMontage> UnarmedMontage;

	/**
	 * 持剑形态的空中攻击（AM_Melle_Air，用 Kallari 自带的 Attack_Melee_Air，1.0s）。
	 * 留空 = 持剑形态没有动画。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Anim")
	TObjectPtr<UAnimMontage> ArmedMontage;
};
