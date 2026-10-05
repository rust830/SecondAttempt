// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_StatusMontage.h"
#include "GAS/HeroCombatCharacter.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameplayEffectTypes.h"

AGC_StatusMontage::AGC_StatusMontage()
{
	// GE 移除时自动销毁本 cue actor，不用手写清理。
	bAutoDestroyOnRemove = true;

	// 不改角色 transform，只是就地播动画，不需要附着到 owner。
	bAutoAttachToOwner = false;

	// 同一个状态可能被重复挂（刷新时长）：允许重复 OnActive，第二次挂上时动画重播，刷新才有反馈。
	bAllowMultipleOnActiveEvents = true;
}

bool AGC_StatusMontage::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return true;
	}

	// 先挑蒙太奇（子类可能按受击方向换），记下来，OnRemove 停的就是这条。
	ActiveMontage = PickMontage(MyTarget, Parameters);
	PlayStatusMontage(MyTarget, ActiveMontage);

	// 永远返回 true：Montage 没配只是「没表现」，不是失败。
	// 返回 false 会被当成 cue 挂载失败，actor 当场销毁，OnRemove 就没机会收尾了。
	return true;
}

bool AGC_StatusMontage::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// 到期 / 被打断 / 被驱散都会走到这里。MyTarget 可能已失效（角色被销毁），StopStatusMontage 对 null 免疫。
	StopStatusMontage(MyTarget);
	return true;
}

UAnimMontage* AGC_StatusMontage::PickMontage(AActor* Target, const FGameplayCueParameters& Parameters) const
{
	return Montage;
}

UAnimInstance* AGC_StatusMontage::GetTargetAnimInstance(AActor* Target) const
{
	const ACharacter* Character = Cast<ACharacter>(Target);
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	return Mesh ? Mesh->GetAnimInstance() : nullptr;
}

void AGC_StatusMontage::PlayStatusMontage(AActor* Target, UAnimMontage* InMontage)
{
	if (!InMontage)
	{
		if (!bLoggedMissingMontage)
		{
			bLoggedMissingMontage = true;
			UE_LOG(LogTemp, Warning,
				TEXT("[StatusMontage] %s 的 Montage 为空：BP 里没配，状态不会播动画"), *GetNameSafe(this));
		}
		return;
	}

	// ★ 已经死了就【不要播】硬控蒙太奇。这道门是"死亡动画偶尔不播"的根治点。
	//
	// 【为什么必须在这里挡】三条硬控蒙太奇（AM_Stun_Kallari / AM_KnockBack_Kallari / AM_KnockUp）
	// 全都走 DefaultSlot —— 和死亡蒙太奇【同一条 slot group】。而下面那句 Montage_Play 传的是
	// bStopOtherMontages=true（硬控默认值），会按 slot group 清场。
	//
	// 致命的那一下如果本身带击退，链路是：
	//   UExecCalc_Damage 结算 → 伤害 GE + 击退 GE 同帧挂上
	//   → UGE_Knockback 的 cue【OnActive】→ 这里播 AM_KnockBack_Kallari
	//   → 死亡广播 → State.Dead 复制到本端 → EnterDeathState 播 AM_Death_A
	// 两条蒙太奇抢同一个 slot group，谁后到谁赢 —— 而 cue 的 OnActive 是【多播】过来的，
	// 落点比 State.Dead 标签的复制晚一帧是常态。赢了的那条是击退，死亡动画就被顶没了。
	//
	// 【为什么表现为"偶尔"】只有"致命那一击带硬控"时才会走到这里。普通的普攻/技能打死
	// 不会挂击退 GE，也就不会有这条 cue，死亡动画每次都能正常播。
	//
	// 【和 PlayHitReact 的关系】那边已经有一道同样的 IsDead() 门（受击动画同理，
	// 死亡蒙太奇在播时不该被抢）。两处各管各的，判据同一个：State.Dead 标签。
	if (const AHeroCombatCharacter* Hero = Cast<AHeroCombatCharacter>(Target))
	{
		if (Hero->IsDead())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[StatusMontage] %s 已死亡 → 跳过 %s 的播放（不然会顶掉死亡蒙太奇）"),
				*GetNameSafe(Target), *GetNameSafe(InMontage));
			return;
		}
	}

	UAnimInstance* Anim = GetTargetAnimInstance(Target);
	if (!Anim)
	{
		return;
	}

	// 最后一个参数 bStopOtherMontages 默认 true：硬控应当打断当前正在播的攻击/移动 Montage。
	Anim->Montage_Play(InMontage, MontagePlayRate, EMontagePlayReturnType::MontageLength, 0.f, bStopOtherMontages);
}

void AGC_StatusMontage::StopStatusMontage(AActor* Target)
{
	UAnimMontage* MontageToStop = ActiveMontage ? ActiveMontage.Get() : Montage.Get();
	if (!MontageToStop)
	{
		return;
	}

	if (UAnimInstance* Anim = GetTargetAnimInstance(Target))
	{
		// 已经播完、或早被别的 Montage 顶掉时这里是 no-op：引擎只会在该 Montage 确实还是当前实例时才停。
		Anim->Montage_Stop(MontageStopBlendOut, MontageToStop);
	}
}
