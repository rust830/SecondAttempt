// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/HeroCombatCharacter.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/AbilitySet.h"
#include "MyPlayerState.h"
#include "AbilitySystemComponent.h"
#include "GAS/LOLGameplayTags.h"

AHeroCombatCharacter::AHeroCombatCharacter()
{
	bReplicates = true;
	// 原生标签：CDO 构造阶段用字符串 RequestGameplayTag 拿不到（返回 None），必须直接用原生标签对象。
	PassiveSlotTag = LOLGameplayTags::Ability_Slot_Passive;
	BasicAttackInputTag = LOLGameplayTags::Event_Input_BasicAttack;
}

void AHeroCombatCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	InitializeAbilityActorInfo();
}

void AHeroCombatCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	InitializeAbilityActorInfo();
}

void AHeroCombatCharacter::InitializeAbilityActorInfo()
{
	AMyPlayerState* PS = GetPlayerState<AMyPlayerState>();
	if (!PS) return;
	AbilitySystemComponent = PS->GetAbilitySystemComponent();
	AbilitySystemComponent->InitAbilityActorInfo(PS, this);

	// 服务端授予英雄技能组（被动 + QWER）。召唤师技能在 PlayerState 里授予。只授一次。
	if (HasAuthority() && !bAbilitiesGranted && ChampionKit)
	{
		ChampionKit->GiveToAbilitySystem(AbilitySystemComponent);
		bAbilitiesGranted = true;
	}
}

void AHeroCombatCharacter::BasicAttackPressed()
{
	// 瞄准态：左键语义 = 确认投掷，拦截，不普攻
	if (AbilitySystemComponent &&
		AbilitySystemComponent->HasMatchingGameplayTag(LOLGameplayTags::State_Throw_Aiming))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 左键被瞄准态拦截 → 发投掷确认"));
		RouteThrowConfirmInput();
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[Passive] BasicAttackPressed 入口到达"));
	RouteBasicAttackInput();
	if (!HasAuthority()) ServerSubmitBasicAttackInput();
}

void AHeroCombatCharacter::RouteThrowConfirmInput()
{
	FGameplayEventData EventData;
	EventData.EventTag = LOLGameplayTags::Event_Input_ThrowConfirm;
	EventData.Instigator = this;
	EventData.Target = this;
	AbilitySystemComponent->HandleGameplayEvent(EventData.EventTag, &EventData);
}

void AHeroCombatCharacter::ServerSubmitBasicAttackInput_Implementation()
{
	RouteBasicAttackInput();
}

void AHeroCombatCharacter::RouteBasicAttackInput()
{
	UMyAbilitySystemComponent* ASC = Cast<UMyAbilitySystemComponent>(AbilitySystemComponent);
	if (!ASC) { UE_LOG(LogTemp, Warning, TEXT("[Passive] RouteBasicAttackInput: ASC 为空")); return; }

	// 普攻 → 激活「被动槽位」里的技能，同时发 GameplayEvent（被动靠它推进连段）。
	// 直接用原生标签对象：CDO 构造阶段设的成员可能因蓝图覆盖/时序是 None，运行时用原生标签最稳。
	const FGameplayTag PassiveTag = LOLGameplayTags::Ability_Slot_Passive;
	const FGameplayTag AttackTag = LOLGameplayTags::Event_Input_BasicAttack;

	const FGameplayAbilitySpecHandle Handle = ASC->GetHandleForSlot(PassiveTag);
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 槽位[%s] 标签有效=%d Handle有效=%d"), *PassiveTag.ToString(), PassiveTag.IsValid(), Handle.IsValid());
	if (Handle.IsValid())
	{
		const bool bActivated = ASC->TryActivateAbility(Handle, true);
		UE_LOG(LogTemp, Warning, TEXT("[Passive] TryActivateAbility 返回=%d"), bActivated);
	}

	FGameplayEventData EventData;
	EventData.EventTag = AttackTag;
	EventData.Instigator = this;
	ASC->HandleGameplayEvent(AttackTag, &EventData);
}

void AHeroCombatCharacter::AbilityInputTagPressed(FGameplayTag SlotTag)
{
	if (UMyAbilitySystemComponent* ASC = Cast<UMyAbilitySystemComponent>(AbilitySystemComponent))
	{
		ASC->AbilityInputTagPressed(SlotTag);
	}
}
