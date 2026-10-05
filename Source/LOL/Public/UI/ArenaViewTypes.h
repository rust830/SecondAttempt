// 竞技场 UI 的数据契约：视图结构与委托声明的唯一出处。
//
// 【和 HUDTypes.h 同一条纪律】这里不出现任何 GAS 类型 —— 不 include AbilitySystemComponent.h /
// GameplayEffect.h / GameplayTagContainer.h，也不用 FGameplayAttribute / FGameplayTag。
// Widget 只认下面这些结构体：它们全是"已经能直接画上去"的成品（文案排好了、图标选好了、百分比算好了）。
//
// 【为什么不直接复用 GAS/ArenaTypes.h 里的 FArenaPendingPrompt】
// 那个结构体是【服务端的结算载荷】：它带着 Item / Augment 两个软引用和一个 Action 枚举，
// 因为服务端要靠它们决定"这一次点击到底发什么"。而 UI 需要的东西正好相反：
//   · UI 不该知道 Action（那是 GameplayMode 的分派语义）—— 它只需要"这张卡能不能点"；
//   · UI 不需要那两个软引用（客户端从不去解析它们，见 ArenaPlayerState.h 的注释）。
// 直接复用的话，UI 会拿到一份"里面装着它不该碰的东西"的结构，而那种结构迟早会被误用。
// 代价只是一次很短的搬运（见 UArenaHUDController::BuildPromptView）。
//
// 【槽位身份是 int32 序号】同 HUDTypes：不引入 FGameplayTag。

#pragma once

#include "CoreMinimal.h"
#include "ArenaViewTypes.generated.h"

class UTexture2D;

/**
 * 卡片品质的三档表现 + 无品质。
 *
 * 【为什么不是直接用 GAS/ArenaTypes.h 里的 EArenaAugmentTier】同本文件的总纪律：
 * UI 契约不 include GAS 头。翻译层（ArenaHUDController）在搬运时把 GAS 侧的品质
 * 映射成这一侧的档位；None 表示"这张卡不做品质表现"（锻造器菜单、「进入战斗」等）。
 */
UENUM(BlueprintType)
enum class EArenaCardTier : uint8
{
	/** 无品质表现（普通按钮 / 锻造器菜单）。 */
	None			UMETA(DisplayName = "无"),

	/** 白银档：银色边框。 */
	Silver			UMETA(DisplayName = "白银"),

	/** 黄金档：金色边框。 */
	Gold			UMETA(DisplayName = "黄金"),

	/** 棱彩档：青紫渐变边框。 */
	Prismatic		UMETA(DisplayName = "棱彩"),
};

/**
 * 相位的 UI 侧镜像。
 *
 * 【为什么不是直接用 GAS/ArenaTypes.h 里的 EArenaPhase】同本文件的总纪律：
 * UI 契约不 include GAS 头。翻译层做一次一对一映射，GAS 侧加相位时这里
 * 同步加一条（漏加的那条会被映射成 Unknown，编译期就能看见要补哪里）。
 * 命名对齐 LoL 的叫法：RewardSelection 对应"备战"（Planning）。
 */
UENUM(BlueprintType)
enum class EArenaPhaseView : uint8
{
	WaitingToStart	UMETA(DisplayName = "未开始"),
	Planning		UMETA(DisplayName = "备战"),
	Combat			UMETA(DisplayName = "战斗"),
	Settlement		UMETA(DisplayName = "结算"),
	MatchEnd		UMETA(DisplayName = "比赛结束"),
	Unknown			UMETA(DisplayName = "未知"),
};

/**
 * 回合线上"一格是什么"。
 *
 * 【值和 GAS 侧 EArenaStageKind 一一对应】不加不减不改名 ——
 * 翻译层按值映射，两边错位一格就是错一个图标，对不上号会被玩家看见。
 */
UENUM(BlueprintType)
enum class EArenaStageKindView : uint8
{
	/** 战斗回合（含"无奖励"回合）。 */
	Combat			UMETA(DisplayName = "战斗"),

	/** 海克斯三选一回合。 */
	Augments		UMETA(DisplayName = "海克斯"),

	/** 装备回合。 */
	ItemPurchase	UMETA(DisplayName = "装备"),

	/** 属性锻造器回合。 */
	StatAnvil		UMETA(DisplayName = "锻造"),
};

/** 回合线上一格的状态（LoL 的 completed / current / upcoming 三态）。 */
UENUM(BlueprintType)
enum class EArenaStageState : uint8
{
	Completed	UMETA(DisplayName = "已完成"),
	Current		UMETA(DisplayName = "进行中"),
	Upcoming	UMETA(DisplayName = "未开始"),
};

/** 回合线上的一格。 */
USTRUCT(BlueprintType)
struct FArenaStageEntryView
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	EArenaStageKindView Kind = EArenaStageKindView::Combat;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	EArenaStageState State = EArenaStageState::Upcoming;

	bool EqualsForUI(const FArenaStageEntryView& Other) const
	{
		return Kind == Other.Kind && State == Other.State;
	}
};

/**
 * 回合线（round tracker）视图：整场比赛的回合计划 + 打到哪了。
 *
 * 【bValid 为 false 时整个回合线收起】和"未就绪不画空血条"同一条纪律：
 * 计划还没推下来（开局一瞬间）或这张图没有回合计划时，不画一排空格子。
 */
USTRUCT(BlueprintType)
struct FArenaRoundTrackerView
{
	GENERATED_BODY()

	/** false = 没有回合计划（还没开局 / 服务端没推）。UI 收起整条。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bValid = false;

	/** 顺序 = 回合顺序，下标 0 = 第 1 回合。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	TArray<FArenaStageEntryView> Stages;

	/** 打到第几回合（1 起）。0 = 还没开始，此时全部画 Upcoming。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 CurrentRound = 0;

	bool EqualsForUI(const FArenaRoundTrackerView& Other) const
	{
		if (bValid != Other.bValid || CurrentRound != Other.CurrentRound
			|| Stages.Num() != Other.Stages.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Stages.Num(); ++Index)
		{
			if (!Stages[Index].EqualsForUI(Other.Stages[Index]))
			{
				return false;
			}
		}
		return true;
	}
};

/**
 * 三选一里的一张卡（也可能是「进入战斗」那颗按钮）。
 *
 * 全是可直接显示的东西：Label / Description / Icon 由服务端拼好后一路带下来，
 * 这个类不做任何格式化 —— 例如"传说装备 ×1"这句话是在 AArenaGameMode 里拼的。
 */
USTRUCT(BlueprintType)
struct FArenaChoiceCardView
{
	GENERATED_BODY()

	/**
	 * 提交这个选项时要回给服务端的下标。
	 *
	 * 【必须原样回传】服务端拿它去自己的待选里取载荷（不信客户端说的内容）。
	 * 所以 UI 不能自己重排卡片顺序 —— 卡片数组的顺序就是待选数组的顺序。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 OptionIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText Label;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText Description;

	/** 已经加载好的图标。留空 = 这张卡不画图标。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	TObjectPtr<UTexture2D> Icon = nullptr;

	/**
	 * 能不能点。false = 卡片应该画成灰的并且挡住点击。
	 *
	 * 【为什么要有这个字段】服务端是权威的，一个载荷为空的选项会被它当场拒掉
	 * （AArenaGameMode::ResolveChoice 里那条 Warning）。与其让玩家点下去、什么都没发生、
	 * 还以为界面坏了，不如一开始就画成灰的。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bInteractable = false;

	/**
	 * 是不是那颗「进入战斗」按钮。
	 *
	 * 【为什么 UI 需要知道这件事】它和其他卡的视觉权重不一样（次要按钮 vs 奖励卡），
	 * 而且它是奖励阶段唯一的出口 —— 美术想给它一个固定的位置/颜色时得认得出它。
	 * 注意这里给的是"看起来像什么"，不是"点了会怎样"：点了会怎样仍然由服务端按 OptionIndex 决定。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bIsFinishAction = false;

	/**
	 * 海克斯卡的品质档（卡片边框/角标用它配色）。
	 * 只有海克斯三选一的卡不是 None；锻造器菜单等普通按钮都是 None。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	EArenaCardTier Tier = EArenaCardTier::None;

	/**
	 * 这张卡能不能重随（整份待选 bRerollable 且还剩次数时才 true）。
	 * 卡片用它把重随按钮画成可点/置灰。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bRerollable = false;

	/** 整场还剩几次重随（重随按钮上的数字）。bRerollable 为 false 时无意义。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 RerollsLeft = 0;

	bool EqualsForUI(const FArenaChoiceCardView& Other) const
	{
		// 比的是"画面会不会变"，所以 Icon 比指针、文案比内容、序号和两个标志直接比。
		return OptionIndex == Other.OptionIndex
			&& bInteractable == Other.bInteractable
			&& bIsFinishAction == Other.bIsFinishAction
			&& Tier == Other.Tier
			&& bRerollable == Other.bRerollable
			&& RerollsLeft == Other.RerollsLeft
			&& Icon == Other.Icon
			&& Label.EqualTo(Other.Label)
			&& Description.EqualTo(Other.Description);
	}
};

/**
 * 当前等着本地玩家做的那次选择。
 *
 * bActive 为 false 时整个界面收起 —— 不是画一个空面板。
 * 和"未就绪不画空血条"是同一条纪律：空界面看起来像 bug，不画才是对的。
 */
USTRUCT(BlueprintType)
struct FArenaPromptView
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bActive = false;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText Title;

	/** 这份待选能不能重随（原样来自服务端）。重随按钮的显隐用它。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bRerollable = false;

	/** 整场还剩几次重随。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 RerollsLeft = 0;

	/** 顺序 = 待选顺序，OptionIndex 已经填好。UI 直接按顺序生成卡片。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	TArray<FArenaChoiceCardView> Cards;

	bool EqualsForUI(const FArenaPromptView& Other) const
	{
		if (bActive != Other.bActive || Cards.Num() != Other.Cards.Num() || !Title.EqualTo(Other.Title)
			|| bRerollable != Other.bRerollable || RerollsLeft != Other.RerollsLeft)
		{
			return false;
		}
		for (int32 Index = 0; Index < Cards.Num(); ++Index)
		{
			if (!Cards[Index].EqualsForUI(Other.Cards[Index]))
			{
				return false;
			}
		}
		return true;
	}
};

/**
 * 一个参赛者的大场状态。
 *
 * 【大场血量不是角色血量】角色血量每回合回满，这个只减不增（需求里的 15/30/40/50）。
 * 两个都显示是有意的：一个是"这回合还能不能打"，一个是"还剩几条命"。
 */
USTRUCT(BlueprintType)
struct FArenaContenderView
{
	GENERATED_BODY()

	/** false = 这个位置还空着（还在等人 / Bot 还没进来）。UI 应该收起这一栏。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bValid = false;

	/** true = 这是本地玩家自己。UI 用它决定"左边还是右边"。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bIsSelf = false;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 MatchHealth = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 MaxMatchHealth = 1;

	/** 血条比例（0~1，已经夹好）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	float HealthPercent = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 RoundsWon = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 RoundsLost = 0;

	/**
	 * 血量的显示文本（"85 / 100"）。
	 *
	 * 【为什么不留给 Widget 自己拼】同 HUDTypes 的纪律：翻译层出去的全是能直接画的成品。
	 * 百分比给血条用、数字文本给文本框用，两者由同一次构建填出，所以不会出现
	 * "条是满的、字是 100/100、其实血只有一半"这种两份数据打架的情况。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText HealthText;

	/** 战绩的显示文本（"3 胜 1 负"）。数字本身在 RoundsWon / RoundsLost 里。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText ScoreText;
};

/** 整场比赛的状态：第几回合、什么相位、双方比分。 */
USTRUCT(BlueprintType)
struct FArenaMatchView
{
	GENERATED_BODY()

	/** 0 = 还没开始。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 RoundNumber = 0;

	/**
	 * 相位的结构化形态。文本（PhaseText）是给它拼的成品，这个枚举是给
	 * "按相位切换表现"的逻辑用的（比如 VS 介绍只在进战斗时播一次）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	EArenaPhaseView Phase = EArenaPhaseView::WaitingToStart;

	/** 回合号的显示文本（"第 3 回合"）。RoundNumber 为 0 时是空文本。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText RoundText;

	/** 相位名，已经本地化好的文案（"备战" / "战斗中"…）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText PhaseText;

	/** 离本相位结束还有几秒。只在 bHasCountdown 为 true 时有意义。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	float PhaseRemainingSeconds = 0.f;

	/**
	 * 倒计时的显示文本（向上取整的秒数）。
	 * bHasCountdown 为 false 时是空文本 —— 那时候它没有意义。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText CountdownText;

	/** false = 这个相位没有倒计时（战斗 / 等待开始）。UI 应该把倒计时数字收起来。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bHasCountdown = false;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FArenaContenderView Self;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FArenaContenderView Opponent;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bMatchEnded = false;

	/**
	 * 结果文案（"你赢了" / "你输了" / "对手掉线，你获胜"）。
	 * 只在 bMatchEnded 为 true 时有意义。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText ResultText;

	/**
	 * 上一回合的结果（"√ 上回合胜利 −30" / "× 上回合失利 −30"）。
	 * 空文本 = 还没有上一回合（开局 / 没记过结果），UI 收起它。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText LastRoundText;

	bool EqualsForUI(const FArenaMatchView& Other) const
	{
		// 倒计时比的是【文本】而不是 PhaseRemainingSeconds 那个浮点数 ——
		// 每一帧的剩余秒数都不一样，按浮点比会让这个结构每帧都判定为"变了"，
		// 于是整个界面被 30Hz 重画。CountdownText 是控制器按"整秒"拼的，
		// 它在秒数真的跳了的时候才变，正好就是该重画的时候。
		return RoundNumber == Other.RoundNumber
			&& Phase == Other.Phase
			&& bMatchEnded == Other.bMatchEnded
			&& bHasCountdown == Other.bHasCountdown
			&& RoundText.EqualTo(Other.RoundText)
			&& CountdownText.EqualTo(Other.CountdownText)
			&& PhaseText.EqualTo(Other.PhaseText)
			&& ResultText.EqualTo(Other.ResultText)
			&& LastRoundText.EqualTo(Other.LastRoundText)
			&& FMath::IsNearlyEqual(Self.HealthPercent, Other.Self.HealthPercent, 0.002f)
			&& FMath::IsNearlyEqual(Opponent.HealthPercent, Other.Opponent.HealthPercent, 0.002f)
			&& Self.RoundsWon == Other.Self.RoundsWon
			&& Self.RoundsLost == Other.Self.RoundsLost
			&& Opponent.RoundsWon == Other.Opponent.RoundsWon
			&& Opponent.RoundsLost == Other.Opponent.RoundsLost
			&& Self.bValid == Other.Self.bValid
			&& Opponent.bValid == Other.Opponent.bValid
			&& Self.DisplayName.EqualTo(Other.Self.DisplayName)
			&& Opponent.DisplayName.EqualTo(Other.Opponent.DisplayName)
			&& Self.HealthText.EqualTo(Other.Self.HealthText)
			&& Opponent.HealthText.EqualTo(Other.Opponent.HealthText)
			&& Self.ScoreText.EqualTo(Other.Self.ScoreText)
			&& Opponent.ScoreText.EqualTo(Other.Opponent.ScoreText);
	}
};

/** 装备栏里的一格（一件装备或一个海克斯）。 */
USTRUCT(BlueprintType)
struct FArenaLoadoutEntryView
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText Description;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	TObjectPtr<UTexture2D> Icon = nullptr;

	/** true = 海克斯，false = 装备。两行分开画的时候用它分流。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	bool bIsAugment = false;

	/**
	 * 海克斯的品质档（装备栏格子上的角标用它配色）。装备条目是 None。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	EArenaCardTier Tier = EArenaCardTier::None;

	bool EqualsForUI(const FArenaLoadoutEntryView& Other) const
	{
		return bIsAugment == Other.bIsAugment
			&& Tier == Other.Tier
			&& Icon == Other.Icon
			&& DisplayName.EqualTo(Other.DisplayName)
			&& Description.EqualTo(Other.Description);
	}
};

/** 锻造器还攒着几次。 */
USTRUCT(BlueprintType)
struct FArenaForgeChargeView
{
	GENERATED_BODY()

	/** "传说锻造器" —— 已经拼好的名字。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	int32 Count = 0;

	bool EqualsForUI(const FArenaForgeChargeView& Other) const
	{
		return Count == Other.Count && DisplayName.EqualTo(Other.DisplayName);
	}
};

/** 本地玩家的装备栏。 */
USTRUCT(BlueprintType)
struct FArenaLoadoutView
{
	GENERATED_BODY()

	/**
	 * 装备和海克斯在同一个数组里，用 bIsAugment 区分。
	 *
	 * 【为什么不分两个数组】它们在界面上是同一排图标，分开只会让"画一排东西"变成
	 * "先画完一个数组再画另一个"，而两段代码里迟早有一处忘了刷新。
	 * 顺序 = 拿到的顺序（装备先、海克斯后）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	TArray<FArenaLoadoutEntryView> Entries;

	/** 只包含次数 > 0 的档位。花完了的不出现在界面上。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|UI")
	TArray<FArenaForgeChargeView> ForgeCharges;

	bool EqualsForUI(const FArenaLoadoutView& Other) const
	{
		if (Entries.Num() != Other.Entries.Num() || ForgeCharges.Num() != Other.ForgeCharges.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			if (!Entries[Index].EqualsForUI(Other.Entries[Index]))
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < ForgeCharges.Num(); ++Index)
		{
			if (!ForgeCharges[Index].EqualsForUI(Other.ForgeCharges[Index]))
			{
				return false;
			}
		}
		return true;
	}
};

// ---------------------------------------------------------------------------
// 委托（全部 BlueprintAssignable，签名里没有任何 GAS 类型）
// ---------------------------------------------------------------------------

/** 待选择变了（来了新的、或者被答完收起了）。奖励界面订阅它重画。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaPromptViewChangedSignature, const FArenaPromptView&, Prompt);

/** 第几回合 / 相位 / 双方大场血量变了。比分栏订阅它。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaMatchViewChangedSignature, const FArenaMatchView&, Match);

/** 自己的装备栏变了。装备栏 Widget 订阅它。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaLoadoutViewChangedSignature, const FArenaLoadoutView&, Loadout);

/** 回合计划变了（开局推下来一次）。回合线订阅它。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaRoundTrackerChangedSignature, const FArenaRoundTrackerView&, Tracker);

/**
 * 相位【变了】（边沿，不是电平）。
 *
 * 【为什么 MatchChanged 之外还要它】OnMatchChanged 是"内容变了才推"——
 * 相位从备战到战斗时血量比分往往一个字没变，Match 一路不会广播，
 * 但"进入战斗"这一刀恰恰是 VS 介绍、镜头切换这类表现的触发点。
 * 边沿委托把"什么时候播"和"画面内容"分成两条通道，表现层不用自己拿文本比对猜相位。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnArenaPhaseViewChangedSignature, EArenaPhaseView, NewPhase);
