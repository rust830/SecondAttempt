// Copyright Epic Games, Inc. All Rights Reserved.


#include "Variant_Combat/Animation/AnimNotify_DoAttackTrace.h"
#include "Variant_Combat/Interfaces/CombatAttacker.h"
#include "Components/SkeletalMeshComponent.h"

void UAnimNotify_DoAttackTrace::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	// cast the owner to the attacker interface
	if (ICombatAttacker* AttackerInterface = Cast<ICombatAttacker>(MeshComp->GetOwner()))
	{
		AttackerInterface->DoAttackTrace(AttackBoneName);
	}
}

FString UAnimNotify_DoAttackTrace::GetNotifyName_Implementation() const
{
	return FString("Do Attack Trace");
}