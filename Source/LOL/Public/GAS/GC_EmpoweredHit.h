// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GC_EmpoweredHit.generated.h"

class UNiagaraSystem;
class USoundBase;
class UCameraShakeBase;

/**
 * 强化普攻【命中】时的额外效果：在命中点炸一下 + 一声 + 攻击者本人的镜头振动（可选）。
 *
 * 为什么单独开一个 cue 而不是让 UGA_ThreeHitPassive 自己 Spawn：
 * 命中结算（ApplyServerHit）是纯服务端的（客户端没有权威 HitResult），在那里生成粒子只有主机看得到。
 * 走服务端 ExecuteGameplayCue 才会把参数复制到各客户端、各自本地播一遍 —— 和 GC_ThrowDaggerHit 同一条路。
 *
 * 时机和「踩中完美窗口」那一刻是两回事：踩中窗口只给本人一声 QTE 反馈（见
 * UThreeHitPassiveData::PerfectWindowSuccessSound），强化击真打到人才是这里的额外效果。
 * 两处都用同一个 cue：破隐那一击也是强化的，命中了同样该炸。
 *
 * 蓝图子类必须命名为 GC_EmpoweredHit（cue 的 BP 命名规则见 LOLGameplayTags.h）。
 */
UCLASS()
class LOL_API UGC_EmpoweredHit : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_EmpoweredHit();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	/** 命中点炸开的 NS，默认指向 /Game/LOL/Niagara/NS_PerfectSuccess。留空 = 只有音效。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredHit")
	TSoftObjectPtr<UNiagaraSystem> HitSystem;

	/** 命中点的一次性音效（可空）。 */

	/** NS 里那个位置参数（含 User. 前缀）。NS_PerfectSuccess 用的是 User.ImpactPos。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredHit")
	FName ImpactParameter = TEXT("User.ImpactPos");

	/**
	 * 强化击命中时给【攻击者本人】的镜头振动 —— 单独一条，和普攻那一击不是同一个资产。
	 * 留空 = 不振（默认就是空）。
	 *
	 * 强度【只能在抖动资产上调】，没有倍率参数：ClientStartCameraShake 的那个 Scale
	 * 在 5.8 里是死的 —— 它只写进 FCameraShakeUpdateParams::ShakeScale，而本项目的抖动
	 * （DefaultCameraShakeBase + WaveOscillatorCameraShakePattern）从头到尾不读它，
	 * 引擎也不会事后拿结果再乘一次（整个 Engine Runtime + Cameras 插件里 GetTotalShakeScale
	 * 零调用）。要改强弱就去改资源上的 LocationAmplitudeMultiplier / LocationFrequencyMultiplier /
	 * Duration。GC_MeleeHit::HitCameraShakeScale 是同一个坑，现在恒为 1 所以看不出来。
	 *
	 * ⚠️ 和 UGC_MeleeHit 那一份是【相加】的，不是二选一：同一击上两个 cue 都会跑
	 * （ApplyServerHit 先 ExecuteMeleeHitCue 再 ExecuteEmpoweredHitCue），抖动资产的
	 * bSingleInstance 又是 false → 两个实例同时生效。所以强化击实际拿到的抖动
	 * = GC_MeleeHit 的（基础，BP_CameraShake_Hit_Player）+ 这里的。要让强化击【只抖一种、
	 * 不叠加】，做法是让这里的资产扛下全部强度（想要多猛就调多猛）。
	 *
	 * 只振【本地控制那台】：cue 在每台机器上都会跑一遍，只有「自己就是攻击者」的那一端该抖，
	 * 别人屏幕上不该因为别人打人而晃。判定和 UGC_MeleeHit 完全一致：用 MyTarget
	 * （= 执行这个 cue 的那个 ASC 的 avatar，即攻击者），不用 Parameters.Instigator。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredHit")
	TSubclassOf<UCameraShakeBase> HitCameraShake;

private:
	/**
	 * 「NS 没配」只打一次警告。Static cue 跑的是 CDO，所以这一份标记是全场的。
	 * 不用 UPROPERTY：UHT 不支持 mutable 属性，而 OnExecute 是 const。
	 */
	mutable bool bLoggedMissingSystem = false;
};
