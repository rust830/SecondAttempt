// 斗魂竞技场：GameState 的实现。

#include "GAS/ArenaGameState.h"

#include "GAS/ArenaPlayerState.h"
#include "Net/UnrealNetwork.h"

AArenaGameState::AArenaGameState()
{
	Contenders.SetNum(ArenaMatch::ContenderCount);
}

void AArenaGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION(AArenaGameState, ArenaPhase, COND_None);
	DOREPLIFETIME_CONDITION(AArenaGameState, RoundNumber, COND_None);
	DOREPLIFETIME_CONDITION(AArenaGameState, PhaseEndServerTime, COND_None);
	DOREPLIFETIME_CONDITION(AArenaGameState, Contenders, COND_None);
	DOREPLIFETIME_CONDITION(AArenaGameState, Winner, COND_None);
	DOREPLIFETIME_CONDITION(AArenaGameState, RoundPlan, COND_None);
	DOREPLIFETIME_CONDITION(AArenaGameState, LastRoundWinnerSlot, COND_None);
	DOREPLIFETIME_CONDITION(AArenaGameState, LastRoundHpLoss, COND_None);
}

// ---------------------------------------------------------------------------
// 相位 / 回合
// ---------------------------------------------------------------------------

float AArenaGameState::GetPhaseRemainingSeconds() const
{
	if (PhaseEndServerTime <= 0.f)
	{
		return 0.f;   // 这个相位不倒计时（战斗 / 结算 / 未开始）
	}

	// 两种坐标系不一样：PhaseEndServerTime 是服务端世界时间，
	// GetServerWorldTimeSeconds() 是"换算到本机时钟上的服务端时间"——
	// 后者已经把网络延迟估进去了，所以直接减就是剩余秒。
	return FMath::Max(0.f, PhaseEndServerTime - GetServerWorldTimeSeconds());
}

void AArenaGameState::SetArenaPhase(EArenaPhase NewPhase, int32 InRoundNumber, float DurationSeconds)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 非权威端调用了 SetArenaPhase，已忽略。相位只在服务端推进。"));
		return;
	}

	ArenaPhase = NewPhase;
	RoundNumber = InRoundNumber;

	// 0 是"没有倒计时"的哨兵值，所以 DurationSeconds <= 0 要落成 0，
	// 不能落成 GetServerWorldTimeSeconds() + 0（那样会被当成"倒计时已经走完"）。
	PhaseEndServerTime = (DurationSeconds > 0.f)
		? (GetServerWorldTimeSeconds() + DurationSeconds)
		: 0.f;

	// 服务端收不到自己的 OnRep_，显式广播一次，让两端走同一条路。
	OnArenaPhaseChanged.Broadcast(ArenaPhase);
}

// ---------------------------------------------------------------------------
// 参赛者
// ---------------------------------------------------------------------------

AArenaPlayerState* AArenaGameState::GetContender(int32 Index) const
{
	return Contenders.IsValidIndex(Index) ? Contenders[Index].Get() : nullptr;
}

bool AArenaGameState::AreContendersReady() const
{
	// 两个都要在，而且是"有效"不是"非空"——被销毁的 PlayerState 指针还在
	// 但 IsValid 是 false，只看非空会把一局已经没有对手的比赛判成可以开打。
	for (const TObjectPtr<AArenaPlayerState>& Contender : Contenders)
	{
		if (!IsValid(Contender))
		{
			return false;
		}
	}

	return Contenders.Num() > 0;
}

void AArenaGameState::SetContender(int32 Index, AArenaPlayerState* PlayerState)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 SetContender，已忽略。"));
		return;
	}

	if (!Contenders.IsValidIndex(Index))
	{
		// 需求是 1V1，第三个玩家进来就会走到这里。不是崩溃点，但也绝不能静默 ——
		// 静默的后果是"第三个人进来了但没人知道他去哪了"，下一步就是他在场上乱跑。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 想把人放进参赛者下标 %d，但只有 %d 个位置 —— 已忽略。"
				 "竞技场是 1V1，多出来的人不该被分到这里。"),
			Index, Contenders.Num());
		return;
	}

	Contenders[Index] = PlayerState;
	OnRep_Contenders();
}

void AArenaGameState::ResetContenders()
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 ResetContenders，已忽略。"));
		return;
	}

	Contenders.Reset();
	Contenders.SetNum(ArenaMatch::ContenderCount);
	OnRep_Contenders();
}

AArenaPlayerState* AArenaGameState::GetOpponentOf(const AArenaPlayerState* PlayerState) const
{
	if (!PlayerState)
	{
		return nullptr;
	}

	for (const TObjectPtr<AArenaPlayerState>& Contender : Contenders)
	{
		if (IsValid(Contender) && Contender.Get() != PlayerState)
		{
			return Contender.Get();
		}
	}

	return nullptr;
}

// ---------------------------------------------------------------------------
// 结果
// ---------------------------------------------------------------------------

void AArenaGameState::SetWinner(AArenaPlayerState* InWinner)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 SetWinner，已忽略。"));
		return;
	}

	Winner = InWinner;
	OnRep_Winner();
}

// ---------------------------------------------------------------------------
// 回合计划
// ---------------------------------------------------------------------------

void AArenaGameState::SetRoundPlan(TArray<EArenaStageKind>&& InPlan)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 非权威端调用了 SetRoundPlan，已忽略。回合计划只在服务端填。"));
		return;
	}

	RoundPlan = MoveTemp(InPlan);

	// 服务端收不到自己的 OnRep_，显式广播一次，让两端走同一条路。
	OnRep_RoundPlan();
}

void AArenaGameState::SetLastRoundResult(int32 WinnerSlot, int32 HpLoss)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 非权威端调用了 SetLastRoundResult，已忽略。结果只在服务端记。"));
		return;
	}

	LastRoundWinnerSlot = WinnerSlot;
	LastRoundHpLoss = FMath::Max(0, HpLoss);

	// 同 SetRoundPlan：服务端收不到自己的 OnRep_，显式广播。
	OnRep_LastRound();
}

// ---------------------------------------------------------------------------
// OnRep
// ---------------------------------------------------------------------------

void AArenaGameState::OnRep_ArenaPhase()
{
	OnArenaPhaseChanged.Broadcast(ArenaPhase);
}

void AArenaGameState::OnRep_RoundPlan()
{
	OnRoundPlanChanged.Broadcast();
}

void AArenaGameState::OnRep_LastRound()
{
	OnLastRoundChanged.Broadcast();
}

void AArenaGameState::OnRep_Contenders()
{
	// 参赛者变了没有专门的事件：它只在开局那一刻变一次，
	// 而那一刻 GameState 必然还能收到随后的相位变化（WaitingToStart → RewardSelection），
	// HUD 在那时候重画就会读到新的 Contenders。为它单开一个委托是多余的。
}

void AArenaGameState::OnRep_Winner()
{
	OnMatchEnded.Broadcast(Winner);
}
