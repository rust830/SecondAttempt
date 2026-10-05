// Fill out your copyright notice in the Description page Project Settings.

#include "Animation/AnimNotifyState_SocketNiagara.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

namespace
{
	/**
	 * 插槽和骨骼都算数：GetSocketLocation 对骨骼名同样有效。
	 *
	 * ⚠ 名字里的文件前缀是必须的，别改回 HasSocket：unity build 会把多个 .cpp 合进同一个
	 * Module.LOL.N.cpp，匿名 namespace 只挡【跨 TU】冲突，合进一个 TU 之后同签名的函数
	 * 就是重定义（C2084）。同样的实现还有 AnimNotifyState_BladeTrail / GA_ThreeHitPassive 两份。
	 */
	bool SocketNiagaraHasSocket(const USkeletalMeshComponent* MeshComp, const FName& SocketName)
	{
		return MeshComp->DoesSocketExist(SocketName) || MeshComp->GetBoneIndex(SocketName) != INDEX_NONE;
	}
}

void UAnimNotifyState_SocketNiagara::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	if (!MeshComp)
	{
		return;
	}

	// 角色在 notify 期间被销毁时 NotifyEnd 不一定跑得到，顺手清掉失效的键（弱引用，不会解野指针）。
	for (auto It = ActiveByMesh.CreateIterator(); It; ++It)
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

	UNiagaraSystem* SystemAsset = System.LoadSynchronous();
	if (!SystemAsset)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SocketNiagara] System 没配（或软引用加载失败）→ %s 上不生成"),
			*GetNameSafe(MeshComp->GetSkeletalMeshAsset()));
		return;
	}

	// [DIAG] diagnostics for "only-hit-effect" case
	UE_LOG(LogTemp, Warning, TEXT("[SocketNiagara] BEGIN anim=%s dur=%.3f socket=%s side=%d"),
		*GetNameSafe(Animation), TotalDuration, *SocketRight.ToString(), static_cast<int32>(Side));
	UE_LOG(LogTemp, Warning, TEXT("[SocketNiagara] MESH=%s world=%s"),
		*GetNameSafe(MeshComp->GetSkeletalMeshAsset()), *GetNameSafe(MeshComp->GetWorld()));
	// 「只有命中特效、看不到过程」的头号嫌疑：notify state 的时长是 0 ⇒ Begin / End 落在同一帧，
	// 粒子刚 spawn 就被 NotifyEnd 收掉，画面上等于只有命中那一炸没有过程。
	// 真凶通常是 montage 上那条 notify 的 Duration 没随重定向/烘焙带过来（默认就是 0）。
	// 这里不指望 montage，直接自己把特效撑住 MinVisibleSeconds（见头文件说明）。
	FActiveSocketNiagaras& Entry = ActiveByMesh.Add(MeshComp);

	if (TotalDuration <= 0.f)
	{
		Entry.bSelfDriven = true;
		if (World)
		{
			TWeakObjectPtr<USkeletalMeshComponent> WeakMesh(MeshComp);
			TWeakObjectPtr<UAnimNotifyState_SocketNiagara> WeakThis(this);
			FTimerDelegate FadeDelegate;
			FadeDelegate.BindLambda([WeakThis, WeakMesh]()
			{
				if (UAnimNotifyState_SocketNiagara* Self = WeakThis.Get())
				{
					if (FActiveSocketNiagaras* Active = Self->ActiveByMesh.Find(WeakMesh))
					{
						// Teardown 幂等：重复调只是再 Deactivate 一次；而第一次会挂上"延迟销毁"的定时器，
						// 回调再跑时组件通常已经被 DestroyComponent 了 —— WeakComponent 取空即安全返回。
						Self->Teardown(Active->Right, Self->TeardownDelay);
						Self->Teardown(Active->Left, Self->TeardownDelay);
					}
				}
			});
			World->GetTimerManager().SetTimer(Entry.FadeTimer, FadeDelegate, MinVisibleSeconds, /*bLoop=*/false);
		}
		UE_LOG(LogTemp, Warning,
			TEXT("[SocketNiagara] [diag] dur<=0 → 这一帧就 End；已挂 %.2fs 兜底存活（MinVisibleSeconds），但特效只覆盖这一帧的位置"),
			MinVisibleSeconds);
	}
	if (Side == ESocketParticleSide::Left || Side == ESocketParticleSide::Both)
	{
		Entry.Left = SpawnOne(MeshComp, SystemAsset, /*bRight=*/false);
		UpdateTrail(Entry.Left, MeshComp, /*bRight=*/false);
		if (!Entry.Left)
		{
			UE_LOG(LogTemp, Warning, TEXT("[SocketNiagara] SPAWN FAIL side=Left socket=%s"), *SocketLeft.ToString());
		}
	}
	if (Side == ESocketParticleSide::Right || Side == ESocketParticleSide::Both)
	{
		Entry.Right = SpawnOne(MeshComp, SystemAsset, /*bRight=*/true);
		UpdateTrail(Entry.Right, MeshComp, /*bRight=*/true);
		if (!Entry.Right)
		{
			UE_LOG(LogTemp, Warning, TEXT("[SocketNiagara] SPAWN FAIL side=Right socket=%s"), *SocketRight.ToString());
		}
	}

	// 两侧都没生成出来就别留空条目，NotifyTick 也就不会白跑。
	if (!Entry.Right && !Entry.Left)
	{
		ActiveByMesh.Remove(MeshComp);
	}
}

void UAnimNotifyState_SocketNiagara::NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	float FrameDeltaTime, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyTick(MeshComp, Animation, FrameDeltaTime, EventReference);

	if (!MeshComp)
	{
		return;
	}

	const FActiveSocketNiagaras* Entry = ActiveByMesh.Find(MeshComp);
	if (!Entry)
	{
		return;
	}

	UpdateTrail(Entry->Right, MeshComp, /*bRight=*/true);
	UpdateTrail(Entry->Left, MeshComp, /*bRight=*/false);
}

void UAnimNotifyState_SocketNiagara::UpdateTrail(UNiagaraComponent* Component, USkeletalMeshComponent* MeshComp, bool bRight) const
{
	if (!Component)
	{
		return;
	}

	const FName SocketName = bRight ? SocketRight : SocketLeft;
	if (!SocketNiagaraHasSocket(MeshComp, SocketName))
	{
		return;
	}

	// base = 插槽位置；tip = 沿插槽自身 X 轴再推 TipLength —— 拳头和前臂的轴向绑死在骨架上，
	// 不用再找一个"第二根插槽"（空手时手上本来就没什么能挂東西的插槽）。
	const FTransform SocketTM = MeshComp->GetSocketTransform(SocketName, RTS_World);
	const FVector BaseWorld = SocketTM.GetLocation();
	const FVector TipWorld = BaseWorld + SocketTM.GetUnitAxis(EAxis::X) * TipLength;

	// NS_Fist_Trail 没勾 Local Space（World）⇒ 直接喂世界坐标。
	// 勾了 Local Space 就得换成 ToLocal.InverseTransformPosition(...) —— 那时喂世界坐标会糊在原点。
	Component->SetVariablePosition(BasePositionParameter, BaseWorld);
	Component->SetVariablePosition(TipPositionParameter, TipWorld);
}

UNiagaraComponent* UAnimNotifyState_SocketNiagara::SpawnOne(USkeletalMeshComponent* MeshComp,
	UNiagaraSystem* SystemAsset, bool bRight) const
{
	const FName SocketName = bRight ? SocketRight : SocketLeft;
	if (!SocketNiagaraHasSocket(MeshComp, SocketName))
	{
		// 插槽不存在时 SpawnSystemAttached 会静默把系统挂在组件原点 —— 看不出是配置错的，必须当场报出来。
		UE_LOG(LogTemp, Warning, TEXT("[SocketNiagara] %s 上找不到插槽 %s → 这一侧不生成"),
			*GetNameSafe(MeshComp->GetSkeletalMeshAsset()), *SocketName.ToString());
		return nullptr;
	}

	const FRotator Rotation = bRight ? RelativeRotationRight : RelativeRotationLeft;

	// SnapToTarget：Location/Rotation 落成【相对插槽】的变换 —— 整个系统挂在插槽上，
	// 于是 ribbon 每帧从插槽位置发一颗粒子，"拳挥到哪拖尾跟到哪"。
	// bAutoDestroy=false：生命周期由 NotifyEnd 的 Teardown 统一管，不给引擎留一个"自己决定什么时候死"的分支。
	UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAttached(
		SystemAsset, MeshComp, SocketName, RelativeOffset, Rotation, EAttachLocation::SnapToTarget,
		/*bAutoDestroy=*/false, /*bAutoActivate=*/true, ENCPoolMethod::None, /*bPreCullCheck=*/false);

	if (!Comp)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SocketNiagara] SpawnSystemAttached 返回空（系统无效，或被可扩展性预剔除挡掉）"));
		return nullptr;
	}

	// 外观参数先喂进去：NS 里没有对应 User 参数时这个 setter 是静默的（换 NS 不该弹红日志）。
	// ⚠ 颜色用 LinearColor 那个重载：NS_Fist_Trail 里的 User.Color 类型是 LinearColor，
	// 用 SetVectorParameter(FVector) 过去是另一种类型，绑定不上（静默失败）。
	// 换 NS 时先看一下它的颜色参数到底是 LinearColor 还是 Vector3f，再挑这个或那个 setter。
	// ⚠ 名字参数吃的是 const FString&（不是 FName）—— 传 FName 是 C2664，必须 .ToString()。
	Comp->SetNiagaraVariableLinearColor(TrailTintParameter.ToString(), TrailTint);

	// 粗细走组件相对缩放（NS emitter 勾了 Local Space 时这个会失效，见头文件说明）。
	Comp->SetRelativeScale3D(FVector(TrailScale));

	// ★ 出拳时是 false：人已经现身了，敌人该看到拳风。
	// （GC_Stealth 的 SwordParticle 是 true —— 那一段抄过来时最容易抄错这个值。）
	Comp->SetOnlyOwnerSee(bOnlyOwnerSee);

	return Comp;
}

void UAnimNotifyState_SocketNiagara::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	UE_LOG(LogTemp, Warning, TEXT("[SocketNiagara] END anim=%s"), *GetNameSafe(Animation));
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (!MeshComp)
	{
		return;
	}

	FActiveSocketNiagaras Entry;
	if (!ActiveByMesh.RemoveAndCopyValue(MeshComp, Entry))
	{
		return;
	}

	// 时长被烘成 0 的情况：收尾交给 NotifyBegin 里挂的那个 FadeTimer。
	// 这里同一帧就 Teardown 的话，刚 spawn 的粒子下一帧就没了 ——「过程」依然看不见，兜底等于没做。
	if (Entry.bSelfDriven)
	{
		return;
	}

	Teardown(Entry.Right, TeardownDelay);
	Teardown(Entry.Left, TeardownDelay);
}

void UAnimNotifyState_SocketNiagara::Teardown(UNiagaraComponent* Component, float DelaySeconds)
{
	if (!Component)
	{
		return;
	}

	// 先停发射：已经生成的那部分按粒子自己的寿命自然收掉，比直接 Destroy 好看。
	Component->Deactivate();
	Component->SetAutoDestroy(true);

	// 兜底：emitter 要是设成无限循环，auto destroy 永远等不到 —— 每出一拳泄漏一个常驻组件。
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

FString UAnimNotifyState_SocketNiagara::GetNotifyName_Implementation() const
{
	switch (Side)
	{
	case ESocketParticleSide::Left:  return TEXT("SocketNiagara (左手)");
	case ESocketParticleSide::Both:  return TEXT("SocketNiagara (双手)");
	default:                         return TEXT("SocketNiagara (右手)");
	}
}
