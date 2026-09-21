// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/ThrowDaggerProjectile.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "Components/SphereComponent.h"
#include "Particles/ParticleSystemComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayEffectTypes.h"
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

	
	// 伤害统一走 GE_Damage + UExecCalc_Damage（见 GAS_Block_Setup.md §3.6）。
	// 这里只喂参数、不算伤害，格挡/抗性/减伤全在 ExecCalc 里，和近战是同一条路。
	if (DamageGE)
	{
		UAbilitySystemComponent* TargetASC =
			UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(OtherActor);
		if (TargetASC)
		{
			// MakeEffectContext 拿到的 instigator 是【被打的人】（它自己的 OwnerActor/AvatarActor），
			// 下面 AddInstigator 必须覆盖成投掷者 —— ExecCalc 靠它取攻击者 ASC 捕获攻击力/穿透，
			// 不覆盖就会拿目标自己的属性算伤害。
			FGameplayEffectContextHandle Context = TargetASC->MakeEffectContext();
			Context.AddInstigator(GetInstigator(), this);
			if (bFromSweep)
			{
				Context.AddHitResult(SweepResult);
			}

			FGameplayEffectSpecHandle Spec = TargetASC->MakeOutgoingSpec(DamageGE, 1.f, Context);
			if (Spec.IsValid())
			{
				// 匕首是纯固定伤害：倍率给 0（不吃攻击力），数值走 FlatDamage。
				// 想让匕首也吃攻击力加成，就在这里填倍率（=AD 系数），别改 ExecCalc 的公式。
				Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, 0.f);
				Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, DamageAmount);
				Spec.Data->AddDynamicAssetTag(LOLGameplayTags::Damage_Physical);

				TargetASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
			}
			else
			{
				// 不静默：Spec 无效 = 匕首扎上去没伤害，看起来像「穿模飞过去了」。
				UE_LOG(LogTemp, Warning, TEXT("[Dagger] DamageGE(%s) 的 Spec 无效 → 本次命中无伤害"),
					*GetNameSafe(DamageGE));
			}
		}
	}


	// 命中表现交给 GameplayCue：命中点/法线/目标类型打包进参数，
	// 由投射物主人的 ASC 执行 → 自动多播，各客户端都看得到。
	// 命中类型不放进 cue 标签（引擎只按弹出的那个标签查表，不会给子标签各跑一遍），
	// 而是塞进 AggregatedTargetTags，由 cue 自己挑粒子。
	if (UAbilitySystemComponent* SourceASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(GetInstigator()))
	{
		const FGameplayTag HitType = ResolveHitType(OtherActor);

		FGameplayCueParameters CueParams;
		// ImpactPoint / ImpactNormal 是 FVector_NetQuantize，隐式转 FVector。
		CueParams.Location = bFromSweep ? FVector(SweepResult.ImpactPoint) : GetActorLocation();
		CueParams.Normal = bFromSweep ? FVector(SweepResult.ImpactNormal) : -GetActorForwardVector();
		CueParams.Instigator = GetInstigator();
		CueParams.EffectCauser = this;
		CueParams.AggregatedTargetTags.AddTag(HitType);

		SourceASC->ExecuteGameplayCue(LOLGameplayTags::GameplayCue_ThrowDagger_Hit, CueParams);
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

