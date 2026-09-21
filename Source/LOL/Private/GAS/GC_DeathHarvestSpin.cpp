// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestSpin.h"
#include "GAS/LOLGameplayTags.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"

AGC_DeathHarvestSpin::AGC_DeathHarvestSpin()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Spin;
	bAutoDestroyOnRemove = true;
	bAutoAttachToOwner = true;
}

bool AGC_DeathHarvestSpin::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return false;
	}

	DestroyLoop();

	USceneComponent* AttachTo = nullptr;
	if (const ACharacter* Character = Cast<ACharacter>(MyTarget))
	{
		AttachTo = Character->GetMesh() ? static_cast<USceneComponent*>(Character->GetMesh()) : nullptr;
	}
	if (!AttachTo)
	{
		AttachTo = MyTarget->GetRootComponent();
	}

	if (LoopParticle && AttachTo)
	{
		// 左右手各一份：右手插槽是镜像的，朝向和偏移都要能单独补，不然同一个粒子挂上去会翻面
		//（朝刀背而不是刀刃）。照 GC_Stealth::SpawnSwordParticles 的写法。
		struct FLoopSide
		{
			FName Socket;
			FVector Offset;
			FRotator Rotation;
		};
		const FLoopSide Sides[2] =
		{
			{ LoopSocketLeft,  RelativeOffsetLeft,  ParticleRotationLeft },
			{ LoopSocketRight, RelativeOffsetRight, ParticleRotationRight },
		};

		// 插槽不存在时 SpawnEmitterAttached 照样返回有效组件，只是把粒子挂在组件原点上 ——
		// 表现为「粒子从角色脚下冒出来」，看不出是名字写错了。
		const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(AttachTo);
		for (const FLoopSide& Side : Sides)
		{
			if (Mesh && !(Mesh->DoesSocketExist(Side.Socket) || Mesh->GetBoneIndex(Side.Socket) != INDEX_NONE))
			{
				UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 转圈粒子：%s 上找不到插槽 %s → 这一侧不生成"),
					*GetNameSafe(Mesh->GetSkeletalMeshAsset()), *Side.Socket.ToString());
				continue;
			}

			UParticleSystemComponent* Comp = UGameplayStatics::SpawnEmitterAttached(
				LoopParticle, AttachTo, Side.Socket, Side.Offset, Side.Rotation, Scale,
				EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false);

			if (Comp)
			{
				LoopComps.Add(Comp);
			}
		}
	}

	if (SpinSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, SpinSound, MyTarget->GetActorLocation());
	}

	return LoopComps.Num() > 0;
}

bool AGC_DeathHarvestSpin::WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// 服务器那一端走的是这条（运行时 Add 的 cue）。转给 OnActive，理由见头文件。
	return OnActive_Implementation(MyTarget, Parameters);
}

bool AGC_DeathHarvestSpin::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	DestroyLoop();
	return true;
}

void AGC_DeathHarvestSpin::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyLoop();
	Super::EndPlay(EndPlayReason);
}

void AGC_DeathHarvestSpin::DestroyLoop()
{
	for (TObjectPtr<UParticleSystemComponent>& Comp : LoopComps)
	{
		if (Comp)
		{
			Comp->Deactivate();
			Comp->bAutoDestroy = true;
			Comp->DestroyComponent();
		}
	}
	LoopComps.Reset();
}
