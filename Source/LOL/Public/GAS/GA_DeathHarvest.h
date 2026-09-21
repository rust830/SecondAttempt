// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GameplayEffectTypes.h"
#include "Engine/EngineTypes.h"          // FTimerHandle（P4 起手甩镜的逐帧驱动）
#include "GA_DeathHarvest.generated.h"

class ACharacter;
class UAbilityTask_PlayMontageAndWait;
class UAbilityTask_Repeat;
class UAbilityTask_WaitDelay;
class UAnimMontage;
class UDeathHarvestData;
class UGameplayEffect;

/**
 * R 大招「Death Harvest」：按住 R 选目标 → 左键点中才放 → 消失 → 目标身后开门 → 现身 → 原地转一圈 → 收招。
 *
 * 选目标（v3）：
 *   按下 R   什么都【不发生】—— 不激活技能、不进冷却、不锁移动、不发 cue、也不破隐。
 *            输入层（AHeroCombatCharacter）只挂一个本地标签 State.DeathHarvest.Selecting 当窗口。
 *   左键     窗口里的左键 = 选目标：客户端从相机准星打一条射线，打到谁就把谁随 RPC 交给服务端。
 *   松开 R   关窗口，这次当没按过。
 *   服务端   收到目标 → 校验（有 ASC / 活着 / 在 LockRange 内 / 视线不挡）→ CommitAbility →
 *            本 ActivateAbility 正式跑起来，之后就是原来那条链路。
 *
 *   所以这个技能是 ActivationPolicy = OnEvent：R 键【不是】它的激活键（ASC 的槽位按键路由会跳过
 *   非 OnInputTriggered 的能力），激活它的是带载荷的事件 Event.DeathHarvest.CastAt
 *   （UMyAbilitySystemComponent::SubmitManualTargetOnServer 发的，载荷 = 点了谁）。
 *   「点了谁」只能由客户端给：相机和准星都在客户端，而 GameplayEvent 只在本地派发、自己不过网络。
 *
 * 相位时间轴（v2：相位不再由蒙太奇上的 notify 推 —— P1~P3 这三段根本没有蒙太奇在播，
 * notify 无从挂起。四条 Event.DeathHarvest.* 标签和四个 WaitGameplayEvent 都已退役）：
 *
 *   P1 消失  锁移动 + 挂 GC.DeathHarvest.Cast（各端藏人 + 关碰撞，主人那一端再叠传送镜头）
 *   P2 开门  WaitDelay(VanishToPortalDelay) → 算落点 → 挂 GC.DeathHarvest.Portal（带落点坐标）
 *   P3 现身  WaitDelay(PortalToAppearDelay) → 传送 + 摘上面两条 cue（各端恢复可见 + 现身 burst）
 *   P4 起手  【现身同一帧】甩镜 + 挂转圈 cue + 播蒙太奇（BeginSpinLead / StartSpinVisuals）
 *   P4 命中  SpinImpactDelay 秒后 = 刀真正扫起来那一帧：慢她自己 + 起伤害跳；第一跳真打到人
 *            就顿帧 + 镜头震动（OnSpinImpactDelayFinished / BeginHitStop）
 *   P5 收招  蒙太奇播完 → EndAbility，移动/朝向/标签/时间流速在那里收干净
 *
 * P4 起手的两条纪律（都很容易漏，漏了都不报错）：
 *   ① 慢放动的是【她自己】的 AActor::CustomTimeDilation，不是世界时间 —— 所以服务器那一份也得设：
 *      不设的话服务器的蒙太奇不慢，它会比施法者本机早一截 EndAbility，把人家的动画掐掉一段。
 *      （v3 那版用 SetGlobalTimeDilation 把整个世界一起拖慢，代价和坑都大得多，见实现里的对照。）
 *   ② 两种时间流速都必须在 EndAbility 里兜底还原：技能被打断时唯一能走到的地方就是那儿。
 *      角色那份漏了 = 她一直慢动作；世界那份漏了 = 整个世界卡在顿帧的流速上。
 *
 * 为什么打击感要晚 SpinImpactDelay 秒（v3.1 之前是"现身那一帧就顿"）：蒙太奇只有一段 Spin，
 * 开头那段是抬刀/转上身，刀真正扫出去在这之后 —— 慢放和震打在蒙太奇第 0 帧上，玩家看到的是
 * "现身之后卡了一下"，那一下和刀的加速度完全对不上。推后之后：蒙太奇全程正常速度播（不傻站），
 * 顿和震正好落在第一个伤害跳上（伤害跳 = 刀扫到人 = 第一跳本该发生的那一帧）。
 *
 * 被打断（死亡/被控/落点全废）时唯一能走到的收尾点就是 EndAbility：两条状态 cue 都是用
 * K2_AddGameplayCueWithParams(..., bRemoveOnAbilityEnd=true) 挂的，Super::EndAbility 会按
 * TrackedGameplayCues 摘干净（摘不掉的表现是「人一直隐形，还带着传送镜头和关闭的碰撞」）。
 *
 * 网络模型（全项目唯一的例外，见 GAS_DeathHarvest_Setup.md §1.1）：
 *  NetExecutionPolicy = ServerInitiated。服务器先激活、独占时间轴；确认之后引擎会给本端客户端
 *  发 ClientActivateAbilitySucceed，本端跟着本地再跑一次 ActivateAbility ——
 *  客户端这一次【只做两件事】：锁移动 + 按同一份时长对齐后播转圈蒙太奇，其余一律 HasAuthority() 门住。
 *
 *  为什么不用 ServerOnly：ServerOnly 时引擎不给本端发 ClientActivateAbilitySucceed
 *  （AbilitySystemComponent_Abilities.cpp 里 `!= ServerOnly` 那个判断），而蒙太奇复制是
 *  "for non-owners"（OnRep_ReplicatedAnimMontage 整个函数包在 !IsLocallyControlled() 里），
 *  两头都堵死 → 【本人看不到自己的大招动画】。
 *
 *  代价（写死在代码里了）：客户端那份实例【绝对不能】调 CommitAbility —— 否则冷却 GE 会在本端
 *  再挂一份（服务端那份还会复制过来），本端 CD 比服务端长一个 RTT。见 ActivateAbility 的分支。
 *
 *  选目标这条链【没有】预测：客户端点完只是把目标交给服务端，本端那份时间轴照样等
 *  ClientActivateAbilitySucceed 才开始（和服务器上真正那次激活严格对齐）。好处是「服务端判定不过」
 *  不会在本端留下一段空转的动画，坏处是点下去到起手之间有一个单程 RTT —— 和原来按下即锁的差别。
 */
UCLASS(Blueprintable)
class LOL_API UGA_DeathHarvest : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_DeathHarvest();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	/** 全部可调数值（一个英雄一份，见 UDeathHarvestData 的注释）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Config")
	TObjectPtr<UDeathHarvestData> AbilityData;

protected:
	// --- 相位（全部跑在服务器上，客户端只对齐时间轴） ---

	/** P1 → P2：消失时长走完，开门。 */
	UFUNCTION() void OnVanishFinished();
	/** P2 → P3：门开够了，传送 + 现身。 */
	UFUNCTION() void OnPortalFinished();

	/** 客户端那份：P1~P3 的时长走完，本端到「现身」那一下了。 */
	UFUNCTION() void OnClientDelayFinished();

	/** 起手之后 SpinImpactDelay 秒到了 = 刀真正扫起来那一帧。 */
	UFUNCTION() void OnSpinImpactDelayFinished();

	/** P4 起手慢放走完，把她自己的时间速率还原。 */
	UFUNCTION() void OnSpinSlowMotionFinished();

	/** 命中顿帧走完，把世界时间还原。 */
	UFUNCTION() void OnHitStopFinished();

	/** 蒙太奇播完/被打断。客户端那份【不】结束技能，收尾由服务器发 ClientEndAbility。 */
	UFUNCTION() void OnMontageFinished();

	// --- 转圈伤害跳（UAbilityTask_Repeat） ---
	UFUNCTION() void OnSpinPulse(int32 ActionNumber);

private:
	// ---------------------------------------------------------------------
	// 服务器侧
	// ---------------------------------------------------------------------

	/**
	 * P0：校验客户端点出来的那个目标。服务端【只验不选】—— 相机在客户端、服务端没有准星，
	 * 所以方向不用验（服务端读到的 control rotation 在点下去那一刻已经不是相机的方向了），
	 * 能验的是：不是自己、有 ASC、活着、在 LockRange 内、视线不被挡。
	 *
	 * 通过 = 这次真放出去了；不通过 = 当没放过（EndAbility(cancelled)，不进冷却）。返回 false 时调用方必须结束技能。
	 */
	bool ValidateTarget(AActor* Target) const;

	/**
	 * 关掉输入层那个「按住 R 选目标」的窗口（摘本地标签 State.DeathHarvest.Selecting）。
	 * 只在【确认成功之后】调（服务器在 CommitAbility 之后、客户端那份在 ActivateAbility 开头）——
	 * 判定不过时窗口必须留着，玩家才走得近一点再点一次。两端都能调，摘不到就是 no-op。
	 */
	void ClearManualSelectWindow();

	/**
	 * P2：算落点。目标身后 → 贴地 → 胶囊扫描判站得下。
	 * 身后站不下就按候选表退让：身后 → 左后 → 右后 → 正前 → 目标正上方。
	 * 返回 false = 全废（调用方取消技能并退冷却）。
	 *
	 * ⚠️ OutDestination 是【地面表面点】（地面射线的命中点），不是胶囊中心：P3 传送时要自己补
	 * 半个胶囊高，否则 actor 原点（= 胶囊中心）落在地面上，人有一半在地下。
	 */

	bool ComputeDestination(AActor* Target, FVector& OutDestination) const;

	/**
	 * 落点能不能站下（胶囊扫描，照 GA_Flash::TryFindBlinkDestination 的写法）。
	 * ⚠️ Location 传的是【地面表面点】：检测会把胶囊抬 2cm 再判，否则"胶囊底正好贴在地面上"
	 * 这种零深度接触会被当成重叠，平地上每个点都被地面自己挡掉（见实现里的说明）。
	 */
	bool IsSpotFree(const FVector& Location, const AActor* IgnoredActor) const;

	/** P1：锁移动 + 挂「消失中」cue + 起第一个相位计时。 */
	void StartVanishPhase();

	/**
	 * P4 起手那一帧（现身同一帧）的可见部分：关自动转向 + 挂转圈 cue + 播蒙太奇。
	 * 【不】含伤害跳 —— 伤害归 StartSpinPulses，晚 SpinImpactDelay 秒。
	 */
	void StartSpinVisuals();

	/**
	 * 转圈伤害跳（Repeat 任务，第一跳在 Activate 时立刻打）。只该在服务器调。
	 * 起手那一帧之后 SpinImpactDelay 秒才轮到它 —— 对齐"刀真正扫起来"那一帧。
	 */
	void StartSpinPulses();

	void StopSpin();

	// --- P4 起手（现身那一帧，见头文件开头的阶段表） ---

	/**
	 * 起手：甩镜 + 排 SpinImpactDelay。两端各调各的。
	 * 慢放【不】在这里 —— 它跟着打击感一起推后到 OnSpinImpactDelayFinished。
	 */
	void BeginSpinLead();

	/** 把起手慢放排到 SpinImpactDelay 之后（0 = 不排，当场就顿）。 */
	void ArmSpinImpact();

	/** SpinImpactDelay 到点：慢自己 +（服务器）起伤害跳。 */
	void OnSpinImpact();

	/** 起手收尾：还原她的时间速率 + 停甩镜。幂等，EndAbility 里还会兜底调一次。 */
	void EndSpinLead();

	/** 把【她自己的】时间速率调慢（AActor::CustomTimeDilation），SpinSlowTime 秒后还原。 */
	void BeginSpinSlowMotion();
	void EndSpinSlowMotion();

	/** 把镜头往「自己 → 敌人」那条线上甩，CameraAlignTime 秒后停手、把镜头交回玩家。 */
	void BeginCameraAlign();
	void EndCameraAlign();

	/**
	 * 命中顿帧：把【世界】时间冻住 HitStopTime 秒。只在服务器调（会随 WorldSettings 复制到全场）。
	 * 空挥不顿 —— 调用方只在真的打到了人时才调它。
	 */
	void BeginHitStop();
	void EndHitStop();

	/**
	 * 真实秒 → 本机世界秒。世界计时器跟的是膨胀后的 delta，所以世界被调慢期间要等一段
	 * 【真实】时长就必须折算。只用在命中顿帧上（角色速率不影响世界计时器，不用折算）。
	 */
	float ScaledWorldSeconds(float RealSeconds) const;

	/** 这台机器上，施法者是不是我在本地控制（决定要不要甩镜 —— 那是纯本机表现）。 */
	bool IsLocallyControlledAvatar() const;

	/** 甩镜：把 control rotation 往「自己 → 目标」推一帧，然后排下一帧。 */
	void TickCameraAlign();
	/** 起甩镜的逐帧驱动（SetTimerForNextTick 自递归，理由见实现）。 */
	void ArmCameraAlignTick();

	/** 服务器发一条带坐标的一次性 cue（ExecuteGameplayCue 会多播到每一端）。 */
	void ExecuteLocationCue(const FGameplayTag CueTag, const FVector& Location, const FVector& Normal = FVector::ZeroVector) const;

	/**
	 * 服务器挂一条【状态】cue（带坐标），由 bRemoveOnAbilityEnd 兜底、也可以自己提前摘。
	 * 一次性表现走 ExecuteLocationCue，有生命周期的走这个。
	 */
	void AddLocationCue(const FGameplayTag CueTag, const FVector& Location, const FVector& Normal = FVector::ZeroVector);

	/** 一跳：球形扫描 → 每个目标填 spec + 施加 + 命中 cue（命中了就顺便顿一下帧）。 */
	void OnSpinPulse_Server();

	/** 返回"这一下真的造成了伤害" —— 命中顿帧和震动都挂在它上面，空挥不算。 */
	bool ApplySpinDamage(AActor* Target, const FHitResult& Hit);

	/** 锁移动（StopMovementImmediately + MOVE_None），恢复时还原原模式。两端各锁各的。 */
	void ApplyMovementLock(bool bLock);

	/**
	 * 转圈期间关掉移动组件的自动转向（bOrientRotationToMovement / bUseControllerRotationYaw）。
	 * 代码【不】驱动 yaw：转的是动画，一圈转完正好回到现身时定的朝向。
	 *
	 * ★ 两端各关各的：这两个开关是【每台机器各自】的组件/角色属性，不复制。服务器在 P4 起手关它那一份，
	 *   客户端在【本端现身那一帧】关自己这一份。只关服务器的话，本端移动组件收到纠偏时会拿本端上一次
	 *   移动里存下的朝向把 actor 的朝向写回去（判据正是 bOrientRotationToMovement）—— 表现就是
	 *   「主机看她是好的，她自己屏幕上位置对、朝向不对」。详见 cpp 里 OnClientDelayFinished 的注释。
	 */
	void ApplySpinRotationOverride(bool bOn);

	/** 技能失败收尾：退冷却 + EndAbility(cancelled)。落点全废/目标丢失都走这里。 */
	void CancelAsFailed();

	/** 客户端那一次：锁移动 + 等 P1~P3 的时长 + 播蒙太奇。 */
	void PlayClientTimeline();

	/**
	 * 客户端那一份在【本端现身那一帧】把服务器 P3 那次传送自己补一遍。
	 *
	 * ★ 为什么非补不可：自主代理的坐标是自己权威的 —— 服务器那次 SetActorLocation 改的是服务器那一份，
	 *   要等【下一次 ServerMove 的纠偏】才会回到本端，而 P1 起移动就是 MOVE_None，本端不发 ServerMove，
	 *   纠偏永远不来。表现是"主机看她是好的，她自己屏幕上在消失的原处转"（门在了、人没过去）。
	 *   落点用和服务器同一个 ComputeDestination 重算（只吃目标和场景，两端输入一致 → 结果一致）。
	 */
	void ApplyClientTeleport();
	/** 客户端那一次真正播蒙太奇（延后到 P4 才调）。 */
	void PlayClientMontage();
	/** 服务器那一次：播转圈蒙太奇 + 绑完成回调（播完就收招）。 */
	void PlaySpinMontage();

	// ---------------------------------------------------------------------
	// 状态
	// ---------------------------------------------------------------------

	/**
	 * 锁定的目标。弱引用：目标中途被销毁时 Get() 返回 null，每个相位边界都重校验一次。
	 * ★ 两端都填：服务器拿它算落点/结算，客户端拿它定甩镜的瞄准方向（落点 → 敌人）
	 *   （载荷由 ClientActivateAbilitySucceedWithEventData 带过来，见 ActivateAbility）。
	 */
	TWeakObjectPtr<AActor> LockedTarget;

	/**
	 * P2 算出来的落点，P3 用。★ 两端各算各的：服务器 P2 算它来传送，客户端在【本端现身那一帧】
	 * 用同一个 ComputeDestination 再算一遍落到自己 pawn 上（原因见 ApplyClientTeleport）。
	 */
	FVector PortalLocation = FVector::ZeroVector;

	/**
	 * 相位计时。用任务而不是裸 FTimerHandle：任务跟着技能一起死，被打断时不会有孤儿回调
	 * （反弹的表现是「人已经死了，两秒后大招还在推相位」）。
	 */
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitDelay> VanishDelayTask;
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitDelay> PortalDelayTask;
	/** 客户端那一份的对齐计时（时长 = 上面两个之和）。 */
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitDelay> ClientDelayTask;

	/**
	 * 起手 → 第一跳之间的那段（等 SpinImpactDelay，【真实】秒）。
	 * 这一段里蒙太奇正常速度播（所以不会出现"提前现身站着等"），走完才慢放 + 起伤害。
	 */
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitDelay> SpinImpactTask;

	/** P4 起手慢放的等待（等 SpinSlowTime —— 角色速率不影响世界计时器，所以填的就是真实秒）。 */
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitDelay> SpinSlowTask;

	/** 命中顿帧的等待（等 HitStopTime，已经按【顿帧期间】的世界流速折算过）。 */
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitDelay> HitStopTask;

	UPROPERTY(Transient) TObjectPtr<UAbilityTask_PlayMontageAndWait> MontageTask;

	/** 伤害跳。用任务而不是裸 FTimerHandle，理由同上。 */
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_Repeat> SpinTask;

	/** 锁移动前记下的模式，恢复时用（默认值只为让"没锁过就恢复"是安全的）。 */
	EMovementMode SavedMovementMode = MOVE_Walking;
	bool bMovementLocked = false;

	/** 甩镜的逐帧定时器（自递归）+ 上一帧的【真实】时间，用来算不受慢放影响的 delta。 */
	FTimerHandle CameraAlignTimer;
	bool bCameraAligning = false;
	/** 甩镜首帧的日志只打一次（逐帧打会刷屏）。每次 BeginCameraAlign 重置。 */
	bool bCameraAlignLoggedFirstTick = false;
	/**
	 * P1 消失那一刻她在哪。两端各记各的（服务器 StartVanishPhase、客户端 PlayClientTimeline）。
	 * 甩镜靠它推「落点 → 敌人」那条线：客户端的自主代理不一定套用服务器 P3 那次传送，
	 * 本端坐标可能还停在这里 —— 那一步只能反推。→ TickCameraAlign 里那段"为什么需要它"。
	 */
	FVector VanishLocation = FVector::ZeroVector;
	bool bHasVanishLocation = false;
	/** 甩镜实际跑了几帧。收尾日志里要它：帧数太少 = 镜头还没转到位就被掐了。 */
	int32 CameraAlignTicks = 0;
	double LastCameraAlignRealTime = 0.0;
	/** 甩镜该在【真实】时间轴的哪一刻停手（慢放不会让它变长或变短）。 */
	double CameraAlignEndRealTime = 0.0;

	/**
	 * 起手慢放是不是【我】按下去的，以及按之前的角色速率。
	 * 只还原自己按的那一次：别的系统也给她上过速率时，无条件写回 1.0 会把它们一起抹掉。
	 */
	bool bSpinSlowMotionActive = false;
	float SavedCustomTimeDilation = 1.f;

	/**
	 * 命中顿帧是不是【我】按下去的（还没还原）/ 这一趟技能里已经顿过了。
	 * 后者要每次 ActivateAbility 清一次：技能实例是 InstancedPerActor，整个对局复用同一个对象。
	 */
	bool bHitStopApplied = false;
	bool bHitStopDone = false;
	/** 顿帧前的原始 TimeDilation —— 同理只还原自己按的那一次。 */
	float SavedTimeDilation = 1.f;

	/** 旋转覆盖是否生效 + 被覆盖前的原值（照 ThrowDaggerAbility 那套，只在真关过时才恢复）。 */
	bool bOrientRotationOverridden = false;
	bool bSavedOrientRotationToMovement = true;
	bool bSavedUseControllerRotationYaw = false;

	/** State.DeathHarvest.Casting 是不是我挂的（只挂/摘自己挂的那一份）。 */
	bool bCastingTagAdded = false;
};
