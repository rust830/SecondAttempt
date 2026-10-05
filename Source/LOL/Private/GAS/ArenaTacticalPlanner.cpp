// 斗魂竞技场：AI 战术层（效用评分的默认实现）的实现。

#include "GAS/ArenaTacticalPlanner.h"

// ===========================================================================
// 评分用的常量
//
// 【为什么全部带 ArenaTactic_ 前缀】本项目是 unity build（多个 .cpp 编进同一个
// 翻译单元），匿名命名空间里的同名符号会 C2084 撞车。见 CONVENTIONS.md。
// ===========================================================================
namespace
{
	// 【这些评分常量去哪了】原先它们写死在这里，现在全部搬到了
	// UArenaTacticalPlanner 的 UPROPERTY 上（见 ArenaTacticalPlanner.h 的
	// "评分常量"一节）—— 理由很简单：调 AI 手感要连着试很多组数，
	// 每轮都等一次 C++ 构建等于把调参变得不可做。
	//
	// 下面两个纯函数因此改成【把地板当参数收进来】：它们是自由函数，
	// 读不到任何 UPROPERTY，只能由调用方（PlanTactics，它是成员函数）传进来。

	/**
	 * 距离贴合：在 [MinRange, MaxRange] 里满分，出去线性掉到地板。
	 *
	 * 掉到地板所需的距离取【带宽的一半】和 100cm 里的大者 —— 带宽为 0 的
	 * 纯近战技（Min == Max）也需要一段过渡带，否则它的分数会在一个点上突跳。
	 *
	 * 【为什么地板由外部传】0 会让"距离不合适"变成"这个技能不存在"，
	 * 是最难查的一类问题 —— 但地板给多少是配置，不是这里该定的。
	 */
	float ArenaTactic_RangeFit(float Distance, float MinRange, float MaxRange, float Floor)
	{
		const float Lo = FMath::Min(MinRange, MaxRange);
		const float Hi = FMath::Max(MinRange, MaxRange);

		if (Distance >= Lo && Distance <= Hi)
		{
			return 1.f;
		}

		const float Falloff = FMath::Max((Hi - Lo) * 0.5f, 100.f);
		const float Outside = (Distance < Lo) ? (Lo - Distance) : (Distance - Hi);

		return FMath::Lerp(1.f, FMath::Clamp(Floor, 0.f, 1.f), FMath::Clamp(Outside / Falloff, 0.f, 1.f));
	}

	/** 血量甜蜜区：离 SweetSpot 越远分越低，到 Band 之外就是地板。地板同样由外部传。 */
	float ArenaTactic_HealthFit(float Value, float SweetSpot, float Band, float Floor)
	{
		const float SafeBand = FMath::Max(Band, KINDA_SMALL_NUMBER);
		const float Fit01 = FMath::Clamp(1.f - FMath::Abs(Value - SweetSpot) / SafeBand, 0.f, 1.f);

		return FMath::Lerp(FMath::Clamp(Floor, 0.f, 1.f), 1.f, Fit01);
	}

	/** 自保型：我吃紧的时候想用的那些。族锁和"对手攒着大招"都用它。 */
	bool ArenaTactic_IsSelfPreservation(EArenaAbilityRole Role)
	{
		return Role == EArenaAbilityRole::Defensive
			|| Role == EArenaAbilityRole::Evade
			|| Role == EArenaAbilityRole::Sustain;
	}

	/** 打一套用的：对面残血时它们一起冲到高分，所以族锁要把它们绑在一起。 */
	bool ArenaTactic_IsDamageSpike(EArenaAbilityRole Role)
	{
		return Role == EArenaAbilityRole::Burst
			|| Role == EArenaAbilityRole::Execute;
	}

	/** 进攻型：对手被硬控时，这些技能才是"该趁现在交"的。比 DamageSpike 多算位移和打断。 */
	bool ArenaTactic_IsOffensive(EArenaAbilityRole Role)
	{
		return Role == EArenaAbilityRole::GapCloser
			|| Role == EArenaAbilityRole::Ranged
			|| Role == EArenaAbilityRole::Burst
			|| Role == EArenaAbilityRole::Execute
			|| Role == EArenaAbilityRole::Disrupt;
	}

	/**
	 * 这个角色放出去【本来就该造成伤害】吗。只有这类技能谈得上"命中率"。
	 *
	 * 【为什么 Unspecified 不算】它是"漏配"的兜底。一个我们根本不知道是干什么的
	 * 技能，拿掉血去评判它，赌的是"它恰好是个伤害技"—— 赌错了就是在惩罚一个
	 * 位移或增益。不认识的东西不评价。
	 */
	bool ArenaTactic_DealsDamage(EArenaAbilityRole Role)
	{
		return Role == EArenaAbilityRole::Burst
			|| Role == EArenaAbilityRole::Execute
			|| Role == EArenaAbilityRole::Ranged;
	}

	/**
	 * 两个角色算不算"同一族"。
	 *
	 * 【为什么需要族，而不是只按角色拉黑】自保这一族在实际配置里有四个技能
	 * （隐身、闪避、翻滚、格挡），它们的画像必然高度重合 —— 都是"我吃紧时想用"。
	 * 于是低血量时它们【同时】拿到高分。按槽位拉黑只压得住"同一个技能别连放"，
	 * 压不住跨槽位的连喷，表现是"挨打时把四个防御技挨个按一遍"。
	 *
	 * 爆发/斩杀合成一族是同一个道理的另一种形态：两者都在对面残血时才值，
	 * 所以对面一残血它们也同时冲到高分。
	 */
	bool ArenaTactic_SameFamily(EArenaAbilityRole A, EArenaAbilityRole B)
	{
		if (A == B)
		{
			return true;
		}

		return (ArenaTactic_IsSelfPreservation(A) && ArenaTactic_IsSelfPreservation(B))
			|| (ArenaTactic_IsDamageSpike(A) && ArenaTactic_IsDamageSpike(B));
	}

	/** 角色的显示名。只用在 Reason 里（调试用），不参与任何计算。 */
	const TCHAR* ArenaTactic_RoleToString(EArenaAbilityRole Role)
	{
		switch (Role)
		{
		case EArenaAbilityRole::GapCloser:  return TEXT("接近");
		case EArenaAbilityRole::Ranged:     return TEXT("远程");
		case EArenaAbilityRole::Burst:      return TEXT("爆发");
		case EArenaAbilityRole::Execute:    return TEXT("斩杀");
		case EArenaAbilityRole::Defensive:  return TEXT("防御");
		case EArenaAbilityRole::Evade:      return TEXT("躲闪");
		case EArenaAbilityRole::Sustain:    return TEXT("回复");
		case EArenaAbilityRole::Buff:       return TEXT("增益");
		case EArenaAbilityRole::Disrupt:    return TEXT("打断");
		case EArenaAbilityRole::Unspecified:
		default:                            return TEXT("未指定");
		}
	}
}

// ---------------------------------------------------------------------------
// 查询 / 记录
// ---------------------------------------------------------------------------

const FArenaAbilityProfile* UArenaTacticalPlanner::FindProfile(const FGameplayTag& SlotTag) const
{
	if (!SlotTag.IsValid())
	{
		return nullptr;
	}

	// 线性查找。槽位最多六七个，而且这个函数每个决策周期才调几次 ——
	// 为它建一张索引表是纯粹的复杂度，换不到任何东西。
	for (const FArenaAbilityProfile& Profile : AbilityProfiles)
	{
		if (Profile.SlotTag == SlotTag)
		{
			return &Profile;
		}
	}

	return nullptr;
}

void UArenaTacticalPlanner::NotifyCast(const FGameplayTag& SlotTag, float Now)
{
	if (!SlotTag.IsValid())
	{
		return;
	}

	LastCastTimes.Add(SlotTag, Now);

	// 族锁要按角色记一笔。没配画像的槽位（返回 nullptr）就没有角色可记 ——
	// 它本身也不参与族锁，跳过是对的。
	if (const FArenaAbilityProfile* Profile = FindProfile(SlotTag))
	{
		LastRoleCastTimes.Add(Profile->Role, Now);
	}
}

void UArenaTacticalPlanner::ResetCastHistory()
{
	LastCastTimes.Reset();
	LastRoleCastTimes.Reset();
}

void UArenaTacticalPlanner::NotifyOutcome(const FGameplayTag& SlotTag, bool bHit)
{
	if (!SlotTag.IsValid())
	{
		return;
	}

	// 只记"本来就该造成伤害"的技能。理由见头文件：位移/防御/增益放出去本来就不掉血，
	// 拿掉血去评判它们 = 把自己的保命技一路降到地板。
	const FArenaAbilityProfile* Profile = FindProfile(SlotTag);
	if (!Profile)
	{
		return;
	}

	// 【必须和评分那一侧用同一条判定】两边各写各的话会出现分叉：评分侧按
	// HitRatePolicy 降权、记录侧按 Role 拒收（或反过来），结果是那一项永远读不到值，
	// 而分别看两处代码都是对的 —— 只能靠对着读才发现。所以判定逻辑只此一份。
	const bool bLearnsHitRate =
		Profile->HitRatePolicy == EArenaHitRatePolicy::Always
		|| (Profile->HitRatePolicy == EArenaHitRatePolicy::FromRole
			&& ArenaTactic_DealsDamage(Profile->Role));

	if (!bLearnsHitRate)
	{
		return;
	}

	const float Blend = FMath::Clamp(ReliabilityBlend, 0.f, 1.f);
	const float Floor = FMath::Clamp(MinReliability, 0.f, 1.f);

	float& Reliability = SlotReliability.FindOrAdd(SlotTag, 1.f);
	Reliability = FMath::Lerp(Reliability, bHit ? 1.f : 0.f, Blend);

	// 夹在地板上：一次没打中不该让一个好技能被雪藏（见 MinReliability 的注释）。
	Reliability = FMath::Clamp(Reliability, Floor, 1.f);
}

void UArenaTacticalPlanner::ResetOutcomeHistory()
{
	SlotReliability.Reset();
}

// ---------------------------------------------------------------------------
// 主入口
// ---------------------------------------------------------------------------

FArenaTacticalDecision UArenaTacticalPlanner::PlanTactics_Implementation(
	const FArenaTacticalContext& Context, const TArray<FArenaAbilityCandidate>& Candidates)
{
	// 默认值 = 不放技能 + 零位移修正。下面每一条都只在这个基础上加东西，
	// 于是"一条规则都不成立"的结果是"这一下什么都不做，保持基线行为"。
	FArenaTacticalDecision Decision;

	const float Distance     = FMath::Max(0.f, Context.Distance);
	const float SelfHealth   = FMath::Clamp(Context.SelfHealthRatio, 0.f, 1.f);
	const float EnemyHealth  = FMath::Clamp(Context.EnemyHealthRatio, 0.f, 1.f);
	const float Readiness    = FMath::Clamp(Context.EnemySkillReadiness, 0.f, 1.f);
	const float AttackRange  = FMath::Max(0.f, Context.AttackRange);

	// =======================================================================
	// 第一段：站位修正 —— 【不管放不放技能都要给】
	//
	// 先算它、并且独立于下面的技能选择，是因为"该退了"这件事和"手里有没有
	// 合适的技能"无关。手里没牌的时候恰恰最该退 —— 那正是这个判断的价值所在。
	//
	// 威胁 = 我有多吃紧 × 他手里有多少牌。注意第二项是【统计出来的平均值】
	// （画像里的冷却占用率），不是此刻 —— 这是有意的：后退是一个持续半秒以上的
	// 动作，用一个持续几秒的趋势量来驱动它比用一个瞬间量稳。
	// 而 bEnemyOnCooldown 那个瞬间量用在下面选技能上：出手是一个瞬间决定。
	// =======================================================================
	const float Threat = FMath::Clamp((1.f - SelfHealth) * 0.6f + Readiness * 0.4f, 0.f, 1.f);

	// 记下来给下面"他不主动"那条用。写成一个变量而不是在那边重抄一遍条件，
	// 是因为两条判断必须永远一致 —— 抄一遍的话改了一处漏一处，表现是
	// "残血的 Bot 会因为他站着不动就冲上去送"，而且不会有任何报错。
	bool bRetreating = false;

	if (!Context.bEnemyOnCooldown && Threat >= RetreatThreatThreshold)
	{
		// 他手里全是牌、我又吃紧 → 拉开。别在他最强的时候站在他脸上。
		Decision.PreferredRangeDelta = MaxRangeDelta;
		bRetreating = true;
	}
	else if (Context.bEnemyOnCooldown && SelfHealth > 0.5f)
	{
		// 他刚交完、我还健康 → 压上去。那是他最虚的半秒。
		// 只推 60% 而不是推满：这是"更想贴"，不是"必须贴"，
		// 推满会让它看起来像一个只会冲的 AI。
		Decision.PreferredRangeDelta = -MaxRangeDelta * 0.6f;
	}

	// 我一张牌都没有、他手里却全是牌 → 退半步等冷却。
	//
	// 【为什么上面那两条盖不住它】Threat 只看"我有多吃紧 × 他有多满"。
	// "我技能全冷却"这件事它看不见 —— 而那恰恰是最该拉开的时候：
	// 此刻贴脸等于用普攻去换他的技能，是纯亏的换血。
	// 只退一半是因为普攻还得打，退满就成了逃跑。
	const bool bNoSkillReady = !Candidates.ContainsByPredicate(
		[](const FArenaAbilityCandidate& Candidate) { return Candidate.bUsable; });

	if (bNoSkillReady && !Context.bEnemyOnCooldown)
	{
		Decision.PreferredRangeDelta = MaxRangeDelta * 0.5f;
	}

	// 他被硬控 → 压上去补刀。
	//
	// 【为什么它能盖住上面两条】上面两条读的都是"他手里还有没有牌"，
	// 而这一条是"他根本动不了"。晕着的对手打不出任何东西，"他手里有牌"在这种
	// 时候是个无关紧要的事实 —— 而且这是 Bot 自己的击飞/眩晕命中之后最该做的一件事。
	//
	// 【和下面"他不主动"是同优先级】两条推的都是满格（-MaxRangeDelta），
	// 谁在后面都一样，所以这里不需要为次序操心。
	if (Context.EnemyAction.bHardControlled)
	{
		Decision.PreferredRangeDelta = -MaxRangeDelta;
	}

	// 他愣着不动 → 我压上去。
	//
	// 【上一次大改漏掉的那一格】上面四条全部要求对手【先给出一个动作】：
	// 他交完技能、他被硬控、他没牌。而 1V1 里最常见、也最容易被玩家利用的
	// 局面恰恰是他什么都不给 —— 站在原地看着你。这时上面四个条件全是 false，
	// 一条都不成立，Bot 就停在基线距离上绕圈，永远不会主动。
	//
	// 【和上面那条"他交完技能"的分工】那条推 60%，因为"他虚了"不等于"我该贴脸"；
	// 这条推满，因为对手不动时贴上去没有任何代价 —— 他既不会还手，也不会拉开。
	// 推满带来的"绕圈半径 → 0"正是这里想要的：绕着不动的人转圈没有意义。
	//
	// 【为什么排在最后、又为什么带 bRetreating】它是这套评分里最强的推进信号，
	// 所以理应盖住上面那些"程度较轻"的（他交完技能的 -60%、没牌时的 +50%）。
	// 但自保必须比它更高 —— 残血时该撤还是要撤，不能因为他站着不动就冲上去送。
	//
	// 【为什么读的是"他多久没动"而不是"我们离得多远"】距离远这件事有好几种解释
	// （他刚把我打退、我刚放完一个位移、他绕着我不肯近身），而"他一动不动"
	// 只有一个解释。用距离做条件的话，Bot 会在自己该退的时候误读成对方消极。
	if (!bRetreating && Context.EnemyIdleSeconds >= IdlePressSeconds)
	{
		Decision.PreferredRangeDelta = -MaxRangeDelta;
	}

	// =======================================================================
	// 第二段：选技能
	// =======================================================================
	float BestScore = 0.f;
	int32 BestIndex = INDEX_NONE;
	FString BestRoleName;
	FString BestTags;

	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		const FArenaAbilityCandidate& Candidate = Candidates[Index];
		if (!Candidate.bUsable || !Candidate.SlotTag.IsValid())
		{
			continue;
		}

		// 关掉评分 = 取第一个可用槽位。这是排查"是不是评分出的问题"用的退化路径，
		// 不是给正式用的（它和原来那套轮询一样瞎）。
		if (!bEnableUtilityScoring)
		{
			BestIndex = Index;
			BestScore = 1.f;
			BestRoleName = TEXT("未评分");
			break;
		}

		// 没配画像 → 一组中性默认值（会用它，但不特别想用）。
		const FArenaAbilityProfile DefaultProfile;
		const FArenaAbilityProfile* Found = FindProfile(Candidate.SlotTag);
		const FArenaAbilityProfile& Profile = Found ? *Found : DefaultProfile;

		// 能量门：不够就是放不出来，直接不参与竞争（理由见 EnergyCost 的注释）。
		// 默认 EnergyCost = 0，所以没配耗能的技能完全不受这条影响。
		if (Profile.EnergyCost > 0.f && Context.SelfEnergyRatio < Profile.EnergyCost)
		{
			continue;
		}

		// ① 距离贴合：技能生效的距离带。
		const float RangeFit = ArenaTactic_RangeFit(Distance, Profile.MinRange, Profile.MaxRange, FitFloor);

		// ② 血量甜蜜区：什么时候用它才划算。
		const float SelfFit  = ArenaTactic_HealthFit(SelfHealth,  Profile.SelfHealthSweetSpot,  Profile.HealthBand, HealthFitFloor);
		const float EnemyFit = ArenaTactic_HealthFit(EnemyHealth, Profile.EnemyHealthSweetSpot, Profile.HealthBand, HealthFitFloor);

		// ③ 角色情境：这一类技能此刻有多合时宜。
		float RoleMul = 1.f;
		switch (Profile.Role)
		{
		case EArenaAbilityRole::GapCloser:
			// 越远越想贴上去。已经在普攻射程内时用位移去"接近"是浪费。
			RoleMul = FMath::Lerp(0.6f, 1.8f, FMath::Clamp((Distance - AttackRange) / 300.f, 0.f, 1.f));
			break;

		case EArenaAbilityRole::Ranged:
			// 越远越该用（近战够不着的时候，远程是唯一的输出）。
			RoleMul = FMath::Lerp(1.6f, 0.5f, FMath::Clamp((Distance - AttackRange * 0.5f) / 400.f, 0.f, 1.f));
			break;

		case EArenaAbilityRole::Burst:
			RoleMul = FMath::Lerp(0.8f, 1.7f, 1.f - EnemyHealth);
			break;

		case EArenaAbilityRole::Execute:
			// 比 Burst 更极端，而且【只在残血时】才真正值 —— 满血阶段几乎不用它。
			RoleMul = FMath::Lerp(0.25f, 2.2f, FMath::Clamp((1.f - EnemyHealth) * 1.5f, 0.f, 1.f));
			break;

		case EArenaAbilityRole::Defensive:
		case EArenaAbilityRole::Evade:
			// 自己吃紧就想要。满血时几乎不用（不用就不会浪费）。
			RoleMul = FMath::Lerp(0.5f, 1.9f, 1.f - SelfHealth);
			break;

		case EArenaAbilityRole::Sustain:
			RoleMul = FMath::Lerp(0.3f, 1.9f, 1.f - SelfHealth);
			break;

		case EArenaAbilityRole::Buff:
			// 满血、而且对面还没交牌的时候开 —— 开了就要打，不该在他刚交完时开
			// （那半秒是用来打的，不是用来上 buff 的）。
			RoleMul = FMath::Lerp(0.7f, 1.5f, SelfHealth)
				* (Context.bEnemyOnCooldown ? 0.6f : 1.f);
			break;

		case EArenaAbilityRole::Disrupt:
			// 他手里有牌 → 打断他。他全在冷却时打断没有意义。
			RoleMul = FMath::Lerp(0.7f, 1.6f, Readiness);
			break;

		case EArenaAbilityRole::Unspecified:
		default:
			RoleMul = 1.f;
			break;
		}

		// ④ 时机加成："他刚交完技能"这半秒是所有进攻型角色最该抢的窗口。
		//
		// Appetite 表达"这一类技能有多喜欢这个窗口"：进攻型喜欢，自保型不喜欢
		// （他刚交完 = 我安全 = 没必要交防御）。这正是把 isOnCooldown 从
		// 一个 bool 变成一个能参与评分的东西的关键 —— 原本它只能写成
		// "if (onCooldown) 用攻击技"，那是一条规则，这里是同一件事的一个特例。
		float Appetite = 0.f;
		switch (Profile.Role)
		{
		case EArenaAbilityRole::GapCloser:
		case EArenaAbilityRole::Burst:
		case EArenaAbilityRole::Execute:
		case EArenaAbilityRole::Disrupt:
			Appetite = 1.f;
			break;

		case EArenaAbilityRole::Defensive:
		case EArenaAbilityRole::Evade:
			Appetite = -0.6f;
			break;

		default:
			Appetite = 0.f;
			break;
		}
		// 【Profile.EnemySpentMul 为什么乘在窗口里而不是单独一项】它是"这个技能对
		// 这个窗口的额外偏好"，而窗口不开时它本来就不该起作用 —— 乘在窗口内就
		// 不会出现"没窗口时也被它整体缩放"的错。默认 1 = 不表态。
		const float OpportunityFit = Context.bEnemyOnCooldown
			? (1.f + OpportunityBonus * Appetite) * Profile.EnemySpentMul
			: 1.f;

		// ⑤ 立场加成：战略层说"这一回合要拉扯"，那远程和防御就更值一点。
		//
		// 【为什么这一项是乘法而不是让战略层直接改技能权重】立场是慢变量、
		// 一秒才变一次；技能权重是配置。让慢变量去改配置意味着两处状态要做同步，
		// 而乘法是每次算一遍、天然一致。
		float StanceMul = 1.f;
		switch (Context.Stance)
		{
		case EArenaAIStance::Chase:
			if (Profile.Role == EArenaAbilityRole::GapCloser) { StanceMul = 1.35f; }
			break;

		case EArenaAIStance::Poke:
			if (Profile.Role == EArenaAbilityRole::Ranged)    { StanceMul = 1.25f; }
			if (Profile.Role == EArenaAbilityRole::Defensive) { StanceMul = 1.20f; }
			if (Profile.Role == EArenaAbilityRole::GapCloser) { StanceMul = 0.75f; }
			break;

		case EArenaAIStance::Counter:
			// 蹲反打：留住防御和打断等他先动，别主动交位移贴上去。
			if (Profile.Role == EArenaAbilityRole::Defensive ||
				Profile.Role == EArenaAbilityRole::Disrupt)   { StanceMul = 1.30f; }
			if (Profile.Role == EArenaAbilityRole::GapCloser) { StanceMul = 0.70f; }
			break;

		case EArenaAIStance::Brawl:
			if (Profile.Role == EArenaAbilityRole::GapCloser ||
				Profile.Role == EArenaAbilityRole::Burst)     { StanceMul = 1.25f; }
			break;

		case EArenaAIStance::Balanced:
		default:
			break;
		}

		// ⑥ 反连放：刚放过【这一个】就大幅降分。
		float RepeatMul = 1.f;
		if (const float* LastCastTime = LastCastTimes.Find(Candidate.SlotTag))
		{
			if (Context.Now - *LastCastTime < Profile.RepeatLockout)
			{
				RepeatMul = RepeatPenalty;
			}
		}

		// ⑦ 族锁：刚放过【这一族】的另一个技能，一起降分。
		//
		// 和⑥的分工：⑥管"同一个技能别连着放"，⑦管"别把一族的技能挨个放一遍"。
		// 后者才是低血量时看起来像乱按的那个 —— 四个自保技同时高分，⑥一个都拦不住。
		float RoleLockoutMul = 1.f;
		if (RoleLockout > 0.f)
		{
			for (const TPair<EArenaAbilityRole, float>& LastRoleCast : LastRoleCastTimes)
			{
				if (ArenaTactic_SameFamily(LastRoleCast.Key, Profile.Role)
					&& Context.Now - LastRoleCast.Value < RoleLockout)
				{
					RoleLockoutMul = RoleLockoutPenalty;
					break;
				}
			}
		}

		// ⑧ 承诺黏性：上一拍选的就是它 → 加分。
		//
		// 【它和⑦是互补的，方向相反】⑦在"放完之后"压住它，⑧在"放出去之前"
		// 托住它。中间那 1~2 秒正是决策会翻来覆去的那段窗口 —— 承诺把这段
		// 窗口里选中的技能锁住，于是"最后按下去的"和"理由里写的"终于是同一个。
		const float CommitMul = (Context.CommittedSlot.IsValid()
			&& Candidate.SlotTag == Context.CommittedSlot)
			? CommitBonus
			: 1.f;

		// ⑨ 连招：画像声明了"跟在某个技能后面用"，且前摇还在窗口内。
		//
		// 【它是最小可用的"序列"】没有状态机、没有行为树 —— 只需要画像上多填
		// 一条"我接在谁后面"。真正的多步连招（A→B→C）用多条 FollowUpTag
		// 首尾相接就能表达，不需要新概念。
		float ComboMul = 1.f;
		if (Profile.FollowUpTag.IsValid() && Profile.FollowUpWindow > 0.f)
		{
			if (const float* LeadCastTime = LastCastTimes.Find(Profile.FollowUpTag))
			{
				if (Context.Now - *LeadCastTime < Profile.FollowUpWindow)
				{
					ComboMul = Profile.FollowUpBonus;
				}
			}
		}

		// ⑩⑪⑫ 对手此刻的姿势：他挨不挨得住打。
		//
		// 【和上面那几条的区别】①~⑨ 读的全是"我"和"配置"（我的距离、我的血量、
		// 我放过什么、画像怎么配的），这三条是【对手的动作】—— 战术层第一次
		// 真的在看他现在在干什么，而不是只看他手里有什么牌。
		//
		// 【为什么分成三项而不是一个"对手状态加成"】三种状态要求的反应是
		// 相反的：他被控 → 交爆发；他举盾 → 别交爆发；他攒着大招 → 别换血。
		// 合成一项就必须在里面写 if，那和拆开写没有区别，只是更难读。
		// 【每一项都是"角色默认 × 画像的额外倍率"，而不是二选一】
		// 角色那半是"不填也能工作"的常识；画像那半是逐技能的微调（默认 1 = 不表态）。
		// 为什么两个都要：角色给的默认值对十个技能是同一个数，而
		// "上挑该在他没被控时用、回身击退该在他已经被控时用"这种区别，
		// 角色层面表达不了 —— 它们同为 Disrupt。
		//
		// 【画像那半为什么写在 if 里面】写在乘法链上单独一项的话，状态没触发时
		// 它照样会缩放分数（比如 EnemyHardControlMul=0.5 会变成"任何时候都减半"）。
		// 放进 if 里，语义才是"他处于这个姿势时的额外偏好"。
		float HardControlMul = 1.f;
		if (Context.EnemyAction.bHardControlled)
		{
			if (ArenaTactic_IsOffensive(Profile.Role))
			{
				HardControlMul = PunishBonus;
			}
			else if (ArenaTactic_IsSelfPreservation(Profile.Role))
			{
				HardControlMul = PunishSelfPreservationMul;
			}

			HardControlMul *= Profile.EnemyHardControlMul;
		}

		float DamageProofMul = 1.f;
		if (Context.EnemyAction.bDamageProof)
		{
			if (ArenaTactic_DealsDamage(Profile.Role))
			{
				DamageProofMul = WastedOnBlockMul;
			}

			DamageProofMul *= Profile.EnemyDamageProofMul;
		}

		float LoadedThreatMul = 1.f;
		if (Context.EnemyAction.bThreatLoaded)
		{
			if (ArenaTactic_IsSelfPreservation(Profile.Role))
			{
				LoadedThreatMul = RespectBonus;
			}
			else if (ArenaTactic_IsDamageSpike(Profile.Role))
			{
				LoadedThreatMul = RespectPenaltyMul;
			}

			LoadedThreatMul *= Profile.EnemyThreatLoadedMul;
		}

		// ⑬ 自省：这个技能最近【实际】打中过几次。
		//
		// 【它和 Weight 的关系】Weight 是人给的"这个技能有多重要"（设计意图），
		// 这个数是 Bot 自己试出来的"这个技能现在还灵不灵"（事实）。两者相乘，
		// 于是"重要的技能手感差"表现为"还是优先用它，但没那么坚决"。
		//
		// 【为什么要判 DealsDamage】不判的话，位移/防御/增益会因为"放出去对手没掉血"
		// 被记成没打中 —— 见 NotifyOutcome 的注释，那正是这条学习最容易变蠢的地方。
		// 【为什么判 HitRatePolicy 而不是直接判 Role】"该不该被掉血评判"和
		// "这个技能是干什么的"是两个问题。打断类（上挑 / 回身击退）既是控制
		// 也是实打实的伤害，按角色判它就永远学不到东西 —— 而它恰恰是最该学的
		// 那类（空了就是白交一个控）。所以给画像一条显式开关，默认仍跟角色走。
		const bool bLearnsHitRate =
			Profile.HitRatePolicy == EArenaHitRatePolicy::Always
			|| (Profile.HitRatePolicy == EArenaHitRatePolicy::FromRole
				&& ArenaTactic_DealsDamage(Profile.Role));

		const float* LearnedReliability = SlotReliability.Find(Candidate.SlotTag);
		const float ReliabilityMul = (bEnableOutcomeLearning && bLearnsHitRate && LearnedReliability)
			? *LearnedReliability
			: 1.f;

		// 全部相乘。
		//
		// 【为什么是乘法而不是加权求和】求和需要给每一项定一个"权重"，而那些权重
		// 又是另一组要调的数，而且它们会互相补偿（距离不合适 0.5 分 + 血量特别合适
		// 0.5 分 = 还是能选上）。乘法没有这个问题：任何一项塌了，整体就塌了。
		// 代价是分数不可解释成"概率"，但这里本来也不需要概率，只需要排序。
		const float Score = FMath::Max(0.f, Profile.Weight)
			* RangeFit
			* SelfFit
			* EnemyFit
			* RoleMul
			* OpportunityFit
			* StanceMul
			* RepeatMul
			* RoleLockoutMul
			* CommitMul
			* ComboMul
			* HardControlMul
			* DamageProofMul
			* LoadedThreatMul
			* ReliabilityMul;

		if (Score > BestScore)
		{
			// 顺手记下这一项是"赢在哪"—— 分数量级看不出是哪一项把它托上去的，
			// 而调参时最想知道的正是那个。只对当前的赢家保留。

			BestScore = Score;
			BestIndex = Index;
			BestRoleName = ArenaTactic_RoleToString(Profile.Role);

			BestTags.Reset();
			if (CommitMul > 1.f)        { BestTags += TEXT(" 承诺"); }
			if (ComboMul > 1.f)         { BestTags += TEXT(" 连招"); }
			if (OpportunityFit > 1.f)   { BestTags += TEXT(" 趁他交完"); }
			if (HardControlMul > 1.f)   { BestTags += TEXT(" 趁他挨控"); }
			if (LoadedThreatMul > 1.f)  { BestTags += TEXT(" 防他攒的大的"); }
			if (RoleLockoutMul < 1.f)   { BestTags += TEXT(" 族锁"); }
			if (RepeatMul < 1.f)        { BestTags += TEXT(" 反连放"); }
			if (DamageProofMul < 1.f)   { BestTags += TEXT(" 他挡着"); }
			if (ReliabilityMul < 1.f)   { BestTags += TEXT(" 最近不灵"); }
		}
	}

	// =======================================================================
	// 第三段：出结论
	// =======================================================================

	// 对手此刻的姿势。拼在这里而不是循环里，因为它和"选了哪个技能"无关 ——
	// 选没选中都该显示出来，否则排查"为什么这次不交技能"时会看不到原因的一半。
	FString EnemyStateNote;
	if (Context.EnemyAction.bHardControlled) { EnemyStateNote += TEXT(" 挨控中"); }
	if (Context.EnemyAction.bDamageProof)    { EnemyStateNote += TEXT(" 挡着"); }
	if (Context.EnemyAction.bThreatLoaded)   { EnemyStateNote += TEXT(" 攒着大的"); }

	if (BestIndex != INDEX_NONE && BestScore >= MinScoreToCast)
	{
		Decision.bCastSkill = true;
		Decision.SlotTag = Candidates[BestIndex].SlotTag;
		Decision.Score = BestScore;
		Decision.Reason = FString::Printf(
			TEXT("%s 分%.2f%s | 距%.0f 我%.0f%% 敌%.0f%% 他%s%s"),
			*BestRoleName, BestScore, *BestTags, Distance,
			SelfHealth * 100.f, EnemyHealth * 100.f,
			Context.bEnemyOnCooldown ? TEXT("刚交完") : TEXT("手里有牌"),
			*EnemyStateNote);
	}
	else
	{
		Decision.bCastSkill = false;
		Decision.Score = BestScore;
		Decision.Reason = (BestIndex == INDEX_NONE)
			? TEXT("没有可用技能")
			: FString::Printf(TEXT("都不到门槛（最高 %s 分%.2f < %.2f）%s"),
				*BestRoleName, BestScore, MinScoreToCast, *EnemyStateNote);
	}

	return Decision;
}
