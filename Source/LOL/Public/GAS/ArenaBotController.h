// 斗魂竞技场：AI 对手。
//
// ===========================================================================
// 【它和真人的区别只有一个：谁来点按钮】
// Bot 的控制器是 AAIController —— 它【不是】APlayerController 的子类
// （AIController.h:87：AAIController 和 APlayerController 都直接继承 AController）。
// 由此带来一个容易踩的后果：Bot 的 IsPlayerController() / IsLocalPlayerController() 恒为 false，
// 而 IsLocallyControlled() 在【单机】下对 Bot 却返回 true —— 所以「只该给本地玩家看」的
// 表现（屏幕后处理、相机修改器、自己的轮廓）不能拿 IsLocallyControlled 判断，
// 详见 GAS/LocalPlayerUtils.h。
// 但 Bot 在「算不算参赛者」这件事上和真人一样：
//   - 有 PlayerState（bWantsPlayerState = true），而且是 AArenaPlayerState；
//   - 有 Pawn，Pawn 走的是和真人完全一样的 DefaultPawnClass；
//   - 输入走的是和真人完全一样的那些函数（BasicAttackPressed / AbilityInputTagPressed）。
//
// 所以 GameMode 里没有任何"如果是 AI 就…否则…"的分支：奖金、等级、装备、
// 胜负判定全部一视同仁。这个类的全部职责就是【替这个人做决定】。
//
// 【为什么不用行为树 / 导航网格】
// 需求里 AI 要做的事是"走到对面、打、放技能、偶尔后退"—— 五条，而且是在一个
// 空场地上 1V1。上一棵 BT + 一套 BehaviorTree 资产，收益是"以后好扩展"，
// 代价是这套模式多出一个必须单独配、单独调的资产，而且资产配错时的表现是
// 【AI 站着不动】—— 没有任何报错。直接用代码驱动（AddMovementInput）没有这个失败模式。
// 场地变复杂（有墙、有绕路）时再换 MoveTo 不迟。
//
// 【它只在战斗阶段动】其余相位在 Tick 里直接 return，一个输入都不发。
// 相位门是唯一的开关 —— 不需要"冻结/解冻 AI"这种状态，因为 AI 的动作
// 本来就全部产生于这个函数里，函数不进那个分支就什么都不发生。
//
// ===========================================================================
// 【这个类在 AI 四层里的位置：执行层】
//
//   观测层  UArenaBehaviorObserver  记玩家怎么打           → FArenaPlayerProfile
//   战略层  UArenaAIDirector        决定这一回合怎么打       → FArenaStrategicIntent
//   战术层  UArenaTacticalPlanner   决定这一下按哪个键       → FArenaTacticalDecision
//   执行层  本类                     合成数值，然后真的按键
//
// 本类【不认识任何策略，也不认识任何技能】—— 它只做三件事：
//   ① ResolveTuning：把 BP 上的基线数值和 intent 的偏移合成一组可用数值；
//   ② BuildCandidates / BuildTacticalContext：把"有哪些技能、现在什么状况"
//      整理成战术层能读的干净输入（它不该自己去查 ASC）；
//   ③ 照着结论动作（下面那些 PreferredRange / AttackInterval 等属性从此
//      是"基线"而不是"实际使用值"，实际用的是 CurrentTuning）。
//
// 于是策略换成 LLM、战术换成 GOAP 或学习模型、整套换成蓝图里手搓的规则，
// 这个文件一行都不用改。
// 【但要改 BP 上那几个基线数时要注意】改了基线，策略的偏移是叠在它上面的 ——
// 比如把 PreferredRange 从 130 调到 200，Poke 立场下就会算成 200+60。
// 这是有意的：基线是"性格"，策略是"针对"，两者相乘而不是互相覆盖。
// 战术层的临时位移（PreferredRangeDelta）再叠一层 —— 那是"应激"，寿命一秒。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "GAS/ArenaAITypes.h"
#include "GAS/ArenaTypes.h"
#include "ArenaBotController.generated.h"

class AArenaGameState;
class AArenaPlayerState;
class AHeroCombatCharacter;
class UArenaAIDirector;
class UArenaBehaviorObserver;
class UArenaTacticalPlanner;
class UInputConfig;

UCLASS()
class LOL_API AArenaBotController : public AAIController
{
	GENERATED_BODY()

public:
	AArenaBotController();

	virtual void Tick(float DeltaSeconds) override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

protected:
	// =====================================================================
	// 配置 —— 全在 BP_ArenaBotController 的 Details 面板里改
	// =====================================================================

	/**
	 * 技能槽位表（复用玩家的 UInputConfig：槽位标签 + 对应的 InputAction）。
	 *
	 * 【这里只用它的 SlotTag】InputAction 那半边对 Bot 没有意义（它不走 Enhanced Input），
	 * 但复用同一份资产的 SlotTag 列表能保证"Bot 会的技能 = 玩家能按的技能"，
	 * 而且以后加一个槽位只需要改一处。为空时 Bot 只普攻。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Input")
	TObjectPtr<UInputConfig> AbilitySlots;

	/** 两次普攻之间的间隔（秒）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combat", meta = (ClampMin = "0.1", Units = "s"))
	float AttackInterval = 1.1f;

	// ---------------------------------------------------------------------
	// 连招（踩连段窗口）
	//
	// 【为什么这几个参数是必须的，而不是"把 AttackInterval 调小就行"】
	// 普攻只是一个起手；真正打出伤害的是三/四段连招，而连招靠的是
	// 「本段窗口开着的时候再按一次」—— 窗口由技能自己的定时器开合，长度只有
	// (ChainWindowCloseTime - ChainWindowOpenTime) × 蒙太奇长度，典型 0.1~0.3 秒，
	// 而且攻速越快越短。用固定间隔去撞它，撞不上是必然：调小间隔只会变成
	// "按得更勤但每一下都重开第 0 段"，观赏性更差、伤害更低。
	// 所以在执行层改成：窗口标签 State.ComboWindow 一开就踩（见 TickCombatPhase 的普攻段）。
	// ---------------------------------------------------------------------

	/**
	 * 看到连段窗口打开之后，隔多久按下去（秒）。
	 *
	 * 【为什么必须远小于窗口长度】窗口一开就按当然最稳（那一刻按下去必然在窗口内），
	 * 但 0 延迟按出来像机器。这里留一点反应时间，让它落在窗口前半段：
	 * 默认 0.06 + 抖动 0.04 → 最晚 0.1 秒，对最短的窗口（攻速拉满时约 0.1 秒）也还够。
	 * 攻速再高的话把这个数往下调。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combo", meta = (ClampMin = "0.0", Units = "s"))
	float ChainReactionDelay = 0.06f;

	/** 反应时间的随机上浮（秒）。每次都精确到同一毫秒会看着像外挂。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combo", meta = (ClampMin = "0.0", Units = "s"))
	float ChainReactionJitter = 0.04f;

	/** 起手一套的时候，这一套最多打几下（含起手那下）。持剑三段填 3、空手四段填 4 都能跑。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combo", meta = (ClampMin = "1", ClampMax = "8"))
	int32 ChainLength = 4;

	/**
	 * 每接一段的放弃概率。
	 *
	 * 【为什么不是所有窗口都踩】踩满是理论最优，但不是人的行为 ——
	 * 会连的人也会看情况停手（对面交控制、自己血线危险、只是压一下距离）。
	 * 全踩满的 Bot 打起来像脚本，而且它会在该收手的时候把最后一段送进对面的反打里。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combo", meta = (ClampMin = "0", ClampMax = "1"))
	float ChainDropChance = 0.2f;

	/**
	 * 血线低于这个比例、且对面手里有牌（不在冷却）时，不【起手】新的连招。
	 * 0 = 关掉这条规则。默认 0.25：残血又打不动对面的时候，压上去连招等于送。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combo", meta = (ClampMin = "0", ClampMax = "1"))
	float ChainHealthFloor = 0.25f;

	/**
	 * 完美窗口正开着时，隔多久按下去（秒）。默认 0.03 + 抖动 0.03 → 落在窗口内的前 1/4 左右。
	 *
	 * 【为什么比 ChainReactionDelay 小】完美窗口（剑套第 1 段是蒙太奇 [0.25, 0.50]，攻速 1x 时
	 * 世界时间约 0.25 秒宽）比连段窗口窄得多，而这一按必须【落在窗口里】才算完美；
	 * 反应延迟用 0.06+0.04 会在攻速拉高（窗口按 Rate 同比变窄）时顶到窗口后半段。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combo", meta = (ClampMin = "0.0", Units = "s"))
	float PerfectPressDelay = 0.03f;

	/** 完美窗口那一按的随机上浮（秒）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combo", meta = (ClampMin = "0.0", Units = "s"))
	float PerfectPressJitter = 0.03f;

	/** 两次技能之间的间隔（秒）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combat", meta = (ClampMin = "0.1", Units = "s"))
	float SkillInterval = 2.5f;

	/** 决策节奏（秒）。移动方向按这个频率重算，不是每帧。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combat", meta = (ClampMin = "0.02", Units = "s"))
	float DecisionInterval = 0.25f;

	/**
	 * 想和对手保持的距离（厘米）。
	 *
	 * 【为什么要有这个数】不用它的话 Bot 会一路走进对手身体里（胶囊体会互相挡住，
	 * 表现是两个模型贴着抖）。1V1 里"贴脸"和"拉开"本身就该是个可调的性格参数。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combat", meta = (ClampMin = "0", Units = "cm"))
	float PreferredRange = 180.f;

	/** 距离误差容限：在 [PreferredRange ± 这个值] 之内就不再前后动，只绕圈。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combat", meta = (ClampMin = "0", Units = "cm"))
	float RangeTolerance = 90.f;

	/** 普攻的射程（厘米）。超出就不打了。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combat", meta = (ClampMin = "0", Units = "cm"))
	float AttackRange = 220.f;

	/** 绕圈时侧向速度占总速度的比例（0 = 直着走，1 = 纯绕圈）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combat", meta = (ClampMin = "0", ClampMax = "1"))
	float StrafeWeight = 0.45f;

	/** 多久换一次绕圈方向（秒）。让它看起来不像在绕固定轨道。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Combat", meta = (ClampMin = "0.5", Units = "s"))
	float StrafeFlipInterval = 4.f;

	// =====================================================================
	// 自适应（观测 + 战略）
	//
	// 【上面那些 Combat 属性从此是"基线"】它们仍然是这个 Bot 的性格 ——
	// 策略只在这个基线上加有界的偏移。所以：
	//   - 调基线立刻生效（策略还不知道该针对谁的时候，Bot 就是基线性格）；
	//   - 把战略层的 bEnableAdaptation 关掉 = 完全回到上面那套数值。
	// =====================================================================

	/**
	 * 战略层。留空则用 C++ 默认的启发式实现（构造函数已经建了一个）。
	 *
	 * 【要换成别的策略就换这个指针】新建一个 UArenaAIDirector 的蓝图子类、
	 * 覆写 BuildIntent 事件，然后在这里填上它。观测层和执行层都不用动。
	 *
	 * Instanced = 允许在 Details 面板里就地新建一个子类实例（不是引用某个资产）。
	 */
	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Arena|Bot|AI")
	TObjectPtr<UArenaAIDirector> StrategyDirector;

	/**
	 * 观测层。留空则用 C++ 默认实现（构造函数已经建了一个）。
	 * 存/读档的槽位名配在它身上，不在这里。
	 */
	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Arena|Bot|AI")
	TObjectPtr<UArenaBehaviorObserver> Observer;

	/**
	 * 战术层。留空则用 C++ 默认的效用评分实现（构造函数已经建了一个）。
	 *
	 * 【要换成别的战术就换这个指针】新建一个 UArenaTacticalPlanner 的蓝图子类、
	 * 覆写 PlanTactics 事件，然后在这里填上它。观测层、战略层和执行层都不用动。
	 *
	 * 【它要生效必须配 AbilityProfiles】没配画像的技能会退化成"中性的伤害技能"
	 * 参与评分（会用，但不特别想用）—— 能跑，但体现不出"该交什么"的智能。
	 * 至少给每个槽位填一个 Role，那是这一层唯一的"知识注入点"。
	 */
	UPROPERTY(EditDefaultsOnly, Instanced, BlueprintReadOnly, Category = "Arena|Bot|AI")
	TObjectPtr<UArenaTacticalPlanner> TacticalPlanner;

	/**
	 * 多久重算一次战略意图（秒）。
	 *
	 * 【为什么不每帧】意图里有一部分依赖血量（Pressure），那个需要跟得上战况；
	 * 但每帧重算意味着"攻击间隔"这类阈值在帧间抖动，而它们是被拿去和
	 * "距上次攻击多久"比较的 —— 阈值抖动会让出手节奏看起来发飘。
	 * 1 秒是个折中：血量变化跟得上，节奏稳。
	 *
	 * 【为什么不是每回合算一次】那样残血的时候 Bot 反应还是慢的 ——
	 * 而"被压着打要更急"恰恰是最该即时生效的一条。
	 *
	 * ⚠️ 战术层的刷新节奏【不跟这个走】，它跟着 CurrentTuning.DecisionInterval
	 * （默认 0.25s）。两者刻意不同频：战略是慢变量、战术是快变量。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|AI", meta = (ClampMin = "0.1", Units = "s"))
	float IntentRefreshInterval = 1.f;

	/**
	 * 战术层的临时位移单次最多能推多远（厘米）。
	 *
	 * 【为什么执行层还要夹一次，战术层不是已经夹过了吗】和 ResolveTuning
	 * 同一个道理：战术层不知道基线距离是多少，所以它给的是"往哪个方向推多少"，
	 * 上限只有拿到基线的那一层才能定。而且这条也兜住了"BP 子类覆写时忘了夹"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|AI", meta = (ClampMin = "0", Units = "cm"))
	float MaxTacticalRangeDelta = 150.f;

	/**
	 * 某个槽位按下去但没发出来（比如「按住选目标」那条链失败了）之后，
	 * 这么多秒内不再把它当候选。
	 *
	 * 【为什么需要它】没有它的话，一个永远发不出去的技能会每个技能周期
	 * 都被评分选中一次、每次都失败 —— 表现是"Bot 站着不放技能"，
	 * 而且日志上看不出为什么（因为失败是静默的）。有了它，它会退到第二个选择上。
	 *
	 * 【为什么是临时而不是永久】失败往往是有条件的（目标不在射程、正在被沉默）。
	 * 永久拉黑会让 Bot 在条件恢复之后仍然不用它。几秒是一条"这一波算了、
	 * 下一波再试"的线。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|AI", meta = (ClampMin = "0.1", Units = "s"))
	float FailedCastRetryDelay = 3.f;

	/**
	 * 一次技能出手之后，等这么久看对手掉不掉血。到点还没掉就记一次"没打中"。
	 *
	 * 【为什么是这个量级】短于 0.3 秒的话，飞行道具（飞刀）还没飞到；
	 * 长于 1 秒的话，普攻（默认间隔 1.1 秒）会挤进窗口里，把普攻的伤害
	 * 记成技能的功劳。0.6 是"够飞行道具飞到、又不至于被下一次普攻污染"的折中。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|AI", meta = (ClampMin = "0.1", Units = "s"))
	float OutcomeCreditWindow = 0.6f;

	/**
	 * 对手掉多少血才算"这一下打中了"（占最大血量的比例）。
	 *
	 * 【为什么需要一个门槛】格挡、护盾、被其他东西抵消会让掉血量是 0 或者很小；
	 * 而随机噪声（这个项目血量是浮点）也可能让差值非零。1% 是"明显不是噪声"的量级。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|AI", meta = (ClampMin = "0", ClampMax = "1"))
	float OutcomeHitHealthDelta = 0.01f;

	/** 画战略层当前状态（立场 + 实际使用的数值）。排查"AI 到底有没有在针对我"时打开。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Debug")
	bool bDrawAIDebug = false;

	/**
	 * 收到待选择之后隔多久作答（秒）。
	 *
	 * 【必须有延迟，而且必须是定时器】不能收到就立刻答：
	 *   - 服务端是在 SetPendingPrompt 里同步广播的，在那个广播里直接调 ResolveChoice
	 *     等于在"发牌"这个函数还没返回的时候就把牌收走、又发下一份 —— 递归，而且
	 *     GameMode 那边手上还拿着这一份的引用；
	 *   - 顺便，秒答的 Bot 在观战视角里看起来像界面坏了。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Reward", meta = (ClampMin = "0", Units = "s"))
	float RewardAnswerDelay = 1.2f;

	/**
	 * 面对「使用锻造器」时，用掉的概率。
	 *
	 * 【为什么不干脆全用掉】需求给了"锻造器机会 ×5"，一次全花完会让这个 Bot
	 * 在第 1 回合就把五次抽完，后面几回合的奖励阶段变成空转。
	 * 0.5 的意思是"每次看到都会掷一次骰子"，所以攒着的次数会在后面几回合陆续花掉。
	 * 配 1 = 见到就用，配 0 = 永远不用（只留「进入战斗」）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Reward", meta = (ClampMin = "0", ClampMax = "1"))
	float ForgeUseChance = 0.5f;

	/** 画调试线（朝向 + 想去的方向）。排查"AI 为什么不动"时把它打开。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Bot|Debug")
	bool bDrawDebug = false;

private:
	/** 按相位分派。三个 Handle 各自管自己那一段，互不干扰。 */
	void TickRewardPhase(AArenaPlayerState* Me);
	void TickCombatPhase(float DeltaSeconds, AArenaGameState* GameState, AArenaPlayerState* Me);

	// ---------------------------------------------------------------------
	// 自适应：相位边沿 → 通知观测层；战斗中定期重算意图
	// ---------------------------------------------------------------------

	/**
	 * 相位发生切换时调一次（只在边沿，不是每帧）。
	 *
	 * 【为什么观测的时间点挂在相位边沿上】"一回合"的边界是 GameState 定义的，
	 * 不是 Bot 自己数的。用同一份真相（相位机）而不是另开一个计时器，
	 * 是为了避免两个计时互相漂移 —— 那正是这个项目在死亡重生上已经吃过一次的亏
	 * （见 HeroCombatCharacter 里"复活时长只有一处来源"那段）。
	 */
	void HandlePhaseTransition(EArenaPhase From, EArenaPhase To, AArenaPlayerState* Me);

	/** 重算战略意图，并把它折算成当前实际使用的数值。 */
	void RefreshIntent(const AActor* Hero, const AActor* Enemy);

	/**
	 * 基线 + 偏移 → 实际使用的数值。
	 *
	 * 【夹在这里而不是战略层】战略层只知道偏移，不知道基线是多少
	 * （那是 BP 上的配置）。所以"容错不能为负""间隔不能为 0"这类
	 * 只有拿到基线才能判的约束，必须在这一层收口。
	 */
	FArenaBotTuning ResolveTuning(const FArenaStrategicIntent& Intent) const;

	/** 画战略层的当前状态。 */
	void DrawAIDebug(const AActor* Hero, const AActor* Enemy) const;

	// ---------------------------------------------------------------------
	// 战术层：整理输入 → 问一次 → 执行
	//
	// 【为什么整理输入是执行层的事】战术层不该认识 UAbilitySystemComponent、
	// 槽位授权、"按住选目标"这些机制 —— 它只该认识"有个技能、现在能不能用、
	// 是什么角色"。把查询留在这一层，战术层就变成了纯函数（同样的输入给同样的
	// 输出），而纯函数是可以离线回放、可以单测的。这也是它将来能换成
	// GOAP / 学习模型的前提。
	// ---------------------------------------------------------------------

	/** 重算一次战术决策，结果进 CurrentDecision。 */
	void RefreshTacticalDecision(const AActor* Hero, const AActor* Enemy, float Now);

	/**
	 * 按刚算出来的 CurrentDecision 更新承诺（CommittedSlot / CommittedSince）。
	 *
	 * 【为什么单独一个函数而不是塞进 RefreshTacticalDecision】承诺要在
	 * PlanTactics 返回【之后】才更新 —— 它是"上一拍的输出"，而本拍的 context
	 * 已经用旧值填好了。写在一起很容易在下次改动时被挪到 PlanTactics 之前，
	 * 那样承诺就变成了"本拍影响本拍"，黏性会失效且没有任何报错。
	 */
	void UpdateCommitment(float Now);

	/** 此刻的战况。全是查好的值，战术层拿到就能算。 */
	FArenaTacticalContext BuildTacticalContext(const AActor* Hero, const AActor* Enemy, float Now) const;

	/** 槽位 → 候选。含"槽位授没授权""在不在冷却""最近是不是试过没成功"。 */
	TArray<FArenaAbilityCandidate> BuildCandidates(const AHeroCombatCharacter* Hero, float Now) const;

	/**
	 * 执行战术层的结论：真的要按下去。
	 *
	 * 【为什么不返回成功与否】本项目的入口（AbilityInputTagPressed）只表达
	 * "我按了"，能力自己会判冷却/射程/沉默，判不过就静默不激活 —— 所以
	 * "成功了"这件事在这里是取不到的。能取到的只有"按住选目标"那条链的返回值，
	 * 那个会拿去做重试抑制（见 SlotRetryAfter）。
	 */
	void ExecuteTacticalDecision(AHeroCombatCharacter* Hero, AActor* Enemy, const FArenaTacticalDecision& Decision, float Now);

	// ---------------------------------------------------------------------
	// 读人：把画像里那个一直没人用的绕圈倾向变成绕圈方向
	// ---------------------------------------------------------------------

	/**
	 * 玩家的绕圈倾向：OutSign = 他习惯往哪边转（-1 / +1），OutStrength = 这个习惯有多强（0~1）。
	 *
	 * 【为什么值得单独一个函数】这两个数在 OnPossess 和每次换向时都要用，
	 * 而且都要先过"样本够不够"那道门 —— 写两遍就会出现一处漏判、另一处判了，
	 * 表现是"有时候读人有时候不读"，很难查。
	 *
	 * 没有样本 / 两边一样多时 OutStrength = 0，调用方于是退回原来的随机绕圈。
	 */
	void GetPlayerOrbitBias(float& OutSign, float& OutStrength) const;

	// ---------------------------------------------------------------------
	// 自省：出手之后看结果，把"这个技能到底灵不灵"喂回战术层
	// ---------------------------------------------------------------------

	/**
	 * 记下"某槽位刚出手了"，开始等结果。执行层在真的按下去之后调。
	 *
	 * 【为什么由执行层做】只有它知道按没按下去、按的是谁、当时的血量是多少。
	 * 战术层要保持纯函数（见 PlanTactics 的注释），所以信用分配留在这里，
	 * 算出来的结论再通过 NotifyOutcome 喂回去。
	 */
	void ArmOutcomeCredit(const FGameplayTag& SlotTag, const AActor* Enemy, float Now);

	/** 每帧看一眼：掉血了 = 打中；窗口过了还没掉 = 没打中。 */
	void ResolveOutcomeCredit(const AActor* Enemy, float Now);

	/** 定时器到点：真的提交这次选择。 */
	void SubmitRewardAnswer();

	/** 从待选里挑一个下标。返回 INDEX_NONE = 不该作答（没有待选 / 选项都无效）。 */
	int32 ChooseRewardOption(const AArenaPlayerState* Me) const;

	/** 停止一切动作：清掉焦点，并且不再产生移动输入。 */
	void Idle();

	/** 撤掉还没到点的作答定时器。离开奖励相位时调 —— 到点了也只会白跑一趟并记一条 Warning。 */
	void CancelPendingRewardAnswer();

	/** 上一次普攻 / 放技能的世界时间。 */
	float LastAttackTime = -1000.f;
	float LastSkillTime = -1000.f;

	/**
	 * 连招状态。
	 *
	 * 【为什么要自己记，而不是"窗口开着就按"】窗口开着就按会让 Bot 像连发枪：
	 * 键盘一路糊在普攻上，每段都踩满 —— 而真人是有节奏的（起手 → 接 → 接 → 收手）。
	 * bChainWindowWasOpen 用来抓窗口的上升沿：窗口一开只决定【这一下接不接】，
	 * NextChainPressTime 是那一刻定下的"该按下去的绝对时刻"。
	 * 边沿触发还顺带解决了"窗口很短、轮询可能刚好错过"的问题 ——
	 * 只要窗口存在过一帧，就一定会留下一个待按时刻。
	 */
	bool bChainWindowWasOpen = false;

	/** 这一套连招还打算接几下。<=0 表示没在连（或者已经决定收手）。 */
	int32 ChainHitsLeft = 0;

	/** 上升沿定下的"该按下去"的绝对世界时间。<0 = 没有待按的（含"正在等完美窗口"）。 */
	float NextChainPressTime = -1.f;

	/**
	 * 完美窗口的两个状态（State.PerfectWindowArmed / State.PerfectWindow 的上一帧值）。
	 *
	 * 【为什么不是"看到完美窗口就按"】比"看到就按"多一层：本段有没有完美窗口是【起手段就知道】的
	 * （Armed 从 StartStage 就挂着），所以连段窗口一开就要决定"这段要不要等"——
	 * 等错了（没窗口的段去等）会白等一整个连段窗口、连段直接断。
	 * WasOpen 用来抓"完美窗口刚开"那一帧（这就是按下去的时机）。
	 */
	bool bPerfectWindowWasArmed = false;
	bool bPerfectWindowWasOpen = false;

	/** 上一次重算移动方向的时刻。 */
	float LastDecisionTime = -1000.f;

	/** 上一次换绕圈方向的时刻，以及当前方向（+1 / -1）。 */
	float LastStrafeFlipTime = -1000.f;
	float StrafeSign = 1.f;

	/**
	 * 已经排上定时器、还没作答。
	 *
	 * 【为什么需要这个标志】Tick 每帧都会看到"有待选"，
	 * 不记一笔的话每帧都会重排一次定时器 —— 那个定时器永远不到期，
	 * 表现就是【AI 永远不选，这局卡在奖励阶段】。
	 * 定时器回调里清掉它，于是"答完又来了新的一份"（锻造器三选一接三选一）
	 * 下一帧会被重新排上。
	 */
	bool bAnswerQueued = false;

	FTimerHandle RewardAnswerHandle;

	// ---------------------------------------------------------------------
	// 自适应状态
	// ---------------------------------------------------------------------

	/** 当前这一回合的战略意图（战略层的输出，只读）。 */
	FArenaStrategicIntent CurrentIntent;

	/**
	 * 当前实际使用的数值 = 基线 + CurrentIntent 的偏移。
	 *
	 * 【为什么缓存而不是每次要用时现算】它是调试 HUD 唯一要显示的落点，
	 * 而且"这次计算用的是哪组数"必须和"这次动作实际用的数"是同一份 ——
	 * 现算两次的话，中间意图被刷新过一次就会出现"看到的和做的不一致"。
	 */
	FArenaBotTuning CurrentTuning;

	/** 上一次重算意图的时刻。 */
	float LastIntentRefreshTime = -1000.f;

	/**
	 * 上一帧看到的相位。用来只在【切换的那一刻】触发观测层的回合边界。
	 *
	 * 初值给 WaitingToStart 是为了让"第一次进入备战"也算一次边沿 ——
	 * 否则第一回合的 BeginRound 会被漏掉（上一帧的初值和当前帧一样时不算切换）。
	 */
	EArenaPhase LastObservedPhase = EArenaPhase::WaitingToStart;

	/**
	 * 本回合开始时 Bot 自己的败场数。回合结束时对比它就知道谁赢了。
	 *
	 * 【为什么用败场数的差值，而不是"谁倒下了"】倒下这件事有几种观测不到的形态：
	 * pawn 掉出世界被 KillZ 销毁（本项目实测过，见 ArenaGameMode 的
	 * MissingPawnGraceSeconds）、双方同归于尽、结算时 pawn 已经不存在。
	 * 回合战绩是 GameMode 已经判完并写下的结果，读它是唯一在所有情况下都对的口径。
	 */
	int32 RoundsLostAtRoundStart = 0;

	/** 这一局的观测已经开始了（BeginMatch 幂等用）。 */
	bool bMatchStarted = false;

	// ---------------------------------------------------------------------
	// 战术层状态
	// ---------------------------------------------------------------------

	/** 最近一次战术决策。既用于执行，也用于调试 HUD。 */
	FArenaTacticalDecision CurrentDecision;

	/**
	 * 战术层的"承诺"：上一拍选中的槽位，以及从什么时候开始选它。
	 *
	 * 【它解决什么】战术层每 DecisionInterval（默认 0.25s）重算一次 argmax，
	 * 而真正按下去还受 SkillInterval 约束、可能在一秒半之后。这段窗口里评分会在
	 * 两个分数接近的技能之间来回翻，于是【最后按下去的那个】和【决策理由里写的
	 * 那个】是两码事 —— 表现就是乱按。承诺给已经在选的技能加分，别的技能要
	 * 明显更好才抢得走，这个窗口里的抖动就没了。
	 *
	 * 【为什么放在执行层而不是战术层】战术层必须是纯函数。承诺是"上一拍决定了
	 * 什么"这件事实，只有执行层知道；它作为 context 的一项喂进去，战术层就
	 * 仍然"同样的输入给同样的输出"，离线回放和单测都不受影响
	 * （和 SlotRetryAfter 是同一个理由，见下面那个成员的注释）。
	 */
	FGameplayTag CommittedSlot;

	/** 当前承诺从什么时候开始（世界时间）。只用于调试 HUD 显示"承诺了多久"。 */
	float CommittedSince = 0.f;

	/**
	 * 槽位 → 在此之前不再当候选的时刻（世界时间）。
	 *
	 * 【为什么不放进战术层】这是"我试过、失败了"的记录 —— 只有执行层知道
	 * 自己按没按下去、有没有发出来。战术层保持纯函数（见 PlanTactics 的注释）。
	 * 所以它表现为【输入的一部分】：执行层在 BuildCandidates 里把它折进 bUsable。
	 */
	TMap<FGameplayTag, float> SlotRetryAfter;

	/**
	 * 等结果的那一次出手：槽位 + 截止时刻 + 出手那一刻对手的血量。
	 *
	 * 【为什么只能有一个"待结算"】同一时刻只可能有一次出手还没揭晓 ——
	 * 出手之间至少隔 SkillInterval（默认 2.5 秒），而窗口只有 0.6 秒。
	 * 用队列反而是为不存在的场景增加复杂度。
	 */
	FGameplayTag PendingOutcomeSlot;
	float PendingOutcomeDeadline = 0.f;
	float PendingOutcomeHealthAtCast = 1.f;
};
