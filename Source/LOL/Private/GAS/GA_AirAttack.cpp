// 空中攻击的实现。架构理由见头文件。

#include "GAS/GA_AirAttack.h"
#include "GAS/GA_ThreeHitPassive.h"
#include "GAS/LOLGameplayTags.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimMontage.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

UGA_AirAttack::UGA_AirAttack()
{
	// 父类构造函数已经设了 InstancingPolicy / NetExecutionPolicy / ActivationBlockedTags(Silenced)
	// / bBreaksStealthOnCast / HitCueTag，这里不重复设。

	// 空中攻击是被 RouteBasicAttackInput 显式 TryActivateAbility 激活的，
	// 走的是和地面普攻一样的输入路径（本地预测 → 服务端权威结算）。
	// 父类已经是 OnInputTriggered，这里写出来是为了让读代码的人看到「它不走事件订阅」。
	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;
}

// ⚠️ 参数按值传，和基类 UGA_FormMelee 逐字一致（写成 const 引用会导致 override 匹配失败，
//   报错是「包含重写说明符"override"的方法没有重写任何基类方法」，看不出是签名问题）。
void UGA_AirAttack::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// ---------------------------------------------------------------------
	// ① 空中判据。
	//
	// RouteBasicAttackInput 已经判过一次 IsFalling 才激活本能力，这里【再判一遍】：
	//   ① 角色可能在这一帧刚好落地（从 Route 判到激活之间隔了一次网络/RPC 往返）；
	//   ② 任何别的代码路径误激活它时，这里是唯一的闸门。
	// 不在空中就安静地结束 —— 不播动画、不结算、不打警告日志：
	// 「地面按左键」是完全正常的输入，警告日志会把它当成异常事件来报。
	// ---------------------------------------------------------------------
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	const UCharacterMovementComponent* MoveComp = Character ? Character->GetCharacterMovement() : nullptr;
	const bool bAirborne = MoveComp && MoveComp->IsFalling();

	if (!bAirborne)
	{
		UE_LOG(LogTemp, Verbose, TEXT("[AirAttack] 不在空中 → 本能力不接管（地面左键归连招/三连普攻）"));
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
		return;
	}

	// ---------------------------------------------------------------------
	// ①.5 地面连招进行中不让空中攻击插进来。
	//
	// 连招（三连普攻 / 空手四连拳）进行中起跳再按攻击：RouteBasicAttackInput 看到
	// 「空中=1」就会激活本能力，于是两条能力同时往同一个 slot 塞蒙太奇 ——
	// 连招第 N 段的动作被空中攻击顶掉、落地后连招状态和能力实例都对不上
	//（用户 2026-10-03 实测的「出招错乱」第二来源，日志里 TryActivateAbility=1 那条）。
	// 地面连招 Family = UGA_ThreeHitPassive 及其派生（InstancedPerActor，实例在跑 =
	// Spec.IsActive()）。空中攻击让位而不是打断连招：起跳应该只影响移动，不抢招。
	// ---------------------------------------------------------------------
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
		{
			if (Spec.Ability && Spec.Ability->IsA(UGA_ThreeHitPassive::StaticClass()) && Spec.IsActive())
			{
				UE_LOG(LogTemp, Verbose, TEXT("[AirAttack] 地面连招（%s）进行中 → 空中攻击让位"),
					*Spec.Ability->GetClass()->GetName());
				EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
				return;
			}
		}
	}

	// ---------------------------------------------------------------------
	// ② 按形态选蒙太奇。
	//
	// 判据读 State_Form.Unarmed（由 GA_FormSwitch 切的 GE 上的标签）。
	// ⚠️ 形态切换是【服务端施加 GE → 标签复制到客户端】，所以刚切完形态的极短窗口内
	//   客户端可能还没收到标签 → 会选错形态的动画。影响是一帧的视觉，
	//   形态切换本身有 0.9s 的蒙太奇过渡，体感上察觉不到。
	//
	//   真要严格的话可以让 GA_FormSwitch 在客户端预测阶段就 AddLooseGameplayTag，
	//   但那会让标签在非权威端也变成"看起来切好了"（而 GE 还没到），
	//   反而会引出"标签说有、GE 说没有"的分叉 —— 不值得。
	// ---------------------------------------------------------------------
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	const bool bUnarmed = ASC && ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed);

	UAnimMontage* Chosen = bUnarmed ? UnarmedMontage.Get() : ArmedMontage.Get();

	if (!Chosen)
	{
		// 只有「这一形态没配动画」才算配错，值得说出来（否则玩家看到的是"空中打空气"）。
		UE_LOG(LogTemp, Warning, TEXT("[AirAttack] %s 形态=%s 但没配 Montage（UnarmedMontage=%s / ArmedMontage=%s）→ 本次不播动画"),
			*GetName(), bUnarmed ? TEXT("空手") : TEXT("持剑"),
			*GetNameSafe(UnarmedMontage.Get()), *GetNameSafe(ArmedMontage.Get()));
	}

	// 父类 ActivateAbility 里就是拿 Montage 这个成员去播的（见 GA_FormMelee.cpp:196）。
	// 所以这里只赋值、不自己播 —— 播动画、订命中通知、收招都由父类那条链负责。
	Montage = Chosen;

	UE_LOG(LogTemp, Log, TEXT("[AirAttack] 空中攻击: 形态=%s 蒙太奇=%s"),
		bUnarmed ? TEXT("空手") : TEXT("持剑"), *GetNameSafe(Chosen));

	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}
