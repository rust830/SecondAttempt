// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "GameplayTagContainer.h"
#include "HeroCombatCharacter.generated.h"

class UAbilitySystemComponent;
class UAbilitySet;

/** GAS 角色基类：ASC 生命周期 + 英雄技能组授权 + 普攻/槽位输入路由。ALOLCharacter 继承它。 */
UCLASS(Blueprintable)
class LOL_API AHeroCombatCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()
public:
	AHeroCombatCharacter();
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystemComponent; }

	/** 绑定到普攻（左键）Started 事件。 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") void BasicAttackPressed();
	/** 由 PlayerController 转发的槽位输入（QWER/DF）。 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") void AbilityInputTagPressed(FGameplayTag SlotTag);

	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;

protected:
	/** 英雄技能组（被动 + QWER），数据资产。 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Abilities") TObjectPtr<UAbilitySet> ChampionKit;

	UPROPERTY(EditDefaultsOnly, Category="Hero|Tags") FGameplayTag PassiveSlotTag;
	UPROPERTY(EditDefaultsOnly, Category="Hero|Tags") FGameplayTag BasicAttackInputTag;

	UPROPERTY(Transient) TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UFUNCTION(Server, Reliable) void ServerSubmitBasicAttackInput();

private:
	void InitializeAbilityActorInfo();
	void RouteBasicAttackInput();
	void RouteThrowConfirmInput();
	bool bAbilitiesGranted = false;
};
