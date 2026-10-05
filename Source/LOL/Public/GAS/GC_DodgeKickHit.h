// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GC_DodgeKickHit.generated.h"

class UNiagaraSystem;
class USoundBase;
class UCameraShakeBase;

/**
 * 飞踢【命中那一下】的表现：GA_Dodge 在飞行结束结算时 ExecuteGameplayCue，
 * 多播到各客户端各播一次。
 *
 * 用 Static 变体：放一次 burst 就完事，没有生命周期。
 * 位置 / 朝向由 GA_Dodge 通过 Parameters 传进来（命中点 + 命中面法线）。
 *
 * 【打击感三件套，各自可空】
 *   ① 命中爆发（HitNiagara）—— 在命中点炸开，朝向跟着命中面法线。
 *   ② 冲击波环（ShockwaveNiagara）—— 可选，单独一条，在命中点铺一个朝外扩散的环。
 *      和爆发分开配是因为两者常常要不同的大小/朝向策略。
 *   ③ 镜头震动（HitCameraShake）—— 只给【踢人的那一端】，别人屏幕上不抖。
 *
 * ④ 音效（ImpactSound）—— 这一类身上【没有】音效属性，一律从 UHeroAudioConfig 的事件表里取
 *    （Audio.DodgeKickHit）。单个 cue 想临时换音，去表里换这一条，别往这个类上加属性。
 *
 * 【为什么不做真正的「顿帧」】项目里唯一能用的冻时间手段是 AActor::CustomTimeDilation
 * （世界级 SetGlobalTimeDilation 会把全场玩家一起卡住，见 GC_MeleeHit 的说明）。
 * 但飞踢的判定——TickKickFlight 的「够近了没」「超时了没」——全部按世界时间算，
 * 角色级慢放只会让【画面】慢下来、判定照旧，反而造成判定与表现不同步。
 * 所以这里改成「命中瞬间的视觉强化」：爆发 + 环 + 抖动同时砸下去，靠这三样制造
 * 「打中了」的实感。这也是现代多人动作游戏的通行做法（真冻帧在联机里代价太高）。
 *
 * 蓝图子类必须命名为 GC_DodgeKickHit。
 */
UCLASS()
class LOL_API UGC_DodgeKickHit : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_DodgeKickHit();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	// ---------------------------------------------------------------------
	// ① 命中爆发
	// ---------------------------------------------------------------------

	/** 命中 Niagara（可空）。软引用：默认值只是路径，编辑器里随时换。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKickHit|Impact")
	TSoftObjectPtr<UNiagaraSystem> HitNiagara;

	/** 命中音效（可空）。 */

	// ---------------------------------------------------------------------
	// ② 冲击波环（可选，和爆发分开配）
	// ---------------------------------------------------------------------

	/**
	 * 冲击波环 Niagara（可空）。在命中点铺一个朝外扩散的环 —— 这是「力量感」最直接的一条。
	 * 不配的话只靠 HitNiagara 也成立。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKickHit|Shockwave")
	TSoftObjectPtr<UNiagaraSystem> ShockwaveNiagara;

	// ---------------------------------------------------------------------
	// ③ 镜头震动（只给踢人的那一端）
	// ---------------------------------------------------------------------

	/**
	 * 踢中时给【踢人那一端】的镜头震动（可空 = 不振，默认就是空）。
	 *
	 * ⚠️ 强度【只能在抖动资产上调】，没有倍率参数：ClientStartCameraShake 的 Scale
	 * 在 5.8 里是死的 —— 它只写进 FCameraShakeUpdateParams::ShakeScale，而本项目的抖动
	 * （DefaultCameraShakeBase + WaveOscillatorCameraShakePattern）从头到尾不读它
	 * （整个 Engine Runtime + Cameras 插件里 GetTotalShakeScale 零调用）。
	 * 要改强弱就去改资源上的 LocationAmplitudeMultiplier / LocationFrequencyMultiplier / Duration。
	 * 详见 GC_EmpoweredHit.h 里同一段说明。
	 *
	 * 飞踢的抖动建议比普攻【更重、更长一点】：普攻是 0.1~0.15s 的小抖，
	 * 飞踢是一记从天而降的重脚，0.2~0.3s + 更大振幅才配得上这个动作。
	 *
	 * 判定用 MyTarget（= 执行这个 cue 的那个 ASC 的 avatar，即踢人者），不用
	 * Parameters.Instigator —— actor 引用要跨网络传，avatar 是接收端自己解析出来的，稳。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKickHit|Camera")
	TSubclassOf<UCameraShakeBase> HitCameraShake;

	// ---------------------------------------------------------------------
	// Niagara 用户参数名（NS 侧改了名就在这里同步改）
	// ---------------------------------------------------------------------

	/** 命中位置参数名（部分 NS 需要手动塞位置而不是靠组件 transform）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKickHit|Params")
	FName ImpactPositionParameter = TEXT("User.ImpactPos");

	/** 命中面法线参数名。决定粒子朝哪边喷（贴着身体 / 地面朝外，而不是永远世界朝前）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKickHit|Params")
	FName ImpactNormalParameter = TEXT("User.ImpactNormal");

	/**
	 * 冲击波环的基础缩放（缩放的是 spawn 出来的 NiagaraComponent，不是 Niagara 用户参数）。
	 *
	 * 【为什么不再走 User.RingScale】这里本来写的是
	 * `SetFloatParameter(ShockwaveScaleParameter, ShockwaveScale * Lerp(...))`，
	 * 但本 cue 实际用的 NS_DodgeKick_V2_ShockWave 实测一个用户参数都没有
	 * （ListUserParameters 返回 0），所以那句一直是【静默 no-op】——环从来没跟着强度变过大小。
	 * 全项目唯一带 User.RingScale 的是 NS_DodgeKick_LeoShockWave，而没有任何 cue 引用它，
	 * 说明这个约定当初是照着它写的、却没落到真正在用的资产上。
	 * 改成缩放组件后不依赖任何参数，以后换任何环系统都照吃。
	 *
	 * ⚠️ 下面这个值以前只喂给那个死参数，所以【从没真正生效过】，现在才第一次起作用：
	 * 实际倍率 = ShockwaveScale × Lerp(0.7, 1.3, 强度)，也就是 1.05 ~ 1.95 倍。
	 * 觉得飞踢的环偏大或偏小，直接调这个数（1.0 = 系统原本大小），不用改代码。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKickHit|Shockwave", meta = (ClampMin = "0.01"))
	float ShockwaveScale = 1.5f;

	// ---------------------------------------------------------------------
	// ④ 强度随【落地速度】缩放
	// ---------------------------------------------------------------------

	/**
	 * 冲击强度参数名（0~1）。喂给命中爆发与冲击波环的 NS，用来驱动粒子数量 / 寿命 / 大小。
	 * 让"从 8 米砸下来"和"贴地铲一脚"在画面上有区别（而不是完全一样）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKickHit|Params")
	FName ImpactStrengthParameter = TEXT("User.ImpactStrength");

	/** 强度归一化的参考速度（cm/s）：实际落地速度除以它再夹到 [0,1]。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKickHit|Params", meta = (ClampMin = "1"))
	float ImpactSpeedRef = 1200.f;
};
