// 近战形态技基类的实现。架构理由见头文件。

#include "GAS/GA_FormMelee.h"
#include "GAS/HeroCombatCharacter.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffect.h"

UGA_FormMelee::UGA_FormMelee()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	// 沉默挡法术：这两个是主动近战，算法术。死亡/硬控基类已经挡了。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Silenced);

	// 攻击类技能要破隐（基类默认 true，这里不重复设 —— 写出来是为了让读代码的人看到这条决策）。
	bBreaksStealthOnCast = true;

	// 近战身形技的默认数值。子类在 BP 里改。
	HitCueTag = LOLGameplayTags::GameplayCue_MeleeHit;
}

// =============================================================================
// 起手 / 收尾
// =============================================================================

void UGA_FormMelee::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 先 Commit 再锁移动：Commit 失败（冷却/蓝/被控）就不要动姿态了。
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 技能实例是 InstancedPerActor —— 整个对局复用同一个对象，上一趟的旗标会留到这一趟。
	// 漏清的表现是「第一次转身，之后每次都不转」。
	HitAppliedStages.Reset();
	TurnElapsed = 0.f;
	bTurning = false;
	bTargetBehind = false;

	// 选目标。只在服务端做（客户端不需要权威目标，见头文件「判定为什么走 notify」）。
	// 两端都要 LockedTarget 的话另说 —— 目前这两个技能不需要（表现全靠 montage）。
	if (K2_HasAuthority())
	{
		LockedTarget = FindTarget();

		// 目标在背后？TurnSlash 靠它决定转 180 还是 0。
		// 判据用【目标 - 角色】和【角色朝向】的点积：< 0 = 在身后 90° 之外。
		if (const AActor* Target = LockedTarget.Get())
		{
			const FVector ToTarget = (Target->GetActorLocation() - Character->GetActorLocation()).GetSafeNormal2D();
			const FVector Facing = Character->GetActorForwardVector().GetSafeNormal2D();
			bTargetBehind = (FVector::DotProduct(ToTarget, Facing) < 0.f);
		}
	}

	// 锁移动 + 关自动转向。收尾统一在 EndAbility（被打断时那是唯一能走到的地方）。
	// ShouldLockMovement 是给空中技能（UGA_AirAttack）的逃生口：空中锁移动会把人从
	// 跳跃轨迹上拽停。EndAbility 那侧不用加判断 —— ApplyStanceLock(false) 内部有
	// bMovementLocked 闩，没锁过时它什么都不做。
	if (ShouldLockMovement())
	{
		ApplyStanceLock(true);
	}

	// 转向：起手立刻定好起点和终点，逐帧插值由 TickTurn 驱动。
	// ★ TurnSlash 覆盖 GetTurnDegrees 返回 180 时，这里会把终点定到目标所在的方位；
	//   子类不覆盖则 TurnDegrees=0、bTurning 直接为 false，一个 timer 都不排。
	if (const float TurnDegrees = GetTurnDegrees(); FMath::Abs(TurnDegrees) > KINDA_SMALL_NUMBER)
	{
		TurnStartRotation = Character->GetActorRotation();

		// 终点 = 「面朝锁定目标」，bTargetBehind 时再加 180°（回身斩要转到目标的另一侧）。
		//
		// 为什么不用「起点 + TurnDegrees」：那样在「角色本来就斜着站」时会转错总量
		//（目标在右后方 135°、起点偏 30°，转 180 就停在 210° 而不是 135°）。
		// 反转法保证「终点一定正好面朝目标（或其反方向）」，
		// TurnDegrees 只影响过程转多快、不影响转到位停在哪。
		FVector FacingLocation = Character->GetActorLocation();
		FRotator FacingTarget = TurnStartRotation;
		ComputeFacingRotation(0.f, FacingLocation, FacingTarget);

		if (FacingLocation.IsNearlyZero())
		{
			// ComputeFacingRotation 没给出有效终点（没有 LockedTarget / 重合位置）⇒
			// 退回「按 TurnDegrees 相对起点转」这个退路，总比不转好。
			// ⚠️ 这条退路只在这一种情况下走，且要在日志里说出来 ——
			//   否则「回身斩转了 180° 但没对着人」是完全静默的。
			FacingTarget = TurnStartRotation;
			FacingTarget.Yaw += TurnDegrees;
			UE_LOG(LogTemp, Warning,
				TEXT("[FormMelee] %s 算不出面朝目标的朝向（LockedTarget=%s）→ 退回「起点 + %.0f°」"),
				*GetName(), *GetNameSafe(LockedTarget.Get()), TurnDegrees);
		}
		else if (bTargetBehind)
		{
			FacingTarget.Yaw += 180.f;
		}

		TurnTargetRotation = FacingTarget;
		// 只取 Yaw：近战转身不该把人物转仰/转翻。
		TurnTargetRotation.Pitch = 0.f;
		TurnTargetRotation.Roll = 0.f;
		bTurning = true;

		if (TurnDuration > KINDA_SMALL_NUMBER)
		{
			GetWorld()->GetTimerManager().SetTimer(
				TurnTimerHandle, this, &UGA_FormMelee::TickTurn, 0.016f, /*bLoop=*/true);
		}
		else
		{
			// 无延迟：一次性转到位。
			TurnElapsed = TurnDuration;
			ApplyTurnRotation();
			bTurning = false;
		}
	}

	// 命中通知：【每一段各订阅一个】。两段在同一条 montage 里连着播，
	// 靠各自的标签区分「哪一段结算了」—— 这是多段技能唯一的区分手段。
	//
	// ⚠️ 每段必须用不同标签：WaitGameplayEvent 是按标签订阅的，同一个标签发两次
	//   只会唤醒一次（第二段被吃掉，症状是「两段动作播完只有第一下有伤害」）。
	//
	// bOnlyTriggerOnce=true：同一段那条标签被放多个 notify 时只结算一次
	//   （真正的去重在 OnAnimImpact 里的 HitAppliedStages，这里是省掉多余回调）。
	//
	// ⚠️ 客户端不挂：notify 默认 bServerOnly=true，客户端永远等不到，
	//   而命中结算本来就只该在服务端跑（客户端没有权威的 HitResult）。
	int32 SubscribedCount = 0;

	// ⚠️ 整个订阅循环只在服务端跑：notify 默认 bServerOnly=true，客户端永远等不到这些事件
	//   （挂上去只是白挂 N 个任务）。命中结算本来就只在服务端跑 —— 客户端没有权威的 HitResult。
	if (K2_HasAuthority())
	{
		for (int32 StageIdx = 0; StageIdx < Stages.Num(); ++StageIdx)
		{
			if (!Stages[StageIdx].ImpactTag.IsValid()) continue;

			UAbilityTask_WaitGameplayEvent* WaitImpact = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(
				this, Stages[StageIdx].ImpactTag, nullptr,
				/*bOnlyTriggerOnce=*/true, /*bOnlyMatchExact=*/true);
			WaitImpact->EventReceived.AddDynamic(this, &UGA_FormMelee::OnAnimImpact);
			WaitImpact->ReadyForActivation();
			++SubscribedCount;
		}

		// 诊断。一段都没订阅 = 这次不会有任何伤害 —— 必须吵出来，
		// 静默的表现是「技能播完了但不掉血」，看起来像伤害配错了。
		// ⚠️ 这里也覆盖了「Stages 整个是空的」这个更常见的情况。
		if (SubscribedCount == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[FormMelee] %s 没有任何一段配了 ImpactTag（Stages 有 %d 段）→ 这次不会有伤害"),
				*GetName(), Stages.Num());
		}
		else
		{
			UE_LOG(LogTemp, Log, TEXT("[FormMelee] %s 订阅了 %d 段的命中通知（Stages 共 %d 段）"),
				*GetName(), SubscribedCount, Stages.Num());
		}
	}

	// 播 montage。没有 Montage 也继续走（形态锁/转向已经生效），
	// 但要提醒 —— 否则「技能放出去没人动」很难查。
	if (Montage)
	{
		UAnimInstance* AnimInstance = ActorInfo ? ActorInfo->GetAnimInstance() : nullptr;
		if (AnimInstance)
		{
			// CreatePlayMontageAndWaitProxy 的签名里【没有 slot 参数】（5.8 的实际签名：
			// OwningAbility, TaskInstanceName, MontageToPlay, Rate, StartSection,
			// bStopWhenAbilityEnds, AnimRootMotionTranslationScale, StartTimeSeconds,
			// bAllowInterruptAfterBlendOut）—— slot 是蒙太奇【资产自己的】属性，
			// 在 Montage 编辑器里设，不是这里传。
			//
			// bStopWhenAbilityEnds = true：技能结束时自动停蒙太奇（我们要的就是这个 ——
			// 被打断时蒙太奇跟着停，不会留下一个「人已经不能动了但动画还在播」的残留）。
			MontageTask = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
				this, NAME_None, Montage, /*Rate=*/1.f, /*StartSection=*/NAME_None,
				/*bStopWhenAbilityEnds=*/true);
			// ★ 五个回调全绑 OnMontageFinished（不是 EndAbility）：EndAbility 带 4 个参数，
			//   和 FPlayMontageDelegateDynamic（无参）签名不匹配 ——
			//   绑了能编译过但运行时不触发，症状是「蒙太奇播完了技能不结束、
			//   人物一直锁着不能动」。被打断也绑：那时也要收尾。
			MontageTask->OnCompleted.AddDynamic(this, &UGA_FormMelee::OnMontageFinished);
			MontageTask->OnBlendOut.AddDynamic(this, &UGA_FormMelee::OnMontageFinished);
			MontageTask->OnInterrupted.AddDynamic(this, &UGA_FormMelee::OnMontageFinished);
			MontageTask->OnCancelled.AddDynamic(this, &UGA_FormMelee::OnMontageFinished);
			MontageTask->ReadyForActivation();
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[FormMelee] %s 没有 AnimInstance → 不播 Montage，本次只锁姿态"),
				*GetNameSafe(Character));
			// 没有动画就没有「收招」这个时刻 ⇒ 立刻收尾（伤害已经由 notify 结算或不结算）。
			EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
			return;
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[FormMelee] %s 没配 Montage → 本技能不会播动画"), *GetName());
		EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
	}
}

void UGA_FormMelee::OnMontageFinished()
{
	// OnBlendOut 与 OnCompleted 会对同一段动画各触发一次；打断/取消也会走这里。
	// 幂等：不加闩的话 EndAbility 会被调两次（第二次是 no-op，但会多打一条日志、
	// 并且 ApplyStanceLock 会被调第二次 —— 第二次是「没锁过所以不动作」，安全但不干净）。
	if (!MontageTask) return;

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
		/*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}

void UGA_FormMelee::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	// 停转向定时器。被打断时这是唯一能走到的地方 —— 漏清的话那个 0.016s 的
	// 逐帧 timer 会一直转到 World 被销毁，而它回调里还在 SetActorRotation
	//（表现为「死亡后尸体还在缓慢转向」）。
	GetWorld()->GetTimerManager().ClearTimer(TurnTimerHandle);
	bTurning = false;

	// 恢复姿态。只在真锁过时恢复（和 ThrowDaggerAbility 那套同一条纪律：
	// 盲目恢复会把别的系统关掉的开关打开）。
	ApplyStanceLock(false);

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_FormMelee::ApplyStanceLock(bool bLock)
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	UCharacterMovementComponent* MoveComp = Character ? Character->GetCharacterMovement() : nullptr;
	if (!MoveComp) return;

	if (bLock)
	{
		if (!bMovementLocked)
		{
			SavedMovementMode = MoveComp->MovementMode;
			bMovementLocked = true;
		}
		// 先停速度再关模式：DisableMovement 本身不会清速度，
		// 不先 StopMovementImmediately 的话角色会在原地滑一段（表现为「出手瞬间还在漂」）。
		MoveComp->StopMovementImmediately();
		MoveComp->DisableMovement();

		// 关自动转向：移动组件每帧会按 RotationRate 把角色转回移动方向，
		// 和下面 TurnSlash 的逐帧 SetActorRotation 打架，表现是「转身转两下 + 镜头抖」。
		bWasOrientRotationToMovement = MoveComp->bOrientRotationToMovement;
		MoveComp->bOrientRotationToMovement = false;
		bOrientRotationOverridden = true;
	}
	else
	{
		if (bMovementLocked)
		{
			bMovementLocked = false;
			MoveComp->SetMovementMode(SavedMovementMode);
		}
		if (bOrientRotationOverridden)
		{
			bOrientRotationOverridden = false;
			MoveComp->bOrientRotationToMovement = bWasOrientRotationToMovement;
		}
	}
}

// =============================================================================
// 转身（TurnSlash 用；不覆盖 GetTurnDegrees 的子类这里一个 timer 都不排）
// =============================================================================

void UGA_FormMelee::ComputeFacingRotation(float Seconds, FVector& OutLocation, FRotator& OutRotation) const
{
	// 默认：面朝锁定目标。子类覆盖成「面朝目标的反方向」（回身斩）。
	const AActor* Character = GetAvatarActorFromActorInfo();
	const AActor* Target = LockedTarget.Get();
	if (!Character || !Target) return;

	const FVector ToTarget = (Target->GetActorLocation() - Character->GetActorLocation()).GetSafeNormal2D();
	if (ToTarget.IsNearlyZero()) return;
	OutLocation = Character->GetActorLocation();
	OutRotation = ToTarget.Rotation();
}

void UGA_FormMelee::TickTurn()
{
	TurnElapsed += GetWorld()->GetDeltaSeconds();
	ApplyTurnRotation();

	if (TurnElapsed >= TurnDuration)
	{
		// 到位就停 timer，但【不】在这里恢复 bOrientRotationToMovement ——
		// 恢复了移动组件会立刻把角色转回移动方向，和刚才的转身连起来是「转两下」。
		// 统一等 EndAbility → ApplyStanceLock(false) 恢复。
		GetWorld()->GetTimerManager().ClearTimer(TurnTimerHandle);
		bTurning = false;
	}
}

void UGA_FormMelee::ApplyTurnRotation()
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return;

	const float Alpha = FMath::Clamp(TurnElapsed / FMath::Max(TurnDuration, KINDA_SMALL_NUMBER), 0.f, 1.f);

	// 手动最短路径 yaw 插值：NormalizeAxis 把角度差压到 [-180,180]，天然走短弧，
	// 避免四元数在 ±180° 附近符号翻转导致的偶发瞬间闪转
	//（回身斩正好要转 180°，这条路径是必经的 —— 用 slerp 一定会偶发闪）。
	const float DeltaYaw = FRotator::NormalizeAxis(TurnTargetRotation.Yaw - TurnStartRotation.Yaw);
	const float CurYaw = TurnStartRotation.Yaw + DeltaYaw * Alpha;

	FRotator Cur = Character->GetActorRotation();
	Cur.Yaw = CurYaw;   // 只改 Yaw，保留当前 Pitch/Roll
	Character->SetActorRotation(Cur);
}

// =============================================================================
// 命中结算（只在服务端）
// =============================================================================

FFormMeleeStage UGA_FormMelee::ResolveStage(int32 StageIndex) const
{
	FFormMeleeStage Resolved;
	if (!Stages.IsValidIndex(StageIndex))
	{
		return Resolved;
	}
	Resolved = Stages[StageIndex];

	// 合并规则逐条对应 FFormMeleeStage 里的「留 X = 用顶层」注释。
	// ⚠️ 数值类「留空/负值 = 继承」，控制 GE「留空 = 不加控制」——
	//   两者故意相反，原因见 FFormMeleeStage::ControlGE 的注释。
	if (!Resolved.DamageGE)                        Resolved.DamageGE        = DamageGE;
	if (Resolved.DamageMultiplier < 0.f)           Resolved.DamageMultiplier = DamageMultiplier;
	if (Resolved.FlatDamage < 0.f)                 Resolved.FlatDamage       = FlatDamage;
	if (Resolved.HitRadius <= 0.f)                 Resolved.HitRadius        = HitRadius;
	if (Resolved.ControlDuration <= 0.f)           Resolved.ControlDuration  = ControlDuration;
	if (Resolved.KnockbackImpulse <= 0.f)          Resolved.KnockbackImpulse = KnockbackImpulse;
	if (Resolved.KnockbackLaunch <= 0.f)           Resolved.KnockbackLaunch  = KnockbackLaunch;
	if (Resolved.KnockUpLaunch <= 0.f)             Resolved.KnockUpLaunch    = KnockUpLaunch;
	if (!Resolved.HitCueTag.IsValid())             Resolved.HitCueTag       = HitCueTag;

	// ControlGE 【不继承顶层】：留空就是「这一段不加控制」。
	return Resolved;
}

void UGA_FormMelee::OnAnimImpact(FGameplayEventData Payload)
{
	// ★ 按【标签】认出是哪一段 —— Payload 里带的是发事件的那个标签。
	//   这一步是整个多段设计的关键：同一个回调被 N 个 WaitGameplayEvent 绑着
	//   （每个段一个），靠 Payload.EventTag 区分是哪一段触发了。
	const FGameplayTag FiredTag = Payload.EventTag;

	int32 StageIndex = INDEX_NONE;
	for (int32 i = 0; i < Stages.Num(); ++i)
	{
		if (Stages[i].ImpactTag.IsValid() && Stages[i].ImpactTag == FiredTag)
		{
			StageIndex = i;
			break;
		}
	}

	if (StageIndex == INDEX_NONE)
	{
		// 收到一个没在任何一段里配过的标签 = 蒙太奇上的 notify 标签和 Stages 对不上。
		// 这是最容易犯的接线错误（notify 填了 Event.Melee.X 但 Stages 里是 Event.Melee.Y），
		// 而它的表现是「动画播完了、notify 也发了、就是不掉血」，极难查。必须吵出来。
		UE_LOG(LogTemp, Warning,
			TEXT("[FormMelee] %s 收到未注册的命中标签 [%s]（Stages 里没有）→ 本次不结算。"
			     L"检查 montage 上的 notify 标签和 Stages[].ImpactTag 是否一致"),
			*GetName(), *FiredTag.ToString());
		return;
	}

	// 防御性去重：同一条标签在 montage 上被放了多个 notify 时只结算一次。
	if (HitAppliedStages.Contains(StageIndex))
	{
		UE_LOG(LogTemp, Log, TEXT("[FormMelee] %s 第 %d 段重复触发（同一标签有多个 notify）→ 跳过"),
			*GetName(), StageIndex);
		return;
	}
	HitAppliedStages.Add(StageIndex);

	const FFormMeleeStage Stage = ResolveStage(StageIndex);

	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !GetWorld()) return;

	UE_LOG(LogTemp, Log, TEXT("[FormMelee] %s 第 %d 段结算：半径=%.0f 伤害=%.2fx+%.0f 控制=%s 时长=%.2f"),
		*GetNameSafe(Avatar), StageIndex, Stage.HitRadius,
		Stage.DamageMultiplier, Stage.FlatDamage,
		*GetNameSafe(Stage.ControlGE), Stage.ControlDuration);

	// 球形扫描。形状和 GA_ThreeHitPassive / GA_DeathHarvest 一致（抄的那两处）。
	// ★ 半径用【本段的】HitRadius：两段的作用范围可以不同
	//   （比如第一段范围大、第二段是贴身的上挑）。
	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FormMelee), /*bTraceComplex=*/false, Avatar);
	const FVector Center = Avatar->GetActorLocation();
	const FCollisionShape Shape = FCollisionShape::MakeSphere(Stage.HitRadius);

	if (!GetWorld()->SweepMultiByChannel(Hits, Center, Center, FQuat::Identity, ECC_Pawn, Shape, Params))
	{
		UE_LOG(LogTemp, Log, TEXT("[FormMelee] %s 第 %d 段空挥（半径 %.0f 内没人）"),
			*GetNameSafe(Avatar), StageIndex, Stage.HitRadius);
		return;
	}

	// 同一个目标可能被返回多次（多段重叠），去重。
	TSet<AActor*> Hit;
	for (const FHitResult& HitResult : Hits)
	{
		AActor* Target = HitResult.GetActor();
		if (!Target || Target == Avatar || Hit.Contains(Target)) continue;
		Hit.Add(Target);
		ApplyServerHit(Target, HitResult, Stage);
	}
}

void UGA_FormMelee::ApplyServerHit(AActor* Target, const FHitResult& Hit, const FFormMeleeStage& Stage)
{
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
	if (!SourceASC || !TargetASC || !Stage.DamageGE)
	{
		// 不静默：Spec 无效 = 这一次一点伤害都没有，屏幕上看起来像「打空了」。
		UE_LOG(LogTemp, Warning, TEXT("[FormMelee] 目标 %s 没有 ASC（或 DamageGE 没配）→ 本次命中无伤害"),
			*GetNameSafe(Target));
		return;
	}

	// ---------------------------------------------------------------------
	// 伤害：只喂参数，不算伤害（公式全在 UExecCalc_Damage 里）
	// ---------------------------------------------------------------------
	FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
	// AddHitResult 不能省：格挡的判定、击退方向（UGEComponent_Knockback 的方向优先级表
	// 第一级就是 ImpactNormal）、以及 ExecCalc 里的命中信息都从它取。
	Context.AddHitResult(Hit);

	FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(Stage.DamageGE, GetAbilityLevel(), Context);
	if (Spec.IsValid())
	{
		Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, Stage.DamageMultiplier);
		Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, Stage.FlatDamage);
		// 近战是物理伤害。不挂类型标签时 ExecCalc 也按物理算，显式挂上是为了
		// 以后出「魔法伤害的近战技能」时不用回头猜默认值。
		Spec.Data->AddDynamicAssetTag(LOLGameplayTags::Damage_Physical);
		// ★ 不挂 Data.CanCrit：技能伤害【不可暴击】，和匕首/死亡收割一致。
		//   能不能暴击是「哪个伤害点」的属性（见 LOLGameplayTags.h 里 Data.CanCrit 那段）。

		SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[FormMelee] DamageGE(%s) 的 Spec 无效 → 本次命中无伤害"),
			*GetNameSafe(Stage.DamageGE));
	}

	// ---------------------------------------------------------------------
	// 控制：挂 GE（位移由 LaunchTarget 补，见下）
	//
	// ⚠️ 判据用【Stage.ControlGE】而不是顶层那个 —— 「这一段要不要控制」是
	//   逐段决定的（SpinSlash 第 1 段平砍不加控制、第 2 段上挑才击飞）。
	//   顶层 ControlGE 只在 Stage 显式配了才被 ResolveStage 继承进来。
	// ---------------------------------------------------------------------
	if (Stage.ControlGE && Stage.ControlDuration > 0.f)
	{
		// ★ 独立一份 Context，不要复用上面伤害那份 —— ControlGE 的组件要从里面
		//   读 HitResult 算方向（UGEComponent_Knockback 的四级回退表第一级就是它）。
		FGameplayEffectContextHandle ControlContext = SourceASC->MakeEffectContext();
		ControlContext.AddHitResult(Hit);

		FGameplayEffectSpecHandle ControlSpec =
			SourceASC->MakeOutgoingSpec(Stage.ControlGE, GetAbilityLevel(), ControlContext);
		if (ControlSpec.IsValid())
		{
			// 三个时长键全填：多余的键被对应的 GE 忽略（它只读自己那个），
			// 这样换控制 GE 时不用回来改这里。
			ControlSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_ControlDuration, Stage.ControlDuration);
			ControlSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackDuration, Stage.ControlDuration);
			ControlSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockUpDuration, Stage.ControlDuration);
			ControlSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackImpulse, Stage.KnockbackImpulse);
			// ⚠️ 这里原本错填了 KnockUpLaunch —— 那会让「击退」也带上击飞的抛量
			//   （表现为被推走的人还往上飞一段）。击退的抛量和击飞的抛量是两个键。
			ControlSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackLaunch, Stage.KnockbackLaunch);
			SourceASC->ApplyGameplayEffectSpecToTarget(*ControlSpec.Data.Get(), TargetASC);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[FormMelee] ControlGE(%s) 的 Spec 无效 → 本次命中没有控制"),
				*GetNameSafe(Stage.ControlGE));
		}
	}

	// 位移。击退走 GE 组件（UGE_Knockback 自带），击飞必须由这里 Launch（UGE_KnockUp 没有）。
	// ★ 传 Stage：上挑那一段和平砍那一段的抛量可以不同。
	LaunchTarget(Target, Hit, Stage);

	// 命中表现走 cue：ApplyServerHit 只在服务端跑，直接 Spawn 的话粒子只有主机看得到。
	//
	// ⚠️ ExecuteGameplayCue 是 【UAbilitySystemComponent 的成员】，不是
	//   UAbilitySystemBlueprintLibrary 的静态函数（5.8 里那个类上没有这个函数，
	//   写 UAbilitySystemBlueprintLibrary::ExecuteGameplayCue 编译不过）。
	//   和 GA_ThreeHitPassive::ExecuteMeleeHitCue 同一个入口。
	//
	// ★ 用【本段的】HitCueTag：镜头效果挂在这个 Cue 的资产里
	//   （照 UGC_DeathHarvestBurst 的做法：Cue 里配 UCameraShakeBase +
	//   ClientStartCameraShake，只震施法者本机）。两段要不同表现就配两个 Cue。
	if (Stage.HitCueTag.IsValid())
	{
		FGameplayCueParameters CueParams;
		// ImpactPoint 优先：它是真正的命中点；退化成 Hit.Location（扫掠命中时两者一致）。
		// 照抄 GA_ThreeHitPassive 那段的写法。
		CueParams.Location = Hit.ImpactPoint.IsNearlyZero() ? Hit.Location : Hit.ImpactPoint;
		CueParams.Normal = Hit.ImpactNormal.IsNearlyZero() ? Hit.Normal : Hit.ImpactNormal;
		CueParams.Instigator = GetAvatarActorFromActorInfo();
		CueParams.EffectCauser = GetAvatarActorFromActorInfo();

		// 服务端 ExecuteGameplayCue 会先在本端跑一次，再把参数多播给各客户端各自跑一次。
		SourceASC->ExecuteGameplayCue(Stage.HitCueTag, CueParams);
	}
}

void UGA_FormMelee::LaunchTarget(AActor* Target, const FHitResult& Hit, const FFormMeleeStage& Stage)
{
	// 默认什么都不做 —— 击退（UGE_Knockback）的位移由它的 UGEComponent_Knockback 做了。
	// 击飞（UGE_KnockUp）没有位移组件，子类覆盖这个函数补 LaunchCharacter。
	// 见头文件 LaunchTarget 的说明。
	//
	// ★ Stage 是【本段】的：SpinSlash 第 2 段（上挑）才填 ControlGE=GE_KnockUp，
	//   平砍那段的 ControlGE 是空的 ⇒ 不会走到这里，也不会把人挑飞。
}

AActor* UGA_FormMelee::FindTarget() const
{
	const AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !GetWorld()) return nullptr;

	// 球形扫描找 Pawn。只按「在范围内 + 不是自己 + 有 ASC」筛，
	// 不做阵营/存活过滤 —— 理由见下面那条注释。
	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(FormMeleeFind), /*bTraceComplex=*/false, Avatar);
	const FVector Center = Avatar->GetActorLocation();
	const FCollisionShape Shape = FCollisionShape::MakeSphere(HitRadius);

	if (!GetWorld()->SweepMultiByChannel(Hits, Center, Center, FQuat::Identity, ECC_Pawn, Shape, Params))
	{
		return nullptr;
	}

	AActor* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();
	for (const FHitResult& Hit : Hits)
	{
		AActor* Candidate = Hit.GetActor();
		if (!Candidate || Candidate == Avatar) continue;

		// ⚠️ 这里【故意】不判阵营和存活：找不到「敌我」这个概念的可靠判据
		//（Target.Hero / Target.Void 那批标签不是拿来做敌我过滤的），
		// 而判错的表现是「对着队友也挥一刀」—— 那是设计问题不是这里的问题。
		// 真要过滤就在这里加：判 IsValid + 找 ASC + 查 State.Dead。
		// 至少要排掉已死的（对尸体挥砍的表现最难看）。
		if (UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Candidate)
			&& Candidate->CanBeDamaged())
		{
			const float DistSq = FVector::DistSquared(Candidate->GetActorLocation(), Center);
			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = Candidate;
			}
		}
	}
	return Best;
}
