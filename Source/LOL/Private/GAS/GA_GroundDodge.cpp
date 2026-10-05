// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GA_GroundDodge.h"
#include "GAS/GE_GroundDodgeCooldown.h"
#include "GAS/GE_DodgeWindow.h"
#include "GAS/HeroCombatCharacter.h"
#include "GAS/LOLGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimMontage.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "TimerManager.h"

UGA_GroundDodge::UGA_GroundDodge()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	// 沉默挡法术。死亡/眩晕/击退/击飞在基类已经挡了。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Silenced);

	CooldownDuration = 5.f;
	CooldownGameplayEffectClass = UGE_GroundDodgeCooldown::StaticClass();

	// 完美闪避的无敌窗口。不给默认值的话窗口是静默失效的
	//（表现就是「闪避了但还是掉血」，从屏幕上分不出是没配还是判定没跑）。
	DodgeWindowEffect = UGE_DodgeWindow::StaticClass();
}

void UGA_GroundDodge::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// LocalPredicted 在 ListenServer 主机上「预测 + 权威」各调一次 ActivateAbility：
	// 第二趟直接返回，别把位移/蒙太奇再跑一遍。
	if (bEvadeStarted)
	{
		return;
	}
	bEvadeStarted = true;

	// 完美闪避的无敌窗口。和 GA_Dodge 同一套路：两端各开一份、时长一致，
	// 判定在服务端由 UDodgeComponent 读 State.Dodge.Window（走 GE ⇒ 会复制，
	// 被闪避的那一方通常是远端玩家，服务端必须能独立知道他这一刻无敌）。
	// 位置放在 bEvadeStarted 之后：LocalPredicted 的权威第二趟会提前 return，
	// 放前面就会在同一帧叠两层窗口。
	if (DodgeWindowEffect && PerfectDodgeWindow > 0.f)
	{
		FGameplayEffectSpecHandle WindowSpec = MakeOutgoingGameplayEffectSpec(DodgeWindowEffect, GetAbilityLevel());
		if (WindowSpec.IsValid())
		{
			WindowSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DodgeWindow, PerfectDodgeWindow);
			ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, WindowSpec);
		}
		else
		{
			// 不静默：Spec 无效 = 这次闪避根本没有无敌帧，而表现上完全看不出来。
			UE_LOG(LogTemp, Warning, TEXT("[Dodge] 无敌窗口 GE(%s) 的 Spec 无效 → 这次地面闪避没有无敌帧"), *GetNameSafe(DodgeWindowEffect));
		}
	}

	// 方向：移动输入（没按方向退回朝向）。
	FVector Dir = Character->GetLastMovementInputVector().GetSafeNormal2D();
	if (Dir.IsNearlyZero())
	{
		Dir = Character->GetActorForwardVector().GetSafeNormal2D();
	}
	DodgeDirection = Dir;

	// 地面冲刺：位移走【每帧补速度】，不是一次性给个初速然后撒手。
	//
	// 【为什么不是 LaunchCharacter 一把给完】原来的写法是
	//   LaunchCharacter(Dir * DodgeImpulse, bXYOverride=true, bZOverride=false)
	// 给完就交给 CharacterMovementComponent 自由减速 —— 地面摩擦（GroundFriction=8）
	// 加 BrakingDecelerationWalking(2000) 会立刻把速度啃掉，实际位移远小于配置值。
	// 更糟的是 bZOverride=false 意味着一路上没有任何 Z 速度，胶囊【底面始终贴着地面】
	// 滑行，而 PhysWalking 的位移是 SafeMoveUpdatedComponent（整胶囊扫掠）——
	// 地面稍微不平/有台阶/斜坡，胶囊就被顶住，向前那一步几乎没走成，表现就是"冲到一半卡住"。
	//
	// 改成：起手给一个初始速度 + 一个很小的离地抬升（DodgeLiftOffSpeed），
	// 之后每个 DodgeUpdateInterval 用【剩余距离】反解当前该有的速度补进去。
	// 这样速度不会被摩擦吃掉（补回来），离地后扫掠也不再和地面纠缠。
	//
	// 只有权威端做位移（客户端靠 character movement 的常规复制跟随）。
	if (Character->HasAuthority())
	{
		if (UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			DodgeStartLocation = Character->GetActorLocation();
			DodgeStartTime = GetWorld()->GetTimeSeconds();

			// 初始速度 = 走完目标距离所需的平均速度。下限 MaxWalkSpeed 避免"距离很短时速度
			// 比走路还慢"，上限用一个宽松的夹取防止配置笔误导致瞬移。
			const float AvgSpeed = DodgeDistance / FMath::Max(DodgeDuration, 0.02f);
			const float LaunchSpeed = FMath::Clamp(AvgSpeed, Movement->MaxWalkSpeed, 6000.f);

			// bXYOverride=true：水平速度就是冲刺方向的速度。
			// bZOverride=false：不动 Z（保留当前竖直状态），另外额外抬一点离地。
			Character->LaunchCharacter(
				DodgeDirection * LaunchSpeed + FVector::UpVector * DodgeLiftOffSpeed,
				/*bXYOverride=*/true, /*bZOverride=*/false);

			GetWorld()->GetTimerManager().SetTimer(DodgeUpdateTimer, this,
				&ThisClass::TickDodgeStep, DodgeUpdateInterval, /*bLoop=*/true);
		}
	}

	// 喷射表现复用 GC_Dodge（多播）。Normal = 闪避方向，背喷粒子据此朝【反方向】喷
	// （朝前闪 → 向后喷），而不是永远朝角色局部前方。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		FGameplayCueParameters CueParams;
		CueParams.Instigator = Character;
		CueParams.EffectCauser = Character;
		CueParams.Location = Character->GetActorLocation();
		CueParams.Normal = Dir;
		ASC->ExecuteGameplayCue(LOLGameplayTags::GameplayCue_Dodge, CueParams);
	}

	// 前 / 后：决定 EvadeDirection（0 = 前，180 = 后）。
	const float FwdDot = FVector::DotProduct(Character->GetActorForwardVector().GetSafeNormal2D(), Dir);
	const bool bForward = FwdDot >= 0.f;

	// 蒙太奇路线（唯一路线）：Fwd/Bwd 各一条完整蒙太奇，Start/Mid/End 三段已烘进蒙太奇里。
	//
	// 【为什么不走 BlendSpace】ABP 里的 AnimGraphNode_BlendSpaceGraph_1 是个空壳
	// （BlendSpace=None，指向的资产已丢），它会把一个未定义姿势混进最终输出 —— 表现就是
	// 抽搐 / 偶尔播错片段。BS_Evade 那套驱动已弃用，组件留着以备将来重做 BS。
	UAnimMontage* EvadeMontage = bForward ? EvadeForwardMontage : EvadeBackwardMontage;
	if (EvadeMontage)
	{
		Character->PlayAnimMontage(EvadeMontage);
	}

	// 【不能在这里 EndAbility】位移是持续性的（DodgeMaxDuration 最长 0.35s），能力必须活到
	// 位移结束，否则 EndAbility 里的 FinishDodge 会把补速计时器直接掐掉 —— 位移就只剩起手
	// 那一下。收尾统一走 TickDodgeStep → FinishDodge → EndAbilitySelf。
	//
	// 【客户端怎么办】非权威端不跑补速计时器（LaunchCharacter 只在 HasAuthority 分支），
	// 能力会在下一次 EndAbility 时由 GAS 的常规流程收掉。这里给一个比位移稍长的兜底定时器，
	// 保证两端都会结束，不会挂住 bEvadeStarted 把后续闪避全锁死。
	if (!Character->HasAuthority())
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick([this]()
		{
			EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
				/*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
		});
	}
}

void UGA_GroundDodge::TickDodgeStep()
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character)
	{
		FinishDodge();
		return;
	}

	UCharacterMovementComponent* Movement = Character->GetCharacterMovement();
	if (!Movement)
	{
		FinishDodge();
		return;
	}

	const float Elapsed = GetWorld()->GetTimeSeconds() - DodgeStartTime;

	// 兜底①：超时（贴墙冲、撞进凹角原地顶住 —— 走不够距离也不会永远拱下去）。
	if (Elapsed >= DodgeMaxDuration)
	{
		FinishDodge();
		return;
	}

	// 已走的水平距离（清 Z —— 竖直的离地/落地不该计入冲刺距离）。
	const FVector Delta = Character->GetActorLocation() - DodgeStartLocation;
	const float Travelled = FVector(Delta.X, Delta.Y, 0.f).Size();
	const float Remaining = DodgeDistance - Travelled;

	// 兜底②：走够距离了。按剩余比例把速度削掉，避免"停表后还往前溜一段"
	// （那是"配置 450 实际走 600"的另一个来源）。
	if (Remaining <= 0.f)
	{
		const FVector Velocity = Movement->Velocity;
		const FVector Velocity2D = FVector(Velocity.X, Velocity.Y, 0.f);
		float Speed2D = Velocity2D.Size();
		if (Speed2D > 1.f)
		{
			// 削多少：走完那一刻剩余距离是 0，但因为是离散采样，最后一步可能超出一点点。
			// 用"超出量"占单步位移的比例来决定削多狠，超得越多削得越干净。
			const float Overshoot = -Remaining;
			const float StepDist = FMath::Max(Speed2D * DodgeUpdateInterval, 1.f);
			const float T = FMath::Clamp(Overshoot / StepDist, 0.f, 1.f);
			const float Damp = FMath::Lerp(0.f, DodgeStopDamping, T);

			const FVector NewVelocity = Velocity - Velocity2D * Damp;
			Movement->Velocity = NewVelocity;
		}
		FinishDodge();
		return;
	}

	// 兜底③：撞墙 —— 水平速度被几何吃掉（或几乎为零），再补也是白补，直接收手。
	// 判据用"上一次补的速度掉了多少"太绕，直接用当前速度：速度趋近 0 说明被顶住了。
	const FVector Velocity2D = FVector(Movement->Velocity.X, Movement->Velocity.Y, 0.f);
	if (Elapsed > DodgeUpdateInterval * 2.f && Velocity2D.Size() < 50.f)
	{
		FinishDodge();
		return;
	}

	// 正常推进：用【剩余距离 ÷ 剩余时间】反解当前该有的速度。
	//
	// 【为什么不用固定速度】固定速度的话，一旦中途被什么挡了一下（对手胶囊、小台阶），
	// 走出来的总距离就永久少了那一截，永远不会补回来 —— 这正是"距离比设置小很多"的来源。
	// 用剩余距离反解：被挡掉多少，后面就补多少，总位移收敛到配置值。
	const float RemainingTime = FMath::Max(DodgeMaxDuration - Elapsed, 0.02f);
	const float DesiredSpeed = FMath::Clamp(Remaining / RemainingTime,
		Movement->MaxWalkSpeed, 6000.f);

	// 朝冲刺方向补速度。保留已被消耗的那部分（不硬覆盖成满值），只把不足的补上 ——
	// 硬覆盖会在撞墙时反复顶墙，看起来像抽搐。
	const FVector Current2D = FVector(Movement->Velocity.X, Movement->Velocity.Y, 0.f);
	const float AlongDir = FVector::DotProduct(Current2D, DodgeDirection);

	if (AlongDir < DesiredSpeed)
	{
		Movement->Velocity += DodgeDirection * (DesiredSpeed - AlongDir);
	}

	// 离地抬升的补速：只在还没离地（几乎贴着地面）时补一点点，让胶囊离开地面，
	// 扫掠位移就不再被地面几何顶住。落地后不再补（否则会一直悬浮）。
	if (DodgeLiftOffSpeed > 0.f && !Movement->IsFalling() && Elapsed < DodgeDuration)
	{
		Movement->Velocity.Z = FMath::Max(Movement->Velocity.Z, DodgeLiftOffSpeed);
	}
}

void UGA_GroundDodge::FinishDodge()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DodgeUpdateTimer);

		// 结束能力。用下一帧而不是当前帧：FinishDodge 可能是在 TickDodgeStep（定时器回调）
		// 里被调的，同步 EndAbility 会在定时器回调栈里改 GAS 状态，容易踩到重入。
		// bDodgeFinishing 闩防止 EndAbility → FinishDodge 再走一圈。
		if (!bDodgeFinishing)
		{
			bDodgeFinishing = true;
			World->GetTimerManager().SetTimerForNextTick([this]()
			{
				EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
					/*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
			});
		}
	}
	DodgeDirection = FVector::ZeroVector;
}

void UGA_GroundDodge::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	// 清计时器 + 复位。不走 FinishDodge（那会再排一次 EndAbility，无限递归）；
	// 这里直接清，正是 FinishDodge 的"已经决定结束了"那一半。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DodgeUpdateTimer);
	}
	DodgeDirection = FVector::ZeroVector;
	bDodgeFinishing = false;

	// 复位「这一趟已起手」闩，下一次激活才能再闪避。
	bEvadeStarted = false;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
