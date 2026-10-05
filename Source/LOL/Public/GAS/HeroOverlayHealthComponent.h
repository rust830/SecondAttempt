// 敌人头顶血条的「迷你翻译层」。
//
// 【为什么不复用 UHeroHUDController】
// 那是「本地玩家一个」的东西：一份绑定、一份快照、一个 30Hz 心跳、一个 Observed 目标。
// 头顶血条是「每个敌人一个」—— 五个人打团就是五份。塞进 HUDController 等于让本地玩家
// 替全场敌人心跳，而且谁的 Widget 谁来管会变得说不清。
//
// 所以这里走一条更短的路：组件直接绑自己目标的 Health / MaxHealth 属性委托，
// 翻成 FHUDVitalsView 推给 UHeroOverlayHealthWidget。
// 注意「复用第 1 步数据路径」复用的是【契约】（UI 语义类型 + 绑定纪律），不是那个对象 ——
// 所以 UHeroOverlayHealthWidget 和 WBP_HUD 里的血条能用同一个 WBP 血条控件。
//
// 和 HUDController 共享的两条纪律：
//   · 属性用 GetGameplayAttributeValueChangeDelegate，不用 OnRep_*；
//   · 绑定不触发初始值 → 绑完显式刷一次（见 SetObservedActor 末尾）。
//
// 挂点：组件自己吸附到 Owner 骨骼的 AttachSocketName 插槽（默认 "HealthBar"），
// 所以蓝图里不必去 Parent Socket 下拉里选 —— 见 AttachToOwnerMeshSocket。
// 想要纯数值（不带百分比的属性查询）走 UHeroAttributeLibrary，别在这里再开一条。

#pragma once

#include "CoreMinimal.h"
#include "Components/WidgetComponent.h"
#include "GameplayTagContainer.h"
#include "GameplayEffectTypes.h"
#include "GAS/HUDAttributeBinding.h"
#include "HeroOverlayHealthComponent.generated.h"

class UAbilitySystemComponent;

UCLASS(ClassGroup = (HUD), meta = (BlueprintSpawnableComponent))
class LOL_API UHeroOverlayHealthComponent : public UWidgetComponent
{
	GENERATED_BODY()

public:
	// 不覆写构造函数：UWidgetComponent 自己的 tick 设置是有讲究的（Screen 空间的朝向/位置更新要用），
	// 关掉它会让头顶血条不跟着镜头刷新。数据更新走属性委托，本来就不占 tick。
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 幂等重绑：先解绑旧目标，再绑新的。传 nullptr = 用 GetOwner()。 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void SetObservedActor(AActor* InActor);

	UFUNCTION(BlueprintPure, Category = "HUD")
	AActor* GetObservedActor() const { return ObservedActor.Get(); }

protected:
	/** 显式指定观察对象。留空 = 用 Owner（绝大多数情况都不填）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	TObjectPtr<AActor> ObservedActorOverride;

	// -----------------------------------------------------------------------
	// 挂点：吸附到骨骼插槽
	//
	// 为什么不让蓝图直接设 Parent Socket 就完事：那要求【每个人物蓝图】都记得去
	// 细节面板里选一次，而且角色换模型、改身高都要重来一遍 —— 漏配的表现是
	// 「血条卡在某处不动」，而不是报错。吸附写在组件里，配一次（其实是零配置，
	// 默认值就是约定好的名字）对所有人物生效。
	// -----------------------------------------------------------------------

	/**
	 * 吸附到 Owner 骨骼上的哪个插槽。默认 "HealthBar"（角色骨骼上留的那个）。
	 *
	 * 置空（None）= 关掉吸附，完全按蓝图里配的父子关系走。
	 * 小兵 / 建筑这类不需要跟骨骼动画的情况清空即可。
	 *
	 * 【名字为什么不叫 AttachSocketName】：那个名字被 USceneComponent 占了 ——
	 * 就是细节面板 Transform 里的 Parent Socket，而且 UHT 不允许子类遮蔽父类成员
	 * （会直接报 "shadowing is not allowed"）。两者的分工是：
	 *   · 这个字段 = 【意图】，我们想吸附到哪；
	 *   · 父类的 AttachSocketName = 【实际结果】，AttachToComponent 成功后会写进去。
	 * 吸附成功时两者内容一致；置空这个只表示「不主动吸附」，父类那个照常如实反映现状。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FName OwnerMeshSocketName = TEXT("HealthBar");

	/**
	 * 吸附之后再叠的相对偏移。默认零 = 正好落在插槽上。
	 *
	 * 要微调（比如让条子再高一点）改这里，别回头去动骨骼插槽 ——
	 * 插槽位置是美术的东西，改它会波及所有引用它的地方。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FVector SocketOffset = FVector::ZeroVector;

	/** 满血时整条收起来（MOBA 里小兵血条的做法）。英雄一般关掉。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bHideWhenFullHealth = false;

	/** 死亡时收起来。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bHideWhenDead = false;

private:
	/**
	 * 把组件吸附到 Owner 骨骼 mesh 的 AttachSocketName 插槽上。BeginPlay 调一次。
	 *
	 * 找不到 mesh / 没有这个插槽时【只警告、不吸附】—— 保留蓝图里配的挂位，
	 * 血条照常工作，只是位置是手填的那个。这比让组件消失好排查得多。
	 */
	void AttachToOwnerMeshSocket();

	void Bind();
	void Unbind();

	/** 读一遍真值、翻成 UI 语义、推给 Widget。绑完要显式调一次（绑定不触发初始值）。 */
	void Refresh();

	/** 血 / 最大血共用这一个回调：处理逻辑一样（重算一遍再推），分两个只会多一个漏绑的机会。 */
	void OnVitalsAttributeChanged(const FOnAttributeChangeData& Data);
	void OnDeadTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> ObservedActor;

	/** 绑定的 ASC。只读数值 + 判 binding 是否还在用，所以留着它，不和句柄记账合并。 */
	UPROPERTY(Transient)
	TWeakObjectPtr<UAbilitySystemComponent> BoundASC;

	/** 属性委托 + 标签事件的记账（和 UHeroHUDController 共用同一份实现）。 */
	FHUDAttributeBinding Binding;
};
