// SpinSlash：向前方水平轻挥砍 → 猛烈向上斩击 → 击飞。
//
// ===========================================================================
// 【这一招的设计】
// 两段：① 轻挥砍（低伤害、无控制，为了衔接和手感）② 上挑斩（高伤害 + 击飞）。
// 击飞比击退强一档 —— 所以伤害和控制都放在第二段，第一段只负责「把这一招起手做出来」
//
// 【★ 下半身不位移 —— 这一点是需求明确要求的】
// 技能期间 DisableMovement（基类 ApplyStanceLock 里做），所以人站在原地只有上身动作。
// ⚠️ 别用 LaunchCharacter 给角色做前冲：那条路会让「下半身不位移」失效，
//   而且位移量和击退控制的观感会打架（自己在动 vs 目标在动，分不清谁在打谁）。
//   招式的前冲感靠【蒙太奇里的 root motion】做，或者干脆不要 ——
//   攻击技站着挥本来就是 LoL 的常态（只有位移技能才带前冲）。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GAS/GA_FormMelee.h"
#include "GA_SpinSlash.generated.h"

/**
 * 上挑斩：① 水平轻挥砍（普通伤害）→ ② 猛烈上挑（更高伤害 + **击飞** + 镜头效果）。
 *
 * 两段在【同一条 Montage】里连着播（BP 里 `Montage` 只填一次），
 * 靠两段各自的 `ImpactTag` 区分结算时机 —— 构造函数里已经把两段配好了。
 *
 * 配 GE_KnockUp 时注意：UGE_KnockUp **没有**位移组件（只给 State.KnockUp + 蒙太奇），
 * 升空那一下的 Z 冲量由 UGA_SpinSlash::LaunchTarget 自己 LaunchCharacter 补 ——
 * 不补的话目标会「站在原地被定住」而不是「被打飞」。
 */
UCLASS(Blueprintable)
class LOL_API UGA_SpinSlash : public UGA_FormMelee
{
	GENERATED_BODY()

public:
	UGA_SpinSlash();

protected:
	/**
	 * 只在「本段的 ControlGE 是 UGE_KnockUp」时才 LaunchCharacter。
	 *
	 * ★ 多段改造后这个函数会被每一段调一次，而只有上挑那段挂了击飞 ——
	 *   平砍段的 ControlGE 是空的，所以直接返回（那一下只掉血、不把人挑飞）。
	 *   判据用 ControlGE 而不是段下标：段下标是接线细节（中间插一段就错了），
	 *   而「这段挂的是击飞 GE」才是这件事的真实语义。
	 */
	virtual void LaunchTarget(AActor* Target, const FHitResult& Hit, const FFormMeleeStage& Stage) override;
};
