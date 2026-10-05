// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GA_DeathHarvest.h"
#include "GAS/DeathHarvestData.h"
#include "GAS/GE_DeathHarvestCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/LocalPlayerUtils.h"
#include "GAS/MyAbilitySystemComponent.h"   // FindAbilitySystemComponent：目标校验要用和客户端同一个解析器
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Abilities/Tasks/AbilityTask_Repeat.h"
#include "Abilities/Tasks/AbilityTask_WaitDelay.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"   // 命中顿帧：世界时间流速 + 真实 delta
#include "GAS/HeroCombatAttributeSet.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"                 // P4 起手：甩镜的逐帧驱动（SetTimerForNextTick）

namespace
{
	/**
	 * 日志用：这台机器在世界里扮演什么角色。
	 * PIE 下各端（主机 + 每个客户端）的日志全混在同一个输出窗口里，不标出来根本分不清是谁在说话 ——
	 * 排查「相位卡住」这类问题时，第一个要回答的就是「这行是谁打的」。
	 */
	const TCHAR* NetModeText(const UWorld* World)
	{
		switch (World ? World->GetNetMode() : NM_Standalone)
		{
		case NM_Standalone:      return TEXT("单机");
		case NM_DedicatedServer: return TEXT("专用服务器");
		case NM_ListenServer:    return TEXT("主机");
		case NM_Client:          return TEXT("客户端");
		default:                 return TEXT("未知");
		}
	}
}

UGA_DeathHarvest::UGA_DeathHarvest()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// ★ 全项目唯一的 ServerInitiated，理由见头文件注释 / GAS_DeathHarvest_Setup.md §1.1。
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerInitiated;

	// ★ R 键【不激活】这个技能。按下 R 只是让输入层开一个「选目标」窗口
	//   （AHeroCombatCharacter::AbilityInputTagPressed 读下面的 bManualTargetSelect 标记后拦下按键），
	//   左键点中目标才把目标当载荷发过来激活。所以这里是事件型而不是按键型 ——
	//   ASC 的槽位按键路由会跳过非 OnInputTriggered 的能力（MyAbilitySystemComponent::AbilityInputTagPressed）。
	ActivationPolicy = EMyAbilityActivationPolicy::OnEvent;

	// 从按键那一刻挪到「确认施法」那一刻：按住 R 时不破隐（还在选目标），点中目标那一下才破
	//（客户端本地摘 + SubmitManualTargetOnServer 在权威端再摘一次）。
	bManualTargetSelect = true;

	// 激活这个技能的那条事件：「左键点中的是谁」（载荷 = Payload.Target）。
	// 触发标签必须用原生标签对象：CDO 构造阶段字符串查标签返回 None，AbilityTriggers 会静默失效
	// （表现是「点了目标什么都不发生」）。发事件的是 UMyAbilitySystemComponent::SubmitManualTargetOnServer，
	// 它从能力身上读这个标签（GetGameplayEventTriggerTag），自己不写死任何具体技能的事件名。
	{
		FAbilityTriggerData Trigger;
		Trigger.TriggerTag = LOLGameplayTags::Event_DeathHarvest_CastAt;
		Trigger.TriggerSource = EGameplayAbilityTriggerSource::GameplayEvent;
		AbilityTriggers.Add(Trigger);
	}

	CooldownDuration = 90.f;
	CooldownGameplayEffectClass = UGE_DeathHarvestCooldown::StaticClass();

	// 大招标签：以后「施法者死亡/被控时取消所有大招」那条路要 CancelAbilitiesWithTag(Ability.Type.Ultimate)，
	// 现在项目里还没有那条路（见 GAS_DeathHarvest_Setup.md §7 的架构缺口），但标签先挂上。
	// AbilityTags 在 5.5 起弃用（要往私有化走），构造函数里改用 SetAssetTags 一次性设定。
	// 效果等价：读的那一侧（MyAbilitySystemComponent 查 Ability.Policy.*）走 GetAssetTags()。
	SetAssetTags(FGameplayTagContainer(LOLGameplayTags::Ability_Type_Ultimate));

	// 挡重入。技能期间挂自己的 State.DeathHarvest.Casting（服务器本地标签，见 ActivateAbility）。
	// 冷却 GE 其实也挡得住，但冷却要等 CommitAbility 之后才有 —— 这两个覆盖的时间窗不一样。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_DeathHarvest_Casting);

	// 沉默挡大招。死亡/眩晕在基类（UMyGameplayAbility 构造函数）已经挡了，别重复加。
	// 注意：死亡打断正在施放的大招【不走 ActivationBlockedTags】（那是「不让开新的」），
	// 走的是 UGE_Death 上的 UCancelAbilityTagsGameplayEffectComponent。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Silenced);

	// 从隐身开 R 仍然破隐，但时机变了：按下 R 不破（那一下只是开窗口），【点中目标】那一刻才破 ——
	// 破隐的本地那一半在 AHeroCombatCharacter::RouteManualTargetConfirm，权威那一半在
	// UMyAbilitySystemComponent::SubmitManualTargetOnServer（都调 BreakStealthForCast）。
	// 好处是「按住 R 瞄人」不会把自己从隐身里暴露出去（GAS_DeathHarvest_Setup.md §5.1 讲的是 v2 的按键即破）。
	bBreaksStealthOnCast = true;
}

// =============================================================================
// 激活
// =============================================================================

void UGA_DeathHarvest::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}


	// P0 目标。目标不是这里选出来的，是客户端左键点出来、随事件载荷送过来的（Payload.Target）。
	//
	// ★ 取在【客户端分支之前】：两端都要它。服务器拿它校验/算落点/结算伤害，
	//   客户端拿它定 P4 起手甩镜的那条「自己 → 敌人」线（这个函数里就记进 LockedTarget）。
	//
	// 载荷里的 Target 是 const 视图（FGameplayEventData 里就声明成 TObjectPtr<const AActor>），
	// 而它后面要当 AActor* 用（落点/朝向/伤害结算）。去掉 const 是安全的：这个指针本来就是
	// 本端（或按键那位玩家的服务端）以 AActor* 塞进载荷的，载荷从头到尾没换过对象。
	AActor* Target = const_cast<AActor*>(TriggerEventData ? TriggerEventData->Target.Get() : nullptr);

	// 技能实例是 InstancedPerActor —— 整个对局复用同一个 UObject，上一趟的旗标会留到这一趟。
	// 漏了这一行的表现很隐蔽：第二趟大招的命中顿帧被上一趟的 bHitStopDone 挡掉，
	// 也就是「第一次放大招手感对，后面每一次都不顿」。
	bHitStopDone = false;

	// 同一类问题：兜底瞄准线是上一趟的坐标。这一行漏掉的表现是「第二趟大招镜头甩向上一趟的消失点」
	//（只在锁不到目标时才会显现，平时看不出来）。
	bHasVanishLocation = false;

	// 海克斯眩晕的「本次已放过」标记，同样是实例复用必须清的那一批。
	// 漏这一行的表现：第一次 R 有眩晕，之后每次 R 都没有（而且完全看不出因果）。
	bAugmentStunApplied = false;

	// ---------------------------------------------------------------------
	// 客户端这一次：锁移动 + 对齐时间轴后播转圈蒙太奇。
	//
	// ServerInitiated 下服务器激活成功后会告诉本端，本端在这里本地跑一次 ActivateAbility。
	// 这一次只做两件事：
	//   - 不能 CommitAbility → 否则冷却 GE 在本端再挂一份，本端 CD 比服务端长一个 RTT；
	//   - 不能动坐标 → 传送是服务器权威的（和 GA_Flash 一致）；
	//   - 不能发 cue → 服务器 ExecuteGameplayCue 会多播过来，本端再发一次就是两份。
	// 技能什么时候结束也由服务器说了算（服务器 EndAbility 会发 ClientEndAbility）。
	// ---------------------------------------------------------------------
	if (!Avatar->HasAuthority())
	{
		// 本端能走到这里 = 服务端已经判定通过（ClientActivateAbilitySucceed 只在成功后才发），
		// 所以这时候关窗口是安全的。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 激活（%s）：本人那一侧开始对齐时间轴，施法者=%s 目标=%s"),
			NetModeText(GetWorld()), *GetNameSafe(Avatar), *GetNameSafe(Target));

		// 本端也记一份目标：P4 起手的甩镜要用（服务端已经验过它了，本端不用再验）。
		LockedTarget = Target;

		ClearManualSelectWindow();
		PlayClientTimeline();
		return;
	}

	// ---------------------------------------------------------------------
	// 下面全部是服务器
	// ---------------------------------------------------------------------

	// 【必须在 CommitAbility 之前】：目标不合格就是"这次没放出去"，不该进冷却。
	if (!ValidateTarget(Target))
	{
		// 不打冷却、不进相位，直接结束。bWasCancelled=true 和 GA_Flash 里 CommitAbility 失败那条一致。
		// 按键那位玩家的选择窗口还开着（本函数开头没摘到标签 —— 摘标签在下面成功之后），可以再点一次。
		UE_LOG(LogTemp, Log, TEXT("[DeathHarvest] 目标不合格（%s，射程 %.0f）→ 不激活"),
			Target ? *GetNameSafe(Target) : TEXT("空载荷"), AbilityData ? AbilityData->LockRange : 0.f);
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 目标合格、也扣了冷却/资源 —— 这次确实放出去了，可以关掉按键那位玩家的选择窗口了
	//（主机自己按 R 时那个标签就挂在这同一个 ASC 上；远端玩家那份服务端实例上没有，摘不到是 no-op）。
	ClearManualSelectWindow();

	LockedTarget = Target;

	// 挡重入的标签。放在【服务器本地】是够的：客户端再按 R 只是发一个 ServerTryActivateAbility 请求，
	// 判定在服务器做（ActivationBlockedTags 命中 → 拒绝），客户端本地有没有这个标签无所谓。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->AddLooseGameplayTag(LOLGameplayTags::State_DeathHarvest_Casting);
		bCastingTagAdded = true;
	}

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 锁定目标 %s（距离 %.0f）｜%s：施法者=%s"),
		*GetNameSafe(Target), FVector::Dist(Avatar->GetActorLocation(), Target->GetActorLocation()),
		NetModeText(GetWorld()), *GetNameSafe(Avatar));

	StartVanishPhase();
}

// =============================================================================
// P1 消失 → P2 开门 → P3 现身
//
// 这三段没有蒙太奇在播（能看见的动画只有 P4 转圈那一段），所以相位只能按时长推：
// 每条 WaitDelay 走完就进下一相位，每一步的第一件事都是重新校验目标还在不在。
// =============================================================================

void UGA_DeathHarvest::ClearManualSelectWindow()
{
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_DeathHarvest_Selecting);
	}
}

void UGA_DeathHarvest::PlayClientTimeline()
{
	// 客户端这一次要做的两件事：① 跟服务器一样锁住移动；② 等 P1~P3 走完（本端到"现身"了），
	// 同一帧起手：按慢放 + 播转圈蒙太奇。
	//
	// ① 锁移动：服务端那次 DisableMovement 不会同步到本端（MovementMode 只发给模拟代理），
	//    不锁的话本端会「一边隐形一边被玩家拖着走」，服务器每帧回拉一次 —— 表现是原地抽搐。
	//    服务器在 StartVanishPhase 里锁，两端锁的是同一个时间窗（都由 EndAbility 还原）。
	ApplyMovementLock(true);

	// 本端记下【消失点】：甩镜的瞄准线要用（见 TickCameraAlign）。
	// 本端这一份看不到服务器那次传送（自主代理不一定套用服务器的 SetActorLocation），所以本端坐标
	// 现身之后可能还停在这儿 —— 得靠"落点在图里怎么推"来定朝向，而不是靠本端坐标。
	if (const AActor* Avatar = GetAvatarActorFromActorInfo())
	{
		VanishLocation = Avatar->GetActorLocation();
		bHasVanishLocation = true;
	}

	if (!AbilityData)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 客户端：AbilityData 没配 → 本人这一侧看不到动画（移动锁由 EndAbility 还原）"));
		return;
	}

	// ② 时间轴对齐：P1~P3 全在服务器上按 AbilityData 的时长走，本端拿同一份时长自己推。
	//    起点差一个单程 RTT（服务器 CommitAbility → 本端 ClientActivateAbilitySucceed），
	//    也就是本人看到的转圈比服务器上真实那一圈晚几十毫秒 —— 这点偏差换来的是相位不用再走一遍网络。
	const float PhasesDuration = AbilityData->VanishToPortalDelay + AbilityData->PortalToAppearDelay;
	ClientDelayTask = UAbilityTask_WaitDelay::WaitDelay(this, PhasesDuration);
	ClientDelayTask->OnFinish.AddDynamic(this, &UGA_DeathHarvest::OnClientDelayFinished);
	ClientDelayTask->ReadyForActivation();
}

void UGA_DeathHarvest::OnClientDelayFinished()
{
	// 走到这里 = 本端时间轴上「现身」那一下到了 → 和服务器同一帧开转。
	// 本端这一份不再有"先站一会儿再转"：蒙太奇当场起播（正常速度），打击感由 BeginSpinLead
	// 排到 SpinImpactDelay 之后。
	//
	// ★ 顺序要紧：先关自动转向，再搬坐标，最后起手。甩镜的瞄准、现身 burst 的位置、
	//   蒙太奇脚下那一帧全都按"她已经站在落点上"来算。
	//
	// 为什么本端也要关（照服务器 P4 起手那一句抄一遍）：这两个开关是【每台机器各自的】
	//   组件/角色属性，不复制 —— 服务器关的是它那一份，本端这一份一直还开着。后果正好是
	//   「位置对、朝向不对」：本端移动组件收到服务器纠偏时会用【本端上一次移动里存下的朝向】
	//   把 actor 的朝向写回去（CharacterMovementComponent.cpp:11316 那段 fallback，
	//   判据就是 bOrientRotationToMovement —— 服务器那份被关成 false 所以走不到，本端是 true 所以走得进去），
	//   而那个朝向是【传送之前】存的：位置被纠到落点、朝向被拽回消失前。
	//   关掉之后这段 fallback 不再成立，本端 actor 的朝向就只剩 ApplyClientTeleport 写的那一次。
	ApplySpinRotationOverride(true);
	ApplyClientTeleport();
	BeginSpinLead();
	PlayClientMontage();
}

void UGA_DeathHarvest::ApplyClientTeleport()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	ACharacter* Character = Cast<ACharacter>(Avatar);
	AActor* Target = LockedTarget.Get();
	if (!Character || !Target)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[DeathHarvest] 客户端本端传送跳过：施法者=%s（是 ACharacter=%d）目标=%s → 本端她会停在原处"),
			*GetNameSafe(Avatar), Cast<ACharacter>(Avatar) ? 1 : 0, *GetNameSafe(Target));
		return;
	}

	// 落点和服务器 P2 用的是同一个函数、同一份输入（只吃目标和场景，不吃"自己"），所以两边结果一致。
	// ⚠️ 唯一的偏差源是目标那台机器上还在插值的坐标/朝向：正常情况（身后站得下 = 候选表第一项）
	//    两边必然相同，只有"身后被挡、退到别的候选位"而且刚好卡在判定边缘时才会差一个候选位，
	//    那点误差由 EndAbility 之后移动组件自己的纠偏抹掉（表现是轻轻一顿）。
	if (!ComputeDestination(Target, PortalLocation))
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 客户端本端传送跳过：落点全废（ComputeDestination=false）｜施法者=%s"),
			*GetNameSafe(Avatar));
		return;
	}

	// 和服务器 P3 那三行一模一样（补半个胶囊高 + 只取 Yaw），差别只在"谁来执行"。
	// 少了半个胶囊高就是半个身子在地下；带上俯仰胶囊的 up 轴会歪、下一帧被移动组件抹平 → 落地抖一下。
	const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.f;
	Character->SetActorLocation(PortalLocation + FVector(0.f, 0.f, HalfHeight), /*bSweep=*/false, nullptr, ETeleportType::None);
	Character->SetActorRotation(FRotator(0.f, (Target->GetActorLocation() - PortalLocation).Rotation().Yaw, 0.f));

	// 坐标和朝向都打：和服务器那条「P3 现身」逐项对，坐标该重合、朝向该同值。
	// 朝向这项要是下一帧被拽走了，就是自动转向没关干净（两端都要关，见 OnClientDelayFinished）。
	UE_LOG(LogTemp, Warning,
		TEXT("[DeathHarvest] 客户端本端传送 @ (%.0f, %.0f, %.0f) 朝向 %.1f（补半高 %.0f；对一眼服务器那条「P3 现身」的坐标，两边应该重合）｜施法者=%s"),
		Character->GetActorLocation().X, Character->GetActorLocation().Y, Character->GetActorLocation().Z,
		Character->GetActorRotation().Yaw, HalfHeight, *GetNameSafe(Avatar));
}

void UGA_DeathHarvest::PlayClientMontage()
{
	if (!AbilityData || !AbilityData->Montage)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 客户端：AbilityData/Montage 没配 → 本人这一侧看不到动画"));
		return;
	}

	// 本端自己播一条蒙太奇。bStopWhenAbilityEnds=true：服务器 EndAbility 后本端这条会被停掉。
	UAbilityTask_PlayMontageAndWait* Task = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, NAME_None, AbilityData->Montage, 1.f, AbilityData->MontageStartSection, /*bStopWhenAbilityEnds=*/true);

	// 刻意【不】绑 OnCompleted/OnInterrupted：客户端蒙太奇播完不代表技能结束（时间轴在服务器），
	// 在这儿 EndAbility 会变成客户端抢先结束技能。收尾只认服务器的 ClientEndAbility。
	Task->ReadyForActivation();
}

void UGA_DeathHarvest::StartVanishPhase()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !AbilityData)
	{
		CancelAsFailed();
		return;
	}

	// 移动从 P1 就锁住：人在消失状态里不该还能自己走（v1 只锁转圈那一段）。
	ApplyMovementLock(true);

	// 记下【消失点】：甩镜的瞄准线要用（和客户端 PlayClientTimeline 里那一份对应，见 TickCameraAlign）。
	VanishLocation = Avatar->GetActorLocation();
	bHasVanishLocation = true;

	// 「消失中」是一条【状态】cue：挂上 = 各端把人藏起来 + 关碰撞，摘掉 = 恢复 + 现身 burst。
	// 它是全场唯一让人真正消失的地方，所以交给 bRemoveOnAbilityEnd 兜底 —— 技能被打断也不会留下隐形人。
	// （起手那一下的特效也在这条 cue 里，见 AGC_DeathHarvestCast。）
	AddLocationCue(LOLGameplayTags::GameplayCue_DeathHarvest_Cast, Avatar->GetActorLocation());

	VanishDelayTask = UAbilityTask_WaitDelay::WaitDelay(this, AbilityData->VanishToPortalDelay);
	VanishDelayTask->OnFinish.AddDynamic(this, &UGA_DeathHarvest::OnVanishFinished);
	VanishDelayTask->ReadyForActivation();

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] P1 消失 @ (%.0f, %.0f, %.0f)，%.2fs 后开门｜%s：施法者=%s"),
		Avatar->GetActorLocation().X, Avatar->GetActorLocation().Y, Avatar->GetActorLocation().Z,
		AbilityData->VanishToPortalDelay, NetModeText(GetWorld()), *GetNameSafe(Avatar));
}

void UGA_DeathHarvest::OnVanishFinished()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	AActor* Target = LockedTarget.Get();

	// ★ 这行是「P1 计时到底有没有走完」的唯一证据：它在所有分支之前。
	//   两秒后日志里没有它 = 任务压根没广播（技能被结束了 / 任务被杀了），有它 = 相位逻辑本身有问题。
	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] P1 计时到，进 P2 判定｜%s：施法者=%s 目标=%s 权威=%d"),
		NetModeText(GetWorld()), *GetNameSafe(Avatar), *GetNameSafe(Target),
		(Avatar && Avatar->HasAuthority()) ? 1 : 0);

	// 每个相位边界都重校验一次：目标可能在消失这段时间里死了/被销毁了。
	// 三个条件分开报：合在一句里的话，「人没了」和「目标没了」看起来一模一样。
	if (!Avatar)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 开门时施法者已失效 → 取消"));
		CancelAsFailed();
		return;
	}
	if (!Avatar->HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 开门时这一端不是权威（相位本该只在服务器上跑）→ 取消"));
		CancelAsFailed();
		return;
	}
	if (!Target)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 开门时目标已失效 → 取消"));
		CancelAsFailed();
		return;
	}

	FVector Dest;
	if (!ComputeDestination(Target, Dest))
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 落点全被挡（目标 %s）→ 取消并退冷却"), *GetNameSafe(Target));
		CancelAsFailed();
		return;
	}

	PortalLocation = Dest;

	// 传送门开在落点上，朝向目标（法线当她"从门里看出去"的方向）。
	// ⚠️ 坐标必须这样带过去：Actor 版 cue 的生成位置取的是 TargetActor 的坐标
	// （GameplayCueManager.cpp:528 用的是 TargetActor->GetActorLocation()），不是 Parameters.Location，
	// 所以 cue 的 OnActive 里还得自己 SetActorLocation 挪一次 —— 见 GC_DeathHarvestPortal。
	const FVector ToTarget = (Target->GetActorLocation() - Dest).GetSafeNormal();
	AddLocationCue(LOLGameplayTags::GameplayCue_DeathHarvest_Portal, Dest, ToTarget);

	PortalDelayTask = UAbilityTask_WaitDelay::WaitDelay(this, AbilityData->PortalToAppearDelay);
	PortalDelayTask->OnFinish.AddDynamic(this, &UGA_DeathHarvest::OnPortalFinished);
	PortalDelayTask->ReadyForActivation();

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] P2 开门 @ (%.0f, %.0f, %.0f)，目标 %s，%.2fs 后现身｜%s"),
		Dest.X, Dest.Y, Dest.Z, *GetNameSafe(Target), AbilityData->PortalToAppearDelay, NetModeText(GetWorld()));
}

void UGA_DeathHarvest::OnPortalFinished()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	AActor* Target = LockedTarget.Get();

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] P2 计时到，进 P3 判定｜%s：施法者=%s 目标=%s 权威=%d"),
		NetModeText(GetWorld()), *GetNameSafe(Avatar), *GetNameSafe(Target),
		(Avatar && Avatar->HasAuthority()) ? 1 : 0);

	if (!Avatar || !Avatar->HasAuthority() || !Target)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 现身时前置条件不满足（施法者=%s 目标=%s）→ 取消"),
			*GetNameSafe(Avatar), *GetNameSafe(Target));
		CancelAsFailed();
		return;
	}

	ACharacter* Character = Cast<ACharacter>(Avatar);
	if (!Character)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 现身时施法者不是 ACharacter（%s）→ 取消"), *GetNameSafe(Avatar));
		CancelAsFailed();
		return;
	}

	// 只有服务器动坐标，和 GA_Flash.cpp:53-56 同一套写法 —— 唯一不成立的地方是 Z：
	//   ★ GA_Flash 的落点是沿【水平】方向从角色当前位置扫出来的，Z 本来就是胶囊中心，直接写就对；
	//   这里的落点是 ComputeDestination 用地面垂直射线打出来的【地面表面点】（Grounded），
	//   而 SetActorLocation 写的是 actor 原点 = 胶囊中心。少了这半个胶囊，人就有一半在地下 ——
	//   现身之后看到的"下地"就是这个，和蒙太奇、和 root motion 都没有关系
	//   （IsSpotFree 里也是先在落点上加 HalfHeight 才摆检测胶囊的，同一个道理）。
	const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.f;
	Character->SetActorLocation(PortalLocation + FVector(0.f, 0.f, HalfHeight), /*bSweep=*/false, nullptr, ETeleportType::None);

	// ★ 只取 Yaw：带上俯仰的话胶囊的 up 轴会歪，移动组件下一帧把它抹平 —— 表现是"落地瞬间抖一下"。
	//   这个朝向就是全程的朝向（转圈是动画在转、actor 不转），所以收招也不用回正。
	const FVector ToTarget = Target->GetActorLocation() - PortalLocation;
	Character->SetActorRotation(FRotator(0.f, ToTarget.Rotation().Yaw, 0.f));

	// 摘掉「消失中」和传送门两条状态 cue：各端在这一刻恢复可见/碰撞、放现身 burst、摘掉主人的传送镜头。
	// 必须在传送【之后】摘 —— 现身 burst 放的是角色当前位置，也就是刚落到的落点。
	// K2_RemoveGameplayCue 会把这个 tag 从 TrackedGameplayCues 里删掉，所以 EndAbility 不会重复摘。
	K2_RemoveGameplayCue(LOLGameplayTags::GameplayCue_DeathHarvest_Cast);
	K2_RemoveGameplayCue(LOLGameplayTags::GameplayCue_DeathHarvest_Portal);

	// 打的是【人真正站住的位置】（= 落点地面 + 半个胶囊），不是落点本身：
	// actor Z 和地面 Z 差一个半高是正常的，差值不等于半高才说明哪里又漏了一步。
	// 朝向也一并打：客户端那一份是自己照抄这行算的（ApplyClientTeleport），两条的 yaw 该一样。
	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] P3 现身 @ (%.0f, %.0f, %.0f) 朝向 %.1f（地面 Z=%.0f，补半高 %.0f）"),
		Character->GetActorLocation().X, Character->GetActorLocation().Y, Character->GetActorLocation().Z,
		Character->GetActorRotation().Yaw, PortalLocation.Z, HalfHeight);

	// ---------------------------------------------------------------------
	// P4 起手：现身这一帧就开转 —— 播蒙太奇 + 挂转圈 cue + 开始甩镜，没有中间那段空等。
	//
	// 打击感（慢放 + 第一跳伤害 + 顿帧 + 震动）【不】在这一帧：蒙太奇只有一段 Spin，
	// 开头是抬刀/转上身，刀真正扫出去在 SpinImpactDelay 之后 —— 顿和震打在第 0 帧上的话，
	// 玩家看到的是"现身之后卡了一下才开始转"。所以这一段交给 BeginSpinLead 排的
	// OnSpinImpactDelayFinished，由它在刀扫起来那一帧同时按下慢放和第一跳伤害。
	// ---------------------------------------------------------------------
	BeginSpinLead();
	StartSpinVisuals();

	// 起手时把三个数摆在一起打一条：转圈总时长对不上蒙太奇长度的话，最后几跳会被收招掐掉，
	// 而现象只是"转得少了两下"，不看这条日志基本查不出来。
	if (AbilityData->Montage)
	{
		const float SpinWindow = AbilityData->SpinImpactDelay + AbilityData->SpinInterval * AbilityData->SpinCount;
		const float MontageLength = AbilityData->Montage->GetPlayLength();
		UE_LOG(LogTemp, Warning,
			TEXT("[DeathHarvest] 转圈时长账：推后 %.2fs + %d 跳 × %.2fs = %.2fs，蒙太奇 %.2fs（%s）"),
			AbilityData->SpinImpactDelay, AbilityData->SpinCount, AbilityData->SpinInterval, SpinWindow, MontageLength,
			SpinWindow > MontageLength ? TEXT("★ 超了：最后几跳会被蒙太奇结束掐掉，调小推后/间隔或加大蒙太奇") : TEXT("够用"));
	}
}

void UGA_DeathHarvest::OnMontageFinished()
{
	// 客户端那份蒙太奇结束不代表技能结束（时间轴在服务器）——收尾只认服务器发来的 ClientEndAbility。
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->HasAuthority())
	{
		return;
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
		/*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}

// =============================================================================
// P0 目标校验
//
// v2 这里是一段「沿 control rotation 做球形扫描 + 夹角打分」的自动选目标（SelectTarget）：
// 按下 R 的瞬间在服务端挑一个最贴近朝向的人。v3 改成了「按住 R、左键点中谁就是谁」之后，
// 这段扫描没有存在意义了 —— 服务端不再代选，只校验客户端点出来的那个。
//
// 为什么方向不再校验：客户端是用【相机】准星打射线选的人，而服务端手里只有角色位置和
// control rotation（还会因为移动 RPC 的到达时间差几十毫秒），拿它去卡夹角会把「镜头已经转过去、
// 服务器还停在上一帧」的正经点击判掉。方向这一层由「客户端必须用相机准星打射线」保证 ——
// 想作弊的客户端也只能点一个真实存在、活着、在射程内、无遮挡的人，作弊空间只剩距离和遮挡以外的东西。
// =============================================================================

bool UGA_DeathHarvest::ValidateTarget(AActor* Target) const
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Target || Target == Avatar || !AbilityData || !GetWorld())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 目标校验不过：载荷无效（Avatar=%s Target=%s Data=%s）"),
			*GetNameSafe(Avatar), *GetNameSafe(Target), AbilityData ? TEXT("有") : TEXT("空"));
		return false;
	}

	// ① 有 ASC 且还活着。没有 ASC 的东西（木桩/场景）直接出局 —— 这个技能只对人放。
	//    （能走到这里的载荷已经是客户端射线打中的 actor，这里挡的是「客户端说谎」和「路上死了」两种情况。）
	//    ⚠️ 解析器必须和客户端选目标时用的是同一个（UMyAbilitySystemComponent::FindAbilitySystemComponent）：
	//    客户端用蓝图库挑、这里用别的挑，就会出现「点中了却被服务端判成没有 ASC」。
	UAbilitySystemComponent* TargetASC = UMyAbilitySystemComponent::FindAbilitySystemComponent(Target);
	if (!TargetASC)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 目标校验不过：%s 上没有 ASC（接口/PlayerState/组件都没找到）"),
			*GetNameSafe(Target));
		return false;
	}
	// 活着 = 取得到 Health 且 > 0。取不到（bFound=false，属性集还没就绪）时当活着，
	// 别在属性集刚初始化那一瞬间把所有人都判死。
	bool bFound = false;
	const float Health = TargetASC->GetGameplayAttributeValue(UHeroCombatAttributeSet::GetHealthAttribute(), bFound);
	if (bFound && Health <= 0.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 目标校验不过：%s 已经死了（Health=%.1f）"), *GetNameSafe(Target), Health);
		return false;
	}

	// ② 射程。客户端那条射线给得很长（只决定「能找到多远的候选」），真正卡距离的只有这里。
	const FVector Start = Avatar->GetActorLocation();
	const float Distance = FVector::Dist(Start, Target->GetActorLocation());
	if (Distance > AbilityData->LockRange)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 目标校验不过：%s 距离 %.0f 超出射程 %.0f"),
			*GetNameSafe(Target), Distance, AbilityData->LockRange);
		return false;
	}

	// ③ 视线无遮挡：从胸口到胸口一条射线，挡住的不算（客户端是从相机打的，这里只是权威复核）。
	const FVector EyeLoc = Start + FVector(0.f, 0.f, 60.f);
	const FVector TargetLoc = Target->GetActorLocation() + FVector(0.f, 0.f, 60.f);
	FCollisionQueryParams LosParams(SCENE_QUERY_STAT(DeathHarvestLockLos), false, Avatar);
	LosParams.AddIgnoredActor(Target);
	FHitResult LosHit;
	// 这里继续用 ECC_Visibility 是对的：角色碰撞对 Visibility 是 Ignore（BaseEngine.ini），
	// 所以这条射线【只会】被墙/地面挡住，不会被人挡 —— 正好是「视线通不通」要的语义。
	if (GetWorld()->LineTraceSingleByChannel(LosHit, EyeLoc, TargetLoc, ECC_Visibility, LosParams))
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 目标校验不过：%s 被 %s 挡住视线"),
			*GetNameSafe(Target), *GetNameSafe(LosHit.GetActor()));
		return false;
	}

	return true;
}

// =============================================================================
// P2 落点（目标身后）
// =============================================================================

bool UGA_DeathHarvest::ComputeDestination(AActor* Target, FVector& OutDestination) const
{
	const AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Target || !AbilityData || !GetWorld())
	{
		return false;
	}

	const FVector TargetLoc = Target->GetActorLocation();
	const FVector TargetForward = Target->GetActorForwardVector();
	const FVector TargetRight = Target->GetActorRightVector();

	// 候选表：身后 → 左后 → 右后 → 正前 → 目标正上方。全废才返回 false。
	const float Behind = AbilityData->BehindDistance;
	const TArray<FVector> Candidates =
	{
		TargetLoc - TargetForward * Behind,
		TargetLoc - TargetForward * Behind + TargetRight * Behind,
		TargetLoc - TargetForward * Behind - TargetRight * Behind,
		TargetLoc + TargetForward * Behind,
		TargetLoc + FVector(0.f, 0.f, Behind * 2.f),
	};

	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		const FVector& Candidate = Candidates[Index];
		FVector Grounded = Candidate;

		// 贴地：从候选点上方往下打一条，打中地面就落在地面上 —— 别用目标自身的 Z
		// （目标站在斜坡/台阶上时，那个 Z 会让落点悬空或陷进地里）。
		const FVector TraceStart = Candidate + FVector(0.f, 0.f, AbilityData->GroundTraceUp);
		const FVector TraceEnd = Candidate - FVector(0.f, 0.f, AbilityData->GroundTraceDown);
		FCollisionQueryParams GroundParams(SCENE_QUERY_STAT(DeathHarvestGround), false, Avatar);
		GroundParams.AddIgnoredActor(Target);
		FHitResult GroundHit;
		const bool bGrounded = GetWorld()->LineTraceSingleByChannel(GroundHit, TraceStart, TraceEnd, ECC_Visibility, GroundParams);
		if (bGrounded)
		{
			Grounded = GroundHit.Location;
		}

		// 站得下吗（胶囊扫描，照 GA_Flash::TryFindBlinkDestination）。
		const bool bFree = IsSpotFree(Grounded, Avatar);

		// ★ 每个候选点都留一行：全废时靠这五行判断是"点太挤"还是"贴地贴到了奇怪的地方"。
		//   BehindDistance 配成 0 的话，五个候选会塌到同一个点上（就是目标自己站的位置），
		//   而 IsSpotFree 只忽略施法者、不忽略目标 → 目标自己的胶囊把五个点全挡掉。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 落点候选[%d/%d] @ (%.0f, %.0f, %.0f) 贴地=%d 站得下=%d"),
			Index + 1, Candidates.Num(), Grounded.X, Grounded.Y, Grounded.Z, bGrounded ? 1 : 0, bFree ? 1 : 0);

		if (bFree)
		{
			OutDestination = Grounded;
			return true;
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 五个落点候选全废（BehindDistance=%.0f，目标=%s）"),
		Behind, *GetNameSafe(Target));
	return false;
}

bool UGA_DeathHarvest::IsSpotFree(const FVector& Location, const AActor* IgnoredActor) const
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character || !GetWorld())
	{
		return false;
	}

	const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 34.f;
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.f;

	// 胶囊扫掠：把一个胶囊"放"在这个位置，看它跟世界有没有重叠。
	// 用 sweep（起点=终点）而不是 overlap：和 GA_Flash 走同一条通道（ECC_Pawn），行为一致。
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeathHarvestSpotCheck), false, IgnoredActor);
	Params.AddIgnoredActor(IgnoredActor);

	// ★ 判之前先把胶囊抬 SpotClearance —— 这一段唯一不显然的地方。
	//   Location 不是"人站着的高度"，是 ComputeDestination 从地面射线打出来的【地面表面点】。
	//   把胶囊底【正好】摆在那个 Z 上，它和地面之间就是零深度接触；而零深度接触在场景查询里
	//   照样算一次重叠。UE 自己就撞过这件事，UWorld::ComponentEncroachesBlockingGeometry_WithAdjustment
	//   里留着一段 #hack："physx returns a 0 MTD even though it reports a contact (returns true)"
	//   —— MTD 为 0 时换个缩小 epsilon 的形状重测，测不到就当没重叠。
	//   不抬的后果：平地上每一个候选点都被地面（Floor_*）自己判成"挡路"，五个候选全废、技能必被取消。
	//   抬起来只是不再把"擦着"当"压着"：真嵌进墙里/被人占住（深度是厘米级）依然是被挡。
	//   注意这 2cm 只用在【检测】上，返回给 P3 的落点还是原来那个地面点 —— 人不会悬空 2cm。
	const float SpotClearance = 2.f;
	const FVector Center = Location + FVector(0.f, 0.f, HalfHeight + SpotClearance);
	const FCollisionShape Probe = FCollisionShape::MakeCapsule(Radius, HalfHeight);

	FHitResult Hit;
	const bool bBlocked = GetWorld()->SweepSingleByChannel(
		Hit, Center, Center, FQuat::Identity, ECC_Pawn, Probe, Params);

	if (bBlocked)
	{
		// 被谁挡住 —— 全废时这一行直接指出「是不是目标自己站在候选点上」。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 落点被挡 @ (%.0f, %.0f, %.0f)：挡路的是 %s（胶囊 r=%.0f h=%.0f）"),
			Location.X, Location.Y, Location.Z, *GetNameSafe(Hit.GetActor()), Radius, HalfHeight);
	}

	return !bBlocked;
}

// =============================================================================
// P4 起手（现身那一帧）
//
// 两件事：① 把【她自己】的时间调慢 SpinSlowTime 秒（起手那几帧）；② 把镜头甩到
// 「自己 → 敌人」这条线上。两件都是本机表现，两端各做各的。
//
// ★ 慢放用的是 AActor::CustomTimeDilation，不是 WorldSettings::TimeDilation。
//   组件 tick 的 delta 会乘上 owner 的 CustomTimeDilation（Actor.h:4890
//   FActorComponentTickFunction::ExecuteTick：`DeltaTime * (MyOwner ? MyOwner->CustomTimeDilation : 1.f)`），
//   而骨骼网格的动画正是走组件 tick 推进的 —— 所以只有她的动画慢下来，别的玩家、粒子、
//   世界计时器、SpinTask 的伤害节奏全都不受影响。于是：
//     · 这里填的是【真实秒】，不需要 ScaledWorldSeconds 那一层折算；
//     · 漏还原的后果只是"她一直慢动作"，不是"整局卡在四分之一速"。
//   对照 v3 那版用 SetGlobalTimeDilation 的写法：整个世界（含所有人的节奏）一起慢，
//   WaitDelay 时长必须按流速折算，而且要在 EndAbility 里提心吊胆地兜底还原。
//
// ⚠️ CustomTimeDilation 【不复制】（Actor.h:797 只有 BlueprintReadWrite，没有 Replicated），
//   所以每台跑这个技能实例的机器各给自己那份角色设一次。跑这个实例的只有两台：
//   服务器（权威）和施法者本端（ClientActivateAbilitySucceed 后本地那次）—— 两台都设，
//   所以两边同步；只有【旁观者】看不到这段慢放，差 ≈ SpinSlowTime × (1 - SpinSlowScale)（默认约 0.13s）。
//   想让旁观者也看到就得再加一条多播 RPC，性价比不高。
//
// ⚠️ 服务器那一份【也】必须设，不能因为"服务器不上屏"就跳过：服务器的蒙太奇不慢的话，
//   它会比施法者本机早一截 broadcast OnMontageFinished → EndAbility，bStopWhenAbilityEnds
//   会把人家还差一截的动画直接掐掉。
// =============================================================================

void UGA_DeathHarvest::BeginSpinLead()
{
	BeginCameraAlign();
	ArmSpinImpact();
}

void UGA_DeathHarvest::EndSpinLead()
{
	EndSpinSlowMotion();
	EndCameraAlign();
}

// -----------------------------------------------------------------------------
// 起手 → 刀扫起来那一帧之间的那段（SpinImpactDelay）
//
// 这一段里蒙太奇是【正常速度】播的（抬刀、转上身），所以不会看到"站在原地等"。
// 走完落到 OnSpinImpact：慢她自己 + （服务器）起伤害跳。因为 Repeat 任务在 Activate 时
// 【立刻】打第一跳（AbilityTask_Repeat.cpp:35 先 PerformAction 再 SetTimer），
// 所以"开始伤害"= "第一跳已经打出去了"，顿帧和震动（挂在第一跳的命中 cue 上）也就在同一帧。
// -----------------------------------------------------------------------------

void UGA_DeathHarvest::ArmSpinImpact()
{
	const float Delay = AbilityData ? AbilityData->SpinImpactDelay : 0.f;
	if (Delay <= 0.f)
	{
		// 不推后：当场就是打击感那一帧（= v3.1 之前的旧行为，方便 A/B）。
		OnSpinImpact();
		return;
	}

	SpinImpactTask = UAbilityTask_WaitDelay::WaitDelay(this, Delay);
	SpinImpactTask->OnFinish.AddDynamic(this, &UGA_DeathHarvest::OnSpinImpactDelayFinished);
	SpinImpactTask->ReadyForActivation();
}

void UGA_DeathHarvest::OnSpinImpactDelayFinished()
{
	OnSpinImpact();
}

void UGA_DeathHarvest::OnSpinImpact()
{
	// 慢放：两端各按各的（CustomTimeDilation 不复制，见上面那段说明）。
	BeginSpinSlowMotion();

	// 伤害跳只有服务器那一份实例该起（OnSpinPulse 走的是服务器结算）。
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (Avatar && Avatar->HasAuthority())
	{
		StartSpinPulses();
	}
}

void UGA_DeathHarvest::BeginSpinSlowMotion()
{
	if (bSpinSlowMotionActive || !AbilityData || !AbilityData->bSlowMotionEnabled || AbilityData->SpinSlowTime <= 0.f)
	{
		// 已经按下去过（重复调用）/ 没配数值 / 配置里关了慢放 —— 三种都什么都不做。
		return;
	}

	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character)
	{
		return;
	}

	// 存【原始成员】而不是无条件写回 1：别的系统以后也可能给她上速率，只还原自己按的那一次。
	SavedCustomTimeDilation = Character->CustomTimeDilation;
	Character->CustomTimeDilation = FMath::Clamp(AbilityData->SpinSlowScale, 0.01f, 1.f);
	bSpinSlowMotionActive = true;

	// ⚠️ 这个 WaitDelay 等的是【世界秒】，而世界计时器不受 CustomTimeDilation 影响 ——
	//    所以填真实秒就是对的。唯一的偏差：这段时间里如果正好撞上命中顿帧（世界被冻住），
	//    世界秒走得比真实秒慢，窗口会被拉长一点（默认配置下 0.15 会变成约 0.22 真实秒）——
	//    起手和第一跳本来就是同一帧，所以这是常态，不是偶发。
	SpinSlowTask = UAbilityTask_WaitDelay::WaitDelay(this, AbilityData->SpinSlowTime);
	SpinSlowTask->OnFinish.AddDynamic(this, &UGA_DeathHarvest::OnSpinSlowMotionFinished);
	SpinSlowTask->ReadyForActivation();

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] P4 起手慢放 %.2fs：角色速率 %.2f → %.2f｜%s：施法者=%s"),
		AbilityData->SpinSlowTime, SavedCustomTimeDilation, Character->CustomTimeDilation,
		NetModeText(GetWorld()), *GetNameSafe(Character));
}

void UGA_DeathHarvest::OnSpinSlowMotionFinished()
{
	EndSpinSlowMotion();
}

void UGA_DeathHarvest::EndSpinSlowMotion()
{
	// 幂等：EndAbility 里还会兜底再调一次（技能被打断时唯一走得到的地方就是那儿）。
	if (!bSpinSlowMotionActive)
	{
		return;
	}
	bSpinSlowMotionActive = false;

	if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
	{
		Character->CustomTimeDilation = SavedCustomTimeDilation;
	}
}

void UGA_DeathHarvest::BeginCameraAlign()
{
	// 甩镜是纯本机表现：服务器上跑远端玩家那一份时不碰（服务器的 control rotation 是被
	// 那位客户端的 ServerMove 驱动的，这边写了下一帧就被覆盖，纯属白写）。
	//
	// 这台机器到底走没走到甩镜，只有日志说得清（甩镜在客户端上"没生效"有好几种同样安静的原因：
	// 不是本机 / 数值没配 / 目标没拿到）。所以四条早退路径各打一条自己的日志，一次跑就能定位。
	if (!IsLocallyControlledAvatar())
	{
		UE_LOG(LogTemp, Log, TEXT("[DeathHarvest] 甩镜跳过：这台机器上施法者不是本机控制（%s）｜施法者=%s"),
			NetModeText(GetWorld()), *GetNameSafe(GetAvatarActorFromActorInfo()));
		return;
	}
	if (!AbilityData || AbilityData->CameraAlignTime <= 0.f || AbilityData->CameraAlignSpeed <= 0.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 甩镜跳过：数据没配（AbilityData=%d 时长=%.2f 速度=%.1f）｜施法者=%s"),
			AbilityData ? 1 : 0, AbilityData ? AbilityData->CameraAlignTime : -1.f,
			AbilityData ? AbilityData->CameraAlignSpeed : -1.f, *GetNameSafe(GetAvatarActorFromActorInfo()));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 结束时刻记在【真实】时间轴上：不管是她自己的起手慢放还是命中顿帧的世界慢放，
	// 都不会让这段甩镜变长或变短。
	CameraAlignEndRealTime = World->GetRealTimeSeconds() + AbilityData->CameraAlignTime;
	bCameraAligning = true;
	bCameraAlignLoggedFirstTick = false;
	CameraAlignTicks = 0;
	ArmCameraAlignTick();

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 甩镜开始：%.2fs 内甩到目标上（速度 %.1f）｜%s 施法者=%s 目标=%s"),
		AbilityData->CameraAlignTime, AbilityData->CameraAlignSpeed, NetModeText(World),
		*GetNameSafe(GetAvatarActorFromActorInfo()), *GetNameSafe(LockedTarget.Get()));
}

void UGA_DeathHarvest::EndCameraAlign()
{
	if (!bCameraAligning)
	{
		return;
	}
	bCameraAligning = false;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CameraAlignTimer);

		// 正常收尾是"甩够了"（下面 TickCameraAlign 里自己关的，那时 bCameraAligning 已经是 false，
		// 根本走不到这儿）。走到这儿说明是被【外力】掐掉的：技能提前结束/被打断/掉控制。
		// 客户端甩镜"没反应"如果伴随着这条日志，那就是技能在本端提前结束了，跟镜头本身无关。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 甩镜被掐断：还剩 %.2fs 没甩（技能提前收招/被打断/掉了控制）｜施法者=%s"),
			CameraAlignEndRealTime - World->GetRealTimeSeconds(), *GetNameSafe(GetAvatarActorFromActorInfo()));
	}
}

// -----------------------------------------------------------------------------
// 命中顿帧
//
// 和起手慢放刚好相反：这个【必须】是世界的。只慢她自己的话敌人照常动，看起来不像"打中了"
// 而像"她卡了"。服务器按下去 → WorldSettings::TimeDilation 复制到全场 → 每一端顿的是同一帧，
// 连被打的人屏幕也一起顿（那正是顿帧该有的分量）。所以这个函数只在服务器调
//（唯一调用方 OnSpinPulse_Server 本身就跑在服务器上）。
// -----------------------------------------------------------------------------

void UGA_DeathHarvest::BeginHitStop()
{
	if (!AbilityData || !AbilityData->bSlowMotionEnabled || AbilityData->HitStopTime <= 0.f)
	{
		return;
	}
	if (bHitStopApplied)
	{
		// 上一次还没还原（HitStopTime 被配得比伤害间隔还长时会发生）→ 不叠第二层，
		// 叠了的话两个还原任务会互相把对方的流速写回去，最后停在哪个值上全看执行顺序。
		return;
	}
	if (bHitStopDone && !AbilityData->bHitStopEveryPulse)
	{
		// 默认只在第一次真的打到人那一跳顿一下。
		return;
	}

	UWorld* World = GetWorld();
	AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
	if (!Settings)
	{
		return;
	}

	// 存【原始成员】而不是 GetEffectiveTimeDilation()：后者把 CinematicTimeDilation 和
	// DemoPlayTimeDilation 也乘了进去（WorldSettings.h:860），拿它回写 SetTimeDilation
	// 等于把电影/回放那一路的缩放永久烙进世界时间，还原不回去。
	SavedTimeDilation = Settings->TimeDilation;
	bHitStopApplied = true;
	bHitStopDone = true;

	// SetTimeDilation 会按 Min/MaxGlobalTimeDilation 夹一下（WorldSettings.cpp:345）：填超区间的值
	// 不报错，只是生效的不是填的那个 —— 所以下面把【实际生效的值】打出来，别只看配置。
	UGameplayStatics::SetGlobalTimeDilation(this, AbilityData->HitStopScale);

	// ★ 时长必须在 SetGlobalTimeDilation【之后】折算：世界计时器跟的是膨胀后的 delta
	//   （LevelTick.cpp:1596 乘、:1816 喂给 TimerManager），冻帧期间"0.07 世界秒"要跑 1.4 真实秒。
	//   传真实秒进去的话顿帧会一直持续到技能被打断。
	HitStopTask = UAbilityTask_WaitDelay::WaitDelay(this, ScaledWorldSeconds(AbilityData->HitStopTime));
	HitStopTask->OnFinish.AddDynamic(this, &UGA_DeathHarvest::OnHitStopFinished);
	HitStopTask->ReadyForActivation();

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 命中顿帧 %.3fs（真实）：世界时间 %.2f → %.2f｜%s"),
		AbilityData->HitStopTime, SavedTimeDilation, Settings->GetEffectiveTimeDilation(),
		NetModeText(World));
}

void UGA_DeathHarvest::OnHitStopFinished()
{
	EndHitStop();
}

void UGA_DeathHarvest::EndHitStop()
{
	// 幂等：EndAbility 里还会兜底再调一次。漏了这一句的报错不是"技能坏了"，
	// 而是"整个世界卡在顿帧的流速上"。
	if (!bHitStopApplied)
	{
		return;
	}
	bHitStopApplied = false;
	UGameplayStatics::SetGlobalTimeDilation(this, SavedTimeDilation);

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 命中顿帧结束：世界时间还原成 %.2f｜%s"),
		SavedTimeDilation, NetModeText(GetWorld()));
}

float UGA_DeathHarvest::ScaledWorldSeconds(float RealSeconds) const
{
	const UWorld* World = GetWorld();
	const AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
	const float Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.f;

	// 世界计时器（UAbilityTask_WaitDelay 用的那个）读的是【膨胀后】的 delta：
	// LevelTick.cpp:1596 `DeltaSeconds *= Info->GetEffectiveTimeDilation()`，1816 再把它喂给 TimerManager。
	// 所以「等 RealSeconds 那么久的真实时间」= 等 RealSeconds × 当前流速 那么多的世界秒。
	// 没有慢放时 Dilation 就是 1，这个函数退化成恒等。
	// 只用在命中顿帧上：起手慢放动的是 CustomTimeDilation，世界计时器根本不看它。
	return RealSeconds * Dilation;
}

bool UGA_DeathHarvest::IsLocallyControlledAvatar() const
{
	// 问的是「这台机器上是不是【我】在放这个技能」——甩镜是纯本机表现，Bot 放不需要甩。
	// 三种情况一次分清：
	//   单机 / 主机自己放 → true；专用服务器上跑的远端玩家 → false；客户端本端 → true。
	//
	// ★ 原来用的是 Pawn->IsLocallyControlled()，它转发给 AController::IsLocalController()，
	//   而那个函数在 NM_Standalone 下【无条件返回 true】（Controller.cpp:94-98）——
	//   于是单机打 AI 时 Bot 放死亡收割也会判成「本机在放」，跟着去甩镜，
	//   和 AArenaBotController::Tick 里的 SetFocus → UpdateControlRotation 抢同一个控制旋转。
	//   （上面那句"单机 → true（那个 PlayerController 就是本地玩家）"就是错在这一步：
	//     Bot 的 Controller 是 AAIController，从来就不是 PlayerController。）
	//   详见 GAS/LocalPlayerUtils.h。
	return LOLLocalPlayer::IsLocalPlayerControlled(GetAvatarActorFromActorInfo());
}

void UGA_DeathHarvest::ArmCameraAlignTick()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 逐帧驱动用 SetTimerForNextTick 自递归，没有用 SetTimer(..., Rate=0, bLoop=true)：
	// FTimerManager::InternalSetTimer 对 InRate <= 0 是【直接 Invalidate、根本不建定时器】
	//（TimerManager.cpp:673 的 if / 730 的 InOutHandle.Invalidate()），那个写法一次都不会触发。
	// 自递归是安全的：定时器 Tick 的循环条件是严格不等（TimerManager.cpp:1212
	// `InternalTime > Top->ExpireTime`），而在回调里新排的 next-tick 定时器 ExpireTime 正好等于
	// 当前 InternalTime（:783），当帧不会再被弹出，要等下一帧 InternalTime 涨上去。
	LastCameraAlignRealTime = World->GetRealTimeSeconds();
	World->GetTimerManager().SetTimerForNextTick(this, &UGA_DeathHarvest::TickCameraAlign);
}

void UGA_DeathHarvest::TickCameraAlign()
{
	if (!bCameraAligning)
	{
		return;
	}

	UWorld* World = GetWorld();
	AActor* Avatar = GetAvatarActorFromActorInfo();
	APawn* Pawn = Cast<APawn>(Avatar);
	APlayerController* PC = Pawn ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	if (!World || !Avatar || !PC)
	{
		// 拿不到本机控制器（中途被解除控制/换 pawn）→ 不再续期，这条链自己断掉。
		// 定时器本身也在 EndCameraAlign / EndAbility 里清过。
		return;
	}
	++CameraAlignTicks;

	// ★ 插值用【真实】delta，不用世界 delta：慢放期间世界 delta 只有四分之一，拿它插值的话
	//   甩镜会跟着一起变慢，顿帧结束时镜头还没转到位。GetRealTimeSeconds 是那条 "NOT dilated"
	//   的时间轴（World.h:2861）。Clamp 是为了扛住卡顿帧 —— 一帧 2 秒的 hitch 会直接跳到位。
	const double Now = World->GetRealTimeSeconds();
	const float RealDelta = FMath::Clamp(static_cast<float>(Now - LastCameraAlignRealTime), 0.f, 0.25f);
	LastCameraAlignRealTime = Now;

	// 目标朝向：现身之后看向敌人。
	//
	// ⚠️ 这里【不能】直接用 (敌人 - 自己)。传送是服务器权威的，而自主代理（本端玩家自己那个 pawn）
	//    不一定套用服务器 P3 那次 SetActorLocation —— 本端坐标还停在【消失点】时，这条线算出来是
	//    "消失前那条线"，而玩家的相机本来就在那条线上（TA 就是对着敌人才点的 R），
	//    RInterpTo 前后几乎一模一样 = 客户端镜头纹丝不动。主机上坐标真的变了，所以看不出这个问题。
	//
	//    改用【落点 → 敌人】：落点在敌人身后一个 BehindDistance（ComputeDestination 的候选表），
	//    所以这个方向 = 来路的反方向，两端用同一份数据（消失点 + 敌人当前位置）算，结果一致。
	//    判据是"她到底动没动"：动了（主机、或被纠正过的客户端）就用真实几何，没动就按上面反推。
	const FVector AvatarPos = Avatar->GetActorLocation();
	// 阈值（cm，只看平面）：传送跨度至少 BehindDistance（140cm），远大于它，不会误判。
	constexpr float MovedThreshold = 10.f;
	const bool bTeleportApplied = bHasVanishLocation
		&& FVector::DistSquared2D(AvatarPos, VanishLocation) > FMath::Square(MovedThreshold);

	FVector ToTarget;
	if (const AActor* Target = LockedTarget.Get())
	{
		ToTarget = (bTeleportApplied || !bHasVanishLocation)
			? Target->GetActorLocation() - AvatarPos                 // 真实几何：落点 → 敌人
			: VanishLocation - Target->GetActorLocation();           // 反推：落点在敌人身后，这条就是「落点 → 敌人」
	}
	else if (bHasVanishLocation)
	{
		// 没拿到目标：只剩"来路"这条线可用（它穿过敌人）。目标为空时这条会退化成零向量，
		// 所以第一帧日志里专门打了目标名 —— 出现"目标=None"就得先查载荷为什么没到本端。
		ToTarget = VanishLocation - AvatarPos;
	}
	else
	{
		ToTarget = Avatar->GetActorForwardVector();
	}

	FRotator Desired = ToTarget.GetSafeNormal().Rotation();
	// Pitch 夹到 ±80：贴脸/上下层的时候这条线会趋近垂直，不夹的话镜头会翻过顶；
	// Roll 一律归零，不然「从下往上看」会带出倾斜。
	Desired.Pitch = FMath::Clamp(Desired.Pitch, -80.f, 80.f);
	Desired.Roll = 0.f;

	// 每帧写 control rotation：摇臂是 bUsePawnControlRotation（LOLCharacter.cpp:39），它经
	// Pawn::GetViewRotation 拿到这个值，所以镜头自动跟着转，不用碰摇臂。
	// RInterpTo 内部对 delta 取 GetNormalized()，yaw 跨 ±180 不会绕远路。
	const float AlignSpeed = AbilityData ? AbilityData->CameraAlignSpeed : 9.f;
	const FRotator Before = PC->GetControlRotation();
	const FRotator After = FMath::RInterpTo(Before, Desired, RealDelta, AlignSpeed);
	PC->SetControlRotation(After);

	// 首帧打一条：把「写之前 / 目标 / 写之后」三个 yaw 摆在一起。
	// 三个数都一样 = 本来就对着目标（那不是 bug，是这条甩镜没必要）；写之后 ≠ 目标 = 还没转到位；
	// 写之后 = 写之前 ≠ 目标 = 这一帧 delta 太小（掉帧/被慢放拖住），得看下一帧。
	if (!bCameraAlignLoggedFirstTick)
	{
		bCameraAlignLoggedFirstTick = true;
		UE_LOG(LogTemp, Warning,
			TEXT("[DeathHarvest] 甩镜第一帧：真实Δ=%.3fs 本端(%.0f,%.0f) 消失点(%.0f,%.0f) 传送已套用=%d yaw %.1f → 目标 %.1f → 写后 %.1f（读回 %.1f）｜%s 施法者=%s 目标=%s"),
			RealDelta, AvatarPos.X, AvatarPos.Y, VanishLocation.X, VanishLocation.Y, bTeleportApplied ? 1 : 0,
			Before.Yaw, Desired.Yaw, After.Yaw, PC->GetControlRotation().Yaw,
			NetModeText(World), *GetNameSafe(Avatar), *GetNameSafe(LockedTarget.Get()));
	}

	// 甩够了就停手，把镜头彻底交回玩家（RInterpTo 是渐近的，不收的话它会一直微调下去）。
	// 判在插值【之后】：最后一帧照样推一次，镜头不会差最后一点点没到位。
	if (Now >= CameraAlignEndRealTime)
	{
		bCameraAligning = false;
		// 「最后一帧写前」是判断"写进去的 control rotation 到底留住没有"的唯一现场：
		// 甩满 0.45s 之后它还停在玩家自己的朝向上（离目标很远）= 每帧都被谁写回去了，
		// 而不是甩镜算错了方向。帧数太少则是另一回事：镜头根本没转到位就被收了。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 甩镜结束：%d 帧，yaw 停在 %.1f（目标 %.1f，差 %.1f；最后一帧写前 %.1f）｜施法者=%s"),
			CameraAlignTicks, After.Yaw, Desired.Yaw, FRotator::NormalizeAxis(Desired.Yaw - After.Yaw), Before.Yaw,
			*GetNameSafe(Avatar));
		return;
	}

	World->GetTimerManager().SetTimerForNextTick(this, &UGA_DeathHarvest::TickCameraAlign);
}

// =============================================================================
// P4 转圈
// =============================================================================

void UGA_DeathHarvest::StartSpinVisuals()
{
	if (!AbilityData)
	{
		// PlaySpinMontage 里也有同一份检查，但那个函数到不了了（它在这一句后面）——直接在这里收招。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] AbilityData 没配 → 转圈无从播起，直接结束"));
		CancelAsFailed();
		return;
	}

	// 移动从 P1 就锁着，这里不再重复锁（ApplyMovementLock 也本来就是幂等的）。
	// 只关掉移动组件的自动转向：转的是动画，actor 的 yaw 全程不动。
	ApplySpinRotationOverride(true);

	// 循环表现挂技能上：技能一结束（正常结束或被取消）引擎自动摘干净，一行清理代码都不用写。
	// 循环的那条走 Add（挂上去等移除），一次性的那几条走 Execute（见 ExecuteLocationCue）。
	// ⚠️ 跟着蒙太奇走在起手这一帧，【不】跟着伤害推后 —— 这一段刀已经在动了，只是还没扫到人。
	if (AActor* Avatar = GetAvatarActorFromActorInfo())
	{
		FGameplayCueParameters Params;
		Params.Instigator = Avatar;
		Params.EffectCauser = Avatar;
		Params.Location = Avatar->GetActorLocation();
		K2_AddGameplayCueWithParams(LOLGameplayTags::GameplayCue_DeathHarvest_Spin, Params, /*bRemoveOnAbilityEnd=*/true);
	}

	PlaySpinMontage();
}

void UGA_DeathHarvest::StartSpinPulses()
{
	if (SpinTask)
	{
		return;
	}

	if (!AbilityData)
	{
		return;
	}

	// 伤害跳用 Repeat 任务，不用裸 FTimerHandle：任务跟着技能一起死，天生不会有孤儿计时器。
	// （ThrowDaggerAbility 用的是裸 FTimerHandle，所以它得在 EndAbility 里自己清 —— 那是要避免的写法。）
	SpinTask = UAbilityTask_Repeat::RepeatAction(this, AbilityData->SpinInterval, AbilityData->SpinCount);
	SpinTask->OnPerformAction.AddDynamic(this, &UGA_DeathHarvest::OnSpinPulse);
	SpinTask->ReadyForActivation();
}

void UGA_DeathHarvest::PlaySpinMontage()
{
	if (!AbilityData || !AbilityData->Montage)
	{
		// 收招认的是「蒙太奇播完」，没有动画就没有终点 → 这次按没放出去处理。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] Montage 没配 → 收招没有终点，直接结束"));
		CancelAsFailed();
		return;
	}

	// 转圈那一节。服务器这一份用任务播、绑完成回调（播完就收招）；客户端那一份在
	// PlayClientMontage 里播（已经按同一份时长对齐过起播时刻）。
	// bStopWhenAbilityEnds=true：被打断时动画跟着技能一起停。
	MontageTask = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, NAME_None, AbilityData->Montage, 1.f, AbilityData->MontageStartSection, /*bStopWhenAbilityEnds=*/true);
	MontageTask->OnCompleted.AddDynamic(this, &UGA_DeathHarvest::OnMontageFinished);
	MontageTask->OnBlendOut.AddDynamic(this, &UGA_DeathHarvest::OnMontageFinished);
	MontageTask->OnInterrupted.AddDynamic(this, &UGA_DeathHarvest::OnMontageFinished);
	MontageTask->OnCancelled.AddDynamic(this, &UGA_DeathHarvest::OnMontageFinished);
	MontageTask->ReadyForActivation();
}

void UGA_DeathHarvest::StopSpin()
{
	if (SpinTask)
	{
		SpinTask->EndTask();
		SpinTask = nullptr;
	}

	ApplySpinRotationOverride(false);
	ApplyMovementLock(false);

	// 收招【不】需要回正朝向：actor 的 yaw 从 P3 现身那一刻起就没动过（转的是动画、不是 actor），
	// 一圈转完正好回到「面向目标」那个起始朝向。
}

void UGA_DeathHarvest::OnSpinPulse(int32 ActionNumber)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->HasAuthority() || !AbilityData)
	{
		return;
	}
	OnSpinPulse_Server();
}

void UGA_DeathHarvest::OnSpinPulse_Server()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !GetWorld() || !AbilityData)
	{
		return;
	}

	// 绕自身的球形扫描（抄 GA_ThreeHitPassive.cpp:424-429 的形状，只是把"沿朝向扫"换成"原地扫"）。
	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeathHarvestSpin), false, Avatar);
	const FVector Center = Avatar->GetActorLocation();
	const FCollisionShape Shape = FCollisionShape::MakeSphere(AbilityData->SpinHitRadius);

	if (!GetWorld()->SweepMultiByChannel(Hits, Center, Center, FQuat::Identity, ECC_Pawn, Shape, Params))
	{
		return;
	}

	// 同一次脉冲里同一个目标可能被返回多次（多段重叠），去重。
	// 目标在转圈中死亡【不中断】：这是范围伤害，继续打完（见 §7 的取消矩阵）。
	TSet<AActor*> Damaged;
	bool bLanded = false;
	for (const FHitResult& Hit : Hits)
	{
		AActor* Target = Hit.GetActor();
		if (!Target || Target == Avatar || Damaged.Contains(Target))
		{
			continue;
		}
		Damaged.Add(Target);
		// ★ 用 |= 【不能】短路：一次脉冲里可能有多个目标，短路会让后面的目标连伤害都不结算
		//   （表现是"人多的时候只有一个人掉血"，而且只在有人先命中的时候才复现）。
		bLanded |= ApplySpinDamage(Target, Hit);
	}

	// 顿帧挂在"真的打到人"上，不是"挥了这一下"上：空挥的那几跳不该顿。
	if (bLanded)
	{
		BeginHitStop();
	}
}

bool UGA_DeathHarvest::ApplySpinDamage(AActor* Target, const FHitResult& Hit)
{
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
	if (!SourceASC || !TargetASC || !AbilityData->DamageEffect)
	{
		// 不静默：Spec 无效 = 这一下一点伤害都没有，屏幕上看起来像"打空了"。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 目标 %s 没有 ASC（或 DamageEffect 没配）→ 本次命中无伤害"),
			*GetNameSafe(Target));
		return false;
	}

	// 这里【只喂参数，不算伤害】：公式只在 UExecCalc_Damage 里（见 GAS_Block_Setup.md §3.6）。
	FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
	Context.AddHitResult(Hit);

	FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(AbilityData->DamageEffect, GetAbilityLevel(), Context);
	if (!Spec.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] DamageEffect(%s) 的 Spec 无效 → 本次命中无伤害"),
			*GetNameSafe(AbilityData->DamageEffect));
		return false;
	}

	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, AbilityData->DamageMultiplier);
	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, AbilityData->FlatDamage);
	// 斩杀系数：目标越残血打得越疼。公式在 UExecCalc_Damage（§5.5），这边只喂系数。
	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_MissingHealthBonus, AbilityData->MissingHealthBonus);
	// 物理伤害。不挂类型标签时 ExecCalc 也按物理算，显式挂上是为了以后加魔法大招时不用回头猜默认值。
	Spec.Data->AddDynamicAssetTag(LOLGameplayTags::Damage_Physical);

	SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);

	// =============================================================================
	// 海克斯「死亡收割 · 压制」：命中额外挂眩晕，【只第一跳】
	//
	// 判据是 ASC 上的状态标签（由 GE_Arena_Augment_DeathHarvestStun 的 TargetTags 组件授予），
	// 技能只读不写 —— 配海克斯不用回来改这个类，不配就完全不进这个分支。
	//
	// ★ bAugmentStunApplied 那个门是【强度约束】，不是优化：
	//   转圈是 SpinCount=8 × SpinInterval=0.25s = 2 秒、8 次 ApplySpinDamage，
	//   而 UGE_Stun 是 HasDuration，重复施加对已生效的同源实例是【刷新时长不是叠层】
	//   ⇒ 每跳都施 = 对手被锁死整整 2 秒且中途无法交闪，远超 LoL 同类大招。
	//   要调强度改 AugmentStunDuration，**不要**把这个门删掉。
	//
	// 时长 <= 0 也要挡住：配成 0 的效果是「GE 挂上就立刻过期」，静默失效且没有任何日志
	// —— 和 SetByCaller 不填是同一个坑。
	// =============================================================================
	if (!bAugmentStunApplied && AugmentStunEffect && AugmentStunDuration > 0.f
		&& SourceASC->HasMatchingGameplayTag(LOLGameplayTags::Hex_DeathHarvest_Stun))
	{
		// 独立一份 Context：不要复用上面伤害那份。Context 会被 GE 组件读
		//（从里面取 HitResult 算方向/位置），共用一份会让后面挂的 GE 拿到被改写过的命中信息。
		FGameplayEffectContextHandle StunContext = SourceASC->MakeEffectContext();
		StunContext.AddHitResult(Hit);

		FGameplayEffectSpecHandle StunSpec =
			SourceASC->MakeOutgoingSpec(AugmentStunEffect, GetAbilityLevel(), StunContext);
		if (StunSpec.IsValid())
		{
			StunSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_ControlDuration, AugmentStunDuration);
			SourceASC->ApplyGameplayEffectSpecToTarget(*StunSpec.Data.Get(), TargetASC);
			// 只在真的挂上之后置位 —— 放前面的话 Spec 失败一次就再也不试了
			//（这一趟 R 剩下的跳全被门掉，整趟完全没有眩晕，而日志只有一条 Warning）。
			bAugmentStunApplied = true;
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[DeathHarvest][%s] 海克斯眩晕没挂上：AugmentStunEffect(%s) 的 Spec 无效"),
				NetModeText(GetWorld()), *GetNameSafe(AugmentStunEffect));
		}
	}

	// 命中表现走 cue：这个函数只在服务端跑，直接 Spawn 的话粒子只有主机看得到
	// （GC_ThrowDaggerHit 修掉的是同一个坑）。镜头震动也挂在这条 cue 上
	// （UGC_DeathHarvestBurst::CameraShake，只在施法者本机真的摇）。
	ExecuteLocationCue(LOLGameplayTags::GameplayCue_DeathHarvest_Hit, Hit.ImpactPoint, Hit.ImpactNormal);

	return true;
}

// =============================================================================
// 移动 / 旋转 / 表现
// =============================================================================

void UGA_DeathHarvest::ApplyMovementLock(bool bLock)
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	UCharacterMovementComponent* MoveComp = Character ? Character->GetCharacterMovement() : nullptr;
	if (!MoveComp)
	{
		return;
	}

	if (bLock)
	{
		if (!bMovementLocked)
		{
			SavedMovementMode = MoveComp->MovementMode;
			bMovementLocked = true;
		}
		MoveComp->StopMovementImmediately();
		MoveComp->DisableMovement();     // = SetMovementMode(MOVE_None)
	}
	else if (bMovementLocked)
	{
		MoveComp->SetMovementMode(SavedMovementMode);
		bMovementLocked = false;
	}
}

void UGA_DeathHarvest::ApplySpinRotationOverride(bool bOn)
{
	// 这里【不】驱动旋转：转圈是动画自己在转（没有 root motion 也照样转 —— 转的是根骨骼，
	// 不是 actor）。代码再叠一层 AddActorWorldRotation 就是转两倍速，所以 v1 那套按 SpinRate
	// 逐帧转 yaw 的实现（连同 bRootMotionSpin/SpinRate 两个配置项）已经删掉。
	// 剩下的作用只有一个：关掉移动组件的自动转向，别让它跟动画抢 actor 的朝向。
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	UCharacterMovementComponent* MoveComp = Character ? Character->GetCharacterMovement() : nullptr;
	if (!Character || !MoveComp)
	{
		return;
	}

	if (!bOn)
	{
		// 只在真正关过的时候恢复，避免取消/提前结束路径把默认值误写回去。
		if (bOrientRotationOverridden)
		{
			MoveComp->bOrientRotationToMovement = bSavedOrientRotationToMovement;
			Character->bUseControllerRotationYaw = bSavedUseControllerRotationYaw;
			bOrientRotationOverridden = false;
		}
		return;
	}

	// 关掉两个开关：bOrientRotationToMovement（ALOLCharacter 默认 true）和
	// bUseControllerRotationYaw（默认 false，但别人身上的默认值不一定）。
	if (!bOrientRotationOverridden)
	{
		bSavedOrientRotationToMovement = MoveComp->bOrientRotationToMovement;
		bSavedUseControllerRotationYaw = Character->bUseControllerRotationYaw;
		bOrientRotationOverridden = true;
	}
	MoveComp->bOrientRotationToMovement = false;
	Character->bUseControllerRotationYaw = false;
}

void UGA_DeathHarvest::ExecuteLocationCue(const FGameplayTag CueTag, const FVector& Location, const FVector& Normal) const
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!Avatar || !ASC || !Avatar->HasAuthority())
	{
		// 客户端不发 cue：服务器 ExecuteGameplayCue 会把参数多播到每一端各自跑一遍，
		// 本端再发一次就是两份（原方案担心的"双份"就是这个，门在这里就够了）。
		return;
	}

	FGameplayCueParameters Params;
	Params.Location = Location;
	Params.Normal = Normal;
	Params.Instigator = Avatar;
	Params.EffectCauser = Avatar;
	ASC->ExecuteGameplayCue(CueTag, Params);
}

void UGA_DeathHarvest::AddLocationCue(const FGameplayTag CueTag, const FVector& Location, const FVector& Normal)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->HasAuthority())
	{
		// 客户端不发 cue（理由同 ExecuteLocationCue）。
		return;
	}

	FGameplayCueParameters Params;
	Params.Location = Location;
	Params.Normal = Normal;
	Params.Instigator = Avatar;
	Params.EffectCauser = Avatar;

	// bRemoveOnAbilityEnd=true 是这里的重点：它是「技能被打断也不会留下隐形人 / 半开的门」的唯一保障。
	// 正常路径上 P3 会自己 K2_RemoveGameplayCue 提前摘（那条路会把 tag 从 TrackedGameplayCues 里删掉，
	// 所以 Super::EndAbility 不会重复摘）。
	//
	// ⚠️ 运行时 Add 的 cue 在服务器那一端收到的是 WhileActive、客户端才是 OnActive
	// （AbilitySystemComponent.cpp:1601-1605），所以这三条 cue 的类都必须把 WhileActive 转发到 OnActive ——
	// 少一个，主机那台机器上的表现就是哑的（而且不报错）。
	// ★ cue 落在【哪个 ASC 的 avatar】上 —— "藏起来的是谁"完全取决于它。
	//   日志里两边名字不一样时，第一个该看的就是这一行。
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 挂 cue %s → ASC=%s（avatar=%s）｜%s"),
		*CueTag.ToString(), *GetNameSafe(ASC), *GetNameSafe(ASC ? ASC->GetAvatarActor() : nullptr),
		NetModeText(GetWorld()));

	K2_AddGameplayCueWithParams(CueTag, Params, /*bRemoveOnAbilityEnd=*/true);
}

void UGA_DeathHarvest::CancelAsFailed()
{
	// 退冷却：落点全废 / 目标丢失时这次技能等于没放出去（见 §7 取消矩阵）。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		FGameplayTagContainer CooldownTags;
		CooldownTags.AddTag(LOLGameplayTags::State_Cooldown_DeathHarvest);
		ASC->RemoveActiveEffectsWithGrantedTags(CooldownTags);
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
		/*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
}

// =============================================================================
// 收尾
// =============================================================================

void UGA_DeathHarvest::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	const bool bServer = Avatar && Avatar->HasAuthority();

	// ★ 技能一旦结束，相位任务会被一起杀掉、OnVanishFinished 再也不会广播 —— 表现就是「人消失了卡住不动」。
	//   所以每次收尾都在这里留一行：谁结束的（bWasCancelled）、这一趟起过哪几个相位任务。
	//   （指针非空只代表「起过」，不代表没跑完 —— 任务跑完不会把我们的指针清掉。）
	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] EndAbility｜%s：施法者=%s 权威=%d 取消=%d 起过的相位任务[P1=%d P2=%d 客户端=%d 打击推后=%d 起手慢放=%d 顿帧=%d 伤害跳=%d]"),
		NetModeText(GetWorld()), *GetNameSafe(Avatar), bServer ? 1 : 0, bWasCancelled ? 1 : 0,
		VanishDelayTask ? 1 : 0, PortalDelayTask ? 1 : 0, ClientDelayTask ? 1 : 0,
		SpinImpactTask ? 1 : 0, SpinSlowTask ? 1 : 0, HitStopTask ? 1 : 0, SpinTask ? 1 : 0);

	// ⚠️ 这里是"被打断/施法者死亡"唯一能走到的地方（相位任务跟着技能一起死，后面的回调一个都不会来），
	// 所以每一样都必须在【这里】收干净。这些都是幂等的。
	//
	// 两端都要收，不能只收服务器那份：客户端的移动锁是 PlayClientTimeline 在本端锁的，
	// 只收服务器那份的话本端会一直停在 MOVE_None ——表现是「放完大招之后自己再也走不动了」。
	//
	// ★ 两个时间流速放在最前面，它们的报错最不像"技能坏了"：
	//   EndHitStop 漏了 = 整个世界卡在顿帧的流速上（0.05 速）；
	//   EndSpinSlowMotion 漏了 = 她本人一直慢动作，走路和出刀都像卡带。
	//   而技能被打断时（死亡/被控）只有这里能还原。
	EndHitStop();
	EndSpinLead();
	StopSpin();
	ApplyMovementLock(false);

	if (bServer)
	{
		if (bCastingTagAdded)
		{
			if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
			{
				ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_DeathHarvest_Casting);
			}
			bCastingTagAdded = false;
		}

		// 三条状态 cue（消失 / 传送门 / 转圈）都是 K2_AddGameplayCueWithParams(..., bRemoveOnAbilityEnd=true)
		// 挂上来的，Super::EndAbility 会按 TrackedGameplayCues 全部摘掉；P3 已经提前摘过的那两条
		// 会被 K2_RemoveGameplayCue 从表里划掉，不会重复摘。
		// 摘不干净的表现最重的是「消失」那条：人一直隐形、碰撞还是关的、传送镜头也留在屏幕上。
	}

	LockedTarget.Reset();
	PortalLocation = FVector::ZeroVector;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
