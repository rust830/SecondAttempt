// 斗魂竞技场：一个参赛者的大场状态（血量 + 战绩 + 装备栏 + 待选择）。
//
// ===========================================================================
// 【为什么继承 AMyPlayerState 而不是另起一个】
// 参赛者同时也是一个普通英雄：要有 ASC、要有属性集、要有等级。那些全在
// AMyPlayerState 上（等级系统和这套是两条并行的线，见 MyPlayerState.h）。
// 另起一个 PlayerState 就得把 ASC / 属性集 / 等级再抄一遍，而且召唤师技能那套
// 也跟着分裂。竞技场要的只是【额外】多几样东西，不是换一套。
//
// 【这一层只管"我这个人的状态"，不管"比赛怎么走"】
// 回合表、抽签池、相位机都在 AArenaGameMode / AArenaGameState 上。
// 这里的每个字段都能回答"这个人现在是什么情况"，
// 没有一个字段需要知道"现在是第几回合"。
// ===========================================================================
//
// 放 GAS/ 的理由：它持有 UArenaLoadoutComponent（直接施加 GE），
// 而且它自己就是 ASC 的宿主（判据见 CONVENTIONS.md 规则 2）。

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyPlayerState.h"
#include "GAS/ArenaTypes.h"
#include "ArenaPlayerState.generated.h"

class UArenaLoadoutComponent;

/** 待选择变了（来了新的 / 被答完清掉了）。奖励界面订阅它重画。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FArenaPromptChangedSignature);

/** 大场血量或回合战绩变了。HUD 的血条 / 比分订阅它。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FArenaMatchStateChangedSignature);

/**
 * 参赛者的 PlayerState。
 *
 * 【AI 对手也有一份这个】Bot 也是走 APlayerController/APlayerState 那套的
 * （见 AArenaBotController），不是"没有 PlayerState 的特殊存在"——
 * 否则 GameMode 里到处都要写"如果是人…否则…"。
 * 每个参赛者一视同仁，只是驱动他做选择的那个东西不同（人点按钮 / AI 自己决定）。
 */
UCLASS()
class LOL_API AArenaPlayerState : public AMyPlayerState
{
	GENERATED_BODY()

public:
	AArenaPlayerState();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// =====================================================================
	// 大场血量
	//
	// 和角色血量（属性集里的 Health）完全是两回事：
	// 角色血量每回合重置，大场血量整场比赛只减不增（需求里的 15/30/40/50）。
	// =====================================================================

	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	int32 GetMatchHealth() const { return MatchHealth; }

	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	int32 GetMaxMatchHealth() const { return MaxMatchHealth; }

	/** 直接设成某个值（会夹在 [0, Max] 之间）。【只在服务端生效】。 */
	void SetMatchHealth(int32 NewHealth);

	/** 扣血。Amount <= 0 时是 no-op。 */
	void ApplyMatchDamage(int32 Amount);

	// =====================================================================
	// 回合战绩
	// =====================================================================

	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	int32 GetRoundsWon() const { return RoundsWon; }

	UFUNCTION(BlueprintPure, Category = "Arena|Match")
	int32 GetRoundsLost() const { return RoundsLost; }

	/** 记一次回合结果。【只在服务端生效】。 */
	void RecordRoundResult(bool bWon);

	// =====================================================================
	// 装备栏 / 海克斯 / 锻造器次数
	// =====================================================================

	UFUNCTION(BlueprintPure, Category = "Arena|Loadout")
	UArenaLoadoutComponent* GetLoadout() const { return Loadout; }

	// =====================================================================
	// 待选择（三选一）
	// =====================================================================

	UFUNCTION(BlueprintPure, Category = "Arena|Choice")
	const FArenaPendingPrompt& GetPendingPrompt() const { return PendingPrompt; }

	UFUNCTION(BlueprintPure, Category = "Arena|Choice")
	bool HasPendingPrompt() const { return PendingPrompt.bActive; }

	/**
	 * 换一个待选。【只在服务端生效】。
	 *
	 * 【不碰 RewardStep】"发什么给玩家看"和"他现在走到流程哪一步"是两件事：
	 * 同一个 RoundReward 步骤会被发两次（三选一装备 Count > 1 时），
	 * 合成一个函数就没法表达这件事了。
	 */
	void SetPendingPrompt(const FArenaPendingPrompt& NewPrompt);

	/**
	 * 这个人的奖励选择阶段结束了：清掉待选、流程状态归零。
	 * 【只在服务端生效】。每回合开始也会调它，保证上一回合的残留在新回合不会复活。
	 */
	void ResetRewardSelection();

	/**
	 * 客户端点了某个选项。服务端会过一遍合法性再结算。
	 *
	 * 【AI 不走这条路】Bot 就在服务端，直接调 AArenaGameMode::ResolveChoice ——
	 * 绕一圈 RPC 只会让它慢一帧，而且"服务端发给自己"的 RPC 本来也发不出去。
	 * 两条入口最后进的是同一个函数，所以不存在"人能做的 AI 不能做"。
	 */
	UFUNCTION(Server, Reliable, Category = "Arena|Choice")
	void ServerSubmitChoice(int32 OptionIndex);

	// =====================================================================
	// 奖励流程状态（服务端专用，不复制）
	//
	// 【为什么不复制】客户端不需要知道"走到哪一步"：每一步都会投影成一份
	// FArenaPendingPrompt（那个是复制的），UI 只看待选。复制一整套流程状态
	// 等于把同一件事说两遍，而且两遍不一致时 UI 会画出不存在的一步。
	// =====================================================================

	EArenaRewardStep GetRewardStep() const { return RewardStep; }
	void SetRewardStep(EArenaRewardStep NewStep) { RewardStep = NewStep; }

	/** 这个人选完了没有。GameMode 用它决定战斗阶段能不能开始。 */
	bool IsRewardSelectionDone() const { return RewardStep == EArenaRewardStep::None; }

	/**
	 * 「本回合奖励」里还要做几次装备三选一。
	 *
	 * 需求里装备分支都是 1 次，这个字段是为"给 2 件"这种配置留的 ——
	 * 有它就不用把 Count 直接丢掉（丢掉了那个配置项就成了骗人的）。
	 */
	int32 GetRemainingItemPicks() const { return RemainingItemPicks; }
	void SetRemainingItemPicks(int32 NewCount) { RemainingItemPicks = FMath::Max(0, NewCount); }
	void DecrementRemainingItemPicks() { RemainingItemPicks = FMath::Max(0, RemainingItemPicks - 1); }

	/**
	 * 这几次三选一从哪个品质的池子里抽。
	 *
	 * 【为什么服务端要单独记一份】待选本身（FArenaPendingPrompt）里没有品质 ——
	 * 它只有"三件具体装备"，发完了就没了。而"再抽一次"需要知道原来抽的是哪一档，
	 * 那是选项发出时才有、之后必须留住的信息。
	 */
	EArenaItemTier GetPendingItemTier() const { return PendingItemTier; }
	void SetPendingItemTier(EArenaItemTier NewTier) { PendingItemTier = NewTier; }

	/** 海克斯待选从哪一档抽的（重随时要按原档重抽）。理由同 PendingItemTier。 */
	EArenaAugmentTier GetPendingAugmentTier() const { return PendingAugmentTier; }
	void SetPendingAugmentTier(EArenaAugmentTier NewTier) { PendingAugmentTier = NewTier; }

	// =====================================================================
	// 重随（reroll，对齐 LoL 斗魂竞技场）
	// =====================================================================

	/**
	 * 开局把重随次数初始化成 N。整场共享（跨所有选择界面、跨回合），
	 * 【只在服务端生效】。
	 */
	void InitRerolls(int32 Charges);

	UFUNCTION(BlueprintPure, Category = "Arena|Choice")
	int32 GetRerollsLeft() const { return RerollsLeft; }

	/** 花一次重随。没次数返回 false。【只在服务端生效】。 */
	bool TryConsumeReroll();

	/**
	 * 客户端请求重随某一项（下标 = 待选里的下标）。
	 * 合法性（有没有待选、能不能重随、还剩几次）全部由服务端判 ——
	 * 和 ServerSubmitChoice 同一条纪律。
	 */
	UFUNCTION(Server, Reliable, Category = "Arena|Choice")
	void ServerRerollChoice(int32 OptionIndex);

	// =====================================================================
	// 给 UI 的事件
	// =====================================================================

	/** 待选变了。奖励界面订阅它重画。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|Choice")
	FArenaPromptChangedSignature OnPromptChanged;

	/** 大场血量或战绩变了。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|Match")
	FArenaMatchStateChangedSignature OnMatchStateChanged;

	// =====================================================================
	// 配置（在 BP_ArenaPlayerState 上改）
	// =====================================================================

	/**
	 * 大场血量上限，也是开局血量。需求里初始 100。
	 *
	 * 【改这个数要想一下血条】HUD 上那根大场血条按 MaxMatchHealth 取百分比，
	 * 所以改它不需要动 UI。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Match", meta = (ClampMin = "1"))
	int32 MaxMatchHealth = 100;

protected:
	virtual void BeginPlay() override;

	/** 当前大场血量。复制 —— 两端都要显示。 */
	UPROPERTY(ReplicatedUsing = OnRep_MatchState, BlueprintReadOnly, Category = "Arena|Match")
	int32 MatchHealth = 100;

	/** 赢过的回合数。复制。 */
	UPROPERTY(ReplicatedUsing = OnRep_MatchState, BlueprintReadOnly, Category = "Arena|Match")
	int32 RoundsWon = 0;

	/** 输掉的回合数。复制。 */
	UPROPERTY(ReplicatedUsing = OnRep_MatchState, BlueprintReadOnly, Category = "Arena|Match")
	int32 RoundsLost = 0;

	/**
	 * 当前等着这个人做的那次选择。复制。
	 *
	 * =====================================================================
	 * 【客户端怎么用它】客户端【只读】Label / Description / Icon / Action / Branch ——
	 * 全是服务端算好后端过来的成品。里面那两个软引用（Item / Augment）是给
	 * 服务端结算用的，客户端不需要解析它们（也解析不出什么 UI 还需要的东西）。
	 *
	 * 这不是"顺手为之"，是这套 UI 的规矩（见 HUDTypes.h）：文案在服务端拼，
	 * UI 拿到手就是能直接画的字。三选一界面因此永远不需要知道"传说装备该显示成什么颜色"。
	 * =====================================================================
	 */
	UPROPERTY(ReplicatedUsing = OnRep_Prompt, BlueprintReadOnly, Category = "Arena|Choice")
	FArenaPendingPrompt PendingPrompt;

	/** 装备栏 / 海克斯 / 锻造器次数。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Arena|Loadout", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UArenaLoadoutComponent> Loadout;

	UFUNCTION()
	void OnRep_Prompt();

	/**
	 * 四个属性共用一个 OnRep。
	 * 客户端只关心"变了，重画那一块"，不关心变的是哪个 ——
	 * 拆成四个换不来任何东西，只会多三个函数。
	 */
	UFUNCTION()
	void OnRep_MatchState();

private:
	/** 服务端专用，见上面那段注释。 */
	EArenaRewardStep RewardStep = EArenaRewardStep::None;

	/** 服务端专用：整场还剩几次重随。开局由 InitRerolls 填。 */
	int32 RerollsLeft = 0;

	/** 服务端专用：当前海克斯待选的品质（重随按原档重抽）。 */
	EArenaAugmentTier PendingAugmentTier = EArenaAugmentTier::Silver;

	/** 服务端专用：本回合奖励里还剩几次装备三选一。 */
	int32 RemainingItemPicks = 0;

	/** 服务端专用：上面那几次三选一抽的是哪一档（见 GetPendingItemTier）。 */
	EArenaItemTier PendingItemTier = EArenaItemTier::Legendary;
};
