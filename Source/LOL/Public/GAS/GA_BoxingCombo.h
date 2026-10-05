// 空手四连拳。数据（4 段蒙太奇 / 收招 / 连招窗口）全在 UThreeHitPassiveData 资产里配，
// 这个类只负责一件父类做不到的事：把输入事件换成空手专用的那一条。
// ===========================================================================
// 【为什么复用 UGA_ThreeHitPassive 而不是新写一个】
// 连段推进、输入缓冲、完美窗口 QTE、命中 notify、Recovery 收招 —— 这些机制
// 「四段」和「三段」没有任何差别，代码里也全都是按 Stages.Num() 通用写的。
// 新写一个就是 350 行几乎逐字重复的代码，而两处重复迟早会各自改出不同的行为。
//
// 【这个子类唯一做的事：换输入标签】
// 父类监听 Event.Input.BasicAttack（持刀三连普攻）。四连拳必须听
// Event.Input.ComboAttack —— 两者都装在同一个角色上时，共用一个标签的话
// 空手按一次左键会同时唤醒两个能力，一次打两段伤害。详见 LOLGameplayTags.h 里
// Event.Input.ComboAttack 的注释。
//
// 【BP 里要配什么】
//   PassiveData  → 一份 4 段的 UThreeHitPassiveData（DA_ThreeHitPassive_Boxing）
//   其余（伤害 GE / 半径 / QTE 相关）→ 留空或照三连普攻的 BP 抄
//   ⚠️ BP 里的 AttackInputTag 必须【留空】：BP 的值会覆盖 C++ 构造函数设的，
//      而三连普攻的 BP 里填的是 Event.Input.BasicAttack —— 忘了清空就会双触发。
//      （本类在 ActivateAbility 里还会再强制设一次兜底，但 BP 里填着会让读配置的人困惑。）
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GAS/GA_ThreeHitPassive.h"
#include "GA_BoxingCombo.generated.h"

/**
 * 空手形态的地面四连拳（Combo_01~04 + 打满后的 Recovery 收招）。
 * 装在 Ability.Slot.Combo 槽位上。
 */
UCLASS(Abstract)
class LOL_API UGA_BoxingCombo : public UGA_ThreeHitPassive
{
	GENERATED_BODY()

public:
	UGA_BoxingCombo();

	// ⚠️ 参数一律【按值】传，和基类 UGA_ThreeHitPassive::ActivateAbility 逐字一致。
	//   写成 const 引用会报「override 没有重写任何基类方法」—— 那个报错完全看不出是签名问题。
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};
