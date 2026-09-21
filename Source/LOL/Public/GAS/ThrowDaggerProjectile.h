// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "ThrowDaggerProjectile.generated.h"

class UGameplayEffect;
class USphereComponent;
class UProjectileMovementComponent;
class UParticleSystemComponent;
class UThrowDaggerFXData;
UCLASS()
class LOL_API AThrowDaggerProjectile : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AThrowDaggerProjectile();
	void Initialize(const FVector& Direction, float Speed,TSubclassOf<UGameplayEffect> InDamageGE, float InDamage, AActor* InInstigator);
protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;
	//flying FX
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UParticleSystemComponent> SpinParticle;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UParticleSystemComponent> SpawnBurst;
	//Collision
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<USphereComponent> Collision;
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	//FlyingPMC	
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;
	UPROPERTY()
	TSubclassOf<UGameplayEffect> DamageGE;
	UPROPERTY()
	float DamageAmount = 0.f;

	// 命中特效搬到 GameplayCue 了（见 UGC_ThrowDaggerHit）：
	// 原来那段 SpawnEmitterAtLocation 写在 `if (!HasAuthority()) return;` 后面，
	// 结果只有服务端放特效、客户端什么都看不到。走 cue 才会多播到各客户端。
	// 同时「命中类型 → 粒子」的表也跟着搬到 cue 上（GC_ThrowDagger_Hit 的 HitFXMap）。

	UFUNCTION()
	void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult);

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;
	FGameplayTag ResolveHitType(AActor* Other)const;
};
