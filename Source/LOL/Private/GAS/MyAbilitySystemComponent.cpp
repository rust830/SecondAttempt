// Fill out your copyright notice in the Description page of Project Settings.


#include "GAS/MyAbilitySystemComponent.h"
#include "GAS/MyGameplayAbility.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GAS/LOLGameplayTags.h"


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

	// 未激活 → 正常激活
	UE_LOG(LogTemp, Warning, TEXT("[ThrowDagger] 槽位 %s 未激活 → TryActivateAbility"), *SlotTag.ToString());
	TryActivateAbility(*Handle, true);
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
