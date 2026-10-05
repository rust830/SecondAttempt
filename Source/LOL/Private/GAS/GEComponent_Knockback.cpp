// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GEComponent_Knockback.h"
#include "GAS/LOLGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Character.h"
#include "GameplayEffect.h"

void UGEComponent_Knockback::OnGameplayEffectApplied(FActiveGameplayEffectsContainer& ActiveGEContainer, FGameplayEffectSpec& GESpec, FPredictionKey& PredictionKey) const
{
	Super::OnGameplayEffectApplied(ActiveGEContainer, GESpec, PredictionKey);

	// 位移只在权威端执行（和伤害/死亡同一个纪律）。客户端靠位置复制看到结果。
	if (!ActiveGEContainer.OwnerIsNetAuthority)
	{
		return;
	}

	UAbilitySystemComponent* ASC = ActiveGEContainer.Owner;
	if (!ASC)
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(ASC->GetAvatarActor());
	if (!Character)
	{
		return;
	}

	// 冲量：SetByCaller，施加方填（没填 = 纯硬直，不动）。
	// 两个都允许负值：Horizontal 为负 = 往施加者方向拉，Vertical 为负 = 向下砸（dive kick 用这个）。
	const float Horizontal = GESpec.GetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackImpulse, /*WarnIfNotFound=*/false, /*Default=*/0.f);
	const float Vertical = GESpec.GetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackLaunch, /*WarnIfNotFound=*/false, /*Default=*/0.f);
	if (FMath::IsNearlyZero(Horizontal) && FMath::IsNearlyZero(Vertical))
	{
		// 只有「两个都是 0」才算纯硬直。用 <= 0 判会把「向下砸」当成没填而不执行。
		return;
	}

	// 方向：① 命中法线取反（远离命中面，只取水平）② 目标-施加者 ③ 目标朝向。
	//
	// ⚠️ 两个坑，都真实踩过（飞踢「击退不明显」的主因之一）：
	//
	// A) 击中角色时 ImpactNormal 是【垂直】的（胶囊侧面朝着打它的人，法线水平向外 —— 但斜着打
	//    到胶囊顶/底弧面时法线是竖直的，飞踢自上而下砸就是这种）。取反再清 Z = (0,0,0)，
	//    于是静默落到兜底②。以前这里写的是 `Direction.IsNearlyZero()` 判完就算了，
	//    现在是每级各自判 —— 清完 Z 之后再判，才能掉到下一级。
	//
	// B) 兜底② 对「上下叠着」的情况同样是零向量：飞踢是从【正上方】砸下来的，
	//    目标与施加者的水平差接近 0，GetSafeNormal2D() 出来的还是零 ——
	//    再落到兜底③ 角色自己的朝向，而那个朝向由玩家鼠标决定，和"该往哪飞"完全无关。
	//    ⇒ 所以要引入兜底③'：撤离方向用【施加者的水平朝向】（攻击推力方向），最后才用目标朝向。
	FVector Direction = FVector::ZeroVector;
	bool bFromHitNormal = false;   // 只给下面那条诊断日志用
	const FGameplayEffectContextHandle Context = GESpec.GetContext();
	if (Context.IsValid())
	{
		const FHitResult* Hit = Context.Get()->GetHitResult();
		if (Hit && Hit->bBlockingHit)
		{
			Direction = -Hit->ImpactNormal;
			Direction.Z = 0.f;
			bFromHitNormal = !Direction.IsNearlyZero();
		}

		// ① 打空（法线是垂直的 / 无效）→ 用「目标 - 施加者」的水平方向。
		if (Direction.IsNearlyZero())
		{
			if (const AActor* Causer = Context.Get()->GetEffectCauser())
			{
				Direction = (Character->GetActorLocation() - Causer->GetActorLocation()).GetSafeNormal2D();

				// ② 上下叠着（水平差≈0）→ 用【施加者朝哪打】。自上而下砸时这就是"往前方推"，
				//    比目标自己的朝向有意义：目标可能正背对着攻击者。
				if (Direction.IsNearlyZero())
				{
					Direction = Causer->GetActorForwardVector().GetSafeNormal2D();
				}
			}
		}
	}
	// ③ 连 EffectContext 都没有（理论上不该发生）→ 目标自己的水平朝向兜底。
	if (Direction.IsNearlyZero())
	{
		Direction = Character->GetActorForwardVector().GetSafeNormal2D();
	}

	// 诊断：这条日志是「冲量通道到底走没走到」的唯一判据（Instant GE 的 GE 组件
	// 会不会被调用、SetByCaller 有没有填上去，从画面上都看不出来 ——
	// 数值太小被阻尼吃掉的表现和「压根没施加」长得一模一样）。
	// ⚠️ ASC->GetAvatarActor() 和 Character 是【同一个人】（上面就是从 avatar 转出来的），
	// 原来这两个 %s 打的都是目标自己的名字，把「施加者」那一格写成了目标，
	// 排查飞踢击退方向时照着日志找错了人。施加者要从 EffectContext 取。
	const AActor* Causer = Context.IsValid() ? Context.Get()->GetEffectCauser() : nullptr;
	UE_LOG(LogTemp, Log, TEXT("[Knockback] %s 推 %s 冲量=%+.0f 抛量=%+.0f（方向来源=%s）"),
		*GetNameSafe(Causer), *GetNameSafe(Character),
		Horizontal, Vertical,
		bFromHitNormal ? TEXT("命中法线") : TEXT("目标-施加者/施加者朝向"));

	Character->LaunchCharacter(Direction * Horizontal + FVector::UpVector * Vertical, /*bXYOverride=*/true, /*bZOverride=*/true);
}
