// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "GameplayTagContainer.h"
#include "HeroCombatCharacter.generated.h"

class UAbilitySystemComponent;
class UAbilitySet;
class UAnimMontage;
class UBlockComponent;
class UDodgeComponent;
class UGameplayEffect;
class UHeroDefinition;
class UHeroEvadeAnimDriver;
class UHeroStatConfig;

/**
 * 伤害从角色的哪个方位来 —— 受击动画和死亡动画都按它挑。
 *
 * 【为什么是四方向而不是八方向】素材只有四条（Kallari/Sparrow/Wukong 都是
 * Front/Back/Left/Right 这个粒度），多切几档也没有对应动画可播。
 *
 * 【左右是按「受击者自己的左右」】不是施暴者的、也不是世界的。判定在
 * AHeroCombatCharacter::ResolveHitDirection 里把来源方向转到受击者本地空间再做，
 * 所以「从我的左边打来」无论我面朝哪都是 Left。
 */
UENUM(BlueprintType)
enum class EHitDirection : uint8
{
	Front UMETA(DisplayName="正面"),
	Back  UMETA(DisplayName="背面"),
	Left  UMETA(DisplayName="左侧"),
	Right UMETA(DisplayName="右侧")
};

/** GAS 角色基类：ASC 生命周期 + 英雄技能组授权 + 普攻/槽位输入路由。ALOLCharacter 继承它。 */
UCLASS(Blueprintable)
class LOL_API AHeroCombatCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()
public:
	AHeroCombatCharacter();
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystemComponent; }

	/**
	 * 闪避 BlendSpace（BS_Evade）的反射驱动。
	 *
	 * 【当前未使用】闪避表现已改回蒙太奇路线（见 GA_Dodge / GA_GroundDodge 的类注释：
	 * ABP 里那个 BlendSpace 节点是空壳，会把未定义姿势混进输出）。这个组件留着不删，
	 * 将来若要重做 BS 无缝过渡可以直接捡回来 —— 它默认不 tick，不产生任何开销。
	 */
	UFUNCTION(BlueprintPure, Category="Hero|Anim") UHeroEvadeAnimDriver* GetEvadeAnimDriver() const { return EvadeAnimDriver; }

	/**
	 * 死亡判定。全项目唯一一处 —— 读的是 ASC 上的 State.Dead 标签，不是 Health 数值：
	 * 「能不能动」和「技能能不能放」必须是同一个判据，而技能那边挡人的就是标签
	 * （UMyGameplayAbility 的 ActivationBlockedTags）。
	 *
	 * 技能本身不用调它：TryActivateAbility → CanActivateAbility → CheckForBlocked 已经挡住了。
	 * 这个函数是给不走 GAS 的入口用的（移动、跳跃这些直接读输入的地方）。
	 */
	UFUNCTION(BlueprintPure, Category="Hero|Death") bool IsDead() const;

	/**
	 * 硬控判定（眩晕 / 击退 / 击飞）。和 IsDead 一样，读的是 ASC 上的标签，不是另立一套状态。
	 *
	 * 存在的理由和 IsDead 一样：技能那边由 UMyGameplayAbility 的 ActivationBlockedTags 挡住了
	 * （State.Stunned / State.Knockback / State.KnockUp 都在里面），但【移动和跳跃不走 GAS】——
	 * 它们在 ALOLCharacter::DoMove / DoJumpStart 里，必须自己问一句。
	 *
	 * 不做这一层会怎样：被击退的 0.5s 里玩家照常能走（MaxWalkSpeed=500），击退冲量（600）
	 * 一帧就被自己的移动输入覆盖掉 —— 表现是「击退几乎看不出来」，但代码上数值明明配了。
	 */
	UFUNCTION(BlueprintPure, Category="Hero|Death") bool IsHardControlled() const;

	/** 绑定到普攻（左键）Started 事件。 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") void BasicAttackPressed();
	/**
	 * 空格（Jump）在闪避窗口内的语义 = 派生二段 evade。
	 * ALOLCharacter::DoJumpStart 调它：窗口开着就路由成第二次闪避并返回 true（不跳），
	 * 没开就返回 false（照常跳）。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") bool TryRouteDodgeEvade();
	/** 由 PlayerController 转发的槽位输入（QWER/DF）—— 按下。 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") void AbilityInputTagPressed(FGameplayTag SlotTag);
	/**
	 * 同一个槽位键松开（PlayerController 的 Completed 事件转发过来）。
	 * 目前只有「按住选目标」这条链用得上：松开 = 关掉选择窗口，这次就当没按过。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero|Input") void AbilityInputTagReleased(FGameplayTag SlotTag);

	/** 最后打死自己的那个角色。没死过 / 已销毁时是 nullptr。给击杀提示用。 */
	UFUNCTION(BlueprintPure, Category="Hero|Death") AActor* GetLastDamageCauser() const { return LastDamageCauser.Get(); }

	/**
	 * 玩家看到的「瞄准方向」—— 由控制旋转（鼠标）决定，不是角色朝向。站位全项目唯一一处。
	 *
	 * 【为什么不能用 GetActorForwardVector()】这个项目的角色是
	 * bUseControllerRotationYaw = false + bOrientRotationToMovement = true：网格朝移动方向，
	 * 和鼠标完全无关，站着不动时甚至可能正对相机。玩家盯着屏幕中心的准心打人，
	 * 判定却按角色朝向走 —— 那就是「准心不生效」。
	 *
	 * 【bIncludePitch】两种用法差在这个开关上：
	 *   true（默认）—— 投掷这类自由弹道，仰角就是瞄准的一部分，必须留着 Pitch；
	 *   false —— 近战挥砍是水平扫，跟着 Pitch 会砍进地里（只取水平方向，不留 Pitch）。
	 *
	 * 拿不到 Pawn / 控制器时退回朝向（APawn::GetControlRotation 自己就做了这个兜底），
	 * 不返回零向量 —— 调用方拿去乘距离的话零向量会让扫描长度变 0。
	 */
	UFUNCTION(BlueprintPure, Category="Hero|Aim")
	static FVector ResolveAimDirection(const AActor* Avatar, bool bIncludePitch = true);

	// ---------------------------------------------------------------------
	// 受击 / 死亡方向
	// ---------------------------------------------------------------------

	/**
	 * 伤害从受击者的哪个方位来。站位全项目唯一一处。
	 *
	 * 【为什么转到受击者本地空间】动画是按「相对角色的前/后/左/右」做的，
	 * 直接比世界角度的话，角色一转身前后就反了。
	 * 【只取水平面】Pitch 不参与 —— 从头顶掉下来的伤害算正面，不做「上/下」这一档
	 * （素材没有对应的受击动画）。
	 * 【拿不到朝向时退回 Front】宁可播个正面的受击，也不要因为算不出方向而完全不播。
	 */
	UFUNCTION(BlueprintPure, Category="Hero|HitReact")
	static EHitDirection ResolveHitDirection(const AActor* Victim, const FVector& SourceLocation);

	/**
	 * 记下这一下是从哪来的。由 UGC_HitReact 在每一端各自调用（cue 是服务端多播过来的）。
	 *
	 * 【为什么死亡要用它】EnterDeathState 是在 State.Dead 标签复制到本端时才跑的，
	 * 那一刻手上没有「凶手在哪」这个信息（LastDamageInstigator 不进网络，只在权威端有）。
	 * 致命的那一下同样会走一遍受击 cue，各端顺手把方向记下来，死亡蒙太奇就有得挑了。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero|HitReact")
	void CacheHitDirection(EHitDirection Direction);

	/** 这个方向的受击动画。该方向没配返回 nullptr（不借用别的方向）。 */
	UFUNCTION(BlueprintPure, Category="Hero|HitReact")
	UAnimMontage* ResolveHitReactMontage(EHitDirection Direction) const;

	/** 这个方向的死亡动画。该方向没配退回不分方向的 DeathMontage。 */
	UFUNCTION(BlueprintPure, Category="Hero|Death")
	UAnimMontage* ResolveDeathMontage(EHitDirection Direction) const;

	/**
	 * 播一次受击动画。由 UGC_HitReact 调用，已经做过「致命就别播了」的判断。
	 *
	 * 内部三道门，任一不过就直接返回（受击动画是表现，宁可少播不要打架）：
	 *   ① 死亡中 —— 死亡蒙太奇在播，抢过来会让尸体站起来
	 *   ② 冷却中 —— 连发的伤害（多段/DoT）每一下都重播的话动画会一直卡在第一帧
	 *   ③ 上一条受击还没播完 —— 同上
	 */
	UFUNCTION(BlueprintCallable, Category="Hero|HitReact")
	void PlayHitReact(EHitDirection Direction);

	/**
	 * 把这个英雄的数值表按当前等级重算一遍（属性集上的 ApplyStats）。
	 *
	 * 两个调用点，都只有服务端：
	 *   ① 授予英雄技能组的同一处（InitializeAbilityActorInfo）—— 这是唯一同时满足
	 *      「服务端 + 只做一次 + ASC 已就绪」三个条件的时机；
	 *   ② 等级变了（AMyPlayerState::SetLevel 直连过来）。
	 *
	 * 幂等，重复调没有副作用（每次都是 Base + PerLevel × (Level-1) 从表重算）。
	 */
	UFUNCTION(BlueprintCallable, Category="Hero|Stats")
	void ApplyChampionStats();

	virtual void BeginPlay() override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;

protected:
	// ---------------------------------------------------------------------
	// 英雄身份：这个 Pawn 是哪个英雄
	//
	// 【HeroDefinition 一旦填了就只读它】—— 不做「按字段合并」。合并会造出
	// 「技能组来自 A、数值来自 B」这种没人一眼看得出来的组合，而那正是这份资产想消掉的东西。
	// 两套都填了会在 InitializeAbilityActorInfo 里各记一条日志（改了下面那个字段没反应，
	// 是最容易踩的坑，必须吵）。
	//
	// ChampionKit / ChampionStats 留着是为了不打断已经接好的蓝图（BP_ThirdPersonCharacter
	// 上现在填的就是 ChampionKit）。迁移完 —— 每个英雄一份 UHeroDefinition —— 就可以删掉它们。
	// ---------------------------------------------------------------------

	/** 英雄定义（技能组 + 数值 + 以后的 per-champion 的东西）。推荐填这个。 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Identity") TObjectPtr<UHeroDefinition> HeroDefinition;

	/** 英雄技能组（被动 + QWER），数据资产。HeroDefinition 填了的话以那份为准。 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Abilities") TObjectPtr<UAbilitySet> ChampionKit;

	/**
	 * 数值表（1 级值 + 每级成长）。留空 = 用属性集里的内置兜底表（= 默认英雄）。
	 * HeroDefinition 填了的话以那份为准。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Stats") TObjectPtr<UHeroStatConfig> ChampionStats;

	UPROPERTY(EditDefaultsOnly, Category="Hero|Tags") FGameplayTag PassiveSlotTag;
	UPROPERTY(EditDefaultsOnly, Category="Hero|Tags") FGameplayTag BasicAttackInputTag;

	UPROPERTY(Transient) TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	/**
	 * 格挡/免疫的减免判定（见 UBlockComponent）。
	 * 状态本身是 GE 标签（State.Blocking / State.BlockImmune），这个组件只做判定 + 记录窗口关闭时刻，
	 * 唯一调用者是 UExecCalc_Damage。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Hero|Block") TObjectPtr<UBlockComponent> BlockComponent;

	/**
	 * 完美闪避的判定与奖励（见 UDodgeComponent）。
	 * 窗口本身是 GE 标签（State.Dodge.Window，由 GE_DodgeWindow 授予），这个组件只做
	 * 判定 + 回蓝 + 子弹时间/推镜，唯一调用者是 UExecCalc_Damage。和 BlockComponent 同一套路。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Hero|Dodge") TObjectPtr<UDodgeComponent> DodgeComponent;

	/**
	 * 闪避 BlendSpace 的驱动（见 UHeroEvadeAnimDriver）。当前未使用（已回退蒙太奇路线），
	 * 保留以便将来重做 BS 无缝过渡；默认不 tick，无开销。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Hero|Anim") TObjectPtr<UHeroEvadeAnimDriver> EvadeAnimDriver;

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
	/** 闪避派生 JumpKick 的「按普攻」也要镜像到服务端：GameplayEvent 是纯本地事件，不过网络。 */
	UFUNCTION(Server, Reliable) void ServerSubmitDodgeKickInput();
	/** 闪避窗口内按空格派生「二段 evade」也要镜像到服务端。 */
	UFUNCTION(Server, Reliable) void ServerSubmitDodgeEvadeInput();

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

	/**
	 * 死亡时播的蒙太奇。每个英雄填自己那条（Kallari: AM_Death_A）。
	 * 留空 = 不播，角色原地定住。
	 *
	 * 【必须是全身槽，也就是 DefaultSlot】实测 Kallari_AnimBlueprint 里只有两个槽节点：
	 * DefaultSlot（主路径）和 UpperBody（叠在上面的层）。死亡要用 DefaultSlot ——
	 * 塞进 UpperBody 的话只有上半身倒下，腿还站着。
	 *
	 * 【资产上要关掉 Enable Auto Blend Out】开着的话播完会自动混出，尸体又站起来了。
	 *
	 * 【只在 bRagdollOnDeath = false 时生效】布娃娃和动画抢同一个骨骼网格：
	 * 网格已经被物理接管，而 AnimGraph 里的 AnimDynamics / Trail 节点还在按自己的模拟写骨骼，
	 * 两边一起写就是「四肢被拉长/切碎」的来源。二选一，不要同时开。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Death")
	TObjectPtr<UAnimMontage> DeathMontage;

	/**
	 * 按方向分的死亡蒙太奇。该方向没配 → 退回上面那条 DeathMontage（不填也能活）。
	 * 键是相对【受击者自己】的方位，见 ResolveHitDirection。
	 * 槽位同 DeathMontage：DefaultSlot。
	 *
	 * 【为什么是散开的单值属性，而不是 TMap<EHitDirection, ...>】踩过，别再改回去：
	 * TMap 写进蓝图 CDO 之后，CDO 上读得到、真正 Spawn 出来的实例却拿到一张【空表】，
	 * 于是 ResolveHitReactMontage 永远返回空、受击动画静默不播（没有任何报错）。
	 * 同一个 CDO 上的单值属性没这个问题（DeathMontage 就是证明）。散开写还有个好处：
	 * Details 面板里一眼看得出哪个方向配了、哪个方向漏了。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|Death") TObjectPtr<UAnimMontage> DeathFrontMontage;
	UPROPERTY(EditDefaultsOnly, Category="Hero|Death") TObjectPtr<UAnimMontage> DeathBackMontage;

	/**
	 * 按方向分的受击蒙太奇。留空 = 那个方向挨打没有任何动画表现。
	 *
	 * 【槽位必须用 UpperBody】受击是叠在移动上的层，不是全身动作 ——
	 * 放进 DefaultSlot（主路径）的话腿会跟着停住，走位时挨打看起来像被定身。
	 * 项目里 AM_ThrowDagger 也是 UpperBody，照着它做。
	 *
	 * 【资产上要【开】着 Enable Auto Blend Out】和死亡那边正好相反：
	 * 受击动画播完必须混回移动，关着会卡在最后一帧。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|HitReact") TObjectPtr<UAnimMontage> HitReactFrontMontage;
	UPROPERTY(EditDefaultsOnly, Category="Hero|HitReact") TObjectPtr<UAnimMontage> HitReactBackMontage;
	UPROPERTY(EditDefaultsOnly, Category="Hero|HitReact") TObjectPtr<UAnimMontage> HitReactLeftMontage;
	UPROPERTY(EditDefaultsOnly, Category="Hero|HitReact") TObjectPtr<UAnimMontage> HitReactRightMontage;

	/** 两次受击动画之间的最短间隔（秒）。连发伤害（多段/DoT）靠它不要一直重播。 */
	UPROPERTY(EditDefaultsOnly, Category="Hero|HitReact", meta=(ClampMin="0", Units="s"))
	float HitReactCooldown = 0.4f;

private:
	void InitializeAbilityActorInfo();
	void RouteBasicAttackInput();
	void RouteThrowConfirmInput();
	/** 闪避窗口内按普攻 → 派生成 JumpKick（发 Event.Input.DodgeKick，GA_Dodge 接住）。 */
	void RouteDodgeKickInput();
	/** 闪避窗口内按空格 → 派生二段 evade（发 Event.Input.DodgeEvade，GA_Dodge 接住）。 */
	void RouteDodgeEvadeInput();
	bool bAbilitiesGranted = false;

	/**
	 * 数值只施加一次。和 bAbilitiesGranted 【分开】两个旗标：技能组可以是空的
	 * （还没配技能的英雄照样该有数值），共用一个旗标会让那种情况永远走不到数值那一步。
	 */
	bool bStatsApplied = false;

	/** 技能组 / 数值表各读哪一份 —— HeroDefinition 优先。见头文件那段。 */
	UAbilitySet* ResolveChampionKit() const;
	UHeroStatConfig* ResolveChampionStats() const;

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
	// 【语义：MoveSpeed 是「目标最大速度」，不是「当前速度」】
	// 属性集里 MoveSpeed 一直是「只是数据，没接 MaxWalkSpeed」（见属性集里的注释），
	// 这里把它接上。接上之后减速（UGE_Slow 乘 MoveSpeed）、以及以后任何改移速的
	// 东西（装备 / buff / 升级）都只动属性一个地方，不用各自去找 CharacterMovement。
	//
	// 【为什么是「目标」而不是「当前」】
	// UE 的移动是**渐进**的：MaxWalkSpeed 只是上限，实际速度由 MaxAcceleration
	// 决定爬升斜率、BrakingDecelerationWalking 决定减速斜率。所以「移速变了」应该是
	// 改【上限】，让角色自然地加速过去 —— 而不是把速度直接设成新值（那会瞬移）。
	// 这也是拟真游戏（LoL / 英雄联盟类）的标准做法：疾跑起步有明显的加速过程。
	//
	// 【中途变速怎么表现】
	// 改了属性 → MaxWalkSpeed 变 → 当前速度按 MaxAcceleration 渐进追上去 ⇒
	// 动画的 GroundSpeed（Velocity.Size2D）也是连续变化的 ⇒ BlendSpace 自然插值，
	// **不需要为"中途变速"做任何额外适配**。
	//
	// 两端都绑：客户端上 MaxWalkSpeed 也是本地值，不绑的话「自己看自己是不减速的」——
	// 服务端算移动，但动画和手感在本地。
	// ---------------------------------------------------------------------

	/** MoveSpeed 属性变化的事件句柄。先摘后挂，理由同 DeadTagDelegateHandle。 */
	FDelegateHandle MoveSpeedDelegateHandle;

	/**
	 * 起步加速度（cm/s²）。UE 原生参数，引擎默认 2048；本项目在构造函数里显式设成
	 * `MoveAccelSpeed` 以便和 MoveSpeed 语义成对（一个是上限、一个是到上限的斜率）。
	 *
	 * 数值感受（MoveSpeed=345 时）：
	 *   2048 → 约 0.17s 达到满速，偏「爽快」
	 *   1200 → 约 0.29s，接近 LoL 疾跑的「有起步但不久」
	 * ⚠️ 别设太大：一旦超过 MaxWalkSpeed/(0.1~0.2s) 就等于没有加速过程了。
	 */
	UPROPERTY(EditDefaultsOnly, Category="Anim|Movement", meta=(ClampMin="0", Units="cm/s^2"))
	float MoveAccelSpeed = 1200.f;

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

	/**
	 * 最近一次受击的方位。由 UGC_HitReact 在【每一端各自】记下（cue 是服务端多播过来的），
	 * 死亡时 EnterDeathState 拿它挑死亡蒙太奇 —— 那条路径上手上没有凶手的信息，
	 * 原因见 CacheHitDirection 的注释。
	 *
	 * 【故意不是 UPROPERTY】纯本端表现缓存，不复制、不进存档、不该出现在 Details 面板里。
	 * 各端各自记各自的，结果一致（cue 参数是同一份）。
	 */
	EHitDirection LastHitDirection = EHitDirection::Front;
	bool bHasHitDirection = false;

	/** 上一次播受击动画的世界时间，配 HitReactCooldown 用。 */
	float LastHitReactTime = -1000.f;
};
