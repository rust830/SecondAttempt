// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_DodgeKick.generated.h"

class UNiagaraSystem;
class UNiagaraComponent;

/**
 * 飞踢【飞行段】的表现：GA_Dodge 在飞踢起手时 AddGameplayCue 挂上、
 * 飞行结束（踢中 / 扑空）时 RemoveGameplayCue 收掉（多播，各端都看得到）。
 *
 * 用 Actor 变体：要跨「起踢 → 命中」这段时间持续存在，且要在 OnRemove 里收掉自己生成的组件。
 *
 * ★ WhileActive 转发到 OnActive：运行时 AddGameplayCue 在服务器那一端收到的是 WhileActive、
 *   客户端才是 OnActive，基类 WhileActive_Implementation 是空实现、不转 OnActive ——
 *   只写 OnActive 的话主机自己屏幕上看不到拖尾（同 AGC_DeathHarvestSpin 那个坑）。
 *
 * 【表现分四层，各自可空】
 *   ⓪ 起手充能（FootChargeNiagara）：挂在脚上，Add 之后 ChargeLeadTime 秒内只开这一层。
 *      雷欧飞踢的第一段 —— 脚掌聚金红色能量、由暗转白热。它和 ① ② 是【同一时间轴的两段】，
 *      到点后充能层自动关掉、拖尾层打开（见 ChargeLeadTime）。
 *   ① 脚部拖尾（TrailNiagara）：挂在 TrailSocket 上，跟着踢的那只脚走。
 *      这是飞踢最重要的速度感来源 —— 玩家看的是「脚在飞」，不是身体。
 *   ② 冲刺气流（AirFlowNiagara）：挂在身体（mesh 根）上，朝飞行方向喷。
 *      可选，用来补「整个人在高速穿过去」的感觉；只配拖尾也完全成立。
 *   ③ 小腿发光（LegNiagara）：挂在小腿骨骼（calf_r / calf_l = 膝盖）上。
 *      NS 里的粒子沿骨骼局部 X 轴（膝→踝方向）铺一根发光柱，贴着小腿走，
 *      膝盖怎么弯它就跟到哪。拖尾跟脚、发光贴腿，两层各管一段。
 *
 * 【素材可以一个都不配】全部空 = 这个 cue 什么都不做（不报错、不崩）。
 *   材质与参数建议见 GAS_DodgeKick_FX_Setup.md。
 *
 * 蓝图子类必须命名为 GC_DodgeKick。
 */
UCLASS()
class LOL_API AGC_DodgeKick : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()
public:
	AGC_DodgeKick();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	/** 服务器那一端走的是这条（运行时 Add 的 cue），转给 OnActive。理由见类注释。 */
	virtual bool WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;

	// ---------------------------------------------------------------------
	// ⓪ 起手充能
	// ---------------------------------------------------------------------

	/**
	 * 起手充能 Niagara（可空）。挂在【脚】上，是「能量在聚集」这一段的表现。
	 *
	 * NS 自己是个自循环的短循环体（聚能 → 白热），不需要 C++ 帮它计时；
	 * C++ 只负责在 ChargeLeadTime 到点时把它关掉、把拖尾打开（见 ScheduleChargeSwap）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Charge")
	TSoftObjectPtr<UNiagaraSystem> FootChargeNiagara;

	/**
	 * 起手充能到「全力出脚」的间隔（秒）。0 = 不起手（一挂上就全功率，退化成旧行为）。
	 *
	 * 【必须和 GA_Dodge::KickChargeLeadTime 一致】—— GA 那一份是"逻辑侧的时间轴"，
	 * 这一份是"表现侧的换挡时刻"。两边不一致就会出现「人已经飞出去了脚还在聚能」这种穿帮。
	 * 之所以不互相引用（GA 直接 Set 给 cue）：cue 是纯数据 asset，C++ 改它的默认值要连带
	 * 改 BP 覆盖，反而更难对齐；两个数值都留在各自的可编辑属性里，改的时候一起改。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Charge", meta = (ClampMin = "0", Units = "s"))
	float ChargeLeadTime = 0.16f;

	/** 起手充能挂的脚插槽。默认和拖尾用同一只脚（向前 = foot_r）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Charge")
	FName ChargeSocket = TEXT("foot_r");

	/** 向后踢时用的充能插槽（仅 bSwitchFootByDirection=true 时用）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Charge")
	FName ChargeSocketBack = TEXT("foot_l");

	/** 起手充能的缩放，经 ChargeScaleParameter 喂给 NS。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Charge", meta = (ClampMin = "0.01"))
	float ChargeScale = 1.f;

	// ---------------------------------------------------------------------
	// ① 脚部拖尾
	// ---------------------------------------------------------------------

	/** 脚上拖尾 Niagara（可空）。跟着踢的那只脚走，是速度感的主力。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Trail")
	TSoftObjectPtr<UNiagaraSystem> TrailNiagara;

	/**
	 * 拖尾挂的脚插槽。Kallari 的脚是 foot_l / foot_r。
	 * 空 = 退回 mesh 根（还能用，但拖尾不会跟着脚摆，速度感大打折扣）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Trail")
	FName TrailSocket = TEXT("foot_r");

	/**
	 * 拖尾的缩放。做「整条腿都在发光」那种大拖尾时调大，做细线时调小。
	 * 传给 Niagara 的 User.TrailScale（NS 里没这条参数的话这里改了没效果，不报错）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Trail", meta = (ClampMin = "0.01"))
	float TrailScale = 1.f;

	/**
	 * 拖尾跟着【踢出去的那只脚】还是固定脚。
	 *   true  —— 按踢的方向挑脚：向前踢用 TrailSocket，向后踢用 TrailSocketBack。
	 *   false —— 永远用 TrailSocket（省事，但向后踢时拖尾在反方向那只脚上）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Trail")
	bool bSwitchFootByDirection = true;

	/** 向后踢时用的插槽（仅 bSwitchFootByDirection=true 时用）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Trail")
	FName TrailSocketBack = TEXT("foot_l");

	// ---------------------------------------------------------------------
	// ② 冲刺气流（可选）
	// ---------------------------------------------------------------------

	/**
	 * 冲刺气流 Niagara（可空）。挂在 mesh 上、朝飞行方向喷，用来补「整个人高速穿过去」的感觉。
	 *
	 * 它的朝向由【飞行方向】决定，所以 GA_Dodge 会通过
	 * Parameters.Normal（= 飞行方向）传进来；NS 里用 User.FlyDir 接。
	 * 不配的话就只靠脚部拖尾，观感也成立。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|AirFlow")
	TSoftObjectPtr<UNiagaraSystem> AirFlowNiagara;

	/** 气流挂的插槽。空 = mesh 根（推荐，气流本来就该从身体喷）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|AirFlow")
	FName AirFlowSocket = NAME_None;

	// ---------------------------------------------------------------------
	// ③ 小腿发光（可选）
	// ---------------------------------------------------------------------

	/**
	 * 小腿发光 Niagara（可空）。挂在小腿骨骼上，NS 内的粒子沿骨骼局部 +X
	 * （膝→踝）铺一根发光柱 —— 挂点决定位置，柱子方向由 NS 里的粒子旋转定。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Leg")
	TSoftObjectPtr<UNiagaraSystem> LegNiagara;

	/**
	 * 小腿发光挂的骨骼/插槽。Kallari 的小腿是 calf_r / calf_l（位于膝盖）。
	 * 空 = 挂 mesh 根（柱子方向就没了意义，等于一根竖在世界原点方向的柱子，不推荐）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Leg")
	FName LegSocket = TEXT("calf_r");

	/** 向后踢时用的插槽（仅 bSwitchFootByDirection=true 时用）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Leg")
	FName LegSocketBack = TEXT("calf_l");

	/**
	 * 小腿发光的缩放，经 LegScaleParameter 喂给 NS。
	 * NS 里没接这条参数的话这里改了没效果（不报错）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Leg", meta = (ClampMin = "0.01"))
	float LegScale = 1.f;

	// ---------------------------------------------------------------------
	// Niagara 用户参数名（NS 侧改了名就在这里同步改）
	// ---------------------------------------------------------------------

	/** 拖尾缩放参数名。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Params")
	FName TrailScaleParameter = TEXT("User.TrailScale");

	/** 起手充能缩放参数名。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Params")
	FName ChargeScaleParameter = TEXT("User.ChargeScale");

	/** 小腿发光缩放参数名。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Params")
	FName LegScaleParameter = TEXT("User.LegScale");

	/** 飞行方向参数名（空气流用）。 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Params")
	FName FlyDirectionParameter = TEXT("User.FlyDir");

	/**
	 * 飞行速度参数名（cm/s，单位与 GA 侧一致）。
	 *
	 * GA_Dodge 通过 FGameplayCueParameters::RawMagnitude 把【实际飞行速度】传进来
	 * （= 水平冲量与下砸初速的合速度大小），这里把它喂给 NS。
	 * 用途：拖尾长度 / 粒子密度 / 寿命按速度缩放 —— 高空下砸虽然水平分量一样，
	 * 但合速度更大，残影更长，观感上「更猛」是对的。
	 *
	 * NS 里没这条参数就什么都不发生（不报错）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Params")
	FName FlySpeedParameter = TEXT("User.FlySpeed");

	/**
	 * 归一化速度的参考值（cm/s）：把 FlySpeed 除以它再夹到 [0,1] 后传给 NS。
	 * 让 NS 侧拿到的是一个可以直接当 0~1 混合权重用的数，而不用在 Niagara 里再做除法。
	 * 默认 1500 ≈ 常用的水平冲量量级。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "DodgeKick|Params", meta = (ClampMin = "1"))
	float FlySpeedRef = 1500.f;

private:
	/**
	 * 起手充能 → 全功率的换挡：关掉充能层、打开拖尾/气流层。
	 *
	 * 【为什么用定时器而不是让 Niagara 自己控制开关】拖尾组件在 OnActive 里就 spawn 好了、
	 * 只是 SetActive(false)（不激活但已附着）—— 到点直接 SetActive(true)，位置/附着一气呵成，
	 * 不会出现"到点才 spawn、有一帧没挂上"的闪断。充电层同理。
	 */
	void SwapChargeToTrail();

	/** 生成的起手充能 Niagara 组件（OnRemove 收掉它）。 */
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> ChargeComponent;

	/** 换挡定时器（OnRemove 要清掉，否则 cue 提前被摘时会打到已销毁的组件上）。 */
	FTimerHandle ChargeSwapTimer;

	/** 生成的拖尾 Niagara 组件（OnRemove 收掉它）。 */
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> TrailComponent;

	/** 生成的气流 Niagara 组件（OnRemove 收掉它）。 */
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> AirFlowComponent;

	/** 生成的小腿发光 Niagara 组件（OnRemove 收掉它）。 */
	UPROPERTY(Transient)
	TObjectPtr<UNiagaraComponent> LegComponent;
};
