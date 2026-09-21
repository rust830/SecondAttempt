# 投掷匕首技能（E 进瞄准 / 左键投掷 / E 再按取消）完整实现步骤

## 0. 一句话方案

物理按键在「路由层」被转成语义 GameplayEvent；能力只订阅事件、永远不碰键盘。

```
左键 → BasicAttackPressed ──┬─ 有 State.Throw.Aiming？ → 发 Event.Input.ThrowConfirm（投掷）
                             └─ 没有 → RouteBasicAttackInput（普攻，现状不动）

E   → ASC::AbilityInputTagPressed ──┬─ E槽能力已激活？ → 发 Event.Input.Repressed（取消）
                                     └─ 未激活 → TryActivateAbility（进瞄准）
```

一个能力 `GA_ThrowDagger`：激活进瞄准 → `WaitGameplayEvent` 监听「确认/取消」两个事件。

**为什么用 GameplayEvent 而不是 `UAbilityTask_WaitInputPress`**：你的 ASC 是自建的 `SlotTag → Handle` 路由（`SlotAbilityMap`），从没调用过 ASC 官方的 `AbilityInputPressed(int32 InputID)`，所以官方 `WaitInputPress` 监听不到。你项目里 `Event.Input.BasicAttack` 已经这么用了，顺着来。

## 文件清单

| 操作 | 文件 | 说明 |
|---|---|---|
| 改 | `Source/LOL/Public/GAS/LOLGameplayTags.h` | 加 3 个原生 tag 声明 |
| 改 | `Source/LOL/Private/GAS/LOLGameplayTags.cpp` | 加 3 个原生 tag 定义 |
| 新建 | `Source/LOL/Public/GAS/ThrowDaggerProjectile.h` | 投射物（Actor） |
| 新建 | `Source/LOL/Private/GAS/ThrowDaggerProjectile.cpp` | 投射物实现 |
| 新建 | `Source/LOL/Public/GAS/GA_ThrowDagger.h` | 投掷技能 |
| 新建 | `Source/LOL/Private/GAS/GA_ThrowDagger.cpp` | 投掷技能实现 |
| 改 | `Source/LOL/Public/GAS/HeroCombatCharacter.h` | 加 `RouteThrowConfirmInput` 声明 |
| 改 | `Source/LOL/Private/GAS/HeroCombatCharacter.cpp` | 左键分流 |
| 改 | `Source/LOL/Private/GAS/MyAbilitySystemComponent.cpp` | E 再按取消路由 |

> 所有 tag 都是原生（`UE_DEFINE_GAMEPLAY_TAG`），**不用改 `DefaultGameplayTags.ini`**（你的 `Ability_Slot_*`、`State_Cooldown_Flash` 就是这么做的）。

---

## 第 1 步：加 GameplayTag

### 1a. `Public/GAS/LOLGameplayTags.h`

在 `namespace LOLGameplayTags { ... }` 里（`Ability_Slot_E` 后面）加：

```cpp
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Throw_Aiming);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_ThrowConfirm);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_Input_Repressed);
```

### 1b. `Private/GAS/LOLGameplayTags.cpp`

在 `namespace LOLGameplayTags { ... }` 里加：

```cpp
	UE_DEFINE_GAMEPLAY_TAG(State_Throw_Aiming,       "State.Throw.Aiming");
	UE_DEFINE_GAMEPLAY_TAG(Event_Input_ThrowConfirm, "Event.Input.ThrowConfirm");
	UE_DEFINE_GAMEPLAY_TAG(Event_Input_Repressed,    "Event.Input.Repressed");
```

---

## 第 2 步：投射物 `AThrowDaggerProjectile`

### 2a. `Public/GAS/ThrowDaggerProjectile.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ThrowDaggerProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;
class UParticleSystemComponent;
class UGameplayEffect;

/** 直线飞行、空中翻滚的匕首投射物。命中判定只在服务端做（单机下 HasAuthority 恒 true）。 */
UCLASS()
class LOL_API AThrowDaggerProjectile : public AActor
{
	GENERATED_BODY()
public:
	AThrowDaggerProjectile();

	/** 初始化飞行方向/速度/伤害，由能力 spawn 后调用。 */
	void Initialize(const FVector& Direction, float Speed,
		TSubclassOf<UGameplayEffect> InDamageGE, float InDamage, AActor* InInstigator);

protected:
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USphereComponent> CollisionComp;

	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	/** 匕首空中翻滚的 Cascade 粒子（已有资产，在 BP 里指定 Template）。 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<UParticleSystemComponent> SpinParticle;

	UPROPERTY()
	TSubclassOf<UGameplayEffect> DamageGE;

	UPROPERTY()
	float DamageAmount = 0.f;

	UFUNCTION()
	void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult);
};
```

### 2b. `Private/GAS/ThrowDaggerProjectile.cpp`

```cpp
#include "GAS/ThrowDaggerProjectile.h"
#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Particles/ParticleSystemComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GAS/LOLGameplayTags.h"

AThrowDaggerProjectile::AThrowDaggerProjectile()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;              // PvP：随投射物复制到客户端
	SetReplicateMovement(true);

	CollisionComp = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	CollisionComp->InitSphereRadius(12.f);
	CollisionComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionComp->SetCollisionResponseToAllChannels(ECR_Overlap);
	CollisionComp->SetGenerateOverlapEvents(true);
	RootComponent = CollisionComp;
	CollisionComp->OnComponentBeginOverlap.AddDynamic(this, &AThrowDaggerProjectile::OnOverlap);

	SpinParticle = CreateDefaultSubobject<UParticleSystemComponent>(TEXT("SpinParticle"));
	SpinParticle->SetupAttachment(RootComponent);

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->InitialSpeed = 0.f;            // Initialize() 里设
	ProjectileMovement->MaxSpeed = 0.f;
	ProjectileMovement->ProjectileGravityScale = 0.f;  // 直线向前
	ProjectileMovement->bRotationFollowsVelocity = false; // 翻滚由 Cascade 粒子自身完成
}

void AThrowDaggerProjectile::Initialize(const FVector& Direction, float Speed,
	TSubclassOf<UGameplayEffect> InDamageGE, float InDamage, AActor* InInstigator)
{
	DamageGE = InDamageGE;
	DamageAmount = InDamage;
	SetInstigator(InInstigator);
	SetOwner(InInstigator);

	ProjectileMovement->InitialSpeed = Speed;
	ProjectileMovement->MaxSpeed = Speed;
	ProjectileMovement->Velocity = Direction.GetSafeNormal() * Speed;
}

void AThrowDaggerProjectile::OnOverlap(UPrimitiveComponent*, AActor* OtherActor,
	UPrimitiveComponent*, int32, bool, const FHitResult&)
{
	if (!OtherActor || OtherActor == GetInstigator() || OtherActor == GetOwner()) return;

	if (!HasAuthority()) return;   // 服务器权威命中

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

	// TODO: 命中粒子（按目标类型分 GameplayCue）+ 命中音效
	Destroy();
}
```

> 高速下 overlap 可能隧穿：v1 把 `InitSphereRadius` 调大（如 20）即可；彻底解决用 PMC 的 sub-step 或改 sweep。

---

## 第 3 步：投掷技能 `GA_ThrowDagger`

### 3a. `Public/GAS/GA_ThrowDagger.h`

```cpp
#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GameplayEffectTypes.h"
#include "GA_ThrowDagger.generated.h"

class AThrowDaggerProjectile;
class UAnimMontage;
class UParticleSystem;
class UParticleSystemComponent;
class UGameplayEffect;
class UAbilityTask_PlayMontageAndWait;

/** 投掷匕首：E 进瞄准 → 左键投掷 / E 再按取消。直线飞行、空中翻滚。 */
UCLASS(Blueprintable)
class LOL_API UGA_ThrowDagger : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_ThrowDagger();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	// ===== 配置（BP 子类里填）=====
	UPROPERTY(EditDefaultsOnly, Category = "Throw|Projectile")
	TSubclassOf<AThrowDaggerProjectile> ProjectileClass;

	UPROPERTY(EditDefaultsOnly, Category = "Throw|Projectile", meta = (ClampMin = "0"))
	float ThrowSpeed = 1800.f;

	UPROPERTY(EditDefaultsOnly, Category = "Throw|Damage")
	TSubclassOf<UGameplayEffect> DamageGE;

	UPROPERTY(EditDefaultsOnly, Category = "Throw|Damage", meta = (ClampMin = "0"))
	float BaseDamage = 80.f;

	/** 瞄准态 GE：授予 State.Throw.Aiming（供动画/阻断/左键路由判断），可选减速。 */
	UPROPERTY(EditDefaultsOnly, Category = "Throw|Aim")
	TSubclassOf<UGameplayEffect> AimingGE;

	/** 投掷 Montage：sections = Targeting(循环) / Cast / Cancel。 */
	UPROPERTY(EditDefaultsOnly, Category = "Throw|Anim")
	TObjectPtr<UAnimMontage> ThrowMontage;

	/** 屏幕中央瞄准框（Cascade 粒子，客户端本地表现）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Throw|VFX")
	TObjectPtr<UParticleSystem> AimingReticleFX;

protected:
	UFUNCTION()
	void OnConfirmEvent(FGameplayEventData Payload);

	UFUNCTION()
	void OnCancelEvent(FGameplayEventData Payload);

private:
	void EnterAimingState();
	void ExitAimingState();
	FVector GetAimDirection() const;
	void SpawnProjectile(const FVector& Direction);

	FActiveGameplayEffectHandle AimingGEHandle;
	UPROPERTY() TObjectPtr<UAbilityTask_PlayMontageAndWait> MontageTask;
	UPROPERTY() TObjectPtr<UParticleSystemComponent> AimReticleComponent;
};
```

### 3b. `Private/GAS/GA_ThrowDagger.cpp`

```cpp
#include "GAS/GA_ThrowDagger.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/ThrowDaggerProjectile.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "Particles/ParticleSystemComponent.h"
#include "Kismet/GameplayStatics.h"

UGA_ThrowDagger::UGA_ThrowDagger()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor; // 瞄准态是有实例的中间态
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	CooldownDuration = 6.f; // 测试值
	// CooldownGameplayEffectClass 在 BP 子类里设（或 C++ 里 = UGE_ThrowDaggerCooldown::StaticClass()）
}

void UGA_ThrowDagger::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// 只检查不消耗：CD/蓝在真正投掷（OnConfirmEvent）时才 commit
	if (!CommitCheck(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 进入瞄准态"));
	EnterAimingState();

	auto* WaitConfirm = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(
		this, LOLGameplayTags::Event_Input_ThrowConfirm, nullptr, true, true);
	WaitConfirm->EventReceived.AddDynamic(this, &UGA_ThrowDagger::OnConfirmEvent);
	WaitConfirm->ReadyForActivation();

	auto* WaitCancel = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(
		this, LOLGameplayTags::Event_Input_Repressed, nullptr, true, true);
	WaitCancel->EventReceived.AddDynamic(this, &UGA_ThrowDagger::OnCancelEvent);
	WaitCancel->ReadyForActivation();
}

void UGA_ThrowDagger::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	ExitAimingState();
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_ThrowDagger::OnConfirmEvent(FGameplayEventData Payload)
{
	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 左键确认 → 投掷"));

	// 真正投掷才 commit（消耗 CD + 蓝）
	if (!CommitAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo))
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	if (MontageTask) MontageTask->JumpToSection("Cast"); // v2 再配合 AnimNotify 出匕首 + 延迟 End

	const FVector AimDir = GetAimDirection();

	// 单机下 HasAuthority() 恒 true，直接 spawn。PvP 见第 8 步。
	if (K2_HasAuthority())
	{
		SpawnProjectile(AimDir);
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UGA_ThrowDagger::OnCancelEvent(FGameplayEventData Payload)
{
	// 只对本槽位（E）的再按响应；不 commit → 无损取消
	if (!Payload.TargetTags.HasTag(LOLGameplayTags::Ability_Slot_E)) return;

	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] E 再按 → 取消（不耗 CD）"));
	if (MontageTask) MontageTask->JumpToSection("Cancel");
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
}

void UGA_ThrowDagger::EnterAimingState()
{
	if (AimingGE)
	{
		AimingGEHandle = ApplyGameplayEffectToOwner(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
			AimingGE.GetDefaultObject(), GetAbilityLevel());
	}

	if (ThrowMontage)
	{
		MontageTask = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
			this, NAME_None, ThrowMontage, 1.f, FName("Targeting"));
		MontageTask->ReadyForActivation();
	}

	// 客户端本地：屏幕中央瞄准框
	if (CurrentActorInfo && CurrentActorInfo->IsLocallyControlled() && AimingReticleFX)
	{
		const AActor* Avatar = GetAvatarActorFromActorInfo();
		AimReticleComponent = UGameplayStatics::SpawnEmitterAttached(
			AimingReticleFX, Avatar->GetRootComponent(), NAME_None,
			FVector(80.f, 0.f, 0.f), FRotator::ZeroRotator, EAttachLocation::KeepRelativeOffset);
	}
}

void UGA_ThrowDagger::ExitAimingState()
{
	if (AimingGEHandle.IsValid())
	{
		BP_RemoveGameplayEffectFromOwnerWithHandle(AimingGEHandle);
		AimingGEHandle.Invalidate();
	}
	if (AimReticleComponent)
	{
		AimReticleComponent->DestroyComponent();
		AimReticleComponent = nullptr;
	}
}

FVector UGA_ThrowDagger::GetAimDirection() const
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character) return FVector::ForwardVector;

	const AController* Controller = Character->GetController();
	if (!Controller) return Character->GetActorForwardVector();

	// 第三人称：朝相机朝向的水平分量投掷（只用 Yaw，和 GA_Flash 一致）
	const FRotator ControlRotation = Controller->GetControlRotation();
	return FRotationMatrix(FRotator(0.f, ControlRotation.Yaw, 0.f)).GetUnitAxis(EAxis::X);
}

void UGA_ThrowDagger::SpawnProjectile(const FVector& Direction)
{
	if (!ProjectileClass) return;

	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	const FVector SpawnLoc = Character
		? Character->GetActorLocation() + Character->GetActorForwardVector() * 60.f + FVector(0.f, 0.f, 60.f)
		: GetAvatarActorFromActorInfo()->GetActorLocation();
	const FRotator SpawnRot = Direction.Rotation();

	AThrowDaggerProjectile* Proj = GetWorld()->SpawnActorDeferred<AThrowDaggerProjectile>(
		ProjectileClass, FTransform(SpawnRot, SpawnLoc));
	if (Proj)
	{
		Proj->Initialize(Direction, ThrowSpeed, DamageGE, BaseDamage, Character);
		Proj->FinishSpawning(FTransform(SpawnRot, SpawnLoc));
	}
}
```

---

## 第 4 步：左键分流（`AHeroCombatCharacter`）

### 4a. `Public/GAS/HeroCombatCharacter.h`

在 `private:` 里（`RouteBasicAttackInput` 旁边）加：

```cpp
	void RouteThrowConfirmInput();
```

### 4b. `Private/GAS/HeroCombatCharacter.cpp`

改 `BasicAttackPressed`，并新增 `RouteThrowConfirmInput`：

```cpp
void AHeroCombatCharacter::BasicAttackPressed()
{
	// 瞄准态：左键语义 = 确认投掷，拦截，不普攻
	if (AbilitySystemComponent &&
		AbilitySystemComponent->HasMatchingGameplayTag(LOLGameplayTags::State_Throw_Aiming))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 左键被瞄准态拦截 → 发投掷确认"));
		RouteThrowConfirmInput();
		return;
	}

	RouteBasicAttackInput();
	if (!HasAuthority()) ServerSubmitBasicAttackInput();
}

void AHeroCombatCharacter::RouteThrowConfirmInput()
{
	FGameplayEventData EventData;
	EventData.EventTag = LOLGameplayTags::Event_Input_ThrowConfirm;
	EventData.Instigator = this;
	EventData.Target = this;
	AbilitySystemComponent->HandleGameplayEvent(EventData.EventTag, &EventData);
}
```

> `HeroCombatCharacter.cpp` 已经 include 了 `GAS/LOLGameplayTags.h`，无需新增。

---

## 第 5 步：E 再按取消（`UMyAbilitySystemComponent`）

### 5a. `Private/GAS/MyAbilitySystemComponent.cpp`

顶部加 include：

```cpp
#include "GAS/LOLGameplayTags.h"
```

改 `AbilityInputTagPressed`（加「已激活 → 再按」分支，其余不动）：

```cpp
void UMyAbilitySystemComponent::AbilityInputTagPressed(const FGameplayTag& SlotTag)
{
	if (!SlotTag.IsValid()) return;

	const FGameplayAbilitySpecHandle* Handle = SlotAbilityMap.Find(SlotTag);
	if (!Handle) return;

	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(*Handle);
	if (!Spec) return;

	// 【新增】槽位能力已激活（如投掷技能正在瞄准）→ 再次按下 = 广播「再按」事件，交给能力自己决定
	if (Spec->GetAbilityInstances().Num() > 0)
	{
		FGameplayEventData EventData;
		EventData.EventTag = LOLGameplayTags::Event_Input_Repressed;
		EventData.Instigator = GetOwnerActor();
		EventData.Target = GetOwnerActor();
		EventData.TargetTags.AddTag(SlotTag);
		HandleGameplayEvent(EventData.EventTag, &EventData);   // 广播给 WaitGameplayEvent，只发一次
		return;
	}

	// 未激活 → 正常激活（保留原有 OnInputTriggered 策略检查）
	const UMyGameplayAbility* Ability = Cast<UMyGameplayAbility>(Spec->Ability);
	if (Ability && Ability->ActivationPolicy != EMyAbilityActivationPolicy::OnInputTriggered)
	{
		return;
	}

	TryActivateAbility(*Handle, true);
}
```

> 顺带修你 `AbilityInputTagReleased` 的隐患：它把 `SendGameplayEventToActor` 放在 `for` 循环里，同一个事件发了 N 次。应挪到循环外只发一次。

---

## 第 6 步：编辑器资产配置

### 6a.（可选，要 CD 才做）`GE_ThrowDaggerCooldown`

复制 `GE_FlashCooldown` 改名。C++ 版的话把 `State_Cooldown_Flash` 换成 `State_Cooldown_ThrowDagger`（需补一个原生 tag）。然后在 `GA_ThrowDagger` BP 里把 `Cooldown Gameplay Effect Class` 设成它。v1 可不做，无 CD 也能跑。

### 6b. `GE_ThrowDaggerAiming`（关键）

新建 **GameplayEffect**（蓝图即可）：
- Duration Policy = **Infinite**（或长时长 30s 兜底）。
- Components 加 **TargetTagsGameplayEffectComponent**，Target Tags 填 `State.Throw.Aiming`。
- （可选）加一个 Movement Speed 乘区减速，模拟瞄准时减速。

### 6c. `GE_ThrowDaggerDamage`

新建 **Instant** GameplayEffect，用 SetByCaller `Data.Damage`（和你 `GE_BasicAttackDamage` 一样的套路）。投射物已经 `SetSetByCallerMagnitude(Data.Damage, DamageAmount)` 填好值。

### 6d. `AM_ThrowDagger`（Montage）

新建 AnimMontage，三个 section：
- `Targeting`（循环）= 你的 `targeting_MSA`
- `Cast` = 你的 `cast`
- `Cancel` = 你的 `cancel_addtive`（或直接 blend out）

`开始_additive` 和 `cancel_addtive` 是 **additive 层动画**：放 AnimBP 里，用 `State.Throw.Aiming` 驱动 `LayeredBlendPerBone` 做上身举刀/收刀姿态，不要塞进主 Montage slot。

### 6e. `BP_ThrowDaggerProjectile`

新建 `AThrowDaggerProjectile` 的蓝图子类，把 `SpinParticle` 组件的 **Template** 设成你已有的「匕首空中翻滚」Cascade 粒子。

### 6f. `ChampionKit`（英雄的 AbilitySet 数据资产）

在 GrantAbility 里加一条：
- `SlotTag` = `Ability.Slot.E`
- `Ability` = `GA_ThrowDagger`（或其蓝图子类）
- `AbilityLevel` = 1

> 授权后 `OnGiveAbility` 会打 `[Passive] OnGiveAbility ...` 日志，确认 E 槽位进了 `SlotAbilityMap`。

### 6g. `DA_InputConfig`

确认 `AbilityInputActions` 里有一条 `InputAction = IA_E` → `SlotTag = Ability.Slot.E`（让 E 键能路由到 E 槽位）。

### 6h. 普攻被动加阻断（兜底）

给 `GA_ThreeHitPassive`（或其 BP 子类）的 **Activation Blocked Tags** 加 `State.Throw.Aiming`。这样即使左键路由漏判，GAS 也会在 `TryActivateAbility` 层拦下普攻。

### 6i. `GA_ThrowDagger` 蓝图子类

新建 `GA_ThrowDagger` 的蓝图子类，填：`ProjectileClass`、`DamageGE`、`AimingGE`、`ThrowMontage`、`AimingReticleFX`、`CooldownGameplayEffectClass`（若做 6a）。

---

## 第 7 步：单机验证清单（按顺序）

1. **编译**通过。
2. 进游戏，看 `[Passive] OnGiveAbility: GA_ThrowDagger ... 标签: Ability.Slot.E`。
3. **按 E** → 日志 `进入瞄准态`，角色播放 Targeting 循环动画，屏幕出现瞄准框。
4. **按 E 再按** → 日志 `E 再按 → 取消`，动画收，瞄准框消失，**CD 没进**（此时再按 E 还能立刻进瞄准）。
5. **按 E → 左键** → 日志 `左键确认 → 投掷`，匕首 spawn、直线向前飞、翻滚，命中目标掉血（`GE_ThrowDaggerDamage`）。
6. **瞄准中左键不普攻**：瞄准状态下按左键，不应触发普攻（日志里没有 `BasicAttackPressed 入口到达` 的普攻路径）。
7. **命中后销毁**：匕首命中目标或撞墙后 `Destroy`。

每步若不符合预期，先看对应 UE_LOG 打点有没有走到，再往下查——别猜。

---

## 第 8 步：PvP 联网补充（后续，单机不用管）

单机下 `K2_HasAuthority()` 恒 true，`SpawnProjectile` 直接走。联网时要补一条「客户端 → 服务器」的投掷通道，**注意：`UGameplayAbility` 是 UObject 不是 Actor，`UFUNCTION(Server)` 在它身上无效**，得放到 `AHeroCombatCharacter`（你已经用 `ServerSubmitBasicAttackInput` 证明了这个模式）：

1. `HeroCombatCharacter.h` 加 `UFUNCTION(Server, Reliable) void ServerSubmitThrowDagger(const FVector& AimDir);`
2. `HeroCombatCharacter.cpp` 里 `RouteThrowConfirmInput` 同时：本地发 `Event.Input.ThrowConfirm`（客户端表现）+ `if (!HasAuthority()) ServerSubmitThrowDagger(AimDir);`
3. `ServerSubmitThrowDagger_Implementation` 里重新校验（CD/是否还在瞄准态/活着），然后 spawn 投射物。

关键点：**GameplayEvent 不复制**，客户端 `HandleGameplayEvent` 只触发客户端的 `WaitGameplayEvent`，服务器那份能力实例收不到，所以投射物 spawn 必须走显式 RPC、且只在服务器做（`AThrowDaggerProjectile` 的 `OnOverlap` 已用 `HasAuthority()` 卡死）。

---

## 命中的「不同目标不同粒子」（下一步）

投射物 `OnOverlap` 里 `TODO` 处接 GameplayCue：给可命中目标挂 `Target.Type.Hero` / `Minion` / `Structure` 原生 tag，命中时 `ResolveTargetType(OtherActor)` 查 tag → 触发对应 `GameplayCue.ThrowDagger.Hit.Hero` / `.Minion` …。出手瞬间 burst 用 `GameplayCue.ThrowDagger.Spawn`。瞄准框是本地表现、不复制。
