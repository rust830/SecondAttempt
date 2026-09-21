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
// 放 GAS/ 而不是 UI/：它直接操作 ASC / GameplayEffect / GameplayTag（CONVENTIONS.md 规则 2）。

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GameplayTagContainer.h"
#include "GameplayEffectTypes.h"
#include "UI/HUDTypes.h"
#include "HeroHUDController.generated.h"

class AActor;
class UHeroCombatAttributeSet;
class UHeroHUDSlotConfig;
class UMyAbilitySystemComponent;
class UWorld;

/** 一条已注册的标签事件。解绑要 (Handle, Tag) 成对，所以要一起存。 */
struct FHUDBoundTagEvent
{
	FGameplayTag Tag;
	FDelegateHandle Handle;
};

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

	void OnHealthAttributeChanged(const FOnAttributeChangeData& Data);
	void OnMaxHealthAttributeChanged(const FOnAttributeChangeData& Data);
	void OnEnergyAttributeChanged(const FOnAttributeChangeData& Data);
	void OnMaxEnergyAttributeChanged(const FOnAttributeChangeData& Data);

	/** 某个槽的冷却标签 0↔1。状态翻转的【唯一】入口。 */
	void OnCooldownTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	/** State.Dead / State.Stunned / State.Silenced 计数变化 → 重算所有槽的灰化。 */
	void OnStatusTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	void OnObservedHealthChanged(const FOnAttributeChangeData& Data);
	void OnObservedMaxHealthChanged(const FOnAttributeChangeData& Data);
	void OnObservedEnergyChanged(const FOnAttributeChangeData& Data);
	void OnObservedMaxEnergyChanged(const FOnAttributeChangeData& Data);
	void OnObservedStatusTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	// ---- 重算 / 广播 ----

	void RebuildVitals(bool bForceBroadcast);
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

	// ---- 状态 ----

	/** 槽位映射表（含顺序 + 静态表现）。强引用：资产要在整局里活着。 */
	UPROPERTY(Transient)
	TObjectPtr<UHeroHUDSlotConfig> SlotConfig;

	/** Self 通道绑着的那个 ASC。弱引用 —— 强引用会把 PlayerState 一起钉住。 */
	UPROPERTY(Transient)
	TWeakObjectPtr<UMyAbilitySystemComponent> BoundASC;

	FDelegateHandle HealthHandle;
	FDelegateHandle MaxHealthHandle;
	FDelegateHandle EnergyHandle;
	FDelegateHandle MaxEnergyHandle;
	TArray<FHUDBoundTagEvent> BoundTagEvents;

	/** Observed 通道。用基类指针即可：只读属性 + 听标签。 */
	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> ObservedTarget;
	UPROPERTY(Transient)
	TWeakObjectPtr<UAbilitySystemComponent> ObservedASC;

	FDelegateHandle ObservedHealthHandle;
	FDelegateHandle ObservedMaxHealthHandle;
	FDelegateHandle ObservedEnergyHandle;
	FDelegateHandle ObservedMaxEnergyHandle;
	TArray<FHUDBoundTagEvent> ObservedTagEvents;

	/** 给 UI 看的投影（缓存）。PullHUDState() 直接返回它。 */
	UPROPERTY(Transient)
	FHUDVitalsView CachedVitals;
	UPROPERTY(Transient)
	TArray<FSkillSlotView> CachedSlots;
	UPROPERTY(Transient)
	FTargetFrameView CachedTarget;

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
