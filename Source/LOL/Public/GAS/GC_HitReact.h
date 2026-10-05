// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GAS/HeroCombatCharacter.h"
#include "GC_HitReact.generated.h"

/**
 * 受击表现：按「伤害从哪个方向来」播对应的受击动画。
 *
 * 【为什么是一条 cue 而不是四条】GameplayCueSet 只按弹射出的那个标签查表，
 * 不会给子标签各跑一份 cue —— 方向和命中类型都只能走 Parameters 传进来
 * （同 UGC_ThrowDaggerHit 处理 Target.Hero/Void/Terrain 的做法）。
 *
 * 【发起点】UHeroCombatAttributeSet::PostGameplayEffectExecute 在服务端
 * ExecuteGameplayCue → 先在本端跑一次、再多播到各客户端各自跑一次。
 * 放在属性集那个唯一的伤害汇聚点上，是为了让「所有伤害来源」都自动带上受击表现，
 * 而不是每加一个技能就得记得补一句。
 *
 * 【致命的那一下也要发】这一下 cue 不只是播动画：
 *   ① 方向要记到角色身上（CacheHitDirection）—— 死亡蒙太奇靠它挑「向前扑还是向后倒」。
 *      致死的那一刻手上的凶手信息不进网络，客户端只能靠这里提前拿到。
 *   ② 但动画【不播】：死亡蒙太奇同一帧就接管了，两个一起上会打架。
 *      靠 Parameters.AggregatedSourceTags 里的 Data.Lethal 分辨。
 *
 * 蓝图子类必须命名为 GC_HitReact（见 LOLGameplayTags.h 里那段命名约定的说明）。
 */
UCLASS()
class LOL_API UGC_HitReact : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_HitReact();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;
};
