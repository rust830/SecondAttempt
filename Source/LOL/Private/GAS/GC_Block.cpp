// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_Block.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/LOLGameplayTags.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"

AGC_Block::AGC_Block()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_Block;

	// GE 移除时自动销毁本 cue actor，不用手写清理。
	bAutoDestroyOnRemove = true;

	// 不改角色 transform，只是挂一个罩子上去，不需要附着到 owner。
	bAutoAttachToOwner = false;
}

bool AGC_Block::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return false;
	}

	// 预测重放 / 免疫被刷新（先摘后挂）时 OnActive 可能被调第二次，先清旧的，避免叠两层罩子。
	DestroyShield();

	SpawnShield(MyTarget);

	if (!bLoggedShieldDiagnostic)
	{
		bLoggedShieldDiagnostic = true;
		LogShieldDiagnostic();
	}

	// 音效从事件表里取（Audio.Block），这一类身上不再留音效属性。
	UHeroAudioLibrary::PlayAt(this, LOLGameplayTags::Audio_Block, MyTarget->GetActorLocation());

	return true;
}

bool AGC_Block::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	DestroyShield();
	return true;
}

void AGC_Block::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyShield();
	Super::EndPlay(EndPlayReason);
}

USceneComponent* AGC_Block::ResolveAttachParent(AActor* Target)
{
	if (const ACharacter* Character = Cast<ACharacter>(Target))
	{
		if (Character->GetMesh())
		{
			return Character->GetMesh();
		}
	}
	return Target ? Target->GetRootComponent() : nullptr;
}

void AGC_Block::SpawnShield(AActor* Target)
{
	USceneComponent* AttachTo = ResolveAttachParent(Target);
	if (!AttachTo)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Block] 目标 %s 没有可挂载的组件 → 防护罩不会出现"), *GetNameSafe(Target));
		return;
	}

	if (ShieldSystem)
	{
		// Local Space 由 NS 自己控制：角色移动时罩子要跟着走，所以 NS 里 Emitter 的
		// Local Space 必须勾上（否则罩子会留在原地）。
		UNiagaraComponent* Niagara = UNiagaraFunctionLibrary::SpawnSystemAttached(
			ShieldSystem, AttachTo, NAME_None, ShieldOffset, FRotator::ZeroRotator,
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false);
		if (Niagara)
		{
			Niagara->SetRelativeScale3D(ShieldScale);
		}
		ShieldComp = Niagara;
		return;
	}

	if (ShieldMesh)
	{
		// Outer 给 Target：目标没了组件跟着没，不会留在世界里。
		UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(Target);
		MeshComp->SetStaticMesh(ShieldMesh);
		if (ShieldMaterial)
		{
			MeshComp->SetMaterial(0, ShieldMaterial);
		}
		// 罩子是纯表现：不参与碰撞、不投影，否则会挡住近战扫描（SweepMultiByChannel 用的是 ECC_Pawn）。
		MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComp->SetGenerateOverlapEvents(false);
		MeshComp->SetCastShadow(false);
		MeshComp->RegisterComponent();
		MeshComp->AttachToComponent(AttachTo, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		MeshComp->SetRelativeLocation(ShieldOffset);
		MeshComp->SetRelativeScale3D(ShieldScale);
		ShieldComp = MeshComp;
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[Block] 防护罩没配：ShieldSystem 和 ShieldMesh 都是空的 → 格挡成功但屏幕上看不到罩子"));
}

void AGC_Block::DestroyShield()
{
	if (ShieldComp)
	{
		ShieldComp->DestroyComponent();
		ShieldComp = nullptr;
	}
}

void AGC_Block::LogShieldDiagnostic()
{
	// 四道看不见的门，任何一道没过都是「什么都不显示、也不报错」（方案文档 §4.6）。
	if (ShieldSystem)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Block] 防护罩诊断: Niagara 系统=%s（罩子不显示时先看 NS 里 emitter 的 Life Cycle Mode 是不是 Infinite、Local Space 有没有勾）"),
			*GetNameSafe(ShieldSystem.Get()));
	}

	const UMaterialInterface* Material = ShieldMaterial;
	if (!Material && ShieldMesh)
	{
		// 没指定材质时看的是网格第 0 槽自带的那个。
		Material = ShieldMesh->GetMaterial(0);
	}
	if (!Material)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Block] 防护罩诊断: 拿不到材质（ShieldMaterial 为空且网格没有默认材质）"));
		return;
	}

	const UMaterial* Base = Material->GetMaterial();
	const bool bNiagaraMeshOK = Material->CheckMaterialUsage_Concurrent(MATUSAGE_NiagaraMeshParticles);
	const bool bNiagaraSpriteOK = Material->CheckMaterialUsage_Concurrent(MATUSAGE_NiagaraSprites);

	const UEnum* BlendEnum = StaticEnum<EBlendMode>();
	const FString BlendName = BlendEnum
		? BlendEnum->GetNameStringByValue((int64)Material->GetBlendMode())
		: FString::FromInt((int32)Material->GetBlendMode());

	const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
	const EMaterialShadingModel FirstShadingModel = ShadingModels.CountShadingModels() > 0
		? ShadingModels.GetFirstShadingModel() : MSM_DefaultLit;
	const UEnum* ShadingModelEnum = StaticEnum<EMaterialShadingModel>();
	const FString ShadingModelName = ShadingModelEnum
		? ShadingModelEnum->GetNameStringByValue((int64)FirstShadingModel)
		: FString::FromInt((int32)FirstShadingModel);

	UE_LOG(LogTemp, Warning,
		TEXT("[Block] 防护罩诊断: 材质=%s 域=%s(须 Surface) 混合模式=%s 着色模型=%s(建议 Unlit) NiagaraMesh用法=%d NiagaraSprite用法=%d"),
		*GetNameSafe(Material),
		Base ? *MaterialDomainString(Base->MaterialDomain) : TEXT("无"),
		*BlendName, *ShadingModelName,
		bNiagaraMeshOK ? 1 : 0, bNiagaraSpriteOK ? 1 : 0);

	if (ShieldSystem && !bNiagaraMeshOK && !bNiagaraSpriteOK)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Block] ↑ 材质没勾任何 Niagara 用法：Mesh Renderer 要勾 'Used with Niagara Mesh Particles'，Sprite Renderer 要勾 'Used with Niagara Sprites'。改完记得重新编译保存材质"));
	}
}
