// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "GameplayTagContainer.h"
#include "HeroCombatCharacter.generated.h"

class UAbilitySystemComponent;
class UAbilitySet;
class UBlockComponent;
class UGameplayEffect;

/** GAS 角色基类：ASC 生命周期 + 英雄技能组授权 + 普攻/槽位输入路由。ALOLCharacter 继承它。 */
UCLASS(Blueprintable)
class LOL_API AHeroCombatCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()
public:
	AHeroCombatCharacter();
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystemComponent; }

	/**
	 * 死亡判定。全项目唯一一处 —— 读的是 ASC 上的 State.Dead 标签，不是 Health 数值：
	 * 「能不能动」和「技能能不能放」必须是同一个判据，而技能那边挡人的就是标签
	 * （UMyGameplayAbility 的 ActivationBlockedTags）。
	 *
	 * 技能本身不用调它：TryActivateAbility → CanActivateAbility → CheckForBlocked 已经挡住了。
	 * 这个函数是给不走 GAS 的入口用的（移动、跳跃这些直接读输入的地方）。
	 */
	UFUNCTION(BlueprintPure, Category="Hero|Death") bool IsDead() const;

	/** 绑定到普攻（左键）Started 事件。 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") void BasicAttackPressed();
	/** 由 PlayerController 转发的槽位输入（QWER/DF）—— 按下。 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") void AbilityInputTagPressed(FGameplayTag SlotTag);
	/**
	 * 同一个槽位键松开（PlayerController 的 Completed 事件转发过来）。
	 * 目前只有「按住选目标」这条链用得上：松开 = 关掉选择窗口，这次就当没按过。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") void AbilityInputTagReleased(FGameplayTag SlotTag);

	/** 最后打死自己的那个角色。没死过 / 已销毁时是 nullptr。给击杀提示用。 */
	UFUNCTION(BlueprintPure, Category="Hero|Death") AActor* GetLastDamageCauser() const { return LastDamageCauser.Get(); }

	virtual void BeginPlay() override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;

protected:
	/** 英雄技能组（被动 + QWER），数据资产。 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Abilities") TObjectPtr<UAbilitySet> ChampionKit;

	UPROPERTY(EditDefaultsOnly, Category="Hero|Tags") FGameplayTag PassiveSlotTag;
	UPROPERTY(EditDefaultsOnly, Category="Hero|Tags") FGameplayTag BasicAttackInputTag;

	UPROPERTY(Transient) TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	/**
	 * 格挡/免疫的减免判定（见 UBlockComponent）。
	 * 状态本身是 GE 标签（State.Blocking / State.BlockImmune），这个组件只做判定 + 记录窗口关闭时刻，
	 * 唯一调用者是 UExecCalc_Damage。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Hero|Block") TObjectPtr<UBlockComponent> BlockComponent;

	// 隐身的渲染表现（只有主人可见 + 涂层 + 屏幕框）已经搬进 GameplayCue，
	// 挂在 UGE_Stealth 的 GameplayCues 上（见 AGC_Stealth），这里不再需要专门的组件。
	// 破隐也不在这里做：普攻破隐归 GA_ThreeHitPassive::ActivateAbility（起手处），
	// 施法破隐归 UMyAbilitySystemComponent::AbilityInputTagPressed。

	UFUNCTION(Server, Reliable) void ServerSubmitBasicAttackInput();
	/** 瞄准态下的「确认投掷」也要镜像到服务端：GameplayEvent 是纯本地事件，不过网络。 */
	UFUNCTION(Server, Reliable) void ServerSubmitThrowConfirmInput();
	/**
	 * 「按住选目标」的确认（左键点中了谁）镜像到服务端。
	 * 目标本身也是载荷的一部分：准星射线和相机都只在客户端，服务端没法自己复现「点了谁」。
	 */
	UFUNCTION(Server, Reliable) void ServerSubmitManualTarget(FGameplayTag SlotTag, AActor* Target);

	// ---------------------------------------------------------------------
	// 死亡与重生
	//
	// 链路只有一条：血量归零 → UHeroCombatAttributeSet::PostGameplayEffectExecute（仅权威端）
	// → OnOutOfHealth → HandleOutOfHealth 挂 UGE_Death → 各端收到 State.Dead 标签事件
	// → EnterDeathState（布娃娃 + 停移动）。GE 到期摘掉标签 → ExitDeathState（还原 + 回出生点 + 回血）。
	//
	// 复活时长只有一处来源：GE_Death 的时长（= RespawnDelay），复活的触发点就是那个 GE 到期。
	// 不要再另开一个 FTimerHandle 计时，两份计时迟早对不上。
	// ---------------------------------------------------------------------

	/** 死亡状态 GE（授予 State.Dead + 打断所有技能）。留空则用 UGE_Death。 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Death")
	TSubclassOf<UGameplayEffect> DeathEffect;

	/** 复活等待时长（秒），作为 Data.RespawnDelay 填给 GE_Death。调复活时间只改这里。 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Death", meta=(ClampMin="0", Units="s"))
	float RespawnDelay = 5.f;

	/** 死亡时把骨骼网格切成布娃娃。关掉的话就只是站着不动（没有死亡动画资产时的兜底）。 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Death")
	bool bRagdollOnDeath = true;

private:
	void InitializeAbilityActorInfo();
	void RouteBasicAttackInput();
	void RouteThrowConfirmInput();
	bool bAbilitiesGranted = false;

	// ---------------------------------------------------------------------
	// 手动选目标（「按住 R 选目标，左键点中才放」，见 UGA_DeathHarvest）
	//
	// 这一整段是【输入层】的事，不占任何 GAS 状态：
	//   按下槽位键  → 开窗口（挂本地标签 State.DeathHarvest.Selecting），能力不激活、不进冷却、不锁移动
	//   左键        → 被 BasicAttackPressed 拦下 → 准星射线打到谁就把谁交给服务端去激活
	//   松开槽位键  → 关窗口，这次当没按过
	//   技能真激活  → 由能力自己摘掉标签关窗口（服务端判定不过时窗口还开着，可以走近了再点）
	// ---------------------------------------------------------------------

	/** 打开选择窗口：记住是哪个槽位 + 挂本地路由标签（幂等，重复按不叠加）。 */
	void BeginManualTargetSelect(const FGameplayTag& SlotTag);
	/** 关掉选择窗口。幂等，没开过也能调。 */
	void EndManualTargetSelect();

	/** 准星（相机视角中心射线）正前方第一个「带 ASC 的别人」。点空了返回 nullptr。 */
	AActor* TraceManualTargetUnderCrosshair() const;

	/** 左键在选择窗口里的语义：选目标 → 破隐 → 交给服务端激活。点空则什么都不做（窗口继续开着）。 */
	void RouteManualTargetConfirm();

	/** 当前开着窗口的槽位。无效 = 没开窗口（标签是它的对外表现，这个是给确认那一下用的）。 */
	FGameplayTag ManualTargetSlotTag;

	/** 血量归零（只在权威端会进来）。挂死亡 GE —— 其余后果一律由 State.Dead 标签事件接管。 */
	UFUNCTION() void HandleOutOfHealth(AActor* DamageInstigator, AActor* DamageCauser);

	/** State.Dead 计数变化。两端都会跑：服务端负责重生，所有端负责布娃娃和停移动。 */
	void OnDeadTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	void EnterDeathState();
	void ExitDeathState();

	/** 复活落点，服务端调 AGameModeBase::FindPlayerStart。返回 false = 没有 GameMode / 没有 PlayerStart。 */
	bool FindRespawnTransform(FTransform& OutTransform) const;

	/**
	 * State.Dead 的标签事件句柄。
	 * InitializeAbilityActorInfo 会被调两次（服务端 PossessedBy / 客户端 OnRep_PlayerState），
	 * 不先摘后挂的话会挂两层，一次死亡跑两遍（布娃娃叠一次、重生传两次）。
	 */
	FDelegateHandle DeadTagDelegateHandle;

	// ---------------------------------------------------------------------
	// MoveSpeed 属性 → CharacterMovement->MaxWalkSpeed 的桥
	//
	// 属性集里 MoveSpeed 一直是「只是数据，没接 MaxWalkSpeed」（见属性集里的注释），
	// 这里把它接上。接上之后减速（UGE_Slow 乘 MoveSpeed）、以及以后任何改移速的
	// 东西（装备 / buff / 升级）都只动属性一个地方，不用各自去找 CharacterMovement。
	//
	// 两端都绑：客户端上 MaxWalkSpeed 也是本地值，不绑的话「自己看自己是不减速的」——
	// 服务端算移动，但动画和手感在本地。
	// ---------------------------------------------------------------------

	/** MoveSpeed 属性变化的事件句柄。先摘后挂，理由同 DeadTagDelegateHandle。 */
	FDelegateHandle MoveSpeedDelegateHandle;

	/** MoveSpeed 属性变了 → 同步到 MaxWalkSpeed。 */
	void OnMoveSpeedChanged(const struct FOnAttributeChangeData& Data);

	/** 把 MoveSpeed 属性的当前值写进 MaxWalkSpeed。绑定后立刻调一次做初始同步。 */
	void ApplyMoveSpeedToMovement();

	/** 布娃娃会把网格的相对变换交给物理，重生时得还原成默认值。BeginPlay 存。 */
	FVector DefaultMeshRelativeLocation = FVector::ZeroVector;
	FRotator DefaultMeshRelativeRotation = FRotator::ZeroRotator;

	/** 死亡时改过碰撞配置，重生时要还原成原来的（不写死 "Pawn"/"CharacterMesh"，BP 可能调过）。 */
	FName DefaultCapsuleCollisionProfileName;
	FName DefaultMeshCollisionProfileName;

	/** 最后打死我的人/施加者。弱引用：目标中途被销毁时 Get() 返回 null，不阻塞、不悬挂。 */
	TWeakObjectPtr<AActor> LastDamageInstigator;
	TWeakObjectPtr<AActor> LastDamageCauser;
};
