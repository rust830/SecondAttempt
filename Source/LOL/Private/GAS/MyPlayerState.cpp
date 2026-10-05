// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/MyPlayerState.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/AbilitySet.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/HeroCombatCharacter.h"
#include "Net/UnrealNetwork.h"

AMyPlayerState::AMyPlayerState()
{
	AbilitySystemComponent = CreateDefaultSubobject<UMyAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
	CombatAttributes = CreateDefaultSubobject<UHeroCombatAttributeSet>(TEXT("CombatAttributes"));
}

void AMyPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// COND_None：等级是公开信息（记分板要显示），不是只有自己看得见的东西。
	DOREPLIFETIME_CONDITION(AMyPlayerState, Level, COND_None);
}

void AMyPlayerState::SetHeroLevel(int32 NewLevel)
{
	// 权威数据：客户端改它只会被下一次复制覆盖掉，那就成了「本地看着升了、别人看着没升」。
	// 直接挡掉并吵一声，不静默生效。
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Hero] SetHeroLevel(%d) 在非权威端被调用 → 已忽略（等级由服务端复制下来）"), NewLevel);
		return;
	}

	const int32 Clamped = FMath::Max(1, NewLevel);   // 等级 1 起，没有 0 级
	if (Clamped == Level)
	{
		return;   // 幂等：没变就不广播、不重算
	}

	Level = Clamped;

	// 服务端【不会】收到自己的 OnRep_，所以这里显式广播一次。
	// 两端因此走的是同一条路径：先落到 Level，再广播。
	OnLevelChanged.Broadcast(Level);

	// 数值重算走这条直连，不走 OnLevelChanged 委托 —— 委托是给 UI 的（BlueprintAssignable），
	// 让它同时承担「重算数值」会让「谁在什么时候重算」变成一件要靠订阅顺序保证的事。
	//
	// 属性集挂在 PlayerState 自己身上，所以就算这一刻还没 Pawn，数值也不会丢：
	// 下一次 PossessedBy → InitializeAbilityActorInfo 会带着新等级再施加一遍。
	if (APawn* MyPawn = GetPawn())
	{
		if (AHeroCombatCharacter* Hero = Cast<AHeroCombatCharacter>(MyPawn))
		{
			Hero->ApplyChampionStats();
		}
	}
}

void AMyPlayerState::OnRep_Level()
{
	// 客户端【不重算】：属性的值本身会随属性集复制下来，在这里再算一遍就是第二条真相，
	// 而且这条会和复制赛跑（本地算出来的值可能被服务端那份覆盖，也可能不）。
	// 只广播，让 UI 跟着动。
	OnLevelChanged.Broadcast(Level);
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
