// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayAbilitySpecHandle.h"
#include "GameplayTagContainer.h"
#include "AbilitySet.generated.h"

class UGameplayAbility;
class UGameplayEffect;
class UAttributeSet;
class UAbilitySystemComponent;
/**
 *
 */
USTRUCT(BlueprintType)
struct FAbilitySet_GrantAbility {
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly) FGameplayTag SlotTag;
	UPROPERTY(EditDefaultsOnly) TSubclassOf<UGameplayAbility> Ability;
	UPROPERTY(EditDefaultsOnly, meta = (ClampMin = "1")) int32 AbilityLevel = 1;
};
USTRUCT(BlueprintType)
struct FAbilitySet_GrantEffect {
	GENERATED_BODY()
	UPROPERTY(EditDefaultsOnly) TSubclassOf<UGameplayEffect> Effect;
	UPROPERTY(EditDefaultsOnly, meta = (ClampMin = "0.01")) float EffectLevel = 1.f;
};
UCLASS()
class LOL_API UAbilitySet final: public UPrimaryDataAsset
{
	GENERATED_BODY()
public:
	/**
	 * 授技能 / GE / 属性集 / 标签。只授不撤。
	 *
	 * 两个调用点：AHeroCombatCharacter（英雄技能组：被动 + QWER）、
	 * AMyPlayerState（召唤师技能 D/F）—— 都是「授完跟着角色一辈子」，
	 * 没有「撤」这个动作，所以句柄直接丢弃。
	 */
	void GiveToAbilitySystem(UAbilitySystemComponent* ASC)const;

	/**
	 * 和上面同一个函数，但把授出去的【技能句柄】带回来。
	 *
	 * 【为什么需要这个重载】竞技场的海克斯是在【运行时】给技能的，将来必须能收回 ——
	 * 而 ClearAllAbilities 会把这个英雄身上所有能力（QWER / 召唤师技能 / 被动 / 形态切换）
	 * 一起清掉。只能按句柄精确撤 —— 撤回的实现在 UArenaLoadoutComponent::RemoveAugmentAt。
	 *
	 * 【只收技能句柄】GE / 属性集 / 标签那几条通道不返回句柄：GE 由施加方自己留
	 * FActiveGameplayEffectHandle（见 FArenaAugmentRuntime），属性集和标签本来就不支持摘除。
	 */
	void GiveToAbilitySystem(UAbilitySystemComponent* ASC, TArray<FGameplayAbilitySpecHandle>& OutGrantedAbilityHandles) const;

	UPROPERTY(EditDefaultsOnly,Category="Abilities")
	TArray<FAbilitySet_GrantAbility> GrantAbility;
	UPROPERTY(EditDefaultsOnly, Category="Abilities")
	TArray<FAbilitySet_GrantEffect> GrantEffect;
	UPROPERTY(EditDefaultsOnly, Category="Abilities")
	TArray<TSubclassOf<UAttributeSet>> GrantAttribute;

	/**
	 * 授给自己的标签（loose tag）。
	 *
	 * ⚠️⚠️ **UE 5.8 里默认【不复制】**：`AddLooseGameplayTags(C, 1, TagRepState=None)`，
	 * 而 `UpdateTagMap_Internal`（AbilitySystemComponent.cpp:795）只在 `TagRepState > None`
	 * 时才写复制容器 ⇒ **服务端挂的这类标签，客户端读不到**。
	 *
	 * 所以这条通道只适合「只有服务端要读」的场合（英雄技能组现在就是）。
	 * 要让客户端也知道，用 GE 的 TargetTags 组件授 Granted Tag
	 * （随 active GE 的 FastArray 复制，两端必然一致）。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Abilities")
	FGameplayTagContainer GrantTag;
};
