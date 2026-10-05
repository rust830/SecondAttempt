// 空手四连拳的实现。架构理由见头文件。

#include "GAS/GA_BoxingCombo.h"
#include "GAS/LOLGameplayTags.h"

UGA_BoxingCombo::UGA_BoxingCombo()
{
	// 父类构造已经把 InstancingPolicy / NetExecutionPolicy / ActivationPolicy(OnEvent) /
	// EmpoweredAttackGE / KnockbackGE 都设好了，这里不动。

	// ★ 换输入标签：连段推进的按键事件从 BasicAttack 换成 ComboAttack。
	// 父类 ActivateAbility 会拿 AttackInputTag 建 WaitGameplayEvent 任务。
	AttackInputTag = LOLGameplayTags::Event_Input_ComboAttack;

	// 激活触发器也换（同一条链上的另一处，漏了能力根本不会被激活）。
	// 写法和 GA_DeathHarvest 同一套：FAbilityTriggerData + GameplayEvent 源。
	//
	// ⚠️ BP 的 CDO 属性会【整体替换】C++ 构造函数加进去的数组。
	//   所以如果 BP 里自己配了 AbilityTriggers，这里的会被顶掉 —— 那是允许的
	//   （BP 说了算），但必须保证填的还是 Event.Input.ComboAttack。
	{
		FAbilityTriggerData Trigger;
		Trigger.TriggerTag = LOLGameplayTags::Event_Input_ComboAttack;
		Trigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
		AbilityTriggers.Add(Trigger);
	}
}

// ⚠️ 参数按值传，和基类 UGA_ThreeHitPassive 逐字一致。
void UGA_BoxingCombo::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// 兜底重设一次 AttackInputTag：它是 EditDefaultsOnly ⇒ BP 里填的值会覆盖构造函数。
	// 而四连拳的 BP 极可能是从三连普攻的 BP 抄的（AttackInputTag 已经是 BasicAttack），
	// 忘了清空就会退回持刀那条标签 ⇒ 空手连招和持刀普攻互相唤醒 ⇒ 一次按键打两段。
	// 在 Super 之前设才有效 —— 父类 ActivateAbility 一开头就用它建 InputTask 了。
	AttackInputTag = LOLGameplayTags::Event_Input_ComboAttack;

	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}
