// 斗魂竞技场：AI 观测层的实现。

#include "GAS/ArenaBehaviorObserver.h"

#include "AbilitySystemComponent.h"
#include "Kismet/GameplayStatics.h"

#include "GAS/ArenaAISaveGame.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyAbilitySystemComponent.h"

namespace
{
	/**
	 * 当前存档格式版本。
	 *
	 * ⚠️ 必须和 FArenaPlayerProfile::SchemaVersion 的默认值一致。
	 * 不一致的话，新存的档会在下次读的时候被自己判成"格式不对"而丢弃 ——
	 * 表现是"每次启动都像第一次见这个玩家"，而且没有任何报错。
	 */
	constexpr int32 CurrentProfileSchemaVersion = 2;

	/** 侧移判定用的死区：归一化速度小于这个值就不算在横向移动。 */
	constexpr float StrafeDeadZone = 0.1f;
}

UArenaBehaviorObserver::UArenaBehaviorObserver()
{
	FillDefaultCooldownTags();
}

void UArenaBehaviorObserver::FillDefaultCooldownTags()
{
	if (TrackedCooldownTags.Num() > 0)
	{
		return;   // BP 上配过了，不覆盖
	}

	// 这些是项目里目前全部"按键技能"的冷却标签（见 LOLGameplayTags.h）。
	// 没有配冷却的技能观测不到 —— 那是【故意的】：观测层不该因为某个技能
	// 还没做冷却就整个失效，也不该为了观测去要求技能必须配冷却。
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_Flash);
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_ThrowDagger);
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_Stealth);
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_Block);
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_Dodge);
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_GroundDodge);
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_SpinSlash);
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_TurnSlash);
	TrackedCooldownTags.Add(LOLGameplayTags::State_Cooldown_DeathHarvest);
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

void UArenaBehaviorObserver::BeginMatch()
{
	if (bMatchBegun)
	{
		// GameMode 可能在重开一局时重复通知。重复读档会把这一局刚折进画像的
		// 数据用盘上的旧版本覆盖掉 —— 所以必须幂等。
		return;
	}

	bMatchBegun = true;
	LoadProfile();
	++Profile.MatchesObserved;
	BeginRound();
}

void UArenaBehaviorObserver::BeginRound()
{
	RoundSeconds = 0.f;
	RoundDistanceSum = 0.f;
	RoundDistanceSamples = 0;
	RoundCloseSeconds = 0.f;
	RoundFarSeconds = 0.f;
	RoundStrafePosSeconds = 0.f;
	RoundStrafeNegSeconds = 0.f;
	RoundApproachSum = 0.f;
	RoundApproachSamples = 0;
	RoundSkillCasts = 0;
	RoundCooldownSeconds = 0.f;
	bEnemyOnCooldownNow = false;
	EnemyActionNow = FArenaEnemyActionState();
	EnemyIdleSeconds = 0.f;

	// 缓存清零 = "上一回合结束时没有技能在冷却"。
	//
	// 【这个假设会不会错】本项目的回合之间有一段备战期（RewardSelection），
	// 冷却早就走完了；而且这里记错一次的代价只是把一次"上回合遗留的冷却"
	// 多算成一次释放。为了这么小的误差去引入"回合开始时先采样一次真实状态"
	// 的额外时序（那要求 BeginRound 时对手 Pawn 必须已经存在）不值得。
	RefreshCooldownTagCache();
}

bool UArenaBehaviorObserver::EndMatchAndSave()
{
	bMatchBegun = false;

	if (SaveSlotName.IsEmpty())
	{
		return false;   // 调试配置：不落盘。不是错误。
	}

	UArenaAISaveGame* Save = Cast<UArenaAISaveGame>(
		UGameplayStatics::CreateSaveGameObject(UArenaAISaveGame::StaticClass()));

	if (!Save)
	{
		return false;
	}

	Save->Profile = Profile;
	return UGameplayStatics::SaveGameToSlot(Save, SaveSlotName, /*UserIndex=*/0);
}

void UArenaBehaviorObserver::ResetProfile()
{
	Profile = FArenaPlayerProfile();
	LastRoundSnapshot = FArenaPlayerProfile();
}

void UArenaBehaviorObserver::SetStrategicMemo(const FArenaStrategicMemo& InMemo)
{
	Profile.Memo = InMemo;
}

// ---------------------------------------------------------------------------
// 每帧观测
// ---------------------------------------------------------------------------

void UArenaBehaviorObserver::ObserveCombatTick(float DeltaSeconds, const AActor* SelfPawn, const AActor* EnemyPawn)
{
	if (!SelfPawn || !EnemyPawn || DeltaSeconds <= 0.f)
	{
		return;
	}

	// BP 上改过标签表的话缓存长度会对不上。对齐一次（每帧查长度是常数开销）。
	if (LastCooldownStateCache.Num() != TrackedCooldownTags.Num())
	{
		RefreshCooldownTagCache();
	}

	RoundSeconds += DeltaSeconds;

	const FVector SelfLocation = SelfPawn->GetActorLocation();
	const FVector EnemyLocation = EnemyPawn->GetActorLocation();

	FVector ToEnemy = EnemyLocation - SelfLocation;
	ToEnemy.Z = 0.0;

	// 【为什么到处 static_cast<float>】UE5 是 LWC：FVector 的分量是 double，
	// 所以 Size() / DotProduct() 返回的也是 double。混进 float 的模板
	// （FMath::Clamp / Lerp）会产生一串和真正的问题毫无关系的实例化报错，
	// 见 ue5-lwc-double-float-gotchas。这里显式收窄，把类型钉死在 float。
	const float Distance = static_cast<float>(ToEnemy.Size());

	RoundDistanceSum += Distance;
	++RoundDistanceSamples;

	if (Distance <= CloseRangeThreshold)
	{
		RoundCloseSeconds += DeltaSeconds;
	}
	if (Distance >= FarRangeThreshold)
	{
		RoundFarSeconds += DeltaSeconds;
	}

	const FVector ToEnemyDir = (Distance > KINDA_SMALL_NUMBER)
		? (ToEnemy / Distance)
		: FVector::ZeroVector;

	const FVector EnemyVelocity = EnemyPawn->GetVelocity();
	const float SafeSpeed = FMath::Max(SpeedNormalizer, 1.f);

	// --- 横向绕圈方向 ---
	// 相对"两个单位之间那条连线"的侧移。正负只是一个任意但固定的约定，
	// 消费者只关心"这个玩家是不是老往同一个方向转"（比率偏离 0.5 多少）。
	const FVector LateralAxis = FVector::CrossProduct(FVector::UpVector, ToEnemyDir);
	if (!LateralAxis.IsNearlyZero())
	{
		const float Side = static_cast<float>(FVector::DotProduct(EnemyVelocity, LateralAxis));
		const float SideNormalized = FMath::Clamp(Side / SafeSpeed, -1.f, 1.f);

		if (SideNormalized > StrafeDeadZone)
		{
			RoundStrafePosSeconds += DeltaSeconds;
		}
		else if (SideNormalized < -StrafeDeadZone)
		{
			RoundStrafeNegSeconds += DeltaSeconds;
		}
	}

	// --- 谁在拉近距离 ---
	// 只看玩家自己的速度在"指向 Bot"那个方向上的分量。这样 Bot 自己压上去
	// 不会被他算成"进攻性强" —— 那是 Bot 的动作，不是他的。
	if (!ToEnemyDir.IsNearlyZero())
	{
		const float TowardSelf = static_cast<float>(FVector::DotProduct(EnemyVelocity, -ToEnemyDir));
		RoundApproachSum += FMath::Clamp(TowardSelf / SafeSpeed, -1.f, 1.f);
		++RoundApproachSamples;
	}

	// --- 技能：靠冷却标签的上升沿推断 ---
	//
	// 【为什么在进这个分支之前先清掉标志】下面拿不到对手 ASC 时直接跳过，
	// 标志会保持上一次的值 —— 那会让 Bot 在一个"读不到对手"的局面里
	// 继续以为"他刚交完"并主动进攻。拿不到信息时应当保守，所以先归零。
	// 动作状态同理：清成"什么都没读到"，行为退化成读它之前的样子。
	bEnemyOnCooldownNow = false;
	EnemyActionNow = FArenaEnemyActionState();

	// 这一帧他有没有交技能。用来给下面的"发呆计时"当刹车 —— 见 GetEnemyIdleSeconds。
	// 和上面两个标志同一个口径：读不到对手 ASC 时保持 false，也就是"没看到他动"，
	// 计时器会继续走 —— 但那只是让他看起来站得更久，方向上和"拿不到信息就保守"一致。
	bool bEnemyCastThisFrame = false;

	if (UAbilitySystemComponent* EnemyASC = ResolveEnemyASC(EnemyPawn))
	{
		bool bAnyOnCooldown = false;

		for (int32 Index = 0; Index < TrackedCooldownTags.Num(); ++Index)
		{
			const FGameplayTag& Tag = TrackedCooldownTags[Index];
			const bool bNowOnCooldown = Tag.IsValid() && EnemyASC->HasMatchingGameplayTag(Tag);

			// 上升沿 = 他刚放了这个技能。下降沿不记（那只是冷却走完了）。
			if (bNowOnCooldown && !LastCooldownStateCache[Index])
			{
				++RoundSkillCasts;
				bEnemyCastThisFrame = true;
			}
			if (bNowOnCooldown)
			{
				bAnyOnCooldown = true;
			}

			LastCooldownStateCache[Index] = bNowOnCooldown;
		}

		bEnemyOnCooldownNow = bAnyOnCooldown;

		if (bAnyOnCooldown)
		{
			RoundCooldownSeconds += DeltaSeconds;
		}

		// --- 动作状态：他此刻是"能白打"还是"打不进去" ---
		//
		// 【为什么逐个查，而不是拼一个 FGameplayTagContainer 再 HasAny】
		// 后者每帧要构造一个容器（有分配），而这里一共七个标签，
		// 逐个 HasMatchingGameplayTag 是常数开销、零分配，和上面冷却那圈同构。
		//
		// 【为什么这几个标签可以放心读】它们全部由真正的 GameplayEffect 授予
		// （GE_Stun / GE_KnockUp / GE_Knockback / GE_Blocking / GE_BlockImmune /
		// GE_Stealth / GE_EmpoweredAttack），所以会复制到服务端 —— Bot 看得见。
		// 纯本地的 loose 标签（State.Dodge.Active、State.DeathHarvest.Casting…）
		// 故意不放进来：服务端读不到，写进来只是一个恒 false 的字段。
		// 判断依据见 FArenaEnemyActionState 的注释。
		EnemyActionNow.bHardControlled =
			EnemyASC->HasMatchingGameplayTag(LOLGameplayTags::State_Stunned)
			|| EnemyASC->HasMatchingGameplayTag(LOLGameplayTags::State_KnockUp)
			|| EnemyASC->HasMatchingGameplayTag(LOLGameplayTags::State_Knockback);

		EnemyActionNow.bDamageProof =
			EnemyASC->HasMatchingGameplayTag(LOLGameplayTags::State_Blocking)
			|| EnemyASC->HasMatchingGameplayTag(LOLGameplayTags::State_BlockImmune);

		EnemyActionNow.bThreatLoaded =
			EnemyASC->HasMatchingGameplayTag(LOLGameplayTags::State_Stealth)
			|| EnemyASC->HasMatchingGameplayTag(LOLGameplayTags::State_EmpoweredAttack);
	}

	// --- 发呆计时：他连着多久没动、也没交技能 ---
	//
	// 【为什么放在最后】它要等 bEnemyCastThisFrame 定下来，而那个标志是上面
	// 冷却那圈里才填的。
	//
	// 【为什么用水平速度】竖直分量要排除掉：被击飞的人在 y 轴上飞得很快，
	// 但那不是他在动。Size2D 让"被打上天"落回"静止"而不是"在移动"。
	//
	// 【他晕着的时候这个计时也在走】晕住的人本来就不动，所以他会被算成发呆 ——
	// 但结论是一样的（都该压上去），而且战术层里"他被硬控"那条分支推得比
	// 这条更满，会盖住它。为了这点重合去加一个 bHardControlled 排除，
	// 只会让读代码的人多一个要证的分支。
	const float EnemySpeed = static_cast<float>(EnemyVelocity.Size2D());
	if (!bEnemyCastThisFrame && EnemySpeed <= IdleSpeedThreshold)
	{
		EnemyIdleSeconds += DeltaSeconds;
	}
	else
	{
		EnemyIdleSeconds = 0.f;
	}
}

// ---------------------------------------------------------------------------
// 回合结算：把原始量折算成比率，EMA 进画像
// ---------------------------------------------------------------------------

void UArenaBehaviorObserver::FoldRoundIntoProfile(bool bPlayerWon)
{
	const float SafeSeconds = FMath::Max(RoundSeconds, KINDA_SMALL_NUMBER);

	// --- 先算出"这一回合"的一组比率 ---
	FArenaPlayerProfile Round;
	Round.AvgEngagementDistance = (RoundDistanceSamples > 0)
		? (RoundDistanceSum / static_cast<float>(RoundDistanceSamples))
		: 0.f;
	Round.CloseRangeRatio = RoundCloseSeconds / SafeSeconds;
	Round.FarRangeRatio = RoundFarSeconds / SafeSeconds;

	const float StrafeTotal = RoundStrafePosSeconds + RoundStrafeNegSeconds;
	Round.PlayerStrafeLeftRatio = (StrafeTotal > KINDA_SMALL_NUMBER)
		? static_cast<float>(RoundStrafePosSeconds / StrafeTotal)
		: 0.5f;   // 没有横向移动 = 无倾向，而不是 0（0 会被读成"总是往右"）

	Round.ApproachBias = (RoundApproachSamples > 0)
		? static_cast<float>(RoundApproachSum / static_cast<float>(RoundApproachSamples))
		: 0.f;
	Round.SkillsPerSecond = static_cast<float>(RoundSkillCasts) / SafeSeconds;
	Round.SkillCooldownOccupancy = FMath::Clamp(RoundCooldownSeconds / SafeSeconds, 0.f, 1.f);

	LastRoundSnapshot = Round;

	// --- 结果和样本量无条件记 ---
	if (bPlayerWon)
	{
		++Profile.RoundsPlayerWon;
	}
	else
	{
		++Profile.RoundsBotWon;
	}
	++Profile.RoundsObserved;
	Profile.TotalCombatSeconds += RoundSeconds;
	Profile.PlayerSkillCasts += RoundSkillCasts;

	// --- 太短的回合不折进画像 ---
	// 秒杀局（对面一个照面就倒了）的距离分布全是噪声，折进去只会污染画像，
	// 而且它恰恰是"这个玩家很强"的信号 —— 那个信息由 RoundsPlayerWon 承担，
	// 不需要再用一堆假的距离样本表达一遍。
	if (RoundSeconds < MinRoundSecondsToFold)
	{
		return;
	}

	// 第一次折的时候直接采纳，不做插值。
	// 否则画像从 0 出发，第一局打完 AvgEngagementDistance 只有真值的 35%，
	// 而战略层已经拿它去做判断了 —— 头几局会针对一个"距离偏小"的假画像。
	const bool bFirstFold = (Profile.RoundsObserved <= 1);
	const float Alpha = FMath::Clamp(ProfileBlendAlpha, 0.f, 1.f);

	auto Fold = [bFirstFold, Alpha](float& Current, float NewValue)
	{
		Current = bFirstFold ? NewValue : FMath::Lerp(Current, NewValue, Alpha);
	};

	Fold(Profile.AvgEngagementDistance, Round.AvgEngagementDistance);
	Fold(Profile.CloseRangeRatio,       Round.CloseRangeRatio);
	Fold(Profile.FarRangeRatio,         Round.FarRangeRatio);
	Fold(Profile.PlayerStrafeLeftRatio, Round.PlayerStrafeLeftRatio);
	Fold(Profile.ApproachBias,          Round.ApproachBias);
	Fold(Profile.SkillsPerSecond,       Round.SkillsPerSecond);
	Fold(Profile.SkillCooldownOccupancy, Round.SkillCooldownOccupancy);
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------

UAbilitySystemComponent* UArenaBehaviorObserver::ResolveEnemyASC(const AActor* EnemyPawn) const
{
	// 用项目自己的查找器（它比蓝图库那条路多试两种挂法，见 MyAbilitySystemComponent.h）。
	return EnemyPawn ? UMyAbilitySystemComponent::FindAbilitySystemComponent(EnemyPawn) : nullptr;
}

void UArenaBehaviorObserver::RefreshCooldownTagCache()
{
	LastCooldownStateCache.Reset();
	LastCooldownStateCache.AddZeroed(TrackedCooldownTags.Num());
}

void UArenaBehaviorObserver::LoadProfile()
{
	if (SaveSlotName.IsEmpty())
	{
		return;
	}

	if (!UGameplayStatics::DoesSaveGameExist(SaveSlotName, /*UserIndex=*/0))
	{
		// 第一次见这个玩家。保持默认画像，不是错误。
		return;
	}

	UArenaAISaveGame* Save = Cast<UArenaAISaveGame>(
		UGameplayStatics::LoadGameFromSlot(SaveSlotName, /*UserIndex=*/0));

	if (!Save)
	{
		return;   // 槽里是别的类型的存档 / 损坏。退回默认画像。
	}

	if (Save->Profile.SchemaVersion != CurrentProfileSchemaVersion)
	{
		// 【为什么直接丢弃而不是"尽力读"】语义变过的字段读进来是【看起来正常但意思不对】
		// 的数，而画像的所有消费者都不会对它做合理性检查 —— 结果是一个
		// 安静地针对假数据的 AI。丢弃 = 退化成"第一次见你"，是个能接受且可解释的结果。
		UE_LOG(LogTemp, Warning,
			TEXT("[ArenaAI] 玩家画像存档版本 %d != 当前 %d，已丢弃（Bot 会从零开始观察）。"),
			Save->Profile.SchemaVersion, CurrentProfileSchemaVersion);
		return;
	}

	Profile = Save->Profile;
}
