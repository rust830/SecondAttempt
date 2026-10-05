// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "MyPlayerState.generated.h"

class UMyAbilitySystemComponent;
class UHeroCombatAttributeSet;
class UAbilitySet;

/** 等级变了。服务端在 SetLevel 里广播，客户端在 OnRep_Level 里广播 —— 两端各响一次，各响在各端。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FHeroLevelChangedSignature, int32, NewLevel);

/**
 *  PlayerState hosting the AbilitySystemComponent for GAS.
 */
UCLASS()
class LOL_API AMyPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AMyPlayerState();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	UMyAbilitySystemComponent* GetMyAbilitySystemComponent() const { return AbilitySystemComponent; }

	// -----------------------------------------------------------------------
	// 英雄等级
	//
	// 【为什么等级住在 PlayerState】两个理由，都不是「顺手」：
	//   ① 死亡会重建/重置 Pawn 上的东西，等级必须活过去；
	//   ② 要被等级改的那些数值本来就挂在 PlayerState 的属性集上 —— 等级和它改的东西放一起，
	//      才对得上「换个 Pawn 不该丢等级」。
	//
	// 等级改数值这件事不在这里【算】：属性集在 PlayerState 上，但「这个英雄的数值表」在 Pawn 上
	// （ChampionStats / HeroDefinition）。PlayerState 只负责说一声「等级变了，你重算」，
	// 不去读那张表 —— 它不该知道英雄是谁。见 SetLevel。
	// -----------------------------------------------------------------------

	/**
	 * 名字带 Hero 前缀不是啰嗦：`AActor` 已经有一个 `GetLevel()`（返回 `ULevel*`），
	 * 同名会被 UHT 挡下来，同名的 C++ 重载也会把父类那个藏掉。别改回去。
	 */
	UFUNCTION(BlueprintPure, Category = "Hero|Stats")
	int32 GetHeroLevel() const { return Level; }

	/**
	 * 改等级。【只在服务端生效】—— 客户端调会被拒并记一条日志，
	 * 因为等级是权威数据，客户端那份由复制覆盖，静默"生效"只会造成两端不一致。
	 *
	 * 幂等：和当前等级相同就直接 return，不广播、不重算。
	 */
	UFUNCTION(BlueprintCallable, Category = "Hero|Stats")
	void SetHeroLevel(int32 NewLevel);

	/** 等级变了。给 UI（等级徽章）用；数值重算是另一条路（见 SetLevel）。 */
	UPROPERTY(BlueprintAssignable, Category = "Hero|Stats")
	FHeroLevelChangedSignature OnLevelChanged;

protected:
	virtual void BeginPlay() override;

	/** 等级从服务端复制下来。客户端不做数值重算 —— 属性集的值自己会复制下来。 */
	UFUNCTION()
	void OnRep_Level();

	/**
	 * 英雄等级，1 起。复制（不是 ReplicatedUsing 之外的第二条通道）。
	 *
	 * 默认 1 = 「1 级 = 只吃基础值」，和属性集构造函数建出来的结果一致 ——
	 * 所以没接等级系统之前，它的存在不改变任何数值。
	 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Level, Category = "Hero|Stats")
	int32 Level = 1;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Abilities", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UMyAbilitySystemComponent> AbilitySystemComponent;

	/** Replicated hero attributes owned beside the ASC, never on a transient pawn. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Abilities", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UHeroCombatAttributeSet> CombatAttributes;

	/** 召唤师技能组（D/F），所有英雄共享。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UAbilitySet> SummonerSpells;
};
