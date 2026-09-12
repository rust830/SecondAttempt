// Fill out your copyright notice in the Description page of Project Settings.


#include "ThrowDaggerAbility.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/GE_ThrowDaggerCooldown.h"
#include "AbilitySystemComponent.h"
#include "ThrowDaggerProjectile.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Particles/ParticleSystemComponent.h"
#include "Kismet/GameplayStatics.h"

UThrowDaggerAbility::UThrowDaggerAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor; // 瞄准态是有实例的中间态
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	CooldownDuration = 6.f; // 测试值
	CooldownGameplayEffectClass = UGE_ThrowDaggerCooldown::StaticClass();

	//dagger FX
	AimingReticleFXComponent = CreateDefaultSubobject<UParticleSystemComponent>(TEXT("AimingReticleFXComponent"));
	AActor* Avator = GetAvatarActorFromActorInfo();
	if (Avator) {
		ACharacter* Character = Cast<ACharacter>(Avator);
		if (Character) {
			AimingReticleFXComponent->SetupAttachment(Character->GetMesh(), TEXT("Muzzle_01"));
		}
	}
	AimingReticleFXComponent->bAutoActivate = false;

}

void UThrowDaggerAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{	
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
	// 只检查不消耗：CD/蓝在真正投掷（OnThrowPressed）时才 commit
	if (!CommitCheck(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	bThrowCommitted = false;

	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 进入瞄准态"));
	EnterAimingState();

	auto* WaitConfirm = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(
		this, LOLGameplayTags::Event_Input_ThrowConfirm, nullptr, true, true);
	WaitConfirm->EventReceived.AddDynamic(this, &UThrowDaggerAbility::OnThrowPressed);
	WaitConfirm->ReadyForActivation();

	auto* WaitCancel = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(
		this, LOLGameplayTags::Event_Input_Repressed, nullptr, true, true);
	WaitCancel->EventReceived.AddDynamic(this, &UThrowDaggerAbility::OnCancelPressed);
	WaitCancel->ReadyForActivation();
}

void UThrowDaggerAbility::CancelAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateCancelAbility)
{
	// 外部取消（被控制/死亡/其它能力打断）走这里。清理统一在 EndAbility 做，这里只转调 Super。
	Super::CancelAbility(Handle, ActorInfo, ActivationInfo, bReplicateCancelAbility);
}

void UThrowDaggerAbility::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool WasCanceled)
{
	ExitAimingState();
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, WasCanceled);
}

void UThrowDaggerAbility::OnThrowPressed(FGameplayEventData Payload)
{
	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 左键确认 → 投掷"));

	// 真正投掷才 commit（消耗 CD + 蓝）
	if (!CommitAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo))
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	// 纯看鼠标方向（相机朝向的水平分量），与人物脸朝向无关。
	// 在确认这一刻锁定，投掷延迟期间鼠标再动也不影响本次出手。
	ThrowDirection = GetAimingDirection();

	// 投出瞬间把人物转向瞄准方向：让手部 socket（Melee_Impact_R）与投掷方向对齐，
	// 否则匕首会从「侧身/背对」的手里朝相机方向飞，看起来脱节。
	StartTurnToFace(ThrowDirection);

	// 确认后锁定准星（出手方向已定），不再跟鼠标刷新。
	// 跳 Cast 段播放；真正出手延迟到动画出手帧（CastThrowDelay），从手部 socket 射出。
	// 能力本身延迟到 Cast 播完（OnThrowMontageFinished）再 End。
	if (MontageTask)
	{
		bThrowCommitted = true;
		MontageJumpToSection(FName("Cast"));
		GetWorld()->GetTimerManager().SetTimer(ThrowDelayTimer, this,
			&UThrowDaggerAbility::FireDagger, CastThrowDelay, false);
	}
	else
	{
		// 没配 Montage：没有动画可等，立即出手后直接收尾
		FireDagger();
	}
}

void UThrowDaggerAbility::OnCancelPressed(FGameplayEventData Payload)
{
	// 只对本槽位（E）的再按响应；不 commit → 无损取消
	if (!Payload.TargetTags.HasTag(LOLGameplayTags::Ability_Slot_E)) return;

	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] E 再按 → 取消（不耗 CD）"));
	if (MontageTask) MontageJumpToSection(FName("Cancel"));
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

void UThrowDaggerAbility::OnThrowMontageFinished()
{
	// 只有左键确认过（跳到了 Cast）才收尾；Targeting 循环段触发的回调在这里被忽略。
	if (!bThrowCommitted) return;
	bThrowCommitted = false;   // 防止 OnBlendOut 与 OnCompleted 重复触发

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UThrowDaggerAbility::EnterAimingState()
{
	// 直接授 State.Throw.Aiming（loose tag）：左键分流、普攻阻断都靠它。
	// 不依赖蓝图 GE 是否配了 tag，保证必生效。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->AddLooseGameplayTag(LOLGameplayTags::State_Throw_Aiming);
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 已授 State.Throw.Aiming（loose tag）"));
	}

	// 瞄准 GE 做额外效果
	if (AimingGE)
	{
		GEHandle = ApplyGameplayEffectToOwner(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
			AimingGE.GetDefaultObject(), GetAbilityLevel());
	}

	if (ThrowDaggerMontage)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 播放 Montage: %s"), *GetNameSafe(ThrowDaggerMontage));

		// 找 section：优先 "Targeting"，找不到就整段播（section 名对不上会导致整段不播）
		FName StartSection = NAME_None;
		FString SectionList;
		for (const FCompositeSection& Sec : ThrowDaggerMontage->CompositeSections)
		{
			SectionList += Sec.SectionName.ToString() + TEXT(" ");
			if (Sec.SectionName == FName("Targeting")) StartSection = FName("Targeting");
		}
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] Montage sections: [%s]"), *SectionList);

		UAnimInstance* AnimInstance = CurrentActorInfo ? CurrentActorInfo->GetAnimInstance() : nullptr;
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] AnimInstance: %s"),
			AnimInstance ? *GetNameSafe(AnimInstance) : TEXT("NULL（角色没配 AnimBP / 没 AnimInstance）"));

		MontageTask = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
			this, NAME_None, ThrowDaggerMontage, 1.f, StartSection);
		// 绑定收尾回调：Targeting 是循环段不会触发；跳到 Cast 播完才触发，此时才 End，保证 cast 动画完整。
		MontageTask->OnCompleted.AddDynamic(this, &UThrowDaggerAbility::OnThrowMontageFinished);
		MontageTask->OnBlendOut.AddDynamic(this, &UThrowDaggerAbility::OnThrowMontageFinished);
		MontageTask->OnInterrupted.AddDynamic(this, &UThrowDaggerAbility::OnThrowMontageFinished);
		MontageTask->OnCancelled.AddDynamic(this, &UThrowDaggerAbility::OnThrowMontageFinished);
		MontageTask->ReadyForActivation();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] ThrowDaggerMontage 为空！BP_GA_ThrowDagger 里没配"));
	}

	if (AimingReticleFXComponent)
	{
		if (!AimingReticleFXComponent->Template && AimingReticleFX) {
			AimingReticleFXComponent->SetTemplate(AimingReticleFX);
		}
		AimingReticleFXComponent->Activate(true);
	}
}

void UThrowDaggerAbility::ExitAimingState()
{
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_Throw_Aiming);
	}
	if (AimingReticleFXComponent) {
		AimingReticleFXComponent->Deactivate();
	}
	if (GEHandle.IsValid())
	{
		BP_RemoveGameplayEffectFromOwnerWithHandle(GEHandle);
		GEHandle.Invalidate();
	}
	if (TurnTimer.IsValid())
	{
		GetWorld()->GetTimerManager().ClearTimer(TurnTimer);
	}
	if (ThrowDelayTimer.IsValid())
	{
		GetWorld()->GetTimerManager().ClearTimer(ThrowDelayTimer);
	}
	RestoreOrientRotationToMovement();   // 唯一恢复点：能力结束才恢复自动朝向，保证整个出手过程只朝瞄准方向转一次
	if (AimReticleComponent)
	{
		AimReticleComponent->DestroyComponent();
		AimReticleComponent = nullptr;
	}
}

FVector UThrowDaggerAbility::GetAimingDirection() const
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return FVector::ForwardVector;

	const AController* Controller = Character->GetController();
	if (!Controller) { UE_LOG(LogTemp, Warning, TEXT("!Controller"));return Character->GetActorForwardVector(); }

	// 第三人称：朝「相机朝向」的水平分量投掷（只用 Yaw，忽略俯仰，和 GA_Flash 一致）。
	// 人物 mesh 是 bOrientRotationToMovement，会随移动转向（甚至正对相机），
	// 所以不能用 GetActorForwardVector()；取控制旋转 Yaw 才是玩家看到的「鼠标方向」。
	const FRotator ControlRotation = Controller->GetControlRotation();
	return FRotationMatrix(FRotator(0.f, ControlRotation.Yaw, 0.f)).GetUnitAxis(EAxis::X);
}

void UThrowDaggerAbility::SpawnProjectile(const FVector& AimDir)
{
	if (!Dagger) return;

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	// 出生点 = 手部 socket（Melee_Impact_R），匕首从手里飞出而不是角色中心凭空出现。
	const FVector SpawnLoc = GetThrowSocketLocation(Character);
	const FRotator SpawnRot = AimDir.Rotation();

	AThrowDaggerProjectile* Proj = GetWorld()->SpawnActorDeferred<AThrowDaggerProjectile>(
		Dagger, FTransform(SpawnRot, SpawnLoc));
	if (Proj)
	{
		Proj->Initialize(AimDir, ThrowSpeed, DamageGE, BaseDamage, Character);
		Proj->FinishSpawning(FTransform(SpawnRot, SpawnLoc));
	}
}

FVector UThrowDaggerAbility::GetThrowSocketLocation(ACharacter* Character) const
{
	if (Character)
	{
		if (const USkeletalMeshComponent* Mesh = Character->GetMesh())
		{
			if (Mesh->DoesSocketExist(ThrowSocketName))
			{
				return Mesh->GetSocketLocation(ThrowSocketName);
			}
			UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] socket [%s] 不存在，回退到角色位置"),
				*ThrowSocketName.ToString());
		}
		return Character->GetActorLocation();
	}
	return GetAvatarActorFromActorInfo()->GetActorLocation();
}

void UThrowDaggerAbility::StartTurnToFace(const FVector& AimDir)
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return;

	// 只转 Yaw（投掷是水平飞行），Pitch/Roll 保持 0，避免把人物转仰/转翻。
	TargetFacingRotation = AimDir.Rotation();
	TargetFacingRotation.Pitch = 0.f;
	TargetFacingRotation.Roll = 0.f;

	TurnStartRotation = Character->GetActorRotation();
	TurnElapsed = 0.f;

	// 关掉「朝向移动」的自动转向：角色默认 bOrientRotationToMovement=true，
	// 移动组件每帧会按 RotationRate 把角色转回移动方向，跟下面的逐帧 SetActorRotation 打架，
	// 造成「转两下 + 镜头抖动」。转向期间只保留能力这一套旋转来源。
	if (UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement())
	{
		bWasOrientRotationToMovement = MoveComp->bOrientRotationToMovement;
		MoveComp->bOrientRotationToMovement = false;
		bOrientRotationOverridden = true;
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 转向开始: 起点Yaw=%.1f → 目标Yaw=%.1f, 关掉bOrientRotationToMovement(原值=%d), 时长=%.2fs"),
			TurnStartRotation.Yaw, TargetFacingRotation.Yaw, bWasOrientRotationToMovement, CastThrowDelay);
	}

	// 按 CastThrowDelay 时长从起点平滑插值到目标朝向：出手帧正好转到位，
	// 不再在 FireDagger 里额外 snap，避免「转两次、第二次是微调」的割裂感。
	if (CastThrowDelay > KINDA_SMALL_NUMBER)
	{
		GetWorld()->GetTimerManager().SetTimer(TurnTimer, this,
			&UThrowDaggerAbility::TickTurnToFace, 0.016f, true);
	}
	else
	{
		// 无延迟（或没配动画）：直接一次转到位。不在这里恢复，统一等能力结束（ExitAimingState）再恢复，
		// 保证整个出手过程都朝瞄准方向，不会中途被移动组件转回去。
		Character->SetActorRotation(TargetFacingRotation);
	}
}

void UThrowDaggerAbility::RestoreOrientRotationToMovement()
{
	// 只在真正关过的时候恢复，避免取消/提前结束路径把 bOrientRotationToMovement 误设成 false。
	if (!bOrientRotationOverridden) return;
	bOrientRotationOverridden = false;

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return;

	// 用 lambda + 角色弱引用，而不是挂在能力实例(this)上：能力在 EndAbility 后会被回收，
	// 挂在 this 上的 timer 不会触发。这里只捕获弱引用，能力销毁后照样能延迟恢复。
	const bool bSaved = bWasOrientRotationToMovement;
	TWeakObjectPtr<ACharacter> WeakChar(Character);
	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 出手结束，%0.2fs 后恢复 bOrientRotationToMovement=%d (当前Yaw=%.1f)"),
		PostCastHoldDuration, bSaved, Character->GetActorRotation().Yaw);

	GetWorld()->GetTimerManager().SetTimer(PostRestoreTimer, [WeakChar, bSaved]()
	{
		ACharacter* C = WeakChar.Get();
		if (!C) return;
		UCharacterMovementComponent* MC = C->GetCharacterMovement();
		if (!MC) return;
		MC->bOrientRotationToMovement = bSaved;
		const FVector Loc = C->GetActorLocation();
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 已恢复 bOrientRotationToMovement=%d (Yaw=%.1f Loc=(%.0f,%.0f,%.0f))"),
			bSaved, C->GetActorRotation().Yaw, Loc.X, Loc.Y, Loc.Z);
	}, PostCastHoldDuration, false);
}

void UThrowDaggerAbility::TickTurnToFace()
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return;

	TurnElapsed += GetWorld()->GetDeltaSeconds();
	const float Alpha = FMath::Clamp(TurnElapsed / CastThrowDelay, 0.f, 1.f);

	// 手动最短路径 yaw 插值：NormalizeAxis 把角度差压到 [-180,180]，天然走短弧，
	// 避免四元数 slerp 在 ±180° 环绕附近符号翻转导致的偶发瞬间闪转。
	const float DeltaYaw = FRotator::NormalizeAxis(TargetFacingRotation.Yaw - TurnStartRotation.Yaw);
	const float CurYaw = TurnStartRotation.Yaw + DeltaYaw * Alpha;

	FRotator Cur = Character->GetActorRotation();
	// 诊断：记录本 tick 读取到的 yaw（= 上一 tick 设置后被外部改动后的结果），对比 CurYaw 就能看出有没有别的东西在转。
	const float YawBefore = Cur.Yaw;
	Cur.Yaw = CurYaw;   // 只改 Yaw，保留当前 Pitch/Roll（不把人物转仰/转翻）
	Character->SetActorRotation(Cur);
	const FVector Loc = Character->GetActorLocation();
	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] TurnTick t=%.3f alpha=%.2f yawBefore=%.1f -> yawSet=%.1f Loc=(%.0f,%.0f,%.0f)"), TurnElapsed, Alpha, YawBefore, CurYaw, Loc.X, Loc.Y, Loc.Z);

	if (Alpha >= 1.f)
	{
		// 转向到位就停 timer。这里【不】恢复 bOrientRotationToMovement：一旦恢复，移动组件会立刻把角色
		// 转回移动方向，跟刚才朝瞄准方向的转向连在一起就是「转两下」。统一等整个能力结束再恢复。
		GetWorld()->GetTimerManager().ClearTimer(TurnTimer);
	}
}

void UThrowDaggerAbility::FireDagger()
{
	GetWorld()->GetTimerManager().ClearTimer(TurnTimer);
	GetWorld()->GetTimerManager().ClearTimer(ThrowDelayTimer);
	// 不在这里恢复 bOrientRotationToMovement：出手那一刻角色还应继续朝瞄准方向，恢复会中途把它转回移动方向（转两下）。
	// 统一由 EndAbility → ExitAimingState 恢复。

	// 平滑转向在 CastThrowDelay 已到位，这里不再额外 snap（去掉「第二次微调」）。
	// 仅当没配 Montage（立即出手、转向还没来得及跑）时才一次性转到位。
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (Character && !MontageTask)
	{
		Character->SetActorRotation(TargetFacingRotation);
	}

	if (K2_HasAuthority())   // 单机恒 true
	{
		SpawnProjectile(ThrowDirection);
	}

	if (!MontageTask)
	{
		// 没配 Montage：没有动画可等，出手后直接收尾
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
	}
}
