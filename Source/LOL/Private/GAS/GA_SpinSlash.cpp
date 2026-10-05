// SpinSlash 的实现。招式设计见头文件。

#include "GAS/GA_SpinSlash.h"
#include "GAS/GE_KnockUp.h"
#include "GAS/GE_SpinSlashCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GameFramework/Character.h"

UGA_SpinSlash::UGA_SpinSlash()
{
	// =============================================================================
	// 两段设计（见头文件）：① 平砍，普通伤害无控制 → ② 上挑，伤害更高 + 击飞
	//
	// 【两段在同一条 Montage 里连着播】—— 靠各自的 ImpactTag 区分结算时机。
	//   所以这里只配一次 Montage（BP 里填），下面只配两段各自的数值。
	//
	// ⚠️ 两段必须用【不同】的 ImpactTag：WaitGameplayEvent 按标签订阅，
	//   同一个标签发两次只会唤醒一次（第二段被吃掉，
	//   症状是「两段动作播完只有第一下有伤害」）。
	// =============================================================================

	// --- 顶层默认值（某段留空/负值时继承这些）---
	HitRadius = 220.f;         // 命中半径比普攻大（重击，不是贴身平 A）
	DamageMultiplier = 1.2f;
	FlatDamage = 25.f;
	ControlDuration = 0.75f;   // 上挑那段的硬直时长
	// 升空冲量 700 cm/s ≈ 3.6m 高、滞空约 0.7s（980 cm/s² 重力下）。
	// 和 KnockbackImpulse 是两条独立的通道：击退走 GE 组件读后者，击飞走 LaunchTarget 读前者。
	KnockUpLaunch = 700.f;
	KnockbackImpulse = 0.f;   // 击飞不给水平冲量（这一招是「上挑」，不推人）

	// 转身 0 度：不转，始终面朝锁定目标。这是 SpinSlash 和 TurnSlash 的核心区别。
	TurnDuration = 0.f;

	CooldownDuration = 8.f;
	// 每个技能一个自己的冷却 GE（不能共用）：CheckCooldown 按【标签】判，共用 GE = 共用标签
	// ⇒ 同时拿到两个海克斯时放一个会把另一个也锁住。
	CooldownGameplayEffectClass = UGE_SpinSlashCooldown::StaticClass();

	// --- 第 1 段：平砍 ---
	{
		FFormMeleeStage Stage1;
		// ★ 标签用 Suffix 那一族（见 LOLGameplayTags.h 的注释：避免父子标签互相唤醒）。
		//   蒙太奇上第 1 个 notify 也必须填这个。
		Stage1.ImpactTag = LOLGameplayTags::Event_Melee_SpinSlash;
		// 平砍伤害比上挑低：倍率 0.7 ≈ 顶层的 60%。
		Stage1.DamageMultiplier = 0.7f;
		Stage1.FlatDamage = 0.f;
		// ★ ControlGE 留空 = **这一段不加控制**（不是「继承顶层的」——
		//   这个「留空=无」是 FFormMeleeStage 里故意和数值类相反的语义）。
		//   这里是多段设计最关键的一处：平砍不该把人挑飞，只有上挑那一下才击飞。
		Stage1.ControlGE = nullptr;
		Stages.Add(Stage1);
	}

	// --- 第 2 段：上挑 + 击飞 ---
	{
		FFormMeleeStage Stage2;
		Stage2.ImpactTag = LOLGameplayTags::Event_Melee_SpinSlash2;
		// 数值留 -1 = 继承顶层（1.2x + 25）
		Stage2.DamageMultiplier = -1.f;
		Stage2.FlatDamage = -1.f;
		// ★ 只有这一段挂击飞。
		Stage2.ControlGE = UGE_KnockUp::StaticClass();
		// ControlDuration / KnockUpLaunch 留 0 = 继承顶层（0.75s / 700）
		// ⚠️ 镜头效果（你要的「第二段特写」）配在这里的 HitCueTag 上：
		//   照 UGC_DeathHarvestBurst 的做法在 Cue 资产里配 UCameraShakeBase，
		//   ClientStartCameraShake 只震施法者本机。
		// 留空 = 用顶层的 GC_MeleeHit（没镜头效果）——
		//   要镜头效果就新建一个带 CameraShake 的 Cue 填在这里。
		Stage2.HitCueTag = FGameplayTag();   // ← 待填：你要的镜头效果 Cue
		Stages.Add(Stage2);
	}
}

void UGA_SpinSlash::LaunchTarget(AActor* Target, const FHitResult& Hit, const FFormMeleeStage& Stage)
{
	// ⚠️★★ 先判「本段是不是击飞」—— 多段改造后这个函数会被【每一段】调一次，
	//   而只有上挑那段的 ControlGE 是 GE_KnockUp。平砍那段如果也 Launch，
	//   就变成「平砍把人挑飞」了（用户需求明确是第 1 段普通伤害）。
	//
	// 判据用 ControlGE 而不是段下标：段下标是接线细节（万一中间插一段就错了），
	// 而「这段挂的是击飞 GE」才是这件事的真实语义。
	if (!Stage.ControlGE || Stage.ControlGE != UGE_KnockUp::StaticClass())
	{
		return;
	}
	// ★ UGE_KnockUp 有一个位移组件吗？——【没有】。
	//   UGE_Knockback 自带 UGEComponent_Knockback（会自己 LaunchCharacter），
	//   而 UGE_KnockUp 只负责「State.KnockUp 状态 + 升空蒙太奇」
	//   （GE_KnockUp.h 里明确写了「位移不在这里做」）。
	//   所以升空那一下必须由施加方补，否则目标只是站着挂 State.KnockUp
	//   —— 看起来像「被定住了」而不是「被打飞了」。
	if (!Target) return;

	ACharacter* TargetCharacter = Cast<ACharacter>(Target);
	if (!TargetCharacter)
	{
		// 不是 Character（纯蓝图演员）⇒ LaunchCharacter 不可用。
		// 不静默：施法者会看到「控制挂上了但人没动」。
		UE_LOG(LogTemp, Warning,
			TEXT("[SpinSlash] 目标 %s 不是 ACharacter ⇒ 无法 LaunchCharacter（只会挂 State.KnockUp，表现为定身）"),
			*GetNameSafe(Target));
		return;
	}

	// 方向：优先用命中法线的水平分量（打在哪一侧就往那边挑），
	// 法线退化（砸中头顶/脚底，接近竖直）时退回「目标 - 施法者」的水平方向。
	// 理由和 UGEComponent_Knockback 的方向优先级表同源（那里是四级回退），
	// 这里只需两级：LaunchCharacter 走的是 XYOverlapped + Z 的三轴冲量，
	// XY 退化成零向量时表现为「原地垂直起跳」，不致命但很怪。
	FVector LaunchDir = Hit.ImpactNormal.GetSafeNormal2D();
	if (LaunchDir.IsNearlyZero())
	{
		const AActor* Instigator = GetAvatarActorFromActorInfo();
		if (Instigator)
		{
			LaunchDir = (Target->GetActorLocation() - Instigator->GetActorLocation()).GetSafeNormal2D();
		}
	}
	if (LaunchDir.IsNearlyZero())
	{
		// 全退完 = 施法者和目标位置重合。给施法者朝向兜底
		//（这一步和 UGEComponent_Knockback 的第③级同理由：「朝哪打」就是「往哪推」）。
		LaunchDir = GetAvatarActorFromActorInfo()
			? GetAvatarActorFromActorInfo()->GetActorForwardVector().GetSafeNormal2D()
			: FVector::ForwardVector;
	}

	// bXYOverlapped = true：冲量替换速度而不是叠加（不叠加是防「连击时越飞越高」）。
	// ★ 冲量读【Stage】而不是顶层成员：多段之后每段可以有各自的抛量
	//   （上挑那段的 KnockUpLaunch 可以在 BP 里配得和平砍段不同）。
	//   这里水平分量填 0（顶层 KnockbackImpulse=0）⇒ 纯垂直起跳。
	// ⚠️ LaunchCharacter 只在权威端有意义 —— ApplyServerHit 只在服务端跑，这里天然满足。
	TargetCharacter->LaunchCharacter(FVector(LaunchDir.X * Stage.KnockbackImpulse,
	                                           LaunchDir.Y * Stage.KnockbackImpulse,
	                                           Stage.KnockUpLaunch),
	                                   /*XYOverlapped=*/true, /*ZOverride=*/true);

	UE_LOG(LogTemp, Log, TEXT("[SpinSlash] 上挑击飞 %s：方向(%.2f,%.2f) 水平冲量=%.0f 上抛=%.0f"),
		*GetNameSafe(Target), LaunchDir.X, LaunchDir.Y, Stage.KnockbackImpulse, Stage.KnockUpLaunch);
}
