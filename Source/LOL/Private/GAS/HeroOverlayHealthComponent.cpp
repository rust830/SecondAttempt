// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroOverlayHealthComponent.h"

#include "AbilitySystemComponent.h"
#include "Blueprint/UserWidget.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "UI/HeroOverlayHealthWidget.h"

void UHeroOverlayHealthComponent::BeginPlay()
{
	Super::BeginPlay();

	// 先挂点、再绑数据。两件事互不依赖，但先摆好位置能避免「第一帧血条在默认位置闪一下」。
	AttachToOwnerMeshSocket();

	// SetWidget / InitWidget 在 Super::BeginPlay 里已经跑完，所以 GetUserWidgetObject() 在这里是有效的。
	// 默认观察 Owner —— 敌人的血条组件挂在敌人自己身上，不用手配。
	SetObservedActor(ObservedActorOverride ? ObservedActorOverride.Get() : GetOwner());
}

void UHeroOverlayHealthComponent::AttachToOwnerMeshSocket()
{
	// 显式关掉吸附（None）：完全按蓝图里配的父子关系走。
	if (OwnerMeshSocketName.IsNone())
	{
		return;
	}

	const AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// 用 FindComponentByClass 而不是 Cast<ACharacter>->GetMesh()：
	// 敌人 / 小兵 / 以后可能的纯装饰演员都可能是别的类，能查出骨骼就够。
	USkeletalMeshComponent* OwnerMesh = Owner->FindComponentByClass<USkeletalMeshComponent>();
	if (!OwnerMesh)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("%s：想吸附到插槽 '%s'，但 %s 上没有 SkeletalMeshComponent。保留蓝图里配的挂位。"),
			*GetName(), *OwnerMeshSocketName.ToString(), *GetNameSafe(Owner));
		return;
	}

	// 插槽不存在时必须【只警告不吸附】：AttachToComponent 传一个不存在的插槽名不会报错，
	// 而是静默挂到 mesh 原点上 —— 表现就是「血条跑到脚底」，比直接不挂更难查。
	if (!OwnerMesh->DoesSocketExist(OwnerMeshSocketName))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("%s：%s 的骨骼上没有插槽 '%s'（拼写不对？还是插槽建在别的骨架上了？）。保留蓝图里配的挂位。"),
			*GetName(), *GetNameSafe(OwnerMesh), *OwnerMeshSocketName.ToString());
		return;
	}

	// SnapToTargetNotIncludingScale：把相对变换整体对齐到插槽，这正是「吸附」的语义 ——
	// 位置由骨骼决定，不再叠加蓝图里手填的那份偏移（两处偏移打架是这类血条最难查的问题）。
	AttachToComponent(OwnerMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, OwnerMeshSocketName);

	// 需要微调就叠这个，而不是回头去动插槽。
	if (!SocketOffset.IsNearlyZero())
	{
		SetRelativeLocation(SocketOffset);
	}
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

	// 头顶血条只要血，不要能量 —— 所以这里只绑两条，不是照抄 HUD 那四条。
	static const TArray<FGameplayAttribute> VitalsAttributes = {
		UHeroCombatAttributeSet::GetHealthAttribute(),
		UHeroCombatAttributeSet::GetMaxHealthAttribute(),
	};
	Binding.BindAttributes(ASC, VitalsAttributes, this, &UHeroOverlayHealthComponent::OnVitalsAttributeChanged);

	// 死亡用标签，不用 Health == 0 —— 和技能那边同一个判据。
	Binding.BindTag(ASC, LOLGameplayTags::State_Dead, this, &UHeroOverlayHealthComponent::OnDeadTagChanged);
}

void UHeroOverlayHealthComponent::Unbind()
{
	// 摘句柄的记账共用一份（见 FHUDAttributeBinding）—— 原来这里是第二份手写的同款代码。
	Binding.Unbind();

	// BoundASC 是「现在观察谁」的一部分，解绑时才清；它和 Binding 内部那份弱引用分工不同：
	// BoundASC 给 Refresh() 读数值用，Binding 那份只在解绑时用来找回委托。
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

void UHeroOverlayHealthComponent::OnVitalsAttributeChanged(const FOnAttributeChangeData& Data)
{
	Refresh();
}

void UHeroOverlayHealthComponent::OnDeadTagChanged(const FGameplayTag ChangedTag, int32 NewCount)
{
	Refresh();
}
