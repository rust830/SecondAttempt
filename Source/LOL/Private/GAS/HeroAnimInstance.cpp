// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/HeroAnimInstance.h"

#include "GAS/HeroAnimationSet.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyAbilitySystemComponent.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "KismetAnimationLibrary.h"   // 5.8 里 UKismetAnimationLibrary 在 AnimGraphRuntime 模块，不是 Kismet
#include "Components/SkeletalMeshComponent.h"

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

	// FormUnarmed 每帧向目标值平滑逼近 —— 这是整个形态管线里 FormUnarmed 的唯一写入点。
	// 放在 ASC 解析之前：标签变化（bTagsDirty → RefreshAnimSelection）只重算 FormUnarmedTarget，
	// 解析失败的那几帧过渡照常进行、不中断。FormBlendSpeed = 0 是瞬切兜底。
	//
	// 【空中强制回持刀】sprint 上身混合（ABP 的 LBPB 那层）只覆盖地面移动——
	// 跳跃/下落/闪避飞行时腿是 Jump 动画、上身却还是疾跑摆臂，必然穿帮。
	// 目标值按 IsFalling 折算成 0，FInterpTo 负责平滑：起跳渐变回持刀、
	// 落地渐变回空手，没有硬切。EvaluteFormTarget 每帧跑，标签没变也能响应离地。
	const bool bAirborne = OwnerCharacter.IsValid() && OwnerCharacter->GetCharacterMovement()
		&& OwnerCharacter->GetCharacterMovement()->IsFalling();
	const float EffectiveFormTarget = bAirborne ? 0.f : FormUnarmedTarget;
	if (FormBlendSpeed > 0.f)
	{
		FormUnarmed = FMath::FInterpTo(FormUnarmed, EffectiveFormTarget, DeltaSeconds, FormBlendSpeed);
	}
	else
	{
		FormUnarmed = EffectiveFormTarget;
	}

	UpdateGroundSpeed();
	UpdateMovementDirection();

	// 【脚步 IK 权重】只管地面跑动：空中/静止一律 0。
	// 见头文件里 FootIKAlpha 的注释（为什么不能常开 —— 会顶髋）。
	// bAirborne 在上面算过了（IsFalling）；GroundSpeed 在上面刚更新。
	FootIKAlpha = (!bAirborne && GroundSpeed > FootIKMinSpeed) ? 1.f : 0.f;

	// 【空手藏剑】weapon_l / weapon_r（lowerarm 子骨）→ sword_root_l/r → sword_handle_l/r
	// 是主武器的蒙皮骨架，整把剑的顶点都绑在这棵子树上。空手 idle（Idle_TravelMode 系）
	// 把武器骨摆在背鞘位所以看不见；但跑步上身一旦混到 JogFwdSlopeLean（样点是持武器跑），
	// 武器骨被摆回右手握持位 —— 剑就"闪现"到手里。剑没有独立网格组件，唯一的可靠藏法
	// 就是 HideBone：直接把该骨及子树的蒙皮几何整个关掉，切回持剑形态再恢复。
	// 阈值 0.5 与 ABP 外层 TwoWayBlend 的可视语义一致（哪边权重过半算哪边）。
	// 注意：dagger_base_l/r（两把匕首）是 lowerarm 下另一棵独立子树，这里不碰。
	if (ACharacter* Character = OwnerCharacter.Get())
	{
		if (USkeletalMeshComponent* MeshComp = Character->GetMesh())
		{
			static const FName WeaponL(TEXT("weapon_l"));
			static const FName WeaponR(TEXT("weapon_r"));
			// ⚠️⚠️ 判据用 FormUnarmedTarget（真实形态）而不是 FormUnarmed（平滑后的权重）。
			//   FormUnarmed 会被上面那段「空中强制回持刀」折成 0（IsFalling 时目标值取 0，
			//   因为空中腿是 Kallari 自带的 Jump 动画、姿势必须配套回持刀）——
			//   而藏剑跟着它走的话，**空手形态一按下跳跃就会把剑亮出来**（用户 2026-10-03 实测）。
			//   「动画回持刀」和「有没有剑」是两件事：姿势可以回持刀，剑必须按真实形态藏。
			// bSwordForceVisible：拔刀 montage 期间由 GA_FormSwitch 置位，短路藏剑判定，
			// 让「拔刀」的全过程剑都在手里可见（否则要等 FormUnarmed 平滑过 0.5 才出现）。
			const bool bWantHide = FormUnarmedTarget > 0.5f && !bSwordForceVisible;

			const bool bNowHidden = MeshComp->IsBoneHiddenByName(WeaponL);
			if (bWantHide && !bNowHidden)
			{
				MeshComp->HideBoneByName(WeaponL, EPhysBodyOp::PBO_None);
				MeshComp->HideBoneByName(WeaponR, EPhysBodyOp::PBO_None);
				// 【装饰骨一起藏】见下面那段的长注释。
				HideUnarmedDecorationBones(MeshComp, true);
			}
			else if (!bWantHide && bNowHidden)
			{
				MeshComp->UnHideBoneByName(WeaponL);
				MeshComp->UnHideBoneByName(WeaponR);
				HideUnarmedDecorationBones(MeshComp, false);
			}
		}
	}

	if (!TryResolveAbilitySystem())
	{		// ASC 还没到（客户端上很常见）。速度已经更新了，移动层照常能用；
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
	// 形态目标值在这里重算（而不是 AnimSet 的规则表里）：
	// 它是「身上有没有 State.Form.Unarmed」的单条布尔事实，不需要 Query 引擎，
	// 而且它必须在 AnimSet 判空的 early-return 之前 —— 没配动画表的英雄形态切换照样要工作。
	FormUnarmedTarget = CachedASC.IsValid() && CachedASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed) ? 1.f : 0.f;

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

void UHeroAnimInstance::HideUnarmedDecorationBones(USkeletalMeshComponent* MeshComp, bool bHide)
{
	// =========================================================================
	// 【为什么藏这些骨 —— 重定向动画的固有缺口，不是配置疏漏】
	//
	// IK Retargeter 只驱动 IK Rig 链上的骨，也就是【源骨架（Manny）有的那批】。
	// 目标骨架（Kallari）独有的骨在重定向出来的动画里【一条轨道都没有】，
	// 于是它们全部停在 mesh 的 ref pose 上：
	//
	//   waste_hood_* / hood_*_dyn   背后兜帽 + 动态兜帽
	//   tentacle_*                  背上的触须（Kallari 的招牌）
	//   thruster*                   背部推进器
	//
	// 持刀形态看不出来（Kallari 自带动画驱动全部 141 根骨，它们会正常摆动），
	// 一进重定向动画（空手 idle / 走 / 跳）就停在 ref pose ⇒ 从背后支棱出来。
	// 用户 2026-10-03 实测：「idle 模型用的是 travelmode 的，他的背包多露出了一些骨骼」。
	//
	// 藏它们和藏剑是同一个机制（HideBone 关掉该骨及子树的蒙皮几何），所以复用。
	// 判据跟着藏剑走（FormUnarmedTarget），空手才藏、持刀原样恢复。
	//
	// ⚠️⚠️ 【故意不藏的骨】
	//   upperarm_twist_01/02_* / thigh_twist_* / calf_twist_01_*
	//   —— 这些是【身体内部的扭转骨】，藏了会破坏蒙皮（手臂/腿会变形）。
	//   它们同样不被重定向驱动，这正是用户说的「全身骨骼排列有微妙差异、动作很僵硬」，
	//   但要解决只能给重定向动画补上这些骨的轨道（从 Kallari 自带动画拷对应轨道，
	//   或者在 IK Rig 里把它们加进链），不是藏能解决的 —— 留作后续专项。
	// =========================================================================
	static const FName DecorationBones[] = {
		// 兜帽（含 ref pose 里最容易支棱出来的后背那一片）
		TEXT("waste_hood_bk"), TEXT("waste_hood_fr_l"), TEXT("waste_hood_fr_r"),
		TEXT("waste_hood_l"), TEXT("waste_hood_r"),
		TEXT("hood_l_dyn"), TEXT("hood_r_dyn"), TEXT("hood_c_dyn"),
		// 触须
		TEXT("tentacle_spring_l"), TEXT("tentacle_spring_r"),
		TEXT("tentacle_l_01"), TEXT("tentacle_l_02"), TEXT("tentacle_l_03"), TEXT("tentacle_l_04"),
		TEXT("tentacle_l_05"), TEXT("tentacle_l_06"), TEXT("tentacle_l_07"), TEXT("tentacle_l_08"),
		TEXT("tentacle_l_09"), TEXT("tentacle_l_010"),
		TEXT("tentacle_r_01"), TEXT("tentacle_r_02"), TEXT("tentacle_r_03"), TEXT("tentacle_r_04"),
		TEXT("tentacle_r_05"), TEXT("tentacle_r_06"), TEXT("tentacle_r_07"), TEXT("tentacle_r_08"),
		TEXT("tentacle_r_09"), TEXT("tentacle_r_010"),
		// 背部推进器
		TEXT("thruster_casing_l"), TEXT("thruster_casing_r"),
		TEXT("thruster_a_l"), TEXT("thruster_b_l"), TEXT("thruster_c_l"),
		TEXT("thruster_a_r"), TEXT("thruster_b_r"), TEXT("thruster_c_r"),
		TEXT("thrusterVent_l_01"), TEXT("thrusterVent_l_02"),
		TEXT("thrusterVent_r_01"), TEXT("thrusterVent_r_02"),
	};

	for (const FName& Bone : DecorationBones)
	{
		if (bHide)
		{
			MeshComp->HideBoneByName(Bone, EPhysBodyOp::PBO_None);
		}
		else
		{
			MeshComp->UnHideBoneByName(Bone);
		}
	}
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

void UHeroAnimInstance::UpdateMovementDirection()
{
	const ACharacter* Character = OwnerCharacter.Get();
	if (!Character || !Character->GetCharacterMovement())
	{
		MovementDirection = 0.f;
		return;
	}

	// 速度趋零时方向是噪声（atan2 的两个输入都趋 0），保持上一帧的值。
	// 见头文件 MovementDirection 属性上的注释（BS 已落 idle 行，保留方向还有减速过渡的好处）。
	if (Character->GetCharacterMovement()->Velocity.Size2D() < 5.f)
	{
		return;
	}

	// CalculateDirection：速度相对角色朝向的夹角（-180..180，正 = 身体右侧）。
	// 这正是方向 BS 横轴的标准约定（模板 BS_Idle_Walk_Run 同款：-45 = 左前，+90 = 正右）。
	MovementDirection = UKismetAnimationLibrary::CalculateDirection(
		Character->GetCharacterMovement()->Velocity,
		Character->GetActorRotation());
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
