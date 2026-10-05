// 斗魂竞技场：AI 对手的实现。

#include "GAS/ArenaBotController.h"

#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "TimerManager.h"

#include "GAS/ArenaAIDirector.h"
#include "GAS/ArenaBehaviorObserver.h"
#include "GAS/ArenaGameMode.h"
#include "GAS/ArenaGameState.h"
#include "GAS/ArenaPlayerState.h"
#include "GAS/ArenaTacticalPlanner.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/HeroCombatCharacter.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/MyGameplayAbility.h"
#include "GAS/InputConfig.h"
#include "GAS/LOLGameplayTags.h"

namespace
{
	/**
	 * 取一个角色的血量比例（0~1）。拿不到属性集时返回 1（当作满血）。
	 *
	 * 【为什么兜底是 1 而不是 0】这个数只喂给战略层的"压力值"。
	 * 返回 0 会读成"它快死了" ⇒ 压力拉满 ⇒ 决策频率和出手节奏全推到极限，
	 * 而那是在一个我们其实什么都不知道的情况下。返回 1 是中性值，最差退化成原版 Bot。
	 */
	float ResolveHealthRatio(const AActor* Actor)
	{
		if (!Actor)
		{
			return 1.f;
		}

		const UAbilitySystemComponent* ASC = UMyAbilitySystemComponent::FindAbilitySystemComponent(Actor);
		if (!ASC)
		{
			return 1.f;
		}

		const UHeroCombatAttributeSet* Attributes = ASC->GetSet<UHeroCombatAttributeSet>();
		if (!Attributes)
		{
			return 1.f;
		}

		const float Max = Attributes->GetMaxHealth();
		if (Max <= KINDA_SMALL_NUMBER)
		{
			return 1.f;   // 数值还没初始化完。别除零，也别报"空血"。
		}

		return FMath::Clamp(Attributes->GetHealth() / Max, 0.f, 1.f);
	}

	/**
	 * 取一个角色的能量比例（0~1）。拿不到时返回 1（当作满蓝）。
	 *
	 * 【兜底为什么也是 1】同 ResolveHealthRatio：返回 0 会读成"蓝是空的"，
	 * 而那会让战术层把耗蓝技能全部压到地板分 —— 又是一个"在什么都不知道的
	 * 情况下做出激进判断"的情况。给满值 = 中性，不影响任何判断。
	 */
	float ResolveEnergyRatio(const AActor* Actor)
	{
		if (!Actor)
		{
			return 1.f;
		}

		const UAbilitySystemComponent* ASC = UMyAbilitySystemComponent::FindAbilitySystemComponent(Actor);
		if (!ASC)
		{
			return 1.f;
		}

		const UHeroCombatAttributeSet* Attributes = ASC->GetSet<UHeroCombatAttributeSet>();
		if (!Attributes)
		{
			return 1.f;
		}

		const float Max = Attributes->GetMaxEnergy();
		if (Max <= KINDA_SMALL_NUMBER)
		{
			return 1.f;
		}

		return FMath::Clamp(Attributes->GetEnergy() / Max, 0.f, 1.f);
	}

	/**
	 * 这个槽位现在在不在冷却里。
	 *
	 * 【判据为什么是"冷却标签在不在 ASC 上"，而不是 GetCooldownTimeRemaining】
	 * 引擎那个版本（GameplayAbility.cpp）内部做的正是同一件事 ——
	 * GetCooldownTags() 返回冷却 GE 的 GrantedTags，再按这些标签查活跃 GE。
	 * 但它要求传一个【非空】的 ActorInfo（代码里直接解引用，没有判空），
	 * 而这里只需要一个是非。
	 *
	 * 而且这条判据和观测层看【玩家】冷却是同一个（State.Cooldown.*）——
	 * 双方用同一个口径，调阈值时不会出现"我看他是好的、他看我是CD"这种错位。
	 */
	bool IsSlotOnCooldown(const UAbilitySystemComponent* ASC, const UGameplayAbility* Ability)
	{
		if (!ASC || !Ability)
		{
			return false;
		}

		const FGameplayTagContainer* CooldownTags = Ability->GetCooldownTags();

		// 没配冷却 GE 的技能永远"不在冷却"（它自己会限流，或者本来就该连放）。
		return CooldownTags && CooldownTags->Num() > 0 && ASC->HasAnyMatchingGameplayTags(*CooldownTags);
	}

	/** 往画像表里加一条，只写槽位/角色/距离带，其余字段留给调用方按需覆盖。 */
	FArenaAbilityProfile& ArenaAddAbilityProfile(UArenaTacticalPlanner& Planner, const FGameplayTag& Slot,
		EArenaAbilityRole Role, float MinRange, float MaxRange)
	{
		FArenaAbilityProfile Profile;
		Profile.SlotTag = Slot;
		Profile.Role = Role;
		Profile.MinRange = MinRange;
		Profile.MaxRange = MaxRange;
		return Planner.AbilityProfiles.Add_GetRef(Profile);
	}

	/**
	 * 默认技能画像表 —— 没人在编辑器里填过时用这一份。
	 *
	 * 【为什么默认值要写进代码，而不是只放在蓝图的 Details 面板里】
	 * 画像为空时，每个技能都是"未指定的中性伤害技"：距离带一样、血量甜蜜区一样，
	 * 于是十来个技能的得分几乎相同，只剩反连放/承诺黏性在起作用 —— 战术层塌回
	 * 原来那套轮询。而这个故障【不报任何错】：Bot 照样走位、照样放技能，只是变蠢。
	 * 把默认值放进代码 = 让"漏填"这件事在构造期自动补上，而不是指望谁记得去点十下。
	 *
	 * 【蓝图里填了会怎样】以蓝图那份为准（UE 的数组默认值本来就是整体覆盖）。
	 * 所以这份是【地板】不是【锁】：想调就在 Details 面板里调，调完整份替换掉它。
	 * 判断标准是"动一个数要不要重编译"—— 手感想连着试的时候走蓝图，定下来了再回填到这里。
	 *
	 * 【为什么挂在控制器而不是战术层】战术层刻意不认识任何具体技能（见它的文件头），
	 * 而这份表是按【这一套技能组】列的。控制器是唯一同时知道"我在开哪个角色"的地方。
	 * 将来有第二个角色时，这份表该跟着 AbilitySlots 一起抽成数据资产 ——
	 * 而不是在这里再加一个 if。
	 *
	 * 【EnergyCost 全部留 0】它是硬门（能量不够就干脆不参与竞争），而这里没有任何一个
	 * 技能的真实耗能数据。猜一个数会让技能静默变成"永远不选它"，比留 0 危险得多。
	 */
	void ArenaSeedDefaultAbilityProfiles(UArenaTacticalPlanner& Planner)
	{
		if (Planner.AbilityProfiles.Num() > 0)
		{
			return;
		}

		// --- 隐身：摸过去的那一手 ---------------------------------------------
		// 自己满血、对面还没交牌的时候开，所以血量甜蜜区压到 0.10（越满血越想开）。
		// 锁 6 秒是因为它开了就该立刻接强化普攻，反复开等于白交一个先手。
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_Q, EArenaAbilityRole::Buff, 0.f, 400.f);
			P.SelfHealthSweetSpot = 0.10f;
			P.RepeatLockout = 6.f;
		}

		// --- 闪避 / 翻滚 / 闪现：都归"躲闪"一族 -------------------------------
		// 三个的距离带和血量曲线写法一致（0.90 = 越吃紧越想用，band 0.40 收窄），
		// 差别只在反连放和"趁他交完"的权重上：翻滚最短，闪现最长（它是唯一的外交牌）。
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_W, EArenaAbilityRole::Evade, 0.f, 400.f);
			P.SelfHealthSweetSpot = 0.90f;
			P.HealthBand = 0.40f;
			P.RepeatLockout = 3.f;
			P.EnemySpentMul = 0.7f;
		}
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_GroundDodge, EArenaAbilityRole::Evade, 0.f, 400.f);
			P.SelfHealthSweetSpot = 0.90f;
			P.HealthBand = 0.40f;
			P.RepeatLockout = 3.f;
			P.EnemySpentMul = 0.7f;
		}
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_F, EArenaAbilityRole::Evade, 0.f, 400.f);
			P.SelfHealthSweetSpot = 0.90f;
			P.HealthBand = 0.40f;
			P.RepeatLockout = 5.f;
			P.EnemySpentMul = 0.7f;
		}

		// --- 飞镖：唯一的远程牌 -------------------------------------------------
		// 最近 150 / 最远 600：贴脸时它不该跟近战抢，拉开距离才是它的场子。
		// 趁他交完给 1.2（这是最容易打中的窗口），权重 1.2 —— 它是这套里最稳的消耗手段。
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_E, EArenaAbilityRole::Ranged, 150.f, 600.f);
			P.HealthBand = 0.60f;
			P.Weight = 1.2f;
			P.RepeatLockout = 2.f;
			P.EnemySpentMul = 1.2f;
		}

		// --- 死亡收割：斩杀 -----------------------------------------------------
		// 对方 0.85（残血）最想用、band 0.30 收得很窄 —— 满血时它就是个普通技能。
		// 他挨控时 1.6（白送一套），他格挡时 0.10（打进去纯浪费，几乎让开）。
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_R, EArenaAbilityRole::Execute, 0.f, 500.f);
			P.EnemyHealthSweetSpot = 0.85f;
			P.HealthBand = 0.30f;
			P.Weight = 1.3f;
			P.RepeatLockout = 4.f;
			P.EnemyHardControlMul = 1.6f;
			P.EnemyDamageProofMul = 0.10f;
			P.EnemySpentMul = 1.2f;
		}

		// --- 格挡：唯一的减伤 ---------------------------------------------------
		// 他攒着一记大的（隐身/强化普攻）时抬到 1.4 —— 这是格挡唯一真正值钱的时刻；
		// 他刚交完牌时压到 0.6（那会儿该压上去，不是举盾）。
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_Block, EArenaAbilityRole::Defensive, 0.f, 400.f);
			P.SelfHealthSweetSpot = 0.90f;
			P.Weight = 1.1f;
			P.RepeatLockout = 3.f;
			P.EnemyThreatLoadedMul = 1.4f;
			P.EnemySpentMul = 0.6f;
		}

		// --- 变身：开场技 -------------------------------------------------------
		// 和隐身同理（满血才想开），但锁 8 秒 —— 它的前摇和收益都比隐身高一档。
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_Form, EArenaAbilityRole::Buff, 0.f, 400.f);
			P.SelfHealthSweetSpot = 0.10f;
			P.RepeatLockout = 8.f;
		}

		// --- 上挑击飞 + 回身击退：一对连招 ---------------------------------------
		// 两个都是 250 的近身控，都设成 Always 参与命中率学习 ——
		// 它们首先是控制，但也是实打实的伤害，"放空了"这件事对它们是有效信息。
		//
		// 【为什么上挑的硬控倍率是 0.6、回身是 1.8】它们是连招的前后半段：
		// 上挑是【起手】，所以他已经挨控的时候再上挑是浪费（0.6）；
		// 回身是【后手】，紧跟在击飞之后（1.8）。这一对正好演示了"角色层面表达不了、
		// 必须逐技能填"的那种区别 —— 两个都是 Disrupt。
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_Hex2, EArenaAbilityRole::Disrupt, 0.f, 250.f);
			P.Weight = 1.1f;
			P.RepeatLockout = 2.f;
			P.EnemyHardControlMul = 0.6f;
			P.EnemyDamageProofMul = 0.15f;
			P.EnemySpentMul = 1.3f;
			P.HitRatePolicy = EArenaHitRatePolicy::Always;
		}
		{
			FArenaAbilityProfile& P = ArenaAddAbilityProfile(Planner, LOLGameplayTags::Ability_Slot_Hex3, EArenaAbilityRole::Disrupt, 0.f, 250.f);
			P.Weight = 1.1f;
			P.RepeatLockout = 2.f;
			P.EnemyHardControlMul = 1.8f;
			P.EnemyDamageProofMul = 0.15f;
			P.EnemySpentMul = 1.3f;
			P.HitRatePolicy = EArenaHitRatePolicy::Always;
			// 连招：上挑之后 2.5 秒内，回身的分数翻一倍多。
			P.FollowUpTag = LOLGameplayTags::Ability_Slot_Hex2;
			P.FollowUpWindow = 2.5f;
			P.FollowUpBonus = 2.2f;
		}
	}
}

AArenaBotController::AArenaBotController()
{
	// 【必须在构造函数里设】AAIController 默认是 false（AIController.cpp:51），
	// 而 PostInitializeComponents 在 SpawnActor 的过程中就会读它并据此建 PlayerState
	// （AIController.cpp:69 → AController::InitPlayerState → GameMode->PlayerStateClass）。
	// 出生之后再设就晚了 —— 表现是 Bot 有控制器、有 Pawn，但 GetPlayerState 永远是 nullptr，
	// 于是它不算参赛者、领不到等级也领不到奖励，而且没有任何报错。
	bWantsPlayerState = true;

	// AController 的构造函数本来就开了 tick（Controller.cpp:62），这里显式写一遍：
	// 这个类的全部行为都挂在 Tick 上，将来谁改基类也不会把它悄悄关掉。
	PrimaryActorTick.bCanEverTick = true;

	// 观测层和战略层的默认实现。
	//
	// 【为什么是 CreateDefaultSubobject 而不是 OnPossess 里 NewObject】
	// 子对象是配置的载体（存档槽名、阈值、上限都挂在它们身上），而配置要么来自
	// 蓝图默认值、要么来自 Details 面板 —— 两者都要求对象在【构造期】就存在。
	// 运行时 NewObject 出来的对象拿不到 BP 上改过的那份默认值。
	Observer = CreateDefaultSubobject<UArenaBehaviorObserver>(TEXT("BehaviorObserver"));
	StrategyDirector = CreateDefaultSubobject<UArenaAIDirector>(TEXT("StrategyDirector"));
	TacticalPlanner = CreateDefaultSubobject<UArenaTacticalPlanner>(TEXT("TacticalPlanner"));

	// 战术层的技能画像默认值。写在构造期而不是 OnPossess 里，是为了让它成为
	// 【类默认值】：能在 Details 面板里被看见、被覆盖，也能被子类继承。
	// 放在这里而不是战术层的构造函数里，理由见上面那个函数的注释。
	ArenaSeedDefaultAbilityProfiles(*TacticalPlanner);
}

void AArenaBotController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	// 换 Pawn 就意味着重新开始：上一轮的计时和待答标志都要清掉。
	// 不清的话新 Pawn 会在"距离上次普攻还不到间隔"的假象里站着不动一小会儿。
	LastAttackTime = -1000.f;
	LastSkillTime = -1000.f;
	LastDecisionTime = -1000.f;
	LastStrafeFlipTime = -1000.f;
	StrafeSign = FMath::RandBool() ? 1.f : -1.f;
	bAnswerQueued = false;
	// 连招状态同理：新 Pawn 身上那个技能实例（如果还活着）的窗口和这套连招无关，
	// 带着旧计数过去会让它在没起手的情况下等着踩一个不属于它的窗口。
	bChainWindowWasOpen = false;
	ChainHitsLeft = 0;
	NextChainPressTime = -1.f;
	bPerfectWindowWasArmed = false;
	bPerfectWindowWasOpen = false;
	GetWorldTimerManager().ClearTimer(RewardAnswerHandle);

	// 战术层的状态也要跟着 Pawn 一起重置。
	// 【为什么不能省】反连放记录的是"这个槽位上次按下的世界时间"，而世界时间
	// 是跨 Pawn 连续的 —— 不清的话新 Pawn 会在"距离上次放技能还不到 1.5 秒"的
	// 假象里开局，表现是换了人之后头一两秒不放技能。
	SlotRetryAfter.Reset();
	CommittedSlot = FGameplayTag();
	PendingOutcomeSlot = FGameplayTag();
	if (TacticalPlanner)
	{
		TacticalPlanner->ResetCastHistory();
	}

	// --- 朝向：让身体真的面朝对手，而不是面朝它在走的方向 ---
	//
	// 【原来的表现】Tick 里那句 SetFocus(Enemy) 只解决【控制旋转】——也就是
	// 视线和瞄准方向。而网格朝哪边是由 CharacterMovement 的旋转模式决定的，
	// 本项目默认是 bOrientRotationToMovement = true，即"网格朝移动方向"。
	// 于是视线锁着玩家、身体朝它当时在走的那个方向。
	//
	// 而它的移动又绝大部分是"贴着对手横向绕圈"（Forward = 0、只有 Strafe 分量），
	// 身体于是常年朝侧面，还会随着绕圈方向翻来覆去 —— 这正是"原地转向微微抽"
	// 里"转"的那一半。（另一半是执行层每秒只发 4 次移动输入，见 Tick 里的注释。）
	//
	// 【为什么用 bUseControllerDesiredRotation 而不是 bUseControllerRotationYaw】
	// 后者是【瞬时对齐】：把控制旋转直接抄给 Actor，转身是一帧到位的硬切。
	// 前者让移动组件按 RotationRate 平滑地转过去，看起来才像人在转身。
	// 两者不能同时开 —— 瞬时那条优先级更高，会把这里想要的过程整个盖掉，
	// 所以下面把 bUseControllerRotationYaw 显式关掉，而不是假设它是关的。
	if (AHeroCombatCharacter* Possessed = Cast<AHeroCombatCharacter>(InPawn))
	{
		if (UCharacterMovementComponent* Movement = Possessed->GetCharacterMovement())
		{
			Possessed->bUseControllerRotationYaw = false;
			Movement->bOrientRotationToMovement = false;
			Movement->bUseControllerDesiredRotation = true;

			// 每秒 540 度：追一个绕着我转的人够用，又不至于快到看不出转身。
			Movement->RotationRate = FRotator(0.f, 540.f, 0.f);
		}
	}

	if (InPawn && !Cast<AHeroCombatCharacter>(InPawn))
	{
		// 不是英雄的话下面每一处 Cast 都会落空，表现是"Bot 站着不动"。
		// 说出来，不然要一路查到 DefaultPawnClass 才发现。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] Bot 接管了 %s，但它不是 AHeroCombatCharacter —— 这个 Bot 不会做任何事。"
				 "检查 GameMode 的 DefaultPawnClass。"),
			*GetNameSafe(InPawn));
	}
}

void AArenaBotController::OnUnPossess()
{
	GetWorldTimerManager().ClearTimer(RewardAnswerHandle);
	bAnswerQueued = false;

	Super::OnUnPossess();
}

// ---------------------------------------------------------------------------
// Tick：按相位分派
// ---------------------------------------------------------------------------

void AArenaBotController::Tick(float DeltaSeconds)
{
	// 父类的 Tick 里会跑 UpdateControlRotation（AIController.cpp:62）——
	// 战斗阶段的瞄准全靠它，所以这一行不能省。
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	AArenaGameState* GameState = World ? World->GetGameState<AArenaGameState>() : nullptr;
	AArenaPlayerState* Me = GetPlayerState<AArenaPlayerState>();

	if (!GameState || !Me)
	{
		// GameState 还没复制过来 / PlayerState 不是竞技场那个。
		// 前者是开局的正常瞬间，后者是配置错了 —— 但两种都不该在这里刷日志。
		return;
	}

	const EArenaPhase Phase = GameState->GetArenaPhase();

	// 回到 WaitingToStart = 新的一局要开始了，重新武装。
	// 上一局的 bMatchStarted 这时必须清掉，否则第二局的观测不会重新读档
	// （BeginMatch 在观测层里是幂等的，不清这里就永远不会再进去）。
	if (Phase == EArenaPhase::WaitingToStart)
	{
		bMatchStarted = false;
	}

	// 第一次离开 WaitingToStart = 这一局真的开始了。
	if (!bMatchStarted && Phase != EArenaPhase::WaitingToStart)
	{
		bMatchStarted = true;
		if (Observer)
		{
			Observer->BeginMatch();   // 内部读档 + 对局计数
		}

		// 命中率记录按【局】清，不按回合清，也不跨局留：
		// 回合太短（一两次出手说明不了任何事），而跨局留会把上一局对另一个人
		// 学到的账带过来 —— 而这个对手可能已经换了打法或换了号。
		if (TacticalPlanner)
		{
			TacticalPlanner->ResetOutcomeHistory();
		}
	}

	// 只在【相位切换的那一刻】通知观测层，不是每帧。
	// 每帧通知的话 BeginRound 会每帧清空一次回合累加器 —— 于是累加器永远是空的，
	// 表现是"Bot 完全没在观察"（所有比率恒为 0），且没有任何报错。
	if (Phase != LastObservedPhase)
	{
		HandlePhaseTransition(LastObservedPhase, Phase, Me);
		LastObservedPhase = Phase;
	}

	switch (Phase)
	{
	case EArenaPhase::RewardSelection:
		TickRewardPhase(Me);
		return;

	case EArenaPhase::Combat:
		TickCombatPhase(DeltaSeconds, GameState, Me);
		return;

	case EArenaPhase::WaitingToStart:
	case EArenaPhase::Settlement:
	case EArenaPhase::MatchEnd:
	default:
		// 非战斗相位一律停手。这就是 AI 的"冻结"—— 它不需要别处再通知它一次，
		// 因为这个函数是它产生一切动作的唯一入口。
		Idle();
		return;
	}
}

// ---------------------------------------------------------------------------
// 自适应：相位边沿 → 观测层；战斗中定期重算意图
// ---------------------------------------------------------------------------

void AArenaBotController::HandlePhaseTransition(EArenaPhase From, EArenaPhase To, AArenaPlayerState* Me)
{
	if (!Observer)
	{
		return;
	}

	// --- 进战斗 = 新的一回合 ---
	if (To == EArenaPhase::Combat)
	{
		Observer->BeginRound();
		RoundsLostAtRoundStart = Me ? Me->GetRoundsLost() : 0;

		// 让下一次 TickCombatPhase 立刻重算一次意图 —— 否则新回合的头几秒
		// 用的还是上一回合（甚至上一局）的数值。
		LastIntentRefreshTime = -1000.f;

		// 战术层的反连放记录也跟着回合清掉：回合之间隔着一段备战期，
		// 上一回合最后一秒放的那个技能不该压着这一回合的开局。
		SlotRetryAfter.Reset();
		CommittedSlot = FGameplayTag();
		PendingOutcomeSlot = FGameplayTag();
		if (TacticalPlanner)
		{
			TacticalPlanner->ResetCastHistory();
		}
	}

	// --- 进结算 = 这一回合打完了 ---
	if (To == EArenaPhase::Settlement && Me)
	{
		// Bot 的败场数涨了 ⇒ 对手（被观测的那个玩家）赢了这一回合。
		const bool bPlayerWonThisRound = Me->GetRoundsLost() > RoundsLostAtRoundStart;
		Observer->FoldRoundIntoProfile(bPlayerWonThisRound);
	}

	// --- 整局结束 = 写备忘录，然后把画像（含备忘录）落盘 ---
	if (To == EArenaPhase::MatchEnd)
	{
		// ===================================================================
		// 【这就是接 LLM 的那一行】
		//
		// 顺序不能反：先把备忘录写进画像，再存盘。反过来的话存下去的还是
		// 上一局的备忘录 —— 而这个错误的表现只是"Bot 的反应慢了一局"，
		// 没有任何报错，是那种要对着存档文件才能看出来的问题。
		//
		// 换成 LLM 时，需要改的只有 UArenaAIDirector::ComposeMemo 的实现
		// （去做一个蓝图子类覆写它）。这一行、观测层、战术层、下面那个存盘
		// 全都不用动。这里之所以把它写成一个同步调用，是因为整个流程本来就在
		// 整局结束的那个瞬间、没有实时性要求 —— 而如果将来要接一个真的会阻塞
		// 几百毫秒的网络调用，正确的做法是把这一行改成一个异步任务，
		// 在回调里再 EndMatchAndSave，而不是把它塞进 Tick 里。
		// ===================================================================
		if (StrategyDirector)
		{
			Observer->SetStrategicMemo(StrategyDirector->ComposeMemo(Observer->GetProfile()));
		}

		Observer->EndMatchAndSave();
	}
}

void AArenaBotController::RefreshIntent(const AActor* Hero, const AActor* Enemy)
{
	const FArenaPlayerProfile& Profile = Observer ? Observer->GetProfile() : FArenaPlayerProfile();

	if (StrategyDirector)
	{
		CurrentIntent = StrategyDirector->BuildIntent(
			Profile, ResolveHealthRatio(Hero), ResolveHealthRatio(Enemy));
	}

	// 战略层不存在时 CurrentIntent 保持默认（Balanced + 零偏移），
	// ResolveTuning 于是吐出纯基线 —— 最差就是原版 Bot。
	CurrentTuning = ResolveTuning(CurrentIntent);
}

FArenaBotTuning AArenaBotController::ResolveTuning(const FArenaStrategicIntent& Intent) const
{
	FArenaBotTuning Tuning;

	// 基线 + 偏移，然后夹到"物理上能成立"的范围。
	//
	// 【这里夹的是结果，不是偏移】战略层只知道偏移（见 ArenaAIDirector 的注释），
	// 不知道基线是多少 —— 所以"容错不能为负"这种约束只能在这里收口：
	// 基线 RangeTolerance 默认 30，一个 -40 的合法偏移会让结果变成 -10，
	// 而负的容错会让 TickCombatPhase 里那两个距离判断同时成立（既算太远又算太近），
	// 表现是 Bot 原地抽搐。
	Tuning.PreferredRange   = FMath::Max(0.f, PreferredRange + Intent.RangeOffset);
	Tuning.RangeTolerance   = FMath::Max(0.f, RangeTolerance + Intent.ToleranceOffset);
	Tuning.AttackInterval   = FMath::Max(0.1f, AttackInterval * Intent.AttackIntervalScale);
	Tuning.SkillInterval    = FMath::Max(0.1f, SkillInterval * Intent.SkillIntervalScale);
	Tuning.StrafeWeight     = FMath::Clamp(StrafeWeight + Intent.StrafeWeightOffset, 0.f, 1.f);
	Tuning.DecisionInterval = FMath::Max(0.02f, DecisionInterval * Intent.DecisionIntervalScale);

	return Tuning;
}

void AArenaBotController::DrawAIDebug(const AActor* Hero, const AActor* Enemy) const
{
	const UWorld* World = GetWorld();
	if (!World || !Hero)
	{
		return;
	}

	// 用颜色表达立场 —— 调试时一眼看出"它现在是不是在针对我"，比读一串数字快得多。
	FColor StanceColor = FColor::White;
	switch (CurrentIntent.Stance)
	{
	case EArenaAIStance::Brawl:   StanceColor = FColor::Red;     break;
	case EArenaAIStance::Poke:    StanceColor = FColor::Cyan;    break;
	case EArenaAIStance::Counter: StanceColor = FColor::Magenta; break;
	case EArenaAIStance::Chase:   StanceColor = FColor::Orange;  break;
	case EArenaAIStance::Balanced:
	default:                      StanceColor = FColor::Silver;  break;
	}

	const FVector Origin = Hero->GetActorLocation() + FVector(0.0, 0.0, 120.0);
	const float LifeTime = FMath::Max(IntentRefreshInterval, 0.1f);

	// 期望站位环（半径 = 当前实际使用的 PreferredRange）+ 容错上下界两个细环。
	// 环的半径就是"策略把 Bot 推到了哪"最直观的答案。
	const FVector YAxis(1.0, 0.0, 0.0);
	const FVector ZAxis(0.0, 1.0, 0.0);

	DrawDebugCircle(World, Origin, CurrentTuning.PreferredRange, 48, StanceColor,
		false, LifeTime, 0, 3.f, YAxis, ZAxis, false);

	DrawDebugCircle(World, Origin, CurrentTuning.PreferredRange + CurrentTuning.RangeTolerance, 48,
		FColor::Silver, false, LifeTime, 0, 1.f, YAxis, ZAxis, false);

	DrawDebugCircle(World, Origin, FMath::Max(0.f, CurrentTuning.PreferredRange - CurrentTuning.RangeTolerance), 48,
		FColor::Silver, false, LifeTime, 0, 1.f, YAxis, ZAxis, false);

	// 战术层把站位推到了哪（黄）。
	//
	// 【为什么要单独一个环】上面那两个银环是【战略层】想要的站位，这个黄环是
	// "叠加了战术层应激之后实际生效的"站位。两者不重合的时候，一眼就能看出
	// "Bot 现在这个站位不是我针对他调的、是它自己在躲"—— 排查"到底是哪一层
	// 在动"时，没有这个环就只能去读代码。
	const float TacticalRangeDelta = FMath::Clamp(
		CurrentDecision.PreferredRangeDelta, -MaxTacticalRangeDelta, MaxTacticalRangeDelta);
	const float EffectiveRange = FMath::Max(0.f, CurrentTuning.PreferredRange + TacticalRangeDelta);

	if (!FMath::IsNearlyZero(TacticalRangeDelta))
	{
		DrawDebugCircle(World, Origin, EffectiveRange, 48, FColor::Yellow,
			false, LifeTime, 0, 2.f, YAxis, ZAxis, false);
	}

	if (Enemy)
	{
		const FArenaPlayerProfile& P = Observer ? Observer->GetProfile() : FArenaPlayerProfile();

		// 对手此刻的姿势 + Bot 读到的绕圈习惯。
		//
		// 【为什么要单独一行】"读不到"和"读到了但没生效"是两种完全不同的问题，
		// 而它们在战术层那行理由里长得一样（都是没出现那几个标记）。
		// 把原始读数摆在这里，一眼就能分开：这行是"无"= 观测层没读到，
		// 这行有内容而战术行没标记 = 读到了但不该有反应。
		const FArenaEnemyActionState Action = Observer
			? Observer->GetEnemyActionState()
			: FArenaEnemyActionState();

		FString EnemyNote;
		if (Action.bHardControlled) { EnemyNote += TEXT(" 挨控"); }
		if (Action.bDamageProof)    { EnemyNote += TEXT(" 挡着"); }
		if (Action.bThreatLoaded)   { EnemyNote += TEXT(" 攒着大的"); }
		if (EnemyNote.IsEmpty())    { EnemyNote = TEXT(" 无"); }

		// 反切强度 = Bot 真去迎着他轨道走的概率。0 = 没有样本，退回随机绕圈。
		float OrbitSign = 0.f;
		float OrbitStrength = 0.f;
		GetPlayerOrbitBias(OrbitSign, OrbitStrength);

		// 参数顺序容易记错：…, TextLocation, Text, TestBaseActor, TextColor, Duration, bDrawShadow, FontScale
		DrawDebugString(World, Origin + FVector(0.0, 0.0, 40.0),
			FString::Printf(
				TEXT("立场=%d 压力=%.2f | 距离=%.0f(±%.0f) 攻=%.2f 技=%.2f 绕=%.2f\n")
				TEXT("画像: 贴脸%.2f 拉开%.2f 接近%.2f 技能%.2f/s 冷却占用%.2f 样本%d回合\n")
				TEXT("对手:%s | 他绕圈%.2f 反切强度%.2f\n")
				TEXT("战术: %s%s\n")
				TEXT("承诺: %s"),
				static_cast<int32>(CurrentIntent.Stance), CurrentIntent.Pressure,
				CurrentTuning.PreferredRange, CurrentTuning.RangeTolerance,
				CurrentTuning.AttackInterval, CurrentTuning.SkillInterval, CurrentTuning.StrafeWeight,
				P.CloseRangeRatio, P.FarRangeRatio, P.ApproachBias,
				P.SkillsPerSecond, P.SkillCooldownOccupancy, P.RoundsObserved,
				*EnemyNote, P.PlayerStrafeLeftRatio, OrbitStrength,
				*CurrentDecision.Reason,
				FMath::IsNearlyZero(TacticalRangeDelta)
					? TEXT("")
					: *FString::Printf(TEXT(" | 站位%+.0f"), TacticalRangeDelta),
				// 承诺时长能直接验证黏性有没有生效：数字一直在涨 = 没被抢走；
				// 反复从 0 开始 = 每拍都在换目标，那说明 CommitBonus 压不住。
				CommittedSlot.IsValid()
					? *FString::Printf(TEXT("%s 已 %.1fs"), *CommittedSlot.ToString(),
						FMath::Max(0.f, World->GetTimeSeconds() - CommittedSince))
					: TEXT("无")),
			nullptr, StanceColor, LifeTime, /*bDrawShadow=*/false, /*FontScale=*/1.f);
	}
}

void AArenaBotController::Idle()
{
	ClearFocus(EAIFocusPriority::Gameplay);
	CancelPendingRewardAnswer();
	// 不发移动输入就等于停下（CharacterMovement 没有输入时会按
	// BrakingDecelerationWalking 自己减速）。这里刻意不调 AAIController::StopMovement ——
	// 那个是给寻路用的，这个 Bot 没走寻路那条路。
}

void AArenaBotController::CancelPendingRewardAnswer()
{
	if (bAnswerQueued || GetWorldTimerManager().IsTimerActive(RewardAnswerHandle))
	{
		GetWorldTimerManager().ClearTimer(RewardAnswerHandle);
		bAnswerQueued = false;
	}
}

// ---------------------------------------------------------------------------
// 奖励选择阶段
// ---------------------------------------------------------------------------

void AArenaBotController::TickRewardPhase(AArenaPlayerState* Me)
{
	// 已经排上了就等它响。这一句必须在本函数最前面 ——
	// 见头文件里 bAnswerQueued 那段：漏了它每帧都会重排定时器，
	// 那个定时器永远不到期，AI 永远不选。
	if (bAnswerQueued)
	{
		return;
	}

	if (!Me->HasPendingPrompt())
	{
		return;
	}

	// 选项全都无效时干脆不排定时器：排了也会在到点时白跑一趟，
	// 而且会把这个人在奖励阶段多留 RewardAnswerDelay 秒。
	if (ChooseRewardOption(Me) == INDEX_NONE)
	{
		return;
	}

	bAnswerQueued = true;
	GetWorldTimerManager().SetTimer(RewardAnswerHandle, this, &AArenaBotController::SubmitRewardAnswer,
		FMath::Max(0.f, RewardAnswerDelay), /*bLoop=*/false);
}

int32 AArenaBotController::ChooseRewardOption(const AArenaPlayerState* Me) const
{
	const FArenaPendingPrompt& Prompt = Me->GetPendingPrompt();
	if (!Prompt.bActive)
	{
		return INDEX_NONE;
	}

	// 按动作分类收集候选下标，而不是"从头扫到第一个能用的"：
	// 优先级是【先拿奖励，再花锻造器，最后才走】—— 直接扫第一个会变成
	// "谁排前面拿谁"，而菜单里「进入战斗」永远排最后，一旦某一档排序变了
	// 就会出现在还没领奖励的时候直接进战斗。
	TArray<int32> Grants;
	TArray<int32> Forges;
	TArray<int32> Finishes;

	for (int32 Index = 0; Index < Prompt.Options.Num(); ++Index)
	{
		const FArenaChoiceOption& Option = Prompt.Options[Index];
		if (!Option.IsValidOption())
		{
			continue;   // 服务端配错的项，跳过（UI 那边也会把它画成不可点）
		}

		switch (Option.Action)
		{
		case EArenaPromptAction::Grant:    Grants.Add(Index);   break;
		case EArenaPromptAction::UseForge: Forges.Add(Index);   break;
		case EArenaPromptAction::Finish:   Finishes.Add(Index); break;
		}
	}

	// ① 本回合的奖励：三选一里随机挑一个。
	//
	// 【为什么是随机而不是"评估哪件更强"】需求只说 AI 要会 pick_item_from_three，
	// 没给任何估值口径。自己编一套"法强英雄该拿法强装备"的评分等于替需求做设计决定，
	// 而且那种代码一旦写下去，改数值就要改代码。随机是可解释的，也是可复现的坏运气。
	if (Grants.Num() > 0)
	{
		return Grants[FMath::RandRange(0, Grants.Num() - 1)];
	}

	// ② 锻造器：按概率用。全用掉会让第 1 回合就把次数花光，后面几回合空转。
	if (Forges.Num() > 0 && FMath::FRand() < ForgeUseChance)
	{
		return Forges[FMath::RandRange(0, Forges.Num() - 1)];
	}

	// ③ 只能走了。它在菜单里永远排最后，所以上面两条都落空时就是它。
	if (Finishes.Num() > 0)
	{
		return Finishes[0];
	}

	return INDEX_NONE;
}

void AArenaBotController::SubmitRewardAnswer()
{
	bAnswerQueued = false;

	AArenaPlayerState* Me = GetPlayerState<AArenaPlayerState>();
	if (!Me)
	{
		return;
	}

	// 到点的一瞬间再挑一次（而不是把下标记在定时器上）：
	// 等待的这一两秒里这份菜单理论上不会变，但多问一次是免费的，
	// 而且避免了"记着一个已经不存在的下标"。
	const int32 Index = ChooseRewardOption(Me);
	if (Index == INDEX_NONE)
	{
		return;
	}

	UWorld* World = GetWorld();
	AArenaGameMode* ArenaGameMode = World ? World->GetAuthGameMode<AArenaGameMode>() : nullptr;
	if (!ArenaGameMode)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] Bot 想提交选项 %d，但当前 GameMode 不是 AArenaGameMode —— 已忽略。"), Index);
		return;
	}

	// 直接调，不走 AArenaPlayerState::ServerSubmitChoice：
	// Bot 本来就在服务端，绕一圈 RPC 只会慢一帧（而且"服务端发给自己"的 RPC 发不出去）。
	// 两条入口最后进的是同一个函数。
	ArenaGameMode->ResolveChoice(Me, Index);
}

// ---------------------------------------------------------------------------
// 战斗阶段
// ---------------------------------------------------------------------------

void AArenaBotController::TickCombatPhase(float DeltaSeconds, AArenaGameState* GameState, AArenaPlayerState* Me)
{
	UWorld* World = GetWorld();
	AHeroCombatCharacter* Hero = Cast<AHeroCombatCharacter>(GetPawn());

	if (!World || !Hero || Hero->IsDead())
	{
		Idle();
		return;
	}

	APawn* Enemy = nullptr;
	if (const AArenaPlayerState* EnemyState = GameState->GetOpponentOf(Me))
	{
		Enemy = EnemyState->GetPawn();
	}

	if (!Enemy)
	{
		Idle();
		return;
	}

	// 对手已经倒下就别打了。
	//
	// 【为什么这一条是必须的】倒下到结算之间有 CombatPollInterval 那么一小段，
	// 而结算相位切换之后这个函数就不再跑了 —— 那一下打出去不会改写结果，
	// 但会在对面已经判负、正在播死亡动画的时候再补一刀，看起来像是"输了还挨打"。
	if (const AHeroCombatCharacter* EnemyHero = Cast<AHeroCombatCharacter>(Enemy))
	{
		if (EnemyHero->IsDead())
		{
			Idle();
			return;
		}
	}

	// --- 观测：把这一帧的玩家行为记进画像 ---
	//
	// 【放在这里而不是函数开头】前面那几个 early-return（自己死了 / 对手没 pawn /
	// 对手已经倒下）都是"这不是有效战斗帧"的情况。观测的目的是刻画玩家的打法，
	// 掺进"对面已经躺着我还在补刀"的那几帧只会污染距离统计。
	if (Observer)
	{
		Observer->ObserveCombatTick(DeltaSeconds, Hero, Enemy);
	}

	// 世界时间这一帧只取一次，下面战略 / 自省 / 战术都用它 ——
	// 各取一次的话，同一帧里会出现"意图按这个时刻刷的、决策按那个时刻算的"。
	const float Now = World->GetTimeSeconds();

	// --- 自省：上一次出手到底打中了没有 ---
	//
	// 【为什么放在决策块外面】它要每帧看，而决策块每 CurrentTuning.DecisionInterval
	// （默认 0.25 秒）才进一次。放进决策里的话，窗口的实际长度会随决策节奏抖 ——
	// 0.6 秒的窗口最坏变成 0.85 秒，于是下一次普攻更容易挤进来，
	// 把普攻的伤害记成技能的功劳。
	ResolveOutcomeCredit(Enemy, Now);

	// --- 战略：定期重算这一回合要怎么打 ---
	if (Now - LastIntentRefreshTime >= FMath::Max(IntentRefreshInterval, 0.1f))
	{
		LastIntentRefreshTime = Now;
		RefreshIntent(Hero, Enemy);
	}

	// 瞄准。这个项目的"瞄准方向"就是控制旋转（AHeroCombatCharacter::ResolveAimDirection），
	// 而 AAIController::Tick → UpdateControlRotation 会拿焦点算控制旋转
	// （AIController.cpp:453-461）。所以 SetFocus 一次就解决了朝向 + Pitch，
	// 不用自己算角度再 FaceRotation。
	SetFocus(Enemy, EAIFocusPriority::Gameplay);

	FVector ToEnemy = Enemy->GetActorLocation() - Hero->GetActorLocation();
	ToEnemy.Z = 0.f;   // 只做水平机动：不做垂直接近，那是飞行单位的事

	const float Distance = static_cast<float>(ToEnemy.Size());
	const FVector Direction = (Distance > KINDA_SMALL_NUMBER)
		? (ToEnemy / Distance)
		: Hero->GetActorForwardVector();   // 完全重叠时随便挑一个方向，别除零

	// 【注意下面用的全是 CurrentTuning 而不是那些 UPROPERTY】
	// 那些属性是【基线性格】，实际使用值是基线 + 战略层的偏移。
	// 直接读它们的话，战略层算了半天没有任何效果，而且不会有任何报错 ——
	// 表现只是"Bot 好像没在针对我"，那是这套系统最难查的失败模式。

	// --- 战术：一次算清"这一下要干什么"，移动和技能都读它的结论 ---
	//
	// 【为什么移动和技能共用一次决策】它们本来就是同一个问题（"此刻该怎么打"）
	// 的两个答案。分开算会出现"想拉开距离、同时又在往前冲"这种自相矛盾的一帧，
	// 而且要多算一次战术层。合并之后双方读的是同一份战况快照。
	//
	// 【为什么决策按这个节奏而不是每帧】每帧重算的话，绕圈方向会在"两个位置之间
	// 来回"时抖成锯齿（每帧算出来的叉积方向在临界点上会翻），表现是 Bot 原地哆嗦。
	//
	// ⚠️ 这个闸门只该管【决策】。移动的施加必须在它外面、每帧一次 ——
	// AddMovementInput 攒的那一笔只活一帧，放进来的话 Bot 会迈不开腿。
	// 详见下面"移动"那段的注释。
	if (Now - LastDecisionTime >= CurrentTuning.DecisionInterval)
	{
		LastDecisionTime = Now;

		RefreshTacticalDecision(Hero, Enemy, Now);

		if (Now - LastStrafeFlipTime >= StrafeFlipInterval)
		{
			LastStrafeFlipTime = Now;

			// 换绕圈方向。没有样本时就是原来的随机（"看起来不像在绕固定轨道"），
			// 但玩家如果有明显的绕圈习惯，就往【他的反方向】切 ——
			// 他往左转我也往左跟，等于陪他转圈、距离和方位都不变；
			// 迎着他切才会把他的轨道掐断，逼他改向或者直接撞上来。
			//
			// 【为什么用概率而不是直接定死】习惯强度本身就是"他有多大概率还这么走"。
			// 拿它当反向概率，于是"他 80% 往左" → "我 80% 往右迎"，
			// 剩下那 20% 保留随机 —— 既读了他，又不会变成一台可以被反向利用的机器。
			float OrbitSign = 0.f;
			float OrbitStrength = 0.f;
			GetPlayerOrbitBias(OrbitSign, OrbitStrength);

			StrafeSign = (FMath::FRand() < OrbitStrength)
				? OrbitSign
				: (FMath::RandBool() ? 1.f : -1.f);
		}

		// --- 技能：由战术层决定放不放、放哪个 ---
		//
		// 【两道门，管的是两件不同的事】
		//   SkillInterval 管"多久能放一次"（出手的频率上限，是性格）；
		//   CurrentDecision.bCastSkill 管"这一下值不值得放"（是判断）。
		// 原来的实现只有第一道门 —— 到点就随便放一个，这正是被替掉的那套轮询。
		if (CurrentDecision.bCastSkill && Now - LastSkillTime >= CurrentTuning.SkillInterval)
		{
			LastSkillTime = Now;

			// 【为什么这里必须打一行】在此之前 Bot 的决策在日志里是完全隐形的：
			// 唯一相关的那行 [Ability] 是玩家和 Bot 共用同一个入口打的，
			// 分不出是谁按的，也看不到理由。于是"AI 到底有没有在按评分打"
			// 这个问题只能靠肉眼盯屏幕（DrawAIDebug 只画在屏幕上，不落日志）。
			// 这一行把它变成可 grep 的：谁、放了什么、为什么。
			UE_LOG(LogTemp, Log, TEXT("[ArenaAI] 出手 %s | %s"),
				*CurrentDecision.SlotTag.ToString(), *CurrentDecision.Reason);

			ExecuteTacticalDecision(Hero, Enemy, CurrentDecision, Now);
		}
	}

	// --- 移动 ---
	//
	// 【为什么它在上面那个决策闸门【外面】，每帧都发】
	// AddMovementInput 不是"设置移动方向"，而是往 ControlInputVector 里攒一笔，
	// 而移动组件的 ConsumeInputVector() 【每帧都会把它清零】。也就是说这一笔的
	// 有效期只有一帧 —— 下一帧不发，输入就是 0。
	//
	// 原先这整段写在上面那条 if (Now - LastDecisionTime >= DecisionInterval) 里面，
	// 于是输入每秒只发 4 次、每次只活一帧（0.25s 的间隔 vs 16ms 的帧长），
	// 有效移动的占空比大约 7%。表现就是 Bot 迈不开腿：站在原地点一点地抽，
	// 永远拉不近也追不上 —— 而所有站位参数看起来都是对的，日志里也没有任何异常，
	// 因为"决策"确实每次都在正确地产出，"执行"才是不生效的那一半。
	//
	// 【那个闸门本身是对的，别把它一起去掉】它防的是战术层每帧重算时绕圈方向
	// 在临界点上翻来翻去（见上面那段注释）。所以这里的分工是：
	// 决策（算得慢、要防抖）留在闸门里，执行（发得勤、发得晚就没用）搬出来。
	// 搬出来用的是 CurrentDecision / CurrentTuning 这些成员，它们就是"上一拍的结论"。
	//
	// 战术层的临时位移夹一次再叠上去。
	//
	// 【为什么执行层还要夹，战术层不是已经夹过了吗】和 ResolveTuning 同理：
	// 给出偏移的那一层不知道基线距离是多少，所以"最多能推多远"这种只有
	// 拿到基线才能判的约束必须在这里收口。这一句也兜住了"蓝图子类覆写
	// PlanTactics 时忘了夹"的情况。
	const float TacticalRangeDelta = FMath::Clamp(
		CurrentDecision.PreferredRangeDelta, -MaxTacticalRangeDelta, MaxTacticalRangeDelta);
	const float EffectiveRange = FMath::Max(0.f, CurrentTuning.PreferredRange + TacticalRangeDelta);

	float Forward = 0.f;
	if (Distance > EffectiveRange + CurrentTuning.RangeTolerance)
	{
		Forward = 1.f;    // 太远 → 靠近
	}
	else if (Distance < FMath::Max(0.f, EffectiveRange - CurrentTuning.RangeTolerance))
	{
		Forward = -1.f;   // 太近 → 后退
	}
	// 在容限之内既不进也不退，只绕圈 —— 1V1 里"贴着对手原地转"比"一直贴脸"更像人

	const FVector Strafe = FVector::CrossProduct(FVector::UpVector, Direction) * StrafeSign;
	FVector Move = Direction * Forward + Strafe * CurrentTuning.StrafeWeight;

	if (!Move.IsNearlyZero())
	{
		// 归一化之后再发：APawn 那边会把输入向量夹到长度 1，
		// 不归一的话"后退 + 绕圈"叠出来的长度是 1.45，会被夹成满速前进的观感，
		// 而只有一个分量时又是半速 —— 速度会随方向莫名其妙地变。
		Hero->AddMovementInput(Move.GetSafeNormal());
	}

	// --- 普攻 + 连招 ---
	//
	// 【为什么格挡里的对手不打】格挡窗口内打上去的伤害会被完全吃掉
	// （UBlockComponent::BlockedDamageMultiplier 默认 0），而格挡窗口很短、
	// 之后跟着 6 秒冷却 —— 等它过去再打才是对的。而且那 6 秒里战术层会自动
	// 拿到"他刚交完"的加成（State.Cooldown.Block 本来就在观测层的冷却表里），
	// 于是"等他交完格挡再打"这件事不需要另写一条规则。
	//
	// 注意这一条要连 LastAttackTime 一起跳过：只拦住 BasicAttackPressed
	// 而照旧推进 LastAttackTime 的话，格挡一结束反而要先白等一个攻击间隔。
	//
	// 【普攻在这里被拆成两条时间尺度完全不同的路】
	//   起手 —— 仍然按 CurrentTuning.AttackInterval 的节奏（和以前一样）；
	//   续段 —— 踩连段窗口 State.ComboWindow。
	// 窗口由 GA_ThreeHitPassive 自己的定时器开合，长度是
	// (ChainWindowCloseTime - ChainWindowOpenTime) × 蒙太奇长度，典型 0.1~0.3 秒，
	// 比任何用得上手的"攻击间隔"都短一个数量级。所以续段必须【边沿触发】：
	// 窗口打开的那一帧决定这一下接不接，接就把"该按的时刻"定在反应延迟之后。
	//
	// 在改成这样之前，这里只有一条 `Now - LastAttackTime >= AttackInterval`（默认 1.1 秒）：
	// 用 1.1 秒的网格去撞 0.1~0.3 秒的窗口，撞不上是必然，而且撞不上之后要白等一整个间隔
	//（那个间隔比技能自己的寿命还长）—— 于是每一下都是"重开第 0 段"，
	// 日志里就是连着几十条 `第0段窗口关闭: 已排队=0`，连招一次都打不出来。
	const bool bEnemyDamageProof = Observer && Observer->GetEnemyActionState().bDamageProof;

	const UAbilitySystemComponent* HeroASC = UMyAbilitySystemComponent::FindAbilitySystemComponent(Hero);
	const bool bChainWindowOpen = HeroASC != nullptr
		&& HeroASC->HasMatchingGameplayTag(LOLGameplayTags::State_ComboWindow);

	// 完美窗口（QTE）：本段有没有（Armed，起手段就知道）、以及现在开没开（Open，按下去就是完美）。
	// 见 GA_ThreeHitPassive::SchedulePerfectWindowTags —— 只有剑套第 1 段配了完美窗口，
	// 所以 Armed 只在那一小段里挂着，其余段恒为 false。
	const bool bPerfectArmed = HeroASC != nullptr
		&& HeroASC->HasMatchingGameplayTag(LOLGameplayTags::State_PerfectWindowArmed);
	const bool bPerfectOpen = HeroASC != nullptr
		&& HeroASC->HasMatchingGameplayTag(LOLGameplayTags::State_PerfectWindow);

	if (bChainWindowOpen && !bChainWindowWasOpen)
	{
		// 窗口刚打开：这一下接不接。三个理由让它可能不接 ——
		//   ① 这一套的配额用完了 / 还没起手（ChainHitsLeft <= 0）；
		//   ② 血线低于地板且对面手里还有牌 → 这一段会直接送进对面的反打里；
		//   ③ ChainDropChance：接满是理论最优，但不是人的行为。会连的人也会停手。
		const bool bTooRisky = ChainHealthFloor > 0.f
			&& ResolveHealthRatio(Hero) < ChainHealthFloor
			&& Observer && !Observer->IsEnemyOnCooldown();
		const bool bKeepGoing = ChainHitsLeft > 0
			&& !bTooRisky
			&& (ChainDropChance <= 0.f || FMath::FRand() > ChainDropChance);

		if (bKeepGoing)
		{
			if (bPerfectArmed)
			{
				// 本段有完美窗口 → 【先不按】。完美判定只认连段窗口内的第一次按键
				//（GA_ThreeHitPassive::OnAttackInput：bQueuedNextStage 之后就 return），
				// 照旧在窗口一开就按掉，等于把这一段的完美机会主动扔掉。
				// -1 = 挂着等，下面按完美窗口的开/关把它补齐。
				NextChainPressTime = -1.f;
			}
			else
			{
				NextChainPressTime = Now + ChainReactionDelay
					+ (ChainReactionJitter > 0.f ? FMath::FRand() * ChainReactionJitter : 0.f);
			}
		}
		else
		{
			ChainHitsLeft = 0;
			NextChainPressTime = -1.f;
		}
	}
	else if (!bChainWindowOpen && bChainWindowWasOpen)
	{
		// 【下降沿不等于连段断了】关窗口和起下一段是同一帧里连着做的
		//（CloseChainWindow 末尾就调 StartStage(StageIndex + 1)），所以【已经按出去的那一次】
		// 一样会看到下降沿：标签摘掉 → 下一段的窗口要等 OpenTime 之后才挂上。
		// 这里如果无条件清零，每接一下都会把配额清空 → 表现和"一段都连不出来"完全一样，
		// 正好把这个修复又抵消掉。所以只在"窗口关了、我们还没按"时才算这套断了。
		if (NextChainPressTime >= 0.f)
		{
			// 决定不接 / 出了射程 / 窗口比反应时间还短 —— 这一套到此为止。
			// 不在这里收的话配额会一直挂着，Bot 会在下一次起手时莫名其妙多贴一段。
			ChainHitsLeft = 0;
		}
		NextChainPressTime = -1.f;
	}
	bChainWindowWasOpen = bChainWindowOpen;

	// 等完美窗口的那一按在这里补齐（NextChainPressTime < 0 就是"挂着等"）。
	if (NextChainPressTime < 0.f && ChainHitsLeft > 0 && bChainWindowOpen)
	{
		if (bPerfectOpen && !bPerfectWindowWasOpen)
		{
			// 完美窗口刚开：这就是完美时刻。
			NextChainPressTime = Now + PerfectPressDelay
				+ (PerfectPressJitter > 0.f ? FMath::FRand() * PerfectPressJitter : 0.f);
		}
		else if (bPerfectWindowWasArmed && !bPerfectArmed)
		{
			// 兜底：完美窗口已经过去（Armed 摘掉）而我们还没按 —— 立刻按，别白丢这一段连段。
			// 这一刻按下去仍然在连段窗口里（完美窗口是连段窗口的子区间，见 ComputePerfectWindow），
			// 所以至少能接出【普通】下一段；完美没了，但连段还在。
			// 没有这条兜底的话，一次掉帧 / 窗口比反应时间还窄就会让 Bot 永远停在"等"上，
			// 表现是连段从第 1 段开始就再也接不下去。
			NextChainPressTime = Now;
		}
	}
	bPerfectWindowWasArmed = bPerfectArmed;
	bPerfectWindowWasOpen = bPerfectOpen;

	const bool bReadyForChain = bChainWindowOpen && NextChainPressTime >= 0.f && Now >= NextChainPressTime;
	const bool bReadyForNewSwing = !bChainWindowOpen && Now - LastAttackTime >= CurrentTuning.AttackInterval;

	// 【起手也看血线】上面那条只拦续段；血线见底的时候起手一套同样是送。
	// 判据和续段一致：血线低于地板、且对面手里有牌（不在冷却）就不起手 ——
	// 对面刚交完技能的时候例外，那是"残血也要抢这一下"的窗口。
	const bool bNewSwingAllowed = ChainHealthFloor <= 0.f
		|| ResolveHealthRatio(Hero) >= ChainHealthFloor
		|| (Observer && Observer->IsEnemyOnCooldown());

	if (!bEnemyDamageProof && Distance <= AttackRange && (bReadyForChain || (bReadyForNewSwing && bNewSwingAllowed)))
	{
		LastAttackTime = Now;

		if (bReadyForChain)
		{
			--ChainHitsLeft;
			NextChainPressTime = -1.f;
		}
		else
		{
			// 起手一套新的。ChainLength 含起手那一下，所以剩下的续段是 ChainLength - 1；
			// 持剑三段 / 空手四段都在最后一段不配连段窗口，配额没用完也不会多按 ——
			// 那时候根本没有窗口上升沿，接不下去是设计如此。
			ChainHitsLeft = FMath::Max(0, ChainLength - 1);
		}

		Hero->BasicAttackPressed();
	}

	if (bDrawDebug)
	{
		DrawDebugDirectionalArrow(World, Hero->GetActorLocation(), Hero->GetActorLocation() + Direction * 150.f,
			40.f, FColor::Yellow, false, CurrentTuning.DecisionInterval);
		DrawDebugLine(World, Hero->GetActorLocation(), Enemy->GetActorLocation(),
			Distance <= AttackRange ? FColor::Red : FColor::Silver, false, CurrentTuning.DecisionInterval);
	}

	if (bDrawAIDebug)
	{
		DrawAIDebug(Hero, Enemy);
	}
}

// ---------------------------------------------------------------------------
// 战术层：整理输入 → 问一次 → 执行
// ---------------------------------------------------------------------------

FArenaTacticalContext AArenaBotController::BuildTacticalContext(
	const AActor* Hero, const AActor* Enemy, float Now) const
{
	FArenaTacticalContext Context;

	if (Hero && Enemy)
	{
		FVector ToEnemy = Enemy->GetActorLocation() - Hero->GetActorLocation();
		ToEnemy.Z = 0.f;
		Context.Distance = static_cast<float>(ToEnemy.Size());
	}

	// 注意：LWC 下 FVector 的分量是 double，Size() 也是 double，
	// 所以上面显式收窄。不窄的话会喂出一串和真正问题无关的模板实例化报错，
	// 见 ue5-lwc-double-float-gotchas。

	Context.AttackRange     = AttackRange;
	Context.SelfHealthRatio = ResolveHealthRatio(Hero);
	Context.EnemyHealthRatio = ResolveHealthRatio(Enemy);
	Context.SelfEnergyRatio = ResolveEnergyRatio(Hero);
	Context.Now = Now;

	// --- 他手里有没有牌 ---
	//
	// 两个来源，各管一头：
	//   bEnemyOnCooldown   —— 此刻的事实（他刚交完），驱动"这一下要不要抢"；
	//   EnemySkillReadiness —— 跨回合的习惯（他留不留技能），驱动"站位要不要保守"。
	// 前者缺失时观测层给 false（保守），后者是画像里的平均值（无样本时是 0
	// → readiness 1 → 也偏保守）。两个方向的兜底都是保守，这是一致的。
	if (Observer)
	{
		Context.bEnemyOnCooldown = Observer->IsEnemyOnCooldown();
		Context.EnemySkillReadiness = 1.f - FMath::Clamp(
			Observer->GetProfile().SkillCooldownOccupancy, 0.f, 1.f);

		// 他此刻的姿势（硬控 / 挡着 / 攒着大的）。拿到的是本帧刚读出来的值 ——
		// 和上面两条同一个来源、同一个时刻，不会出现"资源是新的、动作是旧的"。
		Context.EnemyAction = Observer->GetEnemyActionState();

		// 他连着多久什么都没做。上面三条都要等他先出手才读得到，
		// 这一条是唯一"他没动作"也能给出信息的输入 —— 见字段自己的注释。
		Context.EnemyIdleSeconds = Observer->GetEnemyIdleSeconds();
	}
	else
	{
		Context.bEnemyOnCooldown = false;
		Context.EnemySkillReadiness = 1.f;
		// EnemyAction 保持默认（全 false）= 没读到，评分类项全中性。
	}

	Context.Pressure = CurrentIntent.Pressure;
	Context.Stance   = CurrentIntent.Stance;

	// 上一拍的承诺。注意是【上一拍】—— 本拍的结果在 PlanTactics 返回之后才写回，
	// 所以战术层看到的是自己上一次的输出，这正是"黏性"需要的方向。
	Context.CommittedSlot = CommittedSlot;

	return Context;
}

TArray<FArenaAbilityCandidate> AArenaBotController::BuildCandidates(
	const AHeroCombatCharacter* Hero, float Now) const
{
	TArray<FArenaAbilityCandidate> Candidates;

	if (!AbilitySlots || !Hero)
	{
		return Candidates;   // 没配就是只普攻，不是错误
	}

	const UMyAbilitySystemComponent* ASC =
		Cast<UMyAbilitySystemComponent>(Hero->GetAbilitySystemComponent());
	if (!ASC)
	{
		return Candidates;
	}

	for (const FAbilityInputAction& Entry : AbilitySlots->AbilityInputActions)
	{
		if (!Entry.SlotTag.IsValid())
		{
			continue;
		}

		FArenaAbilityCandidate Candidate;
		Candidate.SlotTag = Entry.SlotTag;

		// --- 硬门一：槽位授权 ---
		//
		// 【为什么先自己查一遍】ASC->AbilityInputTagPressed 在槽位未授权时会打一条
		// Warning（MyAbilitySystemComponent.cpp:22）—— 每个技能周期一条，整场比赛刷屏。
		const FGameplayAbilitySpecHandle Handle = ASC->GetHandleForSlot(Entry.SlotTag);
		if (!Handle.IsValid())
		{
			Candidates.Add(Candidate);   // bUsable 保持 false，让它作为"为什么没选它"的可见证据
			continue;
		}

		const UMyGameplayAbility* Ability = ASC->GetAbilityForSlot(Entry.SlotTag);
		if (!Ability)
		{
			Candidates.Add(Candidate);
			continue;
		}

		Candidate.bManualTarget = Ability->bManualTargetSelect;

		// --- 硬门二：冷却 ---
		const bool bOnCooldown = IsSlotOnCooldown(ASC, Ability);

		// --- 硬门三：刚刚试过、没发出来 ---
		//
		// 这条必须由执行层提供（见 SlotRetryAfter 的注释）：只有它知道自己按过。
		// 折进 bUsable 而不是让战术层去罚分，是因为"发不出去"是一个事实，
		// 不是一个偏好 —— 让战术层去评估它等于让它猜一个它无从知道的东西。
		bool bRecentlyFailed = false;
		if (const float* RetryAfter = SlotRetryAfter.Find(Entry.SlotTag))
		{
			bRecentlyFailed = Now < *RetryAfter;
		}

		Candidate.bUsable = !bOnCooldown && !bRecentlyFailed;

		Candidates.Add(Candidate);
	}

	return Candidates;
}

void AArenaBotController::RefreshTacticalDecision(const AActor* Hero, const AActor* Enemy, float Now)
{
	if (!TacticalPlanner)
	{
		// 没有战术层 = 不放技能。保持默认决策（bCastSkill = false），
		// 于是一切退化成"只普攻 + 按基线走位"—— 一个能跑、能看、不会崩的降级形态。
		CurrentDecision = FArenaTacticalDecision();
		return;
	}

	const FArenaTacticalContext Context = BuildTacticalContext(Hero, Enemy, Now);
	const TArray<FArenaAbilityCandidate> Candidates = BuildCandidates(
		Cast<AHeroCombatCharacter>(Hero), Now);

	CurrentDecision = TacticalPlanner->PlanTactics(Context, Candidates);

	UpdateCommitment(Now);
}

void AArenaBotController::UpdateCommitment(float Now)
{
	// 这一拍打算放技能 → 承诺到它身上；不打算放 → 撤销承诺。
	//
	// 【撤销的那一支为什么是必要的】不撤销的话，Bot 会在"这一刻决定不放"
	// 之后仍然给上一个技能加分，于是下一拍它更容易被重新选中 —— 承诺会
	// 变成"永久偏向某个技能"，那就不是黏性而是偏心了。
	if (!CurrentDecision.bCastSkill || !CurrentDecision.SlotTag.IsValid())
	{
		CommittedSlot = FGameplayTag();
		return;
	}

	if (CurrentDecision.SlotTag != CommittedSlot)
	{
		CommittedSlot = CurrentDecision.SlotTag;
		CommittedSince = Now;
	}
}

// ---------------------------------------------------------------------------
// 读人：绕圈倾向
// ---------------------------------------------------------------------------

void AArenaBotController::GetPlayerOrbitBias(float& OutSign, float& OutStrength) const
{
	// 中性默认：没有倾向、强度 0 —— 调用方据此退回原来的随机绕圈。
	OutSign = 0.f;
	OutStrength = 0.f;

	// 和战略层同一道门：样本不够就当不知道。绕圈统计在几个回合里就能收敛，
	// 但那几个回合里 Bot 的读法会一直变，看起来像在抽搐，而不是在读人。
	if (!Observer || !Observer->GetProfile().HasEnoughSamples())
	{
		return;
	}

	// 0.5 = 往两边转的时长一样多 = 不可预判。偏离 0.5 越远，他绕得越有方向性。
	const float Ratio = FMath::Clamp(Observer->GetProfile().PlayerStrafeLeftRatio, 0.f, 1.f);
	OutStrength = FMath::Clamp(FMath::Abs(Ratio - 0.5f) * 2.f, 0.f, 1.f);

	// 【符号为什么是反的】观测层算的是"他的横向速度落在 +LateralAxis 上的时长占比"，
	// 而 LateralAxis = Cross(Up, 我→他)，和本文件里算绕圈用的那根轴是同一根
	// （两边都是从 Bot 指向对手）。所以 >0.5 = 他习惯朝 +L 走，
	// 而把 StrafeSign 取 -1 就是让 Bot 朝 -L 走 —— 正好迎着他的轨道切过去。
	OutSign = (Ratio > 0.5f) ? -1.f : 1.f;
}

// ---------------------------------------------------------------------------
// 自省：出手之后看结果
// ---------------------------------------------------------------------------

void AArenaBotController::ArmOutcomeCredit(const FGameplayTag& SlotTag, const AActor* Enemy, float Now)
{
	PendingOutcomeSlot = SlotTag;
	PendingOutcomeDeadline = Now + FMath::Max(0.1f, OutcomeCreditWindow);
	PendingOutcomeHealthAtCast = ResolveHealthRatio(Enemy);
}

void AArenaBotController::ResolveOutcomeCredit(const AActor* Enemy, float Now)
{
	if (!PendingOutcomeSlot.IsValid() || !TacticalPlanner)
	{
		return;
	}

	// 掉血超过门槛 = 打中了。用【比例差】而不是绝对血量：最大血量每回合都在涨
	// （每回合 +3 级），绝对值的门槛会随局数慢慢失准。
	const float HealthNow = ResolveHealthRatio(Enemy);
	if (HealthNow <= PendingOutcomeHealthAtCast - FMath::Max(0.f, OutcomeHitHealthDelta))
	{
		TacticalPlanner->NotifyOutcome(PendingOutcomeSlot, /*bHit=*/true);
		PendingOutcomeSlot = FGameplayTag();
		return;
	}

	// 窗口过了还没掉血 = 没打中。每帧都被调，所以最迟在截止后的第一帧判出来。
	if (Now >= PendingOutcomeDeadline)
	{
		TacticalPlanner->NotifyOutcome(PendingOutcomeSlot, /*bHit=*/false);
		PendingOutcomeSlot = FGameplayTag();
	}
}

void AArenaBotController::ExecuteTacticalDecision(
	AHeroCombatCharacter* Hero, AActor* Enemy, const FArenaTacticalDecision& Decision, float Now)
{
	if (!Hero || !Decision.bCastSkill || !Decision.SlotTag.IsValid())
	{
		return;
	}

	UMyAbilitySystemComponent* ASC = Cast<UMyAbilitySystemComponent>(Hero->GetAbilitySystemComponent());
	if (!ASC)
	{
		return;
	}

	const FGameplayAbilitySpecHandle Handle = ASC->GetHandleForSlot(Decision.SlotTag);
	if (!Handle.IsValid())
	{
		// 候选里已经过滤过这一条了，能走到这里说明配的在两次查询之间被撤了。
		// 不记 SlotRetryAfter —— 那是"试过没成功"，这里根本没试。
		return;
	}

	const UMyGameplayAbility* Ability = ASC->GetAbilityForSlot(Decision.SlotTag);
	if (!Ability)
	{
		return;
	}

	if (Ability->bManualTargetSelect)
	{
		// 「按住选目标」那一类（GA_DeathHarvest）：输入层那条路要一个准星射线
		// （AHeroCombatCharacter::RouteManualTargetConfirm → TraceManualTargetUnderCrosshair），
		// 而 Bot 没有相机。所以直接走服务端那一半，把对手当目标 ——
		// 和真人点中对手之后进的是同一个函数（SubmitManualTargetOnServer）。
		if (ASC->SubmitManualTargetOnServer(Decision.SlotTag, Enemy))
		{
			if (TacticalPlanner)
			{
				TacticalPlanner->NotifyCast(Decision.SlotTag, Now);
			}

			// 这一条真的提交出去了 → 开始等结果（打中了没有）。
			ArmOutcomeCredit(Decision.SlotTag, Enemy, Now);
			return;
		}

		// 发不出去（槽位没声明事件触发标签、载荷为空之类）→ 临时拉黑这个槽位，
		// 让战术层下一次去选别的。不拉黑的话它会每个技能周期都被选中、
		// 每次都静默失败，表现是"Bot 站着不放技能"且日志上看不出原因。
		SlotRetryAfter.Add(Decision.SlotTag, Now + FMath::Max(0.1f, FailedCastRetryDelay));
		return;
	}

	// 走和真人按键完全相同的入口。能力自己会判冷却/射程/沉默，
	// 判不过就静默不激活 —— 这里不需要（也不该）替它判一遍。
	Hero->AbilityInputTagPressed(Decision.SlotTag);

	if (TacticalPlanner)
	{
		TacticalPlanner->NotifyCast(Decision.SlotTag, Now);
	}

	// 开始等结果。
	//
	// 【为什么不判断"能力到底激活了没有"】取不到 —— AbilityInputTagPressed 只表达
	// "我按了"，能力判不过就静默不激活。所以"按了但没放出来"和"放出来了但没打中"
	// 在这里是同一件事，都会被记成一次没打中。这是可接受的：两者的结论一样
	// （这个技能在当前情况下不管用），而区分它们需要另一条不存在的回执链路。
	ArmOutcomeCredit(Decision.SlotTag, Enemy, Now);
}
