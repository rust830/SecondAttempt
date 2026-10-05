// 斗魂竞技场：AI 的【战略层】—— 决定"这一回合想怎么打"。
//
// ===========================================================================
// 【它在整个系统里的位置】
//
//   观测层   UArenaBehaviorObserver   → FArenaPlayerProfile   （玩家是什么样的人）
//   战略层   UArenaAIDirector         → FArenaStrategicIntent （所以我想怎么打）
//   执行层   AArenaBotController      → FArenaBotTuning → 移动/攻击/技能
//
// 战略层是唯一"有观点"的一层。观测层只记数，执行层只执行。
//
// ===========================================================================
// 【为什么是可插拔的（子类覆写 BuildIntent），而不是写死的规则】
// 这一层正是将来要接 LLM / 学习模型的地方 —— 需求里"根据玩家行为进化"最终
// 就是这一层的输入输出换一种算法。把它做成 BlueprintNativeEvent 的代价是
// 一次虚函数调用，换来的是：
//   - 现在的启发式规则可以作为"默认策略"单独存在、单独调；
//   - 以后要上 LLM，新建一个子类覆写 BuildIntent 就行，观测层和执行层一行不动；
//   - 蓝图里也能做实验（一个 BP 子类挂上去就能试）。
//
// 【LLM 的落点在哪 —— 现在先别做，但要知道接口留得对不对】
// LLM 不适合放进每帧/每回合的同步决策里：延迟、成本、不可复现、没法离线调参。
// 它适合的位置是【局间】：一局结束后把画像喂给它，产出一份"针对这个玩家的
// 战略备忘录"（比如"他会一直后撤放风筝，第 1 回合就该压"），
// 存进存档，下一局由本类的 C++ 实现读备忘录来偏置。也就是说 LLM 的产物应当是
// 输入给 BuildIntent 的一份额外数据，而不是替代 BuildIntent 本身。
// 真做的时候再加一个 FArenaStrategicMemo 字段进画像即可，接口不用改。
// ===========================================================================
//
// 【所有输出都是有界的偏移，不是绝对数值】
// 理由见 ArenaAITypes.h 的 FArenaStrategicIntent。简单说：战略层失效时
// 最差要退化成原版 Bot，而不是变成一坨乱动的东西。所以这里的每个数
// 都先算出来、再夹进 UPROPERTY 配的上限里。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GAS/ArenaAITypes.h"
#include "ArenaAIDirector.generated.h"

// EditInlineNew 是给 AArenaBotController 上那个 Instanced 属性用的 ——
// 没有它，Details 面板里就没法"就地新建一个子类实例填进去"。
UCLASS(BlueprintType, Blueprintable, EditInlineNew)
class LOL_API UArenaAIDirector : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * 主入口：画像 + 当前战况 → 这一回合的打法。
	 *
	 * 蓝图子类覆写它就能换一套策略（LLM / 手写规则 / 调参实验）。
	 * C++ 里调它走 ProcessEvent，所以 BP 覆写是真的生效的。
	 *
	 * @param Profile          观测层累积的玩家画像（跨局）
	 * @param SelfHealthRatio  Bot 自己当前血量比例（0~1）
	 * @param EnemyHealthRatio 玩家当前血量比例（0~1）
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Arena|AI|Strategy")
	FArenaStrategicIntent BuildIntent(const FArenaPlayerProfile& Profile, float SelfHealthRatio, float EnemyHealthRatio);

	/**
	 * 【局间】写一份针对这个玩家的战略备忘录。整局结束时调一次。
	 *
	 * ===========================================================================
	 * 【这里就是接 LLM 的地方】
	 *
	 * 把画像喂给模型、让它说一段"这个人是什么打法、下一局该怎么打"，
	 * 把结论填进返回值的 Note / RangeBias / AggressionBias / PreferredStance /
	 * Confidence，本类的 C++ 实现下一局就会读它 —— 而观测层和执行层一行都不用改。
	 *
	 * 为什么 LLM 落在【这里】而不是 BuildIntent 里（延迟/成本/可复现/离线可测
	 * 四个理由的完整版见 FArenaStrategicMemo 的注释）：一句话，这里不在热路径上。
	 * 一局只调一次、下一局才用、结果落盘，所以慢一点、贵一点、偶尔抽风都能接受。
	 *
	 * 【默认实现是本地启发式，不是空壳】它按画像里已有的统计给一组偏置 ——
	 * 于是这条链在没有 LLM 的时候也是活的、能调的、能在调试 HUD 上看到的。
	 * 接 LLM 时你是在替换一个能跑的实现，而不是在往一个 TODO 里填东西。
	 *
	 * 【覆写时的约定】
	 *   - 拿不准就返回 bHasContent = false（等于"没有判断"），不要返回一个
	 *     低可信度的瞎猜 —— 两者在效果上一样，但前者更诚实、更好排查；
	 *   - Confidence 是你对自己判断的信心，不是"这个判断有多强烈"。强烈的判断
	 *     配低信心（"我猜他很强"）是完全合理的组合，效果是轻微偏置。
	 * ===========================================================================
	 *
	 * @param Profile  这一局打完之后的画像（已含本局的观测）
	 * @return         备忘录。bHasContent = false 表示"没有可说的"。
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Arena|AI|Strategy")
	FArenaStrategicMemo ComposeMemo(const FArenaPlayerProfile& Profile);

	/** 总开关。关掉 = 永远输出 Balanced + 零偏移（等于原版 Bot）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy")
	bool bEnableAdaptation = true;

	// =====================================================================
	// 适应强度 —— 这几个数决定"Bot 有多针对你"
	//
	// 【这是整个系统里最需要手感调校的一组数】调大 → Bot 明显在变招、
	// 但容易让玩家觉得"它在作弊"；调小 → 玩家根本感觉不到它在学。
	// 默认值取得比较保守：能感觉到倾向，但不会让一个固定打法的玩家必输。
	//
	// ⚠️ 别把这些调成"能完美克制"—— 会完美针对你的 AI 是差评，不是卖点。
	// =====================================================================

	/** 单次策略最多能把交战距离推多远（厘米）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Limits", meta = (ClampMin = "0", Units = "cm"))
	float MaxRangeOffset = 60.f;

	/** 单次策略最多能改多少距离容错（厘米）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Limits", meta = (ClampMin = "0", Units = "cm"))
	float MaxToleranceOffset = 40.f;

	/** 单次策略最多能改多少绕圈占比。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Limits", meta = (ClampMin = "0", ClampMax = "1"))
	float MaxStrafeWeightOffset = 0.25f;

	/** 间隔倍率的上下限。下限 0.6 = 最快能比基线快 40%，上限 1.3 = 最慢慢 30%。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Limits", meta = (ClampMin = "0.1", ClampMax = "1"))
	float MinIntervalScale = 0.6f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Limits", meta = (ClampMin = "1", ClampMax = "3"))
	float MaxIntervalScale = 1.3f;

	// =====================================================================
	// 判据阈值 —— "多大算大"
	//
	// 【为什么这几个是 UPROPERTY 而不是常量】它们标定的是"玩家的行为分布"，
	// 而这取决于游戏本身（近战臂展多长、移速多快）。换一套数值/换一张地图
	// 就该重新标一次，不该重编译。默认值是照当前项目标定的，见各自的注释。
	// =====================================================================

	/** 贴脸时长占比超过这个值 → 判定"这个玩家爱贴脸"。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Thresholds", meta = (ClampMin = "0", ClampMax = "1"))
	float CloseRatioThreshold = 0.55f;

	/** 拉开时长占比超过这个值 → 判定"这个玩家爱放风筝"。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Thresholds", meta = (ClampMin = "0", ClampMax = "1"))
	float FarRatioThreshold = 0.45f;

	/** 接近倾向超过这个值 → 判定"这个玩家很敢冲"。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Thresholds", meta = (ClampMin = "0", ClampMax = "1"))
	float ApproachBiasThreshold = 0.3f;

	/**
	 * 技能冷却占用率低于这个值 → 判定"这个玩家手里一直捏着技能"。
	 * 0.25 的意思是"四分之三的时间他一个技能都没在转"。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Thresholds", meta = (ClampMin = "0", ClampMax = "1"))
	float SkillHoldThreshold = 0.25f;

	/** 技能冷却占用率高于这个值 → 判定"这个玩家好了就放、没有后手"。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Thresholds", meta = (ClampMin = "0", ClampMax = "1"))
	float SkillSpamThreshold = 0.75f;

	// =====================================================================
	// 备忘录
	// =====================================================================

	/**
	 * 备忘录的可信度低于这个值就当没有。
	 *
	 * 【为什么需要一个下限】Confidence 是乘数，0.01 的可信度在数学上"有影响"，
	 * 在表现上完全看不出来 —— 留一条这种记录只会让调试 HUD 上多一行噪声，
	 * 还会让人以为"备忘录生效了"。低于这个值直接当作没有内容。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Memo", meta = (ClampMin = "0", ClampMax = "1"))
	float MinMemoConfidence = 0.1f;

	/**
	 * 备忘录的节奏偏置最多能把间隔缩到/放到多少。
	 * 0.25 = AggressionBias 满格时，间隔变成原来的 0.75 倍（或 1.25 倍）。
	 *
	 * 【为什么要单独一个数而不是直接乘 AggressionBias】备忘录给的是 [-1,1] 的
	 * 倾向，不是倍率。中间必须有一次换算，而那个换算的强度（"满格的凶 = 快多少"）
	 * 是要调的 —— 把它摊进 AggressionBias 里，调强度就得去改模型输出的语义。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|AI|Strategy|Memo", meta = (ClampMin = "0", ClampMax = "0.75"))
	float MemoAggressionScale = 0.25f;
};
