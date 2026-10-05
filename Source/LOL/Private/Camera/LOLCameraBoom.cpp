// Fill out your copyright notice in the Description page of Project Settings.

#include "Camera/LOLCameraBoom.h"

ULOLCameraBoom::ULOLCameraBoom()
{
	// 不覆盖任何原生默认值：这个类只多出「展开平滑」和「最小臂长」两条规则，
	// 其余（臂长 400 / 探针通道 / 探针大小）仍然由 ALOLCharacter 或角色 BP 指定。
}

FVector ULOLCameraBoom::BlendLocations(const FVector& DesiredArmLocation, const FVector& TraceHitLocation, bool bHitSomething, float DeltaTime)
{
	// 枢轴 = 组件位置 + TargetOffset —— UpdateDesiredArmLocation 里就是这么算 ArmOrigin 的
	// （和开不开位置滞后无关：滞后只作用在 DesiredLoc 上，不动 ArmOrigin）。
	// 命中点一定落在这条「枢轴 → 理想落点」线段上，所以「到枢轴的距离 / 全长」就是臂长还剩几成。
	// 全程只用这一个比例表达收缩程度，不去碰方向：SocketOffset 让这条线不是纯径向的，
	// 按比例缩放两个端点的连线最省事，也不会算错。
	const FVector Pivot = GetComponentLocation() + TargetOffset;
	const FVector FullOffset = DesiredArmLocation - Pivot;
	const float FullDist = FullOffset.Size();

	// DeltaTime == 0 是 OnRegister 那次初始化调用：没有上一帧可参考，
	// 直接用引擎原本的算法。不特判的话第一帧会从一个没意义的比例起步，相机从角色身体里飞出来。
	if (DeltaTime <= 0.f || FullDist <= UE_KINDA_SMALL_NUMBER)
	{
		SmoothedArmFraction = 1.f;
		return bHitSomething ? TraceHitLocation : DesiredArmLocation;
	}

	if (bHitSomething)
	{
		// 收拢：只许变短，而且瞬时。慢一帧 = 这一帧相机还在障碍后面。
		const float HitFraction = FMath::Clamp(FVector::Dist(Pivot, TraceHitLocation) / FullDist, 0.f, 1.f);
		SmoothedArmFraction = FMath::Min(SmoothedArmFraction, HitFraction);
	}
	else
	{
		// 展开：平滑回全长。原生在这里瞬移，就是「画面猛然后退」那一下。
		SmoothedArmFraction = FMath::FInterpTo(SmoothedArmFraction, 1.f, DeltaTime, ReleaseSpeed);
	}

	// 下限：贴墙/贴人时宁可让相机留在墙里（单面材质会看穿墙），也不许缩进角色自己身体。
	const float MinFraction = (MinArmLength > 0.f) ? FMath::Min(1.f, MinArmLength / FullDist) : 0.f;
	return Pivot + FullOffset * FMath::Max(SmoothedArmFraction, MinFraction);
}
