// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_Block.generated.h"

class UMaterialInterface;
class UNiagaraComponent;
class UNiagaraSystem;
class USceneComponent;
class USoundBase;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * 防护罩表现（Actor 版 cue）。
 *
 * 生命周期挂在 UGE_BlockImmune 的 GameplayCues 上：免疫挂上 → OnActive 出现，免疫结束
 * （到期 / 被驱散 / 被刷新替换）→ OnRemove 收掉。不需要任何外部清理调用，也不会漏还原。
 * 每个客户端各跑一遍，所以「别人也看得到你的罩子」是天然的。
 *
 * 两种做法都在这里，二选一（都填则 Niagara 优先）：
 *   - ShieldSystem（Niagara NS）—— 推荐，做法见方案文档 §4.3
 *   - ShieldMesh（静态网格 + 材质）—— 零 Niagara 配置的简化版，见 §4.5
 *
 * 蓝图子类必须命名为 GC_Block（不能叫 BP_GC_Block），否则引擎会把继承来的 GameplayCueTag
 * 按类名重推成无效标签、cue 静默不触发。见 LOLGameplayTags.h 里的说明。
 */
UCLASS()
class LOL_API AGC_Block : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()

public:
	AGC_Block();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;

	/** 防护罩 Niagara 系统（推荐）。填了它就不看 ShieldMesh。 */
	UPROPERTY(EditDefaultsOnly, Category = "Block|Visual")
	TObjectPtr<UNiagaraSystem> ShieldSystem;

	/** 防护罩网格（简化版）。用 SM_Sparrow_SlowDome / SM_HalfSphere_100 这类半球最像罩子。 */
	UPROPERTY(EditDefaultsOnly, Category = "Block|Visual")
	TObjectPtr<UStaticMesh> ShieldMesh;

	/** 网格用材质（可空：留空则用网格自带的）。*/
	UPROPERTY(EditDefaultsOnly, Category = "Block|Visual")
	TObjectPtr<UMaterialInterface> ShieldMaterial;

	/** 罩子相对角色的偏移（角色胶囊中心在脚底往上约 90，罩子通常要抬一点）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Block|Visual")
	FVector ShieldOffset = FVector(0.f, 0.f, 90.f);

	/** 罩子缩放。Kallari 高约 180cm，1.0 大致是半径 100cm 的球。 */
	UPROPERTY(EditDefaultsOnly, Category = "Block|Visual")
	FVector ShieldScale = FVector(1.f, 1.f, 1.f);

	/** 格挡成功那一刻的音效（可空）。Paragon: Kallari_Effort_Block。 */

protected:
	/** 兜底：角色被销毁 / 切关卡时 OnRemove 不一定走到，别把组件留在世界里。 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** OnActive 建出来的那一个（Niagara 或网格，二选一）。OnRemove/EndPlay 销毁。 */
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ShieldComp;

	/** 材质诊断只打一次，不刷屏。 */
	bool bLoggedShieldDiagnostic = false;

	/** 罩子挂哪儿：优先角色 mesh（跟着身体走），没有就挂根组件。 */
	static USceneComponent* ResolveAttachParent(AActor* Target);

	/** 建罩子（Niagara 优先，其次网格）。两个都没配就打一行日志，不静默。 */
	void SpawnShield(AActor* Target);

	/** 销毁罩子（幂等，OnRemove 和 EndPlay 都会调）。 */
	void DestroyShield();

	/** 打印材质/系统的各道门槛 —— 罩子不显示、也不报错时靠这条日志定位（见方案文档 §4.6）。 */
	void LogShieldDiagnostic();
};
