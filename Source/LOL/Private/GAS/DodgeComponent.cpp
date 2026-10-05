// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/DodgeComponent.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/GE_EnergyRestore.h"
#include "GAS/DodgeCameraModifier.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"

UDodgeComponent::UDodgeComponent()
{
	PrimaryComponentTick.bCanEverTick = false;

	// 和 UBlockComponent 同一个套路：原生类做默认值，蓝图想换再覆盖。
	// 不给默认值的话回蓝是静默失效的（下面那条警告只在运行时看得到），
	// 表现就是「完美闪避的特效全对、蓝量却一动不动」。
	EnergyRestoreEffect = UGE_EnergyRestore::StaticClass();

	// 镜头修改器同理：类留空 = 不推镜，但子弹时间照跑（不会有一条日志告诉你少了什么）。
	CameraModifierClass = UDodgeCameraModifier::StaticClass();
}

bool UDodgeComponent::TryNegateIncomingDamage(UAbilitySystemComponent* TargetASC, AActor* DamageSource, float& InOutDamage)
{
	// 契约和 UBlockComponent::TryMitigateIncomingDamage 完全一致：
	// InOutDamage 是唯一输出，函数就地改写它；返回值只是「这一击是不是被完全吃掉」的快捷方式。
	if (!TargetASC || InOutDamage <= 0.f) return false;

	// 窗口 = GE_DodgeWindow 授予的标签还挂着。
	// 判定和窗口时长同源，不开定时器、不自己记时刻 —— 漂移和「忘了摘」这两类 bug 都不存在。
	if (!TargetASC->HasMatchingGameplayTag(LOLGameplayTags::State_Dodge_Window)) return false;

	UDodgeComponent* Dodge = FindOn(TargetASC);
	if (!Dodge)
	{
		// 窗口挂着但身上没有组件：只有「窗口是别处挂的」这一种可能，属于接线错误，不静默。
		UE_LOG(LogTemp, Warning, TEXT("[Dodge] 目标挂着 State.Dodge.Window 但身上没有 UDodgeComponent → 这一击不会算完美闪避"));
		return false;
	}

	return Dodge->OnPerfectDodge(DamageSource, InOutDamage);
}

bool UDodgeComponent::OnPerfectDodge(AActor* DamageSource, float& InOutDamage)
{
	UAbilitySystemComponent* ASC = CachedASC;

	// 只在权威端结算。非权威端必须【早退且不改数值】：本地把伤害改成 0、服务端照扣，两边就不一致了。
	if (!ASC || !ASC->IsOwnerActorAuthoritative()) return false;

	// ① 窗口只吃一次：立刻摘掉 GE_DodgeWindow。
	//    ⚠️ 这里是【在 ExecCalc 里】改活动 GE 列表 —— 和格挡那边一样是安全的：
	//    ApplyGameplayEffectSpec 全程持 GAMEPLAY_EFFECT_SCOPE_LOCK，增删会排队到本次施加结束后才落地。
	UMyAbilitySystemComponent::RemoveGrantedTagEffects(
		ASC, FGameplayTagContainer(LOLGameplayTags::State_Dodge_Window));

	// ② 这一击完全吃掉。
	InOutDamage = 0.f;

	// ③ 回蓝。
	if (EnergyRestoreEffect && PerfectDodgeEnergyRestore > 0.f)
	{
		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		Context.AddSourceObject(this);
		Context.AddInstigator(GetOwner(), GetOwner());

		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(EnergyRestoreEffect, 1.f, Context);
		if (Spec.IsValid())
		{
			Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_EnergyRestore, PerfectDodgeEnergyRestore);
			ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
		}
		else
		{
			// 不静默：回蓝挂不上 = 特效全对但蓝条不动，屏幕上看起来就是「完美闪避没奖励」。
			UE_LOG(LogTemp, Warning, TEXT("[Dodge] 回蓝 GE(%s) 的 Spec 无效 → 本次完美闪避没有回蓝"), *GetNameSafe(EnergyRestoreEffect));
		}
	}
	else if (PerfectDodgeEnergyRestore > 0.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Dodge] EnergyRestoreEffect 没配 → 完美闪避了但没有回蓝"));
	}

	// ④ 表现：子弹时间（世界流速，全场都慢，服务端设、WorldSettings 复制）+ 镜头推近（只本机）。
	if (bBulletTimeEnabled && BulletTimeScale < 1.f && BulletTimeRealSeconds > 0.f)
	{
		UWorld* World = GetWorld();
		AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
		if (Settings && !bBulletTimeApplied)
		{
			// 存【原始成员】而不是 GetEffectiveTimeDilation()：后者把 CinematicTimeDilation /
			// DemoPlayTimeDilation 也乘了进去，拿它回写等于把那一路的缩放永久烙进世界时间（见 GA_DeathHarvest）。
			SavedTimeDilation = Settings->TimeDilation;
			bBulletTimeApplied = true;

			UGameplayStatics::SetGlobalTimeDilation(this, BulletTimeScale);

			// ★ 时长必须在 SetGlobalTimeDilation【之后】折算：世界计时器跟的是膨胀后的 delta。
			//   不折算的话「0.4 秒」在 0.35 流速下要跑 1.14 真实秒，慢放会拖到肉眼觉得卡住。
			World->GetTimerManager().SetTimer(BulletTimeTimer, this, &ThisClass::EndBulletTime,
				ScaledWorldSeconds(BulletTimeRealSeconds), /*bLoop=*/false);

			UE_LOG(LogTemp, Warning, TEXT("[Dodge] 完美闪避子弹时间 %.2fs（真实）：世界流速 %.2f → %.2f"),
				BulletTimeRealSeconds, SavedTimeDilation, Settings->GetEffectiveTimeDilation());
		}
	}

	ApplyCameraEffect();

	UE_LOG(LogTemp, Warning, TEXT("[Dodge] 完美闪避成功: 回蓝=%.0f 来源=%s"),
		PerfectDodgeEnergyRestore, *GetNameSafe(DamageSource));

	return true;
}

void UDodgeComponent::EndBulletTime()
{
	// 幂等：EndPlay 里还会兜底再调一次。漏了这一句的报错不是「技能坏了」，
	// 而是「整个世界一直卡在慢动作里」—— 比顿帧还原失败只轻一点。
	if (!bBulletTimeApplied) return;
	bBulletTimeApplied = false;

	if (UWorld* World = GetWorld())
	{
		UGameplayStatics::SetGlobalTimeDilation(this, SavedTimeDilation);
		World->GetTimerManager().ClearTimer(BulletTimeTimer);
	}

	// 镜头交给修改器自己淡出（AlphaOutTime）：这里只按开关，不自己插值。
	// 淡出期间 ModifyCamera 还会被调，Alpha 一路降到 0 后引擎自动把它从相机管理器摘掉。
	if (UCameraModifier* Modifier = ActiveCameraModifier.Get())
	{
		Modifier->DisableModifier(/*bImmediate=*/false);
	}

	UE_LOG(LogTemp, Warning, TEXT("[Dodge] 完美闪避子弹时间结束：世界流速还原成 %.2f"), SavedTimeDilation);
}

void UDodgeComponent::ApplyCameraEffect()
{
	// 只改【自己的】屏幕：敌人闪避不该动你的镜头。
	// 判定跑在服务端，所以这里必须确认「本机就是闪避的那个人」——主机自己闪避成立，
	// 远端玩家在服务端闪避时服务端这里没有他的相机管理器（返回 null，静默跳过是对的）。
	APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn || !Pawn->IsLocallyControlled()) return;
	if (!CameraModifierClass) return;

	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	APlayerCameraManager* CamMgr = PC ? PC->PlayerCameraManager : nullptr;
	if (!CamMgr) return;

	// 防叠加：上一次还在淡出时又中一次，直接复用管理器里那个（重复 Add 会堆出第二个推镜）。
	if (UCameraModifier* Existing = CamMgr->FindCameraModifierByClass(CameraModifierClass))
	{
		Existing->EnableModifier();
		ActiveCameraModifier = Existing;
		return;
	}

	UCameraModifier* Modifier = CamMgr->AddNewCameraModifier(CameraModifierClass);
	ActiveCameraModifier = Modifier;

	if (!Modifier)
	{
		// 不静默：类配错（比如指到一个抽象类）时 AddNewCameraModifier 返回 null，
		// 表现是「子弹时间有、推镜没有」，从屏幕上分不出是没配还是配错了。
		UE_LOG(LogTemp, Warning, TEXT("[Dodge] 镜头修改器(%s)创建失败 → 本次完美闪避没有 FOV 推镜"),
			*GetNameSafe(CameraModifierClass));
	}
}

float UDodgeComponent::ScaledWorldSeconds(float RealSeconds) const
{
	const UWorld* World = GetWorld();
	const AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
	const float Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.f;

	// 世界计时器读的是【膨胀后】的 delta，所以「等 RealSeconds 那么久的真实时间」
	// = 等 RealSeconds × 当前流速 那么多的世界秒。没有慢放时退化成恒等。（同 GA_DeathHarvest）
	return RealSeconds * Dilation;
}

void UDodgeComponent::BindToAbilitySystem(UAbilitySystemComponent* InASC)
{
	CachedASC = InASC;
}

UDodgeComponent* UDodgeComponent::FindOn(UAbilitySystemComponent* ASC)
{
	// 注意是 AvatarActor 不是 OwnerActor：ASC 挂在 PlayerState 上，组件挂在角色身上。
	// 走 OwnerActor 会永远返回 null —— 而且是静默的，表现就是「完美闪避再也不生效」。
	AActor* Avatar = ASC ? ASC->GetAvatarActor() : nullptr;
	return Avatar ? Avatar->FindComponentByClass<UDodgeComponent>() : nullptr;
}

void UDodgeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 兜底还原世界流速：角色在慢放期间被销毁（掉出世界 / 回合结束）的话，
	// 不还原就是整个世界永远慢动作。
	EndBulletTime();

	CachedASC = nullptr;
	ActiveCameraModifier = nullptr;

	Super::EndPlay(EndPlayReason);
}
