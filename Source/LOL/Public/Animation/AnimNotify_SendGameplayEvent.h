// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "GameplayTagContainer.h"
#include "AnimNotify_SendGameplayEvent.generated.h"

/**
 * 把蒙太奇上的一个时刻变成一次 GameplayEvent，推技能相位。
 *
 * 为什么自己写：GAS 插件只有 AnimNotify_GameplayCue 一个 notify，没有发 GameplayEvent 的。
 *
 * ⚠️ 通知会在【每一台播放这条蒙太奇的机器】上触发（服务器 + 本人 + 其他玩家），
 * 不是"只在服务器"。所以 bServerOnly 默认 true：客户端那份蒙太奇触发了也直接返回。
 * 相位推进只认服务器那一次 —— 这正是"服务器独占时间轴"想要的效果。
 */
UCLASS(meta = (DisplayName = "Send Gameplay Event"))
class LOL_API UAnimNotify_SendGameplayEvent : public UAnimNotify
{
	GENERATED_BODY()
public:
	explicit UAnimNotify_SendGameplayEvent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;

	/** 发哪个事件（技能那头用 UAbilityTask_WaitGameplayEvent 同名标签接）。 */
	UPROPERTY(EditAnywhere, Category="GameplayEvent", meta=(GameplayTagFilter="Event"))
	FGameplayTag EventTag;

	/**
	 * true = 只在服务器发（默认，相位推进用）。
	 * false = 各端都发（本地表现类的事件才需要，本项目的相位推进不要用它）。
	 */
	UPROPERTY(EditAnywhere, Category="GameplayEvent")
	bool bServerOnly = true;
};
