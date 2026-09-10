// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/GA_ThreeHitPassive.h"
#include "GAS/ThreeHitPassiveData.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Animation/AnimInstance.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"

UGA_ThreeHitPassive::UGA_ThreeHitPassive()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;   // 每 actor 一个技能实例，成员变量能在多次激活间保持
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;  // 客户端预测表现，服务端权威结算
	AttackInputTag = LOLGameplayTags::Event_Input_BasicAttack;
	DamageSetByCallerTag = LOLGameplayTags::Data_Damage;
	ActivationPolicy = EMyAbilityActivationPolicy::OnEvent;   // Passive: event-triggered, not button-triggered
}

void UGA_ThreeHitPassive::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	UE_LOG(LogTemp, Warning, TEXT("[Passive] ActivateAbility 被调用"));

	// 没配数据资产时临时 New 一个兜底（正式使用应在编辑器里赋值）。
	if (!PassiveData)
	{
		PassiveData = NewObject<UThreeHitPassiveData>(this);
	}

	// 必须是 3 段数据，且能通过消耗/冷却检查，否则直接结束。
	if (PassiveData->Stages.Num() != 3 || !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 检查失败（Stages!=3 或 CommitAbility 失败）→ EndAbility"));
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	// 监听下一次按键（用于连段），然后起手第一段。
	InputTask = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, AttackInputTag, nullptr, false, true);
	InputTask->EventReceived.AddDynamic(this, &ThisClass::OnAttackInput);
	InputTask->ReadyForActivation();

	StartStage(0);
}

void UGA_ThreeHitPassive::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	// 清掉所有定时器。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(HitTimer);
		World->GetTimerManager().ClearTimer(OpenTimer);
		World->GetTimerManager().ClearTimer(CloseTimer);
	}

	// 停掉输入监听任务。
	if (InputTask)
	{
		InputTask->EndTask();
		InputTask = nullptr;
	}

	// 重置连段状态。
	StageIndex = INDEX_NONE;
	bWindowOpen = false;
	bQueuedNextStage = false;
	bEmpowerNextStage = false;
	bCurrentStageKnocksBack = false;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_ThreeHitPassive::OnAttackInput(FGameplayEventData Payload)
{
	// 只有连段窗口开着时，这次按键才算「排队下一段」。
	if (bWindowOpen)
	{
		bQueuedNextStage = true;
	}
}

float UGA_ThreeHitPassive::GetAttackPlayRate() const
{
	// 最终攻速 / 参考攻速 = 播放倍率，钳制在 0.1x ~ 4x，避免动画过快/过慢。
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	const UHeroCombatAttributeSet* Attributes = ASC ? ASC->GetSet<UHeroCombatAttributeSet>() : nullptr;
	if (!Attributes)
	{
		return 1.f;
	}
	return FMath::Clamp(Attributes->GetFinalAttackSpeed() / PassiveData->ReferenceAttackSpeed, 0.1f, 4.f);
}

void UGA_ThreeHitPassive::StartStage(int32 NewStage)
{
	StageIndex = NewStage;
	bWindowOpen = false;
	bQueuedNextStage = false;

	// 上一段若触发了「完美窗口」，本段带击退。
	bCurrentStageKnocksBack = bEmpowerNextStage;
	bEmpowerNextStage = false;

	const FThreeHitAttackStage& Stage = PassiveData->Stages[StageIndex];
	const float Rate = GetAttackPlayRate();

	// 播放本段蒙太奇（按攻速倍率变速）。
	if (Stage.Montage)
	{
		if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
		{
			if (UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance())
			{
				Anim->Montage_Play(Stage.Montage, Rate);
			}
		}
	}

	// 三个定时器，时长都除以攻速倍率（攻速越快，动画和判定都越快）：
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	World->GetTimerManager().SetTimer(HitTimer, this, &ThisClass::ConfirmHit, Stage.HitTime / Rate, false);                   // 命中结算
	World->GetTimerManager().SetTimer(OpenTimer, this, &ThisClass::OpenChainWindow, Stage.ChainWindowOpenTime / Rate, false);   // 连段窗口开启
	World->GetTimerManager().SetTimer(CloseTimer, this, &ThisClass::CloseChainWindow, Stage.ChainWindowCloseTime / Rate, false); // 连段窗口关闭
}

void UGA_ThreeHitPassive::OpenChainWindow()
{
	bWindowOpen = true;
}

void UGA_ThreeHitPassive::CloseChainWindow()
{
	bWindowOpen = false;

	// 没排队下一段，或已经打满最后一段 → 结束技能。
	if (!bQueuedNextStage || StageIndex >= PassiveData->Stages.Num() - 1)
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
		return;
	}

	// 这一段若带「完美窗口」标记，则下一段击退。
	const FThreeHitAttackStage& Stage = PassiveData->Stages[StageIndex];
	if (Stage.bPerfectWindowEnablesNextHitKnockback)
	{
		bEmpowerNextStage = true;
	}

	StartStage(StageIndex + 1);
}

void UGA_ThreeHitPassive::ConfirmHit()
{
	// 只有服务端权威才结算伤害（客户端只负责预测表现）。
	if (GetAvatarActorFromActorInfo()->HasAuthority())
	{
		ApplyServerHit(PassiveData->Stages[StageIndex], bCurrentStageKnocksBack);
	}
}

void UGA_ThreeHitPassive::ApplyServerHit(const FThreeHitAttackStage& Stage, bool bKnockback)
{
	AActor* Source = GetAvatarActorFromActorInfo();
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (!Source || !SourceASC)
	{
		return;
	}

	// 1) 球形扫描：从角色位置沿朝向前扫 TraceDistance 距离、半径 TraceRadius。
	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ThreeHitBasicAttack), false, Source);
	FCollisionShape Shape = FCollisionShape::MakeSphere(PassiveData->TraceRadius);
	const FVector Start = Source->GetActorLocation();
	const FVector End = Start + Source->GetActorForwardVector() * PassiveData->TraceDistance;
	if (!GetWorld()->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, ECC_Pawn, Shape, Params))
	{
		return;
	}

	// 2) 算伤害：攻击力 × 本段倍率。
	const UHeroCombatAttributeSet* Attributes = SourceASC->GetSet<UHeroCombatAttributeSet>();
	const float Damage = (Attributes ? Attributes->GetAttackDamage() : 0.f) * Stage.DamageMultiplier;

	// 3) 对每个命中的目标施加伤害。
	for (const FHitResult& Hit : Hits)
	{
		AActor* Target = Hit.GetActor();
		if (!Target)
		{
			continue;
		}

		UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);

		// 兼容旧的非 GAS 目标：没有 ASC 或没配伤害 GE 时，退回传统 ApplyPointDamage。
		if (!TargetASC || !PassiveData->DamageEffect)
		{
			UGameplayStatics::ApplyPointDamage(Target, Damage, Source->GetActorForwardVector(), Hit, Source->GetInstigatorController(), Source, nullptr);
			if (bKnockback)
			{
				if (ACharacter* TargetCharacter = Cast<ACharacter>(Target))
				{
					TargetCharacter->LaunchCharacter(Source->GetActorForwardVector() * PassiveData->Stages[1].NextHitKnockback + FVector::UpVector * PassiveData->Stages[1].NextHitLaunch, true, true);
				}
			}
			continue;
		}

		// 标准 GAS 路径：造一个伤害 GE 的 Spec，用 SetByCaller 填伤害值，施加到目标 ASC。
		FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
		Context.AddHitResult(Hit);
		FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(PassiveData->DamageEffect, GetAbilityLevel(), Context);
		if (Spec.IsValid())
		{
			Spec.Data->SetSetByCallerMagnitude(DamageSetByCallerTag, Damage);
			SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
		}

		// 需要击退时，给目标一个冲量。
		if (bKnockback)
		{
			if (ACharacter* TargetCharacter = Cast<ACharacter>(Target))
			{
				TargetCharacter->LaunchCharacter(Source->GetActorForwardVector() * PassiveData->Stages[1].NextHitKnockback + FVector::UpVector * PassiveData->Stages[1].NextHitLaunch, true, true);
			}
		}
	}
}
