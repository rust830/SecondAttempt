// 形态切换：挂上 = 空手形态，收回 = 持刀形态。
//
// ===========================================================================
// 【为什么用 GE 挂标签，而不是 AddLooseGameplayTag】
// GAS_FormSwitch_Setup.md §7 记了那条坑：**loose tag 在 UE 5.8 不复制**
// （add_loose_gameplay_tags 的 bReplicate 默认 false），只对 PIE 本地测试有效。
// 正式切换必须让 tag 随 GameplayEffect 复制到每个客户端，各端 AnimInstance
// 各自读自己的 ASC（RefreshAnimSelection 已经按这个写好了）→ 客户端自动生效。
//
// 【为什么是 Infinite】
// 形态不是一个「有时限的状态」，它一直成立到下次切换。用 HasDuration 的话
// 得在技能里另外记一个句柄来续期/撤销，而 Infinite + 摘除就是「挂上 / 摘掉」
// 这一对操作，语义和实现都更直白（摘除见 UGA_FormSwitch::ApplyForm）。
//
// 【不挂任何修正符】
// 形态只影响【视觉】（ABP 的 PoseBlend 姿势差值）。数值上两种形态完全一样 ——
// 攻击、技能、移速都不变。真要加形态差异（比如空手攻速更快）应该走
// UArenaItem 那条数值通道，不要塞进这个 GE。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GE_FormUnarmed.generated.h"

/**
 * 空手形态。Infinite，授予 State.Form.Unarmed。
 *
 * 挂上/摘掉由 UGA_FormSwitch 负责，而且【必须在那两个 montage 播完之后】才做
 * —— 顺序反了会出现「刀已经收进鞘了、人还摆着持刀的姿势」这种半截状态。
 */
UCLASS()
class LOL_API UGE_FormUnarmed : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGE_FormUnarmed();

private:
	/** TargetTags 组件：State.Form.Unarmed 就是从它这里授出去的（持变量防止被 GC）。 */
	UPROPERTY()
	TObjectPtr<UTargetTagsGameplayEffectComponent> TargetTags;
};
