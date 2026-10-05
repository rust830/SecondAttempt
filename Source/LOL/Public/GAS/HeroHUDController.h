// GAS → UMG HUD 数据管道的翻译层。
//
// 职责边界（三句话）：
//   1. 【唯一的 GAS 语义出口】。属性 / 冷却 / 标签在这里被翻成「百分比 / 剩余秒数 / 灰化旗标」，
//      再往上的 Widget 只认 UI 语义 —— 它们连 GameplayTags 模块都不需要认识。
//   2. 【不复制状态】。真值全部现场从 PlayerState 上的 ASC + UHeroCombatAttributeSet 读，
//      这里只缓存一份「给 UI 看的投影」（FHUD*View），缓存坏了最多是显示错，不会影响游戏逻辑。
//   3. 【两条数据通道】。Self（本地玩家自己，跟 PS）+ Observed（目标框，跟选中的目标）。
//      敌人的头顶血条【不走这里】—— 那是「每个敌人一个」，见 UHeroOverlayHealthWidget。
//
// 三条纪律，违反任何一条都会在联网下出错：
//   · 属性用 GetGameplayAttributeValueChangeDelegate，【不用 OnRep_*】——
//     listen server 上主机是权威端，属性是直接写进去的，OnRep 不触发（症状：主机看不到自己的血条动）。
//   · 冷却数字【只采样、不递减】。自己减 float 会漂移，而且预测回滚时真实剩余时长会跳回去。
//   · 冷却状态【只由标签翻转】。倒计时跑完不点亮图标，必须等 State.Cooldown.X 真的落到 0。
//     数字可以回弹，状态不可以 —— 否则客户端会出现「图标亮着但按下去被拒」。
//
// 一条容易漏的：Observed 通道除了属性 + 标签，还要听目标的 OnDestroyed。
// 弱引用变 null 不会触发任何回调，只听属性/标签的话，目标一销毁目标框就冻在最后一帧。
//
// 放 GAS/ 而不是 UI/：它直接操作 ASC / GameplayEffect / GameplayTag（CONVENTIONS.md 规则 2）。

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GameplayTagContainer.h"
#include "GameplayEffectTypes.h"
#include "GAS/HUDAttributeBinding.h"
#include "UI/HUDTypes.h"
#include "HeroHUDController.generated.h"

class AActor;
class UHeroAttributePanelConfig;
class UHeroCombatAttributeSet;
class UHeroHUDSlotConfig;
class UMyAbilitySystemComponent;
class UWorld;
struct FHeroHUDSlotEntry;

/**
 * 本地玩家的 HUD 翻译层。Outer 必须是 ALOLPlayerController（ResolveSelfASC 顺着 Outer 找 PS）。
 *
 * 生命周期：PC 建它 → SetSlotConfig → TryBindHUD（三处调用点）→ ... → PC EndPlay 调 ShutdownHUD。
 */
UCLASS(BlueprintType)
class LOL_API UHeroHUDController : public UObject
{
	GENERATED_BODY()

public:
	// -----------------------------------------------------------------------
	// 绑定
	// -----------------------------------------------------------------------

	/**
	 * 幂等。
	 *
	 * 三个【显式】调用点：ALOLPlayerController 的 BeginPlay / OnRep_PlayerState / AcknowledgePossession。
	 * 注意这三处的覆盖是不对称的：主机（listen server）只有 BeginPlay 会跑，
	 * OnRep_PlayerState 和 AcknowledgePossession 都是客户端路径。所以除了这三处，
	 * 绑不上时还会开一个【有界重试】（0.2s × 25 次，最多 5 秒）兜底，
	 * 覆盖「PS 还没复制到」「ASC 还没 InitAbilityActorInfo」这类时序。
	 *
	 * 幂等键是「现在绑的是不是【同一个 ASC】」，不是 bool：
	 * OnRep_PlayerState 可能在 PS 被替换之后才跑，bool 会说「绑过了」，
	 * 而实际上绑在一个已经销毁的 ASC 上（症状：血条永远停在上一命的值）。
	 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void TryBindHUD();

	/** PC EndPlay 调：清定时器、解绑一切。调完还能再 TryBindHUD。 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void ShutdownHUD();

	UFUNCTION(BlueprintPure, Category = "HUD")
	bool IsHUDReady() const { return BoundASC.IsValid(); }

	// -----------------------------------------------------------------------
	// 拉取（订阅即拉取 —— 初始值不走广播）
	// -----------------------------------------------------------------------

	/**
	 * 拿一份当前完整状态。Widget 在 NativeConstruct 里调它。
	 *
	 * 这就是原来那个『绑定不触发初始值』坑的正解：Widget 的创建时机和 Controller 的绑定时机
	 * 是两条独立时间轴，靠「记得在正确时机 BroadcastInitialValues()」一定会漏。
	 * 让订阅方主动拉一次，初始值就不再依赖时序；Widget 重建（切关卡后重新 AddToViewport）也自动正确。
	 *
	 * 返回的是缓存的投影，缓存永远在事件/采样里同步更新，所以这份快照不会过期。
	 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	FHUDSnapshot PullHUDState() const;

	/** 给【已经在场的】订阅者重放全量。绑定成功 / 状态大变时调。 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void BroadcastInitialValues();

	// -----------------------------------------------------------------------
	// 配置
	// -----------------------------------------------------------------------

	/** PC 在建完 Controller、调 TryBindHUD 之前调。 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void SetSlotConfig(UHeroHUDSlotConfig* InConfig);

	/**
	 * 按【当前】SlotConfig 重算一遍全部槽位的静态表现并强制重播。**不重置运行期状态**。
	 *
	 * 【为什么需要这个入口】DataAsset 在 PIE 运行期被改时，引擎不会通知任何人；
	 * 而 CachedSlots 存的是一份【早就算好】的视图，PullHUDState() 只会把那份旧的再端一遍。
	 * 没有这个函数，「改了 SlotSizeScale 想立刻看看效果」就只能重启 PIE。
	 *
	 * 【和 SetSlotConfig 的区别】那个是"换配置"（槽位集合可能都变了，所以整块重来）；
	 * 这个是"同一份配置的内容被改了"，所以只刷新配置决定的那些量
	 *（Icon / KeyLabel / DisplayName / Kind / SlotSizeScale / bHidden），
	 * 冷却秒数、可用性状态、能力归属这些算出来的东西原样保留。
	 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void RefreshSlotViewsFromConfig();

	/**
	 * 属性面板配置。**必须在 TryBindHUD 之前调** —— 面板订阅哪些属性就是照着它来的，
	 * 晚于第一次绑定的话，第一次绑定不会订阅面板属性，面板会一直显示 0。
	 *
	 * 和 SetSlotConfig 一样是幂等重绑：换配置 = 订阅的属性集合变了，旧的依据已经没了。
	 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void SetAttributePanelConfig(UHeroAttributePanelConfig* InConfig);

	/** 技能栏槽位数（= UI 上要建几个 WBP_SkillSlot）。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	int32 GetNumSlots() const;

	// -----------------------------------------------------------------------
	// Observed 通道（目标框）
	// -----------------------------------------------------------------------

	/**
	 * 幂等重绑：先解绑旧目标、再绑新目标。传 nullptr = 收起目标框。
	 * 目标被销毁 / 切到新目标 / 切回 nullptr，三种走同一条路径 —— 漏了就是「血条乱跳」。
	 *
	 * 谁是「目标」由调用方决定（角色 / PC / 蓝图），HUD 层不猜。
	 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void SetObservedTarget(AActor* NewTarget);

	UFUNCTION(BlueprintPure, Category = "HUD")
	AActor* GetObservedTarget() const { return ObservedTarget.Get(); }

	// -----------------------------------------------------------------------
	// 委托（BlueprintAssignable）
	// -----------------------------------------------------------------------

	/** 血 / 能量。属性事件驱动，两端都触发。 */
	UPROPERTY(BlueprintAssignable, Category = "HUD")
	FOnHUDVitalsChangedSignature OnVitalsChanged;

	/** 单个技能槽：秒数 + 转圈进度 + 状态 + 原因一次给全。心跳和标签事件都走这一条。 */
	UPROPERTY(BlueprintAssignable, Category = "HUD")
	FOnSkillSlotChangedSignature OnSkillSlotChanged;

	/** 目标框（含「有没有目标」）。 */
	UPROPERTY(BlueprintAssignable, Category = "HUD")
	FOnTargetFrameChangedSignature OnTargetFrameChanged;

	/** 绑定状态。false 时 UI 显示「未就绪」，别显示空血条。 */
	UPROPERTY(BlueprintAssignable, Category = "HUD")
	FOnHUDReadySignature OnHUDReady;

	/**
	 * 英雄属性面板（整块）。属性事件驱动，两端都触发。
	 *
	 * 【为什么不是 30Hz 心跳】属性和冷却不一样：冷却是个连续递减的量，必须采样；
	 * 属性只在 GE 施加/移除的那一瞬间变，事件就够，而且事件比采样更准（不会漏掉一帧内的两次变化）。
	 */
	UPROPERTY(BlueprintAssignable, Category = "HUD")
	FOnHUDAttributesChangedSignature OnAttributesChanged;

private:
	// ---- 绑定实现 ----

	/** 顺着 Outer(PC) 找本地玩家的 ASC。找不到返回 nullptr。 */
	UMyAbilitySystemComponent* ResolveSelfASC() const;

	void BindSelf();
	void UnbindSelf();
	void UnbindObserved();
	void UnbindAll();

	/** 绑不上时的有界重试。自排程（一次性定时器），成功或超限就停。 */
	void StartBindRetry();
	void StopBindRetry();

	/** 广播一份「未就绪」的空快照。绑不上时不要留着上一命的残值。 */
	void ResetToUnboundState();

	// ---- 事件回调 ----

	/**
	 * Self 通道的四种资源（血 / 最大血 / 能量 / 最大能量）共用这一个回调 ——
	 * 处理逻辑完全一样（重算一遍，变了才推），分成四个只会多出「忘了绑其中一条」的机会。
	 * 能量那条额外要把所有槽重算一遍（喂「蓝不够」的灰化），那是 RebuildVitals 之后的第二件事。
	 */
	void OnSelfVitalsAttributeChanged(const FOnAttributeChangeData& Data);

	/**
	 * Self 通道的属性面板那批属性共用这一个回调。
	 *
	 * 【为什么一个回调能服务所有面板属性】RebuildAttributes 每次都是「读全部条目、重算整块」，
	 * 不关心是哪一个属性变了 —— 属性之间本来就会互相影响（比如以后加「攻速加成影响最终攻速」），
	 * 按属性分流反而要维护一张「谁影响谁」的表。
	 */
	void OnSelfAttributesAttributeChanged(const FOnAttributeChangeData& Data);

	/** 某个槽的冷却标签 0↔1。状态翻转的【唯一】入口。 */
	void OnCooldownTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	/** State.Dead / State.Stunned / State.Silenced 计数变化 → 重算所有槽的灰化。 */
	void OnStatusTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	/** Observed 通道的四种资源共用。 */
	void OnObservedVitalsAttributeChanged(const FOnAttributeChangeData& Data);

	void OnObservedStatusTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	/**
	 * 目标被销毁。【必须是 UFUNCTION】：`AActor::OnDestroyed` 是 sparse 动态多播委托，
	 * 只能 AddDynamic 绑。
	 *
	 * 为什么非要有这条：弱引用变 null 【不会】触发任何东西。目标一销毁，它的 ASC 跟着没了，
	 * 属性/标签事件再也不会打进来，于是目标框永远停在销毁前那一帧 ——
	 * 敌人是「死了之后过一会儿才 Destroy」，所以表现是屏幕上留一个灰掉的血条不走。
	 */
	UFUNCTION()
	void OnObservedTargetDestroyed(AActor* DestroyedActor);

	// ---- 重算 / 广播 ----

	void RebuildVitals(bool bForceBroadcast);
	void RebuildAttributes(bool bForceBroadcast);
	void RebuildSlot(int32 SlotIndex, bool bForceBroadcast);
	void RebuildAllSlots(bool bForceBroadcast);
	void RefreshTargetFrame(bool bForceBroadcast);

	/** 心跳回调：只刷新【数字】，绝不改状态（见文件头第三条纪律）。 */
	void SampleCooldowns();

	/** 有任一槽的冷却标签在 → 开心跳；全落 → 停。开关只写这一处。 */
	void UpdateCooldownTicker();

	/** 冷却数值的【唯一】来源。现场查，不缓存递减。 */
	bool QueryCooldown(int32 SlotIndex, float& OutRemaining, float& OutDuration) const;

	/** 旗标 → 表现状态。优先级只写这一处，Widget 里不该有第二个判断。 */
	ESkillSlotState ResolveSlotState(bool bHasAbility, ESkillSlotBlockReason Reasons) const;

	FText ResolveTargetDisplayName(const AActor* Target) const;

	/** 保证 CachedSlots 的长度和 SlotConfig 对齐。 */
	void EnsureSlotCacheSize();

	/** 配置里的静态表现 → 视图。不含状态 —— 那部分要跟 ASC 说话。 */
	void ApplyEntryStatic(FSkillSlotView& View, const FHeroHUDSlotEntry* Entry) const;

	/**
	 * 数值 → 显示文本。格式化的【唯一】实现。
	 *
	 * 放在 Controller 而不是 Widget：这样 UI 层永远只收到 FText，不需要认识 EHeroAttributeFormat，
	 * 也不需要认识配置资产（同 FSkillSlotView 把 Icon / KeyLabel 提前摘出来的理由）。
	 *
	 * 做成 static 成员而不是文件内 free function：unity build 把多个 .cpp 合进同一个 TU 时，
	 * 匿名 namespace 里的同名函数会撞（这个坑本项目已经踩过一次）。
	 */
	static FText FormatAttributeValue(float Value, EHeroAttributeFormat Format);

	/**
	 * 预留位的【唯一】判据：配了 bHideWhenUnavailable 且这个槽上现在没有能力。
	 *
	 * 做成 static 成员而不是文件内 free function：既能被四处共用，又不会像匿名 namespace
	 * 那样在 unity build 合并 TU 时跟别的文件撞名（这个坑本项目已经踩过一次）。
	 */
	static bool IsReservedSlotHidden(const FHeroHUDSlotEntry* Entry, bool bHasAbility);

	// ---- 状态 ----

	/** 槽位映射表（含顺序 + 静态表现）。强引用：资产要在整局里活着。 */
	UPROPERTY(Transient)
	TObjectPtr<UHeroHUDSlotConfig> SlotConfig;

	/**
	 * 属性面板配置。强引用（同 SlotConfig）。
	 *
	 * 注意它同时承担两个职责：① 面板显示什么；② Self 通道要订阅哪些属性。
	 * 合成一份是有意的 —— 两份配置迟早会出现「面板上有这条、但订阅漏了它」这种
	 * 「显示 0 且永远不动」的坏状态，而那种症状看起来像数值算错了。
	 */
	UPROPERTY(Transient)
	TObjectPtr<UHeroAttributePanelConfig> AttributePanelConfig;

	/** Self 通道绑着的那个 ASC。弱引用 —— 强引用会把 PlayerState 一起钉住。 */
	UPROPERTY(Transient)
	TWeakObjectPtr<UMyAbilitySystemComponent> BoundASC;

	/**
	 * Self 通道的属性委托 + 标签事件记账。
	 *
	 * 它自己内部也存一份弱引用 ASC（Unbind 时要用它找回那几条委托）。
	 * 和上面 BoundASC 的分工是刻意的，别合并：BoundASC 是【身份】（TryBindHUD 的幂等键、
	 * 所有数值读取都走它），绑定的句柄是【副作用】。混在一起会让幂等判断依赖绑定状态，
	 * 而绑定失败恰恰是幂等判断要处理的情况之一。
	 */
	FHUDAttributeBinding SelfBinding;

	/** Observed 通道。用基类指针即可：只读属性 + 听标签。 */
	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> ObservedTarget;
	UPROPERTY(Transient)
	TWeakObjectPtr<UAbilitySystemComponent> ObservedASC;

	/** Observed 通道的记账。目标被销毁时还要额外摘掉它的 OnDestroyed（见 UnbindObserved）。 */
	FHUDAttributeBinding ObservedBinding;

	/** 给 UI 看的投影（缓存）。PullHUDState() 直接返回它。 */
	UPROPERTY(Transient)
	FHUDVitalsView CachedVitals;
	UPROPERTY(Transient)
	TArray<FSkillSlotView> CachedSlots;
	UPROPERTY(Transient)
	FTargetFrameView CachedTarget;
	UPROPERTY(Transient)
	FHeroAttributePanelView CachedAttributes;

	/** 冷却数字的采样心跳。 */
	FTimerHandle CooldownTickHandle;
	/** 绑定的兜底重试（自排程一次性定时器）。 */
	FTimerHandle BindRetryHandle;
	int32 BindRetryCount = 0;

	/** 冷却数字的采样频率。30Hz 够用（人眼分不出 30 和 60 的数字跳变），又比每帧便宜。 */
	static constexpr float CooldownTickInterval = 1.f / 30.f;
	/** 兜底重试的间隔与上限。5 秒还绑不上就当这个 PlayerState 真的没有 ASC（比如观战）。 */
	static constexpr float BindRetryInterval = 0.2f;
	static constexpr int32 MaxBindRetries = 25;
};
