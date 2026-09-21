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

#pragma once

#include "CoreMinimal.h"
#include "Components/WidgetComponent.h"
#include "GameplayTagContainer.h"
#include "GameplayEffectTypes.h"
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

	/** 满血时整条收起来（MOBA 里小兵血条的做法）。英雄一般关掉。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bHideWhenFullHealth = false;

	/** 死亡时收起来。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bHideWhenDead = false;

private:
	void Bind();
	void Unbind();

	/** 读一遍真值、翻成 UI 语义、推给 Widget。绑完要显式调一次（绑定不触发初始值）。 */
	void Refresh();

	void OnHealthChanged(const FOnAttributeChangeData& Data);
	void OnMaxHealthChanged(const FOnAttributeChangeData& Data);
	void OnDeadTagChanged(const FGameplayTag ChangedTag, int32 NewCount);

	UPROPERTY(Transient)
	TWeakObjectPtr<AActor> ObservedActor;

	UPROPERTY(Transient)
	TWeakObjectPtr<UAbilitySystemComponent> BoundASC;

	FDelegateHandle HealthHandle;
	FDelegateHandle MaxHealthHandle;
	FDelegateHandle DeadTagHandle;
};
