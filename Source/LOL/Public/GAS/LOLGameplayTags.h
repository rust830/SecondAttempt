// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "NativeGameplayTags.h"

// 游戏模块（非插件）有时 UBT 不定义 UE_PLUGIN_NAME，这里兜底，避免宏展开时报未定义。
#ifndef UE_PLUGIN_NAME
#define UE_PLUGIN_NAME TEXT("")
#endif
#ifndef UE_MODULE_NAME
#define UE_MODULE_NAME TEXT("LOL")
#endif

/** 代码强依赖的 GameplayTag，用原生静态注册（DLL 加载时即生效，早于一切 CDO 构造）。 */
namespace LOLGameplayTags
{	//
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Cooldown);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_StealthDuration);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_EmpowerDuration);
	// 伤害参数：伤害点上只填这两条，公式全在 UExecCalc_Damage 里（见方案文档 1.2 / 3.6）。
	// 原始伤害 = 攻击者 AttackDamage × DamageMultiplier + FlatDamage。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_DamageMultiplier);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_FlatDamage);
	// 格挡窗口 / 免疫时长（都由 GA_Block 用 SetByCaller 填给对应的 GE）。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_BlockWindow);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_BlockImmuneDuration);
	// 伤害类型：UExecCalc_Damage 按它挑护甲还是魔抗。什么都不挂 = 物理（近战/匕首的默认）。
	// 挂法见方案文档 3.1：伤害点用 Spec.Data->AddDynamicAssetTag(Damage_Physical) 之类。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Physical);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Magic);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_True);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_BasicAttack);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_ThrowConfirm);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_Repressed);


	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Flash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_ThrowDagger);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Stealth);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Block);
	/**
	 * 被动的冷却（比如「每 30 秒自动触发一次」那类）。
	 *
	 * 【现在没有能力在用它】—— 这是有意预留的：留着它，以后给被动配 CD 只需要在
	 * 被动的 CooldownGameplayEffectClass 上把 CooldownTags 指到这里，
	 * HUD 那条 `State.Cooldown.*` 的管道就自动认（不用回来改 HUD，也不用加新标签）。
	 *
	 * 语义上和另外五条完全同构：被动一样是「提交 CD → 挂 GE → 标签在=冷却中」，
	 * 区别只在被动按不出来（Kind=Passive，不画键位），以及触发方式不是按键。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Passive);
	// 格挡窗口开着（时长 = GE_Blocking 的 Duration）；免疫中（时长 = GE_BlockImmune 的 Duration）。
	// 判定读的就是这两个标签，没有第二个状态源。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Blocking);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_BlockImmune);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Stealth);
	// 强化普攻：破隐或三连击完美窗口触发后挂上的状态，下一次普攻命中吃倍率+击退并消耗掉。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_EmpoweredAttack);
	//slot
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Passive);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Q);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_W);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_E);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_R);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_D);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_F);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Block);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_Normal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_Ultimate);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_Summoner);
	//target
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Hero);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Void);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Terrain);
	//aiming
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Throw_Aiming);
	//gameplay cue
	// 注意：Cue 的蓝图子类必须命名为 GC_<Tag 去掉 GameplayCue. 后的路径，点换下划线>，
	// 否则引擎会把继承来的 GameplayCueTag 清掉、按类名重新推导出一个无效标签，Cue 静默不触发。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Flash);          // GameplayCue.Flash          → BP: GC_Flash
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Stealth);        // GameplayCue.Stealth        → BP: GC_Stealth
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_ThrowAiming);    // GameplayCue.ThrowAiming    → BP: GC_ThrowAiming
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_ThrowDagger_Hit);    // → BP: GC_ThrowDagger_Hit
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_ThrowDagger_Spawn);
	// 强化普攻状态的表现（强化动画 + 音效），挂在 UGE_EmpoweredAttack 上 → BP: GC_EmpoweredAttack
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_EmpoweredAttack);
	// 强化那一击【真的打到人】时的额外效果（反弹一刀的 NS + 音效）→ BP: GC_EmpoweredHit
	// 由 UGA_ThreeHitPassive::ApplyServerHit 在服务端 ExecuteGameplayCue，多播到各客户端。
	// 不放在能力里直接生成：命中结算只在服务端跑（客户端没有权威的 HitResult），
	// 在那儿 Spawn 出来的粒子只有主机看得到。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_EmpoweredHit);
	// 防护罩：挂在 UGE_BlockImmune 上 → 免疫挂上就出现、免疫结束（到期/被驱散）自动收掉。→ BP: GC_Block
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Block);

	//Kallari Ultimate DeathHarvest
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_DeathHarvest_Casting);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_DeathHarvest);

	/**
	 * 「按住 R 选目标」的窗口开着。
	 * 【纯本地】：只挂在按 R 那一端的 ASC 上（AddLooseGameplayTag），不复制、不参与任何 GAS 判定，
	 * 唯一的读者是 AHeroCombatCharacter::BasicAttackPressed —— 它决定左键这一下是普攻还是选目标。
	 * 生命周期：按 R 挂上 → 技能真的激活 / 松开 R / 死亡 时摘掉。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_DeathHarvest_Selecting);

	/**
	 * 「左键点中的是谁」这条事件（载荷放在 Payload.Target 上）。
	 * GA_DeathHarvest 在 AbilityTriggers 里声明了要听它（配合 ActivationPolicy = OnEvent），
	 * 服务端 UMyAbilitySystemComponent::SubmitManualTargetOnServer 用 HandleGameplayEvent 发一次，
	 * 载荷就变成 ActivateAbility 的 TriggerEventData ——「点了谁」是这样过网络的
	 * （GameplayEvent 本身只在本地派发，不复制）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_CastAt);
	//DeathHarvest Cue
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_Portal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_Teleport);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_SpinStart);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_SpinEnd);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_MissingHealthBonus);

	// ---------------------------------------------------------------------
	// 单位状态（死亡 / 控制）。见 GAS_DeathHarvest_Setup.md §7 的架构缺口。
	//
	// 这三个是【施法侧】的准入状态：谁挂上它们，谁就被挡在技能激活之外。
	// 挡人的声明分两层（不要写到别处去）：
	//   基类默认  UMyGameplayAbility 构造函数 → State.Dead + State.Stunned 挡所有技能
	//   技能按需  各技能的构造函数里再加 State.Silenced（普攻不加，见下）
	//
	// State.Silenced 只加在【法术】技能上，普攻（GA_ThreeHitPassive）不加 ——
	// 沉默不挡普攻是 LoL 语义。加错的表现是「被沉默之后连平 A 都打不出来」。
	// ---------------------------------------------------------------------

	/** 已死亡。由 UGE_Death 授予，重生时随 GE 到期自动摘掉。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dead);
	/** 眩晕/击飞等硬控。由 UGE_Stun 授予。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Stunned);
	/** 沉默：只挡法术，不挡普攻、不挡移动。由 UGE_Silence 授予。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Silenced);

	/** UGE_Death 的时长 = 复活等待时间（SetByCaller）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_RespawnDelay);
	/** UGE_Stun / UGE_Silence 的时长（SetByCaller）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_ControlDuration);

	// ---------------------------------------------------------------------
	// 减速（见 UGE_Slow）
	//
	// 减速是【属性】不是【标签】：UGE_Slow 把 MoveSpeed 乘一个倍率，
	// 谁读属性谁就自然慢下来。State.Slowed 只是给动画层看的影子 ——
	// AHeroCombatCharacter 把属性接到 CharacterMovement->MaxWalkSpeed 上，
	// UHeroAnimationSet 读 State.Slowed 决定换不换移动混合空间。
	// 两者不是一回事：属性管「真的走多慢」，标签管「播哪份动画」。
	// ---------------------------------------------------------------------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Slowed);
	/** UGE_Slow 的时长（SetByCaller，秒）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_SlowDuration);
	/** UGE_Slow 的速度倍率（SetByCaller，0.7 = 减速 30%）。乘算，多个减速叠乘。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_SlowMultiplier);

	// ---------------------------------------------------------------------
	// 动画动作字典的键（见 UHeroAnimationSet::ActionMontages）
	//
	// 【这些不是状态标签，别往 ASC 上挂】它们从不参与 GAS 判定，
	// 只是「动作名 → 蒙太奇」这张表的 key，让能力侧能说
	// 「播 Anim.Block」而不是「播我这个成员变量里的 BlockMontage」。
	// 挂上去没有任何效果，只会污染标签容器。
	// ---------------------------------------------------------------------
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Anim_Attack);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Anim_EmpoweredAttack);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Anim_Block);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Anim_Stealth);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Anim_Flash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Anim_ThrowAiming);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Anim_DeathHarvest);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Anim_HitReact);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Cast);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_TeleportOut);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Portal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Appear);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Spin);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Hit);
}
