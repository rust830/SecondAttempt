// Fill out your copyright notice in the Description page of Project Settings.


#include "MyPlayerState.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/AbilitySet.h"
#include "GAS/HeroCombatAttributeSet.h"

AMyPlayerState::AMyPlayerState()
{
	AbilitySystemComponent = CreateDefaultSubobject<UMyAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
	CombatAttributes = CreateDefaultSubobject<UHeroCombatAttributeSet>(TEXT("CombatAttributes"));
}

UAbilitySystemComponent* AMyPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AMyPlayerState::BeginPlay()
{
	Super::BeginPlay();

	// 服务端授予召唤师技能（D/F）。单机下 HasAuthority() 恒为 true。
	// 注意：BeginPlay 早于 InitAbilityActorInfo，所以召唤师技能组只放「技能 + 标签」，不放开局效果。
	if (HasAuthority() && SummonerSpells)
	{
		SummonerSpells->GiveToAbilitySystem(AbilitySystemComponent);
	}
}
