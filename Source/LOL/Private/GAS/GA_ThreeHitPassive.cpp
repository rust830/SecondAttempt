// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/GA_ThreeHitPassive.h"
#include "GAS/ThreeHitPassiveData.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/GE_EmpoweredAttack.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
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
}

UGA_ThreeHitPassive::UGA_ThreeHitPassive()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;   // 每 actor 一个技能实例，成员变量能在多次激活间保持
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;  // 客户端预测表现，服务端权威结算
	AttackInputTag = LOLGameplayTags::Event_Input_BasicAttack;
	ActivationPolicy = EMyAbilityActivationPolicy::OnEvent;   // Passive: event-triggered, not button-triggered
	EmpoweredAttackGE = UGE_EmpoweredAttack::StaticClass();
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

	// 必须是 3 段数据，且能通过消耗/冷却检查，否则直接结束。
	if (PassiveData->Stages.Num() != 3 || !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Passive] 检查失败（Stages!=3 或 CommitAbility 失败）→ EndAbility"));
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

	StartStage(0);
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

		// 完美窗口的提示光和它的三个定时器：技能被打断 / 提前结束时窗口关闭定时器不会跑到，
		// 不在这里收掉的话光会一直挂在刀上。
		World->GetTimerManager().ClearTimer(PerfectVFXOpenTimer);
		World->GetTimerManager().ClearTimer(PerfectVFXCloseTimer);
		World->GetTimerManager().ClearTimer(PerfectVFXTickTimer);
	}

	if (PerfectWindowVFX)
	{
		PerfectWindowVFX->DestroyComponent();
		PerfectWindowVFX = nullptr;
	}
	bPerfectWindowVFXActive = false;

	// 停掉输入监听任务。
	if (InputTask)
	{
		InputTask->EndTask();
		InputTask = nullptr;
	}

	// 重置连段状态。
	StageIndex = INDEX_NONE;
	bWindowOpen = false;
	bQueuedNextStage = false;
	bPerfectQueued = false;
	PerfectWindowOpenWorldTime = 0.f;
	PerfectWindowCloseWorldTime = 0.f;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_ThreeHitPassive::OnAttackInput(FGameplayEventData Payload)
{
	// 只有连段窗口开着时，这次按键才算「排队下一段」。
	if (!bWindowOpen)
	{
		return;
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
	const UWorld* World = GetWorld();
	const float Now = World ? World->GetTimeSeconds() : 0.f;
	bPerfectQueued = PerfectWindowCloseWorldTime > PerfectWindowOpenWorldTime
		&& Now >= PerfectWindowOpenWorldTime && Now <= PerfectWindowCloseWorldTime;

	// 诊断：输入时刻和完美窗口区间并排打出来，早/晚一眼可见；两个 0 表示本段没有完美窗口。
	const AActor* Avatar = GetAvatarActorFromActorInfo();
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段输入: 权威=%d 完美=%d 输入时刻=%.3f 完美窗口=[%.3f,%.3f]"),
		StageIndex, (Avatar && Avatar->HasAuthority()) ? 1 : 0, bPerfectQueued ? 1 : 0, Now,
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

void UGA_ThreeHitPassive::StartStage(int32 NewStage)
{
	StageIndex = NewStage;
	bWindowOpen = false;
	bQueuedNextStage = false;
	bPerfectQueued = false;

	const FThreeHitAttackStage& Stage = PassiveData->Stages[StageIndex];
	const float Rate = GetAttackPlayRate();

	// World 提前取：完美窗口要靠世界时间换算，下面的定时器也要用。
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	ComputePerfectWindow(Stage, Rate);

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

	// 挥击音效和动作同帧起。三连击那几个蒙太奇里一个 AnimNotify_PlaySound 都没配，
	// 这里不补的话普攻就是完全没声的；强化那一击换成另一条更重的。
	PlayAttackSound(bStageEmpowered);

	// 三个定时器，时长都除以攻速倍率（攻速越快，动画和判定都越快）：
	World->GetTimerManager().SetTimer(HitTimer, this, &ThisClass::ConfirmHit, Stage.HitTime / Rate, false);                   // 命中结算
	World->GetTimerManager().SetTimer(OpenTimer, this, &ThisClass::OpenChainWindow, Stage.ChainWindowOpenTime / Rate, false);   // 连段窗口开启
	World->GetTimerManager().SetTimer(CloseTimer, this, &ThisClass::CloseChainWindow, Stage.ChainWindowCloseTime / Rate, false); // 连段窗口关闭

	// 完美窗口的提示光：本段没有完美窗口（只有第 2 段有）时这个函数自己会退出去。
	SchedulePerfectWindowVFX();
}

void UGA_ThreeHitPassive::ComputePerfectWindow(const FThreeHitAttackStage& Stage, float Rate)
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
	const float Now = GetWorld()->GetTimeSeconds();
	PerfectWindowOpenWorldTime = Now + EffectiveOpen / Rate;
	PerfectWindowCloseWorldTime = Now + EffectiveClose / Rate;
}

void UGA_ThreeHitPassive::OpenChainWindow()
{
	bWindowOpen = true;
}

void UGA_ThreeHitPassive::CloseChainWindow()
{
	bWindowOpen = false;

	const FThreeHitAttackStage& Stage = PassiveData->Stages[StageIndex];

	// 诊断：完美窗口那条路径是否武装强化，全看这一次输入有没有踩中完美窗口。
	UE_LOG(LogTemp, Warning, TEXT("[Passive] 第%d段窗口关闭: 已排队=%d 完美窗口标记=%d 完美=%d"),
		StageIndex, bQueuedNextStage ? 1 : 0, Stage.bPerfectWindowEnablesNextHitKnockback ? 1 : 0, bPerfectQueued ? 1 : 0);

	// 没排队下一段，或已经打满最后一段 → 结束技能。
	if (!bQueuedNextStage || StageIndex >= PassiveData->Stages.Num() - 1)
	{
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

void UGA_ThreeHitPassive::GrantEmpoweredAttack()
{
	if (PassiveData)
	{
		// 施加逻辑和破隐共用，见 UMyGameplayAbility::ApplyEmpoweredAttack。
		ApplyEmpoweredAttack(EmpoweredAttackGE, PassiveData->EmpowerDuration);
	}
}

void UGA_ThreeHitPassive::ConfirmHit()
{
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

	// 1) 球形扫描：从角色位置沿朝向前扫 TraceDistance 距离、半径 TraceRadius。
	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ThreeHitBasicAttack), false, Source);
	FCollisionShape Shape = FCollisionShape::MakeSphere(PassiveData->TraceRadius);
	const FVector Start = Source->GetActorLocation();
	const FVector End = Start + Source->GetActorForwardVector() * PassiveData->TraceDistance;
	if (!GetWorld()->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, ECC_Pawn, Shape, Params))
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
	for (const FHitResult& Hit : Hits)
	{
		AActor* Target = Hit.GetActor();
		if (!Target)
		{
			continue;
		}

		// 强化那一击打实了 → 在命中点放额外效果。走 cue 而不是就地 Spawn：这个函数只在服务端跑，
		// 直接生成的粒子只有主机看得到（GC_ThrowDaggerHit 修掉的是同一个坑）。
		if (bEmpowered)
		{
			ExecuteEmpoweredHitCue(Hit);
		}

		UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);

		// 只走 GAS 一条路：伤害统一由 GE_Damage + UExecCalc_Damage 结算（见方案文档 §1.2）。
		// 原来那条「没有 ASC 就退回 ApplyPointDamage」的分支【已删除】：留着它就等于开了一条
		// 绕过 ExecCalc 的路，格挡对那条路上的伤害完全失效，而且是静默失效（没有日志、没有断言）。
		// 代价：Variant_Combat 的敌人/木桩（ACombatEnemy 那套 MaxHP + TakeDamage）不再吃近战伤害 ——
		// 要保留它们，正确做法是给它们也挂 ASC + 属性集，而不是把回退分支留着。
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

				SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
			}
			else
			{
				// 不静默：Spec 无效 = 这一击一点伤害都没有，屏幕上看起来像「打空了」。
				UE_LOG(LogTemp, Warning, TEXT("[Melee] DamageEffect(%s) 的 Spec 无效 → 本次命中无伤害"),
					*GetNameSafe(PassiveData->DamageEffect));
			}
		}

		// 需要击退时，给目标一个冲量。
		if (bEmpowered)
		{
			if (ACharacter* TargetCharacter = Cast<ACharacter>(Target))
			{
				TargetCharacter->LaunchCharacter(Source->GetActorForwardVector() * PassiveData->EmpowerKnockback + FVector::UpVector * PassiveData->EmpowerLaunch, true, true);
			}
		}
	}
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

	USoundBase* Sound = PassiveData->PerfectWindowSuccessSound.LoadSynchronous();
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
		UGameplayStatics::PlaySoundAtLocation(World, Sound, Avatar->GetActorLocation());
	}
}

void UGA_ThreeHitPassive::PlayAttackSound(bool bEmpowered)
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

	// 强化那一击优先用它自己那条；没配就退回普通挥击音，而不是「强化了反而没声」。
	USoundBase* Sound = bEmpowered ? PassiveData->EmpoweredAttackSound.LoadSynchronous() : nullptr;
	if (!Sound)
	{
		Sound = PassiveData->AttackSound.LoadSynchronous();
	}

	if (!Sound)
	{
		if (!bLoggedMissingAttackSound)
		{
			bLoggedMissingAttackSound = true;
			UE_LOG(LogTemp, Warning, TEXT("[Passive] 普攻音效没配：%s 上的 AttackSound 是空的（软引用加载失败也一样）→ 三连击全程没有挥击声（三个蒙太奇里也没有 AnimNotify_PlaySound）"),
				*GetNameSafe(PassiveData));
		}
		return;
	}

	if (const AActor* Avatar = GetAvatarActorFromActorInfo())
	{
		UGameplayStatics::PlaySoundAtLocation(World, Sound, Avatar->GetActorLocation());
	}
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
