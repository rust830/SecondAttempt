// 斗魂竞技场：AI 各层之间传递的数据。
//
// ===========================================================================
// 【这个头存在的理由：层的接缝要有名字】
//
//   观测层  UArenaBehaviorObserver  每帧看玩家  → FArenaPlayerProfile
//   战略层  UArenaAIDirector        这一回合怎么打 → FArenaStrategicIntent
//   战术层  UArenaTacticalPlanner   这一下做什么   → FArenaTacticalDecision
//   执行层  AArenaBotController     合成数值并真的按键 → FArenaBotTuning
//
// 每个结构是一层的"货币"。把它们定义在一个只有数据、不依赖任何游戏类的头里，
// 是为了让各层可以各自替换：换掉观测方式不动策略，换掉策略（启发式 → LLM）不动战术，
// 换掉战术（效用评分 → GOAP / 学习模型）不动执行。
// 依赖方向是单向的 —— 这个头不认识 UAbilitySystemComponent，也不认识 Bot。
//
// 【战略层和战术层的分工，一句话】
//   战略层说"这一回合我要拉扯"——慢变量，一秒级，跨回合有记忆；
//   战术层说"此刻该按哪个键"——快变量，每次决策一次，只看当下。
// 两者都不认识具体的技能，只认识角色（Role）和情境（Context）。
//
// 放 GAS/ 的理由：判据见 CONVENTIONS.md 规则 2 —— 它的字段虽然只是数，
// 但整套东西的输入是 GameplayTag（冷却标签 / 槽位标签），消费方是持有 ASC 的角色。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "ArenaAITypes.generated.h"

/**
 * Bot 这一回合想怎么打。战略层的输出，战术层按它挑动作。
 *
 * 【为什么是枚举而不是几个 float】"怎么打"是一个整体选择 —— 拉扯和贴脸不是
 * 两个独立的旋钮，是同一根轴的两端。拆成独立的 float 会造出"既贴脸又拉扯"
 * 这种自相矛盾的中间态，而实际上这种情况下的正确表现是"没有倾向"（Balanced）。
 * 具体的数值偏移（多贴、多远）由 intent 里的那些 float 承担，枚举只表达"往哪边"。
 */
UENUM(BlueprintType)
enum class EArenaAIStance : uint8
{
	/** 没有明显倾向：按基线性格打。观测样本太少时就是它。 */
	Balanced		UMETA(DisplayName = "均衡"),

	/** 贴脸压制：玩家怕近身（远程/脆皮/一直在退），那就压上去。 */
	Brawl			UMETA(DisplayName = "贴身压制"),

	/** 拉扯消耗：玩家爱贴脸，那就把他的距离拉开，用容错换安全。 */
	Poke			UMETA(DisplayName = "拉扯消耗"),

	/** 后手反打：玩家进攻性很强，那就站住等他的动作，在他的收招里打。 */
	Counter			UMETA(DisplayName = "蹲反打"),

	/** 追击：玩家一直想跑/放风筝，那就别让他拉开。 */
	Chase			UMETA(DisplayName = "追击"),
};

/**
 * 局间"战略备忘录" —— 这是整个系统里【外部智能的输入口】。
 *
 * ===========================================================================
 * 【为什么 LLM 应该落在这里，而不是落进 BuildIntent】
 * 把 LLM 放进每回合的同步决策里有四个绕不过去的问题：延迟（玩家在等）、
 * 成本（每局几十次调用）、不可复现（同一个局面两次给不同答案，没法调参复盘）、
 * 以及离线不可测（CI 里跑不了）。而这些问题的根源都是"它在热路径上"。
 *
 * 放到【局间】就全没了：一局结束、下一局开始之前，有整整一段没有实时性要求的
 * 时间窗口。在那里把画像喂给 LLM，产出一份"针对这个玩家的判断"存进存档，
 * 下一局由本地的 C++ 规则（UArenaAIDirector::BuildIntent）读它来做偏置。
 * 延迟无所谓（下一局才用）、一局只调一次、产物落盘所以可复现、离线也能造数据。
 *
 * 【为什么不是"让 LLM 直接输出数值"】它输出的是【判断】，不是【参数】：
 * "他会一直后撤放风筝，第一回合就该压" —— 这句话到"PreferredRange 减 60"
 * 之间的换算必须留在 C++ 里（那就是 RangeBias 和 Confidence 的用途）。
 * 让模型直接吐厘米数，等于把一个不可解释的映射放进热路径，改一次要重训。
 *
 * 【生产者在哪】UArenaAIDirector::ComposeMemo —— 默认实现是本地启发式，
 * 蓝图子类覆写它就能接 LLM / 离线分析 / 别的什么。见那个函数的注释。
 * ===========================================================================
 */
USTRUCT(BlueprintType)
struct FArenaStrategicMemo
{
	GENERATED_BODY()

	/** 有没有内容。false = 不施加任何偏置（默认，也是"还没接过 LLM"的状态）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Memo")
	bool bHasContent = false;

	/**
	 * 人类可读的判断，比如"他会一直后撤放风筝，第 1 回合就该压"。
	 *
	 * 【为什么留一个纯文本字段】① 调试 HUD 直接显示它，排查"Bot 为什么这么打"
	 * 比读四个 float 快得多；② 接 LLM 时这就是模型的原话，出问题时你能看到它到底说了什么；
	 * ③ 它不影响任何逻辑，可以随便写。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Memo")
	FString Note;

	/**
	 * 这份判断的可信度（0~1）。所有偏置都乘它。
	 *
	 * 【为什么必须有这个数，而不是"有判断就用"】备忘录是一个可能出错的来源
	 * （模型幻觉、画像过期、玩家换号）。可信度是给它一个"可以自我怀疑"的旋钮：
	 * 低可信度的判断会退化成轻微偏置而不是完全接管。默认 0 = 完全不生效。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Memo")
	float Confidence = 0.f;

	/** 距离倾向：-1 = 贴上去，+1 = 拉开。乘 MaxRangeOffset 后叠加。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Memo", meta = (ClampMin = "-1", ClampMax = "1"))
	float RangeBias = 0.f;

	/** 节奏倾向：-1 = 稳一点，+1 = 凶一点（间隔缩短、决策更频繁）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Memo", meta = (ClampMin = "-1", ClampMax = "1"))
	float AggressionBias = 0.f;

	/**
	 * 建议的开局立场。
	 *
	 * 【它和统计出来的立场谁优先】统计优先 —— 样本够了就以观测为准，
	 * 备忘录只补"还没观测够"的那一段。理由是统计是实测、备忘录是推断；
	 * 但反过来，只有备忘录的时候（第一局、或者玩家换了个号）它就是唯一的先验，
	 * 这正是它存在的意义：让 Bot 在第一回合就有备而来，而不是从零开始。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Memo")
	EArenaAIStance PreferredStance = EArenaAIStance::Balanced;

	/** 备忘录格式版本。和 Profile::SchemaVersion 分开，因为它们的生产者不同。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Memo")
	int32 MemoSchemaVersion = 1;
};

/**
 * 观测到的玩家行为画像。
 *
 * ===========================================================================
 * 【跨局持久化 —— 这是整个系统里最重要的一个决定】
 * 一局 1V1 只有几个回合，单局内学不出任何有统计意义的东西。所以这个结构
 * 【必须跨局累积】：读档 → 打一局 → 存档，下一个人机对局里 Bot 才"记得"你。
 *
 * 代价是它同时是一个存档 schema：改字段名/语义 = 改存档格式。所以：
 *   - 只增字段，不改旧字段的语义（旧存档读进来新字段拿默认值，能活）；
 *   - SchemaVersion 变了好让读取方知道该丢弃还是该迁移。
 *
 * 【为什么全是显式命名的 float，而不是 TMap<FGameplayTag, float>】
 * 本项目已经踩过 TMap 的坑：TMap 写进蓝图 CDO 之后，CDO 上读得到、
 * Spawn 出来的实例却拿到一张【空表】，且没有任何报错
 * （见 HeroCombatCharacter.h 里 HitReactMontage 那段注释）。
 * 这里的字段是要存档、要在 Details 面板里看的，不能用会静默变空的东西。
 * 代价是加一个指标要动这个结构 —— 这是有意的摩擦，指标本来就该慎重加。
 * ===========================================================================
 *
 * 【所有比率都是"指数滑动平均"】不是"总次数 / 总时间"。理由是跨局累积时，
 * 早期几百场的样本会和最近几场等权，导致 Bot 永远在针对你三个月前的打法。
 * EMA 让最近的样本权重更高，同时保留历史记忆 —— 换了个打法它跟得上。
 */
USTRUCT(BlueprintType)
struct FArenaPlayerProfile
{
	GENERATED_BODY()

	/** 存档格式版本。改字段语义时 +1，读取方据此决定丢弃还是迁移。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	int32 SchemaVersion = 2;

	// ---------------------------------------------------------------------
	// 样本量 —— 所有其它字段的置信度都由它决定
	// ---------------------------------------------------------------------

	/** 累积观测过的完整对局数（每次 Store 前 +1）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	int32 MatchesObserved = 0;

	/** 累积观测过的回合数。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	int32 RoundsObserved = 0;

	/** 累积的有效战斗观测时长（秒）。太短的话距离统计没有意义。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	float TotalCombatSeconds = 0.f;

	/** 玩家赢下的回合数。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	int32 RoundsPlayerWon = 0;

	/** Bot 赢下的回合数。用来看"这个玩家是不是比 Bot 强"。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	int32 RoundsBotWon = 0;

	// ---------------------------------------------------------------------
	// 距离偏好 —— 决定 Bot 该压上去还是该拉开
	// ---------------------------------------------------------------------

	/** 交战距离的 EMA（厘米，从角色中心算）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	float AvgEngagementDistance = 0.f;

	/**
	 * 玩家处在"贴脸"距离带里的时长占比（0~1）。
	 * 高 = 这个玩家习惯压上来打。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	float CloseRangeRatio = 0.f;

	/**
	 * 玩家处在"远距离"带里的时长占比（0~1）。
	 * 高 = 这个玩家习惯放风筝 / 拉扯。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	float FarRangeRatio = 0.f;

	// ---------------------------------------------------------------------
	// 机动倾向
	// ---------------------------------------------------------------------

	/**
	 * 玩家横向绕圈的左右分布（0 = 全往右，1 = 全往左，0.5 = 均匀）。
	 *
	 * 【为什么值得记】绕着同一个方向转的玩家，移动是可以预判的 ——
	 * 战术层可以据此调整瞄准提前量。值离 0.5 越远，可预判性越强。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	float PlayerStrafeLeftRatio = 0.5f;

	/**
	 * 谁在主动拉近距离。
	 *   +1 = 距离的变化几乎全是玩家在靠近；-1 = 全是玩家在后退；0 = 双方对等。
	 *
	 * 这是"进攻性"最直接的一个代理量 —— 不用去看他按了什么键，
	 * 只看距离是在被谁改变。观测成本为零（本来就是每帧算距离）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	float ApproachBias = 0.f;

	// ---------------------------------------------------------------------
	// 技能使用
	// ---------------------------------------------------------------------

	/** 累积观测到的玩家技能释放次数。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	int32 PlayerSkillCasts = 0;

	/** 技能释放频率的 EMA（次 / 秒）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	float SkillsPerSecond = 0.f;

	/**
	 * 玩家技能冷却的平均占用率（0~1）。
	 *
	 * 【和 SkillsPerSecond 的区别】那个数"放得多快"，这个数"手里有没有牌"。
	 * 1.0 = 一直在冷却中（技能好了就放，没有保留），低值 = 攒着不放。
	 * 对策略的意义：满占用率的玩家没有后手，Bot 可以放心压；
	 * 低占用率的玩家手里一直捏着东西，Bot 压上去要吃一套。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	float SkillCooldownOccupancy = 0.f;

	// ---------------------------------------------------------------------
	// 局间判断 —— 唯一一个【不由观测层写】的字段
	// ---------------------------------------------------------------------

	/**
	 * 局间的战略备忘录。观测层不碰它，战略层读它。
	 *
	 * 写入点只有一处：AArenaBotController 在整局结束时调
	 * UArenaAIDirector::ComposeMemo，把结果放进来再一起存盘。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Profile")
	FArenaStrategicMemo Memo;

	// ---------------------------------------------------------------------
	// 读取辅助
	// ---------------------------------------------------------------------

	/** 样本够不够支撑"针对"这个玩家。不够时战略层应当输出 Balanced。 */
	bool HasEnoughSamples() const
	{
		// 回合数是主要门槛：距离/机动统计在几个回合里就能收敛，
		// 而"这个玩家的胜率"需要更多回合才有意义。取 3 是实测里
		// "第三回合开始 Bot 就有稳定倾向"和"不显得刚开局就作弊"的折中。
		return RoundsObserved >= 3 && TotalCombatSeconds >= 20.f;
	}

	/** 玩家胜率（0~1）。没有样本时返回 0.5（不偏不倚）。 */
	float GetPlayerWinRate() const
	{
		const int32 Total = RoundsPlayerWon + RoundsBotWon;
		return (Total > 0) ? (static_cast<float>(RoundsPlayerWon) / static_cast<float>(Total)) : 0.5f;
	}
};

/**
 * 战略层的输出：这一回合"想怎么打"。
 *
 * ===========================================================================
 * 【为什么全是"偏移"和"倍率"，而不是绝对数值】
 * 绝对数值意味着战略层要知道"贴脸是多少厘米"—— 那是执行层的知识，
 * 而且玩家在 BP 上调过的基线性格会被策略整个覆盖掉（Details 面板上改了没反应，
 * 是最难查的一类 bug）。
 *
 * 这里一律是"相对于配在 BP 上的基线的增量"：策略只负责说"比你原来的习惯
 * 再拉开 60 厘米、技能间隔缩到 0.8 倍"，基线和上限都留在执行层。
 * 于是：调了 BP 上的数立刻生效；策略失效时最差退化成原版 Bot，不会变成一坨乱动。
 * ===========================================================================
 */
USTRUCT(BlueprintType)
struct FArenaStrategicIntent
{
	GENERATED_BODY()

	/** 这一回合的打法倾向。纯表达用（调试 HUD 显示），执行层可以按它分支。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Intent")
	EArenaAIStance Stance = EArenaAIStance::Balanced;

	/**
	 * 压力值（0~1）：Bot 现在有多吃紧。
	 *
	 * 由血量差 + 胜率差共同决定。当前唯一的生产性用途是让决策频率和
	 * 技能释放更激进（被压着打的时候反应要快），但它是给未来留的钩子 ——
	 * "AI Director" 这一类系统（L4D 的导演、Alien Isolation 的导演）
	 * 输出的就是这个东西，玩家行为进化只是它的一个消费者。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Intent")
	float Pressure = 0.f;

	/** 加到 PreferredRange 上的偏移（厘米，可正可负）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Intent")
	float RangeOffset = 0.f;

	/** 加到 RangeTolerance 上的偏移（厘米）。拉开的容错调大、贴身的容错调小。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Intent")
	float ToleranceOffset = 0.f;

	/** 乘到 AttackInterval 上的倍率（<1 = 打得更勤）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Intent")
	float AttackIntervalScale = 1.f;

	/** 乘到 SkillInterval 上的倍率（<1 = 技能放得更勤）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Intent")
	float SkillIntervalScale = 1.f;

	/** 加到 StrafeWeight 上的偏移（会夹在 [0,1]）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Intent")
	float StrafeWeightOffset = 0.f;

	/** 乘到 DecisionInterval 上的倍率（<1 = 反应更快）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Intent")
	float DecisionIntervalScale = 1.f;
};

/**
 * 执行层实际使用的一组数值 = BP 上的基线 + intent 的偏移，夹在有界范围内。
 *
 * 【为什么单独一个结构而不是就地算】它是"策略有没有生效"唯一可观测的落点 ——
 * 调试 HUD 直接画这个，一眼看出策略把哪个数推到了哪。就地算的话
 * 想确认"策略到底动没动"只能去读代码。
 */
USTRUCT(BlueprintType)
struct FArenaBotTuning
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tuning") float PreferredRange = 130.f;
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tuning") float RangeTolerance = 30.f;
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tuning") float AttackInterval = 1.1f;
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tuning") float SkillInterval = 2.5f;
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tuning") float StrafeWeight = 0.45f;
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tuning") float DecisionInterval = 0.25f;
};

// ===========================================================================
// 战术层
// ===========================================================================

/**
 * 一个技能在战术上"是干什么用的"。
 *
 * 【为什么需要这个枚举，而不是让 AI 去看技能的具体数值】
 * 效用评分要问的是"现在该不该交位移"，不是"这个技能伤害多少"。
 * 前者是设计意图（作者知道 GA_SpinSlash 是贴脸爆发），后者是数值
 * （算不出"该不该现在用"，只能算出"值不值"）。
 *
 * 而且这个划分是【跨技能类型稳定】的：换一个英雄、加一个新技能，
 * 战术层的代码一行不用改，只需要在新的 GA 上填一个 Role。
 * 这是这一层唯一需要人填的东西 —— 也是它唯一的"知识注入点"。
 */
UENUM(BlueprintType)
enum class EArenaAbilityRole : uint8
{
	/** 没填。按"中性的伤害技能"参与评分 —— 安全默认，不会因为漏填而变成永不使用。 */
	Unspecified	UMETA(DisplayName = "未指定"),

	/** 接近技：把人拉近的位移/突进。距离越远越想用。 */
	GapCloser	UMETA(DisplayName = "接近"),

	/** 远程消耗：能在近战臂展外用。距离越远越想用。 */
	Ranged		UMETA(DisplayName = "远程"),

	/** 爆发：要打一套的时候用。对方血量越低越值。 */
	Burst		UMETA(DisplayName = "爆发"),

	/** 斩杀：对方残血时的收尾。血量越低分数越高（比 Burst 更极端）。 */
	Execute		UMETA(DisplayName = "斩杀"),

	/** 防御：格挡/减伤。自己越吃紧越想用。 */
	Defensive	UMETA(DisplayName = "防御"),

	/** 躲闪：位移规避。和防御的区别是它换位置而不是换减伤。 */
	Evade		UMETA(DisplayName = "躲闪"),

	/** 回复：自己残血时用。 */
	Sustain		UMETA(DisplayName = "回复"),

	/** 增益：开打之前开。自己状态好、对方还没交牌的时候用。 */
	Buff		UMETA(DisplayName = "增益"),

	/** 控制/打断：对方手里有牌的时候用，把他的节奏打断。 */
	Disrupt		UMETA(DisplayName = "打断"),
};

/**
 * 命中率学习要不要评判这个技能。
 *
 * 【为什么需要它，而不是直接看 Role】Role 是"这个技能是干什么的"的一个粗分类，
 * 而"放出去该不该让对手掉血"是另一个问题，两者大部分时候一致、但不总是：
 * 打断类（SpinSlash 上挑、TurnSlash 回身）既是控制也是实打实的伤害，
 * 按 Role 判它学不到东西，按"它掉血"判又说得通。这是配置该有的自由度，
 * 所以给一条显式的开关而不是让代码去猜。
 */
UENUM(BlueprintType)
enum class EArenaHitRatePolicy : uint8
{
	/** 跟随角色：只有本来就该造成伤害的角色（爆发/斩杀/远程）才学。默认。 */
	FromRole	UMETA(DisplayName = "跟随角色"),

	/** 总是学：不管什么角色，掉血就记打中。控制类想被评判时勾它。 */
	Always		UMETA(DisplayName = "总是学"),

	/** 从不学：位移/增益这类明确不该被掉血评判的，显式关掉。 */
	Never		UMETA(DisplayName = "从不学"),
};

/**
 * 一个槽位的战术画像。UArenaTacticalPlanner::AbilityProfiles 里按 SlotTag 填。
 *
 * 【没填的槽位会退化成什么】用 Role = Unspecified + 默认距离带参与评分。
 * 也就是"会用它，但不会特别想用它"。这是有意的：漏配一个技能的表现应该是
 * "这个技能用得少一点"，而不是"这个技能永远不会被按"——后者在表现上
 * 和"技能坏了"完全一样，是最难查的一类问题。
 */
USTRUCT(BlueprintType)
struct FArenaAbilityProfile
{
	GENERATED_BODY()

	/** 对应 AbilitySlots 里的槽位标签（Ability.Slot.Q / W / E / R / D / F）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability")
	FGameplayTag SlotTag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability")
	EArenaAbilityRole Role = EArenaAbilityRole::Unspecified;

	/** 这个技能有意义的最近距离（厘米）。低于它评分降到地板。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0", Units = "cm"))
	float MinRange = 0.f;

	/** 这个技能有意义的最远距离（厘米）。高于它评分降到地板。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0", Units = "cm"))
	float MaxRange = 400.f;

	/**
	 * 自己血量到什么比例时最想用它（0 = 满血, 1 = 空血）。
	 * 0.5 = 无所谓（默认）。防御/回复填 0.9，增益填 0.1。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0", ClampMax = "1"))
	float SelfHealthSweetSpot = 0.5f;

	/** 对方血量到什么比例时最想用它。斩杀填 0.8，普通伤害填 0.5。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0", ClampMax = "1"))
	float EnemyHealthSweetSpot = 0.5f;

	/**
	 * 血量的"甜蜜区"有多宽。宽 = 什么时候都能用；窄 = 只有正好在那个血量才好用。
	 * 0.5 表示"从 0.5±0.5 都算好"，也就是全量程 —— 那是给无所谓的技能用的。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0.05", ClampMax = "1"))
	float HealthBand = 0.5f;

	/** 这个技能在这套技能组里有多重要。整体缩放它最后的分数。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0"))
	float Weight = 1.f;

	/**
	 * 刚放过之后，这么多秒内不再考虑它（秒）。
	 *
	 * 【和技能自己的冷却是什么关系】冷却管"能不能放"，这个管"想不想连放"。
	 * 一个 8 秒 CD 的技能不需要它；一个 0.5 秒 CD 的技能（或者没配 CD 的技能）
	 * 会因为没有它而变成"每 SkillInterval 都放同一个"—— 战术层就退化成了
	 * 原来那套轮询里最差的情况。所以默认给一个不算小的值。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0", Units = "s"))
	float RepeatLockout = 1.5f;

	/**
	 * 放这个技能需要多少能量（0~1 的比例；0 = 不耗能）。
	 *
	 * 【为什么是硬门而不是降分】能量不够时这个技能根本发不出来。降分的话它
	 * 仍然可能被选中，然后按下去什么都不发生 —— 表现和"技能坏了"一模一样，
	 * 而在日志上只能看到一行"结果=失败"。硬门让它干脆不参与竞争，退到第二选择。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0", ClampMax = "1"))
	float EnergyCost = 0.f;

	/**
	 * 连招：这个技能【跟在哪个槽位后面】用特别值（不填 = 不参与连招）。
	 *
	 * 例：强化普攻的画像填 FollowUpTag = 隐身槽位、Window = 3 秒、Bonus = 2.5，
	 * 于是"隐身之后的三秒内"它会被优先选中 —— 这就是一条不用写状态机的最小连招。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability")
	FGameplayTag FollowUpTag;

	/** 上面那个前摇技能放过之后，多少秒内算"还在连招窗口里"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0", Units = "s"))
	float FollowUpWindow = 0.f;

	/** 连招窗口内的分数倍率（>1 = 更想用）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0"))
	float FollowUpBonus = 1.f;

	// -----------------------------------------------------------------------
	// 对手此刻的姿势 —— 逐技能的额外偏好
	//
	// 【这四个和 Role 的关系：相乘，不是替代】
	// 下面每一个都【乘在角色默认值之上】，默认 1 = 不表态（完全按角色走）。
	// 于是不填的情况下行为和加这四个字段之前一模一样，填了才有额外效果。
	//
	// 【为什么需要它们】角色只有十种，而"对手处于什么姿势时该不该用这个技能"
	// 是个逐技能的问题：同是 Disrupt，上挑（起手控人）该在他没被控时用，
	// 而回身击退（接在后手）该在他已经被控时用。角色层面表达不了这个区别。
	// 默认 1 是有意的：漏填的表现是"退回角色默认"，不是"永远不用"。
	// -----------------------------------------------------------------------

	/** 对手被硬控时，这个技能的额外倍率。>1 = 趁现在，<1 = 这时候别用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0"))
	float EnemyHardControlMul = 1.f;

	/** 对手格挡 / 格挡免疫中的额外倍率。<1 = 这技能打不进去，让开。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0"))
	float EnemyDamageProofMul = 1.f;

	/** 对手攒着一记大的（隐身 / 强化普攻）时的额外倍率。<1 = 别在这时候送。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0"))
	float EnemyThreatLoadedMul = 1.f;

	/** 对手刚交完技能的那个窗口里的额外倍率。>1 = 就等这一下。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability", meta = (ClampMin = "0"))
	float EnemySpentMul = 1.f;

	/** 命中率学习怎么判它。见 EArenaHitRatePolicy。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Arena|AI|Ability")
	EArenaHitRatePolicy HitRatePolicy = EArenaHitRatePolicy::FromRole;
};

/**
 * 一个可以被选中的技能候选。执行层准备好、交给战术层评分。
 *
 * 【为什么不让战术层自己去 ASC 上查】那样战术层就要认识 UAbilitySystemComponent
 * 和槽位授权机制，而它该只认识"有个技能，现在能不能用，是什么距离"。
 * 这层隔离也是蓝图覆写 PlanTactics 时能拿到干净输入的前提。
 */
USTRUCT(BlueprintType)
struct FArenaAbilityCandidate
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	FGameplayTag SlotTag;

	/** 槽位已授权 且 不在冷却。false 的候选战术层不会选它。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bUsable = false;

	/** 「按住选目标」那一类，执行层需要走另一条入口。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bManualTarget = false;
};

/**
 * 对手此刻"正在做什么" —— 观测层每帧读出来，战术层据此确反 / 避险。
 *
 * ===========================================================================
 * 【它和 bEnemyOnCooldown 的分工】
 * 那个说的是"他手里还有没有牌"（资源），这个说的是"他现在这个人是什么状态"（动作）。
 * 一个晕在原地的对手和一个举着盾的对手，手里的牌可能一样多，
 * 但该做的事完全相反 —— 前者白打，后者白打【你】。只看资源是分不出这两者的。
 * ===========================================================================
 *
 * 【为什么只有三个字段，而不是把能读的状态全读上】
 * 因为只放【复制得到】的状态。本项目有一批纯本地 loose 标签
 * （State.Dodge.Active、State.DeathHarvest.Casting / Selecting —— 见 LOLGameplayTags.h
 * 里它们的注释），它们只挂在按键那一端的 ASC 上，服务端的 Bot 根本看不见。
 * 把一个恒为 false 的字段读进来比不读更糟：调试时会以为"读到了但没满足条件"，
 * 于是往战术层找问题，而真正的原因是它压根读不到。
 * 下面三个都由真正的 GameplayEffect 授予，所以跨端可读。
 *
 * 【读不到时给什么】全 false。于是战术层那几项全是中性，行为退化成今天的样子 ——
 * 观测失败的最坏后果是"没变聪明"，而不是"变傻了"。
 */
USTRUCT(BlueprintType)
struct FArenaEnemyActionState
{
	GENERATED_BODY()

	/** 硬控中（眩晕 / 击飞 / 击退）。他动不了，这是白送的输出窗口。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bHardControlled = false;

	/**
	 * 格挡窗口开着，或者格挡成功后的免疫中。这时候打上去的伤害会被完全吃掉
	 * （UBlockComponent 的 BlockedDamageMultiplier 默认 0）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bDamageProof = false;

	/**
	 * 他手里攒着一记大的（隐身中 / 强化普攻在手）。
	 *
	 * 【为什么这两个合成一个字段】破隐和三连击完美窗口都会挂上 State.EmpoweredAttack
	 * （见 LOLGameplayTags.h 里它的注释），所以"隐身中"和"强化普攻在手"是同一件事的
	 * 两个阶段，对 Bot 的结论也一样：接下来那一下很疼，别站在他脸上换血。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bThreatLoaded = false;

	/** 三个都没读到。战术层用它跳过整段判断（也省掉几次无意义的乘法）。 */
	bool IsAnyActive() const { return bHardControlled || bDamageProof || bThreatLoaded; }
};

/**
 * 战术层看到的"此刻的战况"。
 *
 * 【这里的每一项都是执行层已经算好的，战术层不做任何查询】它是纯函数式的：
 * 同样的 context 一定给出同样的 decision。这条性质换来的是可离线回放 ——
 * 把一局里每帧的 context 录下来，就能在没有引擎的情况下重放战术层的决策，
 * 调参和写测试都靠它。
 */
USTRUCT(BlueprintType)
struct FArenaTacticalContext
{
	GENERATED_BODY()

	/** 和对手的水平距离（厘米）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float Distance = 0.f;

	/** 普攻射程（厘米）。接近/远程类技能的评分要用它做参照。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float AttackRange = 220.f;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float SelfHealthRatio = 1.f;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float EnemyHealthRatio = 1.f;

	/** 自己的能量比例（0~1）。给将来的耗蓝技能用；现在只影响不了评分。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float SelfEnergyRatio = 1.f;

	/**
	 * 对手此刻正处在技能冷却里 —— 也就是"他刚交完东西"。
	 *
	 * 【这是战术层最有价值的一个输入】它把"他手里有没有牌"从统计（画像里的
	 * 平均占用率）变成了事实（此刻）。所有进攻型角色都会因为这一条而加分：
	 * 玩家交完技能的半秒是他最虚的时候，而这一点是原版 Bot 完全看不到的。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bEnemyOnCooldown = false;

	/** 对手手里有多少牌（0 = 全在冷却，1 = 全是好的）。由画像的冷却占用率反推。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float EnemySkillReadiness = 1.f;

	/**
	 * 对手此刻在做什么（动作层，而不是资源层）。
	 *
	 * 【为什么它和上面两条是不同的东西】bEnemyOnCooldown 回答"他能不能还手"，
	 * 这一条回答"他现在这个姿势挨不挨得住打"。两个都要有，评分才算看到了对手。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	FArenaEnemyActionState EnemyAction;

	/**
	 * 对手已经连续多少秒没动、也没交技能（秒）。
	 *
	 * 【它和上面那两条的分别】bEnemyOnCooldown 和 EnemyAction 读的都是
	 * "他做了什么"——前提是他【做了】。三条分支（他交完技能 / 他晕着 / 他没牌）
	 * 全部要求对手先给出一个动作。而 1V1 里最常见的僵局恰恰是他什么都不给：
	 * 站在原地看着你。这时候三个字段全是 false，战术层一条理由都找不到，
	 * 于是 Bot 停在基线距离上绕圈 —— 表现就是"他不来我就不动"。
	 *
	 * 这一条填的就是那个空格：它是唯一一个"对手什么都没做"也能读出来的信号。
	 *
	 * 【为什么它在 context 里而不是战术层自己记】和 CommittedSlot 同理 ——
	 * 战术层是纯函数，不能持有跨拍的计时状态。累计由观测层每帧做（它是唯一
	 * 逐帧看得见对手运动的地方），这里只是把结果递进去。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float EnemyIdleSeconds = 0.f;

	/** 战略层的压力值（0~1）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float Pressure = 0.f;

	/** 战略层这一回合的立场。战术层可以按它偏移评分（比如 Chase 下位移技更值）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	EArenaAIStance Stance = EArenaAIStance::Balanced;

	/**
	 * 上一拍选中的槽位（执行层的"承诺"）。空 = 上一拍没打算放技能。
	 *
	 * 【为什么它在 context 里，而不是战术层的成员】战术层必须是纯函数。
	 * 承诺是"上一拍决定了什么"这件事实，只有执行层知道；把它作为输入喂进来，
	 * 战术层就仍然是"同样的输入给同样的输出"，离线回放和单测都不受影响。
	 *
	 * 【它治的是什么】战术层每 0.25 秒重算一次 argmax，而真正按下去受
	 * SkillInterval 约束、可能在一秒半之后。这段窗口里两个分数接近的技能会
	 * 互相翻盘，于是"最后按下去的"和"决策理由里写的"是两码事 ——
	 * 表现就是乱按。给了承诺加分之后，挑战者必须明显更好才抢得走。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	FGameplayTag CommittedSlot;

	/** 世界时间（秒）。用于反连放。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float Now = 0.f;
};

/**
 * 战术层的输出：这一下该做什么。
 */
USTRUCT(BlueprintType)
struct FArenaTacticalDecision
{
	GENERATED_BODY()

	/** 是否要交技能。false 时只有普攻在跑（普攻有自己的节奏，不由战术层管）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bCastSkill = false;

	/** 选中的槽位（bCastSkill 为 true 时有效）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	FGameplayTag SlotTag;

	/** 选中项的得分。调试用 —— 判断"是不是所有技能分数都贴地板"看它。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float Score = 0.f;

	/**
	 * 对期望站位的临时修正（厘米，正 = 想站远一点）。
	 *
	 * 【为什么要让战术层动这个】有些"该退"是当下的：他刚交完控制、我手里没防御技、
	 * 这一下吃下去就是一套。这种判断的寿命是一个决策周期，不该写进战略层
	 * （那一层一秒才刷一次，而且它管的是"这一回合"，不是"这一秒"）。
	 *
	 * 执行层负责夹范围 —— 战术层不知道基线距离是多少，和战略层同理。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	float PreferredRangeDelta = 0.f;

	/**
	 * 人类可读的决策理由（"爆发：他残血" / "保留：都没就绪"）。
	 * 纯调试用，不影响逻辑。战术层"看起来蠢"的时候先看这个。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI|Tactics")
	FString Reason;
};
