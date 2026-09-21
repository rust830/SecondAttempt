// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GA_Block.h"
#include "GAS/GE_Blocking.h"
#include "GAS/GE_BlockCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffect.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Character.h"
#include "Animation/AnimMontage.h"

UGA_Block::UGA_Block()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;   // 和 Flash/Stealth 一致
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	// 沉默挡法术：被沉默时举不起盾。死亡/眩晕在基类已经挡了，别重复加。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Silenced);

	// 默认值就是 true：格挡算「施放技能」→ 会破隐（由 ASC 的输入路由处理，能力不用管）。
	// bBreaksStealthOnCast = true;

	BlockingGE = UGE_Blocking::StaticClass();
	CooldownDuration = 6.f;   // 测试值，BP 子类里按需调
	CooldownGameplayEffectClass = UGE_BlockCooldown::StaticClass();
}

void UGA_Block::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!ASC || !BlockingGE)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Block] ASC(%d) 或 BlockingGE(%d) 缺失 → 没开窗口"),
			ASC ? 1 : 0, BlockingGE ? 1 : 0);
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (BlockWindow <= 0.f)
	{
		// 时长为 0 的 GE 会立刻过期，客户端那份预测副本可能根本来不及让 State.Blocking 生效 →
		// 「按了没反应」而且没有任何日志。宁可报出来。
		UE_LOG(LogTemp, Warning, TEXT("[Block] BlockWindow=%.2f → 窗口时长为 0，这个格挡按不出来（BP 子类里没填？）"), BlockWindow);
	}

	// 开窗：时长由 SetByCaller 填，GE 授予 State.Blocking。
	// 两端各自施加、时长完全一致；联网宽限不在这里，在 UBlockComponent 的判定层（见 BlockComponent.h）。
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(BlockingGE, GetAbilityLevel());
	if (!Spec.IsValid())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_BlockWindow, BlockWindow);
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);

	// 施法动作：跟着能力的预测时机在本地立刻播（理由同 GA_Stealth）。
	if (BlockMontage)
	{
		if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
		{
			Character->PlayAnimMontage(BlockMontage);
		}
	}

	// 能力立刻结束：窗口的状态全在 GE 上，能力不需要活到窗口关闭。
	EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}
