// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GC_EmpoweredHit.generated.h"

class UNiagaraSystem;
class USoundBase;

/**
 * 强化普攻【命中】时的额外效果：在命中点炸一下 + 一声。
 *
 * 为什么单独开一个 cue 而不是让 UGA_ThreeHitPassive 自己 Spawn：
 * 命中结算（ApplyServerHit）是纯服务端的（客户端没有权威 HitResult），在那里生成粒子只有主机看得到。
 * 走服务端 ExecuteGameplayCue 才会把参数复制到各客户端、各自本地播一遍 —— 和 GC_ThrowDaggerHit 同一条路。
 *
 * 时机和「踩中完美窗口」那一刻是两回事：踩中窗口只给本人一声 QTE 反馈（见
 * UThreeHitPassiveData::PerfectWindowSuccessSound），强化击真打到人才是这里的额外效果。
 * 两处都用同一个 cue：破隐那一击也是强化的，命中了同样该炸。
 *
 * 蓝图子类必须命名为 GC_EmpoweredHit（cue 的 BP 命名规则见 LOLGameplayTags.h）。
 */
UCLASS()
class LOL_API UGC_EmpoweredHit : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGC_EmpoweredHit();

	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	/** 命中点炸开的 NS，默认指向 /Game/LOL/Niagara/NS_PerfectSuccess。留空 = 只有音效。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredHit")
	TSoftObjectPtr<UNiagaraSystem> HitSystem;

	/** 命中点的一次性音效（可空）。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredHit")
	TSoftObjectPtr<USoundBase> HitSound;

	/** NS 里那个位置参数（含 User. 前缀）。NS_PerfectSuccess 用的是 User.ImpactPos。 */
	UPROPERTY(EditDefaultsOnly, Category = "EmpoweredHit")
	FName ImpactParameter = TEXT("User.ImpactPos");

private:
	/**
	 * 「NS 没配」只打一次警告。Static cue 跑的是 CDO，所以这一份标记是全场的。
	 * 不用 UPROPERTY：UHT 不支持 mutable 属性，而 OnExecute 是 const。
	 */
	mutable bool bLoggedMissingSystem = false;
};
