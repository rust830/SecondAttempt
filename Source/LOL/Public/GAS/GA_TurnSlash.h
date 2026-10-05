// TurnSlash：边砍边转向后方（下半身无位移）→ 接近 180 度的位置猛击 → 击退。
//
// ===========================================================================
// 【这一招的设计】
// 按「在不在攻击范围内」分两种演法：
//   ① 目标在【身后】（bTargetBehind）：起手就开始转 180°，转到位时那一刀落在目标身上。
//      这是「回身斩」—— 背后有人来攻，你转过去一刀把他推开。
//   ② 目标在【身前】：不转，直接按当前朝向挥（转了反而打空）。
// 所以「接近 180 度」这个设计由基类的转向插值自然达成：
// 起手锁定目标 → 目标在背后就转 180 → 转身时长内逐帧插值 → 到位即命中帧。
//
// 【★ 为什么转身只转 actor 的 yaw，而下半身「看起来」不动】
// 基类 ApplyStanceLock 里 DisableMovement + 关 bOrientRotationToMovement，
// 所以角色不位移、朝向也不被移动组件改写 —— 整个转身完全由 TickTurn 的
// 逐帧 SetActorRotation(Cur.Yaw) 驱动，只改 Yaw、保留 Pitch/Roll。
// 而「下半身无位移」= 没有 AddMovementInput / 没有 LaunchCharacter 给自己
// ⇒ 根骨盆不产生水平位移。**这是代码保证的：不写任何给自己位移的代码就是无位移。**
//
// 【击退 vs 击飞】
// 这一招用击退（UGE_Knockback）而不是击飞：
//   击退**自带** UGEComponent_Knockback 会 LaunchCharacter（方向走那个四级回退表），
//   所以这里【不需要】像 SpinSlash 那样自己 LaunchTarget ——
//   填好 Data.KnockbackImpulse / Data.KnockbackLaunch 就够了，位移是 GE 组件做的。
// 这是两个技能之间最重要的一处结构差异，也是为什么基类把 LaunchTarget 做成虚函数。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GAS/GA_FormMelee.h"
#include "GA_TurnSlash.generated.h"

/**
 * 转身回身斩：① 正常挥砍 → ② 边砍边转满 180°，转到位时猛击 + **击退**。
 *
 * 两段在【同一条 Montage】里连着播（构造函数里已把两段配好，`Montage` 在 BP 里只填一次）。
 *
 * 配 GE_Knockback 时**不需要**额外配位移 —— UGE_Knockback 自带 UGEComponent_Knockback，
 * 位移由那个组件做（填 Data.KnockbackImpulse 就行）。所以本类不覆写 LaunchTarget。
 */
UCLASS(Blueprintable)
class LOL_API UGA_TurnSlash : public UGA_FormMelee
{
	GENERATED_BODY()

public:
	UGA_TurnSlash();

protected:
	/**
	 * 目标在背后才转身（180°），在身前就是 0°。
	 *
	 * bTargetBehind 由基类在 ActivateAbility 里按「目标 - 角色」和角色朝向的点积算好
	 * （< 0 = 在身后 90° 之外）。所以这里只是一句判断 —— 转向的插值、骨骼朝向、
	 * 自动转向开关的处理全在基类的 TickTurn / ApplyTurnRotation / ApplyStanceLock 里。
	 *
	 * ⚠️ 转身在【ActivateAbility 起手就开始】，不是等第 2 段：转身是连续插值，
	 *   从中途再启动会顿一下，而两段之间通常只有 0.2~0.4 秒、插值根本走不完。
	 *   观感 =「第 1 段边砍边转，第 2 段已转到位时猛击」，正是需求要的。
	 */
	virtual float GetTurnDegrees() const override;
};
