// 斗魂竞技场：整场比赛的复制状态（相位 / 回合数 / 倒计时 / 双方是谁）。
//
// ===========================================================================
// 【它和 AArenaGameMode 的分工】
// 一句话：GameMode 是"规则"，GameState 是"比分牌"。
//
//   GameMode（只在服务端存在）  → 回合表、抽签池、扣血表、什么时候换相位
//   GameState（两端都有）       → 现在第几回合、什么相位、还剩几秒、谁赢了
//
// 这条线不是形式主义：客户端【没有】GameMode（AGameModeBase 不复制到客户端），
// 所以任何"客户端也要知道"的东西放 GameMode 上等于没放。反过来，
// 客户端不需要知道"第 7 回合该发什么奖励"，那是服务端算完端过来的事。
// ===========================================================================
//
// 【相位倒计时为什么用"结束时刻"而不是"剩余秒数"】
// 复制"剩余秒数"的话，服务端每帧改、每帧复制，而且客户端拿到的是一个
// 一直在变的数字，两次复制之间的插值要靠猜。复制一个绝对时刻
// （GetServerWorldTimeSeconds 的坐标系），客户端自己减一下就是剩余秒 ——
// 一次复制管全程，而且两端算出来的剩余时间天然一致。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "GAS/ArenaTypes.h"
#include "ArenaGameState.generated.h"

class AArenaPlayerState;

/** 相位变了。HUD 订阅它切界面。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FArenaPhaseChangedSignature, EArenaPhase, NewPhase);

/** 大场结束、赢家定了。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FArenaMatchEndedSignature, AArenaPlayerState*, Winner);

/** 回合计划变了（开局时服务端填好，之后不再变）。回合线订阅它。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FArenaRoundPlanChangedSignature);

/** 上一回合的结果变了（结算时服务端填一次）。比分栏的上回合显示订阅它。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FArenaLastRoundChangedSignature);

/**
 * 竞技场 GameState。
 *
 * 【参赛者按下标区分主客】需求里两边是对称的（1V1，没有攻守），
 * 但服务端总要能说"给他发这个奖励、给他发那个"，所以用 Contenders[0] / [1]。
 * 不叫"玩家/对手"，因为谁是玩家取决于看的是哪台机器 ——
 * 下标是客观的，主客不是。
 */
UCLASS()
class LOL_API AArenaGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	AArenaGameState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// =====================================================================
	// 相位 / 回合
	// =====================================================================

	UFUNCTION(BlueprintPure, Category = "Arena|Phase")
	EArenaPhase GetArenaPhase() const { return ArenaPhase; }

	/** 当前回合号，1 起。MatchEnd 之后停在最后一个回合号上。 */
	UFUNCTION(BlueprintPure, Category = "Arena|Phase")
	int32 GetRoundNumber() const { return RoundNumber; }

	/**
	 * 当前相位的倒计时还剩几秒。0 表示这个相位没有倒计时（比如战斗阶段）。
	 *
	 * 【为什么这个函数在 GameState 上而不是 UI 里】它要减的那个基准
	 * （GetServerWorldTimeSeconds）是 GameState 自己的 API。
	 * 让 UI 去拿基准自己减，等于把"两端时钟怎么对齐"这件事交给每个控件各写一遍。
	 */
	UFUNCTION(BlueprintPure, Category = "Arena|Phase")
	float GetPhaseRemainingSeconds() const;

	/**
	 * 换相位。【只在服务端生效】。DurationSeconds <= 0 表示这个相位不倒计时。
	 *
	 * 【顺带把回合号也定了】相位和回合号是两把尺子量同一件事的两面，
	 * 分开设会出现"第 3 回合的战斗阶段"和"第 4 回合的奖励阶段"同时存在的瞬间，
	 * 而客户端正好在那个瞬间收到复制的话，UI 就会画出这个不存在的组合。
	 */
	void SetArenaPhase(EArenaPhase NewPhase, int32 InRoundNumber, float DurationSeconds = 0.f);

	// =====================================================================
	// 参赛者
	// =====================================================================

	/** 下标 0 / 1。越界或还没就位返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	AArenaPlayerState* GetContender(int32 Index) const;

	/** 都到齐了没有。GameMode 用它决定什么时候开局。 */
	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	bool AreContendersReady() const;

	/** 服务端：把一个参赛者放进下标 Index 的位置。 */
	void SetContender(int32 Index, AArenaPlayerState* PlayerState);

	/** 服务端：清空（重开一局时用）。 */
	void ResetContenders();

	/** 对手是谁（相对某个参赛者而言）。找不到返回 nullptr。 */
	AArenaPlayerState* GetOpponentOf(const AArenaPlayerState* PlayerState) const;

	// =====================================================================
	// 结果
	// =====================================================================

	/** 赢家。比赛还没结束时是 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	AArenaPlayerState* GetWinner() const { return Winner; }

	/** 服务端：定下赢家（nullptr = 平局/中止）。 */
	void SetWinner(AArenaPlayerState* InWinner);

	// =====================================================================
	// 回合计划（回合线的数据源）
	// =====================================================================

	/**
	 * 本场比赛的回合计划，下标 0 = 第 1 回合。开局由服务端填好，之后不再变。
	 *
	 * 【为什么在 GameState 上而不是让客户端自己推】回合表住在 AArenaGameMode 里，
	 * 而 GameMode 不复制到客户端 —— 客户端自己永远算不出"第 7 回合之后是什么"。
	 * 服务端开局算一次推下来，客户端只画不猜，两条纪律（规则在服务端、
	 * 复制的都是成品）各自都成立。
	 *
	 * 【长度】RoundRewards + LoopRewards 各一条（见 AArenaGameMode::BuildRoundPlan）。
	 * 打到表尾之后比赛还没结束的话，回合线停在最后一格 —— 比赛是"血量判负"制，
	 * 拖到表尾还不结束本身就该调配置了，界面不为此再发明表示。
	 */
	UFUNCTION(BlueprintPure, Category = "Arena|Phase")
	TArray<EArenaStageKind> GetRoundPlan() const { return RoundPlan; }

	/** 服务端：填回合计划（开局调一次）。空计划 = 回合线不画。 */
	void SetRoundPlan(TArray<EArenaStageKind>&& InPlan);

	/** 回合计划变了（开局填好时广播一次）。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|Phase")
	FArenaRoundPlanChangedSignature OnRoundPlanChanged;

	// =====================================================================
	// 上一回合的结果（购物阶段顶部的 √/× + 扣血显示）
	// =====================================================================

	/**
	 * 上一回合赢家的参赛者下标（0/1）。INDEX_NONE = 还没有结果。
	 *
	 * 【为什么记下标而不是记赢家的名字】UI 侧要做的是"我赢了还是我输了"，
	 * 而"谁是我"取决于看的是哪台机器 —— 翻译层拿到下标，和本地玩家一比
	 * 就能算出自己视角的结果。直接复制"你赢了/你输了"的文本反而做不到：
	 * 文本没法对两边各自成立。
	 */
	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	int32 GetLastRoundWinnerSlot() const { return LastRoundWinnerSlot; }

	/** 上一回合输家被扣的大场血量（显示 "−N" 用）。 */
	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	int32 GetLastRoundHpLoss() const { return LastRoundHpLoss; }

	/** 服务端：结算时记上一回合的结果（每回合覆盖一次）。 */
	void SetLastRoundResult(int32 WinnerSlot, int32 HpLoss);

	/** 上一回合的结果变了。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|Match")
	FArenaLastRoundChangedSignature OnLastRoundChanged;

	// =====================================================================
	// 事件
	// =====================================================================

	UPROPERTY(BlueprintAssignable, Category = "Arena|Phase")
	FArenaPhaseChangedSignature OnArenaPhaseChanged;

	UPROPERTY(BlueprintAssignable, Category = "Arena|Match")
	FArenaMatchEndedSignature OnMatchEnded;

protected:
	/** 当前相位。复制。 */
	UPROPERTY(ReplicatedUsing = OnRep_ArenaPhase, BlueprintReadOnly, Category = "Arena|Phase")
	EArenaPhase ArenaPhase = EArenaPhase::WaitingToStart;

	/** 当前回合号。复制。 */
	UPROPERTY(ReplicatedUsing = OnRep_ArenaPhase, BlueprintReadOnly, Category = "Arena|Phase")
	int32 RoundNumber = 0;

	/**
	 * 当前相位的结束时刻（服务端世界时间，秒）。0 = 没有倒计时。复制。
	 * 见文件头关于"为什么不是剩余秒数"的说明。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_ArenaPhase, BlueprintReadOnly, Category = "Arena|Phase")
	float PhaseEndServerTime = 0.f;

	/**
	 * 两个参赛者。复制。
	 *
	 * 【这里可以放裸指针】和装备那种 DataAsset 不同，PlayerState 是 AActor，
	 * 天生就走 Actor 复制通道（有 NetGUID、有相关性判定）。
	 * 会静默变 null 的是非 Actor 的 UObject，不是这里。
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Contenders, BlueprintReadOnly, Category = "Arena|Match")
	TArray<TObjectPtr<AArenaPlayerState>> Contenders;

	/** 赢家。复制。 */
	UPROPERTY(ReplicatedUsing = OnRep_Winner, BlueprintReadOnly, Category = "Arena|Match")
	TObjectPtr<AArenaPlayerState> Winner;

	/** 本场比赛的回合计划。复制。见 GetRoundPlan 的注释。 */
	UPROPERTY(ReplicatedUsing = OnRep_RoundPlan, BlueprintReadOnly, Category = "Arena|Phase")
	TArray<EArenaStageKind> RoundPlan;

	/** 上一回合赢家的参赛者下标。复制。INDEX_NONE = 还没有结果。 */
	UPROPERTY(ReplicatedUsing = OnRep_LastRound, BlueprintReadOnly, Category = "Arena|Match")
	int32 LastRoundWinnerSlot = INDEX_NONE;

	/** 上一回合扣掉的大场血量。复制。 */
	UPROPERTY(ReplicatedUsing = OnRep_LastRound, BlueprintReadOnly, Category = "Arena|Match")
	int32 LastRoundHpLoss = 0;

	/** 三个相位属性共用一个 OnRep：UI 只关心"相位这块变了，重画"。 */
	UFUNCTION()
	void OnRep_ArenaPhase();

	UFUNCTION()
	void OnRep_Contenders();

	UFUNCTION()
	void OnRep_Winner();

	UFUNCTION()
	void OnRep_RoundPlan();

	UFUNCTION()
	void OnRep_LastRound();
};
