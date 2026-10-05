// Fill out your copyright notice in the Description page Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "SocketSide.h"
#include "AnimNotifyState_SocketNiagara.generated.h"

class UNiagaraSystem;
class UNiagaraComponent;
class USkeletalMeshComponent;

/**
 * 在动画区间里往插槽上挂一个 Niagara（起 → 挂上，止 → 淡出）。
 *
 * 【和 UAnimNotifyState_SocketParticle 的分工】
 * 那是 Cascade 版，这个是 Niagara 版：拳/腿这类"要拖尾跟着手走"的表现-quality 要求高，
 * Niagara 的 Ribbon、GPU/CPU 精灵拉伸、参数化用户参数都比 Cascade 好控，
 * 所以新的攻击特效走 Niagara，老的刀光/蓄能不动。
 * 两个类刻意【不合并】：合并就要在一个类里同时管 UParticleSystemComponent 和
 * UNiagaraComponent 两套生命周期，将来改任一边都得同时读两套代码。
 *
 * 【为什么是 notify 而不是 GameplayCue】
 * 蒙太奇在【每一台机器】上都会播，notify 于是天然每台机器各跑一次 ——
 * 正好是"每个人都该看到拳上挂特效"想要的，不需要任何复制代码。
 * 而命中那一炸走 GameplayCue（只有命中那一瞬间、且需要服务权威信息），
 * 于是"拳在挥 → 拖尾跟手"和"打中了 → 命中炸开"这两件事各归各的层，互不知情。
 *
 * 【Tint 走 NS 的 User 参数，Scale 走组件相对缩放】
 * 颜色写在 notify 资产上（美术直接改数字），NS 只管"长什么样"、不管"多紫" ——
 * 同一个 NS 配给直拳和普攻时，换个 Tint 就是两种拳风。
 * 只喂一个参数：NS_Fist_Trail 的 ribbon 材质（M_Ribbon_Wispy）自己读 User.Color，
 * 名字见下面的 TrailTintParameter；粗细直接缩放组件（见 TrailScale）。
 * 以前写过 User.TrailTint / User.TrailScale 两个自定义用户参数，但 NS 里没人消费
 * （trail emitter 没有 Color 模块、ScaleSpriteSize 也没有匹配输入）—— 死参数，
 * 已经拿掉。换 NS 时如果它用的是别的颜色参数名，改 TrailTintParameter 这一个字段。
 *
 * ⚠️ 这个对象是蒙太奇资产里的实例，**所有角色共用同一个**，所以生成出来的组件必须按
 * MeshComp 分开存（ActiveByMesh），不能放成员变量里 —— 否则两个人同时出拳会互相顶掉。
 */
UCLASS(meta = (DisplayName = "Socket Niagara (跟手拖尾)"))
class LOL_API UAnimNotifyState_SocketNiagara : public UAnimNotifyState
{
	GENERATED_BODY()

public:
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;

	/**
	 * ⚠️ 必须每帧跑：Ribbon 的粒子位置来自 NS 的 Dynamic Input，读的是
	 * User.SwordBasePos / User.SwordTipPos 这两个【世界坐标】用户参数 ——
	 * 组件挂在插槽上只解决了"跟着手动"，粒子还是要知道这一帧手挥到哪儿、朝向哪儿，
	 * 否则所有粒子堆在一个点上。项目里能用的 AnimNotifyState_BladeTrail 就是这么干的。
	 */
	virtual void NotifyTick(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float FrameDeltaTime,
		const FAnimNotifyEventReference& EventReference) override;

	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;

	// ---------------------------------------------------------------------
	// ① 挂什么 / 挂哪
	// ---------------------------------------------------------------------

	/** 挂到插槽上的 Niagara 系统（软引用：默认值只是路径，编辑器里随时换）。 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	TSoftObjectPtr<UNiagaraSystem> System;

	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	ESocketParticleSide Side = ESocketParticleSide::Right;

	/** 和 GC_Stealth 里那对插槽名保持一致（Kallari 是双刀/双拳）。 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FName SocketLeft = TEXT("hand_l");

	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FName SocketRight = TEXT("hand_r");

	/**
	 * Ribbon 的另一个端点：从插槽位置沿【插槽 X 轴】推出多远（厘米）。
	 *
	 * ⚠️ 不能留 0：NS_BladeTrail / NS_Fist_Trail 的粒子位置是 Dynamic Input（NiagaraPosition），
	 * base 和 tip 都取自 User.SwordBasePos / User.SwordTipPos —— 两个端点重合时
	 * 每一颗粒子都生成在同一个点上，Ribbon 退化成一个点，**画面上什么都看不到**
	 * （症状是"只有命中特效、没有跟手拖尾"）。这两个参数必须每帧喂（见 NotifyTick）。
	 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara", meta = (ClampMin = "0.5"))
	float TipLength = 14.f;

	/** Ribbon 基端点参数名（含 User. 前缀）。换 NS 时跟着改。 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FName BasePositionParameter = TEXT("User.SwordBasePos");

	/** Ribbon 端点参数名（含 User. 前缀）。换 NS 时跟着改。 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FName TipPositionParameter = TEXT("User.SwordTipPos");

	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FVector RelativeOffset = FVector::ZeroVector;

	/** 左右分开配：右手插槽一般是左手的镜像，右手反了会绕手掌长轴转 180°。 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FRotator RelativeRotationLeft = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FRotator RelativeRotationRight = FRotator::ZeroRotator;

	// ---------------------------------------------------------------------
	// ② 外观参数（喂给 NS 的 User 参数）
	// ---------------------------------------------------------------------

	/** 拖尾颜色：Kallari 是影紫，深一点的那个当主体，别用纯白（纯白 = 一个点光源）。 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FLinearColor TrailTint = FLinearColor(0.62f, 0.32f, 1.0f, 1.0f);

	/**
	 * 拖尾粗细倍率 —— 走【组件相对缩放】（SetRelativeScale3D），不走 NS 的用户参数。
	 *
	 * ⚠️ 只在 NS 的 emitter 没勾 Local Space 时真的看得出来：勾了 Local Space 之后
	 * 粒子走的是自己的局部坐标，组件缩放会被忽略（拖尾粗细不跟着变）。
	 * 没效果的时候先去 NS 里把 Local Space 关掉，别在这儿反复调这个数。
	 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara", meta = (ClampMin = "0"))
	float TrailScale = 1.f;

	/**
	 * NS 里的颜色参数名。
	 * 默认 "User.Color" —— NS_Fist_Trail 的 ribbon 材质（M_Ribbon_Wispy）自己吃这个线性色参数，
	 * 所以这里【不】另造 User.TrailTint：多一个参数就要多一层没人读的死配置。
	 * 换 NS 时如果它用的是别的名字，改这一个字段就行。
	 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	FName TrailTintParameter = TEXT("User.Color");

	// ---------------------------------------------------------------------
	// ③ 网络 / 收尾
	// ---------------------------------------------------------------------

	/**
	 * true = 只有本人看得见。
	 * ⚠️ 别照抄 GC_Stealth 的 SwordParticle（那个是 true，隐身时刀上的光只有主人能见）；
	 * 出拳时人已经现身，敌人该看到拳风 ⇒ 默认 false。
	 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara")
	bool bOnlyOwnerSee = false;

	/**
	 * NotifyEnd 之后等多久强制销毁组件。
	 * 正常路径是 Deactivate() + SetAutoDestroy，让已经生成的粒子自然收掉；
	 * 但 NS 的 emitter 要是设成无限循环，auto destroy 永远等不到 ——
	 * 那样每出一拳就泄漏一个常驻组件，所以留个兜底。
	 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara", meta = (ClampMin = "0.1", Units = "s"))
	float TeardownDelay = 1.f;

	/**
	 * 「montage 上这条 notify 的 Duration 被烘成 0」的兜底：时长是 0 的话 Begin / End 落在同一帧，
	 * 粒子刚 spawn 就被 NotifyEnd 收掉 —— 画面上就是「只有命中那一炸、过程完全没有」。
	 * 这时候不指望 montage 了，这里自己保证特效至少活这么久再收。
	 *
	 * 为什么敢直接撑住而不去修 montage：NS_Fist_Charge 这类特效勾了 Local Space 且整个系统
	 * SpawnSystemAttached 到了插槽上，**整坨粒子本来就跟着手动**，不需要 NotifyTick 每帧喂世界坐标；
	 * 所以「时长 0」损失的只是覆盖区间，视觉上仍是跟着拳走的。
	 */
	UPROPERTY(EditAnywhere, Category = "SocketNiagara", meta = (ClampMin = "0.05", Units = "s"))
	float MinVisibleSeconds = 0.35f;

private:
	/** 一次出拳给同一条 mesh 生出的两侧拖尾。 */
	struct FActiveSocketNiagaras
	{
		TObjectPtr<UNiagaraComponent> Right;
		TObjectPtr<UNiagaraComponent> Left;

		/** 「时长 0」兜底的收尾定时器。 */
		FTimerHandle FadeTimer;

		/** true = 收尾由 FadeTimer 接管，NotifyEnd 别在同一帧把粒子收掉。 */
		bool bSelfDriven = false;
	};

	/** 按 mesh 分开存：notify 实例是所有角色共用的。键用弱引用，mesh 没了自动失效。 */
	TMap<TWeakObjectPtr<USkeletalMeshComponent>, FActiveSocketNiagaras> ActiveByMesh;

	/** 生成一侧。插槽不存在时打日志并返回 nullptr（GetSocketLocation 会静默返回组件位置）。 */
	UNiagaraComponent* SpawnOne(USkeletalMeshComponent* MeshComp, UNiagaraSystem* SystemAsset, bool bRight) const;

	/** 每帧把插槽的 base / tip 世界坐标喂给 NS 的两个位置参数（见 NotifyTick 的说明）。 */
	void UpdateTrail(UNiagaraComponent* Component, USkeletalMeshComponent* MeshComp, bool bRight) const;

	/** 停掉一个组件并挂上兜底销毁的定时器。 */
	static void Teardown(UNiagaraComponent* Component, float DelaySeconds);
};
