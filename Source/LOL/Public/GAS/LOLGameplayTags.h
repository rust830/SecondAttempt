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

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_BasicAttack);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_ThrowConfirm);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_Repressed);


	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_Flash);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_ThrowDagger);
	//slot
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Passive);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_Q);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_W);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_E);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_R);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_D);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Slot_F);
	//target
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Hero);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Void);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Target_Terrain);
	//aiming
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Throw_Aiming);
	//gameplay cue
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_ThrowDagger_Hit);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_ThrowDagger_Spawn);
}
