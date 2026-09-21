// Fill out your copyright notice in the Description page of Project Settings.

#include "Animation/AnimNotify_SendGameplayEvent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

UAnimNotify_SendGameplayEvent::UAnimNotify_SendGameplayEvent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UAnimNotify_SendGameplayEvent::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (!MeshComp || !EventTag.IsValid())
	{
		// 标签没配 = 这个相位永远不会推进，动画上完全看不出来（只是技能卡在那儿）。
		UE_LOG(LogTemp, Warning, TEXT("[SendGameplayEvent] 通知没配 EventTag（动画 %s）→ 这次通知什么都不做"),
			*GetNameSafe(Animation));
		return;
	}

	AActor* Owner = MeshComp->GetOwner();
	if (!Owner)
	{
		return;
	}

	// 目的端过滤。客户端的蒙太奇也会触发本通知，那一次是空操作（技能实例在客户端只管播动画）。
	if (bServerOnly && !Owner->HasAuthority())
	{
		return;
	}

	FGameplayEventData Payload;
	Payload.EventTag = EventTag;
	Payload.Instigator = Owner;
	Payload.Target = Owner;
	Payload.EventMagnitude = 0.f;

	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Owner, EventTag, Payload);
}

FString UAnimNotify_SendGameplayEvent::GetNotifyName_Implementation() const
{
	return EventTag.IsValid() ? FString::Printf(TEXT("SendEvent: %s"), *EventTag.ToString()) : FString(TEXT("SendEvent (未配标签)"));
}
