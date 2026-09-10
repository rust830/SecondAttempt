// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GA_Flash.h"
#include "GAS/GE_FlashCooldown.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "NiagaraSystem.h"
#include "NiagaraFunctionLibrary.h"
#include "Sound/SoundBase.h"
#include "Kismet/GameplayStatics.h"

UGA_Flash::UGA_Flash()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;   // Active: triggered by slot button

	CooldownDuration = 300.f;   // LOL 闪现 CD，测试可先改 5~10
	CooldownGameplayEffectClass = UGE_FlashCooldown::StaticClass();
}

void UGA_Flash::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// CommitAbility：检查冷却 → 通过后 ApplyCooldown（SetByCaller 填时长）。
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	const FVector Start = Character->GetActorLocation();
	const FVector Direction = ComputeFlashDirection(Character);
	const FVector Target = Start + Direction * FlashRange;

	// 先算出安全落点（胶囊扫掠防穿墙）。
	FVector Destination = Start;
	TryFindBlinkDestination(Character, Start, Target, Destination);

	// 传送必须服务端权威（反作弊）。单机下 HasAuthority() 恒为 true。
	if (Character->HasAuthority())
	{
		Character->SetActorLocation(Destination, /*bSweep=*/false, nullptr, ETeleportType::None);
	}

	// 播放粒子/音效（表现层；单机下直接播，联网时客户端预测播放）。
	PlayFlashEffects(Destination);

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

FVector UGA_Flash::ComputeFlashDirection(const ACharacter* Character) const
{
	const AController* Controller = Character->GetController();
	if (!Controller)
	{
		return Character->GetActorForwardVector();
	}

	// 第三人称：朝「相机朝向」的水平分量闪现（只用 Yaw，忽略俯仰，保证水平位移）。
	// 若想做成「朝鼠标指向的世界点」，改用 PlayerController::GetHitResultUnderCursor 取点算方向即可。
	const FRotator ControlRotation = Controller->GetControlRotation();
	return FRotationMatrix(FRotator(0.f, ControlRotation.Yaw, 0.f)).GetUnitAxis(EAxis::X);
}

bool UGA_Flash::TryFindBlinkDestination(const ACharacter* Character, const FVector& Start, const FVector& Target, FVector& OutDestination) const
{
	// 用角色胶囊沿闪现方向扫掠：撞墙就把落点缩到墙前（含半径缓冲），保证绝不嵌进墙体。
	const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 34.f;
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.f;

	const FVector Delta = Target - Start;
	const float Distance = Delta.Size();
	if (Distance < KINDA_SMALL_NUMBER)
	{
		OutDestination = Start;
		return true;
	}

	FCollisionQueryParams Params(SCENE_QUERY_STAT(FlashBlink), false, Character);
	Params.AddIgnoredActor(Character);

	FHitResult Sweep;
	const bool bHit = Character->GetWorld()->SweepSingleByChannel(
		Sweep, Start, Target, FQuat::Identity, ECC_Pawn,
		FCollisionShape::MakeCapsule(Radius, HalfHeight), Params);

	if (bHit)
	{
		// 撞墙：把落点放在命中法线外侧，留出胶囊半径 + 缓冲，避免贴墙/嵌墙。
		const float Buffer = Radius + 2.f;
		OutDestination = Sweep.Location + Sweep.Normal * Buffer;
		OutDestination.Z = Start.Z;   // 闪现是水平位移，保持地面高度
		return true;
	}

	OutDestination = Target;
	return true;
}

void UGA_Flash::PlayFlashEffects(const FVector& Location) const
{
	if (FlashNiagara)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(GetWorld(), FlashNiagara, Location);
	}
	if (FlashSound)
	{
		UGameplayStatics::PlaySoundAtLocation(GetWorld(), FlashSound, Location);
	}
}
