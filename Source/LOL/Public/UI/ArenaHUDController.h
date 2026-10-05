// 竞技场 HUD 的翻译层：复制下来的游戏状态 → UI 契约（ArenaViewTypes.h）。
//
// ===========================================================================
// 【和 UHeroHUDController 同一个位置，但简单得多】
// 那个要跟 ASC 打交道（属性委托、标签事件、冷却心跳），所以它住在 GAS/ 里。
// 这个只读 AArenaGameState / AArenaPlayerState / UArenaLoadoutComponent 上
// 【已经复制下来的】显示数据，一次都不碰 ASC / GA / GE / Tag ——
// 所以按 CONVENTIONS.md 规则 2 的判据，它住 UI/，不进 GAS/。
//
// 三层还是那三层：GameMode 算规则 → 这里翻译 → Widget 只画。
// Widget 拿不到 PlayerState、不认识 EArenaPhase、不知道"第 7 回合发什么"。
// ===========================================================================
//
// 【为什么是心跳 + EqualsForUI 去重，而不是一堆事件订阅】
// 三路视图各自都有事件源（PlayerState 的 OnPromptChanged / OnMatchStateChanged、
// GameState 的 OnArenaPhaseChanged / OnMatchEnded、Loadout 的 OnLoadoutChanged），
// 但有两件事事件覆盖不了：
//   ① 倒计时是个连续量 —— 它不变的时候没人发事件，而它每一秒都得重画；
//   ② 对手的 PlayerState 是复制过来的，绑定那一刻【还不存在】，
//      等它到了再去订阅它的委托就得写一套"重新订阅"的逻辑。
// 所以：自方的三路订阅事件（点一下立刻有反应），对手和倒计时靠 0.25 秒的心跳补齐。
// 三条推送路径都走同一个 Rebuild，而且只在 EqualsForUI 说有变化时才广播 ——
// 于是"事件 + 心跳"不会变成"同一件事推两遍"。
//
// 【Outer 必须是 AArenaPlayerController】和 UHeroHUDController 一样的约定：
// 顺 Outer 找本地 PC，再顺 PC 找本地 PlayerState。
// 用 Outer 而不是缓存一个裸指针，是因为强引用会把 PC（和它下面的 PlayerState）
// 钉住不放，而这个对象的寿命本来就该由 PC 决定。

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GAS/ArenaTypes.h"
#include "UI/ArenaViewTypes.h"
#include "ArenaHUDController.generated.h"

class AArenaGameState;
class AArenaPlayerState;
class UTexture2D;
class UUserWidget;

UCLASS(BlueprintType)
class LOL_API UArenaHUDController : public UObject
{
	GENERATED_BODY()

public:
	// ---------------------------------------------------------------------
	// 绑定（幂等）
	// ---------------------------------------------------------------------

	/**
	 * 试着绑上本地 PlayerState 和 GameState。可以随便调多少次。
	 *
	 * 【为什么要有重试】ABeginPlay 的时候 PlayerState 可能还没复制到客户端
	 *（客户端的 PlayerState 是复制过来的，而 PC 的 BeginPlay 早于 OnRep_PlayerState），
	 * 所以绑不上是正常现象，不是错误 —— 排一个 0.25 秒的定时器一直试，
	 * 超过 MaxBindRetries 才记一条 Warning 收手（那时多半是这张图根本没有竞技场模式）。
	 *
	 * 绑上之后同一条定时器【转成心跳】，一直跑到 Shutdown。
	 */
	void TryBindArena();

	/** PC EndPlay 调。解绑 + 停心跳。不靠"定时器绑了 UObject 引擎会收"兜底。 */
	void ShutdownArena();

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	bool IsBound() const;

	// ---------------------------------------------------------------------
	// 拉取（订阅即拉取）
	//
	// Widget 绑上来的时候必须能立刻拿到【此刻】的全量，否则第一屏是空的，
	// 一直要等到下一次变化才有东西 —— 而那可能要等到玩家点一下。
	//
	// 【这里和 UHeroHUDController::PullHUDState 有一点不同】那个直接返回缓存，
	// 因为它的构建要现场问 ASC（属性、冷却），不便宜。这边三个视图全是【读复制下来的 POD】，
	// 构建代价可以忽略，所以这三个是【现场构建】—— 拉到的永远是最新的一份，
	// 不存在"缓存比真值旧 0.25 秒"这种缝。
	// 上面的 CachedXxx 因此只承担一个职责：广播去重的基准。
	// ---------------------------------------------------------------------

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	FArenaPromptView PullPromptState() const;

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	FArenaMatchView PullMatchState() const;

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	FArenaLoadoutView PullLoadoutState() const;

	UFUNCTION(BlueprintPure, Category = "Arena|UI")
	FArenaRoundTrackerView PullRoundTrackerState() const;

	// ---------------------------------------------------------------------
	// 提交
	// ---------------------------------------------------------------------

	/**
	 * 玩家点了三选一里的第 OptionIndex 张卡。转发给本地 PlayerState 的 Server RPC。
	 *
	 * 【这里不判断选项内容】下标之外一个字节都不上传，服务端拿它去自己的待选里取载荷。
	 * 本地只挡一种情况：当前根本没有待选（那说明界面状态和服务端不同步）——
	 * 那种点击连 RPC 都不值得发，记一条 Warning 更容易查。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void SubmitChoice(int32 OptionIndex);

	/**
	 * 玩家点了第 OptionIndex 张卡下的重随按钮。转发给本地 PlayerState 的 Server RPC。
	 * 合法性（能不能重随、还剩几次）全部在服务端判 —— 这里只挡"没有待选"这种明显不同步。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void RerollChoice(int32 OptionIndex);

	// ---------------------------------------------------------------------
	// 委托
	// ---------------------------------------------------------------------

	/** 待选择变了（来了新的 / 被答完收起）。奖励界面订阅它。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaPromptViewChangedSignature OnPromptChanged;

	/** 回合 / 相位 / 倒计时 / 双方大场血量变了。比分栏订阅它。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaMatchViewChangedSignature OnMatchChanged;

	/** 自己的装备栏变了。装备栏订阅它。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaLoadoutViewChangedSignature OnLoadoutChanged;

	/** 回合计划变了（开局推下来一次，之后不变）。回合线订阅它。 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaRoundTrackerChangedSignature OnRoundTrackerChanged;

	/**
	 * 相位【变了】（边沿）。VS 介绍 / 结果横幅这类"一次性的表现"订阅它，
	 * 不要拿 OnMatchChanged 的文本比对去猜相位（那条通道是内容驱动的，
	 * 相位变化时内容可能一个字没变）。
	 */
	UPROPERTY(BlueprintAssignable, Category = "Arena|UI")
	FOnArenaPhaseViewChangedSignature OnPhaseChanged;

	/**
	 * 重算三路视图，把【变了的】广播出去。
	 *
	 * public 是因为它同时是"订阅即拉取"的另一半：Widget 可以先 AddDynamic 再调这个，
	 * 保证订阅和拉取之间那条缝里发生的变化不会被漏掉。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|UI")
	void BroadcastAllChanges();

private:
	// ---- 绑定实现 ----

	/** Outer(PC) → 本地 PlayerState。 */
	AArenaPlayerState* ResolveSelfState() const;
	AArenaGameState* ResolveGameState() const;

	/** 每个心跳：重试绑定（没绑上时）+ 重算三路。 */
	void OnHeartbeat();

	void BindSelf(AArenaPlayerState* Self);
	void BindGameState(AArenaGameState* GameState);
	void UnbindSelf();
	void UnbindGameState();
	void UnbindAll();

	// ---- 事件回调（自方那三路，为了"点一下立刻有反应"）----

	UFUNCTION()
	void HandlePromptChanged();

	UFUNCTION()
	void HandleMatchStateChanged();

	UFUNCTION()
	void HandleLoadoutChanged();

	UFUNCTION()
	void HandleArenaPhaseChanged(EArenaPhase NewPhase);

	UFUNCTION()
	void HandleMatchEnded(AArenaPlayerState* Winner);

	// ---- 构建视图 ----

	FArenaPromptView BuildPromptView() const;
	FArenaMatchView BuildMatchView() const;
	FArenaLoadoutView BuildLoadoutView() const;
	FArenaRoundTrackerView BuildRoundTrackerView() const;

	/** 一个人的那一栏。bIsSelf 由调用方给（这个类不猜"谁是自己"）。 */
	void BuildContenderView(AArenaPlayerState* PS, bool bIsSelf, FArenaContenderView& Out) const;

	// ---- 格式化：UI 层收到的永远是成品文本，所以拼字符串只写在这里 ----
	//
	// 做成 static 成员而不是文件内 free function：unity build 把多个 .cpp 合进同一个 TU 时，
	// 匿名 namespace 里的同名函数会撞（这个坑本项目已经踩过一次，见 HeroHUDController.h）。

	static FText ArenaPhaseDisplayName(EArenaPhase Phase);

	/**
	 * 品质名（"传说" / "棱彩"）。
	 *
	 * 【为什么这里也有一份】AArenaGameMode 里那份在匿名 namespace 里（跨 TU 拿不到），
	 * 而这边需要它拼"传说锻造器"这个徽章名。两处都是两行的 switch ——
	 * 为它单开一个头文件不划算，但值要改（比如加一个品质档）时【两个地方都要改】。
	 */
	static FText ArenaTierDisplayName(EArenaItemTier Tier);

	/**
	 * GAS 侧品质 → UI 侧品质档。纯一对一映射（白银/黄金/棱彩）。
	 * 【为什么做成函数】UI 契约不 include GAS 头（见 ArenaViewTypes.h），
	 * 映射只能发生在翻译层；集中一个入口，档位改名时只动这里。
	 */
	static EArenaCardTier ArenaAugmentTierToCardTier(EArenaAugmentTier Tier);

	/** 装备品质 → UI 侧品质档（锻造器菜单的按钮配色用）。传说→黄金，棱彩→棱彩。 */
	static EArenaCardTier ArenaItemTierToCardTier(EArenaItemTier Tier);

	/** GAS 侧相位 → UI 侧相位。一对一；漏映射的值落 Unknown。 */
	static EArenaPhaseView ArenaPhaseToView(EArenaPhase Phase);

	/** GAS 侧回合计划项 → UI 侧回合线格子。一对一。 */
	static EArenaStageKindView ArenaStageKindToView(EArenaStageKind Kind);

	/** "85 / 100"。 */
	static FText MakeHealthText(int32 Health, int32 MaxHealth);

	/** "3 胜 1 负"。 */
	static FText MakeScoreText(int32 Won, int32 Lost);

	/**
	 * 软引用资产 → 能直接读的指针。
	 *
	 * 【为什么要主动加载】三选一的图标在 FArenaChoiceOption 里是软引用（服务端刻意如此，
	 * 见 ArenaTypes.h：非 Actor 的裸指针过不了网络）。装备栏那边的软引用在
	 * UArenaLoadoutComponent::OnRep_Loadout 里预热过，但那是"复制到达的时机"，
	 * 不保证本 Widget 第一次重建时已经好了 —— 所以拿不到就 LoadSynchronous。
	 * 先 .Get() 再加载是有意的：心跳每 0.25 秒会走一遍这里，无脑 LoadSynchronous
	 * 等于每秒四次去查资产注册表。
	 *
	 * 返回 nullptr = 这个引用是空的（没配），或者路径解析不出来（资产改名 / 删了）——
	 * 两种都由调用方决定怎么表现（卡片不画图标 / 跳过这一格）。
	 *
	 * 【为什么是模板】装备和海克斯是两种资产类，函数体一模一样。写成模板就不用
	 * 为了两行代码维护两份 —— 代价是它必须是头文件里的 inline（UHT 不管模板，没问题）。
	 */
	template <typename TAsset>
	static TAsset* ResolveAsset(const TSoftObjectPtr<TAsset>& SoftAsset)
	{
		if (SoftAsset.IsNull())
		{
			return nullptr;
		}
		return SoftAsset.Get() ? SoftAsset.Get() : SoftAsset.LoadSynchronous();
	}

	/**
	 * 资产上的显示名，空的时候退回资产名。
	 *
	 * 【为什么要有兜底】DisplayName 是 FText，忘了填不会报错，界面上就是一块空白 ——
	 * 那看起来像 UI 坏了。退回资产名至少能让人一眼看出是哪件装备没填名字。
	 */
	static FText ResolveDisplayName(const FText& DisplayName, const UObject* Asset);

	// ---- 缓存（去重用的上一次推送）----
	//
	// 三个都必须是 UPROPERTY：里面装着 TObjectPtr<UTexture2D> 图标，
	// 不加 UPROPERTY 的话那些贴图会被 GC 掉，界面上表现为"图标过一会儿变成空白"。

	UPROPERTY(Transient)
	FArenaPromptView CachedPrompt;

	UPROPERTY(Transient)
	FArenaMatchView CachedMatch;

	UPROPERTY(Transient)
	FArenaLoadoutView CachedLoadout;

	UPROPERTY(Transient)
	FArenaRoundTrackerView CachedTracker;

	/**
	 * 上一次广播出去的相位。相位边沿委托（OnPhaseChanged）的基准：
	 * 只有它和这一帧构建出来的相位不同时才广播。必须跟着 CachedMatch 一起更新，
	 * 不然第一次广播前边沿基准是错的。
	 */
	EArenaPhaseView CachedPhase = EArenaPhaseView::Unknown;

	/**
	 * 绑着的两个对象。
	 *
	 * 【必须显式 Unbind】弱引用变 null 不会触发任何东西：PlayerState 被销毁之后
	 * 它的委托再也不会打进来，而弱引用只是静静地变空。所以 ShutdownArena 里
	 * 用这两个指针把委托一条条摘掉 —— 摘的时候要用同一批指针，所以它们得留着。
	 */
	UPROPERTY(Transient)
	TWeakObjectPtr<AArenaPlayerState> BoundSelfState;

	UPROPERTY(Transient)
	TWeakObjectPtr<AArenaGameState> BoundGameState;

	/** 重试 / 心跳共用的那一条定时器。 */
	FTimerHandle HeartbeatHandle;

	/** 已经试了几次绑定。绑上之后就归零不再用。 */
	int32 BindAttempts = 0;

	/**
	 * 心跳间隔（秒）。
	 *
	 * 0.25 是照着倒计时的显示精度挑的：界面上只显示整秒，所以只要比 1 秒密就够了；
	 * 而它同时兼任对手血条 / 对手战绩的刷新，所以也不能太稀。4Hz 的代价是每 0.25 秒
	 * 读几十个字段 + 比三个结构，可以忽略。
	 */
	static constexpr float HeartbeatInterval = 0.25f;

	/** 绑不上的重试上限。40 × 0.25 = 10 秒。 */
	static constexpr int32 MaxBindAttempts = 40;
};
