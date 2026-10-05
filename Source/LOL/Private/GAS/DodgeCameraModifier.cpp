// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/DodgeCameraModifier.h"
#include "Camera/CameraTypes.h"

UDodgeCameraModifier::UDodgeCameraModifier()
{
	// 推镜要【快】：完美闪避只有零点几秒，淡入比它慢的话镜头刚开始动就结束了。
	AlphaInTime = 0.05f;
	// 收回来要【慢】：这才是「子弹时间结束、世界恢复速度」那一下的余韵。
	// 收得太快会显得像镜头抖了一下，而不是一次定格。
	AlphaOutTime = 0.45f;
	// Priority 保持基类默认（127），不跟别的相机修改器抢顺序。
}

bool UDodgeCameraModifier::ModifyCamera(float DeltaTime, FMinimalViewInfo& InOutPOV)
{
	// 先让基类跑：UpdateAlpha(DeltaTime) 就在它里面（CameraModifier.cpp:24），
	// 不先调它的话下面读到的 Alpha 永远是上一帧的（第一帧是 0，等于白挂）。
	// 基类同时会把 BlueprintModifyCamera 也派发掉。
	Super::ModifyCamera(DeltaTime, InOutPOV);

	if (Alpha > KINDA_SMALL_NUMBER)
	{
		if (!bLoggedFirstApply)
		{
			bLoggedFirstApply = true;
			UE_LOG(LogTemp, Warning, TEXT("[Dodge] 完美闪避镜头修改器已生效：推近 %.1f 度（基准 FOV %.1f）"),
				ZoomInDegrees, InOutPOV.FOV);
		}

		// 按 Alpha 从「这一帧的基准 FOV」往目标插一次。
		// 不能累加：InOutPOV 每帧都从相机重新算，这里直接改就行（和 MyStealthCameraModifier 同一个口径）。
		// 下限钳一下：FOV 被推到 <=0 会直接渲染不出东西。
		InOutPOV.FOV = FMath::Max(1.f, InOutPOV.FOV - ZoomInDegrees * Alpha);
	}

	return false;
}
