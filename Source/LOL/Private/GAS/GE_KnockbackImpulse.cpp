// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_KnockbackImpulse.h"
#include "GAS/GEComponent_Knockback.h"

UGE_KnockbackImpulse::UGE_KnockbackImpulse(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	// 只挂位移组件：不授标签、不打断技能、不挂 cue、不进 ActiveGameplayEffects。
	// 冲量两个 SetByCaller 由施加方填（没填 = 组件直接返回，什么都不做）。
	UGEComponent_Knockback* KnockbackComp =
		ObjectInitializer.CreateDefaultSubobject<UGEComponent_Knockback>(this, TEXT("KnockbackComponent"));
	GEComponents.Add(KnockbackComp);
}
