// Fill out your copyright notice in the Description page of Project Settings.


#include "ThrowDaggerProjectile.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Components/SphereComponent.h"
#include "Particles/ParticleSystemComponent.h"
#include "Particles/ParticleSystem.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Kismet/GameplayStatics.h"
#include "GAS/LOLGameplayTags.h"

// Sets default values
AThrowDaggerProjectile::AThrowDaggerProjectile()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->InitSphereRadius(12.f);
	Collision->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Collision->SetCollisionResponseToAllChannels(ECR_Overlap);
	Collision->SetGenerateOverlapEvents(true);
	RootComponent = Collision;
	Collision->OnComponentBeginOverlap.AddDynamic(this, &AThrowDaggerProjectile::OnOverlap);

	SpinParticle = CreateDefaultSubobject<UParticleSystemComponent>(TEXT("SpinParticle"));
	SpinParticle->SetupAttachment(RootComponent);

	SpawnBurst = CreateDefaultSubobject<UParticleSystemComponent>(TEXT("SpawnBurst"));
	SpawnBurst->SetupAttachment(RootComponent);

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->InitialSpeed = 0.f;
	ProjectileMovement->MaxSpeed = 0.f;
	ProjectileMovement->ProjectileGravityScale = 0.f;
	ProjectileMovement->bRotationFollowsVelocity = false;
	// 关键：Velocity 在世界空间设置（Initialize 里 = AimDir * Speed）。
	// 引擎默认 bInitialVelocityInLocalSpace=true，会在 FinishSpawning 的 InitializeComponent 里
	// 把世界方向当本地方向再转一次组件旋转，导致飞向 2θ。必须关掉。
	ProjectileMovement->bInitialVelocityInLocalSpace = false;
}

void AThrowDaggerProjectile::Initialize(const FVector& Direction, float Speed, TSubclassOf<UGameplayEffect> InDamageGE, float InDamage, AActor* InInstigator)
{
	DamageGE = InDamageGE;
	DamageAmount = InDamage;
	APawn* InstigatorPawn = Cast<APawn>(InInstigator);
	if(InstigatorPawn)
	SetInstigator(InstigatorPawn);

	SetOwner(InInstigator);

	ProjectileMovement->InitialSpeed = Speed;
	ProjectileMovement->MaxSpeed = Speed;
	// 关键：Velocity 是世界空间方向。这里（实例级，FinishSpawning 之前）再关一次 bInitialVelocityInLocalSpace，
	// 盖过 BP 子类可能序列化回来的 true。否则 InitializeComponent（在 FinishSpawning 里、晚于本函数）会
	// 把世界方向当本地方向、再按 spawn 旋转转一次，导致飞向 2θ。
	ProjectileMovement->bInitialVelocityInLocalSpace = false;
	ProjectileMovement->Velocity = Direction.GetSafeNormal() * Speed;
}



// Called when the game starts or when spawned
void AThrowDaggerProjectile::BeginPlay()
{
	Super::BeginPlay();
}

// Called every frame
void AThrowDaggerProjectile::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

void AThrowDaggerProjectile::OnOverlap(UPrimitiveComponent*, AActor* OtherActor,
	UPrimitiveComponent*, int32, bool bFromSweep, const FHitResult& SweepResult)
{
	if (!OtherActor || OtherActor == GetInstigator() || OtherActor == GetOwner()) return;
	if (!HasAuthority()) return;   

	
	if (DamageGE)
	{
		UAbilitySystemComponent* TargetASC =
			UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(OtherActor);
		if (TargetASC)
		{
			FGameplayEffectContextHandle Context = TargetASC->MakeEffectContext();
			Context.AddInstigator(GetInstigator(), this);
			FGameplayEffectSpecHandle Spec = TargetASC->MakeOutgoingSpec(DamageGE, 1.f, Context);
			if (Spec.IsValid())
			{
				Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_Damage, DamageAmount);
				TargetASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
			}
		}
	}


	const FGameplayTag HitType = ResolveHitType(OtherActor);
	if (HitFXMap.Contains(HitType))
	{
		if (UParticleSystem* FX = HitFXMap[HitType].Get())
		{
			// 在命中点（而非 actor 中心）放 burst；detached 发射器不受 Destroy 影响，会播完
			FVector HitLoc = GetActorLocation();
			if (bFromSweep) HitLoc = SweepResult.ImpactPoint; // ImpactPoint 是 FVector_NetQuantize，隐式转 FVector
			UGameplayStatics::SpawnEmitterAtLocation(GetWorld(), FX, FTransform(HitLoc));
		}
	}

	Destroy();
}

FGameplayTag AThrowDaggerProjectile::ResolveHitType(AActor* Other) const
{
	if (UAbilitySystemComponent* ASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Other))
	{
		if (ASC->HasMatchingGameplayTag(LOLGameplayTags::Target_Hero)) return LOLGameplayTags::Target_Hero;
		if (ASC->HasMatchingGameplayTag(LOLGameplayTags::Target_Void)) return LOLGameplayTags::Target_Void;
	}


	return LOLGameplayTags::Target_Terrain;
}

