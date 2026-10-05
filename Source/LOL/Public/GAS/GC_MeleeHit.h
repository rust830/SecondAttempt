// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GameplayTagContainer.h"
#include "GC_MeleeHit.generated.h"

class UParticleSystem;
class USoundBase;
class UCameraShakeBase;

/**
 * 普攻命中表现：命中点 burst + 一声打击音，外加【攻击者本人】的镜头振动。
 *
 * 和 UGC_ThrowDaggerHit 的分工是同一套：命中类型（Target.Hero / Target.Void）不可能塞进 cue 标签本身
 * （GameplayCueSet 只按弹射出的那个标签查表，不会给子标签各跑一份 cue），所以统一用
 * GameplayCue.MeleeHit 一个 cue，类型走 Parameters.AggregatedTargetTags —— 和匕首那条完全一样的口径，
 * 两边的 HitFXMap 也用同一套键。
 *
 * 走服务端 ExecuteGameplayCue → 多播到各客户端：ApplyServerHit 只在服务端跑，就地 Spawn 的话
 * 只有主机看得见这一下（GC_ThrowDaggerHit / GC_EmpoweredHit 修掉的是同一个坑）。
 *
 * 蓝图子类必须命名为 GC_MeleeHit。
 */
class UNiagaraSystem;

UCLASS()
class LOL_API UGC_MeleeHit : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_MeleeHit();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	/** 命中类型 → 粒子。键用 Target.Hero / Target.Void / Target.Terrain（近战扫掠用 ObjectType 查询，会命中地形）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit")
	TMap<FGameplayTag, TObjectPtr<UParticleSystem>> HitFXMap;

	/**
	 * HitFXMap 没匹配上时的兜底粒子。默认指向 Paragon 的近战命中特效（开箱即用，不用先进编辑器配）。
	 * 用软引用：默认值只是路径，想要别的、或者想彻底关掉粒子，在 GC_MeleeHit 里覆盖成别的 / 清空即可。
	 * （音效不在这上面：命中音走 UHeroAudioConfig 的 Audio.MeleeHit 事件。）
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit")
	TSoftObjectPtr<UParticleSystem> DefaultFX;


	/**
	 * 命中表现（Niagara 通道，与上面的 Cascade DefaultFX 是两条并行路）。
	 *
	 * 【为什么还要一条 Niagara 路】普攻原来只有 Cascade DefaultFX，而它一直是空的
	 * ——所以普攻命中只有音效、看不到打击特效。新的 Kallari 紫色特效全走 Niagara
	 * （可脚本构建、好调），于是这里补一个 Niagara 兜底；Cascade 那条保留不动，
	 * 老的 Paragon 命中特效还能照旧用。两边都配时 Niagara 优先（新素材先上）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit")
	TSoftObjectPtr<UNiagaraSystem> DefaultNiagaraFX;

	// ---------------------------------------------------------------------
	// ② 冲击波环（可选，和命中爆发分开配）
	// ---------------------------------------------------------------------

	/**
	 * 冲击波环 Niagara（可空）。和 GC_DodgeKickHit / GC_DodgePunchHit 同一个口径：
	 * 命中那一下另开一个环，铺在命中面上，是「一眼看得见」的主要来源。
	 *
	 * 为什么不在 DefaultNiagaraFX 里塞两个 emitter：那要往已经建好的 NS 里再挂一层
	 * emitter（Niagara emitter 不是独立资产，只能用「复制同系统内 emitter」的写法去凑，
	 * 还得改渲染器材质，风险比多 spawn 一个系统大）。这里照搬兄弟类的做法：
	 * 一个 cue 拖两个系统，各自调，互不影响。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit")
	TSoftObjectPtr<UNiagaraSystem> ShockwaveNiagara;

	/**
	 * 环的基础缩放（缩放的是 spawn 出来的 NiagaraComponent，不是 Niagara 用户参数）。
	 *
	 * 【为什么不再走 User.RingScale】本 cue 用的 NS_Melee_Shockwave 实测一个用户参数都没有
	 * （ListUserParameters 返回 0 个），所以 SetFloatParameter("User.RingScale", ...) 是静默 no-op
	 * —— 环从来没跟着打击分量变过大小。兄弟类 GC_DodgeKickHit 用的 NS_DodgeKick_LeoShockWave
	 * 确实带 User.RingScale，那条路在那边是通的，只有普攻这边一直没生效。
	 * 改成缩放组件后不依赖任何参数，以后换任何环系统都照吃。
	 *
	 * 1.0 = 系统原本大小。环的大小差异交给逐段的 HitCueWeight 去拉，别在这里预先缩小。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit", meta = (ClampMin = "0.01"))
	float ShockwaveScale = 1.f;

	/**
	 * 打中时给攻击者本人的镜头振动。留空 = 不振（默认就是空：项目里现在一个 CameraShake 资产都没有）。
	 *
	 * 只振【本地控制那台】：这个 cue 在每台机器上都会跑一次，
	 * 但只有「自己就是攻击者」的那一端才有资格振镜头，别端的屏幕不该因为别人打人而抖。
	 * 判定用的是 MyTarget（= 执行这个 cue 的那个 ASC 的 avatar，见 .cpp），
	 * 不用 Parameters.Instigator —— 后者要跨网络传 actor 引用，不如 MyTarget 稳。
	 *
	 * 这也是普攻唯一能加的「顿帧」：世界级 SetGlobalTimeDilation（见 GA_DeathHarvest::BeginHitStop）
	 * 在 PvP 里不能用 —— 普攻一秒两下，每次把全世界慢下来会让所有玩家一起卡；
	 * 改成每个角色自己的 CustomTimeDilation 又会和能力里那堆世界时间定时器错开
	 * （连段窗口/完美窗口全按世界时间算），动画慢放了判定不慢 → 手感更怪。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit")
	TSubclassOf<UCameraShakeBase> HitCameraShake;

	/**
	 * 振动的强度倍率。
	 *
	 * ⚠️ 【在 5.8 里是死的，改它没有任何效果】—— 和 UGC_EmpoweredHit::HitCameraShake 头上那段
	 * 是同一个坑：ClientStartCameraShake 的 Scale 只写进 FCameraShakeUpdateParams::ShakeScale，
	 * 而本项目的抖动（DefaultCameraShakeBase + WaveOscillatorCameraShakePattern）从头到尾不读它，
	 * 引擎也不会事后拿结果再乘一次（整个 Engine Runtime + Cameras 插件里 GetTotalShakeScale 零调用）。
	 * 留在这里只是为了不破坏已有的资产引用；要改强弱去改抖动资产上的
	 * LocationAmplitudeMultiplier / LocationFrequencyMultiplier / Duration。
	 *
	 * 想让「重的那一击抖得更狠」用下面的 HeavyHitCameraShake【换资产】，不要试图乘这个数。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit", meta = (ClampMin = "0"))
	float HitCameraShakeScale = 1.f;

	/**
	 * 打击分量达到 HeavyShakeWeightThreshold 时【换用】的抖动资产（重击用更猛的那一个）。
	 *
	 * 【为什么是换资产而不是乘倍率】见上面 HitCameraShakeScale 那段：倍率在 5.8 里是死的，
	 * 唯一能真的改抖动强弱的办法就是换一个更猛的抖动资产。
	 * 由 UGA_ThreeHitPassive 通过 CueParams.NormalizedMagnitude 传进来的 Stage.HitCueWeight 驱动 ——
	 * 空手四连拳的第四段（蓄力拳）配 2.5，前三段没配（= 1.0），于是只有收尾那一拳切到这里。
	 * 留空 = 重击仍用 HitCameraShake（等于没有重击区分）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit")
	TSubclassOf<UCameraShakeBase> HeavyHitCameraShake;

	/** 打击分量到多少算「重击」（切到 HeavyHitCameraShake）。0 = 只要传了分量就用重击那一个。 */
	UPROPERTY(EditDefaultsOnly, Category = "Hit", meta = (ClampMin = "0"))
	float HeavyShakeWeightThreshold = 1.5f;
};
