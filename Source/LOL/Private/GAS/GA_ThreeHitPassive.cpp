// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/GA_ThreeHitPassive.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/ThreeHitPassiveData.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/HeroCombatCharacter.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/GE_EmpoweredAttack.h"
#include "GAS/GE_Knockback.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotify_SendGameplayEvent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"   // GetNameSafe(Mesh->GetSkeletalMeshAsset())：SkeletalMeshComponent.h 只前置声明 USkeletalMesh
#include "Engine/World.h"
#include "Camera/CameraShakeBase.h"   // Stage.ChargeCameraShake：ClientStartCameraShake 要 TSubclassOf<UCameraShakeBase>
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"   // 命中顿帧：世界时间流速（TimeDilation 要完整类型，前向声明不够）
#include "GameFramework/PlayerController.h"   // 蓄力镜头（ClientStartCameraShake）
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"

namespace
{
	/**
	 * 提示光 NS 里的两个位置参数名（含 User. 前缀），和 NS_BladeTrail 用的是同一套命名 ——
	 * 这几个系统是一起做的，改 NS 里的参数名时这里要跟着改（对不上是静默 no-op，见下）。
	 * （命中那个 NS 的 User.ImpactPos 不在这儿：它已经搬到 UGC_EmpoweredHit 里去了。）
	 */
	const FName PerfectWindowBaseParameter(TEXT("User.SwordBasePos"));
	const FName PerfectWindowTipParameter(TEXT("User.SwordTipPos"));

	// 本文件里这几个 helper 都带 ThreeHit 前缀，别改回通用名（HasSocket / DescribeUserParameters /
	// WarnIfMissingParameter）：unity build 会把多个 .cpp 合进同一个 Module.LOL.N.cpp，匿名 namespace
	// 只挡【跨 TU】的冲突，合进一个 TU 之后同签名就是重定义（C2084），而哪些文件同批由 UBT 按文件名
	// 决定、不受这里控制。AnimNotifyState_BladeTrail / GC_EmpoweredHit 里各有一份同样的实现。

	/** 插槽和骨骼都算数：GetSocketLocation 对骨骼名同样有效。和 UAnimNotifyState_BladeTrail 里那份判断一致。 */
	bool ThreeHitHasSocket(const USkeletalMeshComponent* MeshComp, const FName& SocketName)
	{
		return MeshComp->DoesSocketExist(SocketName) || MeshComp->GetBoneIndex(SocketName) != INDEX_NONE;
	}

	/** 把系统里现有的 User.* 参数拼成一行，参数名对不上时直接打出来对照。 */
	FString ThreeHitDescribeUserParameters(UNiagaraSystem* System)
	{
		TArray<FNiagaraVariable> Parameters;
		System->GetExposedParameters().GetParameters(Parameters);

		TArray<FString> Names;
		for (const FNiagaraVariable& Parameter : Parameters)
		{
			const FString Name = Parameter.GetName().ToString();
			if (Name.StartsWith(TEXT("User.")))
			{
				Names.Add(Name);
			}
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("（一个都没有）"));
	}

	/**
	 * 参数名对不上时 SetVariablePosition 是静默 no-op（NiagaraComponent.cpp 里会退化成 SetVariableVec3，
	 * 名字还是找不到就什么都不做）→ 特效生成了但坐标喂不进去（光一直趴在原点），看不出原因。
	 * 插槽名对不上也一样是静默的（GetSocketLocation 返回组件位置），所以这两处都当场核一遍并打日志。
	 */
	void ThreeHitWarnIfMissingParameter(UNiagaraSystem* System, const FName& ParameterName, const TCHAR* What)
	{
		TArray<FNiagaraVariable> Parameters;
		System->GetExposedParameters().GetParameters(Parameters);

		const bool bFound = Parameters.ContainsByPredicate(
			[&ParameterName](const FNiagaraVariable& Parameter) { return Parameter.GetName() == ParameterName; });
		if (!bFound)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Passive] %s：NS %s 里没有参数 %s → 坐标喂不进去。系统里现有的 user parameter：%s"),
				What, *GetNameSafe(System), *ParameterName.ToString(), *ThreeHitDescribeUserParameters(System));
		}
	}

	/**
	 * 命中目标类型（Target.Hero / Target.Void / 其它 → Target.Terrain），塞进 cue 参数的
	 * AggregatedTargetTags，让 cue 按类型挑粒子 —— 和 AThrowDaggerProjectile::ResolveHitType 同一套判据。
	 *
	 * 为什么不复用匕首那份：它是投射物的成员函数（要用自己的命中上下文），这里没有投射物。
	 * 抽出公共 helper 要动那个已经在跑的类，为 6 行代码不值 —— 判据只有「有没有这两个标签」，
	 * 两边同时要改的概率极低；真改了记得两处一起（匕首/近战在目标类型上应当永远一致）。
	 */
	FGameplayTag ThreeHitResolveTargetType(AActor* Target)
	{
		if (const UAbilitySystemComponent* ASC =
			UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target))
		{
			if (ASC->HasMatchingGameplayTag(LOLGameplayTags::Target_Hero)) return LOLGameplayTags::Target_Hero;
			if (ASC->HasMatchingGameplayTag(LOLGameplayTags::Target_Void)) return LOLGameplayTags::Target_Void;
		}
		return LOLGameplayTags::Target_Terrain;
	}

	/**
	 * 这条蒙太奇上「命中结算」通知（UAnimNotify_SendGameplayEvent，标签对得上）的时刻（蒙太奇秒）。
	 * 直接读资产上的 Notifies 数组，不需要蒙太奇正在播。没有则返回 -1。
	 *
	 * 为什么要读这个时刻：命中判定该跟着【动画帧】走，但不再依赖「服务器真的评估动画、触发 notify」——
	 * 服务器端用这个时刻排一个定时器，dedicated server（mesh 走参考姿势、不评估 notify）下也照样结算。
	 * 详见 StartStage 里那段注释。
	 */
	float ThreeHitGetImpactNotifyTime(const UAnimMontage* Montage, const FGameplayTag& Tag)
	{
		if (!Montage || !Tag.IsValid())
		{
			return -1.f;
		}

		for (const FAnimNotifyEvent& NotifyEvent : Montage->Notifies)
		{
			const UAnimNotify_SendGameplayEvent* SendEvent = Cast<UAnimNotify_SendGameplayEvent>(NotifyEvent.Notify);
			if (SendEvent && SendEvent->EventTag == Tag)
			{
				return NotifyEvent.GetTime();
			}
		}
		return -1.f;
	}
}

UGA_ThreeHitPassive::UGA_ThreeHitPassive()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;   // 每 actor 一个技能实例，成员变量能在多次激活间保持
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;  // 客户端预测表现，服务端权威结算
	AttackInputTag = LOLGameplayTags::Event_Input_BasicAttack;
	MeleeImpactTag = LOLGameplayTags::Event_Melee_Impact;
	ActivationPolicy = EMyAbilityActivationPolicy::OnEvent;   // Passive: event-triggered, not button-triggered
	EmpoweredAttackGE = UGE_EmpoweredAttack::StaticClass();
	KnockbackGE = UGE_Knockback::StaticClass();
}

void UGA_ThreeHitPassive::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	UE_LOG(LogTemp, Warning, TEXT("[Passive] ActivateAbility 被调用"));

	// 没配数据资产时临时 New 一个兜底（正式使用应在编辑器里赋值）。
	if (!PassiveData)
	{
		PassiveData = NewObject<UThreeHitPassiveData>(this);
	}

	// 至少要有 1 段数据，且能通过消耗/冷却检查，否则直接结束。
	// ⚠️ 这里原来写的是 `Stages.Num() != 3`（三连击时代的硬编码），现在放开成「>=1」：
	//   连段推进、窗口、命中结算这些逻辑全都是按 Stages.Num() 通用写的
	//   （IsValidIndex / Stages.Num()-1 / 循环遍历），段数从来不需要固定。
	//   而空手四连拳（ParagonCrunch 拳击重定向）要的就是 4 段。
	//   段数配错（比如 0 段）仍然会被这里挡掉。
	if (PassiveData->Stages.Num() < 1 || !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 检查失败（Stages=%d <1 或 CommitAbility 失败）→ EndAbility"),
			PassiveData->Stages.Num());
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	// 破隐那一击本身就是强化的：「起手这一刻还在隐身」就当场挂强化，再把隐身摘掉。
	// 放在能力里（而不是角色按键处理里）是为了两端对称：客户端本地预测的这次激活、
	// 服务端收到 ServerTryActivateAbility 的那次激活，都在 StartStage 之前走这一段，
	// 不再依赖「破隐那条 RPC 先到、还是激活消息先到」—— 实测服务端是激活先到，
	// 于是服务端起手选蒙太奇时 tag 还是旧的（两端都播了普通动画）。
	// 也不能只靠 GA_Stealth 的 State.Stealth 归零回调来挂强化：那个回调挂在 GA_Stealth 的实例上，
	// 客户端那份实例未必还在，回调不跑 → 客户端表现退回普通攻击，而服务端那份跑得到 →
	// 就是「有击退、没强化动画」的样子。这里就地做完，两边都跑得到。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		if (ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Stealth))
		{
			const AActor* Avatar = GetAvatarActorFromActorInfo();
			UE_LOG(LogTemp, Warning, TEXT("[Stealth] 普攻破隐（起手时，权威=%d）"), (Avatar && Avatar->HasAuthority()) ? 1 : 0);

			GrantEmpoweredAttack();	// 先挂：挂的时候还得读得到「我刚才在隐身」

			// 必须用 Granted 版：State.Stealth 是 UTargetTagsGameplayEffectComponent 授的 Granted Tag，
			// RemoveActiveEffectsWithTags 只比对 GE 的 Asset Tags，匹配不上会静默不删（破隐看起来「没反应」）。
			// 也得走 RemoveGrantedTagEffects：ASC 自带的 Remove* 在非权威端是静默 no-op，
			// 客户端那份预测副本要自己摘（客户端摘到 0 个属正常：本地副本已被 catch-up 收走，
			// 只剩服务端复制来的那份，由服务端自己摘）。移除不可预测，两端各自跑到这里，各摘各的。
			UMyAbilitySystemComponent::RemoveGrantedTagEffects(ASC, FGameplayTagContainer(LOLGameplayTags::State_Stealth));
		}
	}

	// 监听下一次按键（用于连段），然后起手第一段。
	InputTask = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, AttackInputTag, nullptr, false, true);
	InputTask->EventReceived.AddDynamic(this, &ThisClass::OnAttackInput);
	InputTask->ReadyForActivation();

	// ⚠️ 命中通知任务【不在这里建】—— 它改成在 StartStage 里按本段建（见 RebuildImpactTask）。
	//   原因：每段可以有独立的 ImpactTag（空手四连拳就是 Boxing1~4 四个不同标签），
	//   固定建一个订阅就等于「只有其中一段的标签生效」。
	//   不填段 ImpactTag 的数据（三连普攻）行为和以前完全一样：每段都建一个订阅 MeleeImpactTag。

	StartStage(0);

	// 起手这一按自己会作为 Event.Input.BasicAttack 紧跟着送进来（RouteBasicAttackInput 里激活之后紧接着发），
	// 置位让 OnAttackInput 吞掉它 —— 不然它会被输入缓冲收下、当成「连段输入」，一次按键出两段。
	// 放在 StartStage 之后：这次输入一定在 ActivateAbility 返回之后才派发，置位不会被 StartStage 冲掉。
	bSwallowOpeningInput = true;
}

void UGA_ThreeHitPassive::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	// 清掉所有定时器。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(HitTimer);
		World->GetTimerManager().ClearTimer(OpenTimer);
		World->GetTimerManager().ClearTimer(CloseTimer);
		World->GetTimerManager().ClearTimer(FinishTimer);
		StageMontageEndWorldTime = 0.f;

		// 完美窗口的提示光和它的三个定时器：技能被打断 / 提前结束时窗口关闭定时器不会跑到，
		// 不在这里收掉的话光会一直挂在刀上。
		World->GetTimerManager().ClearTimer(PerfectVFXOpenTimer);
		World->GetTimerManager().ClearTimer(PerfectVFXCloseTimer);
		World->GetTimerManager().ClearTimer(PerfectVFXTickTimer);
		// 完美窗口那两个标签定时器同理：能力结束还留着的话，AI 会以为窗口一直开着。
		World->GetTimerManager().ClearTimer(PerfectTagOpenTimer);
		World->GetTimerManager().ClearTimer(PerfectTagCloseTimer);
	}
	if (PerfectWindowVFX)
	{
		PerfectWindowVFX->DestroyComponent();
		PerfectWindowVFX = nullptr;
	}
	bPerfectWindowVFXActive = false;

	// 蓄力粒子：能力被打断 / 提前结束时那段定时器不会跑到，不收掉就会一直挂在手上下一段还带着光。
	StopChargeVFX();

	// 命中顿帧：还原定时器同样可能跑不到（能力被打断 / 提前结束）。
	// 不在这里兜底还原的话，整个世界会一直卡在顿帧的流速上 —— 这是顿帧唯一真正危险的失败模式，
	// 比「技能坏了」严重得多（症状是世界一直是慢动作，而不是某一次攻击不对）。
	// ⚠️ 必须在下面 StageIndex = INDEX_NONE 之前调，否则日志打不出是哪一段。
	EndHitStop();

	// 停掉输入监听任务。
	if (InputTask)
	{
		InputTask->EndTask();
		InputTask = nullptr;
	}
	if (ImpactTask)
	{
		ImpactTask->EndTask();
		ImpactTask = nullptr;
	}

	// 重置连段状态。
	StageIndex = INDEX_NONE;
	bWindowOpen = false;
	SetComboWindowTag(false);
	bQueuedNextStage = false;
	bHitAppliedThisStage = false;
	bPerfectQueued = false;
	bSwallowOpeningInput = false;
	SetPerfectWindowTags(/*bArmed=*/false, /*bOpen=*/false);
	PerfectWindowOpenWorldTime = 0.f;
	PerfectWindowCloseWorldTime = 0.f;
	ChainWindowOpenWorldTime = 0.f;
	ChainWindowCloseWorldTime = 0.f;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_ThreeHitPassive::OnAttackInput(FGameplayEventData Payload)
{
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;

	// 连段窗口开着，或者窗口开之前 ChainInputBufferTime 以内 → 这次按键算「排队下一段」。
	//
	// 缓冲这一段是给「窗口还没开就按了」的那一下兜的：原来直接 return 丢掉，
	// 玩家体感就是「我明明按了却没接上」，攻速快起来窗口只有零点几秒时尤其明显。
	// 注意缓冲只决定【收不收这次输入】，不决定【什么时候起手】—— 下一段永远等 CloseChainWindow
	// 才 StartStage，所以提前按不会让连段变快，只是不丢输入。
	//
	// 【PvP】和完美窗口同一套时刻口径（世界时间、都从各自那端的起手时刻算起）：
	// 服务端的起手比客户端晚单程延迟，这次按键的事件也走同一条 RPC 通道、同样晚单程延迟到，
	// 两端算出的「相对起手的时刻」一致 → 一端接上了另一端也接上了，不会出现「客户端连了、服务端断了」。
	// （恒定延迟下成立；延迟抖动会让两端判定有偏差，和完美窗口是同一个固有代价。）
	const bool bInChainWindow = bWindowOpen;
	const bool bInBuffer = !bInChainWindow
		&& ChainWindowCloseWorldTime > ChainWindowOpenWorldTime
		&& Now >= ChainWindowOpenWorldTime - GetChainInputBufferSeconds()
		&& Now <= ChainWindowCloseWorldTime;
	if (!bInChainWindow && !bInBuffer)
	{
		return;
	}

	// 起手那一按不是连段输入，吞掉一次（见 bSwallowOpeningInput）。它和本段起手同帧，落在缓冲区间里，
	// 不吞的话一次按键就能把下一段排上队 —— 表现就是「按一下平a打出两下」。
	if (bSwallowOpeningInput)
	{
		bSwallowOpeningInput = false;
		if (!bInChainWindow)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段起手那一按已吞掉（不是连段输入）"), StageIndex);
			return;
		}
	}

	// 完美判定只认连段窗口内的第一次按键：早按下去（哪怕后面补按在完美窗口里）也只能吃普通下一段。
	// 不这么锁的话，按住/连打会让按键事件反复进来（WaitGameplayEvent 的 OnlyTriggerOnce=false），
	// 总有一次落在完美窗口内 → 必完美，窗口就没有门槛了。
	if (bQueuedNextStage)
	{
		return;
	}
	bQueuedNextStage = true;

	// 时刻换算不需要补偿网络延迟：服务端的起手时刻比客户端晚单程延迟，
	// 而这次按键的事件也是走同一条 RPC 通道、同样晚单程延迟到 → 两端算出的「相对起手的时刻」一致。
	// （恒定延迟下成立；延迟抖动会让两端判定有偏差，属于预测本身的固有代价。）
	bPerfectQueued = PerfectWindowCloseWorldTime > PerfectWindowOpenWorldTime
		&& Now >= PerfectWindowOpenWorldTime && Now <= PerfectWindowCloseWorldTime;

	// 诊断：输入时刻和完美窗口区间并排打出来，早/晚一眼可见；两个 0 表示本段没有完美窗口。
	// 来源那一栏区分「窗口开着时按的」和「靠缓冲收下的」—— 缓冲有没有在起作用只能从这儿看出来。
	const AActor* Avatar = GetAvatarActorFromActorInfo();
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段输入: 权威=%d 来源=%s 完美=%d 输入时刻=%.3f 连段窗口开=%.3f 完美窗口=[%.3f,%.3f]"),
		StageIndex, (Avatar && Avatar->HasAuthority()) ? 1 : 0, bInBuffer ? TEXT("缓冲") : TEXT("窗口内"),
		bPerfectQueued ? 1 : 0, Now, ChainWindowOpenWorldTime,
		PerfectWindowOpenWorldTime, PerfectWindowCloseWorldTime);

	// 踩中的反馈当场放：玩家要的就是「按对了」这一下的即时反馈，等到下一段起手（CloseChainWindow 之后）
	// 才播就已经晚了一整段连段窗口，反馈和手感对不上。
	// 这里只有一声 —— 真正的「额外效果」（在目标身上炸开的那一下）要等强化击真命中，
	// 走 GameplayCue.EmpoweredHit（见 ApplyServerHit / UGC_EmpoweredHit）。
	if (bPerfectQueued)
	{
		PlayPerfectSuccessSound();
	}
}

float UGA_ThreeHitPassive::GetAttackPlayRate() const
{
	// 最终攻速 / 参考攻速 = 播放倍率，钳制在 0.1x ~ 4x，避免动画过快/过慢。
	const UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	const UHeroCombatAttributeSet* Attributes = ASC ? ASC->GetSet<UHeroCombatAttributeSet>() : nullptr;
	if (!Attributes)
	{
		return 1.f;
	}
	return FMath::Clamp(Attributes->GetFinalAttackSpeed() / PassiveData->ReferenceAttackSpeed, 0.1f, 4.f);
}

float UGA_ThreeHitPassive::GetChainInputBufferSeconds() const
{
	if (!PassiveData || PassiveData->ChainInputBufferTime <= 0.f)
	{
		return 0.f;
	}

	// 缓冲属于「当前这一段」，所以换算口径必须和本段的连段窗口一致 ——
	// 也就是连本段的 PlayRateMultiplier 一起除（StartStage 里那个 Rate 是两者的乘积）。
	// 只除攻速的话，单独提速的那一段缓冲会相对窗口变长，等于把那一段的连段门槛抹平。
	float Multiplier = 1.f;
	if (PassiveData->Stages.IsValidIndex(StageIndex))
	{
		Multiplier = FMath::Max(0.1f, PassiveData->Stages[StageIndex].PlayRateMultiplier);
	}

	// 和三个定时器同一套换算：蒙太奇秒 / 速率 = 世界秒。
	// 缓冲和连段窗口因此同比缩放 —— 攻速再快，缓冲也不会相对窗口变得过长（那等于把门槛抹掉）。
	return PassiveData->ChainInputBufferTime / (GetAttackPlayRate() * Multiplier);
}

void UGA_ThreeHitPassive::StartStage(int32 NewStage)
{
	// ★ 死了就收手。这道门必须放在【最前面】，在任何定时器/蒙太奇动作之前。
	//
	// 【为什么必须有】连段是靠"打开连段窗口 → 玩家在窗口内再按 → 起手下一段"跑起来的，
	// 中间挂着两个定时器（连段窗口开关）。玩家在【窗口开着的时候】被打死时：
	//   死亡蒙太奇已经在 DefaultSlot 上播了 → 窗口定时器到点 → 回到这里 → 下面那句
	//   `Anim->Montage_Play(MontageToPlay, Rate)` 用的是默认 bStopAllMontages=true
	//   ⇒ 按 slot group 清场，把【死亡蒙太奇直接停掉】（DefaultSlot 同组）。
	// 表现就是"死亡动画偶尔不播"——只在死亡恰好落在连段窗口内时发生，所以是概率性的。
	//
	// 顺带把没有 IsDead 门的隐患一起挡了：死人还能开连段窗口、还能再起手下一段。
	// 判定走 AHeroCombatCharacter::IsDead()（= State.Dead 标签），和移动/技能两侧同源。
	if (const AHeroCombatCharacter* Hero = Cast<AHeroCombatCharacter>(GetAvatarActorFromActorInfo()))
	{
		if (Hero->IsDead())
		{
			UE_LOG(LogTemp, Warning, TEXT("[Passive] StartStage %d 被跳过：角色已死亡（权威=%d）"),
				NewStage, Hero->HasAuthority() ? 1 : 0);
			// 把自己收掉：连段不会再继续，留着这个能力实例只会让下次激活的输入缓冲错乱。
			EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
				/*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
			return;
		}
	}

	StageIndex = NewStage;
	bWindowOpen = false;
	SetComboWindowTag(false);
	// 上一段的完美窗口标签一并摘掉，下面 SchedulePerfectWindowTags 会按本段重新挂。
	SetPerfectWindowTags(/*bArmed=*/false, /*bOpen=*/false);
	bQueuedNextStage = false;
	bPerfectQueued = false;
	bHitAppliedThisStage = false;

	const FThreeHitAttackStage& Stage = PassiveData->Stages[StageIndex];

	// 本段实际播放速率 = 攻速倍率 × 本段额外倍率（Stage.PlayRateMultiplier，默认 1）。
	//
	// ⚠️ 两个倍率必须合成【同一个】Rate：下面所有「蒙太奇秒 → 世界秒」的换算
	//   （HitTime 定时器、连段窗口 Open/Close、完美窗口 Open/Close）都是 `蒙太奇秒 / Rate`，
	//   而 Montage_Play 拿的也是这个 Rate。播放用另一个倍率的话，
	//   动画和判定/窗口就会各按各的速度走（见 Stage.PlayRateMultiplier 的注释）。
	//
	// 【蓄力慢放也并进同一个 Rate】（Stage.ChargeTimeDilation，默认 1 = 不慢）。
	//   这是「时间流速」在这套连招里的落点：倍率一变，
	//   动画变慢、命中定时器变晚、连段窗口变长、蒙太奇播完时刻也变晚 —— 全是同一个数推出来的，
	//   不存在「慢放期间命中打在动作前面」这种错位。
	//   ⚠️ 千万别改成「只设 Actor 的 CustomTimeDilation」那套：那只让动画慢，
	//      定时器仍按世界秒跑 ⇒ 命中提前于挥拳那一下（蓄力越明显的段越明显）。
	const float ChargeDilation = FMath::Clamp(Stage.ChargeTimeDilation, 0.05f, 1.f);
	const float Rate = GetAttackPlayRate()
		* FMath::Max(0.1f, Stage.PlayRateMultiplier)
		* (ChargeDilation < 1.f ? ChargeDilation : 1.f);

	// 本段的命中通知订阅。放在 World 检查【之前】—— 提前 return 的那条路上
	// 也必须有 ImpactTask，否则命中永远不触发（而且不会有任何报错，只是「打人不掉血」）。
	RebuildImpactTask(Stage);

	// World 提前取：完美窗口要靠世界时间换算，下面的定时器也要用。
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 三个时刻基准都取同一个 Now，换算也都用同一个 Rate。
	// 各取各的 GetTimeSeconds() 的话，「完美窗口」和「连段窗口」会差几微秒，
	// 两个窗口的边界上就可能出现「缓冲收下了但完美窗口还没开」这种只差一帧的判定。
	const float Now = World->GetTimeSeconds();
	ComputePerfectWindow(Stage, Rate, Now);
	// 判定窗口也挂成标签（和提示光分开排：提示光没配/没渲染时玩法状态照样要挂）。
	SchedulePerfectWindowTags();

	// 连段窗口的绝对时刻：输入缓冲要拿它当基准（见 OnAttackInput）。
	// 换算口径和下面三个定时器完全一致（同一个 Now、同一个 Rate）。
	ChainWindowOpenWorldTime = Now + Stage.ChainWindowOpenTime / Rate;
	ChainWindowCloseWorldTime = Now + Stage.ChainWindowCloseTime / Rate;

	// 强化那一击换蒙太奇：State.EmpoweredAttack 在起手前就挂好了（破隐是同步挂的，完美窗口在 CloseChainWindow 里挂），
	// 所以这里现读就能拿到正确答案。
	// 表现必须由能力在起手这一刻播，不能交给 GE 上的 cue：cue 只在 GE 挂上那一瞬触发一次（可能早于这一击好几秒），
	// 对不上起手时机；何况它播完就被紧随其后的本函数 Montage_Play（bStopAllMontages=true → 按 slot group 清场）顶掉。
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	bStageEmpowered = ASC && ASC->HasMatchingGameplayTag(LOLGameplayTags::State_EmpoweredAttack);
	UAnimMontage* MontageToPlay = (bStageEmpowered && PassiveData->EmpowerMontage) ? PassiveData->EmpowerMontage : Stage.Montage;

	if (bStageEmpowered)
	{
		// 用掉即消耗：强化只够打这一击，起手就把状态摘掉 —— 打没打中都算用掉了（空挥也一样）。
		// 之所以要等到起手才摘，是因为「哪一击是强化的」只有这一刻才定得下来（完美窗口在窗口关闭时才挂上）。
		// 也不能留到命中结算再摘：那期间下一段可能已经排上，会把强化错给到下一段。
		// 两端各自摘各自的（GE 移除不可预测、也不复制），客户端摘自己那份，服务端摘自己那份；
		// 本段的强化与否已经锁在 bStageEmpowered 里，摘掉不影响这一击的伤害/击退判定。
		// Granted 版：State.EmpoweredAttack 是 UTargetTagsGameplayEffectComponent 授的，
		// Asset Tags 里没有它，RemoveActiveEffectsWithTags 会匹配不到 → 强化永远消耗不掉。
		//
		// 还得走 RemoveGrantedTagEffects 而不是 ASC 自带的那个：自带版本在非权威端是静默 no-op
		// （引擎硬门槛），客户端的强化窗口因此一直消耗不掉 —— 本地那份预测副本是在定时器里施加的、
		// 激活预测键早被 ack 过，引擎的 catch-up 清理也不会来收，服务端那份又是同帧挂上又消耗、
		// 标签容器的 net delta 从没变过，于是客户端这份能活满 EmpowerDuration（默认 3s）：
		// 这 3 秒内每次普攻起手都读成强化、播强化蒙太奇，而服务端早就消耗掉了 → 结算其实还是普通攻击。
		// 详见该函数的注释。
		UMyAbilitySystemComponent::RemoveGrantedTagEffects(ASC, FGameplayTagContainer(LOLGameplayTags::State_EmpoweredAttack));
	}

	// 诊断：权威/客户端各起手一次会各打一行，两行对比就知道是哪一端读到的强化状态不对。
	// 顺带打出两个配置值：EmpowerDuration 是「0 就静默不挂强化」的坑；完美窗口是「没配就回退整段连段窗口」
	// 的兜底 —— 一眼能看出这一段到底有没有门槛。
	AActor* Avatar = GetAvatarActorFromActorInfo();
	UE_LOG(LogTemp, Warning, TEXT("[Passive] StartStage %d: 权威=%d 强化=%d EmpowerMontage=%s 本段=%s → 播 %s（配置: EmpowerDuration=%.2f 完美窗口=[%.2f,%.2f] 连段窗口=[%.2f,%.2f]）"),
		NewStage, (Avatar && Avatar->HasAuthority()) ? 1 : 0, bStageEmpowered ? 1 : 0,
		*GetNameSafe(PassiveData->EmpowerMontage), *GetNameSafe(Stage.Montage), *GetNameSafe(MontageToPlay),
		PassiveData->EmpowerDuration, Stage.PerfectWindowOpenTime, Stage.PerfectWindowCloseTime,
		Stage.ChainWindowOpenTime, Stage.ChainWindowCloseTime);

	// 蓄力表现放在蒙太奇【之前】：起手这一帧就该看见拳头上聚起光，
	// 晚一帧就会变成「动画先出、光后冒出来」，是典型的接在播动画之后才 spawn 的观感错。
	StartStageCharge(Stage, ChargeDilation);

	// 播放本段蒙太奇（按攻速倍率变速）。
	if (MontageToPlay)
	{
		if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
		{
			if (UAnimInstance* Anim = Character->GetMesh()->GetAnimInstance())
			{
				Anim->Montage_Play(MontageToPlay, Rate);
			}
		}
	}

	// ======================================================================
	// 【出拳前冲】—— 用位移补「手短」
	//
	// 为什么需要：源角色 Crunch 的上肢比例是 Kallari 的 1.63 倍（手到 spine_01
	// 最远 94.7cm vs 58.0cm，静态骨架比例实测）。重定向把动作搬过来了，
	// 但 Kallari 的手臂【物理上伸不到那么远】⇒ 视觉上「拳头没打出去」。
	// 加位移后拳头的世界位置能到该去的距离，观感上就"打出去了"。
	//
	// 为什么用 LaunchCharacter 而不是 Motion Warping：位移量很小（十几到几十 cm），
	// 一次性的冲量就够，Motion Warping 那套要挂 Notify、要在时间轴上配，
	// 对「每段补一点前冲」这个量级是过度设计。项目闪避（GA_Dodge）也走同一条路。
	//
	// 【为什么只服务端施加】位移和伤害一样必须服务端权威，否则两端各自推一次会翻倍。
	// 客户端靠 bReplicateMovement 看到结果。
	// ======================================================================
	if (Stage.PunchLunge > 0.f)
	{
		if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
		{
			if (Character->HasAuthority())
			{
				// 沿角色**当前朝向**水平前推。bZOverride=false：不改垂直速度，
				// 免得打拳时把角色顶起来或打飞。
				const FVector Dir = Character->GetActorForwardVector().GetSafeNormal2D();
				Character->LaunchCharacter(Dir * Stage.PunchLunge, /*bXYOverride=*/true, /*bZOverride=*/false);

				UE_LOG(LogTemp, Log, TEXT("[Passive] stage %d punch lunge = %.1f cm"), NewStage, Stage.PunchLunge);
			}
		}
	}

	// 挥击音效和动作同帧起。三连击那几个蒙太奇里一个 AnimNotify_PlaySound 都没配，
	// 这里不补的话普攻就是完全没声的；强化那一击换成另一条更重的。
	PlayAttackSound(bStageEmpowered, NewStage);

	// 连段窗口的两个定时器 + 命中时刻。时长都除以攻速倍率（攻速越快，动画和判定都越快）。
	//
	// 命中时刻一律用【服务器端定时器】排，不再依赖「服务器播放蒙太奇、蒙太奇评估 notify」：
	// 动画 notify 只会在「真的评估动画的那台机器」上触发 —— ListenServer 下服务器也播蒙太奇所以能到，
	// 但换成 dedicated server（服务器只跑权威逻辑、mesh 用参考姿势不评估动画）后，Event.Melee.Impact
	// 这条 notify 永远不触发 → 命中结算永远不来、技能卡死且无报错。所以延迟取「蒙太奇上那个 notify 的
	// 时刻」（直接读资产、不依赖动画评估），没配 notify 才回退 Stage.HitTime —— 判定时刻仍然跟着动画帧走。
	//
	// 蒙太奇上的 notify 仍然留着（bServerOnly=true）：ListenServer 下它是和定时器同一时刻的冗余触发，
	// ResolveHit 的 bHitAppliedThisStage 闩保证只结算一次；dedicated server 下它不触发，由定时器兜底。
	// 先 ClearTimer 再排：上一段的 HitTimer 万一还挂着（HitTime 配得比连段窗口关闭还晚时会发生），
	// 落到这一段里会把这一段的命中提前结算掉。
	World->GetTimerManager().ClearTimer(HitTimer);
	// ⚠️⚠️ 探测 notify 时刻必须用【本段自己的 ImpactTag】，不是全局的 MeleeImpactTag。
	//
	// 每段一个标签（Event.Melee.Boxing1~4）时，用 MeleeImpactTag 去探【永远探不到】
	// ⇒ ImpactNotifyTime 恒为 -1 ⇒ 静默回退 Stage.HitTime。
	// 而 HitTime 是手填的蒙太奇秒、未必和 montage 上 notify 的真实时刻一致：
	// 第四段 HitTime=0.18 但 notify 在 0.207，两者都晚于该段连段窗口关闭(0.10)
	// ⇒ ConfirmHit 排在 EndAbility 之后 ⇒ 这一拳永远不结算（症状：单独打第四段打不中，
	// 而前面几段因为窗口长(0.80)所以看不出来）。
	//
	// 先试本段标签，再退回全局标签：段标签没配的老数据（三连普攻 3 段共用
	// Event.Melee.Impact）仍然走原来的路径。
	const FGameplayTag StageImpactTag = Stage.ImpactTag.IsValid() ? Stage.ImpactTag : MeleeImpactTag;
	const float ImpactNotifyTime = ThreeHitGetImpactNotifyTime(MontageToPlay, StageImpactTag);
	const float HitDelaySeconds = ImpactNotifyTime >= 0.f ? ImpactNotifyTime : Stage.HitTime;
	World->GetTimerManager().SetTimer(HitTimer, this, &ThisClass::ConfirmHit, HitDelaySeconds / Rate, false);

	// 本段蒙太奇播完的世界时刻（最后一段的收尾要用：收招不能叠在出拳中途，见 ScheduleLastStageFinish）。
	StageMontageEndWorldTime = MontageToPlay ? Now + MontageToPlay->GetPlayLength() / Rate : 0.f;

	// 诊断：命中时刻的延迟是多少、来源是 notify 时刻还是 HitTime。
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段命中时刻: %s（蒙太奇=%s %s 通知，延迟=%.3fs）"),
		StageIndex,
		ImpactNotifyTime >= 0.f ? TEXT("notify 时刻(定时器)") : TEXT("HitTime 定时器"),
		*GetNameSafe(MontageToPlay),
		ImpactNotifyTime >= 0.f ? TEXT("有") : TEXT("没有"),
		HitDelaySeconds / Rate);

	// ⚠️⚠️ 连段窗口的两个定时器：【时间 <= 0 时不能调 SetTimer】——
	//   UE 的 SetTimer 传 <= 0 的语义是【清除该定时器】，不是「立刻触发」。
	//   所以「这一段不配连段窗口」（ChainWindowCloseTime = 0，最后一段常用这个配置）
	//   会让 CloseChainWindow 永远不被调用 ⇒ 能力永不 EndAbility ⇒
	//   InstancedPerActor 的实例永久 active ⇒ 之后每次 TryActivateAbility 都返回 0。
	//   症状：连招打得完，但【打满最后一段之后就再也打不出来】（2026-10-03 实测）。
	//
	// 「不配窗口」的正确语义是【这一段打完立刻收招】，所以这里直接调 CloseChainWindow，
	// 不去排那个注定被清除的定时器。
	if (Stage.ChainWindowCloseTime <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogTemp, Log, TEXT("[Passive] stage %d has no chain window (CloseTime=0) -> close now"), StageIndex);
		OpenChainWindow();   // 置 bWindowOpen=true 并放开起手那一按的吞闩（顺序上等价于窗口开了再关）
		CloseChainWindow();
		return;
	}

	World->GetTimerManager().SetTimer(OpenTimer, this, &ThisClass::OpenChainWindow, Stage.ChainWindowOpenTime / Rate, false);   // 连段窗口开启
	World->GetTimerManager().SetTimer(CloseTimer, this, &ThisClass::CloseChainWindow, Stage.ChainWindowCloseTime / Rate, false); // 连段窗口关闭

	// 完美窗口的提示光：本段没有完美窗口（只有第 2 段有）时这个函数自己会退出去。
	SchedulePerfectWindowVFX();
}

void UGA_ThreeHitPassive::ComputePerfectWindow(const FThreeHitAttackStage& Stage, float Rate, float Now)
{
	PerfectWindowOpenWorldTime = 0.f;
	PerfectWindowCloseWorldTime = 0.f;

	// 最后一段没有「下一段」可以武装，勾了也没有意义。
	if (!Stage.bPerfectWindowEnablesNextHitKnockback || !PassiveData->Stages.IsValidIndex(StageIndex + 1))
	{
		return;
	}

	float OpenSeconds = Stage.PerfectWindowOpenTime;
	float CloseSeconds = Stage.PerfectWindowCloseTime;

	// 没配（两个都留 0）或配反了 → 回退成整段连段窗口，也就是加完美窗口之前的老行为。
	// 不静默：这是最容易「改了 DS_Passive 却看不出区别」的地方，日志里必须能看见。
	if (CloseSeconds <= OpenSeconds)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段完美窗口没配（开=%.2f 关=%.2f）→ 回退成整段连段窗口 [%.2f, %.2f]"),
			StageIndex, OpenSeconds, CloseSeconds, Stage.ChainWindowOpenTime, Stage.ChainWindowCloseTime);
		OpenSeconds = Stage.ChainWindowOpenTime;
		CloseSeconds = Stage.ChainWindowCloseTime;
	}

	// 只取和连段窗口的交集：连段窗口开启前的输入会被整段丢掉，关闭后也不再收（见 OnAttackInput）。
	const float EffectiveOpen = FMath::Max(OpenSeconds, Stage.ChainWindowOpenTime);
	const float EffectiveClose = FMath::Min(CloseSeconds, Stage.ChainWindowCloseTime);
	if (EffectiveClose <= EffectiveOpen)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段完美窗口 [%.2f, %.2f] 和连段窗口 [%.2f, %.2f] 没有交集 → 本段不会武装强化，只能打出普通下一段"),
			StageIndex, OpenSeconds, CloseSeconds, Stage.ChainWindowOpenTime, Stage.ChainWindowCloseTime);
		return;
	}

	// 蒙太奇秒 → 世界秒：和三个定时器同一套换算（除以攻速倍率）。
	// Now 由 StartStage 传进来（不是这里现取）：本段的连段窗口基准也要用同一个时刻，
	// 各取各的话两个窗口的边界会差几微秒，缓冲和完美窗口在边界上就会各判各的。
	PerfectWindowOpenWorldTime = Now + EffectiveOpen / Rate;
	PerfectWindowCloseWorldTime = Now + EffectiveClose / Rate;
}

void UGA_ThreeHitPassive::SetComboWindowTag(bool bOpen)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!ASC)
	{
		return;
	}

	// 幂等，所以没有任何"当前是不是已经挂了"的判断 —— 那些判断自己就是 bug 的来源
	//（少摘一次 = 标签永久残留，AI 会以为窗口一直开着）。
	if (bOpen)
	{
		ASC->AddLooseGameplayTag(LOLGameplayTags::State_ComboWindow);
	}
	else
	{
		ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_ComboWindow);
	}
}

void UGA_ThreeHitPassive::SetPerfectWindowTags(bool bArmed, bool bOpen)
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!ASC)
	{
		return;
	}

	// 和 SetComboWindowTag 一样：幂等，所以不判断"现在挂没挂"，无条件挂/摘。
	const FGameplayTag ArmedTag = LOLGameplayTags::State_PerfectWindowArmed;
	const FGameplayTag OpenTag = LOLGameplayTags::State_PerfectWindow;

	if (bArmed)
	{
		ASC->AddLooseGameplayTag(ArmedTag);
	}
	else
	{
		ASC->RemoveLooseGameplayTag(ArmedTag);
	}

	if (bOpen)
	{
		ASC->AddLooseGameplayTag(OpenTag);
	}
	else
	{
		ASC->RemoveLooseGameplayTag(OpenTag);
	}
}

void UGA_ThreeHitPassive::OpenPerfectWindow()
{
	SetPerfectWindowTags(/*bArmed=*/true, /*bOpen=*/true);
}

void UGA_ThreeHitPassive::ClosePerfectWindow()
{
	SetPerfectWindowTags(/*bArmed=*/false, /*bOpen=*/false);
}

void UGA_ThreeHitPassive::SchedulePerfectWindowTags()
{
	// 先把上一段可能还挂着的摘掉（StartStage 的复位块也会摘一次，这里兜住
	// "上一段的关闭定时器还没到就被新一段覆盖"的情况）。
	SetPerfectWindowTags(/*bArmed=*/false, /*bOpen=*/false);

	UWorld* World = GetWorld();
	if (!World || GetPerfectWindowDuration() <= 0.f)
	{
		return;
	}

	// Armed 从本段起手就挂上（不是等窗口开）：AI 要在连段窗口一开就知道"这段别急着按"。
	// 没有它的话，AI 只会在"窗口真的开了"之后才等 —— 那时候它已经按完了。
	SetPerfectWindowTags(/*bArmed=*/true, /*bOpen=*/false);

	FTimerManager& Timers = World->GetTimerManager();
	const float Now = World->GetTimeSeconds();

	// 先清再排：完美窗口的关闭时刻和连段窗口的关闭时刻【可以重合】（没配时间时
	// ComputePerfectWindow 会回退成整段连段窗口），两者同帧到点时谁先跑不确定 ——
	// 上一段那个还没跑的关闭定时器万一落在 StartStage 之后，就会把新一段刚挂上的标签摘掉。
	Timers.ClearTimer(PerfectTagOpenTimer);
	Timers.ClearTimer(PerfectTagCloseTimer);

	Timers.SetTimer(PerfectTagOpenTimer, this, &ThisClass::OpenPerfectWindow,
		FMath::Max(PerfectWindowOpenWorldTime - Now, 0.f), /*bLoop=*/false);
	Timers.SetTimer(PerfectTagCloseTimer, this, &ThisClass::ClosePerfectWindow,
		FMath::Max(PerfectWindowCloseWorldTime - Now, 0.f), /*bLoop=*/false);
}

void UGA_ThreeHitPassive::OpenChainWindow()
{
	bWindowOpen = true;
	SetComboWindowTag(true);

	// 兜底：窗口都开了，起手那一按要么早被吞掉、要么根本没到（丢包/事件没发）。
	// 这时候不能再吞，否则吞的就是玩家真的按键。
	bSwallowOpeningInput = false;
}

void UGA_ThreeHitPassive::CloseChainWindow()
{
	bWindowOpen = false;
	SetComboWindowTag(false);

	const FThreeHitAttackStage& Stage = PassiveData->Stages[StageIndex];

	// 诊断：完美窗口那条路径是否武装强化，全看这一次输入有没有踩中完美窗口。
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段窗口关闭: 已排队=%d 完美窗口标记=%d 完美=%d"),
		StageIndex, bQueuedNextStage ? 1 : 0, Stage.bPerfectWindowEnablesNextHitKnockback ? 1 : 0, bPerfectQueued ? 1 : 0);

	// 没排队下一段，或已经打满最后一段 → 结束技能。
	if (!bQueuedNextStage || StageIndex >= PassiveData->Stages.Num() - 1)
	{
		if (StageIndex >= PassiveData->Stages.Num() - 1)
		{
			// 最后一段：收尾推迟到「命中结算完 + 本段蒙太奇播完」。
			// 不能在这里立刻 EndAbility —— 窗口关闭时刻可能早于命中时刻
			//（最后一段的 ChainWindowCloseTime 故意配得很小），立刻结束会：
			//   ① ClearTimer(HitTimer) 把还没落地的命中静默吞掉（打不出最后一击）；
			//   ② 收招叠在出拳中途播（同 slot 并发 → 画面错乱）；
			//   ③ 玩家再按攻击会立刻重开新连招，顶掉还在播的最后一拳。
			ScheduleLastStageFinish();
			return;
		}

		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
		return;
	}

	// 这一次输入踩中了完美窗口 → 下一段是强化普攻；没踩中就是普通下一段（连段照常推进，不会断）。
	// 「这一段有没有完美窗口」已经在 ComputePerfectWindow 里判过了（没窗口时 bPerfectQueued 恒为 false），
	// 这里不用再看 bPerfectWindowEnablesNextHitKnockback。
	// 走和破隐同一套机制（挂 State.EmpoweredAttack），数值也共用 UThreeHitPassiveData 上的 Empower*，
	// 不再各配一份击退参数。
	if (bPerfectQueued)
	{
		GrantEmpoweredAttack();
	}

	StartStage(StageIndex + 1);
}

void UGA_ThreeHitPassive::ScheduleLastStageFinish()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		// 理论上到不了（CloseChainWindow 只在有 World 的路径上被调）——兜底直接收。
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
		return;
	}

	const float Now = World->GetTimeSeconds();
	float Delay = 0.f;

	// ① 命中还没结算（命中时刻晚于窗口关闭，最后一段的常态）→ 等它落地。
	//    否则下面 EndAbility 里 ClearTimer(HitTimer) 会把这一击静默吞掉。
	if (World->GetTimerManager().IsTimerActive(HitTimer))
	{
		Delay = FMath::Max(Delay, World->GetTimerManager().GetTimerRemaining(HitTimer) + 0.02f);
	}

	// ② 等本段蒙太奇播完再收招：两条蒙太奇在同一个 slot 上并发 =
	//    收招把还没演完的最后一拳盖掉 —— 出招画面错乱的另一半根因。
	if (StageMontageEndWorldTime > Now)
	{
		Delay = FMath::Max(Delay, StageMontageEndWorldTime - Now);
	}

	if (Delay <= KINDA_SMALL_NUMBER)
	{
		FinishLastStage();
		return;
	}

	World->GetTimerManager().SetTimer(FinishTimer, this, &ThisClass::FinishLastStage, Delay, /*bLoop=*/false);
}

void UGA_ThreeHitPassive::FinishLastStage()
{
	// 等待收尾的这段时间里可能被打死 —— 死亡蒙太奇不该被收招盖住。
	// 死亡时只收能力，不放收招（StartStage 的死亡门管不到这里，这里要自己判）。
	const AHeroCombatCharacter* Hero = Cast<AHeroCombatCharacter>(GetAvatarActorFromActorInfo());
	const bool bDead = Hero && Hero->IsDead();

	if (!bDead)
	{
		PlayRecoveryMontage();
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}

void UGA_ThreeHitPassive::GrantEmpoweredAttack()
{
	if (PassiveData)
	{
		// 施加逻辑和破隐共用，见 UMyGameplayAbility::ApplyEmpoweredAttack。
		ApplyEmpoweredAttack(EmpoweredAttackGE, PassiveData->EmpowerDuration);
	}
}

// =============================================================================
// 本段起手的蓄力表现（Stage.Charge*）
// =============================================================================

void UGA_ThreeHitPassive::StartStageCharge(const FThreeHitAttackStage& Stage, float TimeDilation)
{
	// 上一段的蓄力光可能还没到点（ChargeDuration 比这一段的起手还长），先收掉，
	// 免得两段的光叠在同一只手上。
	StopChargeVFX();

	// 没配粒子是【正常情况】（只想要慢放 / 只想要镜头也可以）：这里静默返回，
	// 不打 warning —— 否则四段普攻里三段都会刷「没配蓄力特效」，真正的配错了会被淹掉。
	UNiagaraSystem* System = Stage.ChargeSystem.LoadSynchronous();
	if (!System)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		// 专用服务器没有渲染，一个粒子都不生成。和 SchedulePerfectWindowVFX 同一套判断。
		return;
	}

	USkeletalMeshComponent* Mesh = GetAvatarMesh();
	if (!Mesh)
	{
		return;
	}

	// 插槽不存在时 SpawnSystemAttached 会静默挂在网格原点 —— 光会从肚子里冒出来，看不出是配错了。
	// 骨骼名同样算数（ThreeHitHasSocket 和 UAnimNotifyState_BladeTrail 里那个是同一份判断）。
	if (!ThreeHitHasSocket(Mesh, Stage.ChargeSocket))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 蓄力特效：%s 上找不到插槽 %s → 挂在网格原点"),
			*GetNameSafe(Mesh->GetSkeletalMeshAsset()), *Stage.ChargeSocket.ToString());
	}

	// 挂到【插槽】而不是世界坐标：蓄力是「能量聚到拳头上」，角色被推/被击退时
	// 世界坐标的光会糊在原地不动，和手脱节。
	UNiagaraComponent* NC = UNiagaraFunctionLibrary::SpawnSystemAttached(
		System, Mesh, Stage.ChargeSocket, FVector::ZeroVector, FRotator::ZeroRotator,
		EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/true);
	if (!NC)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 蓄力特效 %s 生成失败（软引用加载成功但 spawn 失败）"),
			*GetNameSafe(System));
		return;
	}

	// 粒子跟着一起慢（和本段 Rate 用同一个数）。
	// ⚠️ 只对 Age Update Mode = Tick Delta Time（默认）的 NS 有效：DesiredAge 模式下
	//    CustomTimeDilation 不参与推进，设了也没反应（NS_PerfectWindow 那条注释是同一个坑）。
	if (Stage.bChargeSystemSlowsWithTime && TimeDilation < 1.f)
	{
		NC->SetCustomTimeDilation(TimeDilation);
	}

	ChargeVFX = NC;

	// 到点自动收：auto destroy 对「无限循环 + 不 auto destroy 的 emitter」永远等不到，
	// 光会一直烧在手上。挂个一次性定时器强拆，不管 NS 怎么配都不会累积。
	// ChargeDuration <= 0 时排一个 0.3s 的兜底（StopChargeVFX 是幂等的，多一层兜底不亏）。
	//
	// 时长要按慢放折算：这里填的是【世界秒】，而本段动画因为 Rate 乘了 TimeDilation 变长了，
	// 不除回去的话 0.45s 的世界秒在 0.5 倍速下只覆盖动画的前 0.22s —— 光提前灭掉、蓄力演到一半就没了。
	// 资产上填的数因此保持「动画看起来有多长」这个直觉，不用去心算慢放倍率。
	const float StopDelay = Stage.ChargeDuration > 0.f
		? Stage.ChargeDuration / (TimeDilation > 0.f ? TimeDilation : 1.f)
		: 0.3f;
	World->GetTimerManager().SetTimer(ChargeVFXStopTimer, this, &ThisClass::StopChargeVFX, StopDelay, /*bLoop=*/false);

	// 镜头：起手那一下要「看向自己蓄力」，所以只震控制者本人那台机器 ——
	// 和 GC_DeathHarvestBurst 用 ClientStartCameraShake 同一个理由（不是全队共享的表现）。
	if (UCameraShakeBase* Shake = Stage.ChargeCameraShake.LoadSynchronous())
	{
		if (AActor* Avatar = GetAvatarActorFromActorInfo())
		{
			if (APlayerController* PC = Cast<APlayerController>(Avatar->GetInstigatorController()))
			{
				PC->ClientStartCameraShake(Shake->GetClass(), Stage.ChargeCameraShakeScale);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Passive] 蓄力镜头没播：%s 的控制者不是 APlayerController"),
					*GetNameSafe(Avatar));
			}
		}
	}
}

void UGA_ThreeHitPassive::StopChargeVFX()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChargeVFXStopTimer);
	}

	if (!ChargeVFX)
	{
		return;
	}

	// 先停发射再销毁：已经生成的那一段按 NS 自己的寿命自然收掉，比一刀切好看，
	// 也避开「auto destroy 组件被手动 Destroy 两次」这类重复销毁路径。
	ChargeVFX->Deactivate();
	ChargeVFX->SetAutoDestroy(true);

	TWeakObjectPtr<UNiagaraComponent> WeakComponent(ChargeVFX);
	FTimerHandle TeardownTimer;
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(TeardownTimer, FTimerDelegate::CreateWeakLambda(ChargeVFX.Get(), [WeakComponent]()
		{
			if (UNiagaraComponent* StillAlive = WeakComponent.Get())
			{
				StillAlive->DestroyComponent();
			}
		}), 0.3f, /*bLoop=*/false);
	}
	else if (UNiagaraComponent* NC = WeakComponent.Get())
	{
		NC->DestroyComponent();
	}

	ChargeVFX = nullptr;
}

void UGA_ThreeHitPassive::RebuildImpactTask(const FThreeHitAttackStage& Stage)
{
	// 上一段的订阅先收掉。不收也不会出错（OnAnimImpact 有 bHitAppliedThisStage 闩兜底），
	// 但会白留一个还在跑的 WaitGameplayEvent 到 EndAbility 才清 —— 四段就是四个空转对象。
	if (ImpactTask)
	{
		ImpactTask->EndTask();
		ImpactTask = nullptr;
	}

	// 段标签优先；留空回退到能力上的全局标签（三连普攻的旧行为：3 段共用一个 Event.Melee.Impact）。
	const FGameplayTag Tag = Stage.ImpactTag.IsValid() ? Stage.ImpactTag : MeleeImpactTag;

	if (!Tag.IsValid())
	{
		// 两个来源都没配 = 这一刀既没有通知也没有兜底标签。
		// 不会崩，但玩家会看到「挥到了却不掉血」。所以明确喊出来。
		UE_LOG(LogTemp, Warning,
			TEXT("[Passive] 第%d段既没配 Stage.ImpactTag、能力上的 MeleeImpactTag 也无效 → 本段不订阅命中通知（退回 HitTime 定时器兜底）"),
			StageIndex);
		return;
	}

	// 最后一个参数 bOnlyMatchExact=true：只认精确匹配的标签，
	// 否则父标签（例如 Event.Melee）会顺带把子标签的事件也吃掉。
	ImpactTask = UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, Tag, nullptr, false, true);
	ImpactTask->EventReceived.AddDynamic(this, &ThisClass::OnAnimImpact);
	ImpactTask->ReadyForActivation();

	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段命中通知订阅 = %s（%s）"),
		StageIndex, *Tag.ToString(),
		Stage.ImpactTag.IsValid() ? TEXT("段内配置") : TEXT("全局兜底"));
}

void UGA_ThreeHitPassive::PlayRecoveryMontage()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	ACharacter* Char = Cast<ACharacter>(Avatar);
	if (!Char || !PassiveData || !PassiveData->RecoveryMontage)
	{
		return;
	}

	// ⚠️⚠️ 这里【不能】用 ACharacter::PlayAnimMontage —— 它的签名只有 3 个参数
	//   (Montage, Rate, StartSection)，没有 bStopAllMontages；而且它内部固定按
	//   bStopAllMontages=true 去调 UAnimInstance::Montage_Play ⇒ 无法关掉「停掉所有其他蒙太奇」。
	//   【5 参那个版本是 UAnimInstance::Montage_Play，不是 ACharacter::PlayAnimMontage】——
	 //   项目记忆里那条「第 5 参 bStopAllMontages 默认 true」的记录说的是 Montage_Play，别记混。
	// 要 bStopAllMontages=false 就必须自己拿 AnimInstance 调。
	//
	// 传 false 的两个理由：
	//   ① 收招是「接着这一招往下演」，把刚播完的攻击段停掉没有意义；
	//   ② 死亡/硬控那组蒙太奇（DefaultSlot）可能正在播 —— 收招不该把它们顶掉。
	USkeletalMeshComponent* Mesh = Char->GetMesh();
	UAnimInstance* AnimInst = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (!AnimInst)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] %s 收招失败：角色没有 AnimInstance"), *GetName());
		return;
	}

	const float Rate = GetAttackPlayRate();
	const float Length = AnimInst->Montage_Play(PassiveData->RecoveryMontage, Rate,
		EMontagePlayReturnType::MontageLength, /*InTimeToStartMontageAt=*/0.f,
		/*bStopAllMontages=*/false);

	UE_LOG(LogTemp, Log, TEXT("[Passive] 最后一段打完 → 放收招 %s（速率=%.2f 长度=%.2fs，权威=%d）"),
		*PassiveData->RecoveryMontage->GetName(), Rate, Length,
		(Avatar && Avatar->HasAuthority()) ? 1 : 0);
}

void UGA_ThreeHitPassive::ConfirmHit()
{
	// HitTime 定时器这条：蒙太奇上没配 Event.Melee.Impact 通知时的兜底（行为和加通知之前一样）。
	ResolveHit(/*bFromAnimNotify=*/false);
}

void UGA_ThreeHitPassive::OnAnimImpact(FGameplayEventData Payload)
{
	// 蒙太奇上的命中通知。判定时刻因此跟着【动画帧】走，而不是跟着 Stage.HitTime 那个数字走 ——
	// 换蒙太奇/改蒙太奇里的节奏/改攻速时不用再回头同步一个手填的时间。
	ResolveHit(/*bFromAnimNotify=*/true);
}

void UGA_ThreeHitPassive::ResolveHit(bool bFromAnimNotify)
{
	// 一段只结算一次。正常情况下这一段只会有【一个】来源（StartStage 里按蒙太奇上有没有通知定的），
	// 闩是给「一条蒙太奇上放了多个同类通知」和重复触发兜底的。
	// 放在权威检查之前：客户端也照样锁（客户端本来就不结算，锁上只是让两端状态一致）。
	if (bHitAppliedThisStage)
	{
		return;
	}
	bHitAppliedThisStage = true;

	const AActor* Avatar = GetAvatarActorFromActorInfo();

	// 诊断：这一段的命中时刻到底是谁给的，一行就能看出来。
	// 「来源=HitTime 定时器」而蒙太奇上明明加了通知 = 通知加在了别的蒙太奇上，或者标签不是 Event.Melee.Impact
	// （StartStage 那一行会打出本段播的到底是哪个蒙太奇，对着看就知道）。
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段命中结算: 权威=%d 来源=%s"),
		StageIndex, (Avatar && Avatar->HasAuthority()) ? 1 : 0,
		bFromAnimNotify ? TEXT("动画通知") : TEXT("HitTime 定时器"));

	// 只有服务端权威才结算伤害（客户端只负责预测表现）。
	if (GetAvatarActorFromActorInfo()->HasAuthority())
	{
		ApplyServerHit(PassiveData->Stages[StageIndex]);
	}
}

void UGA_ThreeHitPassive::ApplyServerHit(const FThreeHitAttackStage& Stage)
{
	AActor* Source = GetAvatarActorFromActorInfo();
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (!Source || !SourceASC)
	{
		return;
	}

	// 这一击是不是强化普攻：起手时锁下来的结论（见 StartStage），不重读 State.EmpoweredAttack ——
	// 强化在起手那一刻就已经消耗掉了，现在读必然是空的。
	// 只看状态不看来源：破隐（GA_Stealth）/完美窗口（CloseChainWindow）共用同一套数值，这里不需要分情况。
	const bool bEmpowered = bStageEmpowered;

	// 1) 球形扫描：从角色位置沿【瞄准方向】前扫 TraceDistance 距离、半径 TraceRadius。
	//
	// 【为什么不是 GetActorForwardVector()】网格是 bOrientRotationToMovement，朝移动方向、
	// 和鼠标无关；沿它扫的话玩家对着准心挥空、背对目标反而打中。取控制旋转才是准心的方向。
	// 这里不带 Pitch（近战挥砍是水平扫，跟着仰角会砍进地里）。
	//
	// 【为什么用 ObjectType 而不是 Channel】Channel(ECC_Pawn) 走「对方 profile 对 Pawn 通道的响应」，
	// 而 CharacterMesh profile 对 Pawn 是 Ignore —— 打角色时只会命中胶囊、打不到模型（这是想要的）；
	// 但同样道理，地形若用了「忽略 Pawn 通道」的 profile 也会被漏掉。ObjectType 是显式点名单，
	// 这里点名 Pawn（角色胶囊）+ WorldStatic（地形/墙体），二者都进命中结果。角色身上的 CharacterMesh
	// 也是 Pawn 类型会被扫到，下面循环里对「角色的 SkeletalMesh 命中」跳过，只认胶囊那一击。
	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ThreeHitBasicAttack), false, Source);
	FCollisionShape Shape = FCollisionShape::MakeSphere(PassiveData->TraceRadius);
	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_Pawn);
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	const FVector Start = Source->GetActorLocation();
	const FVector End = Start + AHeroCombatCharacter::ResolveAimDirection(Source, /*bIncludePitch=*/false) * PassiveData->TraceDistance;
	if (!GetWorld()->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, ObjectParams, Shape, Params))
	{
		// 空挥：不再有任何额外处理 —— 强化在起手时已经消耗掉了，空挥同样是「用掉了一次」。
		return;
	}

	// 2) 本段的攻击力倍率（强化再乘一档）。
	//    ⚠️ 这里【只算倍率，不算伤害】：攻击者的面板攻击力由 UExecCalc_Damage 自己捕获，
	//    在这里再乘一遍就等于伤害公式有两份真相，改一处漏一处。
	const float DamageMultiplier = Stage.DamageMultiplier
		* (bEmpowered ? PassiveData->EmpowerDamageMultiplier : 1.f);

	// 3) 对每个命中的目标施加伤害。
	// bLandedAny：本段有没有真的打到东西（角色的模型命中不算）。给下面的顿帧用 ——
	// 空挥（扫到的全是自己的 mesh / 装饰物）不该把全场慢下来。
	bool bLandedAny = false;
	for (const FHitResult& Hit : Hits)
	{
		AActor* Target = Hit.GetActor();
		if (!Target)
		{
			continue;
		}

		// ObjectType 查询会同时扫到角色的胶囊和模型（都是 Pawn 类型）。模型的命中位置是「贴图表面」
		// 而不是逻辑身体，留着会造成同一角色双重命中 + 命中点漂移。角色的 SkeletalMesh 命中直接跳过，
		// 只认胶囊那一击 —— 地形（WorldStatic）没有这层问题，照常走后面的 Target.Terrain 分支。
		if (Cast<ACharacter>(Target) && Cast<USkeletalMeshComponent>(Hit.Component))
		{
			continue;
		}

		// 真打到目标了（上面把「角色的模型命中」滤掉了）→ 记一笔，供本段结算后的顿帧判定。
		bLandedAny = true;

		// 打实了 → 命中点的基础打击表现（粒子 + 打击音 + 本人镜头振动），走 GameplayCue.MeleeHit。
		// 每一击都有：加这条之前，普攻命中在攻击者这一端是【完全没有反馈】的
		// （只有强化那一击有 cue）—— 打中了和打空了看起来一模一样，这是手感差的主因之一。
		// 本段的打击分量一并传进去（Stage.HitCueWeight）：蓄力拳靠它把环和震屏放大。
		ExecuteMeleeHitCue(Hit, Stage.HitCueWeight, StageIndex);

		// 强化那一击在此之上再叠一层额外效果。同样走 cue 而不是就地 Spawn：这个函数只在服务端跑，
		// 直接生成的粒子只有主机看得到（GC_ThrowDaggerHit 修掉的是同一个坑）。
		if (bEmpowered)
		{
			ExecuteEmpoweredHitCue(Hit);
		}

		UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);

		// 只走 GAS 一条路：伤害统一由 GE_Damage + UExecCalc_Damage 结算（见方案文档 §1.2）。
		// 原来那条「没有 ASC 就退回 ApplyPointDamage」的分支【已删除】：留着它就等于开了一条
		// 绕过 ExecCalc 的路，格挡对那条路上的伤害完全失效，而且是静默失效（没有日志、没有断言）。
		// 代价：没挂 ASC 的 actor（比如纯地形/装饰物）打中了只有命中表现、没有伤害 ——
		// 想让某个东西吃伤害，正确做法是给它挂 ASC + 属性集，而不是把回退分支留着。
		if (!TargetASC || !PassiveData->DamageEffect)
		{
			UE_LOG(LogTemp, Warning, TEXT("[Melee] 目标 %s 没有 ASC（或 DamageEffect 没配）→ 本次命中无伤害"),
				*GetNameSafe(Target));
		}
		else
		{
			FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
			Context.AddHitResult(Hit);
			// MakeEffectContext 已经把 instigator 设成施加者（PlayerState + 角色），
			// ExecCalc 的攻击力捕获和格挡的来源判定都靠它，不用再手动 AddInstigator。

			FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(PassiveData->DamageEffect, GetAbilityLevel(), Context);
			if (Spec.IsValid())
			{
				Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, DamageMultiplier);
				Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, 0.f);
				// 近战是物理伤害。不挂类型标签时 ExecCalc 也按物理算，这里显式挂上是为了
				// 以后加「魔法伤害的近战技能」时不用回头猜默认值。
				Spec.Data->AddDynamicAssetTag(LOLGameplayTags::Damage_Physical);
				// 普攻（含强化那一击）打上普攻标签：生命偷取只认它（见 ExecCalc_Damage ⑤）。
				// 不借 Data.CanCrit 的理由见 LOLGameplayTags.h 里 Data.BasicAttack 那段。
				Spec.Data->AddDynamicAssetTag(LOLGameplayTags::Data_BasicAttack);
				// 普攻（含强化那一击）可暴击。技能/匕首/死亡收割【不设】这一条 —— 和 LoL 一致：
				// 能不能暴击是「哪个伤害点」的属性，不是「什么伤害类型」的属性。
				// 判定读的是攻击者的 CritChance / CritDamage（见 UExecCalc_Damage ①c）。
				Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_CanCrit, 1.f);

				SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
			}
			else
			{
				// 不静默：Spec 无效 = 这一击一点伤害都没有，屏幕上看起来像「打空了」。
				UE_LOG(LogTemp, Warning, TEXT("[Melee] DamageEffect(%s) 的 Spec 无效 → 本次命中无伤害"),
					*GetNameSafe(PassiveData->DamageEffect));
			}
		}

		// 每一击都带的纯冲量击退（UGE_KnockbackImpulse：只推一下，不带 State.Knockback 硬控）。
		//
		// 【为什么放在强化判定之外】空手四连拳每段都有 PunchLunge 前冲，按手感每一击都该把人推开一点；
		// 但用 UGE_Knockback（硬控）的话平 A 就自带软控，连打四段等于连控四次，太强。
		// 所以普攻推力走「无标签的 Instant 冲量」，强化那一击要真控制时再走下面的 GE。
		//
		// 两个冲量都为 0 时组件内部直接返回（见 UGEComponent_Knockback），所以老资产
		// （HitImpulse=0）即便 CDO 默认挂了 GE 也什么都不发生。
		//
		// 数值解析（段级优先，留 0 / 留空才回退 Data 资产上的全局一份）：
		//   Stage.HitImpulseGE / Stage.HitImpulse / Stage.HitLaunch —— 本段自己的（四连拳就是靠这个
		//   「每段推多远不同」）；留空/0 才用 PassiveData->HitImpulse*（老资产不用重填）。
		// 段和 Data 两边都留空 = 这一击不推人，包括 CDO 默认挂着的 HitImpulseGE —— 组件自己会挡掉，不吵。
		//
		// ⚠️ 类型必须是 TSubclassOf<UGameplayEffect>：TSubclassOf 只能隐式转成
		//   UGameplayEffect*（裸指针），写成 const UGameplayEffect* 是 C2440。
		const TSubclassOf<UGameplayEffect> ImpulseGE = Stage.HitImpulseGE ? Stage.HitImpulseGE : PassiveData->HitImpulseGE;
		const float ResolvedImpulse = Stage.HitImpulse > 0.f ? Stage.HitImpulse : PassiveData->HitImpulse;
		const float ResolvedLaunch = Stage.HitLaunch > 0.f ? Stage.HitLaunch : PassiveData->HitLaunch;

		if (ImpulseGE && TargetASC && (ResolvedImpulse > 0.f || ResolvedLaunch > 0.f))
		{
			FGameplayEffectContextHandle ImpulseContext = SourceASC->MakeEffectContext();
			// 必须把 Hit 塞进 Context：UGEComponent_Knockback 的四级方向回退里前两级
			// （命中法线、目标-施加者）全靠它，不塞就只剩「施加者朝向」和「目标朝向」两级，
			// 侧面/背身打时会往奇怪的方向推。
			ImpulseContext.AddHitResult(Hit);

			FGameplayEffectSpecHandle ImpulseSpec = SourceASC->MakeOutgoingSpec(ImpulseGE, GetAbilityLevel(), ImpulseContext);
			if (ImpulseSpec.IsValid())
			{
				ImpulseSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackImpulse, ResolvedImpulse);
				ImpulseSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackLaunch, ResolvedLaunch);
				SourceASC->ApplyGameplayEffectSpecToTarget(*ImpulseSpec.Data.Get(), TargetASC);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Melee] HitImpulseGE(%s) 的 Spec 无效 → 本次命中不推人"),
					*GetNameSafe(ImpulseGE));
			}
		}

		// 本段的【眩晕】：Stage 上配了 StunGE + StunDuration 才生效（默认两个都空 = 不眩晕，
		// 平 A 不该每段都把人定住）。用「第三段的 Stage 上勾一下」代替在代码里写死 StageIndex==2 ——
		// 段序是资产里的一行配置，硬编码段号的话改段数 / 换个 Data 资产复用就得回头动 C++。
		//
		// UGE_Stun 自己从 SetByCaller Data.ControlDuration 读时长（见 GE_Stun.cpp），这里只负责把数喂进去。
		// 走的是同一个 TargetASC 判定，没 ASC 的目标（地形）自然不会拿到眩晕。
		if (TargetASC && Stage.StunGE && Stage.StunDuration > 0.f)
		{
			FGameplayEffectContextHandle StunContext = SourceASC->MakeEffectContext();
			// 即便眩晕用不上方向，也把 Hit 塞进 Context：EffectContext 里没有命中结果时，
			// ExecCalc 里的暴击/格挡判定和这条 GE 的来源记录都会退化（见上面冲量那段的同一条纪律）。
			StunContext.AddHitResult(Hit);

			FGameplayEffectSpecHandle StunSpec = SourceASC->MakeOutgoingSpec(Stage.StunGE, GetAbilityLevel(), StunContext);
			if (StunSpec.IsValid())
			{
				StunSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_ControlDuration, Stage.StunDuration);
				SourceASC->ApplyGameplayEffectSpecToTarget(*StunSpec.Data.Get(), TargetASC);

				UE_LOG(LogTemp, Log, TEXT("[Melee] 第%d段眩晕 %s：%.2fs"),
					StageIndex, *GetNameSafe(Target), Stage.StunDuration);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("[Melee] Stage.StunGE(%s) 的 Spec 无效 → 本次命中不眩晕"),
					*GetNameSafe(Stage.StunGE));
			}
		}

		// 强化那一击的控制：每个 Data 资产自己选击退还是击飞（见 UThreeHitPassiveData::EmpowerControlGE）。
		//
		// 【击飞路径】EmpowerControlGE 配了（拳击资产配 UGE_KnockUp）：
		//   挂 GE（State.KnockUp 硬控到落地 + 升空蒙太奇）→ 施加方自己 LaunchCharacter 补上抛 ——
		//   UGE_KnockUp 没有位移组件（见 GE_KnockUp.h），和 SpinSlash::LaunchTarget 同一条纪律。
		//   上抛用 bXYOverride=false：水平动量保留（击退过的目标边飞边升才自然）。
		//
		// 【击退路径（旧行为）】EmpowerControlGE 留空：施加 GE_Knockback（硬直 + 击退蒙太奇 + 位移）。
		//   位移全在 GE 组件里（读 Data.KnockbackImpulse / Data.KnockbackLaunch），
		//   方向由 EffectContext 的 HitResult 法线决定（命中面朝外），顺带修掉了原来「击退用 GetActorForwardVector、
		//   扫掠用准心」的方向不同源问题。
		if (bEmpowered && TargetASC)
		{
			FGameplayEffectContextHandle ControlContext = SourceASC->MakeEffectContext();
			ControlContext.AddHitResult(Hit);

			if (PassiveData->EmpowerControlGE)
			{
				FGameplayEffectSpecHandle UpSpec = SourceASC->MakeOutgoingSpec(PassiveData->EmpowerControlGE, GetAbilityLevel(), ControlContext);
				if (UpSpec.IsValid())
				{
					UpSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockUpDuration, PassiveData->EmpowerKnockUpDuration);
					SourceASC->ApplyGameplayEffectSpecToTarget(*UpSpec.Data.Get(), TargetASC);

					// 上抛的 Z 冲量：UGE_KnockUp 只管状态不管位移，这里补（权威端天然满足）。
					if (PassiveData->EmpowerKnockUpLaunch > 0.f)
					{
						if (ACharacter* TargetCharacter = Cast<ACharacter>(Target))
						{
							TargetCharacter->LaunchCharacter(
								FVector(0.f, 0.f, PassiveData->EmpowerKnockUpLaunch),
								/*bXYOverride=*/false, /*bZOverride=*/true);
							UE_LOG(LogTemp, Log, TEXT("[Melee] 强化击飞：上抛 %.0f 硬控 %.2fs"),
								PassiveData->EmpowerKnockUpLaunch, PassiveData->EmpowerKnockUpDuration);
						}
					}
				}
				else
				{
					UE_LOG(LogTemp, Warning, TEXT("[Melee] EmpowerControlGE(%s) 的 Spec 无效 → 强化击只有伤害、没控制"),
						*GetNameSafe(PassiveData->EmpowerControlGE));
				}
			}
			else if (KnockbackGE)
			{
				FGameplayEffectSpecHandle KBSpec = SourceASC->MakeOutgoingSpec(KnockbackGE, GetAbilityLevel(), ControlContext);
				if (KBSpec.IsValid())
				{
					KBSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackDuration, KnockbackDuration);
					KBSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackImpulse, PassiveData->EmpowerKnockback);
					KBSpec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_KnockbackLaunch, PassiveData->EmpowerLaunch);
					SourceASC->ApplyGameplayEffectSpecToTarget(*KBSpec.Data.Get(), TargetASC);
				}
			}
		}
	}

	// 4) 顿帧放在【整个循环之后】、且只做一次：一记范围攻击可能同时扫到好几个人，
	//    在循环里调的话第一个目标就会把世界慢下来，后面几个目标的 GE 结算时间跟着被拉长。
	if (bLandedAny)
	{
		BeginImpactHitStop(Stage);
	}
}

// =============================================================================
// 命中那一下的顿帧
// =============================================================================

void UGA_ThreeHitPassive::BeginImpactHitStop(const FThreeHitAttackStage& Stage)
{
	if (!Stage.bHitStopOnImpact || Stage.HitStopTime <= 0.f)
	{
		return;
	}

	// 只在服务端施加：UWorld 的流速由 WorldSettings 复制到全场，客户端自己设会被服务端覆盖回去。
	if (!GetAvatarActorFromActorInfo()->HasAuthority())
	{
		return;
	}

	// 上一次还没还原（顿帧时长配得比这段的剩余时间还长，或者被同帧的第二段命中又调了一次）
	// → 不叠第二层。叠了的话两个还原定时器会互相把对方的流速写回去，最后停在哪个值看执行顺序。
	if (bHitStopApplied)
	{
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
	SavedWorldTimeDilation = Settings->TimeDilation;
	bHitStopApplied = true;

	// SetTimeDilation 会按 Min/MaxGlobalTimeDilation 夹一下（WorldSettings.cpp:345）：
	// 填超区间的值不报错，只是生效的不是填的那个 —— 所以日志打【实际生效的值】，别只看配置。
	UGameplayStatics::SetGlobalTimeDilation(this, Stage.HitStopScale);

	// ★ 时长必须在 SetGlobalTimeDilation【之后】折算：世界计时器跟的是膨胀后的 delta
	//   （LevelTick.cpp:1596 乘、:1816 喂给 TimerManager），冻帧期间「0.1 真实秒」只对应
	//   0.1×0.35 世界秒。传真实秒进去的话顿帧会一直持续到能力结束。
	World->GetTimerManager().SetTimer(
		HitStopTimer, this, &ThisClass::EndHitStop,
		FMath::Max(ScaledWorldSeconds(Stage.HitStopTime), 0.01f), /*bLoop=*/false);

	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段命中顿帧 %.3fs（真实）：世界时间 %.2f → %.2f"),
		StageIndex, Stage.HitStopTime, SavedWorldTimeDilation, Settings->GetEffectiveTimeDilation());
}

void UGA_ThreeHitPassive::EndHitStop()
{
	// 幂等：EndAbility 里还会兜底再调一次。漏了这一句的报错不是「技能坏了」，
	// 而是「整个世界卡在顿帧的流速上」。
	if (!bHitStopApplied)
	{
		return;
	}
	bHitStopApplied = false;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(HitStopTimer);
	}
	UGameplayStatics::SetGlobalTimeDilation(this, SavedWorldTimeDilation);

	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段命中顿帧结束：世界时间还原成 %.2f"),
		StageIndex, SavedWorldTimeDilation);
}

float UGA_ThreeHitPassive::ScaledWorldSeconds(float RealSeconds) const
{
	const UWorld* World = GetWorld();
	const AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
	const float Dilation = Settings ? Settings->GetEffectiveTimeDilation() : 1.f;

	// 世界计时器（定时器管理器用的那个）读的是【膨胀后】的 delta：
	// LevelTick.cpp:1596 `DeltaSeconds *= Info->GetEffectiveTimeDilation()`，1816 再把它喂给 TimerManager。
	// 所以「等 RealSeconds 那么久的真实时间」= 等 RealSeconds × 当前流速 那么多的世界秒。
	// 没有顿帧时 Dilation 是 1，这个函数退化成恒等。
	return RealSeconds * Dilation;
}

// =============================================================================
// 完美窗口的 QTE 表现
// =============================================================================

float UGA_ThreeHitPassive::GetPerfectWindowDuration() const
{
	return PerfectWindowCloseWorldTime - PerfectWindowOpenWorldTime;
}

USkeletalMeshComponent* UGA_ThreeHitPassive::GetAvatarMesh() const
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	return Character ? Character->GetMesh() : nullptr;
}

void UGA_ThreeHitPassive::SchedulePerfectWindowVFX()
{
	// 没数据资产 / 本段没有完美窗口（没勾标记、没配时间、已经是最后一段、和连段窗口没交集）：
	// 这些原因 ComputePerfectWindow 已经打过日志了，这里静默退出就够。
	if (!PassiveData || GetPerfectWindowDuration() <= 0.f)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		// 专用服务器没有渲染，一个粒子都不生成。和 UAnimNotifyState_BladeTrail 同一套判断。
		return;
	}

	// 两个延迟直接由那两个世界时间减出来，不自己再算一遍「开窗时间 / 攻速」——
	// 提示光和判定必须共用同一组时刻，否则会出现「光还没扫到刀尖就已经算完美」。
	FTimerManager& Timers = World->GetTimerManager();
	const float Now = World->GetTimeSeconds();

	Timers.SetTimer(PerfectVFXOpenTimer, this, &ThisClass::ShowPerfectWindowVFX,
		FMath::Max(PerfectWindowOpenWorldTime - Now, 0.f), /*bLoop=*/false);
	Timers.SetTimer(PerfectVFXCloseTimer, this, &ThisClass::HidePerfectWindowVFX,
		FMath::Max(PerfectWindowCloseWorldTime - Now, 0.f), /*bLoop=*/false);
}

void UGA_ThreeHitPassive::ShowPerfectWindowVFX()
{
	if (bPerfectWindowVFXActive || !PassiveData)
	{
		return;
	}

	// 提示光是可选的：没配就什么都不做，不影响连段本身 —— 但要说一声，
	// 否则「数据资产上忘了填 NS」和「填了但插槽/参数名不对」在屏幕上都是「什么都没发生」。
	UNiagaraSystem* System = PassiveData->PerfectWindowSystem.LoadSynchronous();
	if (!System)
	{
		if (!bLoggedMissingPerfectWindowVFX)
		{
			bLoggedMissingPerfectWindowVFX = true;
			UE_LOG(LogTemp, Warning, TEXT("[Passive] 完美窗口提示光没配：%s 上的 PerfectWindowSystem 是空的（软引用加载失败也一样）→ 这段窗口不会有任何提示"),
				*GetNameSafe(PassiveData));
		}
		return;
	}

	USkeletalMeshComponent* Mesh = GetAvatarMesh();
	if (!Mesh)
	{
		return;
	}

	const FName BaseSocket = PassiveData->PerfectWindowBaseSocket;
	const FName TipSocket = PassiveData->PerfectWindowTipSocket;

	// 插槽不存在时 GetSocketLocation 静默返回组件位置 —— 光会从角色原点扫出来，看不出是配错了。
	if (!ThreeHitHasSocket(Mesh, BaseSocket) || !ThreeHitHasSocket(Mesh, TipSocket))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 完美窗口提示光：%s 上找不到插槽 %s / %s → 不生成"),
			*GetNameSafe(Mesh->GetSkeletalMeshAsset()), *BaseSocket.ToString(), *TipSocket.ToString());
		return;
	}
	ThreeHitWarnIfMissingParameter(System, PerfectWindowBaseParameter, TEXT("完美窗口提示光"));
	ThreeHitWarnIfMissingParameter(System, PerfectWindowTipParameter, TEXT("完美窗口提示光"));

	// 挂在 mesh 上（不是挂在刀根插槽上）：坐标是每帧喂进去的，挂哪儿只影响第一帧和组件被回收时。
	PerfectWindowVFX = UNiagaraFunctionLibrary::SpawnSystemAttached(
		System, Mesh, NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
		EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/true, /*bAutoActivate=*/true);
	if (!PerfectWindowVFX)
	{
		// SpawnSystemAttached 被 Niagara 的可扩展性预剔除挡掉时也是返回 nullptr，不报错。
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 完美窗口提示光：SpawnSystemAttached 返回空（系统无效，或被 Niagara 预剔除挡掉）"));
		return;
	}

	// 把 NS 的时间轴整体压/拉到窗口长度：NS 原速播完要 PerfectWindowSystemDuration 秒，
	// 窗口只有 WindowSeconds 秒，倍率就是两者之比 —— 光正好在窗口关闭那一刻扫到刀尖。
	// CustomTimeDilation 是在 NiagaraComponent 的 tick 里乘进系统 delta 的（NiagaraComponent.cpp
	// 的 ManualTick(DeltaSeconds * CustomTimeDilation)），所以只对 Age Update Mode = Tick Delta Time
	// 的 NS 有效：DesiredAge 模式按 DesiredAge 推进，这个倍率不参与。
	const float WindowSeconds = GetPerfectWindowDuration();
	const float SystemDuration = PassiveData->PerfectWindowSystemDuration;
	const bool bScaleToWindow = SystemDuration > 0.f && WindowSeconds > 0.f;
	const float Dilation = bScaleToWindow ? SystemDuration / WindowSeconds : 1.f;
	if (bScaleToWindow)
	{
		PerfectWindowVFX->SetCustomTimeDilation(Dilation);
	}

	// 诊断：倍率是算出来的，光没在窗口关闭时扫到刀尖时，看这一行就知道是 NS 的时长填错了还是窗口不对。
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 完美窗口提示光：%s 窗口=%.3fs NS标准时长=%.2fs → 播放倍率=%.2f 插槽=%s→%s"),
		*GetNameSafe(System), WindowSeconds, SystemDuration, Dilation, *BaseSocket.ToString(), *TipSocket.ToString());

	// 生成当帧先喂一次，免得光在组件原点闪一帧再跳到刀上。
	bPerfectWindowVFXActive = true;
	TickPerfectWindowVFX();
}

void UGA_ThreeHitPassive::TickPerfectWindowVFX()
{
	if (!bPerfectWindowVFXActive || !PerfectWindowVFX || !PassiveData)
	{
		return;
	}

	if (USkeletalMeshComponent* Mesh = GetAvatarMesh())
	{
		// 每帧重新喂两个插槽的世界坐标：窗口这一百多毫秒里刀还在挥，只喂一次的话光会停在原地、
		// 刀自己走掉，看起来就是「光没跟着刀」。
		const FVector BaseWorld = Mesh->GetSocketLocation(PassiveData->PerfectWindowBaseSocket);
		const FVector TipWorld = Mesh->GetSocketLocation(PassiveData->PerfectWindowTipSocket);

		// World Space 时 ToLocal 是单位变换，InverseTransformPosition 原样返回；
		// Local Space 时转成 Niagara 组件的局部坐标（组件挂在 mesh 上，于是等于角色网格空间）。
		// 和 UAnimNotifyState_BladeTrail 的处理一致。
		const FTransform ToLocal = PassiveData->bPerfectWindowSystemLocalSpace
			? PerfectWindowVFX->GetComponentTransform() : FTransform::Identity;

		PerfectWindowVFX->SetVariablePosition(PerfectWindowBaseParameter, ToLocal.InverseTransformPosition(BaseWorld));
		PerfectWindowVFX->SetVariablePosition(PerfectWindowTipParameter, ToLocal.InverseTransformPosition(TipWorld));
	}

	// UGameplayAbility 没有 Tick，用「下一帧」定时器自己续：从定时器回调里再排一个下一帧的定时器，
	// 新的那个 ExpireTime 正好等于当前的 InternalTime，而 FTimerManager 的循环判的是
	// InternalTime > ExpireTime（严格大于）→ 不会在同一帧里被反复取出来，稳定一帧一次。
	// 停止靠 bPerfectWindowVFXActive：HidePerfectWindowVFX / EndAbility 置 false 之后自己就断了。
	if (UWorld* World = GetWorld())
	{
		PerfectVFXTickTimer = World->GetTimerManager().SetTimerForNextTick(this, &ThisClass::TickPerfectWindowVFX);
	}
}

void UGA_ThreeHitPassive::HidePerfectWindowVFX()
{
	bPerfectWindowVFXActive = false;

	if (!PerfectWindowVFX)
	{
		return;
	}

	// 先停发射：已经生成的那一段光按 NS 自己的寿命自然收掉，比直接销毁好看。
	PerfectWindowVFX->Deactivate();
	PerfectWindowVFX->SetAutoDestroy(true);

	UWorld* World = GetWorld();
	if (!World)
	{
		PerfectWindowVFX->DestroyComponent();
		PerfectWindowVFX = nullptr;
		return;
	}

	// 兜底：NS 的 emitter 要是设成无限循环，auto destroy 永远等不到 —— 那样每打一次连段就泄漏一个
	// 常驻组件。挂个一次性定时器强拆，保证不管 NS 怎么配都不会累积。（和 BladeTrail 的 TeardownTrail 同理。）
	TWeakObjectPtr<UNiagaraComponent> WeakComponent(PerfectWindowVFX);
	FTimerHandle TeardownTimer;
	World->GetTimerManager().SetTimer(TeardownTimer, FTimerDelegate::CreateWeakLambda(PerfectWindowVFX, [WeakComponent]()
	{
		if (UNiagaraComponent* StillAlive = WeakComponent.Get())
		{
			StillAlive->DestroyComponent();
		}
	}), PassiveData ? PassiveData->PerfectWindowTeardownDelay : 1.f, /*bLoop=*/false);

	PerfectWindowVFX = nullptr;
}

// =============================================================================
// 音效
// =============================================================================

void UGA_ThreeHitPassive::PlayPerfectSuccessSound()
{
	if (!PassiveData)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		// 专用服务器没有音频设备，建出来的组件也没人听。和生成粒子那几处同一套判断。
		return;
	}

	// 注意不能写成 const：ResolveSound 是 out 参数（bool&），const 会编不过。
	bool bOverridden = true;
	USoundBase* Sound = UHeroAudioLibrary::ResolveSound(LOLGameplayTags::Audio_PerfectWindow, PassiveData->PerfectWindowSuccessSound, bOverridden);
	if (!Sound)
	{
		// 不静默：否则「数据资产上忘了填」和「填了但软引用加载失败」都是「按对了没反应」，看不出区别。
		if (!bLoggedMissingPerfectWindowSound)
		{
			bLoggedMissingPerfectWindowSound = true;
			UE_LOG(LogTemp, Warning, TEXT("[Passive] 完美窗口成功音效没配：%s 上的 PerfectWindowSuccessSound 是空的（软引用加载失败也一样）→ 踩中完美窗口不会有任何反馈"),
				*GetNameSafe(PassiveData));
		}
		return;
	}

	if (const AActor* Avatar = GetAvatarActorFromActorInfo())
	{
		// 放在角色身上（3D）而不是 PlaySound2D：这是世界里的挥砍反馈，跟着角色走才对得上画面。
		// 两端都会跑到这里（客户端预测那份 + 服务端那份），于是旁边的玩家也听得到 —— 这是想要的。
		UHeroAudioLibrary::PlayAt(World, LOLGameplayTags::Audio_PerfectWindow, PassiveData->PerfectWindowSuccessSound,
			Avatar->GetActorLocation());
	}
}

bool UGA_ThreeHitPassive::IsUnarmedForm() const
{
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	return ASC && ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed);
}

void UGA_ThreeHitPassive::PlayAttackSound(bool bEmpowered, int32 InStageIndex)
{
	if (!PassiveData)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	const AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar)
	{
		return;
	}

	// 强化那一击优先用它自己那条（DS 上的 EmpoweredAttackSound 单点 override 仍然最优先），
	// 没配就退回事件表里 Audio.PassiveEmpoweredAttack 那条，而不是「强化了反而没声」。
	if (bEmpowered)
	{
		UHeroAudioLibrary::PlayAt(World, LOLGameplayTags::Audio_PassiveEmpoweredAttack,
			PassiveData->EmpoweredAttackSound, Avatar->GetActorLocation());
		return;
	}

	// 空手四段 / 持剑三段共用一个事件键不方便：两形态的挥击音色不一样，
	// 塞进同一条 ComboSoundByTag 只能靠「持剑只用前 N 个、空手用满 4 个」这种隐式约定。
	// 这里按形态选事件，配置表就能各配各的（空手 4 条、持剑 3 条），互不牵连。
	// 表里的 Boxing 键没配 → PlayAtStage 自动退回 Audio.PassiveAttack 的基础条目。
	const FGameplayTag AttackAudioEvent = IsUnarmedForm()
		? LOLGameplayTags::Audio_PassiveAttackBoxing
		: LOLGameplayTags::Audio_PassiveAttack;

	// 优先 DS 上的 AttackSound 单点 override；没配才走连段分句表
	// （ComboSoundByTag 的每段一个 Kallari 短语气词，
	// 每条 cue 内部还带随机变体，连击不会句句一样）。哪一段没配 → 退回事件表基础条目。
	if (PassiveData->AttackSound.IsValid())
	{
		UHeroAudioLibrary::PlayAt(World, AttackAudioEvent,
			PassiveData->AttackSound, Avatar->GetActorLocation());
		return;
	}

	UHeroAudioLibrary::PlayAtStage(World, AttackAudioEvent,
		FMath::Max(InStageIndex, 0), Avatar->GetActorLocation());
}

// =============================================================================
// 命中的基础打击表现
// =============================================================================

void UGA_ThreeHitPassive::ExecuteMeleeHitCue(const FHitResult& Hit, float CueWeight, int32 InStageIndex) const
{
	AActor* Source = GetAvatarActorFromActorInfo();
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (!Source || !SourceASC)
	{
		return;
	}

	// Location / Normal 由 UGC_MeleeHit 用来决定「打在哪、朝哪边喷」。
	// 扫掠出来的 HitResult 这两个字段都是有效的；用 IsNearlyZero 兜一下没填的情况
	// （和 UGC_ThrowDaggerHit 那边同一套写法）。
	FGameplayCueParameters CueParams;
	CueParams.Location = Hit.ImpactPoint.IsNearlyZero() ? Hit.Location : Hit.ImpactPoint;
	CueParams.Normal = Hit.ImpactNormal.IsNearlyZero() ? Hit.Normal : Hit.ImpactNormal;
	CueParams.Instigator = Source;
	CueParams.EffectCauser = Source;

	// 命中类型走参数而不是换 cue 标签：GameplayCueSet 只按弹射出的那个标签查表，
	// 不会给子标签各跑一份 cue（匕首那条 cue 的头注释里写了同一件事）。
	CueParams.AggregatedTargetTags.AddTag(ThreeHitResolveTargetType(Hit.GetActor()));

	// 本段的打击分量（Stage.HitCueWeight，0 = 没配 = 按 1 算）。
	// UGC_MeleeHit 拿它乘冲击波环的缩放和镜头振动强度 —— 四段共用一个 cue，
	// 不给分量的话「最重的收尾拳」和「第一下轻拳」在屏幕上完全一样。
	// 顺带把段号也塞进来（RawMagnitude，1 起），UGC_MeleeHit 解出来交给
	// UHeroAudioLibrary::PlayAtStage 查连段分句表（UHeroAudioConfig::ComboSoundByTag）；
	// 两个通道分开放是有意的：这里是「这一击多重」，那里是「这一击是第几段」，
	// 合成一个 float 的话命中类型（AggregatedTargetTags）之外的维度全要拆包，读起来容易搞混。
	CueParams.NormalizedMagnitude = CueWeight > 0.f ? CueWeight : 1.f;
	CueParams.RawMagnitude = FMath::Max(InStageIndex, 0) + 1.f;   // 0 段 → 1，三段 → 3

	// 服务端 ExecuteGameplayCue 会先在本端跑一次，再把参数多播给各客户端各自跑一次。
	// 必须走这条路：ApplyServerHit 只在服务端跑，就地 SpawnSystemAtLocation 的话只有主机看得见。
	SourceASC->ExecuteGameplayCue(LOLGameplayTags::GameplayCue_MeleeHit, CueParams);
}

// =============================================================================
// 强化命中的额外效果
// =============================================================================

void UGA_ThreeHitPassive::ExecuteEmpoweredHitCue(const FHitResult& Hit) const
{
	AActor* Source = GetAvatarActorFromActorInfo();
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (!Source || !SourceASC)
	{
		return;
	}

	// Location / Normal 由 UGC_EmpoweredHit 用来决定「炸在哪、朝哪边炸」。
	// 扫掠出来的 HitResult 这两个字段都是有效的；用 IsNearlyZero 兜一下没填的情况。
	FGameplayCueParameters CueParams;
	CueParams.Location = Hit.ImpactPoint.IsNearlyZero() ? Hit.Location : Hit.ImpactPoint;
	CueParams.Normal = Hit.ImpactNormal.IsNearlyZero() ? Hit.Normal : Hit.ImpactNormal;
	CueParams.Instigator = Source;
	CueParams.EffectCauser = Source;

	// 服务端 ExecuteGameplayCue 会先在本端跑一次，再把参数多播给各客户端各自跑一次。
	// 必须走这条路：ApplyServerHit 只在服务端跑，就地 SpawnSystemAtLocation 的话只有主机看得见。
	SourceASC->ExecuteGameplayCue(LOLGameplayTags::GameplayCue_EmpoweredHit, CueParams);
}
