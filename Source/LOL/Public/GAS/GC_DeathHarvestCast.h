// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_DeathHarvestCast.generated.h"

class APlayerCameraManager;
class UCameraModifier;
class UMaterialInterface;
class UMyDeathHarvestCameraModifier;
class UParticleSystem;
class USoundBase;

/**
 * 「消失中」状态表现（起手那一段就是消失，所以还是这条 cue —— 类名不动：
 * 标签是按类名推导的，改名等于换标签、还要重挂蓝图）。
 *
 * 挂上 = 进入消失，摘掉 = 现身，两头都由 UGA_DeathHarvest 在服务器上 Add / Remove：
 *   - 本机把施法者的所有 primitive 藏起来（【所有端】都藏，包括主人自己那台）+ 关掉 actor 碰撞；
 *   - 主人这一台再往本地相机上叠一层传送镜头（淡入淡出）。
 * 位置不归它管：现身时是 GA 先 SetActorLocation，再摘这条 cue，所以现身 burst 放的是新位置。
 *
 * ★ 为什么 OnActive 和 WhileActive 都转发到同一段逻辑
 *   这条 cue 是运行时 K2_AddGameplayCueWithParams 挂上去的，而这个路径上：
 *     服务器那一端收到的是 WhileActive，客户端收到的是 OnActive
 *     （AbilitySystemComponent.cpp:1601-1605 那句 "Call on server here, clients get it from repnotify"）。
 *   而 AGameplayCueNotify_Actor::WhileActive_Implementation 是空实现、【不会】转到 OnActive
 *   （GameplayCueNotify_Actor.cpp:341-349）。只实现 OnActive 的后果是：主机的屏幕上施法者不会消失，
 *   而且引擎不报任何错 —— 联机下看着就像「消失这个功能时灵时不灵」。
 *
 * 蓝图子类必须命名为 GC_DeathHarvest_Cast（不能叫 BP_GC_DeathHarvest_Cast），见 LOLGameplayTags.h 的说明。
 */
UCLASS()
class LOL_API AGC_DeathHarvestCast : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()

public:
	AGC_DeathHarvestCast();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;

	/**
	 * 运行时 Add 的 cue 在服务器走的是 WhileActive，而基类那条是空实现 —— 这里转给 OnActive，
	 * 让「服务器那台机器」和客户端跑同一段逻辑。理由见类注释。
	 */
	virtual bool WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;

	/** 消失那一下的一次性粒子（Cascade，世界坐标，放消失点上）。 */
	UPROPERTY(EditDefaultsOnly, Category="Vanish") TObjectPtr<UParticleSystem> VanishParticle;

	/** 现身那一下的一次性粒子（Cascade，世界坐标，放在传送后的新位置上）。 */
	UPROPERTY(EditDefaultsOnly, Category="Vanish") TObjectPtr<UParticleSystem> AppearParticle;

	/** 消失/现身的音效（可空，全端都能听到）。 */

	// --- 传送镜头（只给施法者本人看，淡入淡出）---
	/** 本地相机修改器类。默认 UMyDeathHarvestCameraModifier，一般不用改。 */
	UPROPERTY(EditDefaultsOnly, Category="Vanish|Screen")
	TSubclassOf<UCameraModifier> ScreenModifierClass;

	/**
	 * 全屏后处理材质（后处理域！），运行时推给相机修改器实例。
	 * 放在 cue 上而不是只放在修改器上，是为了不用再单独建一个 BP_DeathHarvestCameraModifier 子类 ——
	 * ScreenMaterial 在修改器上是 EditDefaultsOnly，不建子类就永远是空的。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Vanish|Screen")
	TObjectPtr<UMaterialInterface> ScreenMaterial;

protected:
	/** 兜底：角色被销毁 / 切关卡时 OnRemove 不一定走到，隐形/无碰撞和屏幕效果都不能留下。 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 隐身/现身时被本 cue 藏起来的那些 primitive（弱引用：中途被销毁就跳过）。 */
	TArray<TWeakObjectPtr<UPrimitiveComponent>> HiddenPrimitives;

	/** 关碰撞之前它开没开（本来就没开的别顺手打开）。 */
	bool bSavedActorCollision = true;
	bool bCollisionDisabled = false;

	/** 消失中的那个角色。OnRemove 和 EndPlay 都要靠它（cue 没 attach 到 owner）。 */
	TWeakObjectPtr<AActor> VanishTarget;

	/**
	 * 本实例是否真的往本地相机上加过修改器。加过才摘。
	 *
	 * 摘是按类在【本地】相机的 PlayerCameraManager 上找的，认不出「这个修改器是谁加的」：
	 * 一台机器上并存着好几个角色的大招 cue 实例，别人那份结束时它的目标在这台机器上
	 * GetController() 是空的（APlayerController 是 bOnlyRelevantToOwner，不复制给非属主）→
	 * 会掉进兜底的本机 PC，把我加的修改器当成自己的摘掉。表现就是「别人的大招结束，我的传送镜头跟着没了」。
	 * （同一个坑在 AGC_Stealth 上踩过一次，见 GC_Stealth.h 里 bAppliedLocalScreen 的说明。）
	 */
	bool bAppliedLocalScreen = false;

	/** 加修改器时用的那个相机管理器，摘的时候必须还给同一个（弱引用：随 PC 重建）。 */
	TWeakObjectPtr<APlayerCameraManager> ScreenCameraManager;

	/** 藏人 + 关碰撞（bVanished=true）/ 还原 + 开碰撞（false）。幂等，直接按目标状态写。 */
	void ApplyVanishVisuals(AActor* Target, bool bVanished);

	/** 在本地相机的 PlayerCameraManager 上加/摘本修改器。bOn=false 时按 AlphaOutTime 淡出。 */
	void ApplyLocalScreen(AActor* Target, bool bOn);

	/** 把 cue 上配的屏幕材质推给刚建出来的修改器实例，并打印最终生效的一整套参数。 */
	void ConfigureScreenModifier(UMyDeathHarvestCameraModifier* Modifier);
};
