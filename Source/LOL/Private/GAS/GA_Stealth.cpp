// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GA_Stealth.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/GE_Stealth.h"
#include "GAS/GE_StealthCooldown.h"
#include "GAS/GE_EmpoweredAttack.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffect.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Character.h"
#include "Animation/AnimMontage.h"

UGA_Stealth::UGA_Stealth()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	// 隐身自身不破除隐身（再按 Q 不误破）。
	bBreaksStealthOnCast = false;

	// 沉默挡法术。死亡/眩晕在基类（UMyGameplayAbility 构造函数）已经挡了，这里不要重复加。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Silenced);

	StealthGE = UGE_Stealth::StaticClass();
	EmpoweredAttackGE = UGE_EmpoweredAttack::StaticClass();
	CooldownDuration = 10.f;   // 测试值，BP 子类里按需调
	CooldownGameplayEffectClass = UGE_StealthCooldown::StaticClass();
}

void UGA_Stealth::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!ASC || !StealthGE)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 挂隐身 GE：SetByCaller 填时长，GE 授予 State.Stealth。
	// GE 上挂了 GameplayCue.Stealth —— 涂层、粒子、音效、屏幕框都在那个 cue 里，
	// 各客户端本地执行，GE 被移除（破隐/到期）时自动收尾。
	FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(StealthGE, GetAbilityLevel());
	if (!Spec.IsValid())
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_StealthDuration, StealthDuration);
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);

	// 施法动作留在能力里：它要跟着能力的预测时机在本地立刻播，不受 cue 的网络路径影响。
	if (StealthMontage)
	{
		if (ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
		{
			Character->PlayAnimMontage(StealthMontage);
		}
	}

	// 隐身结束才挂强化普攻，所以能力【不】在这里结束，要活到 State.Stealth 归零。
	// 用标签计数而不是某个 GE 的 handle：普攻破隐、施法破隐、自然到期三条路径都只有「tag 归零」这一个共同点，
	// 而且不关心是哪个 GE、哪个 handle 移除了它（预测回滚换 handle 也不影响）。
	StealthTagDelegateHandle = ASC->RegisterGameplayTagEvent(
		LOLGameplayTags::State_Stealth, EGameplayTagEventType::NewOrRemoved)
		.AddUObject(this, &UGA_Stealth::OnStealthTagChanged);

	// 兜底：GE 没挂上（被免疫 / 时长为 0 瞬间过期）时上面的注册永远等不到回调，能力会一直挂着。
	// 这种情况没真正进过隐身，所以不发强化普攻，直接以取消收尾。
	if (!ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Stealth))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Stealth] 挂上 GE 后仍无 State.Stealth → 没进隐身，直接收尾（不发强化）"));
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
	}
}

void UGA_Stealth::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	// 先摘监听再动 GE：顺序反过来的话，下面摘 GE 会让标签归零、回调立刻跑一次
	// OnStealthTagChanged → 给一个刚被打断（死亡/被控）的人挂上强化普攻。
	UnregisterStealthTagEvent();

	// 被外部取消（死亡、被控、被打断）时自己把隐身收掉。
	//
	// 这个 GE 的「正常」清理路径是「State.Stealth 归零」，而取消走的不是那条路 ——
	// 不收的话就是「能力没了、隐身还在」，表现成人以隐身状态死掉、再隐身复活。
	// 顺带把「任何外部取消都漏一层隐身 GE」这个老问题一起修了：
	// 这和 UThrowDaggerAbility::EndAbility → ExitAimingState 是同一条纪律 ——
	// 能力自己持有的状态，自己在 EndAbility 里清。
	//
	// 用 RemoveGrantedTagEffects 而不是 ASC->RemoveActiveEffectsWithGrantedTags：后者在非权威端
	// 是静默 no-op，客户端本地预测出来的那份摘不掉（理由见那个函数的注释）。
	if (bWasCancelled)
	{
		if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
		{
			UMyAbilitySystemComponent::RemoveGrantedTagEffects(ASC, FGameplayTagContainer(LOLGameplayTags::State_Stealth));
		}
	}

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

void UGA_Stealth::OnStealthTagChanged(const FGameplayTag ChangedTag, int32 NewCount)
{
	// 只关心「隐身没了」那一次；进入隐身（NewCount > 0）不处理。
	if (NewCount > 0)
	{
		return;
	}

	// 破隐（普攻/施法，两端各自的移除）和自然到期都汇到这里。
	// 但这只是「隐身没了」的通知，别把「破隐那一击是强化的」指望在它身上：普攻破隐的强化是
	// GA_ThreeHitPassive::ActivateAbility 自己挂的（那份实例一定在），本回调只兜底自然到期那条路
	// —— 客户端这份 GA_Stealth 实例未必还在（预测回滚/已结束），回调就不跑。
	UE_LOG(LogTemp, Warning, TEXT("[Stealth] State.Stealth 归零 → 挂强化普攻(%.2fs)"), EmpowerDuration);
	GrantEmpoweredAttack();

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}

void UGA_Stealth::GrantEmpoweredAttack()
{
	// 施加逻辑和三连击完美窗口共用，见 UMyGameplayAbility::ApplyEmpoweredAttack。
	ApplyEmpoweredAttack(EmpoweredAttackGE, EmpowerDuration);
}

void UGA_Stealth::UnregisterStealthTagEvent()
{
	if (!StealthTagDelegateHandle.IsValid())
	{
		return;
	}

	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->UnregisterGameplayTagEvent(StealthTagDelegateHandle, LOLGameplayTags::State_Stealth,
			EGameplayTagEventType::NewOrRemoved);
	}
	StealthTagDelegateHandle.Reset();
}
