// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeathHarvestData.generated.h"

class UAnimMontage;
class UGameplayEffect;
class USoundBase;

/**
 * Death Harvest（R 大招）的全部可调数值，仿 UThreeHitPassiveData。
 *
 * 为什么放 DataAsset 而不是技能的 UPROPERTY：一个英雄一份数值，换英雄/调平衡不碰 C++ 也不碰 BP 图，
 * 而且和 UThreeHitPassiveData / 破隐那套「数值只在一个地方」的纪律一致。
 */
UCLASS(BlueprintType)
class LOL_API UDeathHarvestData final : public UDataAsset
{
	GENERATED_BODY()
public:
	// ---------------------------------------------------------------------
	// 动画
	// ---------------------------------------------------------------------

	/**
	 * 大招蒙太奇 —— 现在只需要盖住【转圈那一段】：消失 → 开门 → 现身这三段没有动画在播
	 * （相位由下面两个时长推，见 GAS_DeathHarvest_Setup.md §2）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Anim")
	TObjectPtr<UAnimMontage> Montage;

	/** 从哪一段开始播（留空 = 从头）。代码不认段名，指向转圈那一节即可。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Anim")
	FName MontageStartSection = NAME_None;

	// ---------------------------------------------------------------------
	// 相位时间（P1→P2→P3）。这三段没有蒙太奇在播，所以只能按时长走。
	// 两端各自按同一份时长跑：服务器从 CommitAbility 起算，客户端从
	// ClientActivateAbilitySucceed 起算，差值就是一个单程 RTT（几十 ms 级）。
	// ---------------------------------------------------------------------

	/** P1 消失 → P2 开门：人不见多久之后开门（秒）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Phase", meta=(ClampMin="0", Units="s"))
	float VanishToPortalDelay = 0.5f;

	/** P2 开门 → P3 现身：门开多久之后人从门里出来（秒）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Phase", meta=(ClampMin="0", Units="s"))
	float PortalToAppearDelay = 0.6f;

	// ---------------------------------------------------------------------
	// P4 起手（现身那一帧）
	//
	// 「现身就开转」：P3 传送完的【同一帧】播蒙太奇、挂转圈 cue、开始甩镜，没有"先站一会儿"。
	// 但打击感（慢放 + 伤害跳 + 顿帧 + 震动）【不】在这一帧 —— 它跟着 SpinImpactDelay 推后，
	// 落在刀真正扫起来那一帧上。这一段里蒙太奇是正常速度播的，所以不会看到"站在原地等"。
	//
	// ⚠️ 慢放动的是【角色自己的】CustomTimeDilation，不是世界时间：只有她的动画和组件 tick
	//    变慢，别的玩家、粒子、世界计时器、伤害节奏统统不受影响。所以这里填的是【真实秒】，
	//    也不用担心漏还原会毁掉整局（最坏只是她一直慢动作）。代价见实现的注释：
	//    CustomTimeDilation 不复制，旁观者那台机器看不到这段慢放。
	// ---------------------------------------------------------------------

	/**
	 * 从蒙太奇起播到「刀真正扫起来」之间有多少秒（【真实】秒）。
	 *
	 * 为什么需要它：转圈蒙太奇只有一段（Spin），开头那段是抬刀、转上身，刀扫出去在这之后。
	 * 慢放和镜头震动如果打在蒙太奇第 0 帧上，玩家看到的是"现身之后卡了一下" ——
	 * 那一顿和刀的加速度完全对不上。填对这个值以后，顿/震正好落在第一跳伤害上。
	 *
	 * 0 = 不推后（= 回到"现身那一帧就顿"的旧行为）。
	 * ⚠️ 这段时间是从转圈总时长里扣掉的：要保证
	 *    SpinImpactDelay + SpinCount × SpinInterval ≤ 蒙太奇长度，
	 *    否则最后几跳会被收招掐掉。起手时服务器会打一条日志把三个数摆在一起，对不上一眼就能看出来。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="SpinLead", meta=(ClampMin="0", Units="s"))
	float SpinImpactDelay = 0.25f;

	/** 「顿」持续多久（【真实】秒，从刀扫起来那一帧 = SpinImpactDelay 之后起算）。0 = 不顿。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="SpinLead", meta=(ClampMin="0", Units="s"))
	float SpinSlowTime = 0.15f;

	/** 那段时间里她自己的时间流速（1 = 不慢，0.15 = 几乎定住）。越小越"顿"。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="SpinLead", meta=(ClampMin="0.01", ClampMax="1"))
	float SpinSlowScale = 0.15f;

	/** 关掉 = 起手不放慢 + 命中不顿帧（甩镜和震动照旧）。调效果时拿来 A/B 很方便。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="SpinLead")
	bool bSlowMotionEnabled = true;

	/** 甩镜一共甩多久（【真实】秒，从头转到大致到位要这么久）。0 = 不碰镜头。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="SpinLead", meta=(ClampMin="0", Units="s"))
	float CameraAlignTime = 0.45f;

	/**
	 * 甩镜速度（1/秒，也就是 RInterpTo 的 InterpSpeed）。越大越硬，越小越飘。
	 * 插值用的是【真实】delta，所以任何慢放都不会让甩镜跟着变慢。
	 * 这里不写 Units：UHT 认识的单位是 cm/s/kg 那一套，"/s" 会直接编译报错。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="SpinLead", meta=(ClampMin="0"))
	float CameraAlignSpeed = 9.f;

	// ---------------------------------------------------------------------
	// P4 命中顿帧
	//
	// 打到人那一下把【世界】冻住一瞬。这个必须是世界的：只慢她自己的话敌人照常动，
	// 看起来不像"打中了"而像"她卡了"。服务器按下去，WorldSettings::TimeDilation 会复制到全场，
	// 所以每一端顿的是同一帧（连被打的人屏幕也一起顿 —— 那正是顿帧该有的样子）。
	//
	// ⚠️ 第一跳是【StartSpinPulses 那一帧】就打的（AbilityTask_Repeat::Activate 先 PerformAction
	//    再排定时器）—— 也就是起手之后 SpinImpactDelay 秒，不是现身那一帧。
	//    而人是从目标身后 140cm 现身的、扫描半径 320cm，所以正常情况下第一跳必中 ——
	//    也就是说这两段慢放默认是叠在同一个瞬间的，那一下最重。想分开调就把其中一个填 0。
	// ---------------------------------------------------------------------

	/** 顿帧持续多久（【真实】秒）。0 = 不顿。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="HitStop", meta=(ClampMin="0", Units="s"))
	float HitStopTime = 0.07f;

	/** 顿帧期间的世界流速（0.05 = 几乎冻住）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="HitStop", meta=(ClampMin="0.01", ClampMax="1"))
	float HitStopScale = 0.05f;

	/**
	 * false = 只在【第一次】真的打到人那一跳顿一下（默认，起手那一下最有分量）。
	 * true  = 每一跳命中的都顿 —— 8 跳 × HitStopTime 会把整段转圈明显拉长，想要"连击感"再开。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="HitStop")
	bool bHitStopEveryPulse = false;

	// ---------------------------------------------------------------------
	// P0 选目标（客户端按住 R、左键点中谁就是谁；服务端只校验，不代选）
	// ---------------------------------------------------------------------

	/**
	 * 点中的目标离自己不能超过这么远（cm）。客户端射线本身的长度不在这里 —— 那个只决定
	 * 「能找到多远的候选」，真正卡住射程的是服务端这一次校验。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Lock", meta=(ClampMin="0", Units="cm"))
	float LockRange = 1400.f;

	// v2 在这里有过 LockHalfAngle / LockSphereRadius 两个「自动锁敌」的配置项（沿 control rotation
	// 锥形扫描的角度和球半径）。v3 改成手动点选之后，服务端不再自己挑目标，两个值都没有消费者了 ——
	// 删掉而不是留着，免得以后有人调了它们却看不到任何变化。DS 资产里那两条旧值会被自动忽略。

	// ---------------------------------------------------------------------
	// P2 落点（目标身后）
	// ---------------------------------------------------------------------

	/** 落点在目标身后多远（cm）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Blink", meta=(ClampMin="0", Units="cm"))
	float BehindDistance = 140.f;

	/** 贴地检测：从落点上方这么多开始往下打（cm）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Blink", meta=(ClampMin="0", Units="cm"))
	float GroundTraceUp = 150.f;

	/** 贴地检测：往下打这么多（cm）。打不中地面就用原始 Z（悬空/浮空平台会碰到）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Blink", meta=(ClampMin="0", Units="cm"))
	float GroundTraceDown = 400.f;

	// ---------------------------------------------------------------------
	// P4 转圈
	// ---------------------------------------------------------------------

	/**
	 * 每两跳之间的间隔（秒）。
	 * ⚠️ 整段伤害的时间 = SpinInterval × SpinCount，它必须 ≤ 蒙太奇里转圈那一节的长度：
	 *    技能是「蒙太奇播完就收招」的，蒙太奇短了后面的伤害跳会被一起掐掉。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Spin", meta=(ClampMin="0.01", Units="s"))
	float SpinInterval = 0.25f;

	/** 一共跳几次。跳完伤害就停（蒙太奇可能还在播收招）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Spin", meta=(ClampMin="1"))
	int32 SpinCount = 8;

	/** 每跳的球形扫描半径（cm）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Spin", meta=(ClampMin="0", Units="cm"))
	float SpinHitRadius = 320.f;

	// 转圈这件事【没有】可配的旋转速度：动画自己转一圈（没勾 root motion 也一样转 ——
	// 那是根骨骼在转，不是 actor 在转），代码全程不碰 actor 的 yaw。
	// v1 在这里有过 bRootMotionSpin / SpinRate 一对开关（代码按角速度逐帧 AddActorWorldRotation），
	// 现在删掉了：动画在转的情况下再叠一层代码旋转就是【转两倍速】，而「动画转、代码不转」
	// 正是当前资产的真实情况。朝向由 P3 现身那一下定（面向目标），转完一圈 360° ≡ 原朝向，
	// 所以收招也不用回正。

	// ---------------------------------------------------------------------
	// 伤害
	// ---------------------------------------------------------------------

	/** Instant GE，靠 SetByCaller 吃 Data.DamageMultiplier / Data.FlatDamage / Data.MissingHealthBonus。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage")
	TSubclassOf<UGameplayEffect> DamageEffect;

	/** 攻击力倍率（原始伤害 = 攻击者 AttackDamage × 这个 + FlatDamage）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage", meta=(ClampMin="0"))
	float DamageMultiplier = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage", meta=(ClampMin="0"))
	float FlatDamage = 0.f;

	/**
	 * 斩杀系数：目标每损失 100% 生命，这一下最多多打这么多（0.5 = 残血时 ×1.5）。
	 * ★ 加成对象是【目标的】已损失生命值，公式在 UExecCalc_Damage（§5.5）。
	 * 想改成施法者的，把 ExecCalc 里那两条捕获从 Target 改成 Source 即可，一行的事。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage", meta=(ClampMin="0"))
	float MissingHealthBonus = 0.5f;

	// 音效不在这里配：消失/现身那两下由 GC_DeathHarvest_Cast 播（那条 cue 每台机器都会跑一次，
	// 服务器单方面 PlaySoundAtLocation 只有主机听得到），转圈的音效在蒙太奇里挂 AnimNotify_PlaySound。
};
