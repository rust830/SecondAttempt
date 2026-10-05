// 斗魂竞技场：跨类共享的枚举与配置结构。
//
// 【为什么单独一个头】相位 / 品质 / 奖励配置这三样，GameMode、GameState、PlayerState、
// 装备栏组件、HUD 翻译层、UI 契约都要用。塞进任何一边都会让另一边反向依赖它 ——
// 这个头不依赖任何别的游戏类，谁都能放心 include。
//
// 放 GAS/ 而不是别处：它和 ArenaItemData / ArenaAugmentData 是一套（那两个持有
// TSubclassOf<UGameplayEffect>），判据见 CONVENTIONS.md 规则 2。

#pragma once

#include "CoreMinimal.h"
#include "ArenaTypes.generated.h"

class UArenaItemData;
class UArenaAugmentData;

/**
 * 整场比赛共享的几个固定数。
 *
 * 【为什么是常量而不是 UPROPERTY】参赛人数不是"配一下就能变"的东西 ——
 * 它同时决定了 GameState 里有几个位子、GameMode 的回合循环要遍历几次、
 * 出生点要找几个。做成可配的等于给出一个"改了会静默半坏"的旋钮。
 * 真要做三人混战，那是另一套流程，不是把这个数改成 3。
 */
namespace ArenaMatch
{
	/** 参赛人数。需求是 1V1。 */
	inline constexpr int32 ContenderCount = 2;
}

/**
 * 比赛相位。回合循环 = 备战（含奖励选择）→ 战斗 → 结算 → （回到备战）。
 *
 * WaitingToStart 只在第一回合之前存在（等两个人都 Possess 完）；
 * MatchEnd 是终态，进去就不再出来。
 *
 * 【RewardSelection 现在的语义是"备战阶段"（2026-10-01 改）】
 * 对齐 LoL 斗魂竞技场的真实节奏：每个战斗回合之前有一段备战时间，
 * 玩家站在备战区（独立的地图区域）里【自由移动】，本回合的奖励选择
 * （分支 / 海克斯 / 装备 / 锻造器）全部在这段里完成 —— 倒计时走完或双方都
 * 点了「进入战斗」才开打。枚举值保留旧名字不改：已保存的蓝图里可能有
 * 按这个值分支的 switch，删值会让那些资产静默变坏。
 */
UENUM(BlueprintType)
enum class EArenaPhase : uint8
{
	/** 还没开局：等双方 Pawn / PlayerState 都就位。 */
	WaitingToStart		UMETA(DisplayName = "未开始"),

	/** 备战阶段：在备战区自由移动 + 完成本回合的奖励选择（原"奖励选择"）。 */
	RewardSelection		UMETA(DisplayName = "备战"),

	/** 战斗阶段：解除冻结，打到一方倒下为止。 */
	Combat				UMETA(DisplayName = "战斗"),

	/** 结算阶段：扣血、判胜负，停一会儿给表现层播动画。 */
	Settlement			UMETA(DisplayName = "结算"),

	/** 大场结束。 */
	MatchEnd			UMETA(DisplayName = "比赛结束"),
};

/**
 * 回合计划里"这一回合是什么"，给回合线（round tracker）用。
 *
 * 【为什么不是直接用 EArenaRewardKind】那个枚举里有 BranchChoice / FixedForge
 * 这类"流程形状"，而回合线要画的是"玩家视角这一格是什么"：
 * 二选一分支抽到装备还是锻造器、固定发放的锻造器，在回合线上都各归各的图标。
 * 映射发生在服务端（AArenaGameMode::BuildRoundPlan），GameState 上复制的
 * 已经是"能直接画"的答案 —— 和 UI 契约那条"翻译层只给成品"的纪律同源。
 */
UENUM(BlueprintType)
enum class EArenaStageKind : uint8
{
	/** 战斗回合（包括"无奖励"回合 —— 那也是打一场）。 */
	Combat				UMETA(DisplayName = "战斗"),

	/** 海克斯三选一回合。 */
	Augments			UMETA(DisplayName = "海克斯"),

	/** 装备回合（分支抽到装备，或纯装备发放）。 */
	ItemPurchase		UMETA(DisplayName = "装备"),

	/** 属性锻造器回合（分支抽到锻造器，或固定发锻造器）。 */
	StatAnvil			UMETA(DisplayName = "锻造"),
};

/**
 * 奖励分支：直接拿装备，还是拿锻造器次数（属性锻造器 stat anvil）。
 *
 * 【两者的区别在产出物】装备是当场三选一立刻进装备栏；
 * 锻造器是先攒次数，每次使用【直接随机发属性】（不占装备栏、不可摘除）——
 * 对应 LoL Arena 的 Stat Anvil，不是传说装备池的又一条入口。
 */
UENUM(BlueprintType)
enum class EArenaRewardBranch : uint8
{
	/** 装备：当场三选一，选中直接进装备栏。 */
	Item				UMETA(DisplayName = "装备"),

	/** 属性锻造器：给次数，每次使用直接随机发属性。 */
	Forge				UMETA(DisplayName = "锻造器"),
};

/**
 * 品质。对应需求里的「传说 / 棱彩」。
 *
 * 【加品质要改这里 + 在池子里配对应品质的条目】，不是纯数据 ——
 * 和属性名一样是 schema（见 HeroStatConfig.h 的边界 1）。
 */
UENUM(BlueprintType)
enum class EArenaItemTier : uint8
{
	/** 传说装备 / 传说锻造器。 */
	Legendary			UMETA(DisplayName = "传说"),

	/** 棱彩装备 / 棱彩锻造器。 */
	Prismatic			UMETA(DisplayName = "棱彩"),
};

/**
 * 海克斯（强化符文）的品质。对应 LoL 斗魂竞技场的「银色 / 金色 / 棱彩」三档。
 *
 * 【为什么不复用 EArenaItemTier】装备的品质是「传说 / 棱彩」，RollItems 按它过滤池子；
 * 海克斯是「银 / 金 / 棱彩」，回合表按它决定第几回合发哪一档。两边的档位名字都不一样，
 * 硬塞进同一个枚举会让「传说品质的海克斯」这种不存在的组合变得可以表达出来。
 */
UENUM(BlueprintType)
enum class EArenaAugmentTier : uint8
{
	/** 银色海克斯（前期：第 2 回合的海克斯三选一）。 */
	Silver				UMETA(DisplayName = "白银"),

	/** 金色海克斯（中期：第 4 回合的海克斯三选一）。 */
	Gold				UMETA(DisplayName = "黄金"),

	/** 棱彩海克斯（第 7 回合起循环里的海克斯回合）。 */
	Prismatic			UMETA(DisplayName = "棱彩"),
};

/** 一个回合发什么。 */
UENUM(BlueprintType)
enum class EArenaRewardKind : uint8
{
	/** 什么都不发（第 6 回合：首次大场阈值判定那一回合）。 */
	None				UMETA(DisplayName = "无奖励"),

	/** 玩家先选分支（装备 / 锻造器），再按分支走。 */
	BranchChoice		UMETA(DisplayName = "二选一分支"),

	/** 海克斯：随机三选一。 */
	Augment				UMETA(DisplayName = "海克斯"),

	/** 固定统一发放（第 3 回合：棱彩锻造器×1）。 */
	FixedForge			UMETA(DisplayName = "固定锻造器"),
};

/**
 * 一次三选一从哪个池子抽。
 *
 * 抽签结果出来后，装备/锻造器都变成「装备栏里多一件」，海克斯变成「海克斯列表里多一个」——
 * 但抽签那一刻它们取的是不同池子，所以这里要分开。
 */
UENUM(BlueprintType)
enum class EArenaOfferSource : uint8
{
	/** 从 UArenaRewardPool::Items 里抽（装备 / 锻造器共用）。 */
	Item				UMETA(DisplayName = "装备池"),

	/** 从 UArenaRewardPool::Augments 里抽。 */
	Augment				UMETA(DisplayName = "海克斯池"),
};

/**
 * 装备栏满了之后再抽到装备怎么办。
 *
 * 需求里没写这条 —— 装备栏 6 格、第 7 件以后必然撞上。默认「顶掉最早的那件」，
 * 因为「发不出去」会让后面所有回合的奖励凭空消失，比顶掉更糟。
 */
UENUM(BlueprintType)
enum class EArenaFullRackPolicy : uint8
{
	/** 顶掉最早装上的那件（FIFO）。 */
	ReplaceOldest		UMETA(DisplayName = "顶掉最早的一件"),

	/** 直接丢弃这次抽签结果（记一条日志）。 */
	Discard				UMETA(DisplayName = "丢弃"),
};

/**
 * 奖励分支的一个选项：走哪条分支、什么品质、给几个。
 *
 * 第 1 回合：{Item, 传说, 1} / {Forge, 传说, 5}
 * 第 5 回合：{Item, 棱彩, 1} / {Forge, 棱彩, 3}
 */
USTRUCT(BlueprintType)
struct FArenaBranchOption
{
	GENERATED_BODY()

	/** 装备 还是 锻造器。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward")
	EArenaRewardBranch Branch = EArenaRewardBranch::Item;

	/** 从池子里抽的时候按这个品质过滤。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward")
	EArenaItemTier Tier = EArenaItemTier::Legendary;

	/**
	 * 给几个。
	 * 装备分支是「当场三选一的次数」（1 = 只来一次）；锻造器分支是「攒几次使用机会」。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward", meta = (ClampMin = "1"))
	int32 Count = 1;
};

/**
 * 一个回合的奖励配置。整个回合表就是这些东西的数组（见 AArenaGameMode::RoundRewards）。
 *
 * 【为什么是结构体数组而不是 if-else】需求里第 1~6 回合各不相同、第 7 回合起三回合一轮循环，
 * 写死成分支的话每调一个数字都要重编译。这样配完能直接在编辑器里改。
 */
USTRUCT(BlueprintType)
struct FArenaRoundReward
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward")
	EArenaRewardKind Kind = EArenaRewardKind::None;

	/**
	 * Kind == BranchChoice 时用：两个分支。
	 * 空数组 = 配错了，GameMode 会记一条 Warning 并当 None 处理（不会崩，也不静默）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward")
	TArray<FArenaBranchOption> Branches;

	/** Kind == FixedForge 时用：固定发的那一份。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward")
	FArenaBranchOption FixedGrant;

	/**
	 * Kind == Augment 时用：这一回合的海克斯三选一发哪一档。
	 *
	 * 对应 LoL 斗魂的节奏：第 2 回合白银、第 4 回合黄金、循环里的海克斯回合棱彩。
	 * 默认在 C++ 构造函数里按这个节奏填好，改节奏在 BP_ArenaGameMode 的回合表上改。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Reward")
	EArenaAugmentTier AugmentTier = EArenaAugmentTier::Silver;
};

/**
 * 攒着的锻造器次数，按品质分。
 *
 * 需求里锻造器有两种品质（第 3 回合固定发棱彩、第 1 回合的传说分支、第 5 回合的棱彩分支），
 * 所以次数要分开记 —— 一个「棱彩锻造器×3」不能冒充传说品质的产出。
 * 【花法】每次使用（EArenaPromptAction::UseForge）按品质从对应的 stat anvil 属性池里抽。
 */
USTRUCT(BlueprintType)
struct FArenaForgeCharge
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Arena|Forge")
	EArenaItemTier Tier = EArenaItemTier::Legendary;

	/** 还能用几次。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Forge", meta = (ClampMin = "0"))
	int32 Count = 0;
};

/**
 * 点一个选项之后做什么。
 *
 * 【为什么要有这个字段】光靠载荷猜不出来：锻造器菜单里的「使用棱彩锻造器」和
 * 「进入战斗」两个按钮都没有装备/海克斯载荷（前者带的是 Branch.Tier，后者什么都不带），
 * 让 UI 或服务端去猜「这个空载荷的按钮是什么意思」是猜不准的。
 * 明写出来，UI 就真的只剩「画按钮 + 回传下标」。
 */
UENUM(BlueprintType)
enum class EArenaPromptAction : uint8
{
	/** 按载荷发放：Item 进装备栏 / Augment 进海克斯列表 / Branch 展开成下一层选择。 */
	Grant				UMETA(DisplayName = "发放"),

	/** 花掉一次 Branch.Tier 品质的锻造器次数，直接随机发属性（stat anvil，见 ArenaGameMode 的 Arena|Forge 配置）。 */
	UseForge			UMETA(DisplayName = "使用锻造器"),

	/**
	 * 本回合的奖励选择结束，可以进战斗了。
	 *
	 * ⚠️ 【当前没有生产方 —— 这是一份保留的 API，不是死代码删错了】
	 * 「进入战斗」按钮已按需求移除（备战阶段改成限时，到点由
	 * `ForceFinishPreparation` 沿链随机代选、统一开战）。所以现在
	 * **没有任何地方生成 `Action = Finish` 的选项**，这个枚举值只在消费侧存在：
	 *   ① `ArenaGameMode::HandleChoice` 的 `case`（代选链里作为「提前收尾」的合法出口）
	 *   ② `ArenaHUDController` 把它映射成 `bIsFinishAction` → 卡片画 FinishFrame 边框
	 *   ③ `ArenaBotController` 的选项分类
	 *   ④ `FArenaChoiceOption::IsValidOption()` 的 `case`
	 *
	 * 保留的理由：代选链里"随机到结束选择"是一个**合理的合法出口**，将来
	 * 备战限时去掉、改回"手动点进入战斗"，或者锻造器菜单需要一个"收尾"按钮，
	 * 只要在待选里塞一条 Finish 选项就能立刻接上，整条消费链已经在了。
	 * 真要删就得同时动 4 个文件 + 蓝图事件，比留着贵。
	 */
	Finish				UMETA(DisplayName = "结束选择"),
};

/**
 * 一个玩家的奖励选择流程走到哪一步了。服务端专用状态，不复制。
 *
 * 流程是串行的，每一步答完决定下一步去哪：
 *
 *   RoundReward ──(分支/海克斯答完)──┐
 *                                    ↓
 *   ForgeMenu ──(点「使用锻造器」)──→ 直接发属性 → 回到 ForgeMenu
 *       └──────(点「结束选择」)────→ None（这个人的奖励阶段完了）
 *
 * ItemOffer 只剩一个来源（本回合发的装备奖励），所以 bFromRoundReward
 * 现在恒为 true —— 字段留着，等以后出现第二个来源时好区分。
 *
 * ⚠️ 图上那条「点结束选择」现在【没有生产方】（Finish 枚举已无生成处，
 * 见上方注释）。所以实际终止路径是：`ForceFinishPreparation` 的随机代选链
 * 走到 `ResetRewardSelection()`（或代选满 16 步无推进时的兜底）→ None。
 * 图保留这条边是因为它是"代选链的合法出口"，将来要恢复手动结束也用它。
 */
UENUM(BlueprintType)
enum class EArenaRewardStep : uint8
{
	/** 不在奖励流程里（战斗 / 结算阶段，或者已经答完了）。 */
	None				UMETA(DisplayName = "无"),

	/** 正在答本回合发的奖励（二选一分支 / 海克斯三选一）。 */
	RoundReward			UMETA(DisplayName = "回合奖励"),

	/** 正在做一次装备三选一。 */
	ItemOffer			UMETA(DisplayName = "装备三选一"),

	/** 锻造器菜单：可以继续用次数，也可以结束选择。 */
	ForgeMenu			UMETA(DisplayName = "锻造器菜单"),
};

/**
 * 三选一界面上的一个选项。
 *
 * =====================================================================
 * 【为什么装备 / 海克斯 / 分支三种东西共用一个结构】
 * 需求里玩家要做四类选择，但它们全都是同一件事 ——「给你 N 个选项，点一个」：
 *   ① 第 1 / 5 回合：装备 还是 锻造器        （2 个选项）
 *   ② 装备奖励 / 锻造器使用：三选一装备       （3 个选项）
 *   ③ 海克斯回合：三选一海克斯               （3 个选项）
 * 给每种选择各做一个结构 + 各做一套 UI，等于把「N 选一」这件事抄四遍。
 * 统一成一个结构之后，UI 那边只有一个循环：**有几个选项就画几个按钮**。
 * =====================================================================
 *
 * 【文案在服务端算好】Label / Description 是服务端填的成品文本，不是给客户端
 * 「品质 + 名字」让它自己拼。理由和 HUD 那套一样（见 HUDTypes.h）：
 * UI 一旦开始做判断，改文案就得翻 UI 蓝图，而且两端拼法不一致时很难发现。
 *
 * 【载荷三选一】Item / Augment / Branch 里【只有一个】是有效的，
 * 由服务端在结算时判断。用三个指针而不是 union/变体，是因为 UHT 对 union 支持很差，
 * 而三个指针的实际代价只有 24 字节。
 */
USTRUCT(BlueprintType)
struct FArenaChoiceOption
{
	GENERATED_BODY()

	/** 点下去干什么。决定下面哪几个载荷字段有意义。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	EArenaPromptAction Action = EArenaPromptAction::Grant;

	/** 按钮上的标题。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	FText Label;

	/** 标题下面的一行说明。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	FText Description;

	/** 图标。软引用 —— 三选一界面上要显示时才加载。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	TSoftObjectPtr<UTexture2D> Icon;

	/**
	 * 海克斯选项的品质。只有 Action == Grant 且载荷是海克斯时有意义。
	 *
	 * 【为什么直接放品质而不是让客户端去解析 Augment 软引用】客户端从不去解析
	 * 载荷软引用（见 ArenaPlayerState.h 的注释），品质是 UI 配色要用的成品信息 ——
	 * 按「翻译层只给能直接画的东西」的口径，服务端在这里一次填好。
	 * 非海克斯选项保持默认值，UI 对它不做品质表现。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	EArenaAugmentTier Tier = EArenaAugmentTier::Silver;

	// --- 载荷：按 Action 决定哪个有意义 ----------------------------

	/**
	 * 选它就装这件装备。
	 *
	 * 【软引用不是随手写的】这个结构要被复制到客户端，而 DataAsset 不是 Actor ——
	 * 裸的 TObjectPtr<UArenaItemData> 在客户端会【静默变成 null】：
	 * UObject 默认 IsSupportedForNetworking() 返回 false，包映射根本不会传它。
	 * TSoftObjectPtr 走 FSoftObjectProperty::NetSerializeItem（UnrealType.h:3425），
	 * 传的是资产路径字符串，客户端能据此找到同一个资产。
	 * 服务端那边 .Get() 永远是有效的（资产本来就已经加载着）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	TSoftObjectPtr<UArenaItemData> Item;

	/** 选它就拿这个海克斯。软引用的理由同上。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	TSoftObjectPtr<UArenaAugmentData> Augment;

	/**
	 * 选它就走这条分支（装备 / 锻造器）。
	 * 「选分支」那一步装在 Branch 里；「使用锻造器」那一步只有 Tier 有意义。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	FArenaBranchOption Branch;

	/**
	 * 这个选项能不能结算。
	 * GameMode 收到选择后会过一遍，不合法就记日志并忽略（不崩，也不静默）。
	 */
	bool IsValidOption() const
	{
		switch (Action)
		{
		case EArenaPromptAction::Grant:
			// 三种载荷里必须有一个 —— 都没有就是服务端配错了。
			// 用 IsNull() 而不是 .Get()：这里问的是"配没配"，不是"加载没加载"。
			// 客户端上资产可能还没加载，那时候 .Get() 是 null 但配置其实是好的。
			return !Item.IsNull() || !Augment.IsNull() || Branch.Count > 0;

		case EArenaPromptAction::UseForge:
			// Count 在这里是「这个品质还剩几次」，0 就不该出现这个按钮。
			return Branch.Count > 0;

		case EArenaPromptAction::Finish:
			return true;
		}

		return false;
	}
};

/**
 * 当前等着某个玩家做的一次选择。
 *
 * 【一次只有一个】需求里的选择是串行的：先选分支 → 再三选一 → 再（可选地）花锻造器
 * 一次一次抽。所以 PlayerState 上只留一个待选，不做队列 ——
 * 队列会引出「上一个没答完又来一个」这种没人定义过的情况。
 * 花多次锻造器是「答完一次、服务端再发下一个」，天然串行。
 */
USTRUCT(BlueprintType)
struct FArenaPendingPrompt
{
	GENERATED_BODY()

	/** 有没有待选。false 时其余字段都不该被读。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	bool bActive = false;

	/** 界面标题（如「选择你的奖励」/「锻造器」）。服务端填好。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	FText Title;

	/**
	 * true = 这是本回合发的奖励，false = 这是花锻造器次数抽出来的。
	 * UI 只是拿来换个副标题，不参与任何逻辑。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	bool bFromRoundReward = true;

	/** 选项，顺序即界面上的顺序。2 个或 3 个（池子不够时可能更少）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	TArray<FArenaChoiceOption> Options;

	/**
	 * true = 这份待选可以重随（回合发的装备 / 海克斯三选一）。
	 *
	 * 【对齐 LoL 斗魂竞技场】锻造器菜单、二选一分支、属性锻造器（stat anvil）的选项
	 * 都不可重随 —— 重随的意义是"换一批候选"，而属性锻造器点下去就是随机结算，
	 * 没有"候选"可换。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice")
	bool bRerollable = false;

	/**
	 * 还剩几次重随。整场共享（跨所有选择界面、跨回合），开局由 GameMode 初始化。
	 * bRerollable 为 false 时 UI 不显示它。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|Choice", meta = (ClampMin = "0"))
	int32 RerollsLeft = 0;
};

/**
 * 大场血量的一条扣除区间。
 *
 * 需求：1-4 回合扣 15，5-8 扣 30，9-12 扣 40，13+ 扣 50。
 * 写成 {起始回合, 扣多少} 的有序数组，取「最后一条 FromRound <= 当前回合」的那条。
 */
USTRUCT(BlueprintType)
struct FArenaHpLossBand
{
	GENERATED_BODY()

	/** 从这个回合起（含）生效。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Match", meta = (ClampMin = "1"))
	int32 FromRound = 1;

	/** 输掉这一回合要扣多少大场血量。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Arena|Match", meta = (ClampMin = "0"))
	int32 HpLoss = 15;
};
