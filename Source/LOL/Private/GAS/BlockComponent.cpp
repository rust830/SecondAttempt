// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/BlockComponent.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/GE_BlockImmune.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"

UBlockComponent::UBlockComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// 和 GA_Block 里 BlockingGE 的默认值同一个套路：原生类做默认值，蓝图想换再覆盖。
	// 不给默认值的话免疫是静默失效的（OnBlockSucceeded 里那条警告只在运行时才看得到），
	// 表现就是「挡是挡住了，但防护罩不出现、后续伤害也不免」。
	ImmuneEffect = UGE_BlockImmune::StaticClass();
}

bool UBlockComponent::TryMitigateIncomingDamage(UAbilitySystemComponent* TargetASC, AActor* DamageSource, float& InOutDamage)
{
	// 契约：InOutDamage 是唯一输出，函数一定就地改写它。
	// 返回值只是给调用方（ExecCalc）判断「要不要再输出 modifier」用的快捷方式，不要只看返回值。
	if (!TargetASC || InOutDamage <= 0.f) return false;

	UBlockComponent* Block = FindOn(TargetASC);
	if (!Block) return false;   // 没挂组件 = 这个角色根本不会格挡/免疫，不需要任何特判

	// 1) 免疫优先：免疫期间再挨打不该再触发一次格挡（否则会刷出第二个防护罩）。
	if (TargetASC->HasMatchingGameplayTag(LOLGameplayTags::State_BlockImmune))
	{
		InOutDamage *= Block->ImmuneDamageMultiplier;
		return InOutDamage <= 0.f;
	}

	// 2) 窗口：标签在 = 窗口还开着（时长就是 GE_Blocking 的 Duration）。
	const bool bWindowOpen = TargetASC->HasMatchingGameplayTag(LOLGameplayTags::State_Blocking);

	// 3) 宽限：窗口刚关 RTT/2 内到达的伤害也算挡到（见头文件里那段说明）。
	bool bInGrace = false;
	if (!bWindowOpen && Block->bGraceAvailable)
	{
		const UWorld* World = Block->GetWorld();
		const float Now = World ? World->GetTimeSeconds() : 0.f;
		bInGrace = (Now - Block->WindowCloseServerTime) <= Block->GetGraceSeconds();
	}

	if (!bWindowOpen && !bInGrace) return false;

	if (Block->bRequireFacingAttacker && !Block->IsInFront(DamageSource))
	{
		// 方向不对：不消耗宽限，这一击就是没挡到。
		return false;
	}

	// 宽限被用掉就作废：一次窗口只挡一下。
	if (bInGrace)
	{
		Block->bGraceAvailable = false;
	}

	return Block->OnBlockSucceeded(DamageSource, InOutDamage);
}

float UBlockComponent::GetGraceSeconds() const
{
	if (!bCompensatePing) return 0.f;

	// 取【挨打那一方】的 ping。GetPingInMilliseconds 只在服务端返回真实值（ExactPing 不是 UPROPERTY、
	// 不复制，客户端拿到的是 CompressedPing*4 的量化值），而整套判定也只在服务端跑，所以没问题。
	// 没有 PlayerState（AI / 训练假人）就按 0 延迟算 —— 它们本来也没有「客户端预测早于服务端」的问题。
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const APlayerState* PS = Pawn ? Pawn->GetPlayerState() : nullptr;
	const float PingSeconds = PS ? PS->GetPingInMilliseconds() * 0.001f : 0.f;

	return FMath::Clamp(PingSeconds * 0.5f, 0.f, MaxGraceSeconds);
}

void UBlockComponent::BindToAbilitySystem(UAbilitySystemComponent* InASC)
{
	if (CachedASC == InASC) return;

	if (CachedASC && BlockingTagDelegateHandle.IsValid())
	{
		CachedASC->UnregisterGameplayTagEvent(BlockingTagDelegateHandle,
			LOLGameplayTags::State_Blocking, EGameplayTagEventType::NewOrRemoved);
		BlockingTagDelegateHandle.Reset();
	}

	CachedASC = InASC;
	if (!CachedASC) return;

	// 两端都注册：判定只在服务端跑，但客户端那份 ASC 上注册着也不会有副作用
	// （回调里只写 WindowCloseServerTime / bGraceAvailable 两个本地字段）。
	BlockingTagDelegateHandle = CachedASC->RegisterGameplayTagEvent(
		LOLGameplayTags::State_Blocking, EGameplayTagEventType::NewOrRemoved)
		.AddUObject(this, &UBlockComponent::OnBlockingTagChanged);
}

void UBlockComponent::OnBlockingTagChanged(const FGameplayTag Tag, int32 NewCount)
{
	if (NewCount > 0)
	{
		// 开窗：宽限作废（还在窗口里，用不上它）。
		bGraceAvailable = false;
		return;
	}

	// 关窗：记下时刻并放开一次宽限。
	if (const UWorld* World = GetWorld())
	{
		WindowCloseServerTime = World->GetTimeSeconds();
		bGraceAvailable = true;
	}
}

bool UBlockComponent::OnBlockSucceeded(AActor* DamageSource, float& InOutDamage)
{
	UAbilitySystemComponent* ASC = CachedASC;

	// 只在权威端结算。非权威端必须【早退且不改数值】：本地把伤害改成 0、服务端照扣，两边就不一致了。
	if (!ASC || !ASC->IsOwnerActorAuthoritative()) return false;

	// ① 窗口只吃一次：立刻摘掉 GE_Blocking。
	//    复用那个「非权威端也真的摘」的入口，客户端那份预测副本走同一条路。
	//    ⚠️ 这里是【在 ExecCalc 里】改活动 GE 列表：安全 —— ApplyGameplayEffectSpec 全程持
	//    GAMEPLAY_EFFECT_SCOPE_LOCK，增删会排队到本次施加结束后才落地（见方案文档 §1.2）。
	UMyAbilitySystemComponent::RemoveGrantedTagEffects(
		ASC, FGameplayTagContainer(LOLGameplayTags::State_Blocking));

	// ② 这一击的减免。
	InOutDamage *= BlockedDamageMultiplier;

	// ③ 挂免疫（刷新式：先摘后挂，任何时刻最多一层），免疫 GE 上挂着 GameplayCue.Block → 防护罩。
	if (ImmuneEffect)
	{
		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		Context.AddSourceObject(this);
		Context.AddInstigator(GetOwner(), GetOwner());

		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(ImmuneEffect, 1.f, Context);
		if (Spec.IsValid())
		{
			Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_BlockImmuneDuration, ImmuneDuration);
			UMyAbilitySystemComponent::RemoveGrantedTagEffects(
				ASC, FGameplayTagContainer(LOLGameplayTags::State_BlockImmune));
			ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
		}
		else
		{
			// 不静默：免疫挂不上 = 防护罩不出现、后续伤害也不免，屏幕上看起来就是「格挡没生效」。
			UE_LOG(LogTemp, Warning, TEXT("[Block] 免疫 GE(%s) 的 Spec 无效 → 本次格挡没有免疫"), *GetNameSafe(ImmuneEffect));
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[Block] ImmuneEffect 没配 → 格挡了但不会有免疫和防护罩"));
	}

	UE_LOG(LogTemp, Warning, TEXT("[Block] 格挡成功: 剩余伤害=%.1f 免疫=%.2fs 来源=%s"),
		InOutDamage, ImmuneDuration, *GetNameSafe(DamageSource));

	return InOutDamage <= 0.f;
}

bool UBlockComponent::IsInFront(AActor* DamageSource) const
{
	// 拿不到来源（或来源和自己重叠）就不拦：这套判定的原则是「宽松 > 误判」——
	// 误判成「没挡到」玩家会认为格挡坏了，误判成「挡到了」只是少吃一次伤害。
	const AActor* Owner = GetOwner();
	if (!Owner || !DamageSource) return true;

	const FVector ToSource = DamageSource->GetActorLocation() - Owner->GetActorLocation();
	const FVector ToSourceFlat = FVector(ToSource.X, ToSource.Y, 0.f);
	if (ToSourceFlat.IsNearlyZero()) return true;

	const float CosAngle = FVector::DotProduct(Owner->GetActorForwardVector().GetSafeNormal2D(), ToSourceFlat.GetSafeNormal());
	return CosAngle >= FMath::Cos(FMath::DegreesToRadians(FacingHalfAngleDeg));
}

UBlockComponent* UBlockComponent::FindOn(UAbilitySystemComponent* ASC)
{
	// 注意是 AvatarActor 不是 OwnerActor：ASC 挂在 PlayerState（AMyPlayerState）上，
	// 组件挂在角色身上。走 OwnerActor 会永远返回 null —— 而且是静默的，
	// 表现就是「格挡再也不生效」，没有任何日志。
	AActor* Avatar = ASC ? ASC->GetAvatarActor() : nullptr;
	return Avatar ? Avatar->FindComponentByClass<UBlockComponent>() : nullptr;
}

void UBlockComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (CachedASC && BlockingTagDelegateHandle.IsValid())
	{
		CachedASC->UnregisterGameplayTagEvent(BlockingTagDelegateHandle,
			LOLGameplayTags::State_Blocking, EGameplayTagEventType::NewOrRemoved);
	}
	BlockingTagDelegateHandle.Reset();
	CachedASC = nullptr;

	Super::EndPlay(EndPlayReason);
}
