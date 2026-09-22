// Copyright Epic Games, Inc. All Rights Reserved.
#include "Animation/AnimNotifyState_BladeTrail.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "TimerManager.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	const TCHAR* GetHandLabel(bool bRight)
	{
		return bRight ? TEXT("右手") : TEXT("左手");
	}

	// 下面两个 helper 名字里的文件前缀是必须的，别改回 HasSocket / DescribeUserParameters：
	// unity build 会把多个 .cpp 合进同一个 Module.LOL.N.cpp，匿名 namespace 只挡【跨 TU】的冲突，
	// 合进一个 TU 之后同签名的函数就是重定义（C2084）。GA_ThreeHitPassive / GC_EmpoweredHit
	// 里各有一份同样的实现，谁跟谁同批是 UBT 按文件名分批决定的，不受这里控制。

	/** 插槽和骨骼都算数：GetSocketLocation 对骨骼名同样有效。 */
	bool BladeTrailHasSocket(const USkeletalMeshComponent* MeshComp, const FName& SocketName)
	{
		return MeshComp->DoesSocketExist(SocketName) || MeshComp->GetBoneIndex(SocketName) != INDEX_NONE;
	}

	/** 把系统里现有的 User.* 参数拼成一行，参数名对不上时直接打出来对照。 */
	FString BladeTrailDescribeUserParameters(const UNiagaraSystem* System)
	{
		TArray<FNiagaraVariable> Parameters;
		System->GetExposedParameters().GetParameters(Parameters);

		TArray<FString> Names;
		for (const FNiagaraVariable& Parameter : Parameters)
		{
			const FString Name = Parameter.GetName().ToString();
			if (Name.StartsWith(TEXT("User.")))
			{
				Names.Add(Name);
			}
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("（一个都没有）"));
	}
}

UAnimNotifyState_BladeTrail::UAnimNotifyState_BladeTrail()
{
	// 默认指向项目里那条系统。用软引用：不赋值就保持默认，编辑器里也能随手换成别的。
	TrailSystem = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(TEXT("/Game/LOL/Niagara/NS_BladeTrail.NS_BladeTrail")));
}

FName UAnimNotifyState_BladeTrail::ResolveSocket(bool bTip, bool bRight) const
{
	if (SocketSet == EBladeTrailSocketSet::Custom)
	{
		if (bTip) { return bRight ? CustomTipSocketRight : CustomTipSocketLeft; }
		return bRight ? CustomBaseSocketRight : CustomBaseSocketLeft;
	}

	if (SocketSet == EBladeTrailSocketSet::ExtensionAB)
	{
		if (bTip) { return bRight ? FName(TEXT("extensionB_r")) : FName(TEXT("extensionB_l")); }
		return bRight ? FName(TEXT("extensionA_r")) : FName(TEXT("extensionA_l"));
	}

	if (bTip) { return bRight ? FName(TEXT("sword_tip_r")) : FName(TEXT("sword_tip_l")); }
	return bRight ? FName(TEXT("sword_base_r")) : FName(TEXT("sword_base_l"));
}

UNiagaraComponent* UAnimNotifyState_BladeTrail::SpawnTrail(USkeletalMeshComponent* MeshComp, UNiagaraSystem* System, bool bRight) const
{
	const TCHAR* Hand = GetHandLabel(bRight);
	const FName BaseSocket = ResolveSocket(/*bTip=*/false, bRight);
	const FName TipSocket = ResolveSocket(/*bTip=*/true, bRight);

	// 插槽不存在时 GetSocketLocation 会静默返回组件位置 —— 拖尾会从角色原点长出来，看不出是配置错的，
	// 必须当场报出来。改 SocketSet，或者切到 Custom 手填名字。
	if (!BladeTrailHasSocket(MeshComp, BaseSocket) || !BladeTrailHasSocket(MeshComp, TipSocket))
	{
		UE_LOG(LogTemp, Warning, TEXT("[BladeTrail] %s：%s 上找不到插槽 %s / %s → 这一侧不生成拖尾"),
			Hand, *GetNameSafe(MeshComp->GetSkeletalMeshAsset()), *BaseSocket.ToString(), *TipSocket.ToString());
		return nullptr;
	}

	// 参数名对不上时 SetVariablePosition 也是静默 no-op（NiagaraComponent.cpp 里会退化成 SetVariableVec3，
	// 名字还是找不到就什么都不做）→ 拖尾会挂在刀上但一动不动，同样看不出原因。
	TArray<FNiagaraVariable> Parameters;
	System->GetExposedParameters().GetParameters(Parameters);
	auto HasParameter = [&Parameters](const FName& ParameterName)
	{
		return Parameters.ContainsByPredicate(
			[&ParameterName](const FNiagaraVariable& Parameter) { return Parameter.GetName() == ParameterName; });
	};
	if (!HasParameter(BaseUserParameter) || !HasParameter(TipUserParameter))
	{
		UE_LOG(LogTemp, Warning, TEXT("[BladeTrail] %s：NS 里没有参数 %s / %s → 坐标喂不进去，拖尾不会动。系统里现有的 user parameter：%s"),
			Hand, *BaseUserParameter.ToString(), *TipUserParameter.ToString(), *BladeTrailDescribeUserParameters(System));
	}

	UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAttached(
		System, MeshComp, NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
		EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/true, /*bAutoActivate=*/true);

	if (!Component)
	{
		// SpawnSystemAttached 被 Niagara 的可扩展性预剔除挡掉时也是返回 nullptr，不报错。
		UE_LOG(LogTemp, Warning, TEXT("[BladeTrail] %s：SpawnSystemAttached 返回空（系统无效，或被 Niagara 预剔除挡掉）"), Hand);
		return nullptr;
	}

	// 生成当帧先喂一次，免得拖尾在组件原点闪一帧再跳到刀上。
	UpdateTrail(Component, MeshComp, bRight);
	return Component;
}

void UAnimNotifyState_BladeTrail::UpdateTrail(UNiagaraComponent* Component, USkeletalMeshComponent* MeshComp, bool bRight) const
{
	if (!Component)
	{
		return;
	}

	const FVector BaseWorld = MeshComp->GetSocketLocation(ResolveSocket(/*bTip=*/false, bRight));
	const FVector TipWorld = MeshComp->GetSocketLocation(ResolveSocket(/*bTip=*/true, bRight));

	// World Space 时 ToLocal 是单位变换，InverseTransformPosition 原样返回；
	// Local Space 时转成 Niagara 组件的局部坐标（组件挂在 mesh 上，于是等于角色网格空间）。
	const FTransform ToLocal = bLocalSpace ? Component->GetComponentTransform() : FTransform::Identity;
	Component->SetVariablePosition(BaseUserParameter, ToLocal.InverseTransformPosition(BaseWorld));
	Component->SetVariablePosition(TipUserParameter, ToLocal.InverseTransformPosition(TipWorld));
}

void UAnimNotifyState_BladeTrail::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	if (!MeshComp)
	{
		return;
	}

	// 角色在 notify 期间被销毁时 NotifyEnd 不一定跑得到，顺手清掉失效的键（弱引用，不会解引用野指针）。
	for (auto It = ActiveTrails.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// 专用服务器没有渲染，不生成。
	UWorld* World = MeshComp->GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	UNiagaraSystem* System = TrailSystem.LoadSynchronous();
	if (!System)
	{
		UE_LOG(LogTemp, Warning, TEXT("[BladeTrail] TrailSystem 没配（或软引用加载失败）→ %s 上不生成拖尾"),
			*GetNameSafe(MeshComp->GetSkeletalMeshAsset()));
		return;
	}

	FActiveTrail& Trail = ActiveTrails.Add(MeshComp);
	if (Side == EBladeTrailSide::Right || Side == EBladeTrailSide::Both)
	{
		Trail.Right = SpawnTrail(MeshComp, System, /*bRight=*/true);
	}
	if (Side == EBladeTrailSide::Left || Side == EBladeTrailSide::Both)
	{
		Trail.Left = SpawnTrail(MeshComp, System, /*bRight=*/false);
	}

	// 两侧都没生成出来就别留空条目，NotifyTick 也就不会白跑。
	if (!Trail.Right && !Trail.Left)
	{
		ActiveTrails.Remove(MeshComp);
	}
}

void UAnimNotifyState_BladeTrail::NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyTick(MeshComp, Animation, FrameDeltaTime, EventReference);

	if (!MeshComp)
	{
		return;
	}

	FActiveTrail* Trail = ActiveTrails.Find(MeshComp);
	if (!Trail)
	{
		return;
	}

	UpdateTrail(Trail->Right, MeshComp, /*bRight=*/true);
	UpdateTrail(Trail->Left, MeshComp, /*bRight=*/false);
}

void UAnimNotifyState_BladeTrail::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (!MeshComp)
	{
		return;
	}

	FActiveTrail Trail;
	if (!ActiveTrails.RemoveAndCopyValue(MeshComp, Trail))
	{
		return;
	}

	TeardownTrail(Trail.Right, TeardownDelay);
	TeardownTrail(Trail.Left, TeardownDelay);
}

void UAnimNotifyState_BladeTrail::TeardownTrail(UNiagaraComponent* Component, float DelaySeconds)
{
	if (!Component)
	{
		return;
	}

	// 先停发射：已经生成的那一段拖尾按 NS 自己的寿命自然收掉，比直接销毁好看。
	Component->Deactivate();
	Component->SetAutoDestroy(true);

	// 兜底：NS 的 emitter 要是设成无限循环，auto destroy 永远等不到 —— 那样每挥一次就泄漏一个
	// 常驻组件。挂个一次性定时器强拆，保证不管 NS 怎么配都不会累积。
	if (UWorld* World = Component->GetWorld())
	{
		TWeakObjectPtr<UNiagaraComponent> WeakComponent(Component);
		FTimerHandle TeardownTimer;
		World->GetTimerManager().SetTimer(TeardownTimer, FTimerDelegate::CreateWeakLambda(Component, [WeakComponent]()
		{
			if (UNiagaraComponent* StillAlive = WeakComponent.Get())
			{
				StillAlive->DestroyComponent();
			}
		}), DelaySeconds, /*bLoop=*/false);
	}
}

FString UAnimNotifyState_BladeTrail::GetNotifyName_Implementation() const
{
	switch (Side)
	{
	case EBladeTrailSide::Left: return TEXT("BladeTrail (左手)");
	case EBladeTrailSide::Both: return TEXT("BladeTrail (双手)");
	default: return TEXT("BladeTrail (右手)");
	}
}
