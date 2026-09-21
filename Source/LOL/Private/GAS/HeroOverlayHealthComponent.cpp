// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroOverlayHealthComponent.h"

#include "AbilitySystemComponent.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/Actor.h"

#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "UI/HeroOverlayHealthWidget.h"

void UHeroOverlayHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// SetWidget / InitWidget 在 Super::BeginPlay 里已经跑完，所以 GetUserWidgetObject() 在这里是有效的。
	// 默认观察 Owner —— 敌人的血条组件挂在敌人自己身上，不用手配。
	SetObservedActor(ObservedActorOverride ? ObservedActorOverride.Get() : GetOwner());
}

void UHeroOverlayHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Unbind();

	Super::EndPlay(EndPlayReason);
}

void UHeroOverlayHealthComponent::SetObservedActor(AActor* InActor)
{
	if (InActor && !IsValid(InActor))
	{
		InActor = nullptr;
	}

	// 幂等：同一个目标重复调不重绑。
	if (InActor == ObservedActor.Get())
	{
		Refresh();
		return;
	}

	// 先摘后挂 —— 目标被销毁 / 换人走的是同一条路。
	Unbind();

	ObservedActor = InActor;
	Bind();

	// 【绑定不触发初始值】：绑完必须显式刷一次，否则第一次受伤之前血条是满的（错的）。
	Refresh();
}

void UHeroOverlayHealthComponent::Bind()
{
	AActor* Target = ObservedActor.Get();
	if (!Target)
	{
		return;
	}

	// 用项目自己的静态入口（它比蓝图库那条路多兜了一层，见其注释）。
	UAbilitySystemComponent* ASC = UMyAbilitySystemComponent::FindAbilitySystemComponent(Target);
	if (!ASC)
	{
		// 没有 ASC 的目标（纯 Actor 装饰物）不报错，血条保持空。
		return;
	}

	BoundASC = ASC;

	HealthHandle = ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetHealthAttribute())
		.AddUObject(this, &UHeroOverlayHealthComponent::OnHealthChanged);
	MaxHealthHandle = ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxHealthAttribute())
		.AddUObject(this, &UHeroOverlayHealthComponent::OnMaxHealthChanged);

	// 死亡用标签，不用 Health == 0 —— 和技能那边同一个判据。
	DeadTagHandle = ASC
		->RegisterGameplayTagEvent(LOLGameplayTags::State_Dead, EGameplayTagEventType::NewOrRemoved)
		.AddUObject(this, &UHeroOverlayHealthComponent::OnDeadTagChanged);
}

void UHeroOverlayHealthComponent::Unbind()
{
	if (UAbilitySystemComponent* ASC = BoundASC.Get())
	{
		if (HealthHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetHealthAttribute()).Remove(HealthHandle);
		}
		if (MaxHealthHandle.IsValid())
		{
			ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetMaxHealthAttribute()).Remove(MaxHealthHandle);
		}
		if (DeadTagHandle.IsValid())
		{
			ASC->UnregisterGameplayTagEvent(DeadTagHandle, LOLGameplayTags::State_Dead, EGameplayTagEventType::NewOrRemoved);
		}
	}

	HealthHandle.Reset();
	MaxHealthHandle.Reset();
	DeadTagHandle.Reset();
	BoundASC = nullptr;
}

void UHeroOverlayHealthComponent::Refresh()
{
	// 每次现场取 Widget：WidgetComponent 可能在运行时被换掉（换样式），缓存它反而要处理失效。
	// 命名避开成员变量（UWidgetComponent 自己有 Widget 成员），不然 -Wshadow / C4458 会报遮蔽。
	UHeroOverlayHealthWidget* TargetWidget = Cast<UHeroOverlayHealthWidget>(GetUserWidgetObject());
	if (!TargetWidget)
	{
		return;
	}

	FHUDVitalsView Vitals;
	bool bIsDead = false;

	if (const UAbilitySystemComponent* ASC = BoundASC.Get())
	{
		Vitals.Health = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetHealthAttribute());
		Vitals.MaxHealth = ASC->GetNumericAttribute(UHeroCombatAttributeSet::GetMaxHealthAttribute());
		Vitals.HealthPercent = Vitals.MaxHealth > 0.f ? FMath::Clamp(Vitals.Health / Vitals.MaxHealth, 0.f, 1.f) : 0.f;
		bIsDead = ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Dead);
	}

	// 隐藏策略留在组件上（每个敌人可以配得不一样），Widget 只负责照做。
	TargetWidget->ApplyOverlayVitals(Vitals, bIsDead, bHideWhenFullHealth, bHideWhenDead);
}

void UHeroOverlayHealthComponent::OnHealthChanged(const FOnAttributeChangeData& Data)
{
	Refresh();
}

void UHeroOverlayHealthComponent::OnMaxHealthChanged(const FOnAttributeChangeData& Data)
{
	Refresh();
}

void UHeroOverlayHealthComponent::OnDeadTagChanged(const FGameplayTag ChangedTag, int32 NewCount)
{
	Refresh();
}
