// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DodgeKick.h"
#include "GAS/LOLGameplayTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"

AGC_DodgeKick::AGC_DodgeKick()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_DodgeKick;

	// 拖尾要跨「进入飞踢 → 踢中」存在，移除时自动销毁本 cue actor。
	bAutoDestroyOnRemove = true;

	// 不改角色 transform，只是把 Niagara 挂到脚上，不需要附着到 owner。
	bAutoAttachToOwner = false;
}

bool AGC_DodgeKick::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// ★ 幂等门：客户端上 OnActive 与 WhileActive 会【都】被派发（RPC_InvokeGameplayCueAdded
	//   派 OnActive，cue 容器 PostReplicatedAdd 派 WhileActive），两者各自独立的去重标志
	//   互不拦截。不加这个门时，第二次进来会再 spawn 一套组件并覆盖下面的指针 ——
	//   第一套泄漏：bAutoDestroy=false + 发射器无限循环 = 永远发光，OnRemove 只能销毁第二套。
	//   （主机端只派发 WhileActive，所以主机没事、客户端必泄漏 —— 与"不消失只在客户端看到"吻合。）
	if (ChargeComponent || TrailComponent || AirFlowComponent || LegComponent)
	{
		return true;
	}

	if (!IsValid(MyTarget))
	{
		return true;
	}

	const ACharacter* Character = Cast<ACharacter>(MyTarget);
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (!Mesh)
	{
		// 不是角色（理论上不会）：没地方挂，当「没表现」处理，不算失败。
		return true;
	}

	// 飞行方向由 GA_Dodge 通过 Normal 传进来（起踢时的【水平】飞行方向）。
	// 不传也能跑：气流会朝世界前方，拖尾完全不受影响。
	const FVector FlyDir = Parameters.Normal.IsNearlyZero()
		? MyTarget->GetActorForwardVector().GetSafeNormal2D()
		: Parameters.Normal.GetSafeNormal2D();
	const FRotator FlyRot = FlyDir.Rotation();

	// 实际飞行速度（cm/s）由 GA_Dodge 用 RawMagnitude 带进来（= 水平与下砸的合速度）。
	// 归一化成 0~1 的权重喂给 NS：高空下砸时合速度更大 → 拖尾更长，"更猛"。
	// 没传（0）时给 1.0，等于"一个参考速度"，不会让参数变成 0 把特效关掉。
	const float FlySpeed = Parameters.RawMagnitude > 0.f ? Parameters.RawMagnitude : FlySpeedRef;
	const float SpeedWeight = FMath::Clamp(FlySpeed / FMath::Max(FlySpeedRef, 1.f), 0.f, 1.f);

	// ---- 按飞行方向挑「踢出去的那只脚」（拖尾 / 小腿发光共用）-----------
	// 判定用【飞行方向 vs 角色朝向】的前后点积，和 GA_Dodge 里挑 Fwd/Bwd 蒙太奇一致。
	// 脚的插槽（foot_r/l）与小腿的插槽（calf_r/l）用同一个前后判定，保证在同一侧腿上。
	const bool bBackKick = bSwitchFootByDirection &&
		FVector::DotProduct(MyTarget->GetActorForwardVector().GetSafeNormal2D(), FlyDir) < 0.f;

	// ---- ⓪ 起手充能（雷欧飞踢的第一段）--------------------------------
	// 有充能素材 + ChargeLeadTime > 0 才走"两段式"；否则拖尾直接全开（旧行为）。
	// ★ 这条门决定了下面①②③的 bAutoActivate：要换挡就必须"先 spawn 好但不激活"。
	const bool bUseCharge =
		ChargeLeadTime > 0.f && !FootChargeNiagara.IsNull();

	// ---- ① 脚部拖尾 ----------------------------------------------------
	if (UNiagaraSystem* Trail = TrailNiagara.LoadSynchronous())
	{
		// 插槽名没配 / 名字写错时退回 mesh 根：SpawnSystemAttached 对不存在的插槽会附着到组件原点，
		// 还能用（只是不跟脚摆），比整条拖尾消失好。
		FName SocketName = bBackKick ? TrailSocketBack : TrailSocket;
		if (SocketName.IsNone())
		{
			SocketName = NAME_None;
		}

		// 【关键】两段式时拖尾"附着但先不激活"：到点只需 SetActive(true)，不会有生成延迟造成的闪断。
		TrailComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
			Trail, Mesh, SocketName, FVector::ZeroVector, FRotator::ZeroRotator,
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false, /*bAutoActivate=*/!bUseCharge);

		if (TrailComponent)
		{
			TrailComponent->SetFloatParameter(TrailScaleParameter, TrailScale);
			TrailComponent->SetFloatParameter(FlySpeedParameter, SpeedWeight);

			if (bUseCharge)
			{
				// 先停：SetActive(false) 会 Reset 系统（清掉已经跑了几帧的粒子），
				// 所以换挡时打开的是"从头开始"的拖尾，不会看到一个跑到一半的残影。
				TrailComponent->SetActive(false, /*bReset=*/true);
			}
		}
	}

	// ---- ⓪ 生成充能层（放在拖尾之后：换挡要把拖尾打开，先备好更直观）------
	if (bUseCharge)
	{
		if (UNiagaraSystem* Charge = FootChargeNiagara.LoadSynchronous())
		{
			FName ChargeSocketName = bBackKick ? ChargeSocketBack : ChargeSocket;

			ChargeComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
				Charge, Mesh, ChargeSocketName, FVector::ZeroVector, FRotator::ZeroRotator,
				EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false, /*bAutoActivate=*/true);

			if (ChargeComponent)
			{
				ChargeComponent->SetFloatParameter(ChargeScaleParameter, ChargeScale);
				ChargeComponent->SetFloatParameter(FlySpeedParameter, SpeedWeight);

				// 到点换挡。用世界定时器（Actor cue 的生命周期由 GA 的 Remove 控制，
				// 不依赖角色 tick），所以 cue 被提前摘掉时必须 ClearTimer（见 OnRemove）。
				// 用弱 this 是不必要的 —— AActor 的 FTimerManager 回调在 Actor 销毁时会自动失效，
				// 且 OnRemove 里先 ClearTimer 再 DestroyComponent，顺序上是安全的。
				GetWorld()->GetTimerManager().SetTimer(
					ChargeSwapTimer, this, &AGC_DodgeKick::SwapChargeToTrail,
					ChargeLeadTime, /*bLoop=*/false);
			}
		}
	}

	// ---- ② 冲刺气流（可选）----------------------------------------------
	if (UNiagaraSystem* AirFlow = AirFlowNiagara.LoadSynchronous())
	{
		AirFlowComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
			AirFlow, Mesh, AirFlowSocket, FVector::ZeroVector, FlyRot,
			EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false, /*bAutoActivate=*/!bUseCharge);

		if (AirFlowComponent)
		{
			AirFlowComponent->SetVectorParameter(FlyDirectionParameter, FlyDir);
			AirFlowComponent->SetFloatParameter(FlySpeedParameter, SpeedWeight);

			if (bUseCharge)
			{
				// 气流和拖尾同属"第二段"，一起等换挡。reset 的理由同上。
				AirFlowComponent->SetActive(false, /*bReset=*/true);
			}
		}
	}

	// ---- ③ 小腿发光（可选）----------------------------------------------
	// NS 内的粒子沿骨骼局部 +X（膝→踝）铺发光柱，这里只负责把它挂到正确的那条腿上。
	if (UNiagaraSystem* Leg = LegNiagara.LoadSynchronous())
	{
		LegComponent = UNiagaraFunctionLibrary::SpawnSystemAttached(
			Leg, Mesh, bBackKick ? LegSocketBack : LegSocket, FVector::ZeroVector, FRotator::ZeroRotator,
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false, /*bAutoActivate=*/true);

		if (LegComponent)
		{
			LegComponent->SetFloatParameter(LegScaleParameter, LegScale);
		}
	}

	// 素材没配只是「没表现」，不是失败。
	return true;
}

bool AGC_DodgeKick::WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// 运行时 Add 的 cue 在服务器走 WhileActive，转给 OnActive（否则主机看不到拖尾）。
	return OnActive_Implementation(MyTarget, Parameters);
}

void AGC_DodgeKick::SwapChargeToTrail()
{
	// 充能层下场：起手结束。用 Deactivate 而不是 DestroyComponent ——
	// 系统还在自循环，Deactivate 会让它把已有粒子自然放完（bAllowParticlesToFinish，
	// Deactivate 默认就是"停止发射 + 放完余量"），比硬销毁自然。
	// 组件本身留到 OnRemove 再销毁，免得这一帧还有回调打到空指针上。
	if (ChargeComponent)
	{
		ChargeComponent->Deactivate();
	}

	// 全功率段上场：拖尾 + 气流一起点亮（它们在 OnActive 里已经 spawn 好并附着）。
	if (TrailComponent)
	{
		TrailComponent->SetActive(true, /*bReset=*/false);
	}
	if (AirFlowComponent)
	{
		AirFlowComponent->SetActive(true, /*bReset=*/false);
	}
}

bool AGC_DodgeKick::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// ★ 先清换挡定时器：cue 可能在 ChargeLeadTime 之前就被摘掉（踢中得快 / 被打断），
	//   不清的话定时器会在组件销毁之后再打过来。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChargeSwapTimer);
	}

	if (ChargeComponent)
	{
		ChargeComponent->Deactivate();
		ChargeComponent->DestroyComponent();
		ChargeComponent = nullptr;
	}
	if (TrailComponent)
	{
		TrailComponent->Deactivate();
		TrailComponent->DestroyComponent();
		TrailComponent = nullptr;
	}
	if (AirFlowComponent)
	{
		AirFlowComponent->Deactivate();
		AirFlowComponent->DestroyComponent();
		AirFlowComponent = nullptr;
	}
	if (LegComponent)
	{
		LegComponent->Deactivate();
		LegComponent->DestroyComponent();
		LegComponent = nullptr;
	}
	return true;
}
