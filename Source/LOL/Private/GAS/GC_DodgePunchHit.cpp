// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DodgePunchHit.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/CueCameraShake.h"
#include "GAS/LOLGameplayTags.h"
#include "Camera/CameraShakeBase.h"
#include "Engine/World.h"
#include "GameplayEffectTypes.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"

UGC_DodgePunchHit::UGC_DodgePunchHit()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DodgePunchHit;
}

bool UGC_DodgePunchHit::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// 命中点在 Parameters.Location（GA_Dodge 填的胶囊表面接触点），朝向用命中面法线。
	const FRotator ImpactRotation = Parameters.Normal.IsNearlyZero()
		? FRotator::ZeroRotator
		: Parameters.Normal.Rotation();

	UNiagaraSystem* FX = HitNiagara.LoadSynchronous();
	UNiagaraSystem* Ring = ShockwaveNiagara.LoadSynchronous();

	// 冲击强度 0~1：由 GA_Dodge 用 RawMagnitude 带进来的攻击者速度归一化（和飞踢同一条路）。
	const float ImpactSpeed = Parameters.RawMagnitude > 0.f ? Parameters.RawMagnitude : ImpactSpeedRef;
	const float Strength = FMath::Clamp(ImpactSpeed / FMath::Max(ImpactSpeedRef, 1.f), 0.f, 1.f);

	// ---- ① 命中爆发 ----------------------------------------------------
	if (FX)
	{
		UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World, FX, Parameters.Location, ImpactRotation, FVector(1.f),
			/*bAutoDestroy=*/true, /*bAutoActivate=*/true);

		if (Comp)
		{
			Comp->SetVectorParameter(ImpactPositionParameter, Parameters.Location);
			Comp->SetVectorParameter(ImpactNormalParameter, Parameters.Normal);
			Comp->SetFloatParameter(ImpactStrengthParameter, Strength);
		}
	}

	// ---- ② 冲击波环（可选）----------------------------------------------
	if (Ring)
	{
		UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World, Ring, Parameters.Location, ImpactRotation, FVector(1.f),
			/*bAutoDestroy=*/true, /*bAutoActivate=*/true);

		if (Comp)
		{
			// 环的大小：直接缩放组件，不写 Niagara 用户参数 —— 本 cue 用的 NS_Punch_Shockwave
			// 实测没有任何用户参数，User.RingScale 不存在，原来那句 SetFloatParameter 是静默 no-op。
			// XY 跟强度放大、Z 只吃基础值：环是 SM_ShockRing 这种平铺的盘，
			// Z 一起放大会把厚度也吹起来，看着像根柱子而不是冲击波。
			const float RingScale = ShockwaveScale * FMath::Lerp(0.7f, 1.3f, Strength);
			Comp->SetWorldScale3D(FVector(RingScale, RingScale, ShockwaveScale));
			Comp->SetVectorParameter(ImpactNormalParameter, Parameters.Normal);
			Comp->SetFloatParameter(ImpactStrengthParameter, Strength);
		}
	}

	// ---- ③ 音效 ---------------------------------------------------------
	// 这一类身上不再留 ImpactSound：一律从事件表里取（Audio.DodgePunchHit）。
	UHeroAudioLibrary::PlayAt(World, LOLGameplayTags::Audio_DodgePunchHit, Parameters.Location);

	// ---- ④ 镜头震动：只给打人的那一端 ------------------------------------
	// 与 GC_DodgeKickHit 共用同一份实现，理由见 CueCameraShake.h。
	HeroCueCameraShake::PlayLocalHitShake(MyTarget, HitCameraShake);

	// 音效不计入「有没有表现」：它走全局事件表。
	return FX != nullptr || Ring != nullptr || HitCameraShake != nullptr;
}
