// 形态切换技能：带刀 ⇄ 空手。播完收刀/拔刀 montage 才真的切标签。
//
// ===========================================================================
// 【这个技能不是 Arena 专属】
// 形态是【角色属性】：任何模式都能按（正常对局、竞技场、训练场都一样）。
// 它由英雄技能组（AS_Abilitys_Kallari）授予，槽位 Ability.Slot.Form ——
// 和海克斯给的 Ability.Slot.Hex1 是两套独立的东西，别混。
//
// 【为什么切标签要放在 montage 播完之后】
// GAS_FormSwitch_Setup.md §1 记了一条关键事实：montage 播在 slot 上会**整体覆盖姿势**，
// 形态差值（PoseBlend 那个 additive）自动消失 ⇒ 「播 montage 时视觉强制回持刀」
// 是天然行为。所以：
//   ① 切换动画（收刀/拔刀）播的是**持刀姿势**的动画，本来就该全程持刀 ——
//      期间形态标签是【旧的】，ABP 的 FormUnarmed 权重仍按旧值走；
//   ② 动画播完、slot 释放、姿势回到状态机那一刻，权重才需要切到新值。
// 反过来做（先切标签再播动画）会出现「还在播收刀动画，但姿势已经变成空手」，
// 而 ABP 的权重是 FInterpTo 平滑的（FormBlendSpeed=8 ≈ 0.5s 收敛），
// 两者会在半程打架 —— 表现是「收刀收到一半人就空了手」，比完全不切还难看。
//
// 【标签必须走 GE，不能用 AddLooseGameplayTag】
// loose tag 在 UE 5.8 不复制（add_loose_gameplay_tags 的 bReplicate 默认 false，
// 见 GAS_FormSwitch_Setup.md §7）⇒ 服务端切了形态、客户端完全看不到。
// 这里挂 UGE_FormUnarmed（Infinite，GrantedTags 授 State.Form.Unarmed），
// 标签随 active GE 的 FastArray 复制，各端 AnimInstance 各自读自己的 ASC → 自动生效。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GA_FormSwitch.generated.h"

class UAnimMontage;
class UAbilityTask_PlayMontageAndWait;
class UGameplayEffect;

/**
 * 带刀 ⇄ 空手。按一下切到另一态，播对应 montage，播完才换标签。
 *
 * 槽位：Ability.Slot.Form（见 LOLGameplayTags.h）。由英雄技能组授予。
 */
UCLASS(Blueprintable)
class LOL_API UGA_FormSwitch : public UMyGameplayAbility
{
	GENERATED_BODY()

public:
	UGA_FormSwitch();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

protected:
	/**
	 * 持刀 → 空手：收刀动画。
	 *
	 * ★ 这段动画是【持刀姿势】的（收刀当然要有刀）。形态标签在这段播完才切，
	 *   期间视觉维持持刀 —— 那是正确的，见头文件那段说明。
	 * 留空 = 不播动画直接切（会「瞬切」，没手感但不会坏）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Form|Anim")
	TObjectPtr<UAnimMontage> ToUnarmedMontage;

	/** 空手 → 持刀：拔刀动画。留空 = 不播动画直接切。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Form|Anim")
	TObjectPtr<UAnimMontage> ToArmedMontage;

	/**
	 * 形态 GE。授予 State.Form.Unarmed。
	 *
	 * 默认指向 UGE_FormUnarmed，一般不用改。做成可配是为了将来出「第三形态」
	 * （比如双刀）时能再挂一个 GE 走同一条通道，而不用改这个类。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Form")
	TSubclassOf<UGameplayEffect> FormUnarmedGE;

private:
	/**
	 * 真的切形态：挂 / 摘 FormUnarmedGE。
	 *
	 * 【只在服务端执行】标签走 GE 复制（见头文件），客户端那份由复制下来的
	 * active GE 自然产生，不需要也不应该在这里本地施加。
	 */
	void ApplyForm(bool bToUnarmed);

	/**
	 * 挂上去的那个 FormUnarmedGE 的句柄。
	 *
	 * 为什么必须存：切回持刀时要用它摘。用「按标签找 GE」代替（RemoveGrantedTagEffects）
	 * 也能摘，但那条路是 GE 的 Granted Tags 查询 —— 一旦有人把 State.Form.Unarmed
	 * 改成 loose tag 就会静默摘不掉（查询匹配不到）。存句柄是直接的。
	 *
	 * ⚠️ 每次 ActivateAbility 必须重置：技能实例 InstancedPerActor、全对局复用同一个对象，
	 *   忘了清的症状是「第一次切形态正常，之后切不回去，而且没有任何日志」。
	 */
	FActiveGameplayEffectHandle FormGEHandle;

	/** 本次要切到哪一态（起手时锁定，不能被中途改掉）。 */
	bool bTargetUnarmed = false;

	/** 本次有没有真的播到动画。false = EndAbility 直接收尾。 */
	bool bPlayedMontage = false;

	/** montage 播完（被打断 / 播完 / 被取消都会走这里）。 */
	UFUNCTION()
	void OnMontageFinished();
};
