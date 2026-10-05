// TurnSlash 的实现。招式设计见头文件。

#include "GAS/GA_TurnSlash.h"
#include "GAS/GE_Knockback.h"
#include "GAS/GE_TurnSlashCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GameFramework/Character.h"

UGA_TurnSlash::UGA_TurnSlash()
{
	// =============================================================================
	// 两段设计：① 正常挥砍（普通伤害）→ ② 边砍边转满 180°，转到位时猛击 + 击退
	//
	// 【两段在同一条 Montage 里连着播】—— 构造函数只配一次 Montage（BP 里填）。
	//
	// ⚠️【转体的时机】转身在 ActivateAbility 里就【起手开始】了（转满 TurnDuration），
	//   不是「第 2 段才转」。原因：转身是【连续插值】，从中途再启动会顿一下；
	//   而两段之间通常只有 0.2~0.4 秒，插值根本来不及走完。
	//   观感上是「第 1 段边砍边转，第 2 段已经转到位时猛击」——
	//   这恰好就是需求说的「边砍边转向后方，在接近 180 度的位置猛击」。
	// =============================================================================

	// --- 顶层默认值（某段留空/负值时继承这些）---
	HitRadius = 200.f;
	DamageMultiplier = 1.1f;
	FlatDamage = 20.f;
	ControlDuration = 0.55f;

	// 水平冲量 900 cm/s；垂直 250 cm/s 给一点小浮空（不填就是纯水平推）。
	KnockbackImpulse = 900.f;
	KnockbackLaunch = 250.f;
	KnockUpLaunch = 0.f;      // 这一招不击飞，击飞留给 SpinSlash

	// 转身时长。「接近 180 度的位置猛击」这个手感全靠这个数：
	// 太短（< 0.2s）像瞬间闪转、动作读不出来；太长（> 0.5s）转身期间站着挨打。
	// 0.4 ≈ 转到位刚好接上第 2 段的命中帧（notify 放在 montage 后半段）。
	TurnDuration = 0.4f;

	CooldownDuration = 9.f;
	// 每个技能一个自己的冷却 GE（不能共用，理由见 GE_SpinSlashCooldown.h）。
	CooldownGameplayEffectClass = UGE_TurnSlashCooldown::StaticClass();

	// --- 第 1 段：正常挥砍（不转到位就砍，靠 bTargetBehind 决定要不要转）---
	{
		FFormMeleeStage Stage1;
		Stage1.ImpactTag = LOLGameplayTags::Event_Melee_TurnSlash;
		// 第 1 段伤害比第 2 段低（它是「起手」，不是主要伤害来源）。
		Stage1.DamageMultiplier = 0.6f;
		Stage1.FlatDamage = 0.f;
		// ControlGE 留空 = 这一段不加控制（回身斩的控制全在第 2 段）。
		Stage1.ControlGE = nullptr;
		Stages.Add(Stage1);
	}

	// --- 第 2 段：转到位后的猛击 + 击退 ---
	{
		FFormMeleeStage Stage2;
		Stage2.ImpactTag = LOLGameplayTags::Event_Melee_TurnSlash2;
		// 数值留 -1 = 继承顶层（1.1x + 20）
		Stage2.DamageMultiplier = -1.f;
		Stage2.FlatDamage = -1.f;
		// ★ 只有这一段击退。击退的位移由 UGE_Knockback 自带的
		//   UGEComponent_Knockback 做（方向走那个四级回退表），
		//   所以本类【不需要】像 SpinSlash 那样覆写 LaunchTarget。
		Stage2.ControlGE = UGE_Knockback::StaticClass();
		// ControlDuration / KnockbackImpulse / KnockbackLaunch 留 0 = 继承顶层
		Stage2.HitCueTag = FGameplayTag();   // ← 待填：想第二段有镜头效果就填带 CameraShake 的 Cue
		Stages.Add(Stage2);
	}
}

float UGA_TurnSlash::GetTurnDegrees() const
{
	// 只在目标【背后】时转 180。目标在身前就 0 —— 转过去等于主动把敌人让开，
	// 那一刀挥空（基类的球形扫描是 360° 的所以仍会命中，但动画和实际受击方向对不上，
	// 看起来像「背对着人却打到了他」，比不转糟糕得多）。
	//
	// bTargetBehind 由基类 ActivateAbility 算好：点积 < 0，也就是在身后 90° 之外。
	return bTargetBehind ? 180.f : 0.f;
}
