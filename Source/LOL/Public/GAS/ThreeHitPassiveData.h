// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ThreeHitPassiveData.generated.h"
class UAnimMontage;
class UGameplayEffect;
class UNiagaraSystem;
class USoundBase;

USTRUCT(BlueprintType)
struct FThreeHitAttackStage
{
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TObjectPtr<UAnimMontage> Montage;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0")) float DamageMultiplier = 1.f;
	/** Times are in authored montage seconds, before attack-speed play-rate scaling. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float HitTime = .15f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float ChainWindowOpenTime = .25f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float ChainWindowCloseTime = .50f;
	/**
	 * On stage 2, the first input inside the perfect window (PerfectWindow* below, inside the chain
	 * window) arms the next hit as an empowered attack (bonus damage multiplier + knockback, values
	 * from UThreeHitPassiveData::Empower*).  An input elsewhere in the chain window still advances
	 * the combo — the next hit just comes out normal.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) bool bPerfectWindowEnablesNextHitKnockback = false;

	/**
	 * 完美窗口（蒙太奇秒，和上面几个时间同一口径，同样按攻速缩放）。
	 * 只有落在 [开, 关] 内的那一次连段输入才会武装下一段强化；连段窗口内的其余输入照样推进连段，
	 * 只是下一段是普通攻击。两者都留 0（或关 <= 开）表示「没配」→ 回退成整段连段窗口，
	 * 也就是老行为（连段窗口内按键即强化），日志里会打出来，不会静默。
	 * 实际有效区间取它和连段窗口的交集（连段窗口开启前/关闭后的输入本来就不收）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float PerfectWindowOpenTime = 0.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, meta=(ClampMin="0", Units="s")) float PerfectWindowCloseTime = 0.f;
};

/** Per-hero tuning asset.  The delivery GE reads Data.Damage via SetByCaller. */
UCLASS(BlueprintType)
class LOL_API UThreeHitPassiveData final : public UDataAsset
{
	GENERATED_BODY()
public:
	UThreeHitPassiveData();
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack") TArray<FThreeHitAttackStage> Stages;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0.01")) float ReferenceAttackSpeed = .658f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0", Units="cm")) float TraceDistance = 180.f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack", meta=(ClampMin="0", Units="cm")) float TraceRadius = 55.f;
	/** Instant GE with a SetByCaller magnitude named Data.Damage. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage") TSubclassOf<UGameplayEffect> DamageEffect;

	// ---------------------------------------------------------------------
	// 强化普攻（破隐 / 三连击完美窗口共用同一套数值）
	// 谁挂 State.EmpoweredAttack 谁决定窗口时长，但强度只在这里配一份。
	// ---------------------------------------------------------------------

	/**
	 * 强化那一击替换 Stage.Montage 播放的蒙太奇。
	 * slot 必须和普攻同槽（UpperBody），否则 AnimBP 里没有对应 slot 节点，播了也看不见姿势。
	 * 留空则强化那一击仍播本段原来的蒙太奇（只是伤害/击退变强）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower") TObjectPtr<UAnimMontage> EmpowerMontage;

	/** 强化命中在 Stage.DamageMultiplier 之上再乘的倍率。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0")) float EmpowerDamageMultiplier = 2.f;

	/** 强化命中的水平击退（沿攻击者朝向，cm/s）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0", Units="cm/s")) float EmpowerKnockback = 800.f;

	/** 强化命中的垂直上抛（cm/s）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0", Units="cm/s")) float EmpowerLaunch = 120.f;

	/** 三连击完美窗口武装的强化窗口时长（秒），填进 GE 的 SetByCaller Data.EmpowerDuration。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Empower", meta=(ClampMin="0", Units="s")) float EmpowerDuration = 3.f;

	// ---------------------------------------------------------------------
	// 完美窗口的 QTE 表现（由 UGA_ThreeHitPassive 播）
	// 窗口打开时刀上扫一道光当进度条，踩中窗口时出一声。
	// 放在这个资产上而不是能力类上：NS 和插槽都是每个英雄一份（蒙太奇也在这里）。
	// 而「窗口什么时候开、什么时候关」由能力算（见 ComputePerfectWindow），两边共用同一组时刻，
	// 提示光和判定不会各算各的。
	//
	// 「踩中窗口」和「强化那一击命中」是两回事：
	//   踩中 → 只给本人一声 QTE 反馈（下面的 PerfectWindowSuccessSound），此刻还没有命中信息；
	//   命中 → 才是在目标身上炸开的额外效果，走 GameplayCue.EmpoweredHit（见 UGC_EmpoweredHit），
	//          因为命中结算只在服务端跑，粒子必须在 cue 里生成才能复制到各客户端。
	// ---------------------------------------------------------------------

	/** 完美窗口打开时刀上扫过的那道光（NS_PerfectWindow）。留空 = 不生成提示。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") TSoftObjectPtr<UNiagaraSystem> PerfectWindowSystem;

	/** 提示光的两端：光从 Base 扫到 Tip（Kallari：sword_base_l → sword_tip_l）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") FName PerfectWindowBaseSocket = TEXT("sword_base_l");
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") FName PerfectWindowTipSocket = TEXT("sword_tip_l");

	/**
	 * 踩中完美窗口那一刻的音效（QTE 的「按对了」反馈）。默认 Kallari_Ability_ScoredCrit。
	 * 留空 = 踩中窗口没有任何声音。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") TSoftObjectPtr<USoundBase> PerfectWindowSuccessSound;

	// ---------------------------------------------------------------------
	// 普攻音效（由 UGA_ThreeHitPassive 在每段起手时播）
	// 三连击的蒙太奇里没有配任何 AnimNotify_PlaySound，所以不在这儿补的话普攻是完全没声的。
	// ---------------------------------------------------------------------

	/** 每段普攻起手的挥击音效。默认 Kallari_Effort_Swing。留空 = 普攻无声。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Sound") TSoftObjectPtr<USoundBase> AttackSound;

	/** 强化那一击起手的音效（有配就用它顶掉 AttackSound）。默认 Kallari_Effort_Ability_Primary_Strike。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|Sound") TSoftObjectPtr<USoundBase> EmpoweredAttackSound;

	/**
	 * NS_PerfectWindow 按原速播完要多久（秒）—— 也就是光从刀根走到刀尖的标准时长。
	 * 能力拿它和完美窗口的实际长度算一个播放倍率（SetCustomTimeDilation）：窗口比它短就快放、
	 * 比它长就慢放，光正好在窗口关闭那一刻扫到刀尖（进度条的观感全靠这个）。
	 * 填 0 = 不缩放，NS 按原速播。
	 * ⚠️ 只对 Age Update Mode = Tick Delta Time（默认值）的 NS 有效：DesiredAge 模式下
	 * CustomTimeDilation 不参与推进，改了也没反应。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX", meta=(ClampMin="0", Units="s")) float PerfectWindowSystemDuration = 1.f;

	/**
	 * NS_PerfectWindow 的 emitter 勾了 Local Space 就打开：喂进去的坐标会先转成 Niagara 组件的
	 * 局部坐标（组件挂在 mesh 上，所以是角色网格空间），光才会跟着角色走而不是糊在世界里。
	 * 和 NS_BladeTrail 上那个 bLocalSpace 一个意思。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX") bool bPerfectWindowSystemLocalSpace = false;

	/** 窗口关闭后等多久强拆提示光。NS 的 emitter 要是设成无限循环，auto destroy 永远等不到。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Attack|PerfectWindowVFX", meta=(ClampMin="0.05", Units="s")) float PerfectWindowTeardownDelay = 1.f;
};
