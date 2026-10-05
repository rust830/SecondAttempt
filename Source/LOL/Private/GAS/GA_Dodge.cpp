// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GA_Dodge.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/HeroCombatCharacter.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/GE_Damage.h"
#include "GAS/GE_DodgeCooldown.h"
#include "GAS/GE_DodgeWindow.h"
#include "GAS/GE_Knockback.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffectTypes.h"
#include "TimerManager.h"

namespace
{
	/** 取角色的胶囊半径 / 半高；不是 ACharacter（或没胶囊）就返回 0，退化成点对点判定。 */
	void GetCapsuleSize(const AActor* Actor, float& OutRadius, float& OutHalfHeight)
	{
		if (const ACharacter* Character = Cast<ACharacter>(Actor))
		{
			if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
			{
				OutRadius = Capsule->GetScaledCapsuleRadius();
				OutHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
				return;
			}
		}
		OutRadius = 0.f;
		OutHalfHeight = 0.f;
	}

	/**
	 * 两个胶囊的【表面】最短距离（cm，负数 = 已经嵌进去了）。
	 *
	 * 胶囊 = 一段竖直轴 + 半径，所以先求两段轴的最近距离，再各减一个半径。
	 * 轴的两个端点不是「原点 ± 半高」，是「原点 ± (半高 − 半径)」—— 半高那两头是半球心，
	 * 半径已经算在球里了。写错这一项会让"上下叠着"的一对角色算出负值（把 24cm 的真实间隙
	 * 算成 −44cm），命中判定就会提前触发。
	 *
	 * 两段轴正交分解：竖直方向取两段在 Z 上不重叠时的空档，水平方向取两条轴的水平距离。
	 */
	float CapsuleSurfaceDistance(const AActor* A, const AActor* B)
	{
		if (!A || !B)
		{
			return TNumericLimits<float>::Max();
		}

		float RadiusA = 0.f, HalfA = 0.f, RadiusB = 0.f, HalfB = 0.f;
		GetCapsuleSize(A, RadiusA, HalfA);
		GetCapsuleSize(B, RadiusB, HalfB);

		const float AxisHalfA = FMath::Max(0.f, HalfA - RadiusA);
		const float AxisHalfB = FMath::Max(0.f, HalfB - RadiusB);

		const float VerticalGap = FMath::Max(0.f,
			FMath::Abs(A->GetActorLocation().Z - B->GetActorLocation().Z) - AxisHalfA - AxisHalfB);
		const float HorizontalDist = FVector::Dist2D(A->GetActorLocation(), B->GetActorLocation());

		return FMath::Sqrt(VerticalGap * VerticalGap + HorizontalDist * HorizontalDist) - RadiusA - RadiusB;
	}

	/**
	 * 算「脚落在目标身上的哪儿」—— 表现层用的命中点 + 命中面法线。
	 *
	 * 飞踢是自上而下砸下来的，所以不能拿两个原点的连线中点：那个点会飘在两人之间的空中，
	 * 粒子炸在空气里，看着像打空了。
	 *
	 * 做法（和 CapsuleSurfaceDistance 同一套胶囊模型）：
	 *   ① 把「踢人者位置」竖直夹到目标胶囊的【轴线段】上（两端各缩一个半径，那两头是半球心）；
	 *   ② 从这个轴上最近点，朝「远离踢人者」的水平方向推一个【半径】，落在胶囊表面上。
	 * 这个点就是脚该踩的位置，法线指向踢人者（粒子朝踢来的方向喷）。
	 *
	 * 退化情况（不是角色 / 没胶囊）：退回目标原点 + 朝踢人者方向的法线。
	 */
	void ComputeImpactPointAndNormal(const AActor* Attacker, const AActor* Target,
		FVector& OutPoint, FVector& OutNormal)
	{
		if (!Attacker || !Target)
		{
			OutPoint = Target ? Target->GetActorLocation() : FVector::ZeroVector;
			OutNormal = FVector::ZeroVector;
			return;
		}

		float RadiusT = 0.f, HalfT = 0.f;
		GetCapsuleSize(Target, RadiusT, HalfT);

		const FVector TargetCenter = Target->GetActorLocation();
		const FVector AttackerLoc = Attacker->GetActorLocation();

		// 法线 = 从目标指向踢人者（粒子朝踢来的方向喷）。
		FVector Normal = (AttackerLoc - TargetCenter).GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = FVector::UpVector;
		}
		OutNormal = Normal;

		if (RadiusT <= 0.f)
		{
			// 没胶囊：退化成目标原点。
			OutPoint = TargetCenter;
			return;
		}

		// ① 把踢人者位置竖直夹到目标胶囊的轴线段上。
		//    轴线段 = 原点的 Z ± (半高 − 半径)。
		const float AxisHalf = FMath::Max(0.f, HalfT - RadiusT);
		const float ClampedZ = FMath::Clamp(AttackerLoc.Z, TargetCenter.Z - AxisHalf, TargetCenter.Z + AxisHalf);
		const FVector ClosestOnAxis(TargetCenter.X, TargetCenter.Y, ClampedZ);

		// ② 从轴上最近点朝「水平远离踢人者」的方向推一个半径。
		FVector Radial = (ClosestOnAxis - AttackerLoc);
		Radial.Z = 0.f;
		Radial = Radial.GetSafeNormal();
		if (Radial.IsNearlyZero())
		{
			// 正上/正下踢（水平重合）：竖直方向也没得推，就沿法线的反向给一个水平方向。
			Radial = (-Normal).GetSafeNormal2D();
		}
		if (Radial.IsNearlyZero())
		{
			Radial = FVector::ForwardVector;
		}

		OutPoint = ClosestOnAxis + Radial * RadiusT;
	}
}

UGA_Dodge::UGA_Dodge()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	// 沉默挡法术，闪避也不例外。死亡/眩晕/击退/击飞在基类已经挡了。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Silenced);

	CooldownDuration = 10.f;   // 测试值，BP 子类里按需调
	CooldownGameplayEffectClass = UGE_DodgeCooldown::StaticClass();

	DamageEffect = UGE_Damage::StaticClass();
	KnockbackGE = UGE_Knockback::StaticClass();

	// 空手直拳的命中 cue 默认指向 GC_DodgePunchHit（BP 子类建好后自动生效）。
	// 想让拳和脚共用特效就在 BP 里清空它（回落到 GameplayCue.DodgeKickHit）。
	PunchHitCueTag = LOLGameplayTags::GameplayCue_DodgePunchHit;

	// 完美闪避的无敌窗口。不给默认值的话窗口是静默失效的
	//（表现就是「闪避了但还是掉血」，从屏幕上分不出是没配还是判定没跑）。
	DodgeWindowEffect = UGE_DodgeWindow::StaticClass();
}

void UGA_Dodge::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!Character || !ASC)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// LocalPredicted 在 ListenServer 主机上「预测 + 权威」各调一次 ActivateAbility：
	// 第二趟直接返回，别把 LaunchCharacter / 标签 / 任务再跑一遍（evade 位移会翻倍）。
	if (bEvadeStarted)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] GA_Dodge 激活（第二趟，跳过）"));
		return;
	}
	bEvadeStarted = true;

	// InstancedPerActor：成员跨多次激活保留，这里把「这一趟」的两个闩复位。
	bKickPerformed = false;
	bSecondEvadeAvailable = false;
	KickTarget = nullptr;
	bKickSettled = false;

	// 闪避位移 + 喷射表现（两端各自跑，位移只在权威端）。第一段。
	PerformEvade(Character, /*bSecond=*/false);

	// 完美闪避的无敌窗口。两端各开一份、时长一致（和 GA_Block 开格挡窗口同一个套路），
	// 判定在服务端由 UDodgeComponent 读 State.Dodge.Window。
	// ⚠️ 和下面那个 State.Dodge.Active 不是一回事：那个是【纯本地】的派生窗口（loose 标签，只给输入层看），
	//    这个走 GE ⇒ 会复制 —— 被闪避的那一方通常是远端玩家，服务端必须能独立知道他这一刻无敌。
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
			UE_LOG(LogTemp, Warning, TEXT("[Dodge] 无敌窗口 GE(%s) 的 Spec 无效 → 这次闪避没有无敌帧"), *GetNameSafe(DodgeWindowEffect));
		}
	}

	// 开派生窗口：挂本地路由标签，输入层 BasicAttackPressed 靠它把左键改判成「踢」，
	// 空格改判成「二段 evade」。
	// 只在本地控制那端挂（和 State.Throw.Aiming / State.DeathHarvest.Selecting 同一套套路）。
	if (Character->IsLocallyControlled())
	{
		ASC->AddLooseGameplayTag(LOLGameplayTags::State_Dodge_Active);
	}

	UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] GA_Dodge 激活：权威=%d 本地控制=%d → 挂 State.Dodge.Active（标签现在=%d）"),
		Character->HasAuthority() ? 1 : 0,
		Character->IsLocallyControlled() ? 1 : 0,
		ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Dodge_Active) ? 1 : 0);

	// 二段 evade 可用（一次闪避最多补一段）。
	bSecondEvadeAvailable = true;

	// 监听「按普攻 = 派生踢」。GameplayEvent 只在本地派发，服务端那份靠 ServerSubmitDodgeKickInput 补。
	KickInputTask = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, LOLGameplayTags::Event_Input_DodgeKick, nullptr, false, true);
	KickInputTask->EventReceived.AddDynamic(this, &ThisClass::OnKickInput);
	KickInputTask->ReadyForActivation();

	// 监听「按空格 = 二段 evade」。同一套镜像机制。
	EvadeInputTask = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, LOLGameplayTags::Event_Input_DodgeEvade, nullptr, false, true);
	EvadeInputTask->EventReceived.AddDynamic(this, &ThisClass::OnEvadeInput);
	EvadeInputTask->ReadyForActivation();

	// 落地即关窗口（空中才能派生向下踢）。跃起后 Landed 触发 → EndDodge。
	Character->LandedDelegate.AddDynamic(this, &ThisClass::OnLanded);

	// 兜底超时：万一角色一直没落地（卡在某个地方），窗口也别永远开着。
	GetWorld()->GetTimerManager().SetTimer(KickWindowTimer, this, &ThisClass::EndDodge, KickWindowDuration, false);
}

void UGA_Dodge::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	// 派生窗口标签一定要摘：不然 BasicAttackPressed 会一直把左键判成踢、普攻永远按不出来。
	// RemoveLooseGameplayTag 幂等（没挂过时是 no-op），这里无条件摘最稳。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_Dodge_Active);
	}

	// 解绑落地回调，避免能力结束后还留悬垂委托。
	if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
	{
		Character->LandedDelegate.RemoveDynamic(this, &ThisClass::OnLanded);
	}

	// 收掉脚上拖尾 cue + 清飞行结算定时器（被打断/被取消时不漏）。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveGameplayCue(LOLGameplayTags::GameplayCue_DodgeKick);
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(KickHitTimer);
	}

	// 复位「这一趟已起手」闩，下一次激活才能再 evade。
	bEvadeStarted = false;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_Dodge::PerformEvade(ACharacter* Character, bool bSecond)
{
	// jet deploy 蒙太奇：走 DefaultSlot，和 evade 蒙太奇抢同一条槽（AM_JetDeploy / AM_Evade_Fwd
	// 都是 DefaultSlot）。它长 1.7s，而紧随其后的 evade 蒙太奇走 Montage_Play 的默认
	// bStopAllMontages=true，会以 0.25s 淡出把它顶掉 —— 这是既有的观感，保持不变。
	if (JetDeployMontage)
	{
		Character->PlayAnimMontage(JetDeployMontage);
	}

	// 位移方向：按【移动输入方向】，不是准心（闪避是躲，方向由玩家走位决定）。
	// 没按方向时退回角色朝向（正前方闪）。
	FVector Dir = Character->GetLastMovementInputVector().GetSafeNormal2D();
	if (Dir.IsNearlyZero())
	{
		Dir = Character->GetActorForwardVector().GetSafeNormal2D();
	}

	// 喷射表现走 cue（多播），各端都看得到。喷射是背喷 booster，粒子在 cue 里挂到角色 mesh 上。
	// ★ 放在 Dir 之后：把闪避方向通过 Normal 传给 cue，背喷粒子才能朝【反方向】喷
	//   （朝前闪 → 向后喷），而不是永远朝角色的局部前方。GA_GroundDodge 同处理。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		FGameplayCueParameters CueParams;
		CueParams.Instigator = Character;
		CueParams.EffectCauser = Character;
		CueParams.Location = Character->GetActorLocation();
		CueParams.Normal = Dir;
		ASC->ExecuteGameplayCue(LOLGameplayTags::GameplayCue_Dodge, CueParams);
	}

	// 前 / 后：相对角色朝向（向前 = Fwd，向后 = Bwd）。
	// 左右输入（本项目移动只做了 WASD 的 W/S 前后 + A/D 转身，实际没有独立左右闪避动画，
	// 所以左右折进「前」，方向仍按输入 Dir 走）。
	const float FwdDot = FVector::DotProduct(Character->GetActorForwardVector().GetSafeNormal2D(), Dir);
	const bool bForward = FwdDot >= 0.f;

	// 蒙太奇路线（唯一路线）：Fwd/Bwd 各一条完整蒙太奇，Start/Mid/End 三段已烘进蒙太奇里。
	//
	// 【为什么不走 BlendSpace】ABP 里的 AnimGraphNode_BlendSpaceGraph_1 是个空壳
	// （BlendSpace=None，指向的资产已丢），它会把一个未定义姿势混进最终输出 —— 表现就是
	// 抽搐 / 偶尔播错片段。BS_Evade 那套驱动已弃用，保留组件只为将来重做 BS 时能捡回来。
	UAnimMontage* EvadeMontage = bForward ? EvadeForwardMontage : EvadeBackwardMontage;
	if (EvadeMontage)
	{
		Character->PlayAnimMontage(EvadeMontage);
	}

	// 跃向空中 + 方向位移：LaunchCharacter 给一个「向上 + 沿方向」的冲量。
	// 位移只在权威端执行（同 GA_Flash：反作弊 + 避免和 CharacterMovement 打架）。
	// Evade 序列没有根运动（已确认），所以纯靠这里两个冲量（水平 + 竖直）驱动位移。
	if (Character->HasAuthority())
	{
		const float Mult = bSecond ? SecondDodgeRangeMultiplier : 1.f;
		const FVector Impulse = Dir * (EvadeHorizontalImpulse * Mult) + FVector::UpVector * EvadeVerticalImpulse;
		Character->LaunchCharacter(Impulse, /*bXYOverride=*/true, /*bZOverride=*/true);
	}
}

void UGA_Dodge::OnKickInput(FGameplayEventData Payload)
{
	UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] OnKickInput 收到事件（权威=%d 已踢=%d）"),
		(GetAvatarActorFromActorInfo() && GetAvatarActorFromActorInfo()->HasAuthority()) ? 1 : 0,
		bKickPerformed ? 1 : 0);

	if (bKickPerformed)
	{
		return;
	}
	PerformKick();
}

void UGA_Dodge::OnEvadeInput(FGameplayEventData Payload)
{
	if (!bSecondEvadeAvailable)
	{
		return;
	}
	bSecondEvadeAvailable = false;

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character)
	{
		return;
	}

	// 二段 evade：距离乘倍率（略强一点），表现和第一段一样。
	PerformEvade(Character, /*bSecond=*/true);

	// 二段 evade 之后派生窗口重置：再给一个完整的踢窗口（SetTimer 同 handle 会重启）。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(KickWindowTimer, this, &ThisClass::EndDodge, KickWindowDuration, false);
	}
}

void UGA_Dodge::OnLanded(const FHitResult& Hit)
{
	UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] OnLanded：落地，关空中派生窗口"));

	// 踢还在飞就收尾：落地 = 这一脚飞完了，不能让 EndDodge 把待结算的命中吞掉。
	// （低空踢常见的「落到目标头上就停住」那一刻未必触发 Landed —— 对方胶囊顶不是可行走面。）
	//
	// 【这里以前是静默丢击】原来只调 TickKickFlight() 就往下走：它只在「够近」或「超时」两种
	// 情况下才收尾，两者都不满足时什么都不做，紧接着 EndDodge() 结束能力 ——
	// 结果既没有伤害、也没有任何日志，日志里只留下一句「落地」。落地是明确的终局信息，
	// 所以这里必须给出【踢中】或【扑空】其中之一，不允许第三种"悄悄没了"。
	if (!bKickSettled && KickTarget.IsValid())
	{
		const float Gap = CapsuleSurfaceDistance(GetAvatarActorFromActorInfo(), KickTarget.Get());

		// 落地判据比飞行中宽松（KickLandingMargin > KickHitMargin）：踩到人身上、贴脸落地就是踢到了。
		if (Gap <= KickLandingMargin)
		{
			UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 落地即结算：胶囊表面距离 %.1fcm ≤ 落地余量 %.0fcm → 踢中"),
				Gap, KickLandingMargin);
			OnKickHit();
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 落地扑空：还差 %.1fcm（落地余量 %.0fcm）→ 不结算伤害"),
				Gap, KickLandingMargin);
			OnKickWhiff();
		}
	}

	if (!bKickSettled)
	{
		EndDodge();
	}
}

void UGA_Dodge::PerformKick()
{
	bKickPerformed = true;

	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!ASC || !Avatar)
	{
		EndDodge();
		return;
	}

	// 空手形态 + 配了直拳蒙太奇 ⇒ 这次派生是【直拳】（判定/飞行/结算整套复用，只换表现）。
	// 两端各判一次：State.Form.Unarmed 是复制的标签，两端读得一致。
	bPunchMode = PunchMontage != nullptr && ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed);

	UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] PerformKick 执行派生（形态=%s 权威=%d）"),
		bPunchMode ? TEXT("空手直拳") : TEXT("飞踢"), Avatar->HasAuthority() ? 1 : 0);

	// 踢蒙太奇（两端都播，纯表现）。播放速率【延后到算出飞行耗时之后再定】——
	// 见下面 ApplyKickMontageRate：蒙太奇是固定长度的，而飞行耗时随距离变，
	// 不缩放就会出现"出脚动作已播完、人还在空中飘"，落地接触帧对不上 ⇒ 观感是"没劲"。
	// 所以这里不能直接 PlayAnimMontage，得等 LaunchCharacter 之后的飞行数据。
	float ExpectedFlightTime = 0.f;

	// 立刻关掉派生窗口（不能再踢/二段），但能力先不结束 —— 等飞行结束踢中再收。
	// 踢是普通攻击，不吃 State.EmpoweredAttack —— 隐身中 dodge 后，破隐强化（State.EmpoweredAttack）
	// 保留给下一次普通普攻（GA_ThreeHitPassive）消耗，不在这里消耗。
	ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_Dodge_Active);
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(KickWindowTimer);
	}

	// 权威端：挂脚上拖尾 cue（多播）→ 找目标 → 飞向目标 → 飞行结束踢中结算。
	if (Avatar->HasAuthority())
	{
		FVector KickDir = AHeroCombatCharacter::ResolveAimDirection(Avatar, /*bIncludePitch=*/true);   // 飞向准心方向（含俯仰，用于索敌的角度判据）
		AActor* Target = FindKickTarget(KickDir);

		// 拖尾 / 气流的朝向用【水平方向】而不是飞行方向。
		//
		// 【为什么】飞行方向在俯视时会朝下（高空 pitch 能到 73°~79°），特效跟着它喷就是
		// "往下喷" —— 观感是垂直下坠，正好是我们要消灭的那种"飘"。强制水平之后，
		// 高空下砸时拖尾仍朝前拉出残影，读起来是"在冲"而不是"在掉"。
		// 方向本身由 CueParams 带出去，速度大小用 FlySpeed 另带一个参数（长度/浓度）。
		FVector FlyDir2D = KickDir.GetSafeNormal2D();
		if (FlyDir2D.IsNearlyZero())
		{
			FlyDir2D = Avatar->GetActorForwardVector().GetSafeNormal2D();
		}
		// 注意：方向取自【吸附之后】的 KickDir（FindKickTarget 会改写它），所以必须在它之后算。

		FVector FlyImpulse = FVector::ZeroVector;
		float DiveSpeed = 0.f;
		if (Target)
		{
			KickTarget = Target;

			// ------------------------------------------------------------------
			// 飞行冲量 = 【水平】满值 + 【竖直】恒定落地速度，两个分量互相独立。
			//
			// 旧模型是 KickDir * KickFlyImpulse，水平分量 = 冲量 × cos(pitch)，
			// 高空时 pitch≈79° ⇒ 水平只剩 18%，横向位移在鸟瞰视角下几乎看不见。
			// 见 GA_Dodge.h 里那段的完整推导。
			//
			// 竖直改用"落地所需速度"做下限：目标越远（越高），初速自动抬高，
			// 使得到达目标那一刻的竖直速度【恒定】。好处：
			//   ① 力量感和高度解耦（低空/高空耗时差从 5 倍压到约 2 倍）
			//   ② 顺带封死穿地（冲刺速度不会超过落地速度）
			//   ③ 下砸距离被竖直分量吃掉 ⇒ 剩下给水平飞的净空更短 ⇒ 飞行整体更快
			// ------------------------------------------------------------------
			if (ACharacter* Character = Cast<ACharacter>(Avatar))
			{
				// 需要下砸的高度 = 我比目标高多少（同一个水平位置）。
				// 目标在我上方时不需要额外下砸，走 KickDiveSpeed 下限即可。
				const float HeightToCover = FMath::Max(0.f,
					Avatar->GetActorLocation().Z - Target->GetActorLocation().Z);

				// 从高度 H 砸到地面所需速度：v = sqrt(2gH)。g 取世界重力（含 GravityScale 更准，
				// 但那要把 CMC 拖进来；这里用名义值 980 够用，因为这只是"初速下限"）。
				constexpr float NominalGravityZ = 980.f;
				const float DescentSpeed = FMath::Sqrt(2.f * NominalGravityZ * HeightToCover);

				DiveSpeed = FMath::Max(KickDiveSpeed, DescentSpeed);
				FlyImpulse = FlyDir2D * KickFlyImpulse - FVector::UpVector * DiveSpeed;

				Character->LaunchCharacter(FlyImpulse, /*bXYOverride=*/true, /*bZOverride=*/true);
			}

			UE_LOG(LogTemp, Warning,
				TEXT("[DodgeKick] 冲量分离：水平=(%.0f,%.0f) %.0fcm/s 下砸 %.0fcm/s（未吸附前方向 pitch %.1f°）"),
				FlyDir2D.X, FlyDir2D.Y, KickFlyImpulse, DiveSpeed,
				FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(KickDir.Z, -1.f, 1.f))));

			// 预估飞到目标要多久 —— 只用来定蒙太奇播放速率（见 ApplyKickMontageRate），
			// 判定本身仍然走 TickKickFlight 的实际距离检查，两者互不依赖。
			ExpectedFlightTime = EstimateKickFlightTime(Avatar, Target, FlyImpulse);

			// 飞行中每 KickHitCheckInterval 检查一次「够到目标了没」：够近就结算，超时就扑空。
			// 不用「固定时长后结算」的原因是射程 300~1200cm 之间变 —— 定死时长会让中远距离隔空打人。
			KickFlightStartTime = GetWorld()->GetTimeSeconds();
			GetWorld()->GetTimerManager().SetTimer(KickHitTimer, this, &ThisClass::TickKickFlight, KickHitCheckInterval, /*bLoop=*/true);

			// 脚上拖尾 / 气流 cue：只在【真的起飞了】的这条分支里挂（见 else 分支的说明）。
			// 附在 ChargeCue 上（同 an actor cue 跨「充能 → 飞行」两段存在，靠 ChargeLeadTime 换挡），
			// 这样脚部特效不会在换挡时闪断。
			// ★ 空手直拳不挂：脚部聚能/彗星拖尾是"脚"的语言（GC_DodgeKick 里是腿部发光 + 脚焰），
			//   拳的飞行表现将来走自己的 cue（见 PunchHitCueTag 的注释），先干干净净地飞。
			if (!bPunchMode)
			{
				StartKickTrail(Cast<ACharacter>(Avatar), FlyDir2D, FlyImpulse.Size());
			}
		}
		else
		{
			// 没踢到目标：滑翔一下 + 起手充能的表现收掉，能力结束。
			//
			// ★ 这里【不能】先 Remove 再在外面 Add —— 旧写法就是那样，结果 cue 被挂在
			//   一个已经结束的能力上：没有飞行、没启动 KickHitTimer，TickKickFlight 永远不跑
			//   ⇒ 既不会命中也不会超时扑空 ⇒ 没有任何一条路径会来摘它。
			//   EndAbility 里那次 Remove 只在【被打断/取消】时执行，正常走完不执行。
			//   于是 GC_DodgeKick actor 永久留在世界（bAutoDestroyOnRemove=true 也没用），
			//   表现就是"没锁到人时特效一直挂着不消失"。
			//   飞行段 / 拖尾的 cue 现在只在 if (Target) 分支里挂 —— 没有飞行就没有拖尾。
			CancelKickCharge(Cast<ACharacter>(Avatar));
			EndDodge();
		}
	}

	// 蒙太奇统一在这里播：两端都播（纯表现），且已经知道预估飞行耗时（权威端才有值，
	// 模拟端拿 0 → 走原速，不影响观感）。
	ApplyKickMontageRate(Avatar, ExpectedFlightTime);
}

void UGA_Dodge::TickKickFlight()
{
	if (bKickSettled)
	{
		return;
	}

	UWorld* World = GetWorld();
	const AActor* Avatar = GetAvatarActorFromActorInfo();
	AActor* Target = KickTarget.Get();
	if (!World || !Avatar)
	{
		OnKickWhiff();
		return;
	}
	if (!Target)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 飞行中目标失效（死了/销毁了）→ 扑空收尾"));
		OnKickWhiff();
		return;
	}

	// 判定用的是【胶囊表面】距离，不是原点距离：原点距离在「上下叠着」时要减去两人半高才对，
	// 直接用原点距离会让低空踢永远判不到。
	const float Gap = CapsuleSurfaceDistance(Avatar, Target);
	if (Gap <= KickHitMargin)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 踢中：飞行 %.2fs，胶囊表面距离 %.1fcm ≤ 余量 %.0fcm"),
			World->GetTimeSeconds() - KickFlightStartTime, Gap, KickHitMargin);
		OnKickHit();
		return;
	}

	const float FlightTime = World->GetTimeSeconds() - KickFlightStartTime;
	if (FlightTime >= KickMaxFlightTime)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 飞行 %.2fs 超时（还差 %.0fcm）→ 扑空收尾，不结算伤害"),
			FlightTime, Gap);
		OnKickWhiff();
	}
}

void UGA_Dodge::OnKickHit()
{
	bKickSettled = true;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(KickHitTimer);
	}

	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	AActor* Target = KickTarget.Get();
	AActor* Attacker = GetAvatarActorFromActorInfo();
	if (Target && ASC)
	{
		// 命中点 / 法线：算「脚落在目标胶囊表面的哪儿」，而不是拿目标原点。
		// 飞踢自上而下砸，原点连线中点会飘在空中 —— 粒子炸在空气里像打空了（见该函数注释）。
		FVector ImpactPoint = Target->GetActorLocation();
		FVector ImpactNormal = FVector::ZeroVector;
		ComputeImpactPointAndNormal(Attacker, Target, ImpactPoint, ImpactNormal);

		// 命中 cue：直拳和飞踢分开（GC_DodgePunchHit / GC_DodgeKickHit），
		// 拳和脚的特效语言不同（见 GA_Dodge.h 的 PunchHitCueTag 注释）。没配就退回踢的 cue。
		const FGameplayTag HitCueTag = (bPunchMode && PunchHitCueTag.IsValid())
			? PunchHitCueTag
			: LOLGameplayTags::GameplayCue_DodgeKickHit;

		// 踢中命中 cue（多播，各端都看到命中粒子 + 各端本地抖自己的镜头）。
		FGameplayCueParameters CueParams;
		CueParams.Location = ImpactPoint;
		CueParams.Normal = ImpactNormal;
		CueParams.Instigator = Attacker;
		CueParams.EffectCauser = Attacker;
		// 用 RawMagnitude 传【落地速度】：让高空砸下来的命中特效/环比贴地铲的更猛
		// （GC_DodgeKickHit 拿它算 0~1 的强度）。取攻击者当前竖直速度，拿不到就退回名义值。
		{
			const ACharacter* AttackerChar = Cast<ACharacter>(Attacker);
			const float VerticalSpeed = AttackerChar
				? FMath::Abs(AttackerChar->GetVelocity().Z)
				: KickDiveSpeed;
			CueParams.RawMagnitude = FMath::Max(VerticalSpeed, KickDiveSpeed);
		}
		ASC->ExecuteGameplayCue(HitCueTag, CueParams);

		// 先摆方向再施加击退。理由：角色的 CMC 是 bOrientRotationToMovement=true + RotationRate.Yaw=500，
		// 这个"朝向"平时由【移动输入】驱动 —— 站着不动的目标朝向随缘（可能正背对攻击者）。
		// 而击退是会平移的：不先转过去，被砸出去的人就是"横向平移的高清贴图"，从哪个角度看都不对，
		// 飞踢那一下的力量感差一大截。这一步只在权威端做（旋转会复制）。
		//
		// 朝向用「从攻击者指向目标的水平方向」= 目标被推出去的方向（和 UGEComponent_Knockback
		// 里 Direction 的兜底②同源）。上下叠着时水平差≈0 → GetSafeNormal2D 给零向量，
		// 那就退回攻击者的水平朝向（和组件的兜底③保持一致，两边别用不同的方向）。
		if (Attacker && Target && Target->HasAuthority())
		{
			FVector AwayDir = (Target->GetActorLocation() - Attacker->GetActorLocation()).GetSafeNormal2D();
			if (AwayDir.IsNearlyZero())
			{
				AwayDir = Attacker->GetActorForwardVector().GetSafeNormal2D();
			}
			if (!AwayDir.IsNearlyZero())
			{
				Target->SetActorRotation(AwayDir.Rotation());
			}
		}

		// 伤害 + 击退 GE（State.Knockback 硬直 + 打断 + 位移）。
		ApplyKickDamage(Target);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] OnKickHit：目标已失效，无结算"));
	}

	// 收拖尾 + 结束（EndDodge → EndAbility 里也会收，这里显式收一次避免中间帧还挂着拖尾）。
	if (ASC)
	{
		ASC->RemoveGameplayCue(LOLGameplayTags::GameplayCue_DodgeKick);
	}
	EndDodge();
}

void UGA_Dodge::OnKickWhiff()
{
	bKickSettled = true;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(KickHitTimer);
	}

	// 扑空表现：在「脚扫过去的地方」放一下（风声 / 尘土）。不打伤害、不改状态，纯表现。
	// 走 cue 的理由和命中一样：多播到各端 + 素材能在 BP 里换。标签没配就什么都不做。
	if (WhiffCueTag.IsValid())
	{
		if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
		{
			AActor* Avatar = GetAvatarActorFromActorInfo();
			if (Avatar)
			{
				FGameplayCueParameters CueParams;
				CueParams.Location = Avatar->GetActorTransform().TransformPosition(WhiffCueOffset);
				CueParams.Normal = Avatar->GetActorForwardVector();
				CueParams.Instigator = Avatar;
				CueParams.EffectCauser = Avatar;
				ASC->ExecuteGameplayCue(WhiffCueTag, CueParams);
			}
		}
	}

	// 扑空只是没打中：不结算伤害、不施加击退。收尾（收拖尾 cue、摘标签、结束能力）交给 EndDodge。
	EndDodge();
}

void UGA_Dodge::StartKickTrail(ACharacter* Character, const FVector& FlyDir2D, float FlySpeed)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!ASC || !Character)
	{
		return;
	}

	// cue 绑在【角色】上（不是脚上）：脚部 socket 在 GC 里按 TrailSocket 自己找，
	// 这样 GC 的 BP 换 socket 名字就能换装饰部位，不用改 C++。
	FGameplayCueParameters CueParams;
	CueParams.Instigator = Character;
	CueParams.EffectCauser = Character;
	CueParams.Location = Character->GetActorLocation();
	CueParams.Normal = FlyDir2D;          // → GC 的 User.FlyDir（拖尾/气流的朝向，已强制水平）
	CueParams.RawMagnitude = FlySpeed;    // → GC 的 User.FlySpeed（cm/s，用来调浓淡/长度）

	// ★【这里】才是整个飞踢唯一的 Add。旧代码有两处 Add（一处在这个位置、一处无条件跟在
	//   if/else 之后），后者会在"没锁到目标"时把 cue 挂到一个已经结束的能力上 —— 表现就是
	//   脚部特效永久留在世界里。现在 Add 只在"真的起飞了"的这条路径上，一一对应。
	ASC->AddGameplayCue(LOLGameplayTags::GameplayCue_DodgeKick, CueParams);

	UE_LOG(LogTemp, Log, TEXT("[DodgeKick] 起手充能/拖尾 cue 已挂（方向 %.2f,%.2f 速度 %.0fcm/s 充能 %.2fs）"),
		FlyDir2D.X, FlyDir2D.Y, FlySpeed, KickChargeLeadTime);
}

void UGA_Dodge::CancelKickCharge(ACharacter* Character)
{
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		// RemoveGameplayCue 幂等（没挂过时是 no-op），所以这里不必先查。
		// 对应的 Add 只在 StartKickTrail 里 —— 本函数就是它唯一配对的摘除点（扑空路径）。
		ASC->RemoveGameplayCue(LOLGameplayTags::GameplayCue_DodgeKick);
	}
}

float UGA_Dodge::ComputeAcquireLength(const ACharacter* Character, const FVector& Start) const
{
	// 贴地：和以前完全一样（KickRange），不去动地面上的手感。
	const UCharacterMovementComponent* Move = Character ? Character->GetCharacterMovement() : nullptr;
	if (!Move || !Move->IsFalling())
	{
		return KickRange;
	}

	const float MaxLength = FMath::Max(KickRange, KickAcquireRange);

	// 从脚下打到地面，量出离地高度（用 ImpactPoint 的 Z 差，别用 Hit.Distance：直线向下时两者等价，
	// 但 Z 差是肉眼可验的）。
	FCollisionQueryParams GroundParams(SCENE_QUERY_STAT(DodgeKickGround), false, Character);
	FHitResult GroundHit;
	const bool bHitGround = GetWorld()->LineTraceSingleByChannel(
		GroundHit, Start, Start - FVector::UpVector * MaxLength, ECC_Visibility, GroundParams);
	const float Height = bHitGround ? FMath::Max(0.f, Start.Z - GroundHit.ImpactPoint.Z) : MaxLength;

	// 容差是个半角 KickAimAssistAngle 的锥，它在高度 H 处和地面相交的距离是 H / cos(半角)；
	// 再加一个 KickRadius（命中球的半径），就是「把正下方那片地面圈进索敌球所需的半径」。
	const float HalfAngle = FMath::DegreesToRadians(FMath::Clamp(KickAimAssistAngle, 0.f, 80.f));
	const float Reach = Height / FMath::Cos(HalfAngle) + KickRadius;
	return FMath::Clamp(Reach, KickRange, MaxLength);
}

float UGA_Dodge::EstimateKickFlightTime(const AActor* Avatar, const AActor* Target, const FVector& FlyImpulse) const
{
	// 只用来定蒙太奇播放速率的【估算】，不参与任何判定（判定是 TickKickFlight 的实际距离检查）。
	// 所以不用把轨迹积到"撞上胶囊"那么精确，用"到目标还差多少距离 ÷ 初速"再按加速度修一下就够。
	if (!Avatar || !Target || FlyImpulse.IsNearlyZero())
	{
		return KickMontageRefFlightTime;
	}

	constexpr float NominalGravityZ = 980.f;

	// 需要飞过的距离：从「我」到「目标胶囊表面」，不是到目标原点 ——
	// 用原点会让估算偏长（尤其贴脸时差着两个胶囊半径），而这里要的正是"什么时候撞上"。
	const float Distance = FMath::Max(0.f, CapsuleSurfaceDistance(Avatar, Target));

	// 沿飞行方向做一维运动：v(t) = |v0| + g·t·cosθ（θ = 飞行方向与竖直的夹角，
	// 竖直向下为正 ⇒ 重力在飞行方向上的分量是加速还是减速取决于 θ）。
	// 取 cosθ 的绝对值近似：下砸时竖直初速向下、重力同向 = 加速；平飞时重力把轨迹压弯、
	// 对"沿直线飞到目标"这件事是减速。两种情况都能用同一式近似，精度足够定速率。
	const float LaunchSpeed = FlyImpulse.Size();
	const float CosTheta = FMath::Abs(FlyImpulse.Z) / FMath::Max(LaunchSpeed, 1.f);
	const float AlongAccel = NominalGravityZ * CosTheta;

	// |v0|t + ½·a·t² = D 的正根。
	const float Disc = LaunchSpeed * LaunchSpeed + 2.f * AlongAccel * Distance;
	const float T = AlongAccel > KINDA_SMALL_NUMBER
		? (-LaunchSpeed + FMath::Sqrt(Disc)) / AlongAccel
		: Distance / FMath::Max(LaunchSpeed, 1.f);

	return FMath::Max(T, 0.01f);
}

void UGA_Dodge::ApplyKickMontageRate(AActor* Avatar, float ExpectedFlightTime)
{
	// 直拳 / 飞踢各用自己的蒙太奇和接触帧基准（空手派生分支见 PerformKick 开头）。
	UAnimMontage* Montage = bPunchMode && PunchMontage ? PunchMontage : KickMontage;
	const float RefFlightTime = (bPunchMode && PunchMontageRefFlightTime > KINDA_SMALL_NUMBER)
		? PunchMontageRefFlightTime
		: KickMontageRefFlightTime;

	if (!Montage || !Avatar)
	{
		return;
	}

	// 权威端算出了预估耗时，但【蒙太奇是各端各自播的】（纯表现，不复制）——
	// 模拟端拿不到这个值（ExpectedFlightTime 传 0）。为了各端看起来一致，
	// 模拟端也按同一套公式自己估一次：它知道自己的位置和目标位置（目标会复制）。
	float FlightTime = ExpectedFlightTime;
	if (FlightTime <= 0.f)
	{
		// 模拟端：拿当前速度方向和速度大小反推（自己也是被 LaunchCharacter 推的，速度会复制）。
		const ACharacter* Character = Cast<ACharacter>(Avatar);
		const AActor* Target = KickTarget.Get();
		const FVector Velocity = Character ? Character->GetVelocity() : FVector::ZeroVector;
		if (Target && !Velocity.IsNearlyZero())
		{
			FlightTime = EstimateKickFlightTime(Avatar, Target, Velocity);
		}
		else
		{
			// 连速度都还没复制到 → 原速。宁可原速也别乱缩。
			FlightTime = RefFlightTime;
		}
	}

	// 速率 = 基准耗时 / 实际耗时。基准填 0 或没配 = 关闭缩放（退回固定原速的旧行为）。
	float Rate = 1.f;
	if (RefFlightTime > KINDA_SMALL_NUMBER && FlightTime > KINDA_SMALL_NUMBER)
	{
		Rate = FMath::Clamp(RefFlightTime / FlightTime, KickMontageMinRate, KickMontageMaxRate);
	}

	if (ACharacter* Character = Cast<ACharacter>(Avatar))
	{
		Character->PlayAnimMontage(Montage, Rate);
	}

	UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] %s 蒙太奇速率 %.2f（预估飞行 %.3fs，基准 %.2fs，夹在[%.2f,%.2f]）"),
		bPunchMode ? TEXT("直拳") : TEXT("踢"), Rate, FlightTime, RefFlightTime, KickMontageMinRate, KickMontageMaxRate);
}

AActor* UGA_Dodge::FindKickTarget(FVector& OutDirection) const
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character)
	{
		return nullptr;
	}

	const FVector Start = Character->GetActorLocation();
	// 索敌基准方向 = 准心方向（含俯仰），由 PerformKick 传入（从上而下踢 = 准心朝下看）。
	const FVector AimDir = OutDirection.GetSafeNormal();

	// 索敌半径（原来这里写死 KickRange，是「飞高了踢不到人」的根因）。
	const float AcquireLength = ComputeAcquireLength(Character, Start);

	// 索敌 = 【先捞后筛】：
	//   ① 粗筛：以角色为中心、半径 AcquireLength 的球重叠，捞出范围内所有胶囊；
	//   ② 细筛：只保留「有 ASC 的别人」，算「角色→目标」和准心方向的夹角，
	//      夹角 ≤ KickAimAssistAngle 的进容差锥，再按打分挑一个（吸附时 OutDirection 指向它）。
	//
	// 【为什么不再用「沿准心扫一根细管」】
	// 细管能扫到谁，取决于玩家把准心的【方位角】压到多准：命中半径 KickRadius(90) 固定，
	// 于是允许的方位偏差是 ±atan(90 / 距离) —— 距离 250cm 时 ±20°（低空够用），
	// 距离 500cm 时就只剩 ±10°。高空飞踢时目标基本在角色正下方附近，而准心（跟随相机，
	// 400cm 越肩）指不到那么正下方，细管擦着目标过去 ⇒ 一个胶囊都扫不到 ⇒
	// KickAimAssistAngle 这个 30° 容差【完全没有机会生效】。
	// 日志里的证据：低空（Z≈450~520）能锁到 178~245cm 的目标；高空（Z≈600~740）
	// 直接报「扫不到任何胶囊」。容差必须作用在候选集上，而不是「已经扫中的东西」上。
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DodgeKick), false, Character);
	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByChannel(
		Overlaps, Start, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(AcquireLength), Params);

	AActor* Best = nullptr;
	float BestScore = TNumericLimits<float>::Max();
	float BestAngle = 0.f;
	float BestDist = 0.f;

	// 被刷掉的原因要分开记：只打一句「都不合条件」的话，
	// 「瞄歪了」和「范围内压根没人」在日志里长得一模一样，没法定位。
	AActor* NearestOutside = nullptr;            // 范围里有、但角度超容差的最近那个（= 玩家瞄歪了）
	float NearestOutsideAngle = TNumericLimits<float>::Max();
	float NearestOutsideDist = 0.f;
	int32 SkippedNoASC = 0;                      // 范围里但没有 ASC 的（场景物件 / 投射物等）
	int32 OutsideCone = 0;
	int32 InCone = 0;

	// 同一个胶囊可能被重叠结果报到多次（多组件 / 多次接触），按 Actor 去重。
	TSet<const AActor*> Seen;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Target = Overlap.GetActor();
		if (!Target || Target == Character || Seen.Contains(Target))
		{
			continue;
		}
		Seen.Add(Target);

		if (!UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target))
		{
			++SkippedNoASC;
			continue;
		}

		const FVector ToTarget = Target->GetActorLocation() - Start;
		const float Dist = ToTarget.Size();

		// 角度差 = 准心方向 与「角色→目标」的夹角；容差锥就是它的半角。
		//
		// 【判角度用 3D 方向，不用水平投影】目标常在正下方，那时"角色→目标"几乎全在竖直上，
		// 水平投影接近零向量，角度会因为数值噪声在 0°/90° 之间乱跳 —— 容差锥直接失效。
		// 而准心（跟随相机、含俯仰）本来就指得到斜下方，3D 夹角才是"玩家有没有瞄向那个人"
		// 的正确度量。
		const float Angle = FMath::RadiansToDegrees(
			FMath::Acos(FMath::Clamp(FVector::DotProduct(AimDir, ToTarget.GetSafeNormal()), -1.f, 1.f)));

		if (Angle > KickAimAssistAngle)
		{
			++OutsideCone;
			if (Angle < NearestOutsideAngle)
			{
				NearestOutside = Target;
				NearestOutsideAngle = Angle;
				NearestOutsideDist = Dist;
			}
			continue;
		}

		++InCone;
		// 打分：距离 × (1 + 角度占比 × KickTargetAngleWeight)。
		// 角度越大越吃亏，但近的目标仍然占优 —— 既不「放着脚下的人不踢去踢远处准心上的」，
		// 也不「明明瞄着谁却踢了旁边那个」。
		const float AngleRatio = KickAimAssistAngle > KINDA_SMALL_NUMBER
			? (Angle / KickAimAssistAngle) : 0.f;
		const float Score = Dist * (1.f + AngleRatio * KickTargetAngleWeight);
		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Target;
			BestAngle = Angle;
			BestDist = Dist;
			OutDirection = ToTarget.GetSafeNormal();   // 吸附：飞向目标
		}
	}

	if (Best)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 索敌：半径=%.0f（贴地 %.0f / 上限 %.0f）候选 %d 个（锥内 %d / 锥外 %d / 无 ASC %d）→ 锁定 %s（距离 %.0f 偏差 %.1f° 分 %.0f）"),
			AcquireLength, KickRange, KickAcquireRange, Overlaps.Num(), InCone, OutsideCone, SkippedNoASC,
			*GetNameSafe(Best), BestDist, BestAngle, BestScore);
	}
	else if (NearestOutside)
	{
		// 最常见的失败：范围里有敌人，但准心偏得太多（高空俯视时尤其容易 —— 相机在角色身后 400cm，
		// 准心指的方向和「角色到目标」的方向差着一段视差）。
		UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 索敌：半径=%.0f（贴地 %.0f / 上限 %.0f）候选 %d 个（锥内 0 / 锥外 %d / 无 ASC %d）→ 没目标：【瞄歪了】最近的是 %s（距离 %.0f）偏 %.1f° 超过容差 %.0f°"),
			AcquireLength, KickRange, KickAcquireRange, Overlaps.Num(), OutsideCone, SkippedNoASC,
			*GetNameSafe(NearestOutside), NearestOutsideDist, NearestOutsideAngle, KickAimAssistAngle);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 索敌：半径=%.0f（贴地 %.0f / 上限 %.0f）候选 %d 个（无 ASC %d）→ 没目标：范围内没有可锁定的人"),
			AcquireLength, KickRange, KickAcquireRange, Overlaps.Num(), SkippedNoASC);
	}

	return Best;
}

void UGA_Dodge::ApplyKickDamage(AActor* Target)
{
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
	if (!SourceASC || !TargetASC || !DamageEffect)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Dodge] 踢的目标 %s 没有 ASC（或 DamageEffect 没配）→ 无伤害"),
			*GetNameSafe(Target));
		return;
	}

	// 和 GA_ThreeHitPassive::ApplyServerHit 同一套：GE_Damage + ExecCalc 唯一结算点。
	// MakeEffectContext 已经把 instigator 设成施加者（PlayerState + 角色），不用再手动 AddInstigator。
	FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
	FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(DamageEffect, GetAbilityLevel(), Context);
	if (Spec.IsValid())
	{
		Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, KickDamageMultiplier);
		Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, 0.f);
		Spec.Data->AddDynamicAssetTag(LOLGameplayTags::Damage_Physical);
		SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
	}

	// 击退：走 GE_Knockback（和 GA_ThreeHitPassive 的强化击退同一条路），位移也交给它的 GE 组件，
	// 技能里不再自己写 LaunchCharacter。
	//   Data.KnockbackDuration = 硬直时长
	//   Data.KnockbackImpulse  = 水平推力（0 = 不推，目标只在原地上下弹一下）
	//   Data.KnockbackLaunch   = 竖直冲量，正数上弹 / 负数下砸
	// 顺带拿到：State.Knockback（硬控 → 基类 ActivationBlockedTags 挡掉目标一切技能含普攻，
	// ALOLCharacter::DoMove 也靠它拦住移动输入）、打断目标正在放的技能、GC_Knockback 蒙太奇。
	//
	// 【下砸量跟着落地速度走】KickKnockbackVertical 是"贴地铲一脚"的基准值；从高处砸下来的
	// 时候把攻击者真实的竖直速度拿来放大它，让"从 8 米砸下来"和"贴地铲一脚"在被击退强度上
	// 也有区别（不只是特效更大）。倍率夹在 [1, KickKnockbackHeightBonusMax]，
	// 下限为 1 保证贴地时不会比配置值更弱。
	if (KnockbackGE)
	{
		// 竖直分量按形态分叉：飞踢是负数（往地上砸），直拳是正数（往天上弹）。
		// 下砸高度加成只对"砸"成立 —— 拳没有"砸得更狠"一说，不做放大。
		const float BaseVertical = bPunchMode ? PunchKnockbackVertical : KickKnockbackVertical;
		float KnockbackScale = 1.f;
		if (!bPunchMode)
		{
			if (const ACharacter* AttackerChar = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
			{
				const float FallSpeed = FMath::Abs(AttackerChar->GetVelocity().Z);
				if (KickDiveSpeed > KINDA_SMALL_NUMBER && FallSpeed > KickDiveSpeed)
				{
					KnockbackScale = FMath::Min(FallSpeed / KickDiveSpeed, KickKnockbackHeightBonusMax);
				}
			}
		}

		FGameplayEffectContextHandle KBContext = SourceASC->MakeEffectContext();
		FGameplayEffectSpecHandle KBSpec = SourceASC->MakeOutgoingSpec(KnockbackGE, GetAbilityLevel(), KBContext);
		if (KBSpec.IsValid())
		{
			KBSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackDuration, KickStunDuration);
			KBSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackImpulse, KickKnockbackHorizontal);
			KBSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackLaunch, BaseVertical * KnockbackScale);
			SourceASC->ApplyGameplayEffectSpecToTarget(*KBSpec.Data.Get(), TargetASC);

			UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] 击退（%s）：水平 %.0f 竖直 %.0f（下砸倍率 %.2f）硬直 %.2fs"),
				bPunchMode ? TEXT("直拳") : TEXT("飞踢"),
				KickKnockbackHorizontal, BaseVertical * KnockbackScale, KnockbackScale, KickStunDuration);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[DodgeKick] KnockbackGE(%s) 的 Spec 无效 → 踢中只有伤害、没击退硬直"),
				*GetNameSafe(KnockbackGE));
		}
	}
}

void UGA_Dodge::EndDodge()
{
	// 摘派生窗口标签（本地那端），清定时器，结束能力。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_Dodge_Active);
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(KickWindowTimer);
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}
