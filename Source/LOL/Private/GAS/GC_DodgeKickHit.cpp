// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DodgeKickHit.h"
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

UGC_DodgeKickHit::UGC_DodgeKickHit()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DodgeKickHit;
}

bool UGC_DodgeKickHit::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// 命中点在 Parameters.Location（GA_Dodge 填的胶囊表面接触点）。
	// 朝向用命中面法线：这样粒子是「贴着被打中的那一面朝外喷」，而不是永远世界朝前。
	// Normal 没填时退回世界朝前（老行为，不会崩）。
	const FRotator ImpactRotation = Parameters.Normal.IsNearlyZero()
		? FRotator::ZeroRotator
		: Parameters.Normal.Rotation();

	UNiagaraSystem* FX = HitNiagara.LoadSynchronous();
	UNiagaraSystem* Ring = ShockwaveNiagara.LoadSynchronous();

	// 冲击强度 0~1：由 GA_Dodge 用 RawMagnitude 带进来的【落地速度】归一化得到。
	// 目标：从 8 米高空砸下来和贴地铲一脚在画面上不一样。没传（0）时给 1.0，
	// 也就是"一个参考强度"，不会把参数变成 0 反而把特效缩小。
	const float ImpactSpeed = Parameters.RawMagnitude > 0.f ? Parameters.RawMagnitude : ImpactSpeedRef;
	const float Strength = FMath::Clamp(ImpactSpeed / FMath::Max(ImpactSpeedRef, 1.f), 0.f, 1.f);

	// ---- ① 命中爆发 ----------------------------------------------------
	if (FX)
	{
		UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World, FX, Parameters.Location, ImpactRotation, FVector(1.f),
			/*bAutoDestroy=*/true, /*bAutoActivate=*/true);

		// 位置 / 法线也塞进用户参数：有些 NS 是「原地不动、靠参数定位」的写法，
		// 两套都喂上就不用在意 NS 作者选了哪种。NS 里没有这条参数时静默忽略，不报错。
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
		// 环要贴住命中面铺开，所以和爆发共用同一个朝向（法线）。
		// 半径也跟着强度走：砸得越狠，环铺得越大。
		UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World, Ring, Parameters.Location, ImpactRotation, FVector(1.f),
			/*bAutoDestroy=*/true, /*bAutoActivate=*/true);

		if (Comp)
		{
			// 环的大小：直接缩放组件，不写 Niagara 用户参数 —— 本 cue 用的
			// NS_DodgeKick_V2_ShockWave 实测一个用户参数都没有（ListUserParameters 返回 0），
			// User.RingScale 根本不存在，所以原来那句 SetFloatParameter 是静默 no-op。
			// XY 跟强度放大、Z 只吃基础值：环是 SM_ShockRing 这种平铺的盘，
			// Z 一起放大会把厚度也吹起来，1.95 倍时看着像根柱子而不是冲击波。
			const float RingScale = ShockwaveScale * FMath::Lerp(0.7f, 1.3f, Strength);
			Comp->SetWorldScale3D(FVector(RingScale, RingScale, ShockwaveScale));
			Comp->SetVectorParameter(ImpactNormalParameter, Parameters.Normal);
			Comp->SetFloatParameter(ImpactStrengthParameter, Strength);
		}
	}

	// ---- ③ 音效 ---------------------------------------------------------
	// 这一类身上不再留 ImpactSound：一律从事件表里取（Audio.DodgeKickHit）。
	UHeroAudioLibrary::PlayAt(World, LOLGameplayTags::Audio_DodgeKickHit, Parameters.Location);

	// ---- ④ 镜头震动：只给踢人的那一端 ------------------------------------
	// 判定规则（为什么用 MyTarget、为什么要两层判断、为什么不能换顿帧）全在
	// CueCameraShake.h 的注释里；这里是全部命中 cue 共用的那一份实现。
	HeroCueCameraShake::PlayLocalHitShake(MyTarget, HitCameraShake);

	// 全留空 = 这次命中没有任何表现（不是错误，只是没东西可播）。
	// 音效不计入：它走全局事件表。
	return FX != nullptr || Ring != nullptr || HitCameraShake != nullptr;
}
