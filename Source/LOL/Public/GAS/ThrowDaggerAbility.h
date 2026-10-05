// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GameplayEffectTypes.h"
#include "TimerManager.h"
#include "ThrowDaggerAbility.generated.h"

class AThrowDaggerProjectile;
class UAbilityTask_PlayMontageAndWait;
class UGameplayEffect;
class UAnimMontage;
class ACharacter;
class USoundBase;

/**
 * 投掷匕首：E 进瞄准 → 左键投掷 / E 再按取消。直线飞行、空中翻滚。
 */
UCLASS(Blueprintable)
class LOL_API UThrowDaggerAbility : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UThrowDaggerAbility();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CancelAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateCancelAbility) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool WasCanceled) override;

protected:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Projectile")
	TSubclassOf<AThrowDaggerProjectile> Dagger;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Projectile")
	float ThrowSpeed = 1800.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Damage")
	TSubclassOf<UGameplayEffect> DamageGE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Damage")
	float BaseDamage = 80.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Aim")
	TSubclassOf<UGameplayEffect> AimingGE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Anim")
	TObjectPtr<UAnimMontage> ThrowDaggerMontage;

	// --- 海克斯「三连飞刃」三件旋钮（只在身上有 Hex.ThrowDagger.Triple 标签时生效）---

	/**
	 * 一次投出几把。1 = 原来的单发。
	 * 配了 3 但角色身上没有那个海克斯标签 ⇒ 仍然是单发（判据在 ResolveSpawnCount）。
	 */
	UPROPERTY(EditAnywhere,BlueprintReadOnly,Category="HexAugment")
	int32 AugmentCount = 3;

	/**
	 * 扇形总张角（度）。3 把时是 -Spread/2 / 0 / +Spread/2 三路。
	 * 12 是「看起来是一记扇形」的经验值；超过 30 三把会散得像霰弹枪，
	 * 近距离反而不好瞄 —— 而「准心指谁就一定打中谁」是这条设计必须守住的那条线。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="HexAugment")
	float SpreadAngle = 12.f;

	/**
	 * 每一把的伤害缩放（相对 BaseDamage），只在【多发】时生效（单发逐位保持原数值）。
	 *
	 * ⚠️ 这是 E 的总伤倍率：填 1.0 = 三发各自满伤 = 总伤 3 倍。原来 80 伤害 + 6 秒 CD 的 E
	 * 偏重，建议 0.6~0.7（总伤 1.8~2.1 倍）。单发时这个值被忽略。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="HexAugment")
	float SingleDamageScale = 0.7f;

	/**
	 * 多发时每把匕首沿【自己的方向】前推多少（cm），用来避免同帧生成时互相重叠。
	 *
	 * ⚠️⚠️ 【ClampMin 25 不是随便取的】= 匕首碰撞球直径 24（半径 12）+ 1cm 余量。
	 *   小于球体直径的话出生那一帧仍然重叠 ⇒ 同伴互撞。
	 *   症状表现随重叠的先后顺序变化，很容易被误读成别的 bug：
	 *     · 三把全重叠 → 「三发只剩最后一把」（前两把互触发 Destroy，第三把生成时
	 *       前两把已在销毁流程里，检测不到它）—— **这不是随机的，是对称互杀的必然结果**；
	 *     · 间距再小一点 → 「在起点就爆炸」。
	 *   夹紧下界是为了让「配错值」这件事在编辑器里就做不出来。
	 *   即便真被改小，第二层（ThrowDaggerProjectile::OnOverlap 挡同伴）仍会兜底，
	 *   只是兜底的表现是「销毁后来的那把」而不是「爆炸」。
	 *
	 * 单发时【完全不使用】这个值 —— 逐位保持改动前的出生点。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HexAugment", meta = (ClampMin = "25", UIMin = "25", Units = "cm"))
	float MultiSpawnSpacing = 30.f;

	// 瞄准轮廓粒子（AimingReticleFX / AimingOutlineSocketName）搬到 GameplayCue 了，
	// 见 AGC_ThrowAiming。能力只负责 Add/RemoveGameplayCue，不用再存组件指针、自己销毁。

	/**
	 * 出手（匕首脱手）时的一次性音效。默认 Kallari_Effort_Ability_E_Throw。留空 = 出手无声。
	 * 进瞄准的抬手声在 AGC_ThrowAiming 上，命中声在 UGC_ThrowDaggerHit 上，三个点各管各的。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Sound")
	TSoftObjectPtr<USoundBase> ThrowSound;

	// 出手 socket（右手）。只用来【定位】：出生点取它在准心射线上的投影，不是直接用它的坐标。
	// 见 ResolveSpawnLocation —— 直接用坐标的话，路径会和准心平行、近距离整段偏出去。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Projectile")
	FName ThrowSocketName = FName("Melee_Impact_R");

	// 左键确认 → 真正出手的延迟（秒），要匹配 Cast 动画的出手帧，不能超过 Cast 段时长。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Anim")
	float CastThrowDelay = 0.35f;

	/**
	 * 瞄准态的兜底超时（秒）。到点还没投也没取消就自动退出瞄准。
	 *
	 * ⚠️⚠️ 这不是手感调节，是**正确性**保障。
	 * 瞄准态挂着一个 loose tag `State.Throw.Aiming`，而它负责左键分流和普攻阻断。
	 * 能力激活后只挂了两个 WaitGameplayEvent（ThrowConfirm / Repressed）—— 玩家
	 * 既不按左键、也不再按 E（比如切窗口、Alt-Tab、手柄断开、或者干脆就是想站着不动），
	 * 就**没有任何东西会结束这个能力**：EndAbility 不会被调，State.Throw.Aiming
	 * 一直挂着 ⇒ 左键一直被分流、普攻永久失效、移动输入被吃掉。
	 * 蒙太奇那条自动出口也指望不上：Targeting 是循环段，而 OnThrowMontageFinished
	 * 第一行就是 `if (!bThrowCommitted) return;`（没确认过就早退）。
	 * 所以必须有一个不依赖玩家输入的兜底出口。
	 *
	 * 超时按「取消」处理（WasCanceled = true），不 commit ⇒ **不消耗 CD**，
	 * 和玩家自己按 E 取消完全等价。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Aim", meta = (ClampMin = "0.5", UIMin = "0.5", Units = "s"))
	float AimingTimeoutSeconds = 4.f;

	FActiveGameplayEffectHandle GEHandle;
	UPROPERTY() TObjectPtr<UAbilityTask_PlayMontageAndWait> MontageTask = nullptr;
	FTimerHandle ThrowDelayTimer;  // 左键确认 → 真正出手的延迟 timer
	FTimerHandle TurnTimer;        // 出手前平滑转向的逐帧 timer
	FTimerHandle PostRestoreTimer; // 延迟恢复 bOrientRotationToMovement 的一次性 timer
	FTimerHandle AimingTimeoutTimer; // 瞄准态兜底超时（见 AimingTimeoutSeconds）

	// 左键确认后置 true；等 Cast 段播完（OnBlendOut/OnCompleted）才真正 End，否则 EndAbility 会立刻停掉 Montage。
	bool bThrowCommitted = false;

	/**
	 * 确认那一刻锁定的瞄准方向（= 准心射线的方向，来自控制旋转），
	 * 投掷延迟期间鼠标再动也不影响本次出手。
	 */
	FVector ThrowDirection = FVector::ForwardVector;

	/**
	 * 确认那一刻的准心射线【起点】（相机位置）。和 ThrowDirection 同帧一起锁 ——
	 * 出生点要落在这条射线上（见 ResolveSpawnLocation），射线和方向不同帧的话，
	 * 出生点会贴到一条已经不对的线上。
	 *
	 * bHasThrowRayOrigin = false：当时拿不到相机（没控制器 / 专用服务器上的远端角色），
	 * 退回从手部 socket 直接出，也就是改动之前的行为。
	 */
	FVector ThrowRayOrigin = FVector::ZeroVector;
	bool bHasThrowRayOrigin = false;

	/**
	 * 出手时人物要转到的朝向（确认那一刻锁定的瞄准方向，只取 Yaw）。
	 * 关掉 bOrientRotationToMovement 之后靠 TickTurnToFace 逐帧插值到它 ——
	 * 出手帧正好转到位，手臂和匕首路径才在一个方向上。
	 */
	FRotator TargetFacingRotation = FRotator::ZeroRotator;
	FRotator TurnStartRotation = FRotator::ZeroRotator;   // 转向起点（确认时的朝向）
	float TurnElapsed = 0.f;                              // 转向已用时长（秒）

	/**
	 * 出手期间临时关掉 bOrientRotationToMovement，让角色一直朝瞄准方向（只转一次），
	 * 能力结束（ExitAimingState）才恢复 —— 中途恢复的话移动组件会立刻把角色转回移动方向，
	 * 和刚才的转向连在一起就是「转两下 + 镜头抖动」。
	 * bOrientRotationOverridden 标记是否真的关过，防止 ExitAimingState 在还没转向时误恢复成 false。
	 */
	bool bWasOrientRotationToMovement = false;
	bool bOrientRotationOverridden = false;

	/**
	 * 出手后角色保持瞄准朝向的时长（秒）。这段时间内不恢复 bOrientRotationToMovement，
	 * 避免移动组件在出手瞬间立刻把角色转回移动方向（一次很显眼的「小修正」）。过了这段时间再恢复。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Throw|Anim")
	float PostCastHoldDuration = 0.3f;

	void EnterAimingState();
	void ExitAimingState();
	void FireDagger();
	void StartTurnToFace(const FVector& AimDir);
	void TickTurnToFace();
	void RestoreOrientRotationToMovement();
	FVector GetThrowSocketLocation(ACharacter* Character) const;
	/** 准心射线的起点 = 相机世界坐标。拿不到返回 false（调用方退回手部 socket）。 */
	bool TryGetAimRayOrigin(FVector& OutOrigin) const;
	/** 出生点 = 手部 socket 在准心射线上的投影。为什么不是 socket 本身，见 .cpp。 */
	FVector ResolveSpawnLocation(ACharacter* Character) const;
	int32 ResolveSpawnCount()const;
	UFUNCTION() void OnThrowPressed(FGameplayEventData Payload);
	UFUNCTION() void OnCancelPressed(FGameplayEventData Payload);
	UFUNCTION() void OnThrowMontageFinished();
	/** 瞄准态兜底超时：等价于「玩家按了 E 取消」，不消耗 CD。理由见 AimingTimeoutSeconds。 */
	void OnAimingTimeout();
	FVector GetAimingDirection() const;
	void SpawnProjectile(const FVector& AimDir);
};
