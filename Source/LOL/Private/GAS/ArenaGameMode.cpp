// 斗魂竞技场：回合驱动的实现。

#include "GAS/ArenaGameMode.h"

#include "AbilitySystemComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

#include "GAS/ArenaAugmentData.h"
#include "GAS/ArenaBotController.h"
#include "GAS/ArenaGameState.h"
#include "GAS/ArenaItemData.h"
#include "GAS/ArenaLoadoutComponent.h"
#include "GAS/ArenaPlayerState.h"
#include "GAS/ArenaRewardPool.h"
#include "GAS/ArenaTrainingDummy.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/HeroCombatCharacter.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyAbilitySystemComponent.h"

#define LOCTEXT_NAMESPACE "ArenaGameMode"

namespace
{
	/**
	 * 名字统一加 Arena 前缀：这些是匿名命名空间里的自由函数，
	 * 而这个项目是 Unity 构建（UBT 会把多个 .cpp 合并成一个翻译单元）——
	 * 重名的自由函数在合并时会直接编译不过，而且报的错看着毫无关系。
	 */
	FText ArenaTierDisplayName(EArenaItemTier Tier)
	{
		switch (Tier)
		{
		case EArenaItemTier::Legendary:  return LOCTEXT("TierLegendary", "传说");
		case EArenaItemTier::Prismatic:  return LOCTEXT("TierPrismatic", "棱彩");
		}

		// 加了新品质但忘了在这里补一条 —— 返回空文本会让按钮上是空的，
		// 比返回"未知"更容易被当成 bug 发现。
		return LOCTEXT("TierUnknown", "未知品质");
	}

	/** 装备名。DataAsset 上没填 DisplayName 时退回资产名，不要留一个空按钮。 */
	FText ArenaItemLabel(const UArenaItemData* Item)
	{
		if (!Item)
		{
			return LOCTEXT("ItemMissing", "（缺失的装备资产）");
		}

		return Item->DisplayName.IsEmpty() ? FText::FromString(Item->GetName()) : Item->DisplayName;
	}

	/**
	 * 装备品质 → 卡片品质（FArenaChoiceOption::Tier 的统一口径）。
	 * 物品只有传说/棱彩两档：传说上金卡、棱彩上棱彩卡。
	 */
	EArenaAugmentTier ArenaItemTierToAugmentTier(EArenaItemTier Tier)
	{
		return Tier == EArenaItemTier::Prismatic
			? EArenaAugmentTier::Prismatic
			: EArenaAugmentTier::Gold;
	}

	/** 海克斯名。理由同上。 */
	FText ArenaAugmentLabel(const UArenaAugmentData* Augment)
	{
		if (!Augment)
		{
			return LOCTEXT("AugmentMissing", "（缺失的海克斯资产）");
		}

		return Augment->DisplayName.IsEmpty() ? FText::FromString(Augment->GetName()) : Augment->DisplayName;
	}

	FArenaBranchOption ArenaMakeBranch(EArenaRewardBranch Branch, EArenaItemTier Tier, int32 Count)
	{
		FArenaBranchOption Option;
		Option.Branch = Branch;
		Option.Tier = Tier;
		Option.Count = Count;
		return Option;
	}

	/**
	 * 属性锻造器（stat anvil）抽签：从池子里【不重复】地抽 Count 条。
	 * 池子不够时按实际能给的发（抽 Count > Num 条 = 全池发一遍）。
	 * 部分洗牌而不是全洗再截断 —— 抽几条就摇几次骰子。
	 */
	TArray<FArenaItemStatModifier> ArenaRollStatAnvil(const TArray<FArenaItemStatModifier>& Pool, int32 Count)
	{
		TArray<FArenaItemStatModifier> Result;
		if (Pool.Num() == 0 || Count <= 0)
		{
			return Result;
		}

		TArray<int32> Indices;
		Indices.SetNumUninitialized(Pool.Num());
		for (int32 i = 0; i < Pool.Num(); ++i)
		{
			Indices[i] = i;
		}

		const int32 Take = FMath::Min(Count, Pool.Num());
		for (int32 i = 0; i < Take; ++i)
		{
			Swap(Indices[i], Indices[FMath::RandRange(i, Indices.Num() - 1)]);
			Result.Add(Pool[Indices[i]]);
		}
		return Result;
	}

	FArenaRoundReward ArenaMakeBranchReward(EArenaItemTier Tier, int32 ItemCount, int32 ForgeCount)
	{
		FArenaRoundReward Reward;
		Reward.Kind = EArenaRewardKind::BranchChoice;
		Reward.Branches.Add(ArenaMakeBranch(EArenaRewardBranch::Item, Tier, ItemCount));
		Reward.Branches.Add(ArenaMakeBranch(EArenaRewardBranch::Forge, Tier, ForgeCount));
		return Reward;
	}

	FArenaRoundReward ArenaMakeAugmentReward(EArenaAugmentTier Tier)
	{
		FArenaRoundReward Reward;
		Reward.Kind = EArenaRewardKind::Augment;
		Reward.AugmentTier = Tier;
		return Reward;
	}

	FArenaRoundReward ArenaMakeFixedForgeReward(EArenaItemTier Tier, int32 Count)
	{
		FArenaRoundReward Reward;
		Reward.Kind = EArenaRewardKind::FixedForge;
		Reward.FixedGrant = ArenaMakeBranch(EArenaRewardBranch::Forge, Tier, Count);
		return Reward;
	}

	FArenaHpLossBand ArenaMakeBand(int32 FromRound, int32 HpLoss)
	{
		FArenaHpLossBand Band;
		Band.FromRound = FromRound;
		Band.HpLoss = HpLoss;
		return Band;
	}

	/**
	 * 这个 PlayerStart 是不是备战点。
	 *
	 * 【为什么认三种写法，而不是只认对象名】
	 * 原本只匹配 Actor->GetName().StartsWith("PrepStart")，但这在 World Partition
	 * 地图上是失效的：WP 会把摆进关卡的 actor 自动改名成 PlayerStart_UAID_<十六进制>，
	 * 而在编辑器 Outliner 里按 F2 改的是 【Label】、不是对象名。于是"把出生点改名叫
	 * PrepStart_A"这个操作改的根本不是被匹配的那个字符串 —— 摆完静默不生效，
	 * 表现是备战阶段退回战斗出生点，且只在 Log 里留一条。Arena_1v1 里就躺着一个
	 * 现成的样本：label 是 PrepStart_A、对象名是 PlayerStart_3。
	 *
	 * 所以这里三种都认：
	 *   ① Tag  —— 唯一在【打包后】仍然有效的机制（Label 编辑器专用，打包时被剥掉），
	 *             也是这里唯一推荐的写法：Details 面板加一个 PrepStart 标签即可。
	 *   ② 对象名 —— 老式（非 WP）地图里直接摆 actor 并命名的写法，保持兼容。
	 *   ③ Label —— GetActorNameOrLabel() 在编辑器里返回 Label、打包后返回对象名，
	 *             一次调用覆盖两种情况；再叠一层 GetName() 是因为两者可以不一致
	 *             （上面 Arena_1v1 那个样本就是）。
	 *
	 * 【三个调用点】GetContenderSpawnPoint（取反 = 战斗出生点）、GetPrepSpawnPoint、
	 * 以及 ChoosePlayerStart_Implementation 的兜底 —— 最后那个是"别把首次出生的人
	 * 放到备战台上"，见那边的注释。三处必须是同一个判据，否则两套出生点会互相串。
	 */
	bool IsPrepSpawnActor(const AActor* Actor)
	{
		if (!Actor)
		{
			return false;
		}

		static const FName PrepSpawnTag(TEXT("PrepStart"));
		if (Actor->ActorHasTag(PrepSpawnTag))
		{
			return true;
		}

		if (Actor->GetName().StartsWith(TEXT("PrepStart")))
		{
			return true;
		}

		return Actor->GetActorNameOrLabel().StartsWith(TEXT("PrepStart"));
	}
}

// ---------------------------------------------------------------------------
// 构造：把需求里那张表配成默认值
// ---------------------------------------------------------------------------

AArenaGameMode::AArenaGameMode()
{
	// GameState / PlayerState 换成竞技场那两个。
	// 【漏了这两行会怎样】PlayerStateClass 还是基类的 AMyPlayerState 的话，
	// PostLogin 里 Cast<AArenaPlayerState> 全是 null，整个模式一声不响地什么都不做。
	GameStateClass = AArenaGameState::StaticClass();
	PlayerStateClass = AArenaPlayerState::StaticClass();

	// 备战区的训练木桩。C++ 里给默认值，BP_ArenaGameMode 上可以直接换成蓝图子类。
	TrainingDummyClass = AArenaTrainingDummy::StaticClass();

	// --- 属性锻造器的默认属性池 ---
	//
	// 【为什么 C++ 给默认值】池子空着的表现是「锻造器点不动还刷警告」（见 HandleUseForge），
	// 那是配置问题的报法，不是「开箱就能玩」的初始状态。这里的数值是占位基准
	// （口径：传说 ≈ 一件小件、棱彩 ≈ 一件成品的 60%），在 BP_ArenaGameMode 上随便改。
	// 攻速/暴击/吸血按「0.1 = 10%」填 —— 和属性集的口径一致（见 ArenaItemStats.h）。
	{
		auto AddStat = [](TArray<FArenaItemStatModifier>& Pool, const FGameplayAttribute& Attribute, float Value)
		{
			FArenaItemStatModifier Mod;
			Mod.Attribute = Attribute;
			Mod.Value = Value;
			Pool.Add(Mod);
		};

		// 传说：AD / AP / 生命 / 双抗 / 攻速 / 暴击 / 技能急速 / 移速 / 全能吸血
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetAttackDamageAttribute(), 7.f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetAbilityPowerAttribute(), 10.f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetMaxHealthAttribute(), 90.f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetArmorAttribute(), 8.f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetMagicResistAttribute(), 8.f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetBonusAttackSpeedPercentAttribute(), 0.08f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetCritChanceAttribute(), 0.07f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetAbilityHasteAttribute(), 7.f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetMoveSpeedAttribute(), 12.f);
		AddStat(StatAnvilPool_Legendary, UHeroCombatAttributeSet::GetOmnivampAttribute(), 0.04f);

		// 棱彩：同表 ×1.7 左右，另加生命偷取
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetAttackDamageAttribute(), 12.f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetAbilityPowerAttribute(), 17.f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetMaxHealthAttribute(), 150.f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetArmorAttribute(), 14.f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetMagicResistAttribute(), 14.f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetBonusAttackSpeedPercentAttribute(), 0.14f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetCritChanceAttribute(), 0.12f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetAbilityHasteAttribute(), 12.f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetMoveSpeedAttribute(), 20.f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetOmnivampAttribute(), 0.07f);
		AddStat(StatAnvilPool_Prismatic, UHeroCombatAttributeSet::GetLifeStealAttribute(), 0.10f);
	}

	// --- 前 6 回合（下标 0 = 第 1 回合）---
	//
	// R1 二选一：传说装备×1  |  传说锻造器×5
	RoundRewards.Add(ArenaMakeBranchReward(EArenaItemTier::Legendary, 1, 5));
	// R2 海克斯三选一（白银档）
	RoundRewards.Add(ArenaMakeAugmentReward(EArenaAugmentTier::Silver));
	// R3 固定统一发放：棱彩锻造器×1
	RoundRewards.Add(ArenaMakeFixedForgeReward(EArenaItemTier::Prismatic, 1));
	// R4 海克斯三选一（黄金档）
	RoundRewards.Add(ArenaMakeAugmentReward(EArenaAugmentTier::Gold));
	// R5 二选一：棱彩装备×1  |  棱彩锻造器×3
	RoundRewards.Add(ArenaMakeBranchReward(EArenaItemTier::Prismatic, 1, 3));
	// R6 首次大场阈值判定，无奖励
	RoundRewards.Add(FArenaRoundReward());

	// --- 第 7 回合起：装备 / 装备 / 海克斯 三回合一轮 ---
	//
	// 【装备回合的品质和数量是沿用第 5 回合那一对】需求只写了"装备回合"，
	// 没写品质也没写锻造器给几个。这里不新编一组数，直接用需求里已经定义过的
	// 棱彩那一对（这也是唯一出现过的棱彩分支），要改直接在 Details 面板里改。
	LoopRewards.Add(ArenaMakeBranchReward(EArenaItemTier::Prismatic, 1, 3));
	LoopRewards.Add(ArenaMakeBranchReward(EArenaItemTier::Prismatic, 1, 3));
	LoopRewards.Add(ArenaMakeAugmentReward(EArenaAugmentTier::Prismatic));

	// --- 输一回合扣多少大场血量（需求：1-4 → 15，5-8 → 30，9-12 → 40，13+ → 50）---
	HpLossBands.Add(ArenaMakeBand(1, 15));
	HpLossBands.Add(ArenaMakeBand(5, 30));
	HpLossBands.Add(ArenaMakeBand(9, 40));
	HpLossBands.Add(ArenaMakeBand(13, 50));
}

// ---------------------------------------------------------------------------
// 登录 / 掉线
// ---------------------------------------------------------------------------

void AArenaGameMode::PostLogin(APlayerController* NewPlayer)
{
	// Super 里已经把 Pawn 生出来了（PostLogin → HandleStartingNewPlayer → RestartPlayer），
	// 所以这之后就能安全地设等级 / 挪位置。
	Super::PostLogin(NewPlayer);

	AArenaGameState* GS = GetArenaGameState();
	AArenaPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<AArenaPlayerState>() : nullptr;

	if (!GS || !PS)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] PostLogin 拿不到 AArenaGameState / AArenaPlayerState —— 多半是 "
				 "BP_ArenaGameMode 的 GameStateClass / PlayerStateClass 被换掉了。这个连接不参赛。"));
		return;
	}

	const int32 Slot = FindFreeContenderSlot();
	if (Slot == INDEX_NONE)
	{
		// 需求是 1V1。第三个连接不参赛：他的 Pawn 照样会被引擎生出来（那是 AGameModeBase
		// 的流程，不归这里管），但不在参赛者表里 —— 不会被发奖励、不会被判胜负、
		// 也不在出生点的分配里。吵一声是因为"多了个人在场上乱跑"必须有人知道。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 连进来了，但两个参赛位已经满了 —— 竞技场是 1V1，这个人不参赛。"),
			*PS->GetName());
		return;
	}

	GS->SetContender(Slot, PS);

	if (GS->AreContendersReady())
	{
		// 真人凑齐了，把等 Bot 的定时器撤掉 —— 不撤的话过一会儿会多出一个 Bot，
		// 而那时候参赛位满了，Bot 会变成一个没人管的第三者。
		GetWorldTimerManager().ClearTimer(BotSpawnTimerHandle);
		StartMatch();
		return;
	}

	if (bSpawnBotIfAlone)
	{
		// 【为什么用"等多久"而不是"等不到就直接生"】需求要的是两种对手都要：
		// 真人来了就打真人，一直不来才放 Bot。判据只能是时间。
		GetWorldTimerManager().SetTimer(BotSpawnTimerHandle, this, &AArenaGameMode::TrySpawnBot,
			FMath::Max(0.f, BotJoinDelay), /*bLoop=*/false);
	}
}

void AArenaGameMode::Logout(AController* Exiting)
{
	// 【必须在 Super 之前读】AGameModeBase::Logout 会把 PlayerState 从 GameState 上摘掉，
	// 之后 PlayerController 销毁时连它一起销毁 —— 那时候再拿这个指针就是悬空的。
	int32 Slot = INDEX_NONE;
	if (const AArenaGameState* GS = GetArenaGameState())
	{
		if (const AArenaPlayerState* PS = Exiting ? Exiting->GetPlayerState<AArenaPlayerState>() : nullptr)
		{
			if (GS->GetContender(0) == PS)      { Slot = 0; }
			else if (GS->GetContender(1) == PS) { Slot = 1; }
		}
	}

	Super::Logout(Exiting);

	AArenaGameState* GS = GetArenaGameState();
	if (!GS || Slot == INDEX_NONE)
	{
		return;   // 走的是个不参赛的连接，没什么要收的
	}

	GS->SetContender(Slot, nullptr);

	if (GS->GetArenaPhase() != EArenaPhase::MatchEnd)
	{
		// 人走了这场就打不下去了。判剩下那个赢 —— 判平局（nullptr）会在界面上
		// 显示"平局"，但实际是有人掉线，这两件事不是一回事。
		EndMatch(GS->GetContender(1 - Slot));
	}
}

AActor* AArenaGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	const AArenaPlayerState* PS = Player ? Player->GetPlayerState<AArenaPlayerState>() : nullptr;
	const int32 Slot = FindContenderSlotOf(PS);

	if (Slot != INDEX_NONE)
	{
		if (AActor* Spawn = GetContenderSpawnPoint(Slot))
		{
			return Spawn;
		}
	}

	// 还没分到参赛位（比如第一个人第一次出生的时候）就交给引擎自己挑，
	// 反正 StartRound 会把两个人挪到各自那一侧的。
	//
	// 【⚠️ 为什么不能直接用它挑的结果】引擎那边是按"有没有被占用"随便挑一个 PlayerStart，
	// 而【备战点也是 PlayerStart】—— 备战点之所以用 PlayerStart + 标签，就是为了不多一个
	// 资产类型。于是它会挑中角落里那块备战台子：玩家先在备战台上等，直到 StartRound 挪人。
	//
	// 【为什么这条兜底是活的，不是死代码】真人的 RestartPlayer 跑在 Super::PostLogin
	// 里面（PostLogin → HandleStartingNewPlayer → RestartPlayer），而登记参赛位那一句
	// 在 Super::PostLogin【之后】—— 所以真人第一次出生时 FindContenderSlotOf 必然返回
	// INDEX_NONE，每次都走这条路。
	AActor* Chosen = Super::ChoosePlayerStart_Implementation(Player);
	if (IsPrepSpawnActor(Chosen))
	{
		// 只在引擎挑中备战点时才改：它挑中的本来就是战斗出生点的话，
		// 那一套"挑没人占的"逻辑此刻是对的（两个人都还没定位置），不要多事。
		if (AActor* CombatSpawn = GetContenderSpawnPoint(0))
		{
			return CombatSpawn;
		}
	}

	return Chosen;
}

// ---------------------------------------------------------------------------
// 大场 / 回合流程
// ---------------------------------------------------------------------------

void AArenaGameMode::StartMatch()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return;
	}

	// 幂等：两个真人在同一个窗口里先后登录时会各调一次。
	if (GS->GetArenaPhase() != EArenaPhase::WaitingToStart)
	{
		return;
	}

	// 等级不在这里设 —— StartRound 会按"第 1 回合"算出来（需求：初始 3 级）。
	//
	// 【回合计划在这里推】开局算一次，之后回合表不会变。放在 StartRound 里的话
	// 每回合都要重算重复制一遍，而它的内容根本不依赖"现在打到哪了"。
	PushRoundPlanToGameState();

	// 重随次数整场共享：开局给两边各配一份（配置在 RerollCharges，0 = 关闭重随）。
	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		if (AArenaPlayerState* PS = GS->GetContender(Slot))
		{
			PS->InitRerolls(RerollCharges);
		}
	}

	StartRound(1);
}

void AArenaGameMode::StartRound(int32 RoundNumber)
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS || GS->GetArenaPhase() == EArenaPhase::MatchEnd)
	{
		// MatchEnd 是终态。有一条路会走到这儿：结算里判完大场结束，
		// 但那个定时器已经排上了（理论上不会，因为 EndMatch 会清掉它）。
		return;
	}

	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		ResetContenderForRound(GS->GetContender(Slot), Slot, RoundNumber);
	}

	// 上面那一圈已经把人送到备战区了，木桩跟着摆到各自出生点前面。
	// 放在这儿（而不是 BeginPlay）是因为备战区的位置来自关卡里的备战点，
	// 每一回合都要重新确认一遍 —— 木桩是备战阶段的消耗品，不是常驻场景物件。
	SpawnTrainingDummies();

	// 先复位人，再切相位 —— 反过来的话客户端会先看到"第 N 回合备战"
	// 然后才看到自己血回满了，中间那一帧 UI 上是满血但角色还是死的。
	//
	// 【备战阶段带倒计时】倒计时走完还没选完的由 ForceFinishPreparation 强制推进
	//（开关在 bForceStartWhenPrepExpires）。倒计时的"剩余秒数"由 GameState 的
	// PhaseEndServerTime 复制下去，客户端自己减 —— 和结算阶段的机制是同一个。
	GS->SetArenaPhase(EArenaPhase::RewardSelection, RoundNumber, FMath::Max(0.f, PrepPhaseSeconds));

	// 【备战阶段无敌】挂上 State.Invulnerable，开战（BeginCombat）时摘。
	// 判据在 ExecCalc_Damage（伤害的唯一落点）—— 一处挡住所有伤害来源，
	// 不需要每个技能 / 每条伤害路各自判相位。伤害执行只在服务端跑，loose tag 不复制没问题。
	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		if (const AArenaPlayerState* PS = GS->GetContender(Slot))
		{
			if (UAbilitySystemComponent* ASC = PS->GetAbilitySystemComponent())
			{
				ASC->AddLooseGameplayTag(LOLGameplayTags::State_Invulnerable);
			}
		}
	}

	if (HasPrepCountdown())
	{
		GetWorldTimerManager().SetTimer(PrepForceTimerHandle, this,
			&AArenaGameMode::ForceFinishPreparation, PrepPhaseSeconds, /*bLoop=*/false);
	}

	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		BeginRewardSelection(GS->GetContender(Slot), RoundNumber);
	}

	// 兜底：万一两个人都没有待选（比如没能发出任何一份菜单），
	// 就别让他们卡在一个什么都不用点的备战阶段里。
	//
	// 【默认配置下这行不再开战】它走默认的 bPrepCountdownExpired=false，
	// 而倒计时开着的时候那道闸门会拦住它 —— 这时候"没东西可选"并不等于
	// "该开打了"：备战阶段本来就是让你在备战区走动的，木桩也在那儿。
	// 真正开战的是倒计时（下面排的那个 PrepForceTimerHandle）。
	// 只有 bForceStartWhenPrepExpires=false（没有倒计时可等）时它才会生效。
	TryBeginCombat();
}

void AArenaGameMode::ForceFinishPreparation()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS || GS->GetArenaPhase() != EArenaPhase::RewardSelection)
	{
		// 相位已经走了（双方都提前点完了「进入战斗」/ 大场结束）。
		// 定时器只是晚了到，不是异常。
		return;
	}

	// ---- 运行态诊断 ---------------------------------------------------------
	//
	// 「超时自动补发只是偶尔生效」这类问题，靠读代码是定位不到的：整条链路
	// （排定时器 → 到点 → 看相位 → 逐个代选 → TryBeginCombat）每一个环节都能
	// 单独失败，而且都长得像同一个症状（没反应 / 卡在备战）。所以先把入口的
	// 实际状态打全：相位对不对、定时器还在不在、两个人各自选完没有、还有几份待选。
	// 下次再复现，这一行日志就能指到具体是哪一环。
	{
		// 相位不用打名字：能走到这儿，上面那条 `!= RewardSelection` 就已经把它筛掉了。
		// 值得打的是「还剩多少」—— 它是客户端倒计时读的同一个数（PhaseEndServerTime），
		// 到点那一瞬间它还剩多少，能直接看出是早到了还是晚到了。
		FString Snapshot;
		Snapshot += FString::Printf(TEXT("phase=RewardSelection round=%d remain=%.1fs"),
			GS->GetRoundNumber(), GS->GetPhaseRemainingSeconds());
		for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
		{
			const AArenaPlayerState* PS = GS->GetContender(Slot);
			if (!IsValid(PS))
			{
				// 缺席的槽位必须说出来：ForceFinishPreparation 和 TryBeginCombat 都是
				// 「有人没选完就 return」，槽位为空会让这两条路径一起沉默地卡住。
				Snapshot += FString::Printf(TEXT(" | slot%d=<空>"), Slot);
				continue;
			}
			Snapshot += FString::Printf(TEXT(" | slot%d %s done=%s pending=%d"),
				Slot, *PS->GetName(),
				PS->IsRewardSelectionDone() ? TEXT("y") : TEXT("n"),
				PS->GetPendingPrompt().bActive ? PS->GetPendingPrompt().Options.Num() : 0);
		}
		UE_LOG(LogTemp, Log, TEXT("[Arena] 备战倒计时到点 —— %s"), *Snapshot);
	}

	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		AArenaPlayerState* PS = GS->GetContender(Slot);
		if (!IsValid(PS))
		{
			// 槽位缺席：這個人既没被代选、也不会让 TryBeginCombat 通过，只会把备战卡死。
			// 直接判放弃收掉他这一回合的待选（下面 TryBeginCombat 才能往前走）。
			UE_LOG(LogTemp, Warning,
				TEXT("[Arena] 备战倒计时到点，slot%d 的参赛者不存在 —— 按放弃处理，收掉待选。"), Slot);
			continue;
		}

		if (PS->IsRewardSelectionDone())
		{
			continue;
		}

		// 到点没选 = 系统替他随机选（超时白放弃一整回合奖励太亏）。
		// 沿链一路代选下去直到 ResetRewardSelection（进入战斗）；锻造器菜单里
		// 随机到「进入战斗」也算合法出口。
		//
		// ⚠️ 代选必须传 bAllowBeginCombat=false：这里要在【一个函数里依次替两个人
		// 各走几步】，中间任何一步开战，另一个人都会被拖着进战斗 —— 他这一回合的
		// 奖励直接没了，下一回合还是从一个"带着未选状态进战斗"的人开始的。
		for (int32 Guard = 0; Guard < 16 && !PS->IsRewardSelectionDone(); ++Guard)
		{
			const FArenaPendingPrompt& Prompt = PS->GetPendingPrompt();
			if (!Prompt.bActive)
			{
				break;
			}

			// 只在合法选项里随机 —— 随机到一个空载荷会被 ResolveChoice 拒掉并记 Warning。
			TArray<int32> ValidIndices;
			ValidIndices.Reserve(Prompt.Options.Num());
			for (int32 OptionIndex = 0; OptionIndex < Prompt.Options.Num(); ++OptionIndex)
			{
				if (Prompt.Options[OptionIndex].IsValidOption())
				{
					ValidIndices.Add(OptionIndex);
				}
			}
			if (ValidIndices.Num() <= 0)
			{
				break;
			}

			const int32 Pick = ValidIndices[FMath::RandRange(0, ValidIndices.Num() - 1)];
			const int32 StepBefore = static_cast<int32>(PS->GetRewardStep());
			UE_LOG(LogTemp, Log,
				TEXT("[Arena] 备战倒计时走完，%s 未选择 —— 随机代选选项 %d（共 %d 项）。"),
				*PS->GetName(), Pick, Prompt.Options.Num());
			ResolveChoice(PS, Pick, /*bAllowBeginCombat=*/false);

			// 代选之后必须真的往前走了一步。ResolveChoice 对被拒的选项只记 Warning
			// 就返回，什么都不改 —— 循环会再随机同样的死局，16 次转完，
			// 人还是没选完，然后开战永远等不到他。
			const bool bAdvanced = PS->IsRewardSelectionDone()
				|| static_cast<int32>(PS->GetRewardStep()) != StepBefore
				|| PS->GetPendingPrompt().bActive != Prompt.bActive;
			if (!bAdvanced && Guard + 1 >= 4)
			{
				UE_LOG(LogTemp, Warning,
					TEXT("[Arena] 备战倒计时代选第 %d 次仍未推进（%s）—— 当前待选里的选项都拿不到东西，"
						"放弃这一份待选，直接进战斗。"),
					Guard + 1, *PS->GetName());
				break;
			}
		}

		// 兜底判据用【这个人到底选完没有】，而不是"这次有没有代选成功"。
		// 原来是 bPickedAnything：它只要进过循环就是 true，
		// 于是「代选了 16 次但一步没推进」这条路径既不 ResetRewardSelection、
		// 又过不了 TryBeginCombat —— 备战阶段就永久卡住了。
		if (!PS->IsRewardSelectionDone())
		{
			UE_LOG(LogTemp, Log,
				TEXT("[Arena] 备战倒计时走完，%s 仍未选完 —— 按放弃处理（收掉待选），直接开战。"),
				*PS->GetName());
			PS->ResetRewardSelection();
		}
	}

	// 这是【倒计时到点】那一路，所以传 true —— 它是唯一被允许开战的入口。
	// 其余三处（选完 / 空锻造器菜单 / StartRound 兜底）都走默认的 false。
	TryBeginCombat(/*bPrepCountdownExpired=*/true);
}

void AArenaGameMode::SpawnTrainingDummies()
{
	// 先清后摆：换回合时上一批还在（正常路径下 BeginCombat 已经清过一次，
	// 但那条路在"备战阶段直接结束比赛"之类的分支上不一定走得到）。不清的话
	// 每回合会多留一个木桩在场上，几回合之后备战区站满了打不死的靶子。
	ClearTrainingDummies();

	if (!bSpawnTrainingDummies || !TrainingDummyClass)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 每个备战出生点前面摆一个 —— 两边对称，谁都不用跑去找靶子。
	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		const AActor* PrepSpawn = GetPrepSpawnPoint(Slot);
		if (!PrepSpawn)
		{
			// 关卡里没摆备战点时走到这儿。这不是错误：没有木桩不影响比赛流程，
			// 所以只跳过，不报错。
			continue;
		}

		// 偏移跟着出生点的朝向走，这样"正前方 1.5 米"在备战区旋转过之后依然成立。
		// 【这个数为什么不能大】备战点立在四角那块半径 300 的扇形台子上，木桩再往外
		// 就出了弧线；而它是 MOVE_None，掉不下去，会悬在半空。见头文件里那段推导。
		const FTransform PrepTransform = PrepSpawn->GetActorTransform();
		const FVector SpawnLocation = PrepTransform.TransformPosition(TrainingDummySpawnOffset);

		// 转身 180 度：木桩站在玩家正前方，脸要朝着玩家，否则玩家看到的是它的后脑勺。
		const FRotator SpawnRotation(0.f, PrepTransform.Rotator().Yaw + 180.f, 0.f);

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Owner = this;

		if (AActor* Dummy = World->SpawnActor<AActor>(
			TrainingDummyClass.Get(), SpawnLocation, SpawnRotation, SpawnParams))
		{
			TrainingDummies.Add(Dummy);

			// 记一条。木桩是【运行时生成】的 —— 编辑器视口里根本不存在这个东西，
			// 所以"到底摆出来没有、摆在哪"只能从日志回答。看不到木桩时先看这一行：
			// 位置不对 = 备战点摆错了；没有这一行 = 类没配、或者那个备战位缺备战点。
			UE_LOG(LogTemp, Log, TEXT("[Arena] %d 号备战位的木桩：%s @ %s"),
				Slot, *Dummy->GetName(), *SpawnLocation.ToCompactString());
		}
	}
}

void AArenaGameMode::ClearTrainingDummies()
{
	for (AActor* Dummy : TrainingDummies)
	{
		if (IsValid(Dummy))
		{
			Dummy->Destroy();
		}
	}

	// 用 Reset 而不是 Empty：留着那份已经扩容的缓冲，下一回合直接复用。
	TrainingDummies.Reset();
}

bool AArenaGameMode::HasPrepCountdown() const
{
	// 和 StartRound 里排 PrepForceTimerHandle 的条件是同一个 —— 有意共用一个谓词，
	// 理由见声明处。PrepPhaseSeconds <= 0 时没有倒计时可等，闸门必须自动失效，
	// 否则战斗永远开不了。
	return bForceStartWhenPrepExpires && PrepPhaseSeconds > 0.f;
}

void AArenaGameMode::TryBeginCombat(bool bPrepCountdownExpired)
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS || GS->GetArenaPhase() != EArenaPhase::RewardSelection)
	{
		return;
	}

	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		const AArenaPlayerState* PS = GS->GetContender(Slot);
		if (!IsValid(PS) || !PS->IsRewardSelectionDone())
		{
			// 还有人在选。他选完那次会再调进来。
			return;
		}
	}

	// 备战倒计时还在跑 → 不许提前开战。
	//
	// 【为什么这道闸门在这里，而不是在各个调用点】"什么时候可以开战"是一条策略，
	// 而调用点有四处（选完最后一份奖励 / 锻造器空菜单 / StartRound 兜底 /
	// 倒计时到点）。在四处各写一遍的话，漏一处就是"某条路径仍然会提前开战"——
	// 而且那条路径平时不一定走得到，很难发现。收在这里只有一处要维护。
	//
	// bForceStartWhenPrepExpires = false 时没有倒计时可等，闸门自动失效，
	// 退回"所有人选完就开"的老行为（那正是这个开关文档里写的语义）。
	if (!bPrepCountdownExpired && HasPrepCountdown())
	{
		return;
	}

	BeginCombat();
}

void AArenaGameMode::BeginCombat()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(PrepForceTimerHandle);

	// 木桩只在备战阶段存在。留到战斗里的话，普攻的近战扫描会把它们当成合法目标
	// 扫进来 —— 表现是"战斗打着打着偶尔少一下伤害"，极难查。见 bSpawnTrainingDummies。
	ClearTrainingDummies();

	bWarnedMissingPawnThisCombat = false;

	// 观察记录按回合清零。上回合的"见过 Pawn"不能带到这一回合 ——
	// 否则这一回合开局那一瞬的空 Pawn 会被当成"打没了"直接判负。
	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		bPawnSeenThisCombat[Slot] = false;
		PawnMissingSinceSeconds[Slot] = 0.0;
	}

	GS->SetArenaPhase(EArenaPhase::Combat, GS->GetRoundNumber(), 0.f);

	// 备战区 → 战斗场。双方必须先于相位广播之后立刻归位：
	// 客户端看到"战斗中"的同一刻人已经在自己的战斗位上，不会出现
	// "开始打了人还在备战区"的那一帧。
	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		if (AArenaPlayerState* PS = GS->GetContender(Slot))
		{
			if (IsValid(PS))
			{
				TeleportPawnToActor(PS->GetPawn(), GetContenderSpawnPoint(Slot));

				// 摘掉备战阶段的无敌（StartRound 里挂的）。漏摘的表现是整回合打不出伤害，
				// 所以和传送放在同一条路上，不单独排定时器。
				if (UMyAbilitySystemComponent* ASC = PS->GetMyAbilitySystemComponent())
				{
					ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_Invulnerable);

					// --- 把备战阶段残留的攻击动作收干净 ---
					//
					// 【症状】连段段号 StageIndex 是【能力实例上的】状态，只有 EndAbility 才会清零
					//（见 UGA_ThreeHitPassive::EndAbility）。备战阶段对着木桩打两下、不等连段走完
					// 就开战的话，这一回合的第一刀会从第 2/3 段起手；而且手上可能还挂着蓄力光，
					// 世界的顿帧也未必还原完了 —— 都是"上一阶段的东西带进了战斗"。
					//
					// 【为什么取消能力就够了】EndAbility 把这一整套一起收了：段号、蓄力粒子、
					// 完美窗口特效、顿帧、输入监听任务。不需要在这里逐个手动还原
					//（逐个还原的话，将来能力里再加一样残留就会漏）。
					//
					// 【为什么点名这三个槽位，而不是一把全取消】它们就是"连段"的载体
					//（持剑三连 / 空手四连 / 空中攻击）。把 Ability.Slot.* 全取消会连格挡、
					// 闪避、Q/W/E/R 一起掐掉 —— 那是在改玩法，不是在清残留。
					//
					// 【为什么用句柄而不是标签】5.8 的 ASC 上【没有】CancelAbilitiesWithTag
					//（只有 CancelAbilities / CancelAbilityHandle / CancelAbility），
					// 而"槽位 → 能力"的映射本来就由 UMyAbilitySystemComponent::GetHandleForSlot
					// 提供，顺路用现成的。句柄无效（槽位没授权）就直接跳过。
					const FGameplayTag AttackSlotTags[] =
					{
						LOLGameplayTags::Ability_Slot_Passive,
						LOLGameplayTags::Ability_Slot_Combo,
						LOLGameplayTags::Ability_Slot_AirAttack,
					};
					for (const FGameplayTag& SlotTag : AttackSlotTags)
					{
						if (const FGameplayAbilitySpecHandle Handle = ASC->GetHandleForSlot(SlotTag);
							Handle.IsValid())
						{
							ASC->CancelAbilityHandle(Handle);
						}
					}
				}
			}
		}
	}

	// 【为什么战斗阶段要轮询而不是等事件】这个项目的死亡判定是 ASC 上的 State.Dead 标签，
	// 没有任何对外的死亡委托。为它加一个委托要改 AHeroCombatCharacter（那是在跑的东西），
	// 而这里只是每 0.1 秒问两个人一句 IsDead() —— 代价接近零，
	// 顺带还把"Pawn 不见了"这种情况一起覆盖了。
	GetWorldTimerManager().SetTimer(CombatPollHandle, this, &AArenaGameMode::PollCombatOutcome,
		FMath::Max(0.02f, CombatPollInterval), /*bLoop=*/true);
}

void AArenaGameMode::PollCombatOutcome()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS || GS->GetArenaPhase() != EArenaPhase::Combat)
	{
		// 相位已经换了（有人掉线导致直接结束之类）。轮询该停了。
		GetWorldTimerManager().ClearTimer(CombatPollHandle);
		return;
	}

	AArenaPlayerState* Dead = nullptr;

	// Pawn 消失的判定也要用时间，所以先取一次。
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;

	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		AArenaPlayerState* PS = GS->GetContender(Slot);
		if (!IsValid(PS))
		{
			continue;
		}

		AHeroCombatCharacter* Hero = Cast<AHeroCombatCharacter>(PS->GetPawn());
		if (!Hero)
		{
			// 【Pawn 没了必须判负，不能只警告】掉出世界的 Pawn 会在 KillZ 平面被引擎
			// 销毁，GetPawn() 从此一直是空。以前这里只打一条警告就跳过，结果是
			// "活着一方永远赢不了"：回合不结算，整局卡死在战斗相位（实测过一局）。
			if (!bPawnSeenThisCombat[Slot])
			{
				// 本回合它还没出现过（回合刚开始的一瞬），判了会判错人。
				if (!bWarnedMissingPawnThisCombat)
				{
					// 只报一次：这个函数 0.1 秒跑一次，不设标志会把日志刷满。
					bWarnedMissingPawnThisCombat = true;
					UE_LOG(LogTemp, Warning,
						TEXT("[Arena] 战斗阶段里 %s 还没有 Pawn（多半是回合刚开始）—— 暂不判负。"),
						*PS->GetName());
				}
				continue;
			}

			if (PawnMissingSinceSeconds[Slot] <= 0.0)
			{
				// 刚开始消失，起表。
				PawnMissingSinceSeconds[Slot] = Now;
			}
			else if (Now - PawnMissingSinceSeconds[Slot] >= MissingPawnGraceSeconds)
			{
				// 宽限过了还没回来。掉出世界的 Pawn 是被销毁的，不会回来。
				// 两个同时消失就按先找到的那个判负（理由同下面的同时倒下）。
				if (!Dead)
				{
					UE_LOG(LogTemp, Warning,
						TEXT("[Arena] %s 的 Pawn 消失超过 %.2f 秒（掉出世界？）—— 判这一方负。"),
						*PS->GetName(), MissingPawnGraceSeconds);
					Dead = PS;
				}
			}
			continue;
		}

		// 回来了就把表清掉：重生 / 换 Pawn 的瞬时状态不该被累积成判负。
		PawnMissingSinceSeconds[Slot] = 0.0;
		bPawnSeenThisCombat[Slot] = true;

		if (Hero->IsDead() && !Dead)
		{
			// 【两个同时倒下的情况】1V1 里几乎不会发生（伤害是顺序结算的），
			// 真发生了就按先找到的那个判负 —— 下标顺序是确定的，所以结果可复现，
			// 不会出现"同一局两次跑出不同的赢家"。
			Dead = PS;
		}
	}

	if (Dead)
	{
		BeginSettlement(Dead);
	}
}

void AArenaGameMode::BeginSettlement(AArenaPlayerState* Loser)
{
	GetWorldTimerManager().ClearTimer(CombatPollHandle);

	AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return;
	}

	const int32 RoundNumber = GS->GetRoundNumber();
	AArenaPlayerState* Winner = GS->GetOpponentOf(Loser);

	// 相位先切到结算：AI 在 Tick 里查相位，切了它就立刻停手；
	// 也是"结算期间再挨一下不会改写结果"的保证（结果在这一行之下的代码里定）。
	GS->SetArenaPhase(EArenaPhase::Settlement, RoundNumber, SettlementSeconds);

	const int32 HpLoss = GetHpLossForRound(RoundNumber);

	if (IsValid(Loser))
	{
		Loser->RecordRoundResult(/*bWon=*/false);
		Loser->ApplyMatchDamage(HpLoss);
	}

	if (IsValid(Winner))
	{
		Winner->RecordRoundResult(/*bWon=*/true);
	}

	// --- 记上一回合的结果（购物阶段顶部的 √/× + 扣血显示）---
	//
	// 记下标而不是记文本：本地玩家在哪一号位取决于看的是哪台机器，
	// 翻译层拿下标和自己一比，才能对两端各自算出"你赢了/你输了"。
	// Winner 可能为空（这回合判负来自"Pawn 消失"那类异常路径）—— 空就记
	// INDEX_NONE，UI 拿到就收起这一条显示。
	GS->SetLastRoundResult(IsValid(Winner) ? FindContenderSlotOf(Winner) : INDEX_NONE, HpLoss);

	// --- 判大场是否结束（需求：一方 HP <= 0；或达到首次大场阈值）---

	if (IsValid(Loser) && Loser->GetMatchHealth() <= 0)
	{
		EndMatch(Winner);
		return;
	}

	// 阈值判在【记完这一回合之后】。需求里"6 回合全胜"必须是打满 6 回合才算，
	// 所以这个函数内部还有一道 min_rounds 的检查（见 HasReachedFirstThreshold）。
	// 两边都判：全胜和全败是同一场比赛的两个说法，都写出来才对得上需求原文。
	if ((IsValid(Winner) && HasReachedFirstThreshold(Winner)) ||
		(IsValid(Loser) && HasReachedFirstThreshold(Loser)))
	{
		EndMatch(Winner);
		return;
	}

	// --- 还有下一回合 ---
	const int32 NextRound = RoundNumber + 1;
	GetWorldTimerManager().SetTimer(SettlementTimerHandle,
		FTimerDelegate::CreateUObject(this, &AArenaGameMode::StartRound, NextRound),
		FMath::Max(0.1f, SettlementSeconds), /*bLoop=*/false);
}

void AArenaGameMode::EndMatch(AArenaPlayerState* Winner)
{
	GetWorldTimerManager().ClearTimer(CombatPollHandle);
	GetWorldTimerManager().ClearTimer(SettlementTimerHandle);
	GetWorldTimerManager().ClearTimer(PrepForceTimerHandle);
	GetWorldTimerManager().ClearTimer(BotSpawnTimerHandle);

	AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return;
	}

	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		if (AArenaPlayerState* PS = GS->GetContender(Slot))
		{
			// 比赛都结束了，还挂着一份三选一界面会一直盖在结算界面上，
			// 而且点下去还会真的发东西。
			PS->ResetRewardSelection();
		}
	}

	GS->SetWinner(Winner);
	GS->SetArenaPhase(EArenaPhase::MatchEnd, GS->GetRoundNumber(), 0.f);

	UE_LOG(LogTemp, Log, TEXT("[Arena] 大场结束：%s 获胜。"),
		Winner ? *Winner->GetName() : TEXT("<无人（对手掉线或没有对手）>"));
}

// ---------------------------------------------------------------------------
// 奖励：往 PlayerState 上放一份待选
// ---------------------------------------------------------------------------

void AArenaGameMode::BeginRewardSelection(AArenaPlayerState* PS, int32 RoundNumber)
{
	if (!IsValid(PS))
	{
		return;
	}

	const FArenaRoundReward Reward = GetRoundReward(RoundNumber);

	switch (Reward.Kind)
	{
	case EArenaRewardKind::BranchChoice:
		PresentBranchChoice(PS, Reward);
		return;

	case EArenaRewardKind::Augment:
		PresentAugmentOffer(PS, Reward.AugmentTier);
		return;

	case EArenaRewardKind::FixedForge:
		// 固定发放不用玩家做决定，直接进背包，然后给菜单。
		if (Reward.FixedGrant.Count <= 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[Arena] 第 %d 回合是固定发放，但 FixedGrant.Count 是 %d —— 这一回合什么都没发。"),
				RoundNumber, Reward.FixedGrant.Count);
			break;
		}

		if (UArenaLoadoutComponent* Loadout = PS->GetLoadout())
		{
			Loadout->AddForgeCharges(Reward.FixedGrant.Tier, Reward.FixedGrant.Count);
		}
		break;

	case EArenaRewardKind::None:
	default:
		// 需求里第 6 回合就是这一档。什么都不发是【对的】，
		// 但锻造器菜单照样要给 —— 攒着的次数只能在奖励阶段花。
		break;
	}

	EnterForgeMenu(PS);
}

// ---------------------------------------------------------------------------
// 锻造器（stat anvil）分支的图标。
//
// 【为什么按次数挑图】奖励卡上「锻造器 ×3」和「锻造器 ×5」是两件事，玩家得一眼看出攒的次数
// 不一样。Riot 的官方图就是靠"图上数 stat 符文"来区分次数的：
//     ×3 = statsonstats        （三个符文）
//     ×5 = statsonstatsonstats （五个符文）
// 所以这里直接把 Count 翻译成对应的图。
//
// 【为什么只有这两张】图源在项目根的 ArtRefs/ArenaAugments/ 下，只有 _large / _small 两档，
// 没有 ×N 之外的官方 variant；拿不到更细的素材时退回 ×3 那张（也就是"默认用 large"）。
//
// 【为什么路径写死在这】这两张图目前只有这一处消费。等哪天要在编辑器里给分支配图标，
// 应该把字段加进 FArenaBranchOption（现在那个结构体还没有 Icon），而不是继续在这里加表。
// ---------------------------------------------------------------------------
namespace
{
	static const TCHAR* const kForgeIcon_5 =
		TEXT("/Game/LOL/UI/Arena/Textures/AugmentIcons/statsonstatsonstats_large");
	static const TCHAR* const kForgeIcon_3 =
		TEXT("/Game/LOL/UI/Arena/Textures/AugmentIcons/statsonstats_large");

	TSoftObjectPtr<UTexture2D> ArenaForgeIconForCount(int32 Count)
	{
		if (Count >= 5)
		{
			return TSoftObjectPtr<UTexture2D>(FSoftObjectPath(kForgeIcon_5));
		}
		return TSoftObjectPtr<UTexture2D>(FSoftObjectPath(kForgeIcon_3));
	}
}   // namespace

void AArenaGameMode::PresentBranchChoice(AArenaPlayerState* PS, const FArenaRoundReward& Reward)
{
	if (Reward.Branches.Num() <= 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 第 %d 回合配的是二选一分支，但 Branches 是空的 —— 跳过这一回合的奖励。"),
			GetArenaGameState() ? GetArenaGameState()->GetRoundNumber() : 0);
		EnterForgeMenu(PS);
		return;
	}

	FArenaPendingPrompt Prompt;
	Prompt.bActive = true;
	Prompt.bFromRoundReward = true;
	Prompt.Title = LOCTEXT("BranchTitle", "选择你的奖励");

	for (const FArenaBranchOption& Branch : Reward.Branches)
	{
		if (Branch.Count <= 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[Arena] 分支选项的 Count 是 %d（应该 >= 1）—— 这一项被跳过了。"), Branch.Count);
			continue;
		}

		const FText TierName = ArenaTierDisplayName(Branch.Tier);

		FArenaChoiceOption Option;
		Option.Action = EArenaPromptAction::Grant;
		Option.Branch = Branch;

		// 分支选项也带品质上卡（传说→金、棱彩→棱彩）。
		Option.Tier = ArenaItemTierToAugmentTier(Branch.Tier);

		if (Branch.Branch == EArenaRewardBranch::Item)
		{
			// 装备分支的图标要等选完之后真抽到装备才拿得到（图标挂在 UArenaItemData::Icon 上），
			// 这张卡先不占位 —— 比挂一张张冠李戴的图强。
			Option.Label = FText::Format(LOCTEXT("BranchItem", "{0}装备 ×{1}"), TierName, FText::AsNumber(Branch.Count));
			Option.Description = LOCTEXT("BranchItemDesc", "立刻随机三选一，选中的直接进装备栏。");
		}
		else
		{
			Option.Label = FText::Format(LOCTEXT("BranchForge", "{0}锻造器 ×{1}"), TierName, FText::AsNumber(Branch.Count));
			Option.Description = LOCTEXT("BranchForgeDesc", "攒下使用次数，每次使用时随机获得属性加成（stat anvil）。");
			// 锻造器没有对应的装备定义，图标只能按攒的次数现算（见上面 ArenaForgeIconForCount）。
			Option.Icon = ArenaForgeIconForCount(Branch.Count);
		}

		Prompt.Options.Add(Option);
	}

	if (Prompt.Options.Num() <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 二选一分支一个可用选项都没配出来 —— 跳过这一回合的奖励。"));
		EnterForgeMenu(PS);
		return;
	}

	PS->SetRewardStep(EArenaRewardStep::RoundReward);
	PS->SetPendingPrompt(Prompt);
}

void AArenaGameMode::PresentAugmentOffer(AArenaPlayerState* PS, EArenaAugmentTier Tier)
{
	UArenaLoadoutComponent* Loadout = PS ? PS->GetLoadout() : nullptr;
	if (!RewardPool || !Loadout)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 海克斯回合但 RewardPool 为空（在 BP_ArenaGameMode 上配）—— 跳过这一回合的奖励。"));
		EnterForgeMenu(PS);
		return;
	}

	TArray<UArenaAugmentData*> Rolled;
	RewardPool->RollAugments(Tier, OfferCount, Loadout->GetEquippedAugmentList(), Rolled);

	if (Rolled.Num() <= 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 品质的海克斯池里抽不出东西（该品质没配 / 池子为空 / 都已经被这个人拿过了）—— "
				 "这一回合只发锻造器菜单。"),
			*UEnum::GetValueAsString(Tier));
		EnterForgeMenu(PS);
		return;
	}

	FArenaPendingPrompt Prompt;
	Prompt.bActive = true;
	Prompt.bFromRoundReward = true;
	Prompt.Title = LOCTEXT("AugmentTitle", "选择一个海克斯");

	// 【循环变量不能加 const】TSoftObjectPtr<T> 从 const T* 赋值走的是引擎里那个
	// 已弃用的"指针类型不匹配"重载（C4996），下一代引擎会直接编译不过。
	// 从 T* 赋值才是正常路径。
	for (UArenaAugmentData* Augment : Rolled)
	{
		FArenaChoiceOption Option;
		Option.Action = EArenaPromptAction::Grant;
		Option.Augment = Augment;
		Option.Label = ArenaAugmentLabel(Augment);
		Option.Description = Augment ? Augment->Description : FText::GetEmpty();
		Option.Icon = Augment ? Augment->Icon : TSoftObjectPtr<UTexture2D>();
		// 品质跟着资产走（服务端填成品，客户端不做解析 —— 见 FArenaChoiceOption::Tier）。
		Option.Tier = Augment ? Augment->Tier : EArenaAugmentTier::Silver;
		Prompt.Options.Add(Option);
	}

	// 海克斯三选一可以重随（对齐 LoL：银/金/棱彩选项可换，stat anvil 不可）。
	// 档位也在这里留住 —— 重随时要按原档重抽（见 RequestReroll）。
	PS->SetPendingAugmentTier(Tier);
	Prompt.bRerollable = true;
	Prompt.RerollsLeft = PS->GetRerollsLeft();

	PS->SetRewardStep(EArenaRewardStep::RoundReward);
	PS->SetPendingPrompt(Prompt);
}

void AArenaGameMode::PresentItemOffer(AArenaPlayerState* PS, EArenaItemTier Tier, bool bFromRoundReward)
{
	UArenaLoadoutComponent* Loadout = PS ? PS->GetLoadout() : nullptr;
	if (!RewardPool || !Loadout)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 要给装备三选一但 RewardPool 为空（在 BP_ArenaGameMode 上配）—— 跳过这次。"));
		EnterForgeMenu(PS);
		return;
	}

	TArray<UArenaItemData*> Rolled;
	// 排除掉身上已经装着的唯一装备 —— 不然"传说装备三选一"里可能出现一件已经带着的。
	RewardPool->RollItems(Tier, OfferCount, Loadout->GetEquippedUniqueItems(), Rolled);

	if (Rolled.Num() <= 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 品质的池子里抽不出装备（池子为空 / 该品质没配 / 唯一装备都拿过了）—— "
				 "这一次只发锻造器菜单。"),
			*UEnum::GetValueAsString(Tier));
		EnterForgeMenu(PS);
		return;
	}

	FArenaPendingPrompt Prompt;
	Prompt.bActive = true;
	Prompt.bFromRoundReward = bFromRoundReward;
	Prompt.Title = bFromRoundReward
		? LOCTEXT("ItemTitleReward", "选择一件装备")
		: LOCTEXT("ItemTitleForge", "锻造器：三选一");

	// 同上：循环变量不加 const，否则 TSoftObjectPtr 的赋值会走已弃用的重载。
	for (UArenaItemData* Item : Rolled)
	{
		FArenaChoiceOption Option;
		Option.Action = EArenaPromptAction::Grant;
		Option.Item = Item;
		Option.Tier = ArenaItemTierToAugmentTier(Tier);
		Option.Label = ArenaItemLabel(Item);
		Option.Description = Item ? Item->Description : FText::GetEmpty();
		Option.Icon = Item ? Item->Icon : TSoftObjectPtr<UTexture2D>();
		Prompt.Options.Add(Option);
	}

	// 记住这一档，因为"还要再抽几次"的时候选项已经发出去了，品质只存在这里。
	PS->SetPendingItemTier(Tier);

	// 装备三选一同样可重随（回合发的和锻造器抽的都是）。
	Prompt.bRerollable = true;
	Prompt.RerollsLeft = PS->GetRerollsLeft();

	PS->SetRewardStep(EArenaRewardStep::ItemOffer);
	PS->SetPendingPrompt(Prompt);
}

void AArenaGameMode::EnterForgeMenu(AArenaPlayerState* PS)
{
	UArenaLoadoutComponent* Loadout = PS ? PS->GetLoadout() : nullptr;
	if (!Loadout)
	{
		// 连装备栏都没有：直接判这个人的奖励阶段结束，别把他卡住。
		PS->ResetRewardSelection();
		return;
	}

	FArenaPendingPrompt Prompt;
	Prompt.bActive = true;
	Prompt.bFromRoundReward = true;
	Prompt.Title = LOCTEXT("ForgeMenuTitle", "准备进入战斗");

	for (const FArenaForgeCharge& Charge : Loadout->GetForgeCharges())
	{
		if (Charge.Count <= 0)
		{
			continue;   // 花完了的那一档不画按钮
		}

		FArenaChoiceOption Option;
		Option.Action = EArenaPromptAction::UseForge;
		Option.Branch.Branch = EArenaRewardBranch::Forge;
		Option.Branch.Tier = Charge.Tier;
		Option.Branch.Count = Charge.Count;   // 结算时用它校验，UI 也可以拿它画次数
		Option.Label = FText::Format(LOCTEXT("UseForge", "使用{0}锻造器（剩 {1} 次）"),
			ArenaTierDisplayName(Charge.Tier), FText::AsNumber(Charge.Count));
		Option.Description = FText::Format(
			LOCTEXT("UseForgeDesc", "随机获得 {0} 条属性加成，不占装备栏、不可摘除。"),
			FText::AsNumber(StatsPerForgeUse));
		Prompt.Options.Add(Option);
	}

	// 「进入战斗」按钮已按需求移除：备战倒计时到点由 ForceFinishPreparation
	// 自动代选/收尾并开战。锻造器菜单是待选链的终点 —— 没有可选项时
	// 直接判完（见下面空菜单分支），不让他盯着空界面等倒计时。
	if (Prompt.Options.Num() <= 0)
	{
		// 没有锻造器次数 = 这一轮没有任何可选项。直接判完这个人 ——
		// 他现在是"已完成"状态，但仍要等备战倒计时到点才会真的开战
		// （TryBeginCombat 里那道闸门），所以这里不会把人提前传进战斗场。
		PS->ResetRewardSelection();
		TryBeginCombat();
		return;
	}

	PS->SetRewardStep(EArenaRewardStep::ForgeMenu);
	PS->SetPendingPrompt(Prompt);
}

// ---------------------------------------------------------------------------
// 选择结算
// ---------------------------------------------------------------------------

void AArenaGameMode::ResolveChoice(
	AArenaPlayerState* Chooser, int32 OptionIndex, bool bAllowBeginCombat)
{
	if (!IsValid(Chooser))
	{
		return;
	}

	const FArenaPendingPrompt& Prompt = Chooser->GetPendingPrompt();

	if (!Prompt.bActive)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 提交了选项 %d，但现在没有待选择 —— 已忽略（连点，或者界面还没被收回去）。"),
			*Chooser->GetName(), OptionIndex);
		return;
	}

	if (!Prompt.Options.IsValidIndex(OptionIndex))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 提交的选项下标 %d 越界（当前只有 %d 项）—— 已忽略。"),
			*Chooser->GetName(), OptionIndex, Prompt.Options.Num());
		return;
	}

	// 【拷贝一份再处理】下面几条路都会把这份待选换掉（发新提示 / 清空），
	// 拿着引用处理会在中途变成读一份已经被改写的结构。
	const FArenaChoiceOption Option = Prompt.Options[OptionIndex];

	if (!Option.IsValidOption())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 提交的选项 %d 没有有效载荷（Action=%d）—— 已忽略。这是服务端配错了，不是客户端作弊。"),
			*Chooser->GetName(), OptionIndex, static_cast<int32>(Option.Action));
		return;
	}

	switch (Option.Action)
	{
	case EArenaPromptAction::Finish:
		Chooser->ResetRewardSelection();
		// 代选路径传 false：详见 ResolveChoice 声明里 bAllowBeginCombat 那段 ——
		// 倒计时那一路要等两个人都补齐了再统一开战。
		if (bAllowBeginCombat)
		{
			TryBeginCombat();
		}
		return;

	case EArenaPromptAction::UseForge:
		HandleUseForge(Chooser, Option.Branch.Tier);
		return;

	case EArenaPromptAction::Grant:
		HandleGrant(Chooser, Option);
		return;
	}
}

void AArenaGameMode::RequestReroll(AArenaPlayerState* Chooser, int32 OptionIndex)
{
	if (!IsValid(Chooser))
	{
		return;
	}

	const FArenaPendingPrompt& Prompt = Chooser->GetPendingPrompt();

	if (!Prompt.bActive)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 请求重随 %d，但现在没有待选择 —— 已忽略。"),
			*Chooser->GetName(), OptionIndex);
		return;
	}

	// 不可重随的：锻造器菜单、二选一分支（它们的 bRerollable 是 false）。
	// 属性锻造器发的是"直接结算"，根本没有候选可换。
	if (!Prompt.bRerollable)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 请求重随 %d，但当前待选不可重随 —— 已忽略。"),
			*Chooser->GetName(), OptionIndex);
		return;
	}

	if (!Prompt.Options.IsValidIndex(OptionIndex))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 请求重随的下标 %d 越界（当前只有 %d 项）—— 已忽略。"),
			*Chooser->GetName(), OptionIndex, Prompt.Options.Num());
		return;
	}

	// 重抽只排除【当前待选里已有的其它项】—— 本来就没装到身上，
	// Loadout 的排除表覆盖不到它们，所以下面用重试对比兜住。
	UArenaLoadoutComponent* Loadout = Chooser->GetLoadout();
	if (!RewardPool || !Loadout)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 请求重随但 RewardPool / Loadout 缺失 —— 已忽略。"), *Chooser->GetName());
		return;
	}

	// 扣次数放在【抽签成功之后】（见下方 if (!bRerolled) 之后那一段）。
	// 顺序很重要：先抽签后扣，池子空了 / 全是重复项而抽不出新选项时，
	// 玩家不该白掉一次重随 —— 那是「服务端没能给出任何东西」，
	// 不是「玩家按了按钮」。UI 会同步收到 -1 次的通知，不需要额外回滚逻辑。
	if (Chooser->GetRerollsLeft() <= 0)
	{
		// UI 的按钮也会置灰，这里是服务端兜底（客户端可能因为漏事件而多发）。
		UE_LOG(LogTemp, Log,
			TEXT("[Arena] %s 想重随 %d，但重随次数已经用完。"), *Chooser->GetName(), OptionIndex);
		return;
	}

	FArenaPendingPrompt NewPrompt = Prompt;   // 拷贝一份再改（原引用马上要被覆盖）
	FArenaChoiceOption& Slot = NewPrompt.Options[OptionIndex];
	bool bRerolled = false;

	if (Chooser->GetRewardStep() == EArenaRewardStep::RoundReward)
	{
		// 海克斯三选一（分支走不到这里 —— 它的 bRerollable 是 false）。
		for (int32 Attempt = 0; Attempt < 4 && !bRerolled; ++Attempt)
		{
			TArray<UArenaAugmentData*> Rolled;
			RewardPool->RollAugments(Chooser->GetPendingAugmentTier(), 1,
				Loadout->GetEquippedAugmentList(), Rolled);
			if (Rolled.Num() <= 0 || !Rolled[0])
			{
				break;   // 池子空了：保留原选项（次数不扣，下面统一处理）
			}

			// 排除当前待选里已有的其它海克斯，避免"重随完两张一样的卡"。
			const TSoftObjectPtr<UArenaAugmentData> RolledRef(Rolled[0]);
			bool bDuplicate = false;
			for (int32 Index = 0; Index < NewPrompt.Options.Num(); ++Index)
			{
				if (Index != OptionIndex && NewPrompt.Options[Index].Augment == RolledRef)
				{
					bDuplicate = true;
					break;
				}
			}
			if (bDuplicate)
			{
				continue;
			}

			UArenaAugmentData* Augment = Rolled[0];
			Slot.Action = EArenaPromptAction::Grant;
			Slot.Item = TSoftObjectPtr<UArenaItemData>();
			Slot.Augment = Augment;
			Slot.Label = ArenaAugmentLabel(Augment);
			Slot.Description = Augment->Description;
			Slot.Icon = Augment->Icon;
			Slot.Tier = Augment->Tier;
			bRerolled = true;
		}
	}
	else if (Chooser->GetRewardStep() == EArenaRewardStep::ItemOffer)
	{
		for (int32 Attempt = 0; Attempt < 4 && !bRerolled; ++Attempt)
		{
			TArray<UArenaItemData*> Rolled;
			RewardPool->RollItems(Chooser->GetPendingItemTier(), 1,
				Loadout->GetEquippedUniqueItems(), Rolled);
			if (Rolled.Num() <= 0 || !Rolled[0])
			{
				break;
			}

			const TSoftObjectPtr<UArenaItemData> RolledRef(Rolled[0]);
			bool bDuplicate = false;
			for (int32 Index = 0; Index < NewPrompt.Options.Num(); ++Index)
			{
				if (Index != OptionIndex && NewPrompt.Options[Index].Item == RolledRef)
				{
					bDuplicate = true;
					break;
				}
			}
			if (bDuplicate)
			{
				continue;
			}

			UArenaItemData* Item = Rolled[0];
			Slot.Action = EArenaPromptAction::Grant;
			Slot.Item = Item;
			Slot.Augment = TSoftObjectPtr<UArenaAugmentData>();
			Slot.Label = ArenaItemLabel(Item);
			Slot.Description = Item->Description;
			Slot.Icon = Item->Icon;
			Slot.Tier = ArenaItemTierToAugmentTier(Chooser->GetPendingItemTier());
			bRerolled = true;
		}
	}

	if (!bRerolled)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 重随 %d 失败：池子里抽不出不重复的选项（原选项保留，次数未扣）。"),
			*Chooser->GetName(), OptionIndex);
		return;
	}

	// 抽到了才收费。上面的判据保证了这里一定有次数可扣。
	if (!Chooser->TryConsumeReroll())
	{
		// 理论上到不了（上面已经查过 GetRerollsLeft() > 0），留着是为了不把
		// 「白送的选项」写进 Prompt —— 宁可这次重随不生效，也不能让次数和
		// 提示对不上。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 重随 %d 抽到了新选项但扣次数失败 —— 本次重随作废（原选项保留）。"),
			*Chooser->GetName(), OptionIndex);
		return;
	}

	// 同步最新次数 + 广播（SetPendingPrompt 会复制到客户端，UI 走 OnPromptChanged 重画）。
	NewPrompt.RerollsLeft = Chooser->GetRerollsLeft();
	Chooser->SetPendingPrompt(NewPrompt);

	UE_LOG(LogTemp, Log, TEXT("[Arena] %s 重随了选项 %d（剩 %d 次重随）。"),
		*Chooser->GetName(), OptionIndex, Chooser->GetRerollsLeft());
}

void AArenaGameMode::HandleGrant(AArenaPlayerState* PS, const FArenaChoiceOption& Option)
{
	UArenaLoadoutComponent* Loadout = PS->GetLoadout();
	if (!Loadout)
	{
		PS->ResetRewardSelection();
		return;
	}

	// --- 海克斯 ---
	if (!Option.Augment.IsNull())
	{
		Loadout->AddAugment(Option.Augment.Get());
		EnterForgeMenu(PS);   // 海克斯拿完就该问了：还有锻造器要用吗
		return;
	}

	// --- 装备 ---
	if (!Option.Item.IsNull())
	{
		Loadout->EquipItem(Option.Item.Get());

		// 本回合奖励里可能还有"再来一次三选一"（FArenaBranchOption::Count > 1）。
		// 锻造器抽的那次 RemainingItemPicks 本来就是 0，所以这里直接落到菜单。
		PS->DecrementRemainingItemPicks();

		if (PS->GetRemainingItemPicks() > 0)
		{
			PresentItemOffer(PS, PS->GetPendingItemTier(), /*bFromRoundReward=*/true);
			return;
		}

		PS->SetRemainingItemPicks(0);
		EnterForgeMenu(PS);
		return;
	}

	// --- 分支 ---
	if (Option.Branch.Branch == EArenaRewardBranch::Forge)
	{
		Loadout->AddForgeCharges(Option.Branch.Tier, Option.Branch.Count);
		EnterForgeMenu(PS);
		return;
	}

	// 装备分支：展开成三选一。Count 是"要做几次三选一"。
	PS->SetPendingItemTier(Option.Branch.Tier);
	PS->SetRemainingItemPicks(Option.Branch.Count);
	PresentItemOffer(PS, Option.Branch.Tier, /*bFromRoundReward=*/true);
}

void AArenaGameMode::HandleUseForge(AArenaPlayerState* PS, EArenaItemTier Tier)
{
	UArenaLoadoutComponent* Loadout = PS ? PS->GetLoadout() : nullptr;
	if (!Loadout)
	{
		PS->ResetRewardSelection();
		return;
	}

	// 语义修正（2026-09-29）：锻造器 = 属性锻造器（stat anvil），花一次直接发随机属性。
	// 旧实现「花一次 → 展开一次装备三选一」是拿传说装备池的口径套错了对象，已删。
	const TArray<FArenaItemStatModifier>& Pool = (Tier == EArenaItemTier::Prismatic)
		? StatAnvilPool_Prismatic
		: StatAnvilPool_Legendary;

	// 先抽后扣：池子是空的就不要白扣玩家的次数 —— 那种「点了按钮、次数没了、
	// 什么都没拿到」比直接不给用更糟，而且玩家没法从界面上分辨是配置坏了还是抽完了。
	TArray<FArenaItemStatModifier> Roll = ArenaRollStatAnvil(Pool, StatsPerForgeUse);
	if (Roll.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 品质的锻造器属性池为空 —— 本次使用没有扣次数。请在 BP_ArenaGameMode 的 Arena|Forge 分类下配 StatAnvilPool_Legendary / StatAnvilPool_Prismatic。"),
			*UEnum::GetValueAsString(Tier));
		EnterForgeMenu(PS);
		return;
	}

	if (!Loadout->ConsumeForgeCharge(Tier))
	{
		// 界面上显示的是复制下来的旧次数，花的时候已经被扣光了（或者压根没有这一档）。
		// 不卡住：重新发一份菜单，他会看到次数已经变成 0 了。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] %s 想用一次 %s 锻造器，但次数不够 —— 重新发菜单。"),
			*PS->GetName(), *UEnum::GetValueAsString(Tier));
		EnterForgeMenu(PS);
		return;
	}

	Loadout->ApplyStatAnvil(Tier, Roll);

	// 用完回菜单：还能继续花（和旧行为一致 —— 一次使用不代表结束）。
	EnterForgeMenu(PS);
}

// ---------------------------------------------------------------------------
// 每回合复位
// ---------------------------------------------------------------------------

void AArenaGameMode::ResetContenderForRound(AArenaPlayerState* PS, int32 Slot, int32 RoundNumber)
{
	if (!IsValid(PS))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 第 %d 回合开始，但 %d 号位是空的 —— 这一方不会被复位。"),
			RoundNumber, Slot);
		return;
	}

	// 等级：需求"每回合开始 +3 级"。第 1 回合就是初始的 3 级。
	PS->SetHeroLevel(GetLevelForRound(RoundNumber));

	// 奖励流程清零。上一回合残留的待选必须清掉 —— 不清的话新回合一开始
	// 界面上就挂着一份旧的，点下去还会真的发东西（发的是上一回合的奖励）。
	PS->ResetRewardSelection();

	APawn* Pawn = PS->GetPawn();
	if (!Pawn)
	{
		// 【为什么在这里补生一个 Pawn】Pawn 可能在上一回合的战斗里被销毁 ——
		// 掉出世界的那种会在 KillZ 平面被引擎直接销毁，GetPawn() 从此一直是空。
		// 不补的话这一方每回合都上不了场，而 PollCombatOutcome 里"本回合还没出现过
		// Pawn"那条分支是故意不判负的（那时判会判错人），于是整局会从这里再挂一次。
		//
		// 补的方式和 Bot 开局完全一样（RestartPlayer），所以新 Pawn 的出生点、
		// 技能、数值来源和原来那一个没有区别。
		if (AController* Controller = PS->GetOwningController())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[Arena] 第 %d 回合开始，%s 没有 Pawn（上一回合被销毁了？）—— 重新生一个。"),
				RoundNumber, *PS->GetName());
			RestartPlayer(Controller);
			Pawn = PS->GetPawn();
		}

		if (!Pawn)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[Arena] 第 %d 回合开始，%s 还没有 Pawn —— 等级设上了，位置和血量设不上。"),
				RoundNumber, *PS->GetName());
			return;
		}
	}

	if (const AHeroCombatCharacter* Hero = Cast<AHeroCombatCharacter>(Pawn))
	{
		if (Hero->IsDead())
		{
			// 复活不是这里触发的，是 GE_Death 到期（时长 = 角色 BP 上的 RespawnDelay）。
			// 所以走到这一条 = SettlementSeconds 配得比 RespawnDelay 还短，
			// 下一回合会在尸体还躺着的时候开始。说清楚要改哪个数，不然很难联想。
			UE_LOG(LogTemp, Warning,
				TEXT("[Arena] 第 %d 回合开始时 %s 还在死亡状态 —— 把角色 BP 上的 RespawnDelay "
					 "调到小于 GameMode 的 SettlementSeconds（现在 %.1f 秒）。"),
				RoundNumber, *PS->GetName(), SettlementSeconds);
		}
	}

	// 备战区的出生点。备战阶段玩家在这里自由移动、做本回合的选择；
	// 开战时 BeginCombat 会把双方传回战斗出生点。
	// 【回退】关卡里没摆备战点时回退战斗出生点 —— 备战区没布置的关卡
	// 行为退回改动前（人站在战斗位上做选择），模式照样能跑。
	if (const AActor* Spawn = GetPrepSpawnPoint(Slot))
	{
		TeleportPawnToActor(Pawn, Spawn);
	}

	FullRestore(PS);
}

void AArenaGameMode::FullRestore(AArenaPlayerState* PS) const
{
	UAbilitySystemComponent* ASC = IsValid(PS) ? PS->GetMyAbilitySystemComponent() : nullptr;
	if (!ASC)
	{
		return;
	}

	if (const UHeroCombatAttributeSet* Attributes = ASC->GetSet<UHeroCombatAttributeSet>())
	{
		// 直接写基础值，不走 GE —— 和 AHeroCombatCharacter::ExitDeathState 里那段同一个理由：
		// 回合重置不是"可被减免 / 可被驱散的治疗"，走 GE 会被格挡组件、重伤之类的算进去。
		// PreAttributeBaseChange 会把值钳进 [0, Max]，传 Max 正好落回来。
		ASC->SetNumericAttributeBase(UHeroCombatAttributeSet::GetHealthAttribute(), Attributes->GetMaxHealth());
		ASC->SetNumericAttributeBase(UHeroCombatAttributeSet::GetEnergyAttribute(), Attributes->GetMaxEnergy());
	}
}

// ---------------------------------------------------------------------------
// 查表
// ---------------------------------------------------------------------------

FArenaRoundReward AArenaGameMode::GetRoundReward(int32 RoundNumber) const
{
	if (RoundNumber <= 0)
	{
		return FArenaRoundReward();
	}

	// 前几回合照表发。
	if (RoundNumber <= RoundRewards.Num())
	{
		return RoundRewards[RoundNumber - 1];
	}

	// 超出表的部分按 LoopRewards 取模循环。
	// 需求：7 装备 / 8 装备 / 9 海克斯 / 10 装备 / 11 装备 / 12 海克斯…
	if (LoopRewards.Num() > 0)
	{
		const int32 LoopIndex = (RoundNumber - RoundRewards.Num() - 1) % LoopRewards.Num();
		return LoopRewards[LoopIndex];
	}

	// 没配循环表：第 7 回合之后什么都不发。不是崩点，但必须吵 ——
	// 静默的话表现是"打到某一回合突然不发奖励了"，很难联想到是配置缺失。
	UE_LOG(LogTemp, Warning,
		TEXT("[Arena] 第 %d 回合超出了回合表（表里有 %d 条），而 LoopRewards 是空的 —— "
			 "这个回合不发奖励。要循环发就在 BP_ArenaGameMode 上配 LoopRewards。"),
		RoundNumber, RoundRewards.Num());
	return FArenaRoundReward();
}

int32 AArenaGameMode::GetLevelForRound(int32 RoundNumber) const
{
	// 第 1 回合 = InitialHeroLevel，之后每回合加 LevelsPerRound。
	// 用 Max(0, ...) 而不是直接减：万一有人传了 0 或负数，也只是掉到初始等级，
	// 不会算出一个负等级（SetHeroLevel 那边还会再夹一次，但算式本身该是干净的）。
	const int32 Gained = LevelsPerRound * FMath::Max(0, RoundNumber - 1);
	return FMath::Clamp(InitialHeroLevel + Gained, 1, MaxHeroLevel);
}

int32 AArenaGameMode::GetHpLossForRound(int32 RoundNumber) const
{
	// 取"最后一条 FromRound <= 当前回合"的。表是按 FromRound 升序配的，
	// 所以从后往前找第一条命中的就是它。
	//
	// 【不在这里排序】排序会把"配乱了"这件事盖掉。不排的话结果是取了中间某一条，
	// 表现是扣血量不对 —— 那比"顺序被悄悄改过"好查得多。
	for (int32 Index = HpLossBands.Num() - 1; Index >= 0; --Index)
	{
		if (RoundNumber >= HpLossBands[Index].FromRound)
		{
			return HpLossBands[Index].HpLoss;
		}
	}

	// 一条都没命中（表是空的，或者第一条 FromRound 比当前回合还大）。
	// 这条如果不报出来，表现是"打完了谁也不掉血、大场永远不结束"。
	UE_LOG(LogTemp, Warning,
		TEXT("[Arena] 第 %d 回合没有匹配到任何扣血区间（HpLossBands 有 %d 条，第一条 FromRound=%d）—— "
		 "这一回合不扣大场血量。检查 BP_ArenaGameMode 上的 HpLossBands。"),
		RoundNumber, HpLossBands.Num(), HpLossBands.Num() > 0 ? HpLossBands[0].FromRound : 0);
	return 0;
}

bool AArenaGameMode::HasReachedFirstThreshold(const AArenaPlayerState* PS) const
{
	if (!IsValid(PS))
	{
		return false;
	}

	const int32 Won = PS->GetRoundsWon();
	const int32 Lost = PS->GetRoundsLost();

	// min_rounds：没打满不判。5:0 也不是结束（需求 min_rounds 6）。
	if (Won + Lost < FirstThresholdWins)
	{
		return false;
	}

	// 需求："一方 6 回合全胜或全败，即 6:0 / 0:6"。
	// 全胜和全败是同一场比赛从两边看，所以对同一个人只会命中一个方向 ——
	// 但两个都写上：以后 FirstThresholdWins 改成别的数时，这种等价关系未必还成立。
	return (Won == FirstThresholdWins && Lost == 0)
		|| (Lost == FirstThresholdWins && Won == 0);
}

void AArenaGameMode::TeleportPawnToActor(APawn* Pawn, const AActor* Spawn)
{
	if (!Pawn || !Spawn)
	{
		// 出生点缺失各自查得到（GetContenderSpawnPoint / GetPrepSpawnPoint 都记了日志），
		// 这里不重复吵。传送失败 = 人留在原地，等下一轮复位。
		return;
	}

	Pawn->SetActorLocationAndRotation(Spawn->GetActorLocation(), Spawn->GetActorRotation(),
		/*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
}

void AArenaGameMode::PushRoundPlanToGameState()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return;
	}

	TArray<EArenaStageKind> Plan;
	Plan.Reserve(FMath::Max(1, RoundPlanLength));
	for (int32 Round = 1; Round <= RoundPlanLength; ++Round)
	{
		Plan.Add(GetStageKindForRound(Round));
	}

	GS->SetRoundPlan(MoveTemp(Plan));
}

EArenaStageKind AArenaGameMode::GetStageKindForRound(int32 RoundNumber) const
{
	const FArenaRoundReward Reward = GetRoundReward(RoundNumber);

	switch (Reward.Kind)
	{
	case EArenaRewardKind::Augment:
		return EArenaStageKind::Augments;

	case EArenaRewardKind::BranchChoice:
		// 分支回合按第一个有效分支画：抽到装备就是装备回合，抽到锻造器就是锻造回合。
		// 玩家实际选了另一个分支的话，回合线会和那一回合的发的东西对不上 ——
		// 这是"计划"和"实际"的天然差距，LoL 原作的回合线同样只画计划。
		for (const FArenaBranchOption& Branch : Reward.Branches)
		{
			if (Branch.Count > 0)
			{
				return Branch.Branch == EArenaRewardBranch::Forge
					? EArenaStageKind::StatAnvil
					: EArenaStageKind::ItemPurchase;
			}
		}
		return EArenaStageKind::Combat;

	case EArenaRewardKind::FixedForge:
		return EArenaStageKind::StatAnvil;

	case EArenaRewardKind::None:
	default:
		// 无奖励回合（第 6 回合的阈值判定）也是实打实打一场。
		return EArenaStageKind::Combat;
	}
}

// ---------------------------------------------------------------------------
// 参赛者 / 出生点
// ---------------------------------------------------------------------------

AArenaGameState* AArenaGameMode::GetArenaGameState() const
{
	return GetGameState<AArenaGameState>();
}

int32 AArenaGameMode::FindFreeContenderSlot() const
{
	const AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return INDEX_NONE;
	}

	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		if (!IsValid(GS->GetContender(Slot)))
		{
			return Slot;
		}
	}

	return INDEX_NONE;
}

int32 AArenaGameMode::FindContenderSlotOf(const AArenaPlayerState* PS) const
{
	const AArenaGameState* GS = GetArenaGameState();
	if (!GS || !PS)
	{
		return INDEX_NONE;
	}

	for (int32 Slot = 0; Slot < ArenaMatch::ContenderCount; ++Slot)
	{
		if (GS->GetContender(Slot) == PS)
		{
			return Slot;
		}
	}

	return INDEX_NONE;
}

AActor* AArenaGameMode::GetContenderSpawnPoint(int32 Slot) const
{
	UWorld* World = GetWorld();
	if (!World || Slot < 0)
	{
		return nullptr;
	}

	TArray<AActor*> Starts;
	UGameplayStatics::GetAllActorsOfClass(World, APlayerStart::StaticClass(), Starts);

	// 备战点不算战斗出生点：备战点在这里被隔离掉，两套出生点不会互相串。
	// 反过来 GetPrepSpawnPoint 只认备战点，两边是互补的过滤（同一个谓词），不是两份巧合。
	Starts.RemoveAll([](const AActor* Actor)
	{
		return IsPrepSpawnActor(Actor);
	});

	if (Starts.Num() <= Slot)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 关卡里只有 %d 个战斗出生点（备战点不算），找不到 %d 号（下标从 0 起）—— 这一方会留在原地。"),
			Starts.Num(), Slot);
		return nullptr;
	}

	// 【取之前先按名字排序】GetAllActorsOfClass 的顺序来自关卡里的 Actor 数组，
	// 不保证稳定。不排的话 PIE 重开一次两个人就可能换边，而且换边这种事
	// 在两边都长一样的时候根本看不出来。排完之后"名字靠前的那个"永远是 0 号位 ——
	// 关卡里把两个出生点叫 ArenaStart_A / ArenaStart_B 就能一眼对上。
	Starts.Sort([](const AActor& A, const AActor& B) { return A.GetName() < B.GetName(); });

	return Starts[Slot];
}

AActor* AArenaGameMode::GetPrepSpawnPoint(int32 Slot) const
{
	UWorld* World = GetWorld();
	if (!World || Slot < 0)
	{
		return nullptr;
	}

	TArray<AActor*> Starts;
	UGameplayStatics::GetAllActorsOfClass(World, APlayerStart::StaticClass(), Starts);

	// 只认备战点（与 GetContenderSpawnPoint 互补，两边共用同一个谓词）。
	Starts.RemoveAll([](const AActor* Actor)
	{
		return !IsPrepSpawnActor(Actor);
	});

	if (Starts.Num() <= 0)
	{
		// 没布置备战区：回退战斗出生点。不报错 —— "这张图没有备战区"是合法配置，
		// 只在 Log 里留一条痕，方便排查"为什么备战阶段人站在战斗位上"。
		UE_LOG(LogTemp, Log,
			TEXT("[Arena] 关卡里没有备战点 —— 备战阶段回退到战斗出生点。（备战点 = PlayerStart 加 PrepStart 标签，或名字/Label 以 PrepStart 开头）"));
		return GetContenderSpawnPoint(Slot);
	}

	Starts.Sort([](const AActor& A, const AActor& B) { return A.GetName() < B.GetName(); });

	if (Starts.Num() <= Slot)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 关卡里有 %d 个备战点，但 %d 号位没有对应的（标成 PrepStart_A / PrepStart_B 最好认）—— 这一方留在战斗出生点。"),
			Starts.Num(), Slot);
		return GetContenderSpawnPoint(Slot);
	}

	return Starts[Slot];
}

// ---------------------------------------------------------------------------
// Bot
// ---------------------------------------------------------------------------

void AArenaGameMode::TrySpawnBot()
{
	AArenaGameState* GS = GetArenaGameState();
	if (!GS)
	{
		return;
	}

	// 真人来了（或者比赛已经开打了）就不用 Bot 了。
	if (GS->AreContendersReady() || GS->GetArenaPhase() != EArenaPhase::WaitingToStart)
	{
		return;
	}

	if (!BotControllerClass)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 等第二个参赛者的时间到了，但 BP_ArenaGameMode 上没配 BotControllerClass —— "
				 "只能继续等真人（或者关掉 bSpawnBotIfAlone 让这个警告别再出现）。"));
		return;
	}

	const int32 Slot = FindFreeContenderSlot();
	if (Slot == INDEX_NONE)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = this;

	// 【为什么先 SpawnActor、后 RestartPlayer】
	// AAIController 在 PostInitializeComponents 里就建好了 PlayerState（bWantsPlayerState = true，
	// 见 AArenaBotController 的构造函数，引擎那边是 AIController.cpp:69）。
	// 所以要先把这个 PlayerState 登记进参赛者表，RestartPlayer 里的 ChoosePlayerStart
	// 才认得出"这是 N 号位"，把它生在正确的那一侧。
	AArenaBotController* Bot = World->SpawnActor<AArenaBotController>(
		BotControllerClass.Get(), FTransform::Identity, SpawnParams);

	if (!Bot)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] Bot 控制器生成失败 —— 只能继续等真人。"));
		return;
	}

	AArenaPlayerState* BotState = Bot->GetPlayerState<AArenaPlayerState>();
	if (!BotState)
	{
		// 走到了说明 PlayerStateClass 或者 AArenaBotController 的 bWantsPlayerState 出了问题。
		// 不销毁的话场上会多一个没有 PlayerState、也不参赛的空控制器。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] Bot 控制器起来了，但它的 PlayerState 不是 AArenaPlayerState —— "
				 "检查 GameMode 的 PlayerStateClass，以及 BP_ArenaBotController 有没有把 bWantsPlayerState 关掉。"
				 "已销毁这个 Bot。"));
		Bot->Destroy();
		return;
	}

	GS->SetContender(Slot, BotState);

	// 生 Pawn + 让它接管。走的和真人开局是同一条路（RestartPlayer），
	// 所以 Bot 的 Pawn、技能、数值的来源和真人完全一致。
	RestartPlayer(Bot);

	if (!GS->AreContendersReady())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] Bot 起来了但参赛者还是没凑齐 —— 多半是 DefaultPawnClass 没配。"));
		return;
	}

	StartMatch();
}

#undef LOCTEXT_NAMESPACE
