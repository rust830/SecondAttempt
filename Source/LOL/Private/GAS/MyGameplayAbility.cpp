// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/MyGameplayAbility.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/HeroCombatAttributeSet.h"
#include "GameplayEffect.h"
#include "GAS/LOLGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/Actor.h"

UMyGameplayAbility::UMyGameplayAbility()
{
	// 所有技能共享的准入状态，只有这一处声明 —— 子类不要再把 State.Dead / State.Stunned 加一遍。
	//
	// 判定读的是 ASC 的 OwnedGameplayTags，所以 GE 授予的标签和 AddLooseGameplayTag 都算
	// （GameplayAbility.cpp：CheckForBlocked(AbilitySystemComponent.GetOwnedGameplayTags(), ActivationBlockedTags)）。
	//
	// 普攻也走这条路：AHeroCombatCharacter::RouteBasicAttackInput 是直接 TryActivateAbility，
	// 一样会过 CanActivateAbility → CheckForBlocked，所以死亡/眩晕连平 A 一起挡掉，不用在输入层再挡一次。
	//
	// State.Silenced【刻意不在这里】：沉默挡法术不挡普攻是 LoL 语义，
	// 加在这一层会让平 A 也哑掉。要沉默的技能各自在构造函数里加，见各法术技能。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Dead);
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_Stunned);
}

FGameplayTag UMyGameplayAbility::GetGameplayEventTriggerTag() const
{
	// 只认第一 GameplayEvent 触发：一个能力配多条事件触发是引擎允许的，但「带载荷激活」这条链
	// 只需要知道「发哪个标签能把我叫起来」，多条的情况在这里没法区分，与其猜不如让调用方看见空标签。
	for (const FAbilityTriggerData& Trigger : AbilityTriggers)
	{
		if (Trigger.TriggerSource == EGameplayAbilityTriggerSource::GameplayEvent && Trigger.TriggerTag.IsValid())
		{
			return Trigger.TriggerTag;
		}
	}
	return FGameplayTag();
}

float UMyGameplayAbility::ComputeCooldownWithAbilityHaste(const FGameplayAbilityActorInfo* ActorInfo) const
{
	const UAbilitySystemComponent* ASC = ActorInfo ? ActorInfo->AbilitySystemComponent.Get() : nullptr;
	const UHeroCombatAttributeSet* Attributes = ASC ? ASC->GetSet<UHeroCombatAttributeSet>() : nullptr;

	// 没属性集 = 还没 InitAbilityActorInfo（召唤师技能就是这种情况）。返回原 CD，
	// 不要返回 0 —— 那会变成「技能没冷却」，比「冷却没被急速减」糟糕得多。
	if (!Attributes) return CooldownDuration;

	const float Haste = FMath::Max(0.f, Attributes->GetAbilityHaste());
	return CooldownDuration * 100.f / (100.f + Haste);
}

void UMyGameplayAbility::ApplyCooldown(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const
{
	UGameplayEffect* Effect = GetCooldownGameplayEffect();
	if (!Effect) {
		Super::ApplyCooldown(Handle, ActorInfo, ActivationInfo);
		return;
	}
	FGameplayEffectSpecHandle SpecHandle = MakeOutgoingGameplayEffectSpec(Effect->GetClass(), GetAbilityLevel());
	if (SpecHandle.IsValid() && CooldownDuration > 0) {
		SpecHandle.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_Cooldown, ComputeCooldownWithAbilityHaste(ActorInfo));
	}
	ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, SpecHandle);
}

void UMyGameplayAbility::ApplyEmpoweredAttack(TSubclassOf<UGameplayEffect> InEmpoweredAttackGE, float Duration)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	// 这里的失败以前是静默 return 的，排查时看不见：DS_Passive 上 EmpowerDuration 没填（=0）时，
	// 完美窗口/破隐挂强化会「看起来什么都没发生」——表现退回普通攻击，而伤害/击退那半可能还生效
	// （另一端走的是另一条挂载路径、另有自己的时长），症状就是「只有效果没有动画」。
	if (!Avatar || !ASC || !InEmpoweredAttackGE || Duration <= 0.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Empower] 没挂上（参数不全）: Avatar=%d ASC=%d GE=%s 时长=%.2f 权威=%d"),
			Avatar ? 1 : 0, ASC ? 1 : 0, *GetNameSafe(InEmpoweredAttackGE), Duration,
			(Avatar && Avatar->HasAuthority()) ? 1 : 0);
		return;
	}

	FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
	Context.AddSourceObject(this);

	FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(InEmpoweredAttackGE, GetAbilityLevel(), Context);
	if (!Spec.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Empower] 没挂上: MakeOutgoingSpec 失败（GE=%s）"), *GetNameSafe(InEmpoweredAttackGE));
		return;
	}
	// GE 的时长是 SetByCaller（FSetByCallerFloat 没有默认值），不填的话算出来是 0、挂上就过期。
	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_EmpowerDuration, Duration);

	// 刷新而不是叠加：先摘掉旧的同名状态再挂新的。
	// 不摘的话「破隐挂上 → 空挥没打中（不消耗）→ 完美窗口又挂一次」会留下两层 GE，时长各算各的；
	// 更麻烦的是引擎的 cue 事件按 GE 实例发、不按标签计数，第一层到期时强化表现的 OnRemove
	// 会提前跑一次（动画/音效被提前收掉）。摘干净后任何时刻最多一层，语义就是「刷新窗口」。
	// 放在 Spec 校验之后：造 Spec 失败时不动已有状态。
	// Granted 版：State.EmpoweredAttack 是 TargetTags 组件授的 Granted Tag，
	// RemoveActiveEffectsWithTags（比对 Asset Tags）在这里匹配不到，等于没摘。
	// 也得走 RemoveGrantedTagEffects：ASC 自带的 Remove* 在非权威端是静默 no-op，
	// 客户端上一次预测出来的那份（可能还活着，见 UMyAbilitySystemComponent::RemoveGrantedTagEffects 的注释）
	// 就摘不掉 —— 那样本地会叠成两层标签，StartStage 消耗掉一层也还是 >0，
	// 「3 秒内每次起手都算强化」的老问题会原样复发。
	UMyAbilitySystemComponent::RemoveGrantedTagEffects(ASC, FGameplayTagContainer(LOLGameplayTags::State_EmpoweredAttack));

	// 用「触发它的这个能力」的激活预测键施加，两端共用同一个键：
	// 客户端 InternalTryActivateAbility 里 FScopedPredictionWindow(this, true) 生成新键 → ActivationInfo.SetPredicting
	// → ServerTryActivateAbility 把键带上去；服务端 InternalServerTryActivateAbility 里
	// ServerSetActivationPredictionKey(InPredictionKey) 用回同一个键。
	// 客户端因此可以本地预测施加：ApplyGameplayEffectSpecToSelf 的准入是
	// HasNetworkAuthorityToApplyGameplayEffect = IsOwnerActorAuthoritative() || PredictionKey.IsValidForMorePrediction()，
	// 而 IsValidForMorePrediction() 判的就是「这是个本地客户端键」。于是 tag 在起手那一帧就有效，
	// StartStage 当场读得到，不用等复制；服务端用同一个键挂出的那份复制回来时被 NewCaughtUpDelegate 合并，不会变两份。
	//
	// 不能用 ApplyGameplayEffectSpecToOwner：它内部取 GetPredictionKeyForNewAction()（= ScopedPredictionKey，
	// 只在预测窗口内有效），而这里两个调用点（State.Stealth 归零回调、连段窗口定时器）都在窗口外，
	// 客户端会拿到无效键 → 静默什么都不做（就是之前 HasAuthority 门槛挡住的同一个结果）。
	const FPredictionKey PredictionKey = GetCurrentActivationInfo().GetActivationPredictionKey();
	ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get(), PredictionKey);

	// 诊断：两端各打一行。客户端那行必须「施加后标签=1」，否则破隐那一击的起手会退回播本段原蒙太奇
	// （表现退回普通攻击，但服务端结算照样吃强化 —— 正是「有击退、没强化动画」的样子）。
	UE_LOG(LogTemp, Warning, TEXT("[Empower] 挂强化普攻: 权威=%d 预测键=%d 服务端发起=%d 施加后标签=%d 时长=%.2f"),
		ASC->IsOwnerActorAuthoritative() ? 1 : 0, PredictionKey.Current, PredictionKey.bIsServerInitiated ? 1 : 0,
		ASC->HasMatchingGameplayTag(LOLGameplayTags::State_EmpoweredAttack) ? 1 : 0, Duration);
}
