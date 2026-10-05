// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/LOLGameplayTags.h"

namespace LOLGameplayTags
{
	UE_DEFINE_GAMEPLAY_TAG(Data_Damage, "Data.Damage");
	UE_DEFINE_GAMEPLAY_TAG(Data_Cooldown, "Data.Cooldown");
	UE_DEFINE_GAMEPLAY_TAG(Data_Cost, "Data.Cost");
	UE_DEFINE_GAMEPLAY_TAG(Data_StealthDuration, "Data.StealthDuration");
	UE_DEFINE_GAMEPLAY_TAG(Data_EmpowerDuration, "Data.EmpowerDuration");
	UE_DEFINE_GAMEPLAY_TAG(Data_DamageMultiplier, "Data.DamageMultiplier");
	UE_DEFINE_GAMEPLAY_TAG(Data_FlatDamage, "Data.FlatDamage");
	UE_DEFINE_GAMEPLAY_TAG(Data_CanCrit, "Data.CanCrit");
	UE_DEFINE_GAMEPLAY_TAG(Data_BlockWindow, "Data.BlockWindow");
	UE_DEFINE_GAMEPLAY_TAG(Data_BlockImmuneDuration, "Data.BlockImmuneDuration");
	UE_DEFINE_GAMEPLAY_TAG(Data_DodgeWindow, "Data.DodgeWindow");
	UE_DEFINE_GAMEPLAY_TAG(Data_EnergyRestore, "Data.EnergyRestore");
	UE_DEFINE_GAMEPLAY_TAG(Damage_Physical, "Damage.Physical");
	UE_DEFINE_GAMEPLAY_TAG(Damage_Magic, "Damage.Magic");
	UE_DEFINE_GAMEPLAY_TAG(Damage_True, "Damage.True");
	UE_DEFINE_GAMEPLAY_TAG(Event_Input_BasicAttack, "Event.Input.BasicAttack");
	UE_DEFINE_GAMEPLAY_TAG(Event_Input_ComboAttack, "Event.Input.ComboAttack");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_Flash, "State.Cooldown.Flash");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_ThrowDagger, "State.Cooldown.ThrowDagger");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_Stealth, "State.Cooldown.Stealth");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_Block, "State.Cooldown.Block");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_Dodge, "State.Cooldown.Dodge");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_GroundDodge, "State.Cooldown.GroundDodge");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_SpinSlash, "State.Cooldown.SpinSlash");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_TurnSlash, "State.Cooldown.TurnSlash");
	// 被动的冷却标签。目前没有能力在用 —— 预留，见 .h 里的说明。
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_Passive, "State.Cooldown.Passive");
	UE_DEFINE_GAMEPLAY_TAG(State_Blocking, "State.Blocking");
	UE_DEFINE_GAMEPLAY_TAG(State_BlockImmune, "State.BlockImmune");
	UE_DEFINE_GAMEPLAY_TAG(State_Stealth, "State.Stealth");
	UE_DEFINE_GAMEPLAY_TAG(State_EmpoweredAttack, "State.EmpoweredAttack");
	// ============ 槽位（Ability.Slot.*） ============
	// ⚠️ 槽位标签必须是 Ability_Slot_Root 的【子标签或它本身】——
	//    UMyAbilitySystemComponent::OnGiveAbility / OnRemoveAbility 用
	//    Tag.MatchesTag(Ability_Slot_Root) 判，写成 "Slot.Ability.Xxx" 会静默失效
	//    （能力按不出去，按键毫无反应，且没有任何日志）。
	//    父标签本身不参与路由（没有能力会用它当槽位），它只是这一族的锚点。
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Root, "Ability.Slot");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Passive, "Ability.Slot.Passive");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Q, "Ability.Slot.Q");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_W, "Ability.Slot.W");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_E, "Ability.Slot.E");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_R, "Ability.Slot.R");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_D, "Ability.Slot.D");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_F, "Ability.Slot.F");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Block, "Ability.Slot.Block");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_GroundDodge, "Ability.Slot.GroundDodge");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Combo, "Ability.Slot.Combo");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_AirAttack, "Ability.Slot.AirAttack");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Type_Normal, "Ability.Type.Normal");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Type_Ultimate, "Ability.Type.Ultimate");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Type_Summoner, "Ability.Type.Summoner");
	UE_DEFINE_GAMEPLAY_TAG(Target_Hero, "Target.Hero");
	UE_DEFINE_GAMEPLAY_TAG(Target_Void, "Target.Void");
	UE_DEFINE_GAMEPLAY_TAG(Target_Terrain, "Target.Terrain");
	UE_DEFINE_GAMEPLAY_TAG(State_Throw_Aiming, "State.Throw.Aiming");
	UE_DEFINE_GAMEPLAY_TAG(State_Dodge_Active, "State.Dodge.Active");
	UE_DEFINE_GAMEPLAY_TAG(State_Dodge_Window, "State.Dodge.Window");
	UE_DEFINE_GAMEPLAY_TAG(State_ComboWindow, "State.ComboWindow");
	UE_DEFINE_GAMEPLAY_TAG(State_PerfectWindowArmed, "State.PerfectWindowArmed");
	UE_DEFINE_GAMEPLAY_TAG(State_PerfectWindow, "State.PerfectWindow");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Flash, "GameplayCue.Flash");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Stealth, "GameplayCue.Stealth");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_ThrowAiming, "GameplayCue.ThrowAiming");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_ThrowDagger_Hit, "GameplayCue.ThrowDagger.Hit");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_ThrowDagger_Spawn, "GameplayCue.ThrowDagger.Spawn");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_EmpoweredAttack, "GameplayCue.EmpoweredAttack");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_EmpoweredHit, "GameplayCue.EmpoweredHit");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Block, "GameplayCue.Block");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_HitReact, "GameplayCue.HitReact");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_MeleeHit, "GameplayCue.MeleeHit");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Stun, "GameplayCue.Stun");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Knockback, "GameplayCue.Knockback");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_KnockUp, "GameplayCue.KnockUp");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_Dodge, "GameplayCue.Dodge");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DodgeKick, "GameplayCue.DodgeKick");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DodgeKickHit, "GameplayCue.DodgeKickHit");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DodgePunchHit, "GameplayCue.DodgePunchHit");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DodgeKickWhiff, "GameplayCue.DodgeKickWhiff");
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_Impact, "Event.Melee.Impact");
	// 音效事件标签：只当 UHeroAudioConfig 的键用，见 LOLGameplayTags.h 里那段说明。
	UE_DEFINE_GAMEPLAY_TAG(Audio_MeleeHit, "Audio.MeleeHit");
	UE_DEFINE_GAMEPLAY_TAG(Audio_MeleeHitBoxing, "Audio.MeleeHit.Boxing");
	UE_DEFINE_GAMEPLAY_TAG(Audio_PassiveAttackBoxing, "Audio.PassiveAttack.Boxing");
	UE_DEFINE_GAMEPLAY_TAG(Audio_ThrowDaggerHit, "Audio.ThrowDaggerHit");
	UE_DEFINE_GAMEPLAY_TAG(Audio_ThrowDaggerThrow, "Audio.ThrowDaggerThrow");
	UE_DEFINE_GAMEPLAY_TAG(Audio_ThrowAiming, "Audio.ThrowAiming");
	UE_DEFINE_GAMEPLAY_TAG(Audio_Dodge, "Audio.Dodge");
	UE_DEFINE_GAMEPLAY_TAG(Audio_DodgeKickHit, "Audio.DodgeKickHit");
	UE_DEFINE_GAMEPLAY_TAG(Audio_DodgePunchHit, "Audio.DodgePunchHit");
	UE_DEFINE_GAMEPLAY_TAG(Audio_Block, "Audio.Block");
	UE_DEFINE_GAMEPLAY_TAG(Audio_StealthEnter, "Audio.StealthEnter");
	UE_DEFINE_GAMEPLAY_TAG(Audio_StealthExit, "Audio.StealthExit");
	UE_DEFINE_GAMEPLAY_TAG(Audio_Flash, "Audio.Flash");
	UE_DEFINE_GAMEPLAY_TAG(Audio_EmpoweredAttackEnter, "Audio.EmpoweredAttackEnter");
	UE_DEFINE_GAMEPLAY_TAG(Audio_EmpoweredAttackExit, "Audio.EmpoweredAttackExit");
	UE_DEFINE_GAMEPLAY_TAG(Audio_EmpoweredHit, "Audio.EmpoweredHit");
	UE_DEFINE_GAMEPLAY_TAG(Audio_PerfectWindow, "Audio.PerfectWindow");
	UE_DEFINE_GAMEPLAY_TAG(Audio_PassiveAttack, "Audio.PassiveAttack");
	UE_DEFINE_GAMEPLAY_TAG(Audio_PassiveEmpoweredAttack, "Audio.PassiveEmpoweredAttack");
	UE_DEFINE_GAMEPLAY_TAG(Audio_DeathHarvestVanish, "Audio.DeathHarvest.Vanish");
	UE_DEFINE_GAMEPLAY_TAG(Audio_DeathHarvestAppear, "Audio.DeathHarvest.Appear");
	UE_DEFINE_GAMEPLAY_TAG(Audio_DeathHarvestPortalOpen, "Audio.DeathHarvest.PortalOpen");
	UE_DEFINE_GAMEPLAY_TAG(Audio_DeathHarvestPortalClose, "Audio.DeathHarvest.PortalClose");
	UE_DEFINE_GAMEPLAY_TAG(Audio_DeathHarvestSpin, "Audio.DeathHarvest.Spin");
	UE_DEFINE_GAMEPLAY_TAG(Audio_DeathHarvestBurst, "Audio.DeathHarvest.Burst");
	UE_DEFINE_GAMEPLAY_TAG(Event_Input_ThrowConfirm, "Event.Input.ThrowConfirm");
	UE_DEFINE_GAMEPLAY_TAG(Event_Input_Repressed, "Event.Input.Repressed");
	UE_DEFINE_GAMEPLAY_TAG(Event_Input_DodgeKick, "Event.Input.DodgeKick");
	UE_DEFINE_GAMEPLAY_TAG(Event_Input_DodgeEvade, "Event.Input.DodgeEvade");
	UE_DEFINE_GAMEPLAY_TAG(State_DeathHarvest_Casting, "State.DeathHarvest.Casting");
	UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_DeathHarvest, "State.Cooldown.DeathHarvest");
	UE_DEFINE_GAMEPLAY_TAG(State_DeathHarvest_Selecting, "State.DeathHarvest.Selecting");
	UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_CastAt, "Event.DeathHarvest.CastAt");
	UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_Portal, "Event.DeathHarvest.Portal");
	UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_Teleport, "Event.DeathHarvest.Teleport");
	UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_SpinStart, "Event.DeathHarvest.SpinStart");
	UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_SpinEnd, "Event.DeathHarvest.SpinEnd");
	UE_DEFINE_GAMEPLAY_TAG(Data_MissingHealthBonus, "Data.MissingHealthBonus");
	UE_DEFINE_GAMEPLAY_TAG(State_Dead, "State.Dead");
	UE_DEFINE_GAMEPLAY_TAG(State_Invulnerable, "State.Invulnerable");
	UE_DEFINE_GAMEPLAY_TAG(State_Stunned, "State.Stunned");
	UE_DEFINE_GAMEPLAY_TAG(State_Silenced, "State.Silenced");
	UE_DEFINE_GAMEPLAY_TAG(State_Knockback, "State.Knockback");
	UE_DEFINE_GAMEPLAY_TAG(State_KnockUp, "State.KnockUp");
	UE_DEFINE_GAMEPLAY_TAG(State_Form_Unarmed, "State.Form.Unarmed");
	UE_DEFINE_GAMEPLAY_TAG(Data_RespawnDelay, "Data.RespawnDelay");
	UE_DEFINE_GAMEPLAY_TAG(Data_ControlDuration, "Data.ControlDuration");
	UE_DEFINE_GAMEPLAY_TAG(Data_KnockbackDuration, "Data.KnockbackDuration");
	UE_DEFINE_GAMEPLAY_TAG(Data_KnockbackImpulse, "Data.KnockbackImpulse");
	UE_DEFINE_GAMEPLAY_TAG(Data_KnockbackLaunch, "Data.KnockbackLaunch");
	UE_DEFINE_GAMEPLAY_TAG(Data_KnockUpDuration, "Data.KnockUpDuration");
	UE_DEFINE_GAMEPLAY_TAG(Data_Lethal, "Data.Lethal");
	UE_DEFINE_GAMEPLAY_TAG(State_Slowed, "State.Slowed");
	UE_DEFINE_GAMEPLAY_TAG(Data_SlowDuration, "Data.SlowDuration");
	UE_DEFINE_GAMEPLAY_TAG(Data_SlowMultiplier, "Data.SlowMultiplier");
	UE_DEFINE_GAMEPLAY_TAG(Anim_Attack, "Anim.Attack");
	UE_DEFINE_GAMEPLAY_TAG(Anim_EmpoweredAttack, "Anim.EmpoweredAttack");
	UE_DEFINE_GAMEPLAY_TAG(Anim_Block, "Anim.Block");
	UE_DEFINE_GAMEPLAY_TAG(Anim_Stealth, "Anim.Stealth");
	UE_DEFINE_GAMEPLAY_TAG(Anim_Flash, "Anim.Flash");
	UE_DEFINE_GAMEPLAY_TAG(Anim_ThrowAiming, "Anim.ThrowAiming");
	UE_DEFINE_GAMEPLAY_TAG(Anim_DeathHarvest, "Anim.DeathHarvest");
	UE_DEFINE_GAMEPLAY_TAG(Anim_HitReact, "Anim.HitReact");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Cast, "GameplayCue.DeathHarvest.Cast");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_TeleportOut, "GameplayCue.DeathHarvest.TeleportOut");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Portal, "GameplayCue.DeathHarvest.Portal");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Appear, "GameplayCue.DeathHarvest.Appear");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Spin, "GameplayCue.DeathHarvest.Spin");
	UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Hit, "GameplayCue.DeathHarvest.Hit");

	// 斗魂竞技场：装备 / 海克斯的 SetByCaller 键（对应表见 ArenaItemStats.h）。
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_AttackDamage, "Data.Item.AttackDamage");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_AbilityPower, "Data.Item.AbilityPower");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_MaxHealth, "Data.Item.MaxHealth");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_Armor, "Data.Item.Armor");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_MagicResist, "Data.Item.MagicResist");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_AttackSpeed, "Data.Item.AttackSpeed");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_CritChance, "Data.Item.CritChance");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_AbilityHaste, "Data.Item.AbilityHaste");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_MoveSpeed, "Data.Item.MoveSpeed");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_Omnivamp, "Data.Item.Omnivamp");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_LifeSteal, "Data.Item.LifeSteal");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_FlatArmorPen, "Data.Item.FlatArmorPen");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_PercentArmorPen, "Data.Item.PercentArmorPen");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_FlatMagicPen, "Data.Item.FlatMagicPen");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_PercentMagicPen, "Data.Item.PercentMagicPen");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_HealShieldPower, "Data.Item.HealShieldPower");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_HealthRegen, "Data.Item.HealthRegen");
	UE_DEFINE_GAMEPLAY_TAG(Data_Item_Tenacity, "Data.Item.Tenacity");

	// 吸血 / 回血链路（消费者见 UExecCalc_Damage ⑤）。
	UE_DEFINE_GAMEPLAY_TAG(Data_BasicAttack, "Data.BasicAttack");
	UE_DEFINE_GAMEPLAY_TAG(Data_Heal, "Data.Heal");

	// ================= 海克斯（Hex） =================
	// ⚠️ 槽位标签必须是 Ability_Slot_Root 的子标签（见文件开头那一段的完整说明）。
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Hex1, "Ability.Slot.Hex1");
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Hex2, "Ability.Slot.Hex2");   // SpinSlash（上挑击飞）
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Hex3, "Ability.Slot.Hex3");   // TurnSlash（回身击退）
	UE_DEFINE_GAMEPLAY_TAG(Ability_Slot_Form, "Ability.Slot.Form");
	UE_DEFINE_GAMEPLAY_TAG(State_Form_Switching, "State.Form.Switching");

	// 近战新技能的命中帧（挂在蒙太奇 notify 上，理由同 Event.Melee.Impact）。
	// ★ 每段一个标签：WaitGameplayEvent 按标签订阅，同一标签发两次只唤醒一次
	//   ⇒ 两段共用一个标签的症状是「两段动作播完只有第一下有伤害」。
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_SpinSlash, "Event.Melee.SpinSlash");
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_SpinSlash2, "Event.Melee.SpinSlash2");
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_TurnSlash, "Event.Melee.TurnSlash");
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_TurnSlash2, "Event.Melee.TurnSlash2");

	// 空手四连拳（ParagonCrunch 重定向而来，四招等长 0.933s）。每招一个标签，理由同上。
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_Boxing1, "Event.Melee.Boxing1");
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_Boxing2, "Event.Melee.Boxing2");
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_Boxing3, "Event.Melee.Boxing3");
	UE_DEFINE_GAMEPLAY_TAG(Event_Melee_Boxing4, "Event.Melee.Boxing4");

	// 海克斯「增强」标签（由 GE 的 TargetTags 组件授予 —— loose tag 在 5.8 不复制）。
	UE_DEFINE_GAMEPLAY_TAG(Hex_ThrowDagger_Triple, "Hex.ThrowDagger.Triple");
	UE_DEFINE_GAMEPLAY_TAG(Hex_DeathHarvest_Stun, "Hex.DeathHarvest.Stun");
}
