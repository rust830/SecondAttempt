// 斗魂竞技场：回合驱动 + 奖励发放。
//
// ===========================================================================
// 【这一层是整个模式唯一"知道规则"的地方】
// 需求里那张表（第 1 回合二选一分支、第 2 回合海克斯、第 3 回合固定锻造器、
// 第 4 回合海克斯、第 5 回合分支、第 6 回合无奖励、第 7 回合起三回合一轮循环）
// 在这里配；扣血区间、首次大场阈值也在这里判。
//
// 别的类都只回答"我这边是什么情况"：
//   AArenaGameState  → 现在什么相位、第几回合
//   AArenaPlayerState→ 这个人多少血、什么战绩、手上有什么
//   AArenaBotController → 现在该干什么
// 没有一个需要知道"第 7 回合该发什么"。
// ===========================================================================
//
// 【继承 ALOLGameMode 而不是 AGameModeBase】这个项目的 GameMode 基类就是它，
// 换基类会让 World Settings 里配好的 GameMode 覆盖失效。
//
// 放 GAS/ 的理由：它直接施加 GE（回合重置写基础值）、直接读属性集（判满血/死亡），
// 并且持有 UArenaRewardPool（那个资产里有 TSubclassOf<UGameplayEffect>）。
// 判据见 CONVENTIONS.md 规则 2。

#pragma once

#include "CoreMinimal.h"
#include "LOLGameMode.h"
#include "GAS/ArenaItemStats.h"
#include "GAS/ArenaTypes.h"
#include "ArenaGameMode.generated.h"

class AArenaBotController;
class AArenaGameState;
class AArenaPlayerState;
class UArenaRewardPool;

/**
 * 竞技场 GameMode。
 *
 * 【只有服务端有这个对象】AGameModeBase 不复制到客户端。所以：
 *   - 任何客户端也要知道的东西，一律写进 AArenaGameState / AArenaPlayerState；
 *   - 客户端要知道"现在该显示什么"，靠的是复制下来的 FArenaPendingPrompt
 *     （文案已经在服务端拼好了）。
 */
UCLASS()
class LOL_API AArenaGameMode : public ALOLGameMode
{
	GENERATED_BODY()

public:
	AArenaGameMode();

	// ---------------------------------------------------------------------
	// AGameModeBase
	// ---------------------------------------------------------------------

	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	/**
	 * 这个控制器该生在哪个出生点。
	 *
	 * 【为什么必须重写】角色的死亡重生链最后会走到
	 * AGameModeBase::FindPlayerStart（见 HeroCombatCharacter::FindRespawnTransform）——
	 * 也就是说"死一次就换个地方复活"是引擎默认行为。竞技场里必须每次都回到
	 * 【自己那一侧的】出生点，不然打两回合两个人就换边了。
	 */
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;

	// ---------------------------------------------------------------------
	// 选择结算
	// ---------------------------------------------------------------------

	/**
	 * 结算一次选择。
	 *
	 * 【两条入口，一个实现】
	 *   人：AArenaPlayerState::ServerSubmitChoice（Server RPC）→ 这里
	 *   AI：AArenaBotController 直接调（它本来就在服务端）
	 * 所以不存在"人能做的 AI 做不了"。传进来的只有下标 ——
	 * 载荷（到底给了哪件装备）由服务端自己从待选里取，不信客户端说的内容。
	 *
	 * 下标非法 / 当前没有待选 / 载荷为空都会记一条 Warning 并忽略，不崩。
	 *
	 * @param bAllowBeginCombat 「这次结算完了顺手问一句能不能开战」。
	 *   正常一条选择只属于【一个人】：他选完 → TryBeginCombat → 检查双方都选完了才开战
	 *   （默认配置下还会被备战倒计时再拦一道，见 TryBeginCombat）。
	 *   但【超时代选】不一样：倒计时到点那一刻，两个人可能都没选完，而且是要
	 *   在【一个函数里依次替两个人各走几步】。第一个人那一步如果照常开战，
	 *   第二个人还没被代选就被拖进战斗了 —— 他这一回合的奖励白丢，
	 *   而且下一回合的状态是从一个"带着未选状态进战斗"的人开始的。
	 *   所以代选路径传 false：代选只负责把两个人的选择补齐，开战统一放在
	 *   ForceFinishPreparation 末尾做一次。
	 */
	void ResolveChoice(AArenaPlayerState* Chooser, int32 OptionIndex, bool bAllowBeginCombat = true);

	/**
	 * 重随待选里的某一项（对齐 LoL 斗魂竞技场的 reroll）。
	 *
	 * 【两条入口，一个实现】
	 *   人：AArenaPlayerState::ServerRerollChoice（Server RPC）→ 这里
	 *   AI：AArenaBotController 也可以直接调（它本来就在服务端）
	 *
	 * 只换下标指定的那一个选项，其余不动；重抽时排除这份待选里已有的其它项
	 * （避免"重随完出现两张一样的卡"）。次数整场共享，花一次少一次。
	 * 没有待选 / 不可重随 / 下标非法 / 没次数都记 Warning 并忽略。
	 */
	void RequestReroll(AArenaPlayerState* Chooser, int32 OptionIndex);

protected:
	// =====================================================================
	// 配置 —— 全在 BP_ArenaGameMode 的 Details 面板里改
	// =====================================================================

	/**
	 * 回合奖励表。下标 0 = 第 1 回合。
	 *
	 * 【第 7 回合起是循环的】表里配到第 6 条为止，之后按
	 * (回合号 - 7) % 3 走：0 / 1 = 装备回合，2 = 海克斯回合。
	 * 这个循环写死在 GetRoundReward 里，因为它不是"配几条"能表达的
	 * （配 100 条会在第 107 回合断掉）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Round")
	TArray<FArenaRoundReward> RoundRewards;

	/**
	 * 第 7 回合起循环用的那几个回合。默认 [装备, 装备, 海克斯]，对应需求里
	 * "7 装备 / 8 装备 / 9 海克斯 / 10 装备 / 11 装备 / 12 海克斯…"。
	 *
	 * 【循环是取模的】第 N 回合（N > 表长）取 LoopRewards[(N - 表长 - 1) % 数量]，
	 * 所以配 3 条是"三回合一轮"、配 2 条是"两回合一轮"，不需要改代码。
	 * 空数组 = 第 7 回合之后不发奖励（会记 Warning，不静默）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Round")
	TArray<FArenaRoundReward> LoopRewards;

	/**
	 * 输一回合扣多少大场血量，按回合号分段。需求：1-4 扣 15、5-8 扣 30、
	 * 9-12 扣 40、13+ 扣 50。取"最后一条 FromRound <= 当前回合"的那条。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Match")
	TArray<FArenaHpLossBand> HpLossBands;

	/**
	 * 首次大场阈值的回合数。需求 major_match：first_threshold 是
	 * "一方 6 回合全胜或全败，即 6:0 / 0:6"，同时 min_rounds = 6。
	 *
	 * 【为什么两个需求合成了一个数】"至少要打 6 回合"和"6 回合全胜就结束"
	 * 在需求里是同一个 6：没打满不判，打满了就只看是不是全胜或全败。
	 * 拆成两个字段的话可以配出"min_rounds=6 而阈值=4"这种永远判不出来的组合，
	 * 而那种配置的表现是"打满 6 回合了却什么都没发生"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Match", meta = (ClampMin = "1"))
	int32 FirstThresholdWins = 6;

	/** 装备 / 海克斯的抽签池。为空时发不出奖励（会记 Warning，不会静默什么都不发生）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward")
	TObjectPtr<UArenaRewardPool> RewardPool;

	/** 一次三选一给几个候选。需求是 3。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward", meta = (ClampMin = "1"))
	int32 OfferCount = 3;

	/**
	 * 开局每个人有多少次重随（对齐 LoL 斗魂：整场共享 4 次）。
	 * 重随可以换掉三选一里的单个选项；锻造器菜单、二选一分支、属性锻造器不可重随。
	 * 0 = 关掉重随。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward", meta = (ClampMin = "0"))
	int32 RerollCharges = 4;

	// --- 属性锻造器（stat anvil，需求里的「给属性的那个锻造器」）---
	//
	// 【语义】锻造器次数花掉后不给装备，直接发随机属性 —— 对应 LoL Arena 的
	// Stat Anvil（Crowd Favorite / Bravery 给的那种）。数值不占装备栏、不可摘除、可无限叠。

	/** 传说锻造器每次使用时从里面抽的属性池。空池 = 用了只扣次数（会记 Warning）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Forge")
	TArray<FArenaItemStatModifier> StatAnvilPool_Legendary;

	/** 棱彩锻造器每次使用时从里面抽的属性池。空池 = 用了只扣次数（会记 Warning）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Forge")
	TArray<FArenaItemStatModifier> StatAnvilPool_Prismatic;

	/** 每次使用随机发几条属性。池子不够时发几条算几条（不重复抽同一条）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Forge", meta = (ClampMin = "1"))
	int32 StatsPerForgeUse = 2;

	// --- 等级（需求：初始 3 级，每回合开始 +3 级）---

	/** 第 1 回合的等级。需求 initial level 3。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Level", meta = (ClampMin = "1"))
	int32 InitialHeroLevel = 3;

	/** 每回合开始加几级。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Level", meta = (ClampMin = "0"))
	int32 LevelsPerRound = 3;

	/** 等级上限。LoL 是 18 —— 第 6 回合就会顶到。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Level", meta = (ClampMin = "1"))
	int32 MaxHeroLevel = 18;

	// --- 相位时长 ---

	/**
	 * 结算阶段停多久（秒）再开下一回合。
	 *
	 * ⚠️ 【必须大于角色 BP 上的 RespawnDelay】结算期间输的那一方还在死亡状态
	 * （复活是 GE_Death 到期触发的，时长 = RespawnDelay，AHeroCombatCharacter 上默认 5 秒）。
	 * 停得比它短的话，下一回合会在尸体还在布娃娃的时候开始 —— 等级和位置都设不上。
	 * 每回合开始时会检查一次，还在死亡状态就记一条 Warning 提醒调这个数。
	 *
	 * 默认 6 是照着上面那个 5 秒留的余量。换了 RespawnDelay 更大的角色 BP 就要一起调大。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Phase", meta = (ClampMin = "0.1", Units = "s"))
	float SettlementSeconds = 6.f;

	/**
	 * 备战阶段限时（秒）。倒计时走完还没选完的，按"放弃本回合奖励"处理并立刻开战。
	 *
	 * 【语义升级（2026-10-01）】原 RewardPhaseSeconds（0 = 不限时）被它取代：
	 * 备战阶段现在是"在备战地图上自由移动 + 做选择"的完整阶段，
	 * 不限时的话一方可以无限拖 —— 对面已经在备战区站了半分钟，这局就没法打了。
	 * 所以默认 30 秒，且到点强制推进（bForceStartWhenPrepExpires 关掉的话
	 * 退回"不限时"的旧行为）。
	 *
	 * 【到点没选的人怎么办】系统替他【随机代选】，沿奖励链一路选到
	 * ResetRewardSelection（进入战斗）为止；锻造器菜单里随机到「进入战斗」
	 * 也算合法出口。最多代选 16 步（防死循环）。
	 *
	 * ⚠️ 这里【不】是"放弃本回合奖励 / 不补发"—— 超时白丢一整回合奖励太亏，
	 * 而且代选比"双方都等齐"更容易让链路收敛（见 ForceFinishPreparation 实现）。
	 *
	 * 【和「放弃」的真正区别】只有两种情况真的按放弃处理：
	 *   ① 参赛者指针为空（槽位没人）；
	 *   ② 代选 16 步之后仍然没有任何推进（兜底）。
	 * 这两种都在实现里打了 Warning。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Phase", meta = (ClampMin = "0", Units = "s"))
	float PrepPhaseSeconds = 30.f;

	/** true = 备战倒计时走完就强制开战（没选完的由系统随机代选）。false = 等所有人选完。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Phase")
	bool bForceStartWhenPrepExpires = true;

	// =====================================================================
	// 训练木桩（备战区）
	// =====================================================================

	/**
	 * 备战阶段在备战区摆训练木桩（不还手、打不死）。
	 *
	 * 【为什么它们只在备战阶段存在】木桩是【试手】用的，不是场景装饰。战斗阶段
	 * 留着它们的话，普攻的近战扫描会把它们一起扫进来 —— 玩家在战斗场上打到一半
	 * 被一个靶子吃掉一次普攻，那是很难查的一类"偶尔少一下伤害"。
	 * 所以 BeginCombat 里会把它们清掉，下一回合开始再重新摆。
	 *
	 * 【为什么是生成而不是摆在关卡里】摆在关卡里等于每张备战地图都要手动维护一遍，
	 * 而且那张图本身就是战斗图（同一个 Arena_1v1）—— 摆进去的木桩会在战斗阶段
	 * 一直站在场上。生成法让"只在备战阶段存在"这件事由代码保证，不靠人记得删。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|TrainingDummy")
	bool bSpawnTrainingDummies = true;

	/** 摆什么。默认 AArenaTrainingDummy；想换模型/材质就做一个它的蓝图子类填这里。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|TrainingDummy",
		meta = (EditCondition = "bSpawnTrainingDummies"))
	TSubclassOf<AActor> TrainingDummyClass;

	/**
	 * 木桩相对【各自备战出生点】的偏移（厘米，跟随出生点朝向）。
	 *
	 * 默认是"出生点正前方 1.5 米"。这个数是被场地尺寸逼出来的，不是拍的：
	 * 备战点摆在四角那块四分之一圆柱平台上，那块台子是【半径 300 的扇形】——
	 * 出生点在台面上，木桩再往正前方 4 米就出弧线了。而木桩是 MOVE_None，
	 * 掉不下去，出界会【悬在空中】。1.5 米既留在台面上，又比两个胶囊半径之和
	 *（约 70）宽出一倍多，不会和玩家挤在一起。
	 *
	 * 摆的位置不对（比如陷进墙里 / 出界）时改这个数，不用动关卡。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|TrainingDummy",
		meta = (EditCondition = "bSpawnTrainingDummies", Units = "cm"))
	FVector TrainingDummySpawnOffset = FVector(150.f, 0.f, 0.f);

	/**
	 * 回合线最多显示几个回合。服务端按回合表 + 循环表外推到这个数为止，
	 * 之后的回合不进计划（回合线停在最后一格）。
	 *
	 * 【为什么有上限】循环表理论上无限长，而屏幕上的回合线是有限的
	 * （LoL 原作也只显示固定数量的一排）。默认 9 = 前 6 回合 + 第一轮循环，
	 * 和默认回合表刚好对齐。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Round", meta = (ClampMin = "1", ClampMax = "16"))
	int32 RoundPlanLength = 9;

	/**
	 * 战斗阶段多久查一次"有人倒下了吗"（秒）。
	 *
	 * 【为什么是轮询而不是事件】这个项目的死亡判定是 ASC 上的 State.Dead 标签，
	 * 没有任何对外的死亡委托。为它加一个委托要改 AHeroCombatCharacter（能跑的东西），
	 * 而这里只是每 0.1 秒问两个人一句 IsDead() —— 代价接近零，
	 * 而且顺带把"Pawn 没了"这种情况一起覆盖了。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Phase", meta = (ClampMin = "0.02", Units = "s"))
	float CombatPollInterval = 0.1f;

	/**
	 * 战斗阶段里某一方的 Pawn 连续消失多久就判它负（秒）。
	 *
	 * 【为什么要给宽限】回合开始 / 换 Pawn 的一瞬间 GetPawn() 可能是空，
	 * 一没了就判会判错人。宽限只要够跨过那种瞬时状态就行，不需要长 ——
	 * 掉出世界的 Pawn 是被引擎在 KillZ 平面销毁的，不会自己回来。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Phase", meta = (ClampMin = "0", Units = "s"))
	float MissingPawnGraceSeconds = 0.5f;

	// --- AI 对手 ---

	/** Bot 的控制器类。留空 = 不生成 Bot（那就只能等第二个真人）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot")
	TSubclassOf<AArenaBotController> BotControllerClass;

	/**
	 * 第一个人进来之后等多久没等到第二个人，就放一个 Bot。
	 *
	 * 【为什么要有这个】需求是"AI Bot + PvP 另一个玩家"两种都要。
	 * 判据只能是"位置空着多久"—— 真人在的时候不生成，真人一直不来就生成。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot", meta = (ClampMin = "0", Units = "s"))
	float BotJoinDelay = 1.5f;

	/** 关掉它 = 只等人，永远不生 Bot（调试"真人 vs 真人"时用）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot")
	bool bSpawnBotIfAlone = true;

	// =====================================================================
	// 回合流程
	// =====================================================================

	/** 开局。两边都就位后由 PostLogin 调，幂等。 */
	void StartMatch();

	/** 开始第 RoundNumber 回合：复位两个人 → 传送备战区 → 进备战相位 → 发奖励。 */
	void StartRound(int32 RoundNumber);

	/** 给一个人发本回合的奖励（或直接进锻造器菜单）。 */
	void BeginRewardSelection(AArenaPlayerState* PS, int32 RoundNumber);

	/**
	 * 在每个备战出生点前面摆一个训练木桩。StartRound 里调。
	 *
	 * 先清后摆，所以重复调用是安全的（换回合、重开都不用手动管）。
	 * 关卡里没摆备战点时它什么都不做 —— 没有木桩不影响比赛流程。
	 */
	void SpawnTrainingDummies();

	/** 清掉所有训练木桩。BeginCombat 里调 —— 理由见 bSpawnTrainingDummies 的注释。 */
	void ClearTrainingDummies();

	/** 当前活着的木桩。只用来在换回合 / 开战时清掉它们，不参与任何游戏逻辑。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> TrainingDummies;

	/**
	 * 本回合会不会有备战倒计时（= StartRound 里会不会排那个 PrepForceTimerHandle）。
	 *
	 * 【为什么单独提一个谓词】它有两个消费者，而且必须是同一个判断：
	 *   ① StartRound —— 决定要不要排倒计时定时器；
	 *   ② TryBeginCombat —— 决定"等倒计时"这道闸门要不要生效。
	 * 两边各写一遍的话，改了其中一个（比如把 PrepPhaseSeconds 默认值改成 0）
	 * 会让另一个失效：拦住所有开战入口，或者反过来让倒计时形同虚设。
	 * 前者的表现是【整局卡在备战阶段】，而且不会报任何错。
	 */
	bool HasPrepCountdown() const;

	/**
	 * 两个人都选完了就开打。任何一个还没选完就直接返回。
	 *
	 * @param bPrepCountdownExpired 这次调用是不是【备战倒计时到点】触发的那一次。
	 *
	 * 【为什么需要这个参数】"双方都选完"和"可以开战了"是两件事，而备战阶段存在的
	 * 意义正是让你在倒计时走完之前【留在备战区自由活动】。所以选完只是让你变成
	 * "完成"状态，开战仍要等倒计时 —— 于是：
	 *   bForceStartWhenPrepExpires = true （默认）：只有倒计时那一路（传 true）能开战；
	 *   = false：没有倒计时可等，退回"所有人选完就开"的老行为。
	 * 不传 = false，也就是"你这次不是倒计时触发的，按上面的规则决定能不能开"。
	 *
	 * 【修的是什么】「进入战斗」按钮按需求移除之后，这条"选完即开"的路径没有跟着删 ——
	 * 结果双方点完最后一份奖励，备战阶段当场结束，人还没走到木桩跟前就被传进战斗场了。
	 */
	void TryBeginCombat(bool bPrepCountdownExpired = false);

	void BeginCombat();

	/**
	 * 备战倒计时走完了：没选完的由系统沿奖励链随机代选（最多 16 步），
	 * 代选完统一在这里开打一次。
	 * bForceStartWhenPrepExpires 为 false 时这条路不会被排上。
	 */
	void ForceFinishPreparation();

	/** 战斗阶段的轮询：有人倒下了就进结算。 */
	void PollCombatOutcome();

	/** 这一回合输了：扣血、记战绩、判大场是否结束，没结束就排下一回合。 */
	void BeginSettlement(AArenaPlayerState* Loser);

	/** 大场结束。Winner 可以是 nullptr（有人掉线 / 双方都没了）。 */
	void EndMatch(AArenaPlayerState* Winner);

	// =====================================================================
	// 出生点 / 传送
	// =====================================================================

	/**
	 * 把一个 Pawn 传到某个 Actor 的位置（无扫掠 + 物理传送）。
	 * Spawn 为空或 Pawn 为空时什么都不做 —— 出生点缺失各自已经记过日志。
	 */
	static void TeleportPawnToActor(APawn* Pawn, const AActor* Spawn);

	// =====================================================================
	// 回合计划
	// =====================================================================

	/**
	 * 按回合表 + 循环表外推整场比赛的回合计划（RoundPlanLength 条封顶）。
	 * 结果推给 GameState 复制下去，回合线只画不猜。
	 */
	void PushRoundPlanToGameState();

	/** 第 RoundNumber 回合在回合线上画成什么（PhaseKind → StageKind 的映射）。 */
	EArenaStageKind GetStageKindForRound(int32 RoundNumber) const;

	// =====================================================================
	// 奖励：往 PlayerState 上放一份待选
	// =====================================================================

	/** 二选一：装备 还是 锻造器。 */
	void PresentBranchChoice(AArenaPlayerState* PS, const FArenaRoundReward& Reward);

	/** 随机三选一海克斯（按 FArenaRoundReward::AugmentTier 指定的档位从池子里抽）。 */
	void PresentAugmentOffer(AArenaPlayerState* PS, EArenaAugmentTier Tier);

	/**
	 * 随机三选一装备。
	 *
	 * @param bFromRoundReward true = 本回合发的装备奖励；false = 花锻造器次数抽的。
	 *        这个标记会跟着待选一起复制下去，UI 只是拿来换个副标题。
	 */
	void PresentItemOffer(AArenaPlayerState* PS, EArenaItemTier Tier, bool bFromRoundReward);

	/**
	 * 锻造器菜单 + 「进入战斗」。
	 *
	 * 【为什么每个人都一定会收到它】这是奖励选择的"出口"：
	 * 攒着的锻造器次数只能在奖励阶段花，而"花完了吗"只有玩家自己知道。
	 * 所以无论这一回合发没发东西，都发一份菜单，最后一项永远是「进入战斗」——
	 * 少了它，第 6 回合（无奖励）的人就永远卡在奖励阶段了。
	 */
	void EnterForgeMenu(AArenaPlayerState* PS);

	/** Grant 类选项的结算：按载荷分派（装备 / 海克斯 / 分支）。 */
	void HandleGrant(AArenaPlayerState* PS, const FArenaChoiceOption& Option);

	/** UseForge：扣一次次数，然后展开三选一。 */
	void HandleUseForge(AArenaPlayerState* PS, EArenaItemTier Tier);

	// =====================================================================
	// 杂项
	// =====================================================================

	/** 回合复位：等级 / 位置 / 满血 / 奖励流程清零。 */
	void ResetContenderForRound(AArenaPlayerState* PS, int32 Slot, int32 RoundNumber);

	/** 回满血和能量。直接写基础值，不走 GE —— 理由见实现里的注释。 */
	void FullRestore(AArenaPlayerState* PS) const;

	/** 第 RoundNumber 回合发什么（含第 7 回合起的循环）。 */
	FArenaRoundReward GetRoundReward(int32 RoundNumber) const;

	/** 第 RoundNumber 回合的等级。 */
	int32 GetLevelForRound(int32 RoundNumber) const;

	/** 第 RoundNumber 回合输掉要扣多少大场血量。 */
	int32 GetHpLossForRound(int32 RoundNumber) const;

	/**
	 * 这个人的战绩够不够判大场结束（首次大场阈值）。
	 *
	 * 需求："一方 6 回合全胜或全败，即 6:0 / 0:6"，且 min_rounds = 6。
	 */
	bool HasReachedFirstThreshold(const AArenaPlayerState* PS) const;

	/** 还不满的那个参赛者下标。满了返回 INDEX_NONE。 */
	int32 FindFreeContenderSlot() const;

	/** 这个人在几号位。不在参赛者表里返回 INDEX_NONE。 */
	int32 FindContenderSlotOf(const AArenaPlayerState* PS) const;

	/** 第 Slot 号的【战斗】出生点（备战区之外的 PlayerStart，按名字排序取第 N 个）。 */
	AActor* GetContenderSpawnPoint(int32 Slot) const;

	/**
	 * 第 Slot 号的【备战区】出生点。备战点的判据见 .cpp 里的 IsPrepSpawnActor：
	 * PlayerStart 上带 PrepStart 标签，或者名字 / 编辑器 Label 以 PrepStart 开头。
	 *
	 * 【怎么摆】往备战区拖一个空 PlayerStart，在 Details 里加一个 PrepStart 标签即可。
	 * 【为什么优先用标签】World Partition 地图会把摆进去的 actor 自动改名成
	 * PlayerStart_UAID_<十六进制>，而在 Outliner 里改的是 Label 不是对象名 ——
	 * 所以"改个名字"这个操作在那类地图上不生效（详见 IsPrepSpawnActor 的注释）。
	 * 标签是唯一在打包后仍然有效的机制：Label 是编辑器专用的，打包时被剥掉。
	 *
	 * 【为什么不是一个新 Actor 类】备战点在玩法上就是一个出生点，换个类等于多一个
	 * 要维护的资产类型；标签在任意关卡、任意 actor 上都能直接加。
	 *
	 * 一个都找不到时回退战斗出生点：备战区没布置的关卡，行为退回改动前
	 * （人站在战斗出生点做选择），模式照样能跑。
	 */
	AActor* GetPrepSpawnPoint(int32 Slot) const;

	/** Bot 延迟到了：还没等到人就把位置填上。 */
	void TrySpawnBot();

	AArenaGameState* GetArenaGameState() const;

private:
	/** Bot 生成的延迟句柄。真人来了要把它清掉。 */
	FTimerHandle BotSpawnTimerHandle;

	/** 战斗阶段的轮询句柄。 */
	FTimerHandle CombatPollHandle;

	/** 结算 → 下一回合的句柄。 */
	FTimerHandle SettlementTimerHandle;

	/** 备战倒计时 → 强制开战的句柄。开战 / 大场结束时要清掉。 */
	FTimerHandle PrepForceTimerHandle;

	/**
	 * 这一场战斗里有没有已经报过"某个参赛者还没有 Pawn"。
	 * 轮询每 0.1 秒跑一次，不加这个标志的话那种异常会刷满日志。
	 * 只覆盖"本回合还没出现过"这一种（回合刚开始的瞬时状态）；
	 * "出现过又没了"走判负那条路，那边每次都值得记一条日志。
	 * 每次进战斗阶段清零。
	 */
	bool bWarnedMissingPawnThisCombat = false;

	/** 本回合这个位置出现过 Pawn。用来区分"还没生出来"和"打没了"，见 PollCombatOutcome。 */
	bool bPawnSeenThisCombat[ArenaMatch::ContenderCount] = {};

	/** 这个位置的 Pawn 从哪一刻起不见了（世界时间）。0 = 现在看得见。 */
	double PawnMissingSinceSeconds[ArenaMatch::ContenderCount] = {};
};
