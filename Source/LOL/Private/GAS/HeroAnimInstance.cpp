// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroAnimInstance.h"

#include "GAS/HeroAnimationSet.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

void UHeroAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	OwnerCharacter = Cast<ACharacter>(TryGetPawnOwner());
	bTagsDirty = true;

	// 【这里故意不取 ASC】。本项目的 ASC 挂在 PlayerState 上（见
	// AHeroCombatCharacter::InitializeAbilityActorInfo），而客户端上 AnimInstance
	// 的创建早于 PlayerState 复制到位 —— 这一帧拿不到是正常的，不是错误。
	// 在 PossessedBy 里"顺手"补一次也没用，那个回调在两端的时机同样不保证。
	// 所以第一次真正的解析交给 NativeUpdateAnimation 每帧重试。
}

void UHeroAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	UpdateGroundSpeed();

	if (!TryResolveAbilitySystem())
	{
		// ASC 还没到（客户端上很常见）。速度已经更新了，移动层照常能用；
		// 标签相关的选择维持上一帧的结果而不清空 —— 清空的话每次重生/重连
		// 都会看到动画闪一下再回来。
		return;
	}

	if (bTagsDirty)
	{
		CachedASC->GetOwnedGameplayTags(ActiveTags);
		bTagsDirty = false;
		RefreshAnimSelection();
	}

	UpdateLocomotionPlayRate();
}

void UHeroAnimInstance::NativeUninitializeAnimation()
{
	// AnimInstance 销毁时不主动摘的话，ASC 上会留着指向已死对象的委托。
	// 引擎通常也能收拾，但泛型标签事件是全局单播点，挂着不放会一直往这边投递。
	UnbindTagEvent();
	CachedASC.Reset();
	OwnerCharacter.Reset();

	Super::NativeUninitializeAnimation();
}

bool UHeroAnimInstance::TryResolveAbilitySystem()
{
	if (CachedASC.IsValid())
	{
		return true;
	}

	ACharacter* Character = OwnerCharacter.Get();
	if (!Character)
	{
		// 极端情况下 NativeInitializeAnimation 时 Pawn 还没绑上。
		// 每帧重试一次，绑上了就缓存住。
		Character = Cast<ACharacter>(TryGetPawnOwner());
		if (!Character)
		{
			return false;
		}
		OwnerCharacter = Character;
	}

	// 复用 UMyAbilitySystemComponent::FindAbilitySystemComponent 而不是自己 Cast：
	// 它已经处理了「ASC 在 PlayerState 上 / 在 Pawn 上 / 是 Actor 的组件」三条路。
	// 走同一个入口，行为才和项目里别处（选目标、伤害结算）一致。
	UAbilitySystemComponent* ASC = UMyAbilitySystemComponent::FindAbilitySystemComponent(Character);
	if (!ASC)
	{
		return false;
	}

	// 先摘干净再换指针：UnbindTagEvent 用的是【旧的】CachedASC，
	// 顺序反了就会拿新 ASC 去摘旧句柄（无害但也没摘掉旧的）。
	UnbindTagEvent();
	CachedASC = ASC;

	// 泛型标签事件：任何一个标签的增删都会回调，一个句柄覆盖全部状态标签，
	// 不用为 State.Slowed / State.Stunned / State.Stealth ... 各注册一次。
	TagChangedDelegateHandle = ASC->RegisterGenericGameplayTagEvent()
		.AddUObject(this, &UHeroAnimInstance::OnAnyTagChanged);

	// 刚绑上，之前那份快照是空的了，强制重算一次。
	bTagsDirty = true;

	UE_LOG(LogTemp, Verbose, TEXT("[HeroAnim] %s 绑定 ASC %s，开始跟踪标签"),
		*GetNameSafe(Character), *GetNameSafe(ASC));

	return true;
}

void UHeroAnimInstance::OnAnyTagChanged(const FGameplayTag Tag, int32 NewCount)
{
	// 只置脏，不在这里重算：挂一个 GE 可能一次授予/移除好几个标签，
	// 每个都触发全表重挑是白烧。真正的重算留到下一帧的 NativeUpdateAnimation 做一次。
	UE_LOG(LogTemp, Verbose, TEXT("[HeroAnim] %s 标签变化 → count %d"),
		*Tag.ToString(), NewCount);

	bTagsDirty = true;
}

void UHeroAnimInstance::RefreshAnimSelection()
{
	// 先清干净：AnimSet 被清空、或规则被改空之后，不该留着上一次的结果。
	CurrentLocomotion = nullptr;
	CurrentOverride = nullptr;
	bOverrideActive = false;
	bOverrideLoop = true;
	CachedSpeedReference = 0.f;

	if (!AnimSet)
	{
		return;
	}

	// 两份都算。覆盖生效时移动层用不上，但让 CurrentLocomotion 一直有效的话，
	// 覆盖结束那一帧不用等下一次标签事件就能立刻回到正确的混合空间。
	const FHeroLocomotionChoice Locomotion = AnimSet->ResolveLocomotion(ActiveTags);
	CurrentLocomotion = Locomotion.BlendSpace;
	CachedSpeedReference = Locomotion.SpeedReference;

	const FHeroStateOverrideChoice Override = AnimSet->ResolveStateOverride(ActiveTags);
	if (Override.bValid)
	{
		CurrentOverride = Override.Anim;
		bOverrideLoop = Override.bLoop;
		bOverrideActive = true;
	}

	UE_LOG(LogTemp, Verbose,
		TEXT("[HeroAnim] %s 重挑动画：标签=%d 条 → BS=%s 基准速度=%.1f 覆盖=%s(loop=%d)"),
		*GetNameSafe(OwnerCharacter.Get()),
		ActiveTags.Num(),
		*GetNameSafe(CurrentLocomotion),
		CachedSpeedReference,
		*GetNameSafe(CurrentOverride),
		bOverrideLoop ? 1 : 0);
}

void UHeroAnimInstance::UpdateGroundSpeed()
{
	const ACharacter* Character = OwnerCharacter.Get();
	if (!Character)
	{
		GroundSpeed = 0.f;
		return;
	}

	// Size2D() 剥掉垂直分量：跳起/下落时 Velocity 带 Z，不剥的话人在空中会读出一个
	// 很大的「水平速度」，混合空间直接冲到最快那一格。
	//
	// ⚠️ 前提是本项目的移动蒙太奇没开 root motion —— 角色是被 AddMovementInput
	// 推着走的（见 ALOLCharacter::DoMove），所以 Velocity 就是真实的移动速度。
	// 以后要是给 Jog/Sprint 蒙太奇开了 root motion，这一句得改成读蒙太奇自己的位移，
	// 否则速度是 0、混合空间永远停在 idle 格。
	GroundSpeed = Character->GetCharacterMovement()->Velocity.Size2D();
}

void UHeroAnimInstance::UpdateLocomotionPlayRate()
{
	// 基准速度 <= 0（没选中混合空间，或那条规则没配基准）= 不补，恒 1。
	if (CachedSpeedReference <= 0.f)
	{
		LocomotionPlayRate = 1.f;
		return;
	}

	// 速度趋近 0 时不补偿：这时混合空间已经落在 idle 那一格，除出来的 PlayRate
	// 会趋近 0，把 idle 冻成一帧（连呼吸都停了）。死区取 1cm/s，基本等于「真的没动」。
	//
	// ⚠️ 已知代价：极慢速（比如减速 99%，只剩 3cm/s）会落进死区、PlayRate 回到 1，
	// 那段速度下步频对不上、还是会滑一点。彻底解决要混合空间自带 idle 格
	// 并用 Sync Group 对齐，属于美术侧的事；C++ 这层先给一个「不冻住」的近似。
	if (GroundSpeed < 1.f)
	{
		LocomotionPlayRate = 1.f;
		return;
	}

	// 上下限是防呆：基准速度配错一个数量级时不至于把动画播成快进或几乎静止。
	LocomotionPlayRate = FMath::Clamp(GroundSpeed / CachedSpeedReference, 0.25f, 3.f);
}

void UHeroAnimInstance::UnbindTagEvent()
{
	if (!TagChangedDelegateHandle.IsValid())
	{
		return;
	}

	if (UAbilitySystemComponent* ASC = CachedASC.Get())
	{
		// 泛型标签事件没有专门的 Unregister 函数，直接在返回的那个多播委托上摘
		// （见 AbilitySystemComponent.h:729，只有 Register 没有 Unregister 配对）。
		ASC->RegisterGenericGameplayTagEvent().Remove(TagChangedDelegateHandle);
	}
	// ASC 已经没了的话，句柄随它一起销毁，这里只是清掉本地记录。

	TagChangedDelegateHandle.Reset();
}
