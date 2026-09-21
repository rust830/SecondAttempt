// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "MyAbilitySystemComponent.generated.h"

class UMyGameplayAbility;

/**
 * 
 */
UCLASS()
class LOL_API UMyAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere,BlueprintReadWrite)
	float MyLeastInterval = 0.1f;

	void AbilityInputTagPressed(const FGameplayTag& SlotTag);
	void AbilityInputTagHeld(const FGameplayTag& SlotTag);
	void AbilityInputTagReleased(const FGameplayTag& SlotTag);

	FGameplayAbilitySpecHandle GetHandleForSlot(const FGameplayTag& SlotTag)const;
	int32 GetAbilityLevelForSlot(const FGameplayTag& SlotTag)const;
	void SetAbilityLevelForSlot(const FGameplayTag& SlotTag,int32 NewLevel);

	/**
	 * 槽位上是哪个能力（读的是 spec，不关心有没有实例）。
	 * 输入层用它读能力自带的标记（如 bManualTargetSelect），这样输入层不用认识任何具体技能。
	 * 没授权 / 槽位上是别的基类时返回 nullptr。
	 */
	const UMyGameplayAbility* GetAbilityForSlot(const FGameplayTag& SlotTag) const;

	/**
	 * 施法破隐的「本地那一半」：非权威端也真的摘掉本地预测副本（见 RemoveGrantedTagEffects 的长注释），
	 * 非权威端再叫服务端摘一次（ServerBreakStealth）。
	 *
	 * 两个调用点：① 槽位按键那一刻（AbilityInputTagPressed）；② 「按住选目标」确认的那一刻
	 * （SubmitManualTargetOnServer 那条链）。② 的服务端自己还会再摘一次，所以这里那次
	 * ServerBreakStealth 是多余的 —— 但摘的是同一个 GE、第二次是 no-op，换来的是这一段只有一份定义。
	 */
	void BreakStealthForCast(const UMyGameplayAbility* Ability);

	/**
	 * 「按住选目标」的服务端那一半：破隐（权威）+ 把这个目标当载荷去激活槽位上的能力。
	 *
	 * 为什么必须服务端做：目标是客户端准星射线打出来的、相机也在客户端，而载荷过不了网络
	 * （GameplayEvent 只在本地派发），这类能力又都是 ServerInitiated —— 客户端激活不了。
	 * 主机（listen server）直接调；客户端由 AHeroCombatCharacter::ServerSubmitManualTarget 镜像过来。
	 *
	 * 返回是否真的发出了激活事件（false = 非权威端 / 载荷为空 / 槽位没能力 / 能力没声明事件触发标签）。
	 * 返回 true 也只代表「事件发出去了」，能不能激活还要过能力的 CanActivateAbility（冷却/射程/沉默……）。
	 */
	bool SubmitManualTargetOnServer(FGameplayTag SlotTag, AActor* Target);

	/**
	 * 从一个 Actor 上找 ASC。静态入口（手里只有裸 Actor 指针也能用），三条路依次试：
	 *   ① 接口 IAbilitySystemInterface（本项目英雄走这条，返回 PS 上那个）
	 *   ② 所属 Pawn 的 PlayerState 上的接口（ASC 挂在 PS 上、Pawn 只是转发时的兜底）
	 *   ③ Actor 自己身上的组件（纯蓝图演员把 ASC 当组件挂的情况）
	 *
	 * 为什么不直接用 UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent：
	 * 它内部是 UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor)，
	 * 实现是「Cast 到接口就 return 返回值」—— Cast 成功但【返回 nullptr】时它不会往下试
	 * （AbilitySystemGlobals.cpp:239）。本项目 AHeroCombatCharacter 实现了接口、返回的是
	 * 从 PlayerState 上缓存的指针（PossessedBy / OnRep_PlayerState 才填），这个缓存为空时
	 * 蓝图库那条路就给不出 ASC，而 PS 上明明有一个。这里补上 ② 就是为了这种情况。
	 *
	 * ⚠️ 选目标和校验目标必须用同一个函数：客户端用蓝图库挑、服务端用这个挑（或反过来）时，
	 * 会出现「客户端点中了、服务端说没 ASC」的诡异不同步。
	 */
	static UAbilitySystemComponent* FindAbilitySystemComponent(const AActor* Actor);

	virtual void OnGiveAbility(FGameplayAbilitySpec& AbilitySpec)override;
	virtual void OnRemoveAbility(FGameplayAbilitySpec& AbilitySpec)override;

	/**
	 * 施法破隐的「服务端那一半」。GE 的移除不可预测（FActiveGameplayEffectsContainer::RemoveActiveGameplayEffect
	 * 没有预测键参数，也没有 catch up 机制），AbilityInputTagPressed 里那次移除只作用在按键的这个客户端上，
	 * 不会同步到服务端 —— 服务端会一直认为英雄隐身，GA_Stealth 的标签回调不触发，
	 * 强化普攻要等隐身自然到期才挂上。所以按键方额外叫服务端自己摘一次。
	 */
	UFUNCTION(Server, Reliable) void ServerBreakStealth();

	/**
	 * 摘掉「授予了这些标签」的 GE —— 非权威端也真的摘。静态入口，手里只有裸 ASC 指针也能用。
	 *
	 * ASC 自带的 RemoveActiveEffectsWithGrantedTags 在非权威端是【静默 no-op】：
	 * 引擎里就是 `if (IsOwnerActorAuthoritative()) { ... } return 0;`（AbilitySystemComponent.cpp），
	 * 返回 0、不打任何日志。凡是「客户端预测出来的状态要在客户端本地摘掉」的地方都会踩到它：
	 *
	 *  - 强化普攻窗口（State.EmpoweredAttack）：它在【连段窗口关闭的定时器】里预测施加，
	 *    那一刻能力的激活预测键早被服务端 ack 过了 —— 引擎「预测副本 catch-up 时自动清理」那套
	 *    （ApplyGameplayEffectSpec 注册的 NewRejectOrCaughtUpDelegate → OnCaughtUpActiveGameplayEffect）
	 *    不会再为它触发；服务端那份又是同一帧里挂上又消耗掉，标签容器的 net delta 从没变过、
	 *    复制不下来（状态为 None 的本地项永远不会被服务端纠正），于是客户端这份本地副本没人来收，
	 *    一直活到 GE 自然到期（DS_Passive.EmpowerDuration，默认 3s）：这 3 秒里每次普攻起手
	 *    都读成强化、播强化蒙太奇，而服务端结算的其实是普通攻击。
	 *  - 破隐（State.Stealth）：摘掉后客户端当帧就不再是隐身态，不用等服务端那份复制回来。
	 *
	 * 非权威端就自己遍历 ActiveGameplayEffects，只摘「本地预测出来」的那份
	 * （PredictionKey.WasLocallyGenerated() —— 复制下来的那份带 PredictiveConnectionObjectKey，
	 * 一定被排除在外），走引擎给预测副本准备的 RemoveActiveGameplayEffect_AllowClientRemoval
	 * （和 catch-up 清理同一条路）。服务端复制下来的那份不归客户端管 —— 本地删它会让两边
	 * FastArray 对不上，服务端自己消耗时会移除、再复制下来。
	 *
	 * 返回实际摘掉的 GE 数。客户端上返回 0 是正常情况：本地那份预测副本已经被 catch-up 收走了，
	 * 剩下的只有服务端复制来的那一份，等它自己消失（破隐就是这样：本地摘=0，靠 ServerBreakStealth
	 * 让服务端摘）。
	 */
	static int32 RemoveGrantedTagEffects(UAbilitySystemComponent* ASC, const FGameplayTagContainer& Tags);

private:
	/** 上面那个的实例实现（要在子类里访问 protected 的 ActiveGameplayEffects）。 */
	int32 RemoveGrantedTagEffectsIncludingPredicted(const FGameplayTagContainer& Tags);

	TMap<FGameplayTag, FGameplayAbilitySpecHandle> SlotAbilityMap;
};
