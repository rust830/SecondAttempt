// 斗魂竞技场：AI 战略层（启发式默认实现）的实现。

#include "GAS/ArenaAIDirector.h"

FArenaStrategicIntent UArenaAIDirector::BuildIntent_Implementation(
	const FArenaPlayerProfile& Profile, float SelfHealthRatio, float EnemyHealthRatio)
{
	// 默认值 = Balanced + 零偏移 + 倍率 1。也就是"原版 Bot 的行为"。
	// 下面每一条规则都只在这个基础上加东西 —— 任何一条不成立时，
	// 它的贡献就是 0，而不是需要被"撤销"。
	FArenaStrategicIntent Intent;

	if (!bEnableAdaptation)
	{
		return Intent;
	}

	const float SafeSelfHealth = FMath::Clamp(SelfHealthRatio, 0.f, 1.f);
	const float SafeEnemyHealth = FMath::Clamp(EnemyHealthRatio, 0.f, 1.f);

	// -----------------------------------------------------------------------
	// 第一段：战况驱动 —— 【不需要样本，永远生效】
	//
	// 这一段和"了解这个玩家"无关：血量是自己和对手当下的状态，不是学出来的。
	// 所以它不经过 HasEnoughSamples 那道门 —— 第一次见面的第一回合，
	// Bot 也该在残血的时候反应快一点。
	// -----------------------------------------------------------------------

	// 压力 = 我有多吃紧。三项：自己血量越低越高、对面血越多越高、历史胜率越低越高。
	Intent.Pressure = FMath::Clamp(
		(1.f - SafeSelfHealth) * 0.5f + SafeEnemyHealth * 0.3f + Profile.GetPlayerWinRate() * 0.2f,
		0.f, 1.f);

	// 被压着打 → 决策更频繁（反应快）、出手更急。
	// 上界刻意留有余地：不做"残血就变成另一个 AI"那种突变，只是节奏紧一点。
	Intent.DecisionIntervalScale = FMath::Lerp(1.f, 0.55f, Intent.Pressure);
	const float PressureTempo = FMath::Lerp(1.f, 0.75f, Intent.Pressure);
	Intent.AttackIntervalScale *= PressureTempo;
	Intent.SkillIntervalScale *= PressureTempo;

	// -----------------------------------------------------------------------
	// 第二段：局间备忘录 —— 【不需要样本，因为它不是统计出来的】
	//
	// 【为什么它排在统计前面】备忘录是"上一次局间做过的判断"（可能是 LLM 给的）。
	// 它不依赖本局的观测，所以那道 HasEnoughSamples 的门对它没有意义 ——
	// 而那正是它存在的理由：让 Bot 在【还没有任何统计】的第一回合就有备而来。
	//
	// 【为什么它只加偏移、不改写】它和统计是叠加的，不是覆盖的。理由和"策略只加
	// 偏移不动基线"完全一样：万一备忘录是错的（模型幻觉、画像过期），
	// 后果是"偏了一点"，而不是"变成了另一个 Bot"。
	// -----------------------------------------------------------------------
	const float MemoConfidence = (Profile.Memo.bHasContent && Profile.Memo.Confidence >= MinMemoConfidence)
		? FMath::Clamp(Profile.Memo.Confidence, 0.f, 1.f)
		: 0.f;

	if (MemoConfidence > 0.f)
	{
		Intent.RangeOffset += Profile.Memo.RangeBias * MaxRangeOffset * MemoConfidence;

		// AggressionBias 是 [-1,1] 的倾向（+1 = 凶），要换算成倍率。
		// 换算的强度由 MemoAggressionScale 单独控制 —— 见那个属性的注释。
		const float AggressionScale = 1.f
			- MemoAggressionScale * FMath::Clamp(Profile.Memo.AggressionBias, -1.f, 1.f) * MemoConfidence;

		Intent.AttackIntervalScale   *= AggressionScale;
		Intent.SkillIntervalScale    *= AggressionScale;
		Intent.DecisionIntervalScale *= AggressionScale;
	}

	// -----------------------------------------------------------------------
	// 第三段：立场 —— 统计优先，备忘录补位
	//
	// 【为什么统计优先于备忘录】统计是实测（"他刚才真的这么打了"），
	// 备忘录是推断（"他大概会这么打"）。有实测就用实测。
	// 而两者都没有时是 Balanced —— 那等于"按基线性格打"，是个安全的默认。
	// -----------------------------------------------------------------------
	const bool bHasSamples = Profile.HasEnoughSamples();

	if (bHasSamples)
	{
		const bool bLikesClose  = Profile.CloseRangeRatio >= CloseRatioThreshold;
		const bool bLikesFar    = Profile.FarRangeRatio   >= FarRatioThreshold;
		const bool bAggressive  = Profile.ApproachBias    >= ApproachBiasThreshold;
		const bool bPassive     = Profile.ApproachBias    <= -ApproachBiasThreshold;

		// 立场判定。【顺序即优先级，不是随意的】：
		//   Far 先于 Close —— 两个比率同时超阈值只在"他既贴脸又拉开、中间距离几乎没待过"
		//   这种极端情况下才可能（两个比率之和 ≤ 1，而阈值之和 = 1）。真出现时，
		//   "他在放风筝"是更该被处理的那个 —— 追不上比被贴脸更让 Bot 显得无能。
		//   Close 先于 ApproachBias —— 距离是已经发生的事实，接近倾向是意图；
		//   事实比意图稳（意图会被一次换位打乱，距离比率不会）。
		if (bLikesFar)
		{
			Intent.Stance = EArenaAIStance::Chase;
		}
		else if (bLikesClose)
		{
			Intent.Stance = EArenaAIStance::Poke;
		}
		else if (bAggressive)
		{
			Intent.Stance = EArenaAIStance::Counter;
		}
		else if (bPassive)
		{
			Intent.Stance = EArenaAIStance::Brawl;
		}
		else
		{
			Intent.Stance = EArenaAIStance::Balanced;
		}
	}
	else if (MemoConfidence > 0.f && Profile.Memo.PreferredStance != EArenaAIStance::Balanced)
	{
		Intent.Stance = Profile.Memo.PreferredStance;
	}

	// -----------------------------------------------------------------------
	// 立场 → 偏移
	//
	// 【每个立场只动它该动的那个数】比如 Poke 不去改攻击间隔 ——
	// "拉开距离"和"打得勤不勤"是两件事，一起动的话调不出手感：
	// 觉得拉扯得不够远时，你会不知道该调哪一个。
	// -----------------------------------------------------------------------
	switch (Intent.Stance)
	{
	case EArenaAIStance::Chase:
		// 他放风筝 → 压上去，而且别让他有喘息：容错收窄 = 持续贴住。
		Intent.RangeOffset        = -MaxRangeOffset;
		Intent.ToleranceOffset    = -MaxToleranceOffset * 0.5f;
		Intent.AttackIntervalScale *= 0.85f;
		Intent.SkillIntervalScale  *= 0.90f;
		break;

	case EArenaAIStance::Poke:
		// 他爱贴脸 → 把交战距离拉开，容错放宽（拉开之后不用频繁前后微调）。
		Intent.RangeOffset         = +MaxRangeOffset;
		Intent.ToleranceOffset     = +MaxToleranceOffset;
		Intent.StrafeWeightOffset  = +MaxStrafeWeightOffset * 0.5f;
		Intent.AttackIntervalScale *= 1.10f;
		break;

	case EArenaAIStance::Counter:
		// 他敢冲 → 站住别乱晃（容错放宽），绕着他等收招，出手更晚。
		Intent.ToleranceOffset     = +MaxToleranceOffset * 0.5f;
		Intent.StrafeWeightOffset  = +MaxStrafeWeightOffset;
		Intent.AttackIntervalScale *= 1.15f;
		break;

	case EArenaAIStance::Brawl:
		// 他一直在退 → 贴住不放（容错收窄），打得勤一点。
		Intent.RangeOffset         = -MaxRangeOffset * 0.5f;
		Intent.ToleranceOffset     = -MaxToleranceOffset * 0.5f;
		Intent.AttackIntervalScale *= 0.90f;
		Intent.StrafeWeightOffset  = -MaxStrafeWeightOffset * 0.4f;
		break;

	case EArenaAIStance::Balanced:
	default:
		break;
	}

	// -----------------------------------------------------------------------
	// 技能使用习惯 —— 叠在立场之上
	//
	// 这一条和立场是正交的：任何立场下，"他手里有没有牌"都该影响 Bot 敢不敢压。
	//
	// 【为什么它仍然要求样本】它读的两个数（SkillCooldownOccupancy、
	// 以及上面那个 Threshold）都是统计量。备忘录有意不提供这一项 ——
	// "他留不留技能"是一个需要看的习惯，不是能推断出来的。
	// -----------------------------------------------------------------------
	if (bHasSamples)
	{
		if (Profile.SkillCooldownOccupancy <= SkillHoldThreshold)
		{
			// 他手里一直捏着技能 → 进去要吃一套，多绕、晚出手。
			Intent.StrafeWeightOffset  += MaxStrafeWeightOffset * 0.4f;
			Intent.AttackIntervalScale *= 1.10f;
		}
		else if (Profile.SkillCooldownOccupancy >= SkillSpamThreshold)
		{
			// 他好了就放、没有后手 → 压上去，他的技能全在转。
			Intent.AttackIntervalScale *= 0.90f;
			Intent.SkillIntervalScale  *= 0.90f;
		}
	}

	// -----------------------------------------------------------------------
	// 收口：把每一项夹进有界范围
	//
	// 【为什么夹在最后而不是每次加法之后】上面几条规则是叠乘/叠加的，
	// 中间过程允许越界（那样表达起来简单），只在出口收一次口。
	// 这样"某条规则调猛了"的最坏后果是"偏移顶到上限"，而不是"Bot 变得不可理喻"。
	//
	// ⚠️ 注意 ToleranceOffset 夹的是【偏移本身】。基线的 RangeTolerance 只有 30，
	// 一个 -40 的偏移会让最终容错变成负数 —— 那个必须由执行层夹
	// （见 AArenaBotController::ResolveTuning），因为只有它知道基线是多少。
	// -----------------------------------------------------------------------
	Intent.RangeOffset         = FMath::Clamp(Intent.RangeOffset, -MaxRangeOffset, MaxRangeOffset);
	Intent.ToleranceOffset     = FMath::Clamp(Intent.ToleranceOffset, -MaxToleranceOffset, MaxToleranceOffset);
	Intent.StrafeWeightOffset  = FMath::Clamp(Intent.StrafeWeightOffset, -MaxStrafeWeightOffset, MaxStrafeWeightOffset);
	Intent.AttackIntervalScale = FMath::Clamp(Intent.AttackIntervalScale, MinIntervalScale, MaxIntervalScale);
	Intent.SkillIntervalScale  = FMath::Clamp(Intent.SkillIntervalScale, MinIntervalScale, MaxIntervalScale);
	Intent.DecisionIntervalScale = FMath::Clamp(Intent.DecisionIntervalScale, MinIntervalScale, MaxIntervalScale);

	return Intent;
}

// ---------------------------------------------------------------------------
// 局间：写备忘录
// ---------------------------------------------------------------------------

FArenaStrategicMemo UArenaAIDirector::ComposeMemo_Implementation(const FArenaPlayerProfile& Profile)
{
	// 默认值 = bHasContent = false = "没有可说的"。下面每一条都只在这个基础上加。
	FArenaStrategicMemo Memo;

	// -----------------------------------------------------------------------
	// 【为什么这里也有那道样本门，而 BuildIntent 里没有】
	// 因为产物不同：BuildIntent 的输出活一秒钟，错了下一秒就重算了；
	// 备忘录的输出【会存盘、会活到下一局、而且下一局会用】—— 它把一个判断
	// 固化了。用一局的噪声去固化一个跨局的判断，代价比"这一秒偏了一下"大得多。
	// 没有判断，好过有一个错的判断。
	// -----------------------------------------------------------------------
	if (!Profile.HasEnoughSamples())
	{
		return Memo;
	}

	const float WinRate = Profile.GetPlayerWinRate();

	// --- 距离倾向：他爱贴脸就拉开，他爱放风筝就压上去 ---
	// 两个比率的差本身就是 [-1,1] 的（占比之和 ≤ 1），不用再归一化。
	Memo.RangeBias = FMath::Clamp(Profile.CloseRangeRatio - Profile.FarRangeRatio, -1.f, 1.f);

	// --- 节奏倾向：他一直在退 → 凶一点；他一直在赢 → 稳一点 ---
	//
	// 两项的权重 0.6 / 0.4 是"打法"和"实力"的配比。让实力占少数是有意的：
	// 玩家赢得多这件事有很多解释（他强、Bot 这一局手气差、他换了装备），
	// 而"他一直在退"只有一个解释。用一个多义的信号去大改节奏是危险的。
	Memo.AggressionBias = FMath::Clamp(
		0.6f * Profile.ApproachBias + 0.4f * (0.5f - WinRate) * 2.f,
		-1.f, 1.f);

	// --- 建议立场：和学习时同一套判据，保证"学到的"和"记住的"是一回事 ---
	if (Profile.FarRangeRatio >= FarRatioThreshold)
	{
		Memo.PreferredStance = EArenaAIStance::Chase;
	}
	else if (Profile.CloseRangeRatio >= CloseRatioThreshold)
	{
		Memo.PreferredStance = EArenaAIStance::Poke;
	}
	else if (Profile.ApproachBias >= ApproachBiasThreshold)
	{
		Memo.PreferredStance = EArenaAIStance::Counter;
	}
	else if (Profile.ApproachBias <= -ApproachBiasThreshold)
	{
		Memo.PreferredStance = EArenaAIStance::Brawl;
	}
	else
	{
		Memo.PreferredStance = EArenaAIStance::Balanced;
	}

	// --- 可信度：样本越多越自信，但上限 0.8 ---
	//
	// 【为什么上不到 1】满信心意味着"统计就是真理"。而这套观测是有偏采样的：
	// 只观测了一个人、一种打法、几局，还全都发生在同一张地图上。
	// 留 20% 的余地，效果是"即使这份判断全错，也只造成明显但不致命的偏置"。
	//
	// 下限 0.25 则保证"刚够样本线"的第一次判断就有实际影响 ——
	// 否则第一份备忘录会因为可信度太低而被 MinMemoConfidence 过滤掉，
	// 表现是"打了好几局 Bot 才开始有反应"，而那是这道门本身造成的。
	const float SampleConfidence = FMath::Clamp(static_cast<float>(Profile.RoundsObserved) / 30.f, 0.f, 1.f);
	Memo.Confidence = FMath::Lerp(0.25f, 0.8f, SampleConfidence);

	Memo.bHasContent = true;

	// 纯调试/人读用。接 LLM 时这里就是模型的原话。
	Memo.Note = FString::Printf(
		TEXT("样本%d回合 胜率%.0f%% | 贴脸%.2f 拉开%.2f 接近%+.2f 冷却占用%.2f")
		TEXT(" → 立场=%d 距离偏置%+.2f 节奏%+.2f 信心%.2f"),
		Profile.RoundsObserved, WinRate * 100.f,
		Profile.CloseRangeRatio, Profile.FarRangeRatio, Profile.ApproachBias,
		Profile.SkillCooldownOccupancy,
		static_cast<int32>(Memo.PreferredStance), Memo.RangeBias, Memo.AggressionBias, Memo.Confidence);

	return Memo;
}
