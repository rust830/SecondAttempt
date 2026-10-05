// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_EmpoweredAttack.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/LOLGameplayTags.h"
#include "GameFramework/Character.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "GameplayEffectTypes.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"

AGC_EmpoweredAttack::AGC_EmpoweredAttack()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_EmpoweredAttack;

	// GE 移除时自动销毁本 cue actor，不用手写清理。
	bAutoDestroyOnRemove = true;

	// 不改角色 transform，只是就地播动画，不需要附着到 owner。
	bAutoAttachToOwner = false;

	// 强化窗口可能被重复挂（空挥没打中时窗口还能再触发、破隐和完美窗口也可能叠上）：
	// 不允许重复 OnActive 的话，第二次挂上时动画和音效都不会重播，窗口刷新就没了反馈。
	bAllowMultipleOnActiveEvents = true;
}

bool AGC_EmpoweredAttack::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return true;
	}

	PlayEmpowerMontage(MyTarget);
	SpawnFormParticles(MyTarget,
		IsTargetUnarmed(MyTarget) ? nullptr : EnterParticle,
		IsTargetUnarmed(MyTarget) ? UnarmedEnterParticle : nullptr);
	UHeroAudioLibrary::PlayAt(MyTarget, LOLGameplayTags::Audio_EmpoweredAttackEnter, MyTarget->GetActorLocation());

	// 永远返回 true：素材没配只是「没表现」，不是失败。
	// 返回 false 会被当成 cue 挂载失败，actor 当场销毁，OnRemove 就没机会收尾了。
	return true;
}

bool AGC_EmpoweredAttack::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// 命中消耗掉 / 窗口自然到期都会走到这里。MyTarget 可能已经失效（角色被销毁），
	// 但 StopEmpowerMontage / PlaySoundAt 都对 null 免疫，这里不用额外判空。
	StopEmpowerMontage(MyTarget);
	SpawnFormParticles(MyTarget,
		IsTargetUnarmed(MyTarget) ? nullptr : ExitParticle,
		IsTargetUnarmed(MyTarget) ? UnarmedExitParticle : nullptr);
	UHeroAudioLibrary::PlayAt(MyTarget, LOLGameplayTags::Audio_EmpoweredAttackExit, MyTarget->GetActorLocation());

	return true;
}

UAnimInstance* AGC_EmpoweredAttack::GetTargetAnimInstance(AActor* Target) const
{
	const ACharacter* Character = Cast<ACharacter>(Target);
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	return Mesh ? Mesh->GetAnimInstance() : nullptr;
}

void AGC_EmpoweredAttack::PlayEmpowerMontage(AActor* Target)
{
	if (!EmpowerMontage)
	{
		if (!bLoggedMissingMontage)
		{
			bLoggedMissingMontage = true;
			UE_LOG(LogTemp, Warning,
				TEXT("[EmpoweredAttack] EmpowerMontage 为空：BP GC_EmpoweredAttack 里没配，强化状态不会播动画（音效照常）"));
		}
		return;
	}

	UAnimInstance* Anim = GetTargetAnimInstance(Target);
	if (!Anim)
	{
		return;
	}

	// 最后一个参数由 bStopOtherMontages 决定（默认 false）：完美窗口是在普攻命中结算的同时挂状态的，
	// 停光所有 Montage 会把正在播的那段普攻动画当场切断，只顶掉同 slot 的那条就够。
	Anim->Montage_Play(EmpowerMontage, MontagePlayRate, EMontagePlayReturnType::MontageLength, 0.f, bStopOtherMontages);
}

void AGC_EmpoweredAttack::StopEmpowerMontage(AActor* Target)
{
	if (!EmpowerMontage)
	{
		return;
	}

	if (UAnimInstance* Anim = GetTargetAnimInstance(Target))
	{
		// 已经播完、或早被普攻 Montage 顶掉时这里是 no-op：引擎只会在该 Montage 确实还是当前实例时才停，
		// 所以不用自己判断「还在不在播」——空挥不消耗窗口时，这条路径正是靠它收尾的。
		Anim->Montage_Stop(MontageStopBlendOut, EmpowerMontage);
	}
}

USceneComponent* AGC_EmpoweredAttack::ResolveAttachComponent(AActor* Target) const
{
	if (!IsValid(Target))
	{
		return nullptr;
	}

	// 挂角色网格上而不是 root：粒子要跟着身体/手臂走，挂 root 会在转向时脱节（同 GC_Stealth）。
	if (const ACharacter* Character = Cast<ACharacter>(Target))
	{
		if (Character->GetMesh())
		{
			return Character->GetMesh();
		}
	}

	return Target->GetRootComponent();
}

bool AGC_EmpoweredAttack::IsTargetUnarmed(AActor* Target) const
{
	// 判据和 GA_AirAttack 一致：读 State.Form.Unarmed（GA_FormSwitch 切的 GE 授的标签）。
	// cue 在每个客户端各跑一遍，标签是复制的，两端读到的形态一致。
	const UAbilitySystemComponent* ASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
	return ASC && ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed);
}

void AGC_EmpoweredAttack::SpawnFormParticles(AActor* Target, UParticleSystem* Cascade, UNiagaraSystem* NiagaraIn) const
{
	if ((!Cascade && !NiagaraIn) || !IsValid(Target))
	{
		return;
	}

	USceneComponent* AttachTo = ResolveAttachComponent(Target);
	if (!AttachTo)
	{
		return;
	}

	// 左右各一份：右手插槽是镜像的，朝向要能单独补，不然同一个粒子挂上去会翻面。
	// 空手用双拳插槽（拳一般不需要镜像补偿，共用 UnarmedParticleRotation）。
	struct FSide
	{
		FName Socket;
		FRotator Rotation;
	};
	const bool bUnarmed = IsTargetUnarmed(Target);
	const FSide Sides[2] =
	{
		{ bUnarmed ? UnarmedSocketLeft  : SwordSocketLeft,  bUnarmed ? UnarmedParticleRotation : ParticleRotationLeft },
		{ bUnarmed ? UnarmedSocketRight : SwordSocketRight, bUnarmed ? UnarmedParticleRotation : ParticleRotationRight },
	};

	// 插槽不存在时 SpawnEmitterAttached 照样返回有效组件，只是把粒子挂在组件原点上 ——
	// 表现为「粒子从角色脚下冒出来」，看不出是名字写错了（同 GC_Stealth::SpawnSwordParticles）。
	const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(AttachTo);
	for (const FSide& Side : Sides)
	{
		if (Mesh && !(Mesh->DoesSocketExist(Side.Socket) || Mesh->GetBoneIndex(Side.Socket) != INDEX_NONE))
		{
			UE_LOG(LogTemp, Warning, TEXT("[EmpoweredAttack] 粒子插槽：%s 上找不到插槽 %s → 这一侧不生成"),
				*GetNameSafe(Mesh->GetSkeletalMeshAsset()), *Side.Socket.ToString());
			continue;
		}

		// bAutoDestroy=true：这是挂上/收掉那一下的一次性 burst，播完自己销毁，
		// 不像 GC_Stealth 的刀根粒子那样要跨整段隐身、得自己记账收尾。
		if (Cascade)
		{
			UGameplayStatics::SpawnEmitterAttached(Cascade, AttachTo, Side.Socket,
				FVector::ZeroVector, Side.Rotation, ParticleScale,
				EAttachLocation::SnapToTarget, /*bAutoDestroy=*/true);
		}
		else if (NiagaraIn)
		{
			UNiagaraFunctionLibrary::SpawnSystemAttached(NiagaraIn, AttachTo, Side.Socket,
				FVector::ZeroVector, Side.Rotation, ParticleScale,
				EAttachLocation::SnapToTarget, /*bAutoDestroy=*/true,
				ENCPoolMethod::None, /*bAutoActivate=*/true, /*bPreCullCheck=*/false);
		}
	}
}
