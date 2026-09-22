// 属性数值的【唯一查询出口】。全项目想读「某个 Actor 的血 / 攻 / 防 / 移速是多少」都走这里。
//
// 【为什么不做成某个对象上的成员函数】
//   · 挂在 UHeroHUDController 上 → 它就只能查本地玩家自己（目标框、敌人、小兵都查不到），
//     而且那个类的定位是「UI 语义翻译层」，塞一堆属性查询会变成属性大杂烩。
//   · 挂在 PlayerState 上 → PS 被几十个转发 getter 撑大，而且小兵 / 建筑这些
//     没有 PlayerState 的东西立刻就没法查了。
//   · 传 Actor 进来的静态函数 → 谁的数值都能查，没有状态、没有生命周期，
//     以后属性面板 / 观战 / 回放 / 调试 UI 全部复用同一个入口。
//
// 【两条纪律】
//   1. 「怎么从一个 Actor 拿到 ASC」复用 UMyAbilitySystemComponent::FindAbilitySystemComponent，
//      不在这里另写一套。别的路径有坑（比如蓝图库那条在「接口 Cast 成功但返回 nullptr」时
//      不会继续往下试），口径必须和选目标 / 伤害校验保持一致。
//   2. 这里【只读、不监听】。需要「变了通知我」的场景走 UHeroHUDController 的委托，
//      或者自己对 ASC 的属性委托 AddUObject —— 别把这个库扩成事件源。
//
// 返回的是快照结构体（FHeroAttributeView），读不到的 Actor 返回 bValid = false 而不是 nullptr，
// 蓝图侧少一个 IsValid 分支。

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GAS/HeroAttributeView.h"
#include "HeroAttributeLibrary.generated.h"

class AActor;

UCLASS()
class LOL_API UHeroAttributeLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 现场读一遍属性集，返回全部裸数值（24 条，全是当前值，不含任何派生比率）。
	 *
	 * Source 为空 / 没有 ASC / ASC 上没挂 UHeroCombatAttributeSet 时返回 bValid = false 的空视图。
	 * 每次调用都真的去读，不做缓存 —— 属性是真值源，缓存一份就等于多一个会过期的副本。
	 */
	UFUNCTION(BlueprintPure, Category = "Hero|Attributes", meta = (DisplayName = "Get Hero Attributes"))
	static FHeroAttributeView GetHeroAttributes(const AActor* Source);

	/**
	 * 只问「现在能不能读到属性」。比拿全量再判 bValid 便宜，适合当成显示/隐藏的开关。
	 */
	UFUNCTION(BlueprintPure, Category = "Hero|Attributes")
	static bool HasHeroAttributes(const AActor* Source);
};
