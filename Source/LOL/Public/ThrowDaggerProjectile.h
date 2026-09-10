// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "ThrowDaggerProjectile.generated.h"

class UGameplayEffect;
class USphereComponent;
class UProjectileMovementComponent;
class UParticleSystem;
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
	//HitFXTable
	UPROPERTY(EditAnywhere,BlueprintReadWrite)
	TMap<FGameplayTag, TObjectPtr<UParticleSystem>> HitFXMap;

	UFUNCTION()
	void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult);

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;
	FGameplayTag ResolveHitType(AActor* Other)const;
};
