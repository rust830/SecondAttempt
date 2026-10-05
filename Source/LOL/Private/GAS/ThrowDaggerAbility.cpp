// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/ThrowDaggerAbility.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/GE_ThrowDaggerCooldown.h"
#include "AbilitySystemComponent.h"
#include "GAS/ThrowDaggerProjectile.h"
#include "GAS/HeroCombatCharacter.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "UObject/SoftObjectPath.h"

UThrowDaggerAbility::UThrowDaggerAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	// 沉默挡法术：被沉默时扔不出匕首。死亡/眩晕在基类已经挡了，别重复加。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Silenced);

	CooldownDuration = 6.f;
	CooldownGameplayEffectClass = UGE_ThrowDaggerCooldown::StaticClass();

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

	EnterAimingState();

	// 瞄准态兜底超时（必须在 EnterAimingState 之后 —— 从这一刻起 State.Throw.Aiming
	// 才开始分流左键和普攻，所以在这一段里必须有自动出口，否则能力会永久卡在激活态）。
	// 不依赖玩家输入：两个 WaitGameplayEvent 都要玩家按键才有动静。
	GetWorld()->GetTimerManager().SetTimer(AimingTimeoutTimer, this,
		&UThrowDaggerAbility::OnAimingTimeout, AimingTimeoutSeconds, false);

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
	// 兜底清一次：正常路径下 OnThrowPressed / OnCancelPressed 已经清过了，
	// 这里防的是「被外部取消（死亡/击退）时压根没走到那两个回调」。
	GetWorld()->GetTimerManager().ClearTimer(AimingTimeoutTimer);

	ExitAimingState();
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, WasCanceled);
}

void UThrowDaggerAbility::OnThrowPressed(FGameplayEventData Payload)
{
	// 真正投掷才 commit（消耗 CD + 蓝）
	if (!CommitAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo))
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}
	// 匕首沿准心方向（= 控制旋转）。在确认这一刻锁定，投掷延迟期间鼠标再动也不影响本次出手。
	//
	// 【不转人物】方向完全由准心决定，和人物朝向无关，所以不需要「出手瞬间把人物转向瞄准方向」
	// 那套（原 StartTurnToFace：逐帧插值 yaw + 临时关 bOrientRotationToMovement + 延迟恢复）。
	// 代价是侧身/背对时手臂动画和匕首路径不在一个方向上 —— 这是选「方向只看准心」必然要付的。
	ThrowDirection = GetAimingDirection();

	// 离开瞄准态了，兜底超时没必要（而且它若在 Cast 段播完前触发会把能力提前 End 掉）。
	GetWorld()->GetTimerManager().ClearTimer(AimingTimeoutTimer);

	// 准心射线的起点和方向在同一帧一起锁：出生点要落在这条射线上（见 ResolveSpawnLocation）。
	// 拿不到相机就退回从手部 socket 直接出（旧行为）。
	bHasThrowRayOrigin = TryGetAimRayOrigin(ThrowRayOrigin);

	// 投出瞬间把人物转向瞄准方向：网格是 bOrientRotationToMovement（朝移动方向、和鼠标无关），
	// 不转的话手臂会朝「上一次移动的方向」挥出去，而匕首飞向准心 —— 两边完全脱节。
	// 时长就取 CastThrowDelay：出手帧正好转到位，不会「转两次、第二次是微调」。
	StartTurnToFace(ThrowDirection);

	// 确认后锁定准星（出手方向已定），不再跟鼠标刷新。
	// 跳 Cast 段播放；真正出手延迟到动画出手帧（CastThrowDelay），出生点落在准心射线上。
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

void UThrowDaggerAbility::OnAimingTimeout()
{
	// 只在「真的还在瞄准」时才收尾。若玩家已经确认投出（bThrowCommitted），
	// 能力由 OnThrowMontageFinished 收尾，这里不能抢。
	if (bThrowCommitted)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("[ThrowDagger] 瞄准 %.1fs 没有任何输入 → 自动取消（不耗 CD）"), AimingTimeoutSeconds);

	// 和 OnCancelPressed 走同一条语义：WasCanceled = true ⇒ 不 commit ⇒ 不消耗 CD。
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

	// 匕首轮廓交给 GameplayCue（AGC_ThrowAiming）：它自己判断「只给本地控制端生成」，
	// 并负责把粒子挂到武器 socket、退出时销毁。能力不用再存组件指针。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->AddGameplayCue(LOLGameplayTags::GameplayCue_ThrowAiming);
	}
}

void UThrowDaggerAbility::ExitAimingState()
{
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_Throw_Aiming);
	}
	if (GEHandle.IsValid())
	{
		BP_RemoveGameplayEffectFromOwnerWithHandle(GEHandle);
		GEHandle.Invalidate();
	}
	if (ThrowDelayTimer.IsValid())
	{
		GetWorld()->GetTimerManager().ClearTimer(ThrowDelayTimer);
	}
	if (TurnTimer.IsValid())
	{
		GetWorld()->GetTimerManager().ClearTimer(TurnTimer);
	}

	// 唯一恢复点：能力结束才恢复自动朝向，保证整个出手过程只朝瞄准方向转一次。
	RestoreOrientRotationToMovement();

	// 摘掉瞄准轮廓 cue（AGC_ThrowAiming 在 OnRemove 里销毁自己的粒子组件）。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveGameplayCue(LOLGameplayTags::GameplayCue_ThrowAiming);
	}
}

FVector UThrowDaggerAbility::GetAimingDirection() const
{
	// 投掷是自由弹道，仰角算瞄准的一部分 —— 带 Pitch（理由见
	// AHeroCombatCharacter::ResolveAimDirection 的注释）。
	return AHeroCombatCharacter::ResolveAimDirection(GetAvatarActorFromActorInfo(), /*bIncludePitch=*/true);
}

void UThrowDaggerAbility::SpawnProjectile(const FVector& AimDir)
{
	if (!Dagger) return;

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	// 出生点在准心射线上，不是手部 socket 本身 —— 见 ResolveSpawnLocation。
	//
	// 【三发共用同一个出生点】散射是「一把匕首从同一点分成三路」。ResolveSpawnLocation
	// 内部用的是成员 ThrowDirection（不是传进来的 ShotDir），共用正好；各自重算的话
	// 起点会错开，看起来就不像扇形了。共用还有一个好处：没拿海克斯时那一发的落点
	// 和改动前完全重合，玩家能感知到的只有「旁边多了两把」。
	const FVector SpawnLoc = ResolveSpawnLocation(Character);

	//For Hex
	const int32 Count = ResolveSpawnCount();

	// 多发时才缩放伤害（Count == 1 = 原来的单发，逐位保持改动前的数值）。
	// ⚠️ 别写成 Count == 0：ResolveSpawnCount 的两条路径都返回 >= 1（FMax(1,·) 和字面 1），
	//   那个条件恒为 false ⇒ SingleDamageScale 是死代码，三发每发都是满伤（总伤 3 倍）。
	const float Damage = BaseDamage * ((Count > 1) ? SingleDamageScale : 1.f);

	for (int i = 0;i < Count;i++) {
		const float Frac = (Count > 1)
			? (static_cast<float>(i) / static_cast<float>(Count - 1)) - 0.5f
			: 0.f;

		const FVector ShotDir = AimDir.RotateAngleAxis(Frac * SpreadAngle, FVector::UpVector);

		const FRotator ShotRot = ShotDir.Rotation();

		// 【只在多发时把每把沿自己的方向前推一点】—— 纯粹为了视觉：三把从同一点
		// 分叉时，粒子会完全叠在一起，看起来像一把。前推后扇形立刻可见。
		//
		// ⚠️ 别指望这个前推能「防止出生点互撞」：三把是沿**各自**方向前推的，
		//    相邻两把的实际间距 = 2·L·sin(半角)。SpreadAngle=30° ⇒ 半角 7.5°，
		//    L=30 ⇒ 间距只有 7.8cm，而球体直径是 24cm —— 照样重叠。
		//    要靠距离分开得 L>92cm，那匕首会在手前一米处凭空出现，不可接受。
		//    **真正的互撞防护在 AThrowDaggerProjectile::OnOverlap 的「同伴直接 return」**，
		//    那里是唯一能彻底解决的问题所在（2026-10-03 修的真凶）。
		//
		// 单发时 Frac=0 ⇒ ShotDir == AimDir；这里把间距置 0，保证没海克斯时
		// 落点和改动前【完全重合】，玩家感知到的只有「旁边多了两把」。
		const float Spacing = (Count > 1) ? MultiSpawnSpacing : 0.f;
		const FVector ShotSpawn = SpawnLoc + ShotDir * Spacing;

		AThrowDaggerProjectile* Proj = GetWorld()->SpawnActorDeferred<AThrowDaggerProjectile>(
			Dagger, FTransform(ShotRot, ShotSpawn));
		if (Proj)
		{
			// ★ 必须传 ShotDir，不能传 AimDir：Initialize 里做的是
			//   Velocity = Direction.GetSafeNormal() * Speed（ThrowDaggerProjectile.cpp:62）
			//   —— 传 AimDir 的话三把匕首的飞行方向完全相同，只有生成朝向不同，
			//   表现为「三把叠在一起朝同一方向飞」，扇形等于没做。
			Proj->Initialize(ShotDir, ThrowSpeed, DamageGE, Damage, Character);
			Proj->FinishSpawning(FTransform(ShotRot, ShotSpawn));
		}
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

bool UThrowDaggerAbility::TryGetAimRayOrigin(FVector& OutOrigin) const
{
	// 相机的世界坐标就是准心射线的起点。走 PlayerController::GetPlayerViewPoint ——
	// 「手动选目标」的准星射线（AHeroCombatCharacter::TraceManualTargetUnderCrosshair）用的也是它，
	// 两处射线重合，「选目标点什么」和「匕首飞向哪」才对得上。
	//
	// 相机在弹簧臂末端（TargetArmLength 400 + 越肩偏移），旋转跟着控制旋转走
	// （CameraBoom->bUsePawnControlRotation = true），所以「相机位置 + ThrowDirection」
	// 正是屏幕上那条准心射线。
	const APawn* Pawn = Cast<APawn>(GetAvatarActorFromActorInfo());
	const APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!PC)
	{
		return false;
	}

	FVector ViewLocation;
	FRotator ViewRotation;   // 只要位置：方向由 ThrowDirection 负责（它带 Pitch、Roll 已归零）
	PC->GetPlayerViewPoint(ViewLocation, ViewRotation);

	OutOrigin = ViewLocation;
	return true;
}

FVector UThrowDaggerAbility::ResolveSpawnLocation(ACharacter* Character) const
{
	const FVector HandLoc = GetThrowSocketLocation(Character);

	const FVector RayDir = ThrowDirection.GetSafeNormal();
	if (!bHasThrowRayOrigin || RayDir.IsNearlyZero())
	{
		// 没有准心射线可用：退回直接从手部 socket 出，也就是改动之前的行为。
		// 路径会和准心平行（近距离偏出去），但好过出生在一个没有意义的点上。
		return HandLoc;
	}

	// 手在射线上的垂直投影：出生点 = 射线上离手最近的那一点，于是整条飞行路径【就是】准心射线。
	//
	// 【为什么不用 socket 坐标本身】相机是越肩的（SocketOffset 横向 60cm），手又在身体侧面，
	// 手和准心射线差着一大截；从手直接射出的匕首是一条和准心【平行】的线 ——
	// 准心指着一个人的时候，匕首从他旁边飞过去，近距离尤其明显。
	//
	// t 夹到 >= 0：弹簧臂开着碰撞检测，贴墙时会缩到很短，相机可能落在角色身上甚至更靠前，
	// 这时手会跑到射线起点【背后】，不夹的话出生点会被推到镜头后面去。
	//
	// static_cast<float>：UE5 的 FVector 是 double（LWC），DotProduct 返回 FReal=double，
	// 直接喂给 FMath::Max(…, 0.f) 会因模板参数 T 推导出两个不同类型而编不过。
	const float T = FMath::Max(static_cast<float>(FVector::DotProduct(HandLoc - ThrowRayOrigin, RayDir)), 0.f);
	return ThrowRayOrigin + RayDir * T;
}

int32 UThrowDaggerAbility::ResolveSpawnCount() const
{
	// 有海克斯 = AugmentCount 把（FMax(1,·) 兜住 0/负数）；没有 = 字面 1。
	// 两条路径都 >= 1 —— SpawnProjectile 里据此判「多发才缩放伤害」（单发时逐位保持原数值）。
	if (const UMyAbilitySystemComponent* ASC = Cast<UMyAbilitySystemComponent>(GetAbilitySystemComponentFromActorInfo())) {
		if (ASC->HasMatchingGameplayTag(LOLGameplayTags::Hex_ThrowDagger_Triple)) {
			return FMath::Max(1,AugmentCount);
		}
	}
	return 1;
}

// =============================================================================
// 出手前转向瞄准方向
//
// 网格是 bOrientRotationToMovement：朝移动方向，和鼠标无关（站着不动时甚至可能正对相机）。
// 不转的话手臂朝「上一次移动的方向」挥，匕首飞向准心 —— 两边脱节。
// =============================================================================

void UThrowDaggerAbility::StartTurnToFace(const FVector& AimDir)
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return;

	// 只取 Yaw（投掷是水平飞行），Pitch/Roll 归零，避免把人物转仰/转翻。
	TargetFacingRotation = AimDir.Rotation();
	TargetFacingRotation.Pitch = 0.f;
	TargetFacingRotation.Roll = 0.f;

	TurnStartRotation = Character->GetActorRotation();
	TurnElapsed = 0.f;

	// 关掉「朝向移动」的自动转向：移动组件每帧会按 RotationRate 把角色转回移动方向，
	// 和下面的逐帧 SetActorRotation 打架，表现是「转两下 + 镜头抖动」。
	// 转向期间只保留能力这一套旋转来源，等能力结束（ExitAimingState）再恢复。
	if (UCharacterMovementComponent* MoveComp = Character->GetCharacterMovement())
	{
		bWasOrientRotationToMovement = MoveComp->bOrientRotationToMovement;
		MoveComp->bOrientRotationToMovement = false;
		bOrientRotationOverridden = true;
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 转向开始: 起点Yaw=%.1f → 目标Yaw=%.1f, 关掉bOrientRotationToMovement(原值=%d), 时长=%.2fs"),
			TurnStartRotation.Yaw, TargetFacingRotation.Yaw, bWasOrientRotationToMovement, CastThrowDelay);
	}

	if (CastThrowDelay > KINDA_SMALL_NUMBER)
	{
		// 按 CastThrowDelay 的时长从起点插值到目标朝向：出手帧正好转到位，
		// 不在 FireDagger 里再补一次 snap，避免「转两次、第二次是微调」的割裂感。
		GetWorld()->GetTimerManager().SetTimer(TurnTimer, this,
			&UThrowDaggerAbility::TickTurnToFace, 0.016f, true);
	}
	else
	{
		// 无延迟（或没配动画）：直接一次转到位。同样不在这里恢复，见上面。
		Character->SetActorRotation(TargetFacingRotation);
	}
}

void UThrowDaggerAbility::TickTurnToFace()
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return;

	TurnElapsed += GetWorld()->GetDeltaSeconds();
	const float Alpha = FMath::Clamp(TurnElapsed / CastThrowDelay, 0.f, 1.f);

	// 手动最短路径 yaw 插值：NormalizeAxis 把角度差压到 [-180,180]，天然走短弧，
	// 避免四元数 slerp 在 ±180° 附近符号翻转导致的偶发瞬间闪转。
	const float DeltaYaw = FRotator::NormalizeAxis(TargetFacingRotation.Yaw - TurnStartRotation.Yaw);
	const float CurYaw = TurnStartRotation.Yaw + DeltaYaw * Alpha;

	FRotator Cur = Character->GetActorRotation();
	Cur.Yaw = CurYaw;   // 只改 Yaw，保留当前 Pitch/Roll
	Character->SetActorRotation(Cur);

	if (Alpha >= 1.f)
	{
		// 到位就停 timer，但【不】恢复 bOrientRotationToMovement —— 一恢复，移动组件会立刻把角色
		// 转回移动方向，和刚才的转向连起来又是「转两下」。统一等能力结束再恢复。
		GetWorld()->GetTimerManager().ClearTimer(TurnTimer);
	}
}

void UThrowDaggerAbility::RestoreOrientRotationToMovement()
{
	// 只在真正关过的时候恢复：取消 / 还没转向就结束的路径不能把 bOrientRotationToMovement 误设成 false。
	if (!bOrientRotationOverridden) return;
	bOrientRotationOverridden = false;

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return;

	// 用 lambda + 角色弱引用，而不是把回调挂在能力实例（this）上：能力在 EndAbility 之后会被回收，
	// 挂在 this 上的 timer 不会触发。这里只捕获弱引用，能力销毁后照样能延迟恢复。
	const bool bSaved = bWasOrientRotationToMovement;
	TWeakObjectPtr<ACharacter> WeakChar(Character);
	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 出手结束，%.2fs 后恢复 bOrientRotationToMovement=%d (当前Yaw=%.1f)"),
		PostCastHoldDuration, bSaved, Character->GetActorRotation().Yaw);

	GetWorld()->GetTimerManager().SetTimer(PostRestoreTimer, [WeakChar, bSaved]()
	{
		ACharacter* C = WeakChar.Get();
		if (!C) return;
		if (UCharacterMovementComponent* MC = C->GetCharacterMovement())
		{
			MC->bOrientRotationToMovement = bSaved;
		}
	}, PostCastHoldDuration, false);
}

void UThrowDaggerAbility::FireDagger()
{
	GetWorld()->GetTimerManager().ClearTimer(ThrowDelayTimer);
	GetWorld()->GetTimerManager().ClearTimer(TurnTimer);
	// 不在这里恢复 bOrientRotationToMovement：出手那一刻角色还得继续朝瞄准方向，
	// 一恢复移动组件就会把它转回移动方向（转两下）。统一由 EndAbility → ExitAimingState 恢复。

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());

	// 平滑转向在 CastThrowDelay 里已经到位，这里不再额外 snap。
	// 只有「没配 Montage」（立即出手、转向 timer 还没跑）时才一次性转到位。
	if (Character && !MontageTask && bOrientRotationOverridden)
	{
		Character->SetActorRotation(TargetFacingRotation);
	}

	// 出手音效：和匕首脱手同帧，放出手的 socket 上。
	// 两端都会跑到这里（客户端预测那份 + 服务端那份），所以旁边的玩家也听得到 —— 这是想要的。
	// 专用服务器没有音频设备，建出来也没人听，跳过。
	UWorld* World = GetWorld();
	if (World && World->GetNetMode() != NM_DedicatedServer)
	{
		// ThrowSound 是【覆盖】用：不填就走事件表 Audio.ThrowDaggerThrow（见 UHeroAudioConfig）。
		UHeroAudioLibrary::PlayAt(World, LOLGameplayTags::Audio_ThrowDaggerThrow, ThrowSound, GetThrowSocketLocation(Character));
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
