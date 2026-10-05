// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GC_DodgePunchHit.generated.h"

class UNiagaraSystem;
class USoundBase;
class UCameraShakeBase;

/**
 * 空手形态 dodge 派生【直拳】命中那一下的表现（Static cue）。
 *
 * 和 GC_DodgeKickHit 是兄弟而不是同一份的原因：拳和脚的特效语言不同 ——
 * 脚是"从天而降的彗星砸地"（冲击波环 + 尘土），拳是"短促锐利的一记"
 * （命中点的小爆 + 一道快速掠过的气劲）。分开两个 cue，各自在 BP 里配素材，
 * 以后调拳的特效不会动到踢的（反过来也一样）。
 *
 * 结构、参数、判定块和 GC_DodgeKickHit 逐字一致 —— 差异只有 GameplayCueTag 和注释。
 * 改共用逻辑时两边同步（或者将来抽个基类）。
 *
 * 蓝图子类必须命名为 GC_DodgePunchHit。
 */
UCLASS()
class LOL_API UGC_DodgePunchHit : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_DodgePunchHit();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	// ---------------------------------------------------------------------
	// ① 命中爆发
	// ---------------------------------------------------------------------

	/** 命中 Niagara（可空）。软引用：默认值只是路径，编辑器里随时换。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgePunchHit|Impact")
	TSoftObjectPtr<UNiagaraSystem> HitNiagara;

	/** 命中音效（可空）。 */

	// ---------------------------------------------------------------------
	// ② 冲击波环（可选，和爆发分开配）
	// ---------------------------------------------------------------------

	/**
	 * 冲击波环 Niagara（可空）。拳的版本建议比飞踢小一圈 —— 直拳是"打"不是"砸"，
	 * 环太大就抢了"轻灵刺客"的调性。不配的话只靠 HitNiagara 也成立。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgePunchHit|Shockwave")
	TSoftObjectPtr<UNiagaraSystem> ShockwaveNiagara;

	// ---------------------------------------------------------------------
	// ③ 镜头震动（只给打人的那一端）
	// ---------------------------------------------------------------------

	/**
	 * 打中时给【攻击者那一端】的镜头震动（可空 = 不振，默认就是空）。
	 *
	 * ⚠️ 强度【只能在抖动资产上调】：ClientStartCameraShake 的 Scale 在 5.8 里是死的
	 * （详见 GC_EmpoweredHit.h / GC_DodgeKickHit.h 里同一段说明）。
	 * 直拳建议比飞踢【轻、短】一点：一记快拳 0.1~0.15s 的小抖就够，砸地那种 0.2~0.3s 是脚的待遇。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgePunchHit|Camera")
	TSubclassOf<UCameraShakeBase> HitCameraShake;

	// ---------------------------------------------------------------------
	// Niagara 用户参数名（NS 侧改了名就在这里同步改）
	// ---------------------------------------------------------------------

	/** 命中位置参数名（部分 NS 需要手动塞位置而不是靠组件 transform）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgePunchHit|Params")
	FName ImpactPositionParameter = TEXT("User.ImpactPos");

	/** 命中面法线参数名。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgePunchHit|Params")
	FName ImpactNormalParameter = TEXT("User.ImpactNormal");

	/**
	 * 冲击波环的基础缩放（缩放的是 spawn 出来的 NiagaraComponent，不是 Niagara 用户参数）。
	 *
	 * 【为什么不再走 User.RingScale】和 GC_DodgeKickHit 同一个坑：本 cue 用的
	 * NS_Punch_Shockwave 实测没有任何用户参数，所以
	 * `SetFloatParameter(ShockwaveScaleParameter, ShockwaveScale * Lerp(...))` 一直是
	 * 【静默 no-op】——环从来没跟着强度变过大小。改成缩放组件后不依赖任何参数。
	 *
	 * ⚠️ 这个值以前只喂给那个死参数，所以【从没真正生效过】，现在才第一次起作用：
	 * 实际倍率 = ShockwaveScale × Lerp(0.7, 1.3, 强度)，也就是 0.7 ~ 1.3 倍
	 * （中间强度 = 1.0 = 和以前看到的一样大）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgePunchHit|Shockwave", meta = (ClampMin = "0.01"))
	float ShockwaveScale = 1.0f;

	// ---------------------------------------------------------------------
	// ④ 强度随【攻击者速度】缩放（和飞踢同源：RawMagnitude 带进来的落地/前冲速度）
	// ---------------------------------------------------------------------

	/** 冲击强度参数名（0~1）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgePunchHit|Params")
	FName ImpactStrengthParameter = TEXT("User.ImpactStrength");

	/** 强度归一化的参考速度（cm/s）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgePunchHit|Params", meta = (ClampMin = "1"))
	float ImpactSpeedRef = 1200.f;
};
