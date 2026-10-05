// 斗魂竞技场：AI 的【观测层】—— 把"玩家怎么打"变成可以读的数。
//
// ===========================================================================
// 【为什么这一层必须单独存在，而且必须最先做】
// "根据玩家行为进化"的输入是行为，不是"感觉"。没有这一层，策略层能拿到的
// 只有"这回合谁赢了"，那点信息量不足以针对任何人 —— 于是所谓"进化"只能是
// 随机调参，玩家感觉不到，也没法调试。
//
// 这一层干的事就一件：每帧看一眼玩家，往一个画像里累加。它不做任何判断，
// 不决定任何事。所以它不会因为策略改了就失效 —— 这是刻意的：
// 观测口径换来换去，历史存档就没法比了。
//
// ===========================================================================
// 【为什么一个 ASC 委托都不挂，全靠轮询】
// 直觉做法是挂 AbilityActivatedCallbacks / OnGameplayEffectApplied 去精确捕捉
// 每一次技能释放。这里【故意不挂】，理由是这套观测要跨局、跨 Pawn、跨重生活着：
//   - 委托的宿主 ASC 挂在 PlayerState 上，Pawn 每回合会被销毁重建，
//     挂/摘的时机稍错一次就是"某个玩家永远观测不到技能"或者悬空回调；
//   - 而 Bot 的 Tick 本来就每帧在跑（战斗阶段），顺手读几个属性/标签的成本
//     和挂委托相比可以忽略，却没有生命周期问题。
//
// 具体地：技能释放【靠冷却标签的上升沿】推断，而不是靠能力激活回调。
// 玩家按了什么键 Bot 看不到（那是输入层的事），但"某个技能进了冷却"是
// 复制到所有端的公开状态（State.Cooldown.*），读它既准又不需要认识任何具体技能。
// 代价是冷却时长特别短的技能可能漏采 —— 对"这个玩家放不放技能"这个粒度无所谓。
//
// ===========================================================================
// 【两套累加器：回合内的和跨局的】
// 回合内的累加器（Round*）每回合清零，记的是原始量（秒数、次数、求和）。
// 回合结束时 FoldRoundIntoProfile 把它折算成一组比率，用 EMA 并进画像。
//
// 【为什么不在每帧直接 EMA 进画像】那样画像会随帧率变化（30 帧和 120 帧
// 采出来的权重不一样），而且"这一回合他打得怎么样"这个中间量会丢掉 ——
// 调试时你会想知道"是这回合他突然改打法了，还是画像本来就这样"。
// 按回合折算是这套东西的最小可解释单位。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/Object.h"
#include "GAS/ArenaAITypes.h"
#include "ArenaBehaviorObserver.generated.h"

class UAbilitySystemComponent;

// EditInlineNew 同 ArenaAIDirector：让 Details 面板能就地新建子类实例。
UCLASS(BlueprintType, EditInlineNew)
class LOL_API UArenaBehaviorObserver : public UObject
{
	GENERATED_BODY()

public:
	UArenaBehaviorObserver();

	// =====================================================================
	// 生命周期 —— 由 AArenaBotController 调
	// =====================================================================

	/** 新的一局开始：对局计数 +1，并在开局时尝试读档。 */
	void BeginMatch();

	/** 新的一回合开始：清空回合内累加器。 */
	void BeginRound();

	/**
	 * 战斗阶段每帧调一次。
	 *
	 * 【为什么参数是两个裸 Actor 而不是"对手的 PlayerState"】
	 * 这一层是纯观测，不该认识竞技场的任何流程类。给它两个 Pawn 就够了 ——
	 * 于是同一套观测以后可以用在别的模式里（训练关、观战回放）。
	 *
	 * SelfPawn  = Bot 自己（用来算相对位移和血量差）
	 * EnemyPawn = 被观测的玩家
	 */
	void ObserveCombatTick(float DeltaSeconds, const AActor* SelfPawn, const AActor* EnemyPawn);

	/**
	 * 本回合结束。bPlayerWon = 被观测的那一方赢了这一回合。
	 * 把回合内累加器折算成比率并 EMA 进画像。
	 */
	void FoldRoundIntoProfile(bool bPlayerWon);

	/** 整局结束：把画像写盘。返回是否真的写出去了。 */
	bool EndMatchAndSave();

	// =====================================================================
	// 给战略层 / 调试 HUD 读
	// =====================================================================

	const FArenaPlayerProfile& GetProfile() const { return Profile; }

	/** 最近一回合的原始统计（调试 HUD 用：看"这回合"和"历史"的差别）。 */
	const FArenaPlayerProfile& GetLastRoundSnapshot() const { return LastRoundSnapshot; }

	/**
	 * 对手此刻是否有被追踪的技能在冷却（= 他刚交完东西）。
	 *
	 * 【它和 Profile::SkillCooldownOccupancy 的区别】那个是跨回合的平均值
	 * （"这个人习惯不习惯留技能"），这个是【此刻的事实】（"他这一秒手里有没有牌"）。
	 * 战术层两个都要：平均值决定站位该不该保守，瞬间值决定这一下该不该抢。
	 *
	 * 【读不到对手 ASC 时给什么】给 false（= "他没在冷却"），不给上一次的值。
	 * 拿不到信息时把"他在冷却"当成真，会让 Bot 在一个未知局面里主动进攻 ——
	 * 反过来只会让它保守一点。保守是这里唯一安全的默认方向。
	 */
	bool IsEnemyOnCooldown() const { return bEnemyOnCooldownNow; }

	/**
	 * 对手此刻在做什么（硬控中 / 打不进去 / 攒着大招）。
	 *
	 * 【和 IsEnemyOnCooldown 同一个口径】都是"此刻的事实"，都由本层每帧读出来 ——
	 * 战术层拿到的永远是这个 tick 的状态，不是它自己去查。
	 *
	 * 【读不到对手 ASC 时】全 false（= 什么都没读到）。这一层的兜底方向和
	 * IsEnemyOnCooldown 一致：拿不到信息就不做额外判断，最差退化成原来的行为。
	 */
	const FArenaEnemyActionState& GetEnemyActionState() const { return EnemyActionNow; }

	/**
	 * 对手已经连续【什么都不做】了多少秒（站着不动，也没交技能）。
	 *
	 * 【为什么需要一个专门的量，而不是用画像里的比率】画像里那些比率（贴脸率、
	 * 远近倾向、推进倾向）回答的都是"这个人平时怎么打"，是几个回合的平均；
	 * 而这一条回答的是"他此刻是不是在挂机"，寿命只有几秒。前面那几条也全都
	 * 答不了这个问题 —— 一个站着不动的玩家，贴脸率和推进倾向都会算出接近 0 的
	 * 中性值，读起来和"他打得很均衡"一模一样，战术层于是没有任何理由主动。
	 *
	 * 【为什么要"也没交技能"这一半】原地读条 / 站桩放技能的人看起来也是不动的，
	 * 但他明明在做事情。只按速度判的话，一个正在放技能的玩家会被读成发呆，
	 * Bot 就会在他技能放到一半时冲上去 —— 那不是主动，那是送。
	 *
	 * 【读数什么时候是 0】开始计时前、他动了、他交了技能、以及每回合开始。
	 */
	float GetEnemyIdleSeconds() const { return EnemyIdleSeconds; }

	/** 清空画像（换了一个新玩家 / 调试用）。不会自动写盘。 */
	void ResetProfile();

	/**
	 * 写入局间战略备忘录。整局结束时由执行层调，然后跟着画像一起存盘。
	 *
	 * 【为什么备忘录由观测层持有】因为它必须和画像一起原子地存/读 ——
	 * 分成两个槽会出现"读到新画像 + 旧备忘录"这种拼起来的组合，
	 * 而那种组合在表现上完全看不出来（只是 Bot 打得有点怪）。
	 * 观测层不生产它，只负责保存它（生产者是 UArenaAIDirector::ComposeMemo）。
	 */
	void SetStrategicMemo(const FArenaStrategicMemo& InMemo);

	/** 存档槽名。留空则不落盘（调试时用）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Observation")
	FString SaveSlotName = TEXT("ArenaAIOpponentProfile");

protected:
	// =====================================================================
	// 观测参数 —— 全在 BP_ArenaBotController 的 Details 面板里改
	//
	// 【这些数改了会让新样本和旧画像不同口径】所以改完最好调一次 ResetProfile
	// （或者接受一段过渡期）。它们不是"调一下试试"的旋钮。
	// =====================================================================

	/**
	 * 被视为"贴脸"的距离上限（厘米）。
	 * 默认 130 = Bot 基线 PreferredRange（见 BP_ArenaBotController），
	 * 也就是"进了我的近战舒适区"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Observation", meta = (ClampMin = "0", Units = "cm"))
	float CloseRangeThreshold = 130.f;

	/**
	 * 被视为"拉开"的距离下限（厘米）。
	 * 默认 350：超出近战臂展（ThreeHitPassiveData 的 TraceDistance 180 + TraceRadius 55）
	 * 一大截，也就是"他站在我够不着的地方"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Observation", meta = (ClampMin = "0", Units = "cm"))
	float FarRangeThreshold = 350.f;

	/**
	 * 玩家速度归一化的分母（cm/s）。用来把"朝向我的速度"压到 [-1,1]。
	 * 默认 350 ≈ 属性集里 MoveSpeed 的典型值（见 HeroCombatCharacter 的 MoveAccelSpeed 注释）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Observation", meta = (ClampMin = "1", Units = "cm/s"))
	float SpeedNormalizer = 350.f;

	/**
	 * 水平速度低于这个值就算"他没在动"（cm/s），用来累计 GetEnemyIdleSeconds。
	 *
	 * 【为什么不是 0】贴着地面站着的人也会有极小的速度抖动（胶囊体和地面
	 * 解算的残差、动画根位移的回读），拿 0 当门槛的话计时器永远进不去。
	 * 60 远低于任何有意为之的走位（MoveSpeed 是几百），又远高于抖动。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Observation", meta = (ClampMin = "0", Units = "cm/s"))
	float IdleSpeedThreshold = 60.f;

	/**
	 * 折进画像时新样本占的权重（0~1）。
	 *
	 * 【为什么不是"总次数 / 总时间"】跨局累积时那样会让三个月前的打法
	 * 和昨天的等权，Bot 永远在针对一个已经不存在的人。0.35 的意思是
	 * "最近三四个回合的样本就能把画像拽过去"，同时历史上千场也不会被一次
	 * 反常行为冲掉。调大 = 学得快但容易被带偏；调小 = 稳但迟钝。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Observation", meta = (ClampMin = "0", ClampMax = "1"))
	float ProfileBlendAlpha = 0.35f;

	/**
	 * 观测哪些冷却标签来推断"玩家放了技能"。
	 *
	 * 【为什么是可配的数组而不是从 ASC 上动态枚举】枚举"所有 State.Cooldown.* "
	 * 需要遍历 ASC 的全部标签，每帧一次；而项目里的冷却标签是有限且已知的一批
	 * （见 LOLGameplayTags.h）。默认由构造函数填满，加新技能时在这里补一条。
	 *
	 * ⚠️ 标签不存在时是【静默不计数】，不是报错 —— 某个技能还没实现冷却时
	 * 只是观测不到它，不会让整个观测层失效。这是有意的：观测层不该因为
	 * 游戏内容没配全而变脆。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Observation")
	TArray<FGameplayTag> TrackedCooldownTags;

	/**
	 * 一回合里有效观测时长不足这么多秒（比如秒杀局），
	 * 这一回合【不折进画像】—— 样本太短，比率全是噪声。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Observation", meta = (ClampMin = "0", Units = "s"))
	float MinRoundSecondsToFold = 2.f;

private:
	// ---------------------------------------------------------------------
	// 回合内累加器（每次 BeginRound 清零）
	// ---------------------------------------------------------------------

	float RoundSeconds = 0.f;
	float RoundDistanceSum = 0.f;
	int32 RoundDistanceSamples = 0;
	float RoundCloseSeconds = 0.f;
	float RoundFarSeconds = 0.f;

	/** 侧移方向为正 / 为负的时长。 */
	float RoundStrafePosSeconds = 0.f;
	float RoundStrafeNegSeconds = 0.f;

	/** 每帧采样到的"玩家朝我移动的速度分量"的均值（已经归一化到 [-1,1]）。 */
	float RoundApproachSum = 0.f;
	int32 RoundApproachSamples = 0;

	/** 技能释放次数 + 有技能在冷却的时长。 */
	int32 RoundSkillCasts = 0;
	float RoundCooldownSeconds = 0.f;

	/**
	 * 上一帧每个被追踪标签的"在冷却中"状态。
	 * 【必须和 TrackedCooldownTags 同序】所以两个一起改 —— 见 RefreshCooldownTagCache。
	 */
	TArray<bool> LastCooldownStateCache;

	/** 上一帧对手是否有技能在冷却（= IsEnemyOnCooldown 的返回值）。 */
	bool bEnemyOnCooldownNow = false;

	/** 上一帧对手的动作状态（= GetEnemyActionState 的返回值）。 */
	FArenaEnemyActionState EnemyActionNow;

	/**
	 * 对手连续静止的秒数（= GetEnemyIdleSeconds 的返回值）。
	 *
	 * 【为什么不是 Round* 前缀】它不是"这一回合的累计量"，而是"当前这一段
	 * 静止已经持续了多久"——会在对手一动时立刻归零。所以它和上面那些只在
	 * BeginRound / FoldRoundIntoProfile 里进出的累计量不是一类东西。
	 */
	float EnemyIdleSeconds = 0.f;

	// ---------------------------------------------------------------------
	// 跨局画像
	// ---------------------------------------------------------------------

	UPROPERTY()
	FArenaPlayerProfile Profile;

	/** 上一回合折算出来的快照（调试用，不存盘）。 */
	UPROPERTY()
	FArenaPlayerProfile LastRoundSnapshot;

	/** 这一局已经开始过了（BeginMatch 幂等用）。 */
	bool bMatchBegun = false;

	/** 取对手的 ASC。取不到返回 nullptr（还没 Possess / 不是 GAS 角色）。 */
	UAbilitySystemComponent* ResolveEnemyASC(const AActor* EnemyPawn) const;

	/** 把 TrackedCooldownTags 和缓存对齐（标签表在运行时可能被 BP 改过）。 */
	void RefreshCooldownTagCache();

	/** 读档填 Profile。失败时保持默认值（等于"第一次见这个玩家"）。 */
	void LoadProfile();

	/** 把 TrackedCooldownTags 补上默认值（只在构造函数里调一次）。 */
	void FillDefaultCooldownTags();
};
