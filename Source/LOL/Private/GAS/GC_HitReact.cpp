// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_HitReact.h"
#include "GAS/LOLGameplayTags.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"

UGC_HitReact::UGC_HitReact()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_HitReact;
}

bool UGC_HitReact::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	AHeroCombatCharacter* Victim = Cast<AHeroCombatCharacter>(MyTarget);
	if (!Victim)
	{
		// cue 是挂在被打的人身上的。目标不是本项目角色（场景道具之类）就直接认输，
		// 不返回 false 的话调用方会以为「播过了」。
		return false;
	}

	// 伤害来源：优先 EffectCauser —— 本项目的 EffectContext 里它放的就是【角色本人】
	// （见 UExecCalc_Damage 构造函数那段注释：instigator 是 PlayerState、causer 是角色）。
	//
	// 退回 Instigator 时要小心：那多半是个 PlayerState，它没有有意义的世界坐标，
	// 直接拿它的位置算方向会得到「伤害从世界原点打来」——所以先换回它的 Pawn。
	const AActor* Source = Parameters.EffectCauser.Get();
	if (!Source)
	{
		AActor* Instigator = Parameters.Instigator.Get();
		Source = Cast<APawn>(Instigator);
		if (!Source)
		{
			if (const APlayerState* InstigatorPS = Cast<APlayerState>(Instigator))
			{
				Source = InstigatorPS->GetPawn();
			}
		}
	}
	if (!Source)
	{
		return false;
	}

	// ① 方向一律先记下 —— 不管这一下致不致命。致命的那一下正是死亡蒙太奇要用它的时候。
	const EHitDirection Direction = AHeroCombatCharacter::ResolveHitDirection(Victim, Source->GetActorLocation());
	Victim->CacheHitDirection(Direction);

	// ② 致命的那一下不播受击动画：死亡蒙太奇马上接管，两个一起上会打架。
	//    （分辨方式见类注释 —— 属性集在致命时会给 cue 挂上 Data.Lethal。）
	if (Parameters.AggregatedSourceTags.HasTag(LOLGameplayTags::Data_Lethal))
	{
		return true;
	}

	Victim->PlayHitReact(Direction);

	// 播没播成（没配蒙太奇 / 在冷却 / 已经死了）都算「这条 cue 处理过了」：
	// 返回 false 会被当成「没有表现可播」，而这个 cue 的职责里包含「记方向」这件事，
	// 那件事已经做完了。真正的失败在上面两个 early return 里。
	return true;
}
