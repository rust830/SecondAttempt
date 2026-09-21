// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/MyGameplayAbility.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemInterface.h"      // FindAbilitySystemComponent 的接口查询
#include "GameFramework/Pawn.h"           // 同上（② 那条 Pawn → PlayerState 的路）
#include "GameFramework/PlayerState.h"    // 同上（Cast 到接口需要完整类型）
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffect.h"
#include "Engine/World.h"   // AbilityInputTagHeld 里的 GetWorld()->GetTimeSeconds()（unity 构建时被别的文件顺带 include 过）


void UMyAbilitySystemComponent::AbilityInputTagPressed(const FGameplayTag& SlotTag)
{
	if (!SlotTag.IsValid()) return;

	const FGameplayAbilitySpecHandle* Handle = SlotAbilityMap.Find(SlotTag);
	if (!Handle)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 槽位 %s 未授权（SlotAbilityMap 无此键）"), *SlotTag.ToString());
		return;
	}

	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(*Handle);
	if (!Spec) return;

	// 只对 input-triggered 能力响应槽位按键；事件型/passive 能力走事件激活，这里静默忽略（不打日志，避免刷屏）。
	const UMyGameplayAbility* Ability = Cast<UMyGameplayAbility>(Spec->Ability);
	if (Ability && Ability->ActivationPolicy != EMyAbilityActivationPolicy::OnInputTriggered)
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] ASC 收到槽位输入: %s"), *SlotTag.ToString());

	// 已激活（如投掷技能正在瞄准）→ 再次按下 = 广播「再按」事件，交给能力自己决定。
	// 用 IsActive() 而不是 GetAbilityInstances().Num()：InstancedPerActor 的能力结束后实例仍留在数组里，Num() 会一直 >0。
	if (Spec->IsActive())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 槽位 %s 已激活 → 发再按事件"), *SlotTag.ToString());
		FGameplayEventData EventData;
		EventData.EventTag = LOLGameplayTags::Event_Input_Repressed;
		EventData.Instigator = GetOwnerActor();
		EventData.Target = GetOwnerActor();
		EventData.TargetTags.AddTag(SlotTag);
		HandleGameplayEvent(EventData.EventTag, &EventData);   // 广播给 WaitGameplayEvent，只发一次
		return;
	}

	// 破隐：施放非隐身技能时，若正处于隐身则破隐（隐身技能自身 bBreaksStealthOnCast=false，不误破）。
	BreakStealthForCast(Ability);

	// 未激活 → 正常激活
	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 槽位 %s 未激活 → TryActivateAbility"), *SlotTag.ToString());
	TryActivateAbility(*Handle, true);
}

void UMyAbilitySystemComponent::ServerBreakStealth_Implementation()
{
	RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(LOLGameplayTags::State_Stealth));
}

void UMyAbilitySystemComponent::BreakStealthForCast(const UMyGameplayAbility* Ability)
{
	if (!Ability || !Ability->bBreaksStealthOnCast || !HasMatchingGameplayTag(LOLGameplayTags::State_Stealth))
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("[Stealth] 施放技能 %s 破隐（权威=%d）"), *GetNameSafe(Ability), IsOwnerActorAuthoritative() ? 1 : 0);

	// Granted 版（不能用 RemoveActiveEffectsWithTags）：那个走 EffectTagQuery，只比对 GE 的 Asset Tags，
	// 而 State.Stealth 是 UTargetTagsGameplayEffectComponent 授出去的 Granted Tag，比对不上 → 静默不删除。
	// 再走本类的入口：ASC 自带的 Remove* 在非权威端是静默 no-op，客户端那份预测副本得自己摘。
	// 客户端上摘掉 0 个也是正常的（本地副本早被 catch-up 收走，只剩服务端复制来的那份）。
	RemoveGrantedTagEffects(this, FGameplayTagContainer(LOLGameplayTags::State_Stealth));

	// GE 的移除不可预测、也不复制，服务端得自己再摘一次，见 ServerBreakStealth。
	// 主机（listen server）自己按键时 IsOwnerActorAuthoritative() 为真，本地那次就是服务端那次，不用回发。
	if (!IsOwnerActorAuthoritative())
	{
		ServerBreakStealth();
	}
}

const UMyGameplayAbility* UMyAbilitySystemComponent::GetAbilityForSlot(const FGameplayTag& SlotTag) const
{
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(GetHandleForSlot(SlotTag));
	return Spec ? Cast<UMyGameplayAbility>(Spec->Ability) : nullptr;
}

bool UMyAbilitySystemComponent::SubmitManualTargetOnServer(FGameplayTag SlotTag, AActor* Target)
{
	if (!IsOwnerActorAuthoritative())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ManualTarget] 非权威端调用了 SubmitManualTargetOnServer → 忽略（激活只能由服务端做）"));
		return false;
	}

	if (!SlotTag.IsValid() || !Target)
	{
		return false;
	}

	const UMyGameplayAbility* Ability = GetAbilityForSlot(SlotTag);
	if (!Ability)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ManualTarget] 槽位 %s 上没有能力 → 忽略"), *SlotTag.ToString());
		return false;
	}

	// 破隐（权威那一半）：客户端那条路没走 AbilityInputTagPressed（按住键那次被输入层拦下来了），
	// 所以「施法破隐」得在这里补。顺序和按键那条路一致：先破隐，再激活。
	BreakStealthForCast(Ability);

	// 载荷 = 点了谁。走 GameplayEvent 而不是直接 TryActivateAbility：能力自己在 AbilityTriggers 里
	// 声明了「我要听哪个标签」，事件把它叫起来，载荷原样变成 ActivateAbility 的 TriggerEventData
	// （服务端那份直接拿到；按键方那份由 ClientActivateAbilitySucceedWithEventData 带过去）。
	// 标签从能力身上取，这里不认识任何具体技能。
	const FGameplayTag TriggerTag = Ability->GetGameplayEventTriggerTag();
	if (!TriggerTag.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[ManualTarget] 能力 %s 没声明 GameplayEvent 触发标签（AbilityTriggers 空）→ 激活不了"),
			*GetNameSafe(Ability));
		return false;
	}

	FGameplayEventData Payload;
	Payload.EventTag = TriggerTag;
	Payload.Instigator = GetAvatarActor();
	Payload.Target = Target;   // ← 目标就是这么过网络的（GameplayEvent 本身不过网络，载荷随激活复制过去）

	const int32 TriggeredCount = HandleGameplayEvent(TriggerTag, &Payload);
	UE_LOG(LogTemp, Warning, TEXT("[ManualTarget] 服务端收到目标 %s → 发 %s 激活（触发 %d 个能力）"),
		*GetNameSafe(Target), *TriggerTag.ToString(), TriggeredCount);

	return TriggeredCount > 0;
}

UAbilitySystemComponent* UMyAbilitySystemComponent::FindAbilitySystemComponent(const AActor* Actor)
{
	if (!Actor)
	{
		return nullptr;
	}

	// ① 接口。本项目英雄都是这条（AHeroCombatCharacter::GetAbilitySystemComponent 返回从 PS 上缓存的指针）。
	//    Cast 成功【不代表】有 ASC：缓存是 PossessedBy / OnRep_PlayerState 填的，没跑到就是空指针 ——
	//    所以这里不能像蓝图库那样直接 return，得往下试。
	if (const IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Actor))
	{
		if (UAbilitySystemComponent* FromInterface = ASI->GetAbilitySystemComponent())
		{
			return FromInterface;
		}
	}

	// ② 所属 Pawn 的 PlayerState。ASC 挂在 PS 上是本项目的架构，这条路保证「PS 上有就一定找得到」。
	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		if (const IAbilitySystemInterface* PSInterface = Cast<IAbilitySystemInterface>(Pawn->GetPlayerState()))
		{
			if (UAbilitySystemComponent* FromPlayerState = PSInterface->GetAbilitySystemComponent())
			{
				return FromPlayerState;
			}
		}
	}

	// ③ 自己身上的组件：纯蓝图演员（木桩这类）把 ASC 当组件挂的情况。
	return Actor->FindComponentByClass<UAbilitySystemComponent>();
}

int32 UMyAbilitySystemComponent::RemoveGrantedTagEffects(UAbilitySystemComponent* ASC, const FGameplayTagContainer& Tags)
{
	// 本项目的 ASC（英雄都是它，见 MyPlayerState）：走「非权威端也真的摘」那条路。
	if (UMyAbilitySystemComponent* MyASC = Cast<UMyAbilitySystemComponent>(ASC))
	{
		return MyASC->RemoveGrantedTagEffectsIncludingPredicted(Tags);
	}

	// 不是本类（理论上不该发生）：退回引擎原生调用，行为与改动前一致。
	return ASC ? ASC->RemoveActiveEffectsWithGrantedTags(Tags) : 0;
}

int32 UMyAbilitySystemComponent::RemoveGrantedTagEffectsIncludingPredicted(const FGameplayTagContainer& Tags)
{
	// 权威端：原路径就是对的，而且这是唯一真正改权威状态的那次移除。
	if (IsOwnerActorAuthoritative())
	{
		return RemoveActiveEffectsWithGrantedTags(Tags);
	}

	// 非权威端：ASC 那几个 Remove* 全被权威门槛挡着（静默返回 0），只能自己遍历容器。
	// ActiveGameplayEffects / RemoveActiveGameplayEffect_AllowClientRemoval 都是
	// UAbilitySystemComponent 的 protected 成员，在这里（子类）可用。
	const FGameplayEffectQuery Query = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(Tags);
	const TArray<FActiveGameplayEffectHandle> MatchingHandles = ActiveGameplayEffects.GetActiveEffects(Query);

	int32 NumRemoved = 0;
	for (const FActiveGameplayEffectHandle& Handle : MatchingHandles)
	{
		const FActiveGameplayEffect* Effect = ActiveGameplayEffects.GetActiveGameplayEffect(Handle);

		// 只动「本地预测出来」的那份。服务端复制下来的那份（WasReceived）本地删掉会让两边的
		// FastArray 对不上，交给服务端自己消耗时移除。
		if (!Effect || !Effect->PredictionKey.WasLocallyGenerated())
		{
			continue;
		}

		RemoveActiveGameplayEffect_AllowClientRemoval(Handle);
		++NumRemoved;
	}

	// 诊断：客户端这行要能看到「摘掉本地预测副本=1、摘后仍有标签=0」，
	// 否则强化窗口在客户端就是没被消耗掉（3 秒内每次起手都会读成强化）。
	UE_LOG(LogTemp, Warning, TEXT("[GAS] 非权威端摘标签 [%s]: 匹配 GE=%d 摘掉本地预测副本=%d 摘后仍有=%d"),
		*Tags.ToString(), MatchingHandles.Num(), NumRemoved, HasAnyMatchingGameplayTags(Tags) ? 1 : 0);

	return NumRemoved;
}

void UMyAbilitySystemComponent::AbilityInputTagHeld(const FGameplayTag& SlotTag)
{
		if (!SlotTag.IsValid()) return;

		const FGameplayAbilitySpecHandle* Handle = SlotAbilityMap.Find(SlotTag);
		if (!Handle) return;

		FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(*Handle);
		if (!Spec || !Spec->Ability) return;

		// ֱ��ʹ�� Spec->Ability�������Ǽ��ܵ� CDO
		UGameplayAbility* AbilityCDO = Spec->Ability;
		if (!AbilityCDO) return;

		// ��鼼���Ƿ���� "������ס�ظ�����" �ı�ǩ
		if (!AbilityCDO->AbilityTags.HasTag(FGameplayTag::RequestGameplayTag(FName("Ability.Policy.RepeatOnHold"))))
		{
			return;
		}

		// �����߼�����ֹÿ֡������
		static float LastHoldTime = 0.0f;
		float CurrentTime = GetWorld()->GetTimeSeconds();
		if ((CurrentTime - LastHoldTime) < MyLeastInterval) return;

		if (TryActivateAbility(*Handle, true))
		{
			LastHoldTime = CurrentTime;
		}
}

void UMyAbilitySystemComponent::AbilityInputTagReleased(const FGameplayTag& SlotTag)
{
	if (!SlotTag.IsValid()) return;

	const FGameplayAbilitySpecHandle* Handle = SlotAbilityMap.Find(SlotTag);
	if (!Handle) return;

	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(*Handle);
	if (!Spec) return;

	// ��ȡ��ǰ����ļ���ʵ��
	const TArray<UGameplayAbility*>& ActiveInstances = Spec->GetAbilityInstances();

	if (ActiveInstances.Num() == 0)
	{
		return;
	}

	// ��ü��ܵ����м���ʵ�����͡������ͷš��¼�
	FGameplayEventData EventData;
	EventData.EventTag = FGameplayTag::RequestGameplayTag(FName("Event.Input.Released"));
	EventData.Instigator = GetOwnerActor();
	EventData.Target = GetOwnerActor();

	for (UGameplayAbility* ActiveAbility : ActiveInstances)
	{
		UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(GetOwnerActor(), EventData.EventTag, EventData);
	}
}

FGameplayAbilitySpecHandle UMyAbilitySystemComponent::GetHandleForSlot(const FGameplayTag& SlotTag) const
{
	const FGameplayAbilitySpecHandle* Handle = SlotAbilityMap.Find(SlotTag);

	return Handle?*Handle:FGameplayAbilitySpecHandle();
}

int32 UMyAbilitySystemComponent::GetAbilityLevelForSlot(const FGameplayTag& SlotTag) const
{
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(GetHandleForSlot(SlotTag));
	return Spec ? Spec->Level:-1 ;
}

void UMyAbilitySystemComponent::SetAbilityLevelForSlot(const FGameplayTag& SlotTag, int32 NewLevel)
{	
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(GetHandleForSlot(SlotTag));
	if (Spec)
	{
		Spec->Level = NewLevel;
		MarkAbilitySpecDirty(*Spec);
	}
}

void UMyAbilitySystemComponent::OnGiveAbility(FGameplayAbilitySpec& AbilitySpec)
{
	Super::OnGiveAbility(AbilitySpec);
	UE_LOG(LogTemp, Warning, TEXT("[Passive] OnGiveAbility: %s (动态标签 %d 个)"), *GetNameSafe(AbilitySpec.Ability), AbilitySpec.DynamicAbilityTags.Num());
	for (const FGameplayTag& Tag : AbilitySpec.DynamicAbilityTags) {
		UE_LOG(LogTemp, Warning, TEXT("[Passive]   标签: %s"), *Tag.ToString());
		if (Tag.ToString().StartsWith(TEXT("Ability.Slot."))) {
			SlotAbilityMap.Add(Tag, AbilitySpec.Handle);
			return;
		}
	}
}

void UMyAbilitySystemComponent::OnRemoveAbility(FGameplayAbilitySpec& AbilitySpec)
{
	for (const FGameplayTag& Tag : AbilitySpec.DynamicAbilityTags) {
		if (Tag.ToString().StartsWith(TEXT("Ability.Slot."))) {
			SlotAbilityMap.Remove(Tag);
		}
	}
	Super::OnRemoveAbility(AbilitySpec);
}
