// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_ThrowAiming.h"
#include "GAS/LOLGameplayTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"
#include "UObject/SoftObjectPath.h"

AGC_ThrowAiming::AGC_ThrowAiming()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_ThrowAiming;

	// 按「再次进瞄准」时能重新挂上：GE/loose tag 的增删会各触发一次 OnActive/OnRemove，
	// 不允许重复 OnActive 的话第二次瞄准就没有轮廓了。
	bAllowMultipleOnActiveEvents = true;

	// 默认指向 Paragon 的 E 技能抬手语音。软引用：默认值只是路径，编辑器里随时换。
	AimingSound = TSoftObjectPtr<USoundBase>(
		FSoftObjectPath(TEXT("/Game/ParagonKallari/Audio/Cues/Kallari_Effort_Ability_E_Raise.Kallari_Effort_Ability_E_Raise")));
}

bool AGC_ThrowAiming::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// 只给本地控制端：轮廓和抬手声都是「我在瞄准」的反馈，敌人不该看到/听到别人在瞄谁。
	const APawn* Pawn = Cast<APawn>(MyTarget);
	if (!Pawn || !Pawn->IsLocallyControlled())
	{
		return true;   // 不是失败，只是本端不需要这个表现
	}

	// bAllowMultipleOnActiveEvents + 预测重放会让 OnActive 连着来两次，声音不能跟着叠两下。
	// 连续两次 OnActive 之间没有 OnRemove，所以这个标记正好区分「真·重新瞄准」和「同一帧重放」。
	if (!bPlayedAimingSound)
	{
		if (USoundBase* Sound = AimingSound.LoadSynchronous())
		{
			bPlayedAimingSound = true;
			UGameplayStatics::PlaySoundAtLocation(this, Sound, MyTarget->GetActorLocation());
		}
	}

	if (!ReticleFX)
	{
		return true;
	}

	const ACharacter* Character = Cast<ACharacter>(MyTarget);
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	if (!Mesh)
	{
		return true;
	}

	if (!Mesh->DoesSocketExist(AttachSocketName))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowAiming] socket [%s] 不存在，回退到 mesh 根"), *AttachSocketName.ToString());
	}

	// 先清旧的：bAllowMultipleOnActiveEvents 下可能连续触发两次 OnActive。
	if (ReticleComponent)
	{
		ReticleComponent->DestroyComponent();
		ReticleComponent = nullptr;
	}

	ReticleComponent = UGameplayStatics::SpawnEmitterAttached(
		ReticleFX, Mesh, AttachSocketName,
		FVector::ZeroVector, FRotator::ZeroRotator,
		EAttachLocation::SnapToTarget);

	return true;
}

bool AGC_ThrowAiming::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// 退出瞄准：下次再进瞄准时抬手声要重新放。
	bPlayedAimingSound = false;

	if (ReticleComponent)
	{
		ReticleComponent->DestroyComponent();
		ReticleComponent = nullptr;
	}
	return true;
}
