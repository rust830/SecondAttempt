// 斗魂竞技场：装备 / 海克斯身上那一条属性加成的通用 GE。
//
// ===========================================================================
// 【一份 GE 服务所有装备】修正符表在构造函数里按 ArenaItemStats 那张表建死：
// 每条支持的属性一个修正符，幅度全走 SetByCaller。具体数值由施加方
// （UArenaLoadoutComponent）按每件装备的 DataAsset 现填。
//
// 于是「加一件新装备」= 在编辑器里新建一个 DataAsset 填几个数，**不用建新的 GE 资产**。
// 这是需求里「装备来源：回合奖励，随机三选一」能纯数据驱动的前提。
//
// 【为什么不用运行时 NewObject 改 Modifiers】那条路更直观，但只在服务端成立：
//   GE 的 Modifiers 不参与 FGameplayEffectSpec 的复制。
//   客户端（拥有者）会被复制到「同一个 GE 类的一份 spec」，
//   自己那份 Modifiers 是 CDO 上的（空的）——
//   结果是客户端算出来的属性里没有装备加成，而 GAS 在 Mixed 模式下
//   正是让客户端拿 BaseValue 自己叠 GE 来算 CurrentValue 的，
//   所以这不是「显示不对」，是【客户端算出来的攻防就是错的】。
//   SetByCaller 的数值在 spec 的 NetSerialize 里，两边拿到的完全一致。
//
// 【全是加算】理由见 ArenaItemStats.h —— 属性集把百分比类属性单列了，
// 装备要表达的任何加成落到属性上都是加法。真需要乘算就做这个类的子类。
// ===========================================================================
//
// 对应需求里的「传说装备 / 棱彩装备 / 传说锻造器 / 棱彩锻造器」——
// 品质只影响【从哪个池子里抽】，不影响这个 GE 怎么工作。

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_ArenaItem.generated.h"

class UAbilitySystemComponent;
struct FArenaItemStatModifier;

/**
 * 所有装备 / 海克斯属性加成都用这一个 GE 类。
 *
 * 时长 Infinite —— 装备是永久的，摘掉靠 FActiveGameplayEffectHandle。
 */
UCLASS()
class LOL_API UGE_ArenaItem : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGE_ArenaItem(const FObjectInitializer& ObjectInitializer);
};

/**
 * 施加 / 摘除装备加成。
 *
 * 【为什么提成自由函数而不是塞进组件】装备和海克斯走的是同一条路
 * （都是「一串 FArenaItemStatModifier → 一个 Infinite GE」），
 * 两处各写一遍迟早漂。放在 GE 旁边，是因为它俩是一件事的两半。
 */
namespace ArenaItemEffect
{
	/**
	 * 按一串属性加成施加一个 Infinite 的 UGE_ArenaItem，返回活动句柄（用于之后摘除）。
	 *
	 * 【只在服务端调】GE 的施加是权威行为。客户端不需要自己调 ——
	 * SetByCaller 的数值会随 spec 复制下来，客户端那份会自动算对。
	 *
	 * 表里没有的属性会被跳过并记一条 Warning（不崩、不静默）。
	 * 返回值无效 = 一条有效加成都没有 / ASC 为空。
	 */
	LOL_API FActiveGameplayEffectHandle ApplyStatModifiers(
		UAbilitySystemComponent* ASC,
		const TArray<FArenaItemStatModifier>& Modifiers);

	/**
	 * 摘掉先前施加的那一份。句柄会被就地清空（幂等：空句柄直接返回）。
	 *
	 * 句柄要【就地清空】而不是让调用方自己记着清 —— 留着旧句柄再摘一次
	 * 在 GAS 里是静默 no-op，不会报错，所以「忘了清」这个错误永远不会被发现，
	 * 只会表现为「某个装备摘不掉了」。
	 */
	LOL_API void RemoveStatEffect(
		UAbilitySystemComponent* ASC,
		FActiveGameplayEffectHandle& Handle);
}
