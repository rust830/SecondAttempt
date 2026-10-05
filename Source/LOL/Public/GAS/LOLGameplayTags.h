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
	/**
	 * 技能消耗（SetByCaller，扣的是 Energy）。由 UGE_AbilityCost 读。
	 *
	 * 施加方【必须填负值】：GE 上的 modifier 是 Additive，填 -50 才是「扣 50 蓝」。
	 * UMyGameplayAbility::ApplyCost 统一取 -ManaCost，不要在调用点各写各的符号 ——
	 * 以前 Data.Cooldown 就吃过这个亏（填 0 = 冷却 0 秒 = 看起来没生效）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Cost);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_StealthDuration);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_EmpowerDuration);
	// 伤害参数：伤害点上只填这两条，公式全在 UExecCalc_Damage 里（见方案文档 1.2 / 3.6）。
	// 原始伤害 = 攻击者 AttackDamage × DamageMultiplier + FlatDamage。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_DamageMultiplier);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_FlatDamage);
	/**
	 * 这一下【可以暴击】。由施加方显式声明，UExecCalc_Damage 不猜。
	 *
	 * 为什么不是「ExecCalc 里按伤害类型判断」：判断规则一定会漂（哪些技能能暴击是设计问题，
	 * 不是物理/魔法的问题），而且会让「为什么这个技能不暴击」变成一个只能翻 C++ 才能回答的问题。
	 * 现在是：伤害点上填 1 就能暴，不填就不能 —— 和 Data.DamageMultiplier 同一个口径。
	 *
	 * 目前只有普攻（GA_ThreeHitPassive::ApplyServerHit）填它；匕首 / 死亡收割 / 三连击的额外伤害不填。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_CanCrit);
	/**
	 * 这一下是【普攻】（含强化普攻）。由施加方显式声明，UExecCalc_Damage 不猜。
	 *
	 * 消费者：生命偷取（LifeSteal）只回普攻的伤害；全能吸血（Omnivamp）不看这个标签，
	 * 所有伤害都回。为什么单独一个标签而不是借 Data_CanCrit：能不能暴击是数值设计，
	 * 是不是普攻是伤害来源分类 —— 两者将来完全可能分家（比如「普攻不可暴击的 debuff」），
	 * 借道会让吸血跟着暴击一起静默失效。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_BasicAttack);
	/**
	 * 瞬时回血量。GE_Heal 用它从 SetByCaller 拿数值 —— 回血点填多少回多少，
	 * 和 Data.DamageMultiplier 同一个口径（数值在施加点算好，GE 只当载体）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Heal);
	// 格挡窗口 / 免疫时长（都由 GA_Block 用 SetByCaller 填给对应的 GE）。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_BlockWindow);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_BlockImmuneDuration);
	/**
	 * 闪避无敌窗口的时长（GA_Dodge / GA_GroundDodge 用 SetByCaller 填给 GE_DodgeWindow）。
	 * 和 Data.BlockWindow 同一个口径：窗口长度配在技能上，GE 只当载体。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_DodgeWindow);
	/**
	 * 瞬时回蓝量。GE_EnergyRestore 用它从 SetByCaller 拿数值（完美闪避的奖励）。
	 * 和 Data.Heal 同一个口径 —— 数值在施加点算好，GE 只当载体。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_EnergyRestore);
	// 伤害类型：UExecCalc_Damage 按它挑护甲还是魔抗。什么都不挂 = 物理（近战/匕首的默认）。
	// 挂法见方案文档 3.1：伤害点用 Spec.Data->AddDynamicAssetTag(Damage_Physical) 之类。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Physical);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Magic);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_True);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_BasicAttack);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_ThrowConfirm);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_Repressed);

	/**
	 * 空手形态的地面连招输入（喂给 Ability.Slot.Combo 上的四连拳）。
	 *
	 * 【为什么不用 Event.Input.BasicAttack —— 那是双触发，不是复用】
	 * 三连普攻（持刀）和四连拳（空手）都用 GA_ThreeHitPassive，机制完全一样：
	 * `AttackInputTag` 订阅输入事件来推进连段。如果两者填同一个标签，
	 * 两个能力【都装在同一个角色上】时，空手按一次左键会同时唤醒两个能力
	 * ⇒ 一次按键打出两段伤害。
	 *
	 * 而「按形态只装配其中一个」又要求形态切换时重装 ASC（GA_FormSwitch 里加装配逻辑），
	 * 还要处理 GE 复制时序 —— 代价远大于多注册一个标签。
	 *
	 * 所以：两个形态各发各的标签，谁也听不见对方的。
	 * AHeroCombatCharacter::RouteBasicAttackInput 按形态二选一发事件。
	 * 代价是四连拳的 BP 里要填两处：AbilityTriggers 的 GameplayEvent + AttackInputTag。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_ComboAttack);
	/**
	 * 闪避后派生 JumpKick：闪避窗口内按普攻，输入层把左键改判成「踢」。
	 * 和 Event.Input.ThrowConfirm 同一套机制（本地派发 + 非权威端镜像 RPC），见 GA_Dodge。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_DodgeKick);
	/**
	 * 闪避窗口内按空格派生「二段 evade」：输入层把空格改判成第二次闪避（GA_Dodge 接住）。
	 * 和 Event.Input.DodgeKick 同一套机制。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_DodgeEvade);

	/**
	 * 普攻蒙太奇里的「这一下该结算了」。
	 *
	 * 由蒙太奇上的 UAnimNotify_SendGameplayEvent 在挥砍命中帧发出，UGA_ThreeHitPassive 用
	 * WaitGameplayEvent 接住并当场结算 —— 判定时刻因此跟着【动画帧】走，而不是跟着
	 * Stage.HitTime 那个定时器走（改攻速/换蒙太奇/改蒙太奇里的节奏时，定时器的数字就悄悄错位了）。
	 *
	 * 注意 notify 默认 bServerOnly=true：只有服务端会发。这是对的 —— 命中结算只在服务端跑。
	 * 没配这个通知时能力不会报错，会退回 HitTime 定时器（见 UGA_ThreeHitPassive::ResolveHit）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_Impact);


	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Flash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_ThrowDagger);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Stealth);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Block);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Dodge);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_GroundDodge);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_SpinSlash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_TurnSlash);
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
	/**
	 * 槽位族的父标签「Ability.Slot」。下面每一个 Ability_Slot_* 都是它的子标签。
	 *
	 * 【为什么需要这个锚点】UMyAbilitySystemComponent::OnGiveAbility / OnRemoveAbility
	 * 靠它判断「这个能力要不要进 SlotAbilityMap」：
	 *   - 早先的写法是 `Tag.ToString().StartsWith(TEXT("Ability.Slot."))`，
	 *     每次授权都要把**所有**动态标签 ToString() 一次做字符串比较；
	 *   - 现在换成 `Tag.MatchesTag(Ability_Slot_Root)` —— 一次整数层级比较，
	 *     不分配字符串，也不会因为 FName→String 的实现细节翻车。
	 *
	 * ⚠️ 写槽位标签时必须是 Ability_Slot_Root 的子（或它本身）。
	 *   写成 "Slot.Ability.Xxx" ⇒ 永不进 SlotAbilityMap ⇒ 按键毫无反应且无日志。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Root);
	//slot
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Passive);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Q);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_W);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_E);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_R);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_D);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_F);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Block);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_GroundDodge);

	/**
	 * 空手形态的地面连招（GA_ThreeHitPassive + 四段数据，ParagonCrunch 拳击重定向而来）。
	 *
	 * 【为什么单独一个槽位，而不是和 Ability.Slot.Passive 共用】
	 * 左键在两种形态下要打不同的东西：持剑 = 三连普攻，空手 = 四连拳。
	 * 但两个能力不能塞进同一个槽位 —— `SlotAbilityMap.Add` 是**静默覆盖**，
	 * 后装配的会把先装配的顶掉，症状是「其中一种形态的普攻完全没有反应」。
	 * 所以分两个槽位，AHeroCombatCharacter::RouteBasicAttackInput 按当前形态选激活哪个。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Combo);

	/**
	 * 空中攻击（跳跃后按左键）。双形态共用一个能力，动画在能力内部按形态选
	 * —— 所以是【一个槽位一个能力】，不是两个。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_AirAttack);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_Normal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_Ultimate);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Type_Summoner);
	//target
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Hero);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Void);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Terrain);
	//aiming
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Throw_Aiming);
	/**
	 * 闪避（GA_Dodge）的「派生 JumpKick 窗口」开着。
	 * 【纯本地】loose 标签：只在按键那一端挂（不复制），唯一的读者是
	 * AHeroCombatCharacter::BasicAttackPressed —— 它决定左键这一下是普攻还是派生 JumpKick。
	 * 生命周期：闪避起手挂上 → 窗口到期 / 派生出踢 / 死亡 时摘掉（GA_Dodge 自己管）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dodge_Active);
	/**
	 * 闪避的无敌窗口开着 —— 这一刻挨到的伤害会被完全吃掉，并且算【完美闪避】
	 *（回蓝 + 子弹时间 + 镜头推近，见 UDodgeComponent）。
	 *
	 * 【和 State.Dodge.Active 的区别（很容易混）】
	 *   State.Dodge.Active  = 派生 JumpKick 的窗口，【纯本地】loose 标签，读者是输入层。
	 *   State.Dodge.Window  = 无敌窗口，由 GE_DodgeWindow 授予 ⇒ 【会复制】。
	 * 必须复制的原因：判定跑在服务端（UExecCalc_Damage），而被闪避的那一方通常是远端玩家 ——
	 * 服务端得能独立知道「他这一刻在无敌窗口里」，不能依赖客户端本地挂的标签。
	 *
	 * 生命周期由 GE 的时长管（= 闪避技能上的 PerfectDodgeWindow），到期引擎自己摘；
	 * 一次窗口只吃一击（躲掉之后 UDodgeComponent 立刻把 GE 摘掉，和格挡同一套）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dodge_Window);
	/**
	 * 连段窗口（GA_ThreeHitPassive / GA_BoxingCombo 的三段、四段连招）开着 ——
	 * 此刻再按一次普攻能接上下一段。
	 *
	 * 【为什么必须有这个标签】窗口在此之前是技能里的一个私有 bool：真人玩家靠看动画
	 * 决定什么时候接，而 AI 没有眼睛。它只能按固定间隔瞎按，而窗口只有 0.1~0.3 秒
	 * （ChainWindowOpenTime ~ CloseWindowTime × 蒙太奇长度），固定间隔（Bot 是 1.1s）
	 * 撞进去的概率约等于零，且撞不上之后要白等一整个间隔 —— 表现就是"永远只有第 0 段"。
	 * 把窗口挂成标签之后，AI（和以后的血条/连招提示 UI）就能读到同一个事实。
	 *
	 * 【纯本地】loose 标签：只在跑这个技能实例的那一端挂（不复制）。
	 * 生命周期：窗口开挂上 → 窗口关 / 起手段复位 / EndAbility 兜底摘掉（GA 自己管）。
	 * 读者目前只有 AArenaBotController（它和 Bot 的 ASC 在同一端）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_ComboWindow);
	/**
	 * 本段连招【有】完美窗口，而且它还没结束 —— 从起手段挂上，到完美窗口关闭（或本段/本能力结束）摘掉。
	 *
	 * 【和 State.PerfectWindow 的分工】这个标签回答"要不要等"，PerfectWindow 回答"现在按"。
	 * 只有第 1 段（剑套）配了完美窗口，其余段一个都没有 —— 没有这个标签的话，
	 * AI 在"没配完美窗口的段"上会一直等一个永远不会开的窗口，连段直接断掉。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_PerfectWindowArmed);
	/**
	 * 完美窗口正开着 —— 此刻按连段就是完美（QTE），能吃到强化普攻（GrantEmpoweredAttack）。
	 *
	 * 【为什么要有】这是连段窗口里的一小段子区间（剑套第 1 段是蒙太奇 [0.25, 0.50]，
	 * 而连段窗口是 [0.10, 0.60]），判定是"连段窗口内的【第一次】按键落在区间里"
	 * （UGA_ThreeHitPassive::OnAttackInput：bQueuedNextStage 之后直接 return）。
	 * 也就是说【早按 = 消耗掉这一段的完美机会】，AI 必须等这个标签，不能照旧提前按。
	 *
	 * 【纯本地】loose 标签，和 State.ComboWindow 同一套路：只跑技能的那一端挂、不复制、
	 * Add/Remove 幂等。读者目前只有 AArenaBotController。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_PerfectWindow);
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
	// 受击：按「伤害从哪个方向来」播对应的受击动画。→ BP: GC_HitReact
	// 由 UHeroCombatAttributeSet::PostGameplayEffectExecute 在服务端 ExecuteGameplayCue 发出，
	// 多播到各端各自播一次（和 GC_ThrowDaggerHit 同一条路）。致命的那一下也要发 ——
	// 客户端靠它算出「向前倒还是向后倒」，见 UGC_HitReact。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_HitReact);
	// 普攻（含强化那一击）打到人时的打击表现：命中点粒子 + 音效，外加攻击者本地的镜头振动。
	// → BP: GC_MeleeHit
	// 由 UGA_ThreeHitPassive::ApplyServerHit 在服务端 ExecuteGameplayCue，多播到各客户端各自跑一次。
	// 和 GameplayCue.EmpoweredHit 的分工：这条是【每一击都有】的基础打击感，
	// 那条只是强化那一击额外的爆炸效果，两条会同时出现在同一个命中点上。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_MeleeHit);
	// 眩晕/击退的硬控表现（眩晕/击退 Montage），分别挂在 UGE_Stun / UGE_Knockback 上。
	// 生命周期 = GE 生命周期：挂上 → OnActive 播蒙太奇，到期/被打断 → OnRemove 收掉。
	// → BP: GC_Stun / GC_Knockback
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Stun);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Knockback);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_KnockUp);
	// 闪避（GA_Dodge）的喷射表现：施法瞬间的 cascade 喷射粒子 + 尾迹。→ BP: GC_Dodge
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Dodge);
	// 飞踢的脚上拖尾 Niagara（Actor cue，OnActive 挂 / OnRemove 收）。→ BP: GC_DodgeKick
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DodgeKick);
	// 飞踢踢中那一下的命中 Niagara（Static cue）。→ BP: GC_DodgeKickHit
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DodgeKickHit);
	// 空手形态 dodge 派生【直拳】踢中那一下的命中 Niagara（Static cue）。→ BP: GC_DodgePunchHit
	// 和飞踢的 hit cue 分开：拳和脚的特效语言不同（见 UGA_Dodge::PunchHitCueTag）。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DodgePunchHit);
	// 飞踢【扑空】时的表现（风声 / 尘土，Static cue，可选）。→ BP: GC_DodgeKickWhiff
	// 由 GA_Dodge::WhiffCueTag 引用；留空则该标签无人使用，无害。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DodgeKickWhiff);

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
UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Invulnerable);
	/** 眩晕（硬控，站定不动）。由 UGE_Stun 授予。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Stunned);
	/** 沉默：只挡法术，不挡普攻、不挡移动。由 UGE_Silence 授予。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Silenced);
	/** 击退/击飞（硬控，被击退期间挡一切技能与移动）。由 UGE_Knockback 授予。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Knockback);
	/**
	 * 击飞（硬控，垂直升空、落地前挡一切技能与移动）。由 UGE_KnockUp 授予。
	 * 和 State.Knockback 分开：击退是水平位移（正面/背面方向驱动动画），击飞是垂直升空，
	 * 两者动画/手感/能否被韧性减免都不同，不该共用一个标签。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_KnockUp);

	/** UGE_Death 的时长 = 复活等待时间（SetByCaller）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_RespawnDelay);
	/** UGE_Stun / UGE_Silence 的时长（SetByCaller）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_ControlDuration);
	/** UGE_Knockback 的时长（SetByCaller，秒）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_KnockbackDuration);
	/** UGE_Knockback 的水平击退冲量（SetByCaller，cm/s）。由 GE 组件 UGEComponent_Knockback 读。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_KnockbackImpulse);
	/** UGE_Knockback 的垂直上抛冲量（SetByCaller，cm/s）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_KnockbackLaunch);
	/** UGE_KnockUp 的时长（SetByCaller，秒）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_KnockUpDuration);
	/**
	 * 标记「这一下把人打死了」。挂在受击 cue 的 AggregatedSourceTags 上。
	 * 客户端据此知道：方向要记（死亡蒙太奇要用），但【不要】播受击动画 ——
	 * 死亡蒙太奇同一帧就接管了，两个一起上会打架。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Lethal);

	/**
	 * 空手形态。挂上 = 角色进入「没拿刀」的形态。
	 *
	 * 读者有两个：
	 *   1. UHeroAnimInstance::FormUnarmed —— 把标签翻译成 0~1 的平滑权重，
	 *      喂给 ABP 里的 PoseBlend（PoseAsset 差值叠加），只改姿势不改动作；
	 *   2. 攻击类能力 —— 选蒙太奇时按形态挑各自设计的版本。
	 *
	 * 谁挂谁摘：以后由切换形态的技能 / 武器逻辑负责（GE 授予，生命周期即形态）。
	 * ASC 标签天然复制，主机和客户端的动画层读到的是同一个状态。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Form_Unarmed);

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

	// =====================================================================
	// Audio.*：音效事件标签（2026-10-04 新增，见 CodeReview/12_音效层.md）
	//
	// 【这些标签不挂在任何 ASC 上，别往身上 AddLooseGameplayTag，也别让 GE 去设】
	// 它们只是【配置表的键】：UHeroAudioConfig 里 Audio.Ui -> 某个音效资产，
	// 发音效的地方拿一个 Audio.* 标签问 UHeroAudioLibrary::PlayAt 要声音。
	//
	// 【为什么走了 tag 而不是「每个 GC 自己存一个 Sound 属性」】
	// 收敛前 19 个音效槽位散在 16 个类里，默认值一半写死在 C++ 构造函数、
	// 一半靠人手在蓝图里点，填漏了 PlaySoundAtLocation 收 nullptr 静默 no-op，
	// 全项目只有 2 处打过 warning。改成 tag -> 音效 一张表之后：
	//   ① 缺声音是【一张表的事】，Validate() 一次就能列全；
	//   ② GC 只保留一个 SoundOverride（想单个覆盖就填，不想就空着查表）；
	//   ③ 换 Paragon 资源 / 换包只改一张表，不用动 C++ 也不用动 16 个蓝图。
	//
	// 【裸 SoundWave vs SoundCue】一律配 SoundCue：Paragon 的 cue 内部是
	// Kallari_*_NNN_Dialogue 多句随机，直接拿单条 Wavs 下的 wav 会丢掉变体随机。
	// =====================================================================
	// 近战命中的【物理打击音】事件。
	//
	// 三段普攻【不】在这里分句：命中事件表（UHeroAudioConfig）以 FGameplayTag 为键，
	// 而 UE 5.8 的 Python 侧造不出 FGameplayTag（GameplayTagSettings / request_gameplay_tag
	// 都没导出），没法从脚本往表里灌新键。连段的分句改由 FThreeHitAttackStage::HitSound 承担 ——
	// 它本来就是「这一段的表现参数」，摆在 DS_Passive 里比摊在全局事件表直观。
	// 本条是兜底：连段没填 HitSound、或其它近战能力（GA_FormMelee）打中时走这里。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_MeleeHit);          // 近战命中（兜底）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_MeleeHitBoxing);    // 空手四连拳命中（Audio.MeleeHit 的形态变体）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_PassiveAttackBoxing); // 空手四连拳挥击（Audio.PassiveAttack 的形态变体）
	// 这两条是【同一个事件的空手变体】，不是独立事件：普攻有持剑（三段）/ 空手（四段）两套连招，
	// 共用一个 GameplayCue.MeleeHit、共用一条事件表键，但两形态的打击音色必须能分开配 ——
	// 空手是短促的闷响、持剑要金属声，塞在同一条表里只能靠下标错开，改一段容易牵连另一形态。
	// 判形态不靠传参（FGameplayCueParameters 里没有这个槽位），而是读攻击者 ASC 上的
	// State.Form.Unarmed —— 形态本来就是角色身上的状态，见 GC_MeleeHit 和 GA_ThreeHitPassive 的用法。
	// 表里没配这两条键 → 自动退回上面那条基础事件，不会静默无声。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_ThrowDaggerHit);    // 匕首命中
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_ThrowDaggerThrow);  // 匕首投掷出手（能力层）
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_ThrowAiming);       // 匕首瞄准抬手
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_Dodge);             // 闪避喷射
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_DodgeKickHit);      // 雷欧飞踢命中
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_DodgePunchHit);     // 飞踢拳击段命中
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_Block);             // 格挡成立
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_StealthEnter);      // 隐身进入
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_StealthExit);       // 破隐
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_Flash);             // 闪现
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_EmpoweredAttackEnter); // 进入强化形态
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_EmpoweredAttackExit);  // 退出强化形态
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_EmpoweredHit);      // 强化那一下打到人
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_PerfectWindow);     // 完美窗口判定成功
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_PassiveAttack);     // 普攻挥击
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_PassiveEmpoweredAttack); // 强化普攻挥击
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_DeathHarvestVanish); // 死亡收割·消失吟唱
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_DeathHarvestAppear); // 死亡收割·现形
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_DeathHarvestPortalOpen);  // 传送门开
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_DeathHarvestPortalClose); // 传送门关
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_DeathHarvestSpin);  // 死亡收割·旋转
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Audio_DeathHarvestBurst); // 死亡收割·爆裂

	// =====================================================================
	// 斗魂竞技场：装备 / 海克斯的数值通道
	//
	// 【这些标签不挂在任何 ASC 上，别往身上 AddLooseGameplayTag】它们是
	// SetByCaller 的【键】，唯一用途是让 UGE_ArenaItem 的某个修正符知道
	// 「我该读哪个数」。挂上去了没有任何效果，只污染标签容器 ——
	// 和 Anim.* 那批是同一类东西（见上面 Anim 段落）。
	//
	// 【为什么一条属性一个标签】UGE_ArenaItem 是一份【通用】GE：修正符表在
	// C++ 里写死一次（属性 ↔ 标签的对应表见 ArenaItemStats.h），具体数值
	// 由施加方按每件装备的 DataAsset 用 SetByCaller 填。数值必须走 SetByCaller
	// 而不是「运行时改 GE 的 Modifiers」，是因为：
	//   SetByCaller 的数值随 FGameplayEffectSpec 复制给【拥有者客户端】
	//   （FGameplayEffectSpec::SetByCallerTagMagnitudes 在 NetSerialize 里），
	//   客户端因此能用和服端【一模一样】的修正符重算属性；
	//   而运行时 NewObject 改出来的 Modifiers 只活在服务端那份上，
	//   客户端会拿到一份空修正符 —— 表现是「自己看自己没吃到装备」。
	// =====================================================================

	/** 装备加成：攻击力（加算）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_AttackDamage);
	/** 装备加成：法术强度（加算）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_AbilityPower);
	/** 装备加成：最大生命（加算）。⚠️ 只抬上限，不回当前血量 —— 见 ArenaItemStats.h。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_MaxHealth);
	/** 装备加成：护甲（加算）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_Armor);
	/** 装备加成：魔抗（加算）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_MagicResist);
	/** 装备加成：攻速（加算，口径同属性集 BonusAttackSpeedPercent，0.15 = +15%）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_AttackSpeed);
	/** 装备加成：暴击率（加算，0~1）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_CritChance);
	/** 装备加成：技能急速（加算，属性集里是「百分比」不是「冷却缩减率」）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_AbilityHaste);
	/** 装备加成：移速（加算）。改了会立刻过 OnMoveSpeedChanged 同步到 MaxWalkSpeed。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_MoveSpeed);
	/** 装备加成：全能吸血（加算，0~1）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_Omnivamp);
	/** 装备加成：生命偷取（加算，0~1）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_LifeSteal);
	/** 装备加成：固定护甲穿透（加算）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_FlatArmorPen);
	/** 装备加成：百分比护甲穿透（加算，0~1）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_PercentArmorPen);
	/** 装备加成：固定法术穿透（加算）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_FlatMagicPen);
	/** 装备加成：百分比法术穿透（加算，0~1）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_PercentMagicPen);
	/** 装备加成：治疗与护盾强度（加算，0~1）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_HealShieldPower);
	/** 装备加成：生命回复（加算，每 5 秒）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_HealthRegen);
	/** 装备加成：韧性（加算，0~1）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Item_Tenacity);
	
	// =====================================================================
	// 海克斯（Hex）：竞技场里「改规则」的那一类强化
	//
	// 【标签命名的硬约束 —— 这条不是风格问题，是功能问题】
	// UMyAbilitySystemComponent::OnGiveAbility 只认【Ability_Slot_Root 的子标签】
	// （用 Tag.MatchesTag(Ability_Slot_Root) 判），只有这种才会进 SlotAbilityMap、
	// 才会被槽位按键路由到。所以【槽位标签必须写成 Ability.Slot.Xxx】，
	// 写成 "Slot.Ability.Xxx" 的话技能授进来了但按不出来（日志只有一句「槽位未授权」）。
	// =====================================================================

	/**
	 * 海克斯技能槽：按 1 触发的那一个。
	 *
	 * 【为什么单独一个槽、不复用 QWER/D/F】竞技场海克斯是【运行时】叠加的，多个海克斯可能
	 * 都往这一格授予。而 SlotAbilityMap.Add 是静默覆盖 —— 被顶掉的那份 spec 还留在能力列表里、
	 * 从此再也按不出来，且事后没有任何日志能解释（「那个技能突然消失了」）。
	 * 留一个专属于它的槽，被顶掉时至少有上面那条诊断日志能指认是谁顶的。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Hex1);

	/**
	 * 近战新技能（SpinSlash / TurnSlash）的专用槽。
	 *
	 * 【为什么不复用 Hex1】SlotAbilityMap.Add 是静默覆盖 —— 两个海克斯往同一格授予时
	 * 后一个会把前一个顶掉，被顶掉的那份 spec 还留在能力列表里、从此按不出来。
	 * 「上挑击飞」和「回身击退」是两套完全不同的招式，各占一格就不会互相顶。
	 *
	 * 【按键】由输入映射资产（DefaultMappingContexts 里 Entry.SlotTag）绑到具体按键，
	 * 这里只定义槽本身；配哪个键在资产里填，不改代码。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Hex2);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Hex3);

	/**
	 * 形态切换槽（带刀 ⇄ 空手）。见 UGA_FormSwitch。
	 *
	 * 同样必须是 Ability.Slot.* —— 它也是运行时由技能组授予的。
	 * ⚠️ 它【不是】海克斯专属：形态是角色属性，任何模式都能按（见 GAS_FormSwitch_Setup.md）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Form);

	/**
	 * 切换形态中：正在播收刀/拔刀 montage。
	 *
	 * 存在的意义是给「切换期间不许放别的技能」一个判据 —— montage 播到一半被打断的话
	 * 标签会停在「已切一半」的中间态（刀收了一半、姿势差值也过了半程），
	 * 而 ABP 那边权重是自己平滑的，不看这个标签 ⇒ 会平滑到一个不存在的中间形态上。
	 * 消费者：UGA_FormSwitch 自己（挡重入）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Form_Switching);

	/**
	 * 近战新技能（SpinSlash / TurnSlash）的命中帧。
	 *
	 * 挂在蒙太奇上的 UAnimNotify_SendGameplayEvent，判定时刻因此跟着【动画帧】走，
	 * 而不是跟着某个定时器走（改攻速/换蒙太奇时定时器的数字会悄悄错位）。
	 * 理由和 Event.Melee.Impact 完全一样，只是这两个技能各有自己的 montage、不和普攻共用。
	 *
	 * ⚠️⚠️ **每段必须用不同的标签**（所以有两组：SpinSlash / SpinSlash2、TurnSlash / TurnSlash2）。
	 * WaitGameplayEvent 是【按标签】订阅的，同一个标签发两次只会唤醒一次 ——
	 * 两段共用一个标签的症状是「两段动作播完只有第一下有伤害/控制」。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_SpinSlash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_SpinSlash2);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_TurnSlash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_TurnSlash2);

	/**
	 * 空手形态的四连拳命中帧（Combo_01 ~ Combo_04）。
	 *
	 * 动画来自 ParagonCrunch，经 IK Retargeter 重定向到 Kallari 骨架
	 * （Rig_Crunch + RT_Crunch_To_Kallari，19/19 链全自动映射）。
	 * 四招在源资产里就是**等长 0.933s**（Paragon combo 的标准设计），
	 * 所以连招窗口天然对齐，不用额外配时间。
	 *
	 * ⚠️ 每招一个标签，理由和上面 SpinSlash/TurnSlash 那组完全一样：
	 * WaitGameplayEvent 按标签订阅，同一标签发两次只唤醒一次。
	 *
	 * 命中时刻取自 Paragon 原作者在自带 montage 里配的 SaveAttack notify
	 * （Combo_01 = 0.558s / Combo_02 = 0.536s / Combo_03 = 0.582s / Combo_04 = 估算 0.58s）。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_Boxing1);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_Boxing2);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_Boxing3);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Melee_Boxing4);

	// --- 海克斯「增强」标签 ---
	/**
	 * 海克斯：三连飞刃（E 从单发变三发扇形散射）。
	 *
	 * 【必须是 GE 的 TargetTags 组件授的 Granted Tag，不能用 AddLooseGameplayTags】
	 * UE 5.8 的 AddLooseGameplayTag 默认 TagRepState=None ⇒ 完全不复制
	 * （AbilitySystemComponent.cpp:795 只在 >None 时写复制容器），
	 * 而 UAbilitySet::GiveToAbilitySystem 里的 GrantTag 用的就是这个默认值
	 * ⇒ 服务端挂了客户端读不到。本项目的 E/R 两个海克斯都只在服务端读标签
	 * （SpawnProjectile 在 K2_HasAuthority 后面、ApplySpinDamage 只在服务端跑），
	 * 但走 GE 仍然更稳：标签随 active GE 的 FastArray 复制，两端读到的必然一致。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Hex_ThrowDagger_Triple);

	/** 海克斯：死亡收割附带眩晕（只第一跳施加，见 UGA_DeathHarvest::bAugmentStunApplied）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Hex_DeathHarvest_Stun);
}
