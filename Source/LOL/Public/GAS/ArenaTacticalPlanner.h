// 斗魂竞技场：AI 的【战术层】—— 决定"此刻该按哪个键"。
//
// ===========================================================================
// 【它在整个系统里的位置】
//
//   观测层  UArenaBehaviorObserver  每帧看玩家    → FArenaPlayerProfile
//   战略层  UArenaAIDirector        这一回合怎么打 → FArenaStrategicIntent
//   战术层  本类                     这一下做什么   → FArenaTacticalDecision
//   执行层  AArenaBotController     合成数值并按键
//
// 【和战略层的分工，一句话】战略层管"这一回合"，本层管"这一下"。
// 战略层一秒才刷一次、跨回合有记忆（画像）；本层每个决策周期重算一次、只看当下。
// 所以"他刚交完技能，现在压上去"这种判断必须在本层 —— 它的寿命是半秒，
// 写进战略层的话要么被一秒的刷新抹掉，要么把战略层变成一个高频的东西。
//
// ===========================================================================
// 【它替掉的是什么：原来那个"轮着放"】
// 原实现（AArenaBotController::PressNextSkill）是按下标轮询：从上次的下一个
// 开始扫，扫到一个能放的就用。它有三个明确的坏处：
//   ① 完全无视战况 —— 对面残血站在斩杀范围内，它可能去放一个无关的位移；
//   ② 完全无视角色 —— 自己满血去开格挡、空血去开增益，只要轮到了就会发生；
//   ③ 顺序是固定的、可被玩家学会的 —— 打两局就能预判它下一手是什么。
//
// 本类用【效用评分】替掉轮询：给每个可用技能按当前情境打一个分，取最高的。
// 三个坏处各自对应一条评分项（角色情境 / 血量甜蜜区 / 反连放），
// 而且它们是【乘法关系】—— 任何一项不成立，分数就塌下来，不需要一长串 if。
//
// ===========================================================================
// 【它唯一需要人填的东西：AbilityProfiles】
// 战术层不认识任何具体技能，只认识"角色"（EArenaAbilityRole）和"距离带"。
// 所以加一个新技能时，本文件一行都不用改 —— 在 AbilityProfiles 里加一条
// （槽位标签 + 角色），或者干脆不填，它会退化成"中性的伤害技能"参与评分。
//
// 【为什么是可覆写的事件而不是写死的函数】
// 和战略层同理：这里是将来上 GOAP / 行为树 / 学习模型的地方。
// PlanTactics 是纯函数（输入 context + candidates → 输出 decision），
// 所以离线回放录下来的 context 序列就能重放整套决策 —— 调参和写测试都靠这条性质。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/Object.h"
#include "GAS/ArenaAITypes.h"
#include "ArenaTacticalPlanner.generated.h"

UCLASS(BlueprintType, Blueprintable, EditInlineNew)
class LOL_API UArenaTacticalPlanner : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * 主入口：情境 + 候选 → 这一下做什么。纯函数（不改任何状态）。
	 *
	 * 蓝图子类覆写它就能换一套战术（GOAP / 学习模型 / 手搓规则）。
	 * C++ 里调它走 ProcessEvent，所以 BP 覆写是真的生效的。
	 *
	 * ⚠️ 覆写时不要再调 NotifyCast —— 那是执行层的事（见下面）。
	 *
	 * @param Context     此刻的战况（全由执行层算好）
	 * @param Candidates  可候选的槽位（已含"能不能用"，覆写方不用再查 ASC）
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Arena|AI|Tactics")
	FArenaTacticalDecision PlanTactics(const FArenaTacticalContext& Context, const TArray<FArenaAbilityCandidate>& Candidates);

	/**
	 * 执行层真的按下了某个技能时调一次。反连放和族锁都要用。
	 * （族锁需要角色，所以这里会顺手查一次画像 —— 查不到就只记槽位。）
	 *
	 * 【为什么不让 PlanTactics 自己记】它是纯函数 —— 一旦它开始记状态，
	 * "同样的输入给同样的输出"就没了，离线回放和单测全部失效。
	 * 所以"记一笔"这件事必须由执行层显式地做（它本来就知道自己按没按下去）。
	 */
	void NotifyCast(const FGameplayTag& SlotTag, float Now);

	/** 找槽位的战术画像。没配过返回 nullptr（调用方用默认值）。 */
	const FArenaAbilityProfile* FindProfile(const FGameplayTag& SlotTag) const;

	/**
	 * 记一次出手的结果（打中了没有）。执行层在出手后看对手掉没掉血，然后调这里。
	 *
	 * 【为什么这是"自省"，而不是又一条配置】画像里那些 Role / 权重是人【猜】的，
	 * 而这个数是 Bot 自己【试】出来的：某个技能连着几次按下去对手都不掉血，
	 * 它的分数就该自己降下去。这是这套评分唯一一条不依赖人填的输入。
	 *
	 * 【哪些技能会被记】由画像上的 HitRatePolicy 决定，默认"跟随角色"——
	 * 也就是只有本来就该造成伤害的角色（爆发 / 斩杀 / 远程）。位移、格挡、增益
	 * 放出去本来就不掉血，拿掉血去评判它们的结果是把闪现和格挡一路降到地板，
	 * 一个把自己保命技能判成废物的 AI。
	 *
	 * 打断类（上挑 / 回身击退）默认【不】被评判，因为它们首先是控制；但如果你
	 * 希望它们也学（空了就是白交一个控），把那个槽位的 HitRatePolicy 改成 Always。
	 * 没配画像的槽位一律不记（不认识的东西不评价，理由同上）。
	 */
	void NotifyOutcome(const FGameplayTag& SlotTag, bool bHit);

	/** 清掉命中率记录。整局开始时调 —— 单局太短，但跨局记又会把上一局的账带到下一局。 */
	void ResetOutcomeHistory();

	/** 清掉反连放记录。换局/换 Pawn 时调 —— 否则新回合会继承上一局的"刚放过"。 */
	void ResetCastHistory();

	// =====================================================================
	// 配置
	// =====================================================================

	/**
	 * 槽位的战术画像。没配的槽位按"未指定"参与评分（会用它，但不特别想用）。
	 *
	 * 【为什么允许留空】漏配一个技能的后果应该是"这个技能用得少一点"，
	 * 而不是"这个技能永远不会被按"—— 后者在表现上和"技能坏了"一模一样。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics")
	TArray<FArenaAbilityProfile> AbilityProfiles;

	/**
	 * 低于这个分数就干脆不放技能（普攻不受影响，它有自己的节奏）。
	 *
	 * 【为什么需要一个下限，而不是"矮子里拔将军"】候选里全是不合时宜的技能时，
	 * 放出去的那一个会让 Bot 看起来在乱交技能 —— 而在 1V1 里技能就是资源，
	 * 乱交比不放更糟。留一个"这次先不交"的选项，是这套评分和轮询最本质的区别。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "0"))
	float MinScoreToCast = 0.25f;

	/** 关掉 = 退化回"取第一个可用槽位"（排查"是不是评分出的问题"时用）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bEnableUtilityScoring = true;

	/**
	 * 战术层单次最多能把期望站位推多远（厘米）。
	 *
	 * 【注意它和战略层的 MaxRangeOffset 是两个独立的预算】战略层那 60 是针对
	 * "这个玩家"的长期调整，这 100 是"这一秒"的临时反应。两者在执行层相加，
	 * 所以最坏情况下期望距离会比基线远 160 —— 这是有意的：一个是性格、一个是应激。
	 * 觉得 Bot 站位飘的时候，先看是哪一层在推。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "0", Units = "cm"))
	float MaxRangeDelta = 100.f;

	/**
	 * 什么程度的威胁才值得后退。
	 *
	 * 威胁 = (1 - 自己血量) × 0.6 + 对手手里的牌 × 0.4。
	 * 0.6 的意思是"我半血以下、而且他手里全是牌"才触发后退。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "0", ClampMax = "1"))
	float RetreatThreatThreshold = 0.6f;

	/**
	 * 对手静止超过这么多秒，就直接压到最近距离（秒）。见 PlanTactics 里那一条。
	 *
	 * 【为什么要一个下限秒数，而不是"他一停我就上"】人都有停顿 —— 补个刀、
	 * 等一个冷却、手离开键盘半秒。门槛太低的话每一次微小的停顿都会让 Bot
	 * 突然扑上来，那个观感是"抽风"而不是"主动"。
	 *
	 * 【2 秒是怎么来的】它要比人正常操作里的停顿（零点几秒）明显长，
	 * 又要短于"这一回合还打不打了"的耐心上限。2 秒是"他真的没打算过来"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "0", Units = "s"))
	float IdlePressSeconds = 2.f;

	/**
	 * 同一"族"的技能刚放过一个之后，族里其它技能的降分窗口（秒）。
	 *
	 * 【族是什么】自保(防御/躲闪/回复)算一族，爆发(爆发/斩杀)算一族，
	 * 其余角色各自一族。
	 *
	 * 【为什么光有反连放不够】自保这一族在实际配置里有四个技能（闪避、隐身、
	 * 翻滚、格挡），而它们的画像必然高度重合 —— 都是"我吃紧的时候想用"。
	 * 于是低血量时它们【同时】拿到高分。反连放只压"同一个槽位"，
	 * 压不住跨槽位的连喷，表现就是"挨打的时候把四个防御技挨个按一遍"。
	 * 族锁补的就是这一格：族里放过一个，其它几个一起退下去。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "0", Units = "s"))
	float RoleLockout = 3.f;

	/** 族锁生效期间，族内其它技能的分数倍率。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "0", ClampMax = "1"))
	float RoleLockoutPenalty = 0.25f;

	/**
	 * 上一拍选中的技能拿到的分数倍率（承诺黏性，>1）。
	 *
	 * 【怎么读这个数】1.6 的意思是"别的技能要比它好 60% 才抢得走"。
	 * 配 1 = 关掉承诺（回到每拍重新抢的老行为）；配太大 = Bot 会死抱着
	 * 一个已经不合时宜的技能不放。它和反连放/族锁是互补的：
	 * 那两个管"放过之后别连着放"，这个管"还没放之前别改主意"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "1"))
	float CommitBonus = 1.6f;

	/**
	 * 关掉 = 不按命中率降权（排查"是不是它在压着某个技能"时用）。
	 *
	 * 【默认开的理由】它是唯一一条【从实战里学到的】输入 —— 人配的画像说"这个技能
	 * 该在这时候用"，而它说的是"这个技能在这时候【实际】管不管用"。两者冲突时，
	 * 该让步的是猜的那个。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics")
	bool bEnableOutcomeLearning = true;

	/** 命中率每次更新时新样本占的权重（0~1）。和画像的 ProfileBlendAlpha 同理。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "0", ClampMax = "1"))
	float ReliabilityBlend = 0.4f;

	/**
	 * 命中率的下限。
	 *
	 * 【为什么必须有地板】没有它的话，一个手感不好（或者被格挡了几次）的技能会被
	 * 一路压到 0 分并永久雪藏 —— 那和"这个技能坏了"在表现上完全一样，是本项目
	 * 反复踩过的那类坑。地板让"最近不太灵"表现为【用得少】，而不是【永远不用】。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics", meta = (ClampMin = "0", ClampMax = "1"))
	float MinReliability = 0.4f;

	// =====================================================================
	// 评分常量
	//
	// 【为什么这些原本是 .cpp 里的常量，现在搬到这里】它们原先写死在匿名命名空间，
	// 意思是"改一个数就得重编译一次"。而调 AI 手感恰恰是【要连着试很多组数】的活，
	// 每轮都等一次构建等于把调参这件事变得不可做。搬成 UPROPERTY 之后在编辑器里
	// 改完立刻见效（战术层每拍重算，不缓存）。
	//
	// 【调这些数时的顺序】先看 Reason 里那一串标记是哪一项把分数托上去/压下去的，
	// 再来改对应的那一个 —— 盲调九个乘法项会互相盖住，看不出是谁的效果。
	// =====================================================================

	/** 距离掉出有效带之后的分数地板（0~1）。见 FArenaAbilityProfile::MaxRange。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0", ClampMax = "1"))
	float FitFloor = 0.15f;

	/** 血量甜蜜区之外的地板（0~1）。比距离地板高一点：血量不合适没那么致命。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0", ClampMax = "1"))
	float HealthFitFloor = 0.25f;

	/** 反连放的分数倍率。不是 0 —— 极端战况下仍然允许连放同一个技能。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0", ClampMax = "1"))
	float RepeatPenalty = 0.15f;

	/** 他刚交完技能时，进攻型角色的分数加成上限（0.35 = +35%）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0"))
	float OpportunityBonus = 0.35f;

	/** 他被硬控时，进攻型角色的倍率。给得比 OpportunityBonus 猛，因为这是白送的。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0"))
	float PunishBonus = 1.8f;

	/** 他被硬控时，自保型角色的倍率。他动不了 = 我不需要保命。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0", ClampMax = "1"))
	float PunishSelfPreservationMul = 0.5f;

	/** 他格挡 / 免疫中时，伤害技的倍率。不是 0：0 会让"配错了"和"技能坏了"分不清。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0", ClampMax = "1"))
	float WastedOnBlockMul = 0.15f;

	/** 他攒着一记大的（隐身 / 强化普攻）时，自保型角色的倍率。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0"))
	float RespectBonus = 1.5f;

	/** 同上，进攻型角色的倍率 —— 这时候冲上去换血是亏的。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Tactics|Scoring", meta = (ClampMin = "0", ClampMax = "1"))
	float RespectPenaltyMul = 0.7f;

private:
	/** 槽位 → 命中率（0.4~1，没记录过按 1 算）。运行时状态，不进存档（同下面那两个的理由）。 */
	TMap<FGameplayTag, float> SlotReliability;

	/**
	 * 每个槽位上一次被按下的时刻。反连放用。
	 *
	 * 【为什么不是 TArray 也不是 UPROPERTY】它是运行时状态、不进存档、不用给蓝图看；
	 * 而且 TMap 写进 UPROPERTY 在本项目有坑（CDO 上读得到、实例上拿到空表，
	 * 见 ArenaAITypes.h 里 FArenaPlayerProfile 的注释）。不做 UPROPERTY 就没有那个问题。
	 */
	TMap<FGameplayTag, float> LastCastTimes;

	/**
	 * 每个【角色】上一次被按下的时刻。族锁用。
	 *
	 * 【为什么按角色记而不是按槽位】族锁要压的是"族里刚放过一个"，
	 * 而族是角色层面的概念 —— 槽位表答不了"闪避和格挡是不是一族"。
	 * 和 LastCastTimes 一样是裸成员、不进 UPROPERTY（同理由）。
	 */
	TMap<EArenaAbilityRole, float> LastRoleCastTimes;
};
