// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpringArmComponent.h"

#include "LOLCameraBoom.generated.h"

/**
 * 弹簧臂的相机版：只改「撞到障碍之后怎么播放」这一件事。
 *
 * 【为什么要有这个子类】原生 USpringArmComponent::BlendLocations 是一行硬切：
 *
 *     return bHitSomething ? TraceHitLocation : DesiredArmLocation;
 *
 * 障碍物一进一出相机路径，相机就在「全长」和「命中点」之间【一帧瞬移】。
 * 位移技能扫过柱子/门框/箱子时这条路径每帧都在变，于是画面被推近又猛弹回远处 ——
 * 看到的「镜头被建筑挡一下、黑影一闪」就是这么来的。
 *
 * 【规则】收拢瞬时、展开平滑（相机碰撞的常规做法）：
 *   - 撞到 → 立刻收到命中点。慢一帧就意味着相机还留在障碍后面，比硬切更穿帮。
 *   - 障碍离开 → 按 ReleaseSpeed 平滑展开回全长，不再瞬移（这一下才是最常见的「一闪」）。
 *   - 再加一条下限 MinArmLength：贴着墙/贴着人站的时候，不把相机缩进角色自己身体里。
 *     相机停在胸口、眼前只有自己模型内部，是「镜头被挡住」里最难看的一种。
 *     贴墙时宁可让相机留在墙里（单面材质会「看穿」墙，只是穿帮），也不缩进自己身上（那是真看不见）。
 *
 * 【谁在用】ALOLCharacter 的 CameraBoom。两个数值都能在角色 BP 的 Components 面板里覆盖。
 */
UCLASS(ClassGroup = (Camera))
class LOL_API ULOLCameraBoom : public USpringArmComponent
{
	GENERATED_BODY()

public:
	ULOLCameraBoom();

	/**
	 * 障碍离开视线后，臂长展开回全长有多快（每秒插值速度，越大越快）。
	 * 只影响展开；收拢永远是瞬时的。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Collision", meta = (ClampMin = "0.1", UIMax = "20.0"))
	float ReleaseSpeed = 6.0f;

	/**
	 * 相机离枢轴最近能到多近（cm）。0 = 不限制（等同原生行为）。
	 * 按胶囊半径 35 + 模型自身厚度估的 120：再近就进角色身体了。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Collision", meta = (ClampMin = "0.0", UIMax = "400.0"))
	float MinArmLength = 120.0f;

protected:
	/** 引擎在这里给出「理想落点」和「命中点」，由子类决定最终落点。 */
	virtual FVector BlendLocations(const FVector& DesiredArmLocation, const FVector& TraceHitLocation, bool bHitSomething, float DeltaTime) override;

private:
	/** 上一次输出的「臂长占全长的比例」：0 = 贴在枢轴上，1 = 全长。展开时从这里起步。 */
	float SmoothedArmFraction = 1.0f;
};
