// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_Stealth.generated.h"

class APlayerCameraManager;
class UCameraModifier;
class UMaterialInterface;
class UMeshComponent;
class UMyStealthCameraModifier;
class UParticleSystem;
class UParticleSystemComponent;
class USoundBase;

/**
 * 隐身表现：完全隐身版。
 *
 *  - 别人视角：角色所有 primitive 都不渲染（bOnlyOwnerSee），彻底看不见。
 *  - 主人视角：能看到半透明「幽灵」涂层（OverlayMaterial 叠一层，不动原材质）。
 *  - 主人屏幕：边缘全屏框 + 潜行滤镜，只在本地控制端加，敌人隐身不会改我的屏幕。
 *  - 粒子/音效：进入 burst + 持续循环 + 破隐 burst，都是 Cascade；素材直接取 ParagonKallari 的
 *    ShadowPlane 套装（P_ShadowPlane_{Enter,Loop,Exit}_Lens + Kallari_Effort_Ability_Q_{Enter,Exit}）。
 *  - 双刀刀根各挂一份常驻 Cascade（SwordParticle），另有一个按隐身时长缩放的倒计时光环 Cascade
 *    （CountdownParticle）——这两个和循环粒子一样只给主人看。
 *
 * 生命周期挂在 UGE_Stealth 的 GameplayCues 上：GE 挂上 → OnActive，GE 移除（破隐/到期）→ OnRemove，
 * 不需要任何外部清理调用。每个客户端各自执行一次，所以「谁能看到谁」是各端本地决定的，不依赖属性复制。
 *
 * 蓝图子类必须命名为 GC_Stealth（不能叫 BP_GC_Stealth），见 LOLGameplayTags.h 里的说明。
 */
UCLASS()
class LOL_API AGC_Stealth : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()

public:
	AGC_Stealth();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;

	/** 只用来算光环倍率（等那份 GE 进到 ActiveGameplayEffects），算完就自己关掉。 */
	virtual void Tick(float DeltaSeconds) override;

	/** 主人视角的半透明涂层材质（可空：不填则主人也只看到原样）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Visual")
	TObjectPtr<UMaterialInterface> StealthOverlayMaterial;

	/** 进入隐身的一次性粒子（Cascade，可空）。Paragon: P_ShadowPlane_Enter_Lens。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Visual")
	TObjectPtr<UParticleSystem> EnterParticle;

	/** 隐身持续期间的循环粒子（Cascade，可空），挂在角色身上跟着走。Paragon: P_ShadowPlane_Loop_Lens。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Visual")
	TObjectPtr<UParticleSystem> LoopParticle;

	/** 破隐的一次性粒子（Cascade，可空）。Paragon: P_ShadowPlane_Exit_Lens。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Visual")
	TObjectPtr<UParticleSystem> ExitParticle;

	/** 进入隐身的一次性音效（可空）。Paragon: Kallari_Effort_Ability_Q_Enter。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Visual")
	TObjectPtr<USoundBase> EnterSound;

	/** 破隐的一次性音效（可空）。Paragon: Kallari_Effort_Ability_Q_Exit。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Visual")
	TObjectPtr<USoundBase> ExitSound;

	/**
	 * 隐身期间挂在两把刀根插槽上的同一个 Cascade 粒子（可空，左右各生成一份）。
	 * 和 LoopParticle 一样只给主人看：敌人能看见刀上挂着常驻特效的话，隐身本身就白做了。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Sword")
	TObjectPtr<UParticleSystem> SwordParticle;

	/** SwordParticle 挂的两个插槽（Kallari 是双刀）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Sword")
	FName SwordSocketLeft = TEXT("sword_base_l");

	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Sword")
	FName SwordSocketRight = TEXT("sword_base_r");

	/**
	 * 粒子相对插槽的朝向。
	 *
	 * 用的是 EAttachLocation::SnapToTarget —— 这个值直接落成组件的相对旋转，也就是【相对插槽】，
	 * 不是世界朝向，所以跟着刀走、不用管角色转不转身。
	 *
	 * 左右分开配是因为右手插槽一般是左手的镜像：同一个粒子挂上去，「沿刀面」的那个方向会翻过来
	 * （朝着刀背而不是刀刃）。右手反了就调 SwordParticleRotationRight —— 通常是绕刀刃长轴转 180°。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Sword")
	FRotator SwordParticleRotationLeft = FRotator::ZeroRotator;

	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Sword")
	FRotator SwordParticleRotationRight = FRotator::ZeroRotator;

	/**
	 * 隐身期间的倒计时光环 Cascade（可空）。就是 Kallari 的 P_Ultimate_Vision_Lens_Timer 原版。
	 *
	 * 时间轴会被整体压/拉到【隐身的实际时长】：粒子系统原速播完要 CountdownParticleDuration 秒，
	 * 隐身是几秒从 ASC 上那份 State.Stealth 的 GE 现读（UGA_Stealth 用 SetByCaller
	 * Data_StealthDuration 填进去的），倍率 = 两者之比 —— 1s 的环放在 8s 隐身里就是 0.125，
	 * 环正好在破隐/到期那一刻走完。和 UGA_ThreeHitPassive 的完美窗口提示光
	 * （PerfectWindowSystemDuration）是同一套做法。
	 *
	 * 缩放靠 UParticleSystemComponent::CustomTimeDilation（ParticleSystemComponent.h 里的公开成员，
	 * Tick 里 DeltaTime *= CustomTimeDilation）。引擎两条 tick 路径都认这个值：组件自己 tick 走上面那行，
	 * 交给世界粒子管理器批处理时管理器也按它推进 —— 不像 Niagara 有 Age Update Mode = DesiredAge
	 * 那条按 DesiredAge 推进、倍率静默失效的分支。
	 *
	 * 之前这里是转出来的 NS（P_Ultimate_Vision_Lens_Timer_Converted），换成原版 Cascade 的原因：
	 * 那个环的填充是【Dynamic Parameter + Event Generator/Receiver】驱动的（原资产里能查到
	 * ParticleModuleParameterDynamic / EventGenerator / EventReceiverSpawn），这条链正是
	 * Cascade→Niagara 转换最容易断的地方 —— 转完的表现就是环转不完。用回原版资产绕开这个转换。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Countdown")
	TSoftObjectPtr<UParticleSystem> CountdownParticle;

	/**
	 * CountdownParticle 按原速播完要多久（秒）—— 就是 Cascade 编辑器时间轴的总长度
	 * （Emitter Duration × Loops，带 Lifetime 的看粒子寿命）。填 0 = 不缩放，按原速播。
	 * 填错的表现：填大了环提前走完、填小了环在破隐时被 DestroyCountdown 掐断，两种都像「跟不上时间」。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Countdown", meta = (ClampMin = "0", Units = "s"))
	float CountdownParticleDuration = 1.f;

	/** 光环挂哪个插槽/骨骼，留空 = 挂在角色网格原点（挂点原点在脚下时留空即可）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Countdown")
	FName CountdownAttachSocket = NAME_None;

	/** 光环相对挂点的偏移（想把环压到地面之类时用）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Countdown")
	FVector CountdownRelativeOffset = FVector::ZeroVector;

	/**
	 * 光环缩放（1 = 原大小）。
	 * 落成组件的相对缩放，所以会连挂点（插槽/骨骼）自己的缩放一起乘进去 ——
	 * 挂在缩放不是 1 的骨骼上时，实际大小是两者之积。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Countdown")
	FVector CountdownScale = FVector(1.f);

	/** 本地屏幕效果用的相机修改器类。默认 UMyStealthCameraModifier，一般不用改。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Screen")
	TSubclassOf<UCameraModifier> ScreenModifierClass;

	/**
	 * 全屏边缘框后处理材质（后处理域！），运行时推给相机修改器实例。
	 * 放在 cue 上而不是只放在修改器上，是为了不用再单独建一个 BP_StealthCameraModifier 子类 ——
	 * ScreenMaterial 在修改器上是 EditDefaultsOnly，不建子类就永远是空的。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Screen")
	TObjectPtr<UMaterialInterface> ScreenMaterial;

protected:
	/** 兜底：角色被销毁 / 切关卡时 OnRemove 不一定走到，屏幕效果不能留在相机上。 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 隐身期间的循环粒子组件（OnActive 创建，OnRemove/EndPlay 销毁）。 */
	UPROPERTY(Transient)
	TObjectPtr<UParticleSystemComponent> LoopParticleComp;

	/** 隐身期间挂在两把刀根上的粒子组件（OnActive 创建，OnRemove/EndPlay 销毁）。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UParticleSystemComponent>> SwordParticleComps;

	/** 倒计时光环组件（OnActive 创建，OnRemove/EndPlay 销毁）。 */
	UPROPERTY(Transient)
	TObjectPtr<UParticleSystemComponent> CountdownComp;

	/** 光环倍率只算一次算完就锁住（隐身时长在挂 GE 那一刻就定死了）。 */
	bool bCountdownScaled = false;

	/** 破隐/到期时用来从 ASC 现读隐身剩余时间的对象（cue 没 attach 到 owner，得自己记一份）。 */
	TWeakObjectPtr<AActor> StealthTarget;

	/** 涂层材质的诊断日志只打一次。 */
	bool bLoggedOverlayDiagnostic = false;

	/**
	 * 本实例是否真的往本地相机上加过修改器。加过才摘。
	 *
	 * 摘是按类在【本地】相机的 PlayerCameraManager 上找的，认不出「这个修改器是谁加的」：
	 * 联机下一台机器上并存着好几个角色的隐身 cue 实例（同一个 GE 在每个端都会跑一遍），
	 * 别人那份结束时会走 OnRemove → ApplyLocalScreen(..., false) —— 它的目标角色在这台机器上
	 * GetController() 是空的（APlayerController 是 bOnlyRelevantToOwner，不复制给非属主），
	 * 于是掉进 ApplyLocalScreen 的 GetPlayerController(this, 0) 兜底 = 本地主玩家的 PC，
	 * 把我加的那个修改器当成自己的摘掉。表现就是「主机隐身结束，我的屏幕效果跟着没了，隐身还在」。
	 */
	bool bAppliedLocalScreen = false;

	/**
	 * 加修改器时用的那个相机管理器，摘的时候必须还给同一个（弱引用：随 PC 重建）。
	 * EndPlay 兜底那条路径上 MyTarget 已经空了，解析不出 PC，只能靠这里存的。
	 */
	TWeakObjectPtr<APlayerCameraManager> ScreenCameraManager;

	/** 切换「只有主人可见 + 涂层」。幂等，直接按目标状态写，不依赖外部标记。 */
	void ApplyMeshVisuals(AActor* Target, bool bStealth);

	/** 在 MyTarget 位置放一个一次性 Cascade 粒子（Cascade 用 UParticleSystem 而不是 Niagara）。 */
	void SpawnOneShotParticle(AActor* Target, UParticleSystem* Particle) const;

	/** 在 MyTarget 位置播一个 2D/3D 音效（可空，静默跳过）。 */
	void SpawnSound(AActor* Target, USoundBase* Sound) const;

	/** 销毁循环粒子组件（幂等，OnRemove 和 EndPlay 都会调）。 */
	void DestroyLoopParticle();

	/** 循环粒子 / 刀根粒子 / 光环统一挂在角色网格上跟着身体走；没有网格就退回 root。 */
	USceneComponent* ResolveAttachComponent(AActor* Target) const;

	/** 在 SwordSocketLeft / SwordSocketRight 上各生成一份 SwordParticle（先清旧的，可重入）。 */
	void SpawnSwordParticles(USceneComponent* AttachTo);

	/** 销毁两把刀上的粒子组件（幂等）。 */
	void DestroySwordParticles();

	/** 生成倒计时光环并打开 tick（倍率要等 GE 进 ActiveGameplayEffects 才读得到，见 Tick）。 */
	void SpawnCountdown(USceneComponent* AttachTo);

	/** 销毁光环组件 + 关掉 tick（幂等）。 */
	void DestroyCountdown();

	/**
	 * 从 ASC 现读 State.Stealth 那份 GE 的剩余/总时长，算倍率写进光环组件的 CustomTimeDilation；
	 * 拿到之前每帧重试。拿到（或确定拿不到）之后锁住，同时把 tick 关掉。
	 */
	void TryScaleCountdownToStealth();

	/** 在本地相机的 PlayerCameraManager 上加/摘本修改器。bOn=false 时按 AlphaOutTime 淡出。 */
	void ApplyLocalScreen(AActor* Target, bool bOn);

	/** 把 cue 上配的屏幕材质推给刚建出来的修改器实例，并打印最终生效的一整套参数。 */
	void ConfigureScreenModifier(UMyStealthCameraModifier* Modifier);

	/** 打印涂层的各道门槛（域/混合模式/骨骼网格用法/分槽涂层），涂层静默失效时靠这条日志定位。 */
	void LogOverlayDiagnostic(UMeshComponent* Mesh);
};
