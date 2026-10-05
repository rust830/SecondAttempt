// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_Knockback.h"
#include "GAS/HeroCombatCharacter.h"
#include "GAS/LOLGameplayTags.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameplayEffectTypes.h"

AGC_Knockback::AGC_Knockback()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_Knockback;
}

UAnimMontage* AGC_Knockback::PickMontage(AActor* Target, const FGameplayCueParameters& Parameters) const
{
	// 方向来源优先 EffectCauser（和 GC_HitReact 同一套：EffectContext 里 causer 放的是角色本人）。
	// 退回 Instigator 时要小心它多半是 PlayerState、没有世界坐标，先换回它的 Pawn。
	const AActor* Source = Parameters.EffectCauser.Get();
	if (!Source)
	{
		// 局部名避开 Instigator：这个 cue 是 Actor 变体，继承自 AActor，局部变量名 Instigator 会
		// 隐藏 AActor::Instigator 成员（项目把 C4458 当错误）。
		AActor* InstigatorActor = Parameters.Instigator.Get();
		if (const APlayerState* InstigatorPS = Cast<APlayerState>(InstigatorActor))
		{
			Source = InstigatorPS->GetPawn();
		}
		else
		{
			Source = Cast<APawn>(InstigatorActor);
		}
	}

	const EHitDirection Direction = Source
		? AHeroCombatCharacter::ResolveHitDirection(Target, Source->GetActorLocation())
		: EHitDirection::Front;

	// Back = 从背面打来 → 向前扑（BackMontage）。没配 BackMontage 就回退 Montage。
	if (Direction == EHitDirection::Back && BackMontage)
	{
		return BackMontage;
	}
	return Montage;
}
