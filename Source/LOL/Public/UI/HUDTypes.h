// GAS → UMG HUD 数据管道：UI 语义类型与委托声明的唯一出处。
//
// 【这个头文件里不允许出现任何 GAS 类型】—— 不 include AbilitySystemComponent.h /
// GameplayEffect.h / GameplayTagContainer.h，也不使用 FGameplayAttribute / FGameplayTag /
// FGameplayEffectSpec / EGameplayTagEventType。
//
// 理由：Widget 只需要知道「怎么显示」，不需要知道「值从哪来」。契约就是下面的结构体 + 委托签名，
// 于是数据源怎么变（换属性集、加充能、ASC 换子类）都不会波及 UI 层，UI 也用不着跟着重编。
//
// 自查（期望：第一条零命中，第二条只剩 HeroHUDWidget.cpp 那一处）：
//   grep -rn "AbilitySystemComponent.h\|GameplayTagContainer.h\|GameplayEffect.h" Source/LOL/Public/UI/
//   grep -rn "AbilitySystemComponent.h\|GameplayTagContainer.h\|GameplayEffect.h" Source/LOL/Private/UI/
//
// 为什么 Private/UI 允许有一处：HeroHUDWidget.cpp 必须 include GAS/HeroHUDController.h 才能调它、
// 才能 AddDynamic。但那是「UI 面向的头文件」，不是引擎的 GAS 头 —— 纪律管的是
// 【UI 的接口里不出现 GAS 类型】（所以 Public/UI/*.h 零命中），不是「实现文件里不许提 GAS」。
//
// 槽位身份是【int32 序号】，不是 FGameplayTag：序号由 UHeroHUDSlotConfig 里的数组顺序定义，
// Widget 因此连 GameplayTags 模块都不需要认识。

#pragma once

#include "CoreMinimal.h"
#include "Misc/EnumClassFlags.h"
#include "HUDTypes.generated.h"

class UTexture2D;

/**
 * 槽位的【表现状态】。Controller 按下面的优先级把旗标压成一个值，Widget 拿到就直接播，
 * Widget 里不该再有第二个优先级判断。
 *
 *   优先级：Disabled > Greyed(Dead > Stunned > Silenced > NotEnoughEnergy) > Cooled > Normal
 */
UENUM(BlueprintType)
enum class ESkillSlotState : uint8
{
	/** 可放。 */
	Normal      UMETA(DisplayName = "Normal"),
	/** 只是在冷却 —— 图标保持亮，上面盖转圈 + 秒数。 */
	Cooled      UMETA(DisplayName = "Cooled"),
	/** 不可用但不是冷却 —— 整体压暗。注意：死亡时压暗，但冷却数字照常走。 */
	Greyed      UMETA(DisplayName = "Greyed"),
	/** 槽位空 / 没授权 —— 连图标都没有。 */
	Disabled    UMETA(DisplayName = "Disabled"),
};

/**
 * 灰化的【原因】，位旗标。
 *
 * 为什么不是互斥枚举：这些原因可以【同时】成立（被沉默 + 蓝不够 + 死亡）。
 * 单值枚举必须在合成处藏一条隐式优先级规则，UI 和 tooltip 都看不出来。
 * 表现枚举（ESkillSlotState）负责「播什么」，这个旗标负责「为什么」——
 * tooltip 要说的是「被沉默，还有 2.3 秒」，而不只是「灰的」。
 */
UENUM(BlueprintType, meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class ESkillSlotBlockReason : uint8
{
	None            = 0         UMETA(Hidden),
	/** 槽位上是空的（没授权 / 映射表里没配）。 */
	NoAbility       = 1 << 0    UMETA(DisplayName = "No Ability"),
	/** State.Cooldown.X 标签在（标签是冷却的唯一真相）。 */
	Cooldown        = 1 << 1    UMETA(DisplayName = "Cooldown"),
	/** Energy < Ability->ManaCost。 */
	NotEnoughEnergy = 1 << 2    UMETA(DisplayName = "Not Enough Energy"),
	/** 能力自己的 ActivationBlockedTags 命中 State.Silenced（沉默只挡法术，不挡普攻）。 */
	Silenced        = 1 << 3    UMETA(DisplayName = "Silenced"),
	/** 同上，命中 State.Stunned。 */
	Stunned         = 1 << 4    UMETA(DisplayName = "Stunned"),
	/** 同上，命中 State.Dead。 */
	Dead            = 1 << 5    UMETA(DisplayName = "Dead"),
	/**
	 * 能力的标签要求没满足，但原因是 HUD 认不出来的那一条（以后新加的准入标签）。
	 * 留着它是为了「宁可灰错不可亮错」：权威判据是能力自己的 ActivationBlockedTags，
	 * 认不出具体是哪条时至少要保证状态是灰的，而不是误判成 Normal。
	 */
	BlockedByTags   = 1 << 6    UMETA(DisplayName = "Blocked By Tags"),
};
ENUM_CLASS_FLAGS(ESkillSlotBlockReason);

/**
 * 槽位的【种类】。纯粹是分类，不影响冷却/灰化的任何算法 —— 它的读者只有两个：
 *   · Widget：被动槽不画键位标注（按不出来），Block 槽可以画成鼠标右键图标；
 *   · 人：在 DA 里一眼看出这一条是主动还是预留的被动/格挡。
 *
 * 为什么不省掉它：这个字段是【可配置的】，不是 C++ 里 if (SlotTag == "Passive") 猜出来的。
 * 加一个英雄带三个被动时，只需要在数组里多加三条 Kind=Passive，C++ 一行不动。
 */
UENUM(BlueprintType)
enum class EHeroHUDSlotKind : uint8
{
	/** 按得出来的主动技能（Q / W / E / R / D / F）。默认值。 */
	Active      UMETA(DisplayName = "Active"),
	/**
	 * 被动。常驻、按不出来 —— 但【可以有冷却】：转圈和秒数走的是同一条管道，
	 * 唯一区别是不画键位（见 ESkillSlotBlockReason / Widget 的处理）。
	 */
	Passive     UMETA(DisplayName = "Passive"),
	/** 另一个输入面的能力（鼠标右键的格挡）。语义同 Active，只是键位标注不是字母。 */
	Block       UMETA(DisplayName = "Block"),
};

/**
 * 一个技能槽的完整 UI 视图。
 *
 * 静态表现（Icon / KeyLabel / DisplayName）也塞在这里，是【故意的】：这样 Widget 完全不需要认识
 * UHeroHUDSlotConfig（那个资产持有 FGameplayTag，会把 GAS 依赖漏进 UI 层）。
 * 代价是每条广播多拷两个 FText 和一个 TObjectPtr，30Hz × 6 槽对这个量级完全不算事。
 */
USTRUCT(BlueprintType)
struct FSkillSlotView
{
	GENERATED_BODY()

	/** 槽位序号。Widget 只用它认人（配 KeyLabel 显示 / 找对应子 Widget）。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	int32 SlotIndex = INDEX_NONE;

	/** false = 这个槽位上没有能力（映射表没配 / 没授权）。State 会是 Disabled。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	bool bHasAbility = false;

	/** 种类（主动 / 被动 / 格挡）。来自 DA，Controller 原样带出来，UI 自己决定怎么画。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	EHeroHUDSlotKind Kind = EHeroHUDSlotKind::Active;

	/**
	 * 整格收起来（Collapsed），不是灰化。
	 *
	 * 【和 Disabled 不是一回事】：Disabled 说的是「槽位空」，UI 照样该画一个空框占位；
	 * bHidden 说的是「这条根本没打算让人看见」—— 为预留的被动 / 格挡准备的。
	 * 只由 DA 上的 `bHideWhenUnavailable` 决定，能力一挂上就自动出现，不需要回来改配置。
	 *
	 * 判据写在 Controller 里（Widget 不该有第二个判断，见 ResolveSlotState 的同款理由）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	bool bHidden = false;

	// ---- 静态表现（来自 UHeroHUDSlotConfig）----

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	TObjectPtr<UTexture2D> Icon = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	FText KeyLabel;

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	FText DisplayName;

	// ---- 冷却（连续量，30Hz 采样）----

	/** 剩余秒数。现场从 ASC 查来的，不是本地递减出来的。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	float CooldownRemaining = 0.f;

	/** 总时长，【已经含急速】（Duration 是 ASC 按 CooldownDuration × 100/(100+Haste) 算好的）。UI 不要再乘一次。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	float CooldownDuration = 0.f;

	/** 转圈进度：1 = 刚进 CD，0 = 好了。UI 直接拿去喂 radial fill。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	float CooldownPercent = 0.f;

	// ---- 充能（Phase 6 才有真属性；字段先占位，默认 1/1 = 不画分段）----

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	int32 Charges = 1;

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	int32 MaxCharges = 1;

	// ---- 状态 ----

	/** 表现状态。由 Controller 从 BlockReasons 按优先级压出来。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	ESkillSlotState State = ESkillSlotState::Disabled;

	/** 原因旗标（ESkillSlotBlockReason 的位组合）。给 tooltip 用。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (Bitmask, BitmaskEnum = "/Script/LOL.ESkillSlotBlockReason"))
	int32 BlockReasons = 0;

	/** 标签说这个槽在冷却（和 CooldownRemaining 是两回事：数字可以回弹，这个不会）。 */
	bool IsCoolingDown() const { return (BlockReasons & static_cast<int32>(ESkillSlotBlockReason::Cooldown)) != 0; }

	/** 该不该在 UI 上转圈。冷却中才转；死了也照转（两条通道互相独立）。 */
	bool ShouldShowCooldown() const { return IsCoolingDown() && CooldownDuration > 0.f; }

	/**
	 * 这次变化需不需要推给 UI。
	 * 数值带容差：30Hz 心跳下 Remaining 每拍都会变，所以心跳天然会通过；
	 * 而标签事件带来的重复计算（比如同一帧两条状态标签一起变）会被这里挡掉，不会各播一遍。
	 */
	bool EqualsForUI(const FSkillSlotView& Other) const
	{
		return bHasAbility == Other.bHasAbility
			&& Kind == Other.Kind
			&& bHidden == Other.bHidden
			&& State == Other.State
			&& BlockReasons == Other.BlockReasons
			&& Charges == Other.Charges
			&& MaxCharges == Other.MaxCharges
			&& Icon == Other.Icon
			&& KeyLabel.EqualTo(Other.KeyLabel)
			&& DisplayName.EqualTo(Other.DisplayName)
			&& FMath::IsNearlyEqual(CooldownRemaining, Other.CooldownRemaining, 0.005f)
			&& FMath::IsNearlyEqual(CooldownPercent, Other.CooldownPercent, 0.002f);
	}
};

/** 血 + 能量。一个结构体一条委托：拆成两条就会多一个「忘了订阅」的理由。 */
USTRUCT(BlueprintType)
struct FHUDVitalsView
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "HUD") float Health = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") float MaxHealth = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") float HealthPercent = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "HUD") float Energy = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") float MaxEnergy = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") float EnergyPercent = 0.f;

	bool EqualsForUI(const FHUDVitalsView& Other) const
	{
		return FMath::IsNearlyEqual(Health, Other.Health, 0.05f)
			&& FMath::IsNearlyEqual(MaxHealth, Other.MaxHealth, 0.05f)
			&& FMath::IsNearlyEqual(Energy, Other.Energy, 0.05f)
			&& FMath::IsNearlyEqual(MaxEnergy, Other.MaxEnergy, 0.05f);
	}
};

/** 目标框。bHasTarget = false 时其余字段无意义（UI 收起整个框）。 */
USTRUCT(BlueprintType)
struct FTargetFrameView
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "HUD") bool bHasTarget = false;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") FText DisplayName;

	UPROPERTY(BlueprintReadOnly, Category = "HUD") float Health = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") float MaxHealth = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") float HealthPercent = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "HUD") float Energy = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") float MaxEnergy = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "HUD") float EnergyPercent = 0.f;

	/** 目标是否处于 State.Dead。死亡的目标框通常要压暗但继续显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD") bool bIsDead = false;

	/** 名字是 FText，不能靠 IsNearlyEqual 那一套比 —— 逐字段比。 */
	bool EqualsForUI(const FTargetFrameView& Other) const
	{
		return bHasTarget == Other.bHasTarget
			&& bIsDead == Other.bIsDead
			&& DisplayName.EqualTo(Other.DisplayName)
			&& FMath::IsNearlyEqual(Health, Other.Health, 0.05f)
			&& FMath::IsNearlyEqual(MaxHealth, Other.MaxHealth, 0.05f)
			&& FMath::IsNearlyEqual(Energy, Other.Energy, 0.05f)
			&& FMath::IsNearlyEqual(MaxEnergy, Other.MaxEnergy, 0.05f);
	}
};

/**
 * 当前完整状态快照。
 *
 * 这是「订阅即拉取」的载体：任何时刻新来的 Widget 调一次 PullHUDState() 就拿到全量，
 * 不需要等下一次广播，也不依赖「Controller 记得在正确时机 BroadcastInitialValues()」。
 * 于是 Widget 的创建时机和 Controller 的绑定时机彻底解耦 —— 这正是原来那个坑。
 */
USTRUCT(BlueprintType)
struct FHUDSnapshot
{
	GENERATED_BODY()

	/** false = 还没绑上 ASC（PS 还没到 / 还没 InitAbilityActorInfo）。UI 应显示「未就绪」而不是空血条。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD") bool bBound = false;

	UPROPERTY(BlueprintReadOnly, Category = "HUD") FHUDVitalsView Vitals;

	/** 按 UHeroHUDSlotConfig::Slots 的顺序排列，长度 = 槽位数。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD") TArray<FSkillSlotView> Slots;

	UPROPERTY(BlueprintReadOnly, Category = "HUD") FTargetFrameView Target;
};

// ---------------------------------------------------------------------------
// 委托（全部 BlueprintAssignable，签名里没有任何 GAS 类型）
//
// 冷却和灰化【合并】成一条 OnSkillSlotChanged：FSkillSlotView 里同时带秒数、进度、状态、原因。
// 拆成两条会多一个忘记订阅的理由，而且两条广播之间会有一帧的不一致。
// ---------------------------------------------------------------------------

/** 血 / 能量变化（属性事件驱动，两端都触发）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHUDVitalsChangedSignature, const FHUDVitalsView&, Vitals);

/** 单个技能槽变化（标签事件 + 30Hz 心跳驱动）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSkillSlotChangedSignature, int32, SlotIndex, const FSkillSlotView&, View);

/** 目标框变化（含「有没有目标」）。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTargetFrameChangedSignature, const FTargetFrameView&, View);

/** 绑定状态变化。false 时 UI 显示未就绪；换 PS 时会先 false 再 true。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHUDReadySignature, bool, bBound);

/**
 * UI 层用的小工具。放在这里而不是某个 Widget 里，是为了让「显示口径」只有一份 ——
 * 技能槽、目标框、以后的观战界面都调它，改一次全部跟着改。
 */
namespace HeroHUD
{
	/**
	 * 冷却秒数的显示口径（对齐 LoL）：>= 1 秒显示整数（向上取整），< 1 秒显示一位小数，0 返回空串。
	 * 注意传的是「剩余秒数」，不是百分比 —— 百分比是给转圈用的。
	 */
	inline FText FormatCooldownSeconds(float Seconds)
	{
		if (Seconds <= KINDA_SMALL_NUMBER)
		{
			return FText::GetEmpty();
		}
		if (Seconds >= 1.f)
		{
			return FText::AsNumber(FMath::CeilToInt(Seconds));
		}
		return FText::AsNumber(FMath::RoundToInt(Seconds * 10.f) / 10.f);
	}
}
