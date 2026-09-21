// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "MyDeathHarvestCameraModifier.generated.h"

class UMaterialInterface;

/**
 * 大招传送期间【施法者本人】的屏幕表现：全屏后处理材质 + 内置的压暗/褪色。
 *
 * 做法和 UMyStealthCameraModifier 一字不差：淡入淡出、优先级、以及「谁最后写谁生效」这套顺序
 * 引擎已经管好了（Alpha 由基类按 AlphaInTime/AlphaOutTime 插值，DisableModifier() 淡完自动从
 * 相机管理器摘掉），调用方只需要 Add / Disable 两个动作，不用自己写清理。
 *
 * 只有施法者本机的相机会挂这个修改器（见 AGC_DeathHarvestCast::ApplyLocalScreen）——
 * 别人看我放大招不该跟着变屏幕。
 */
UCLASS()
class LOL_API UMyDeathHarvestCameraModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	UMyDeathHarvestCameraModifier();

	/** 传送期间的全屏后处理材质。留空则只吃下面两个内置参数。 */
	UPROPERTY(EditDefaultsOnly, Category = "Portal|Screen")
	TObjectPtr<UMaterialInterface> ScreenMaterial;

	/** 暗角强度目标（0~1）：从消失到现身把画面往中间收。 */
	UPROPERTY(EditDefaultsOnly, Category = "Portal|Screen", meta = (ClampMin = "0", ClampMax = "1"))
	float VoidVignette = 0.85f;

	/** 饱和度目标（1 = 原色，0 = 全灰）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Portal|Screen", meta = (ClampMin = "0", ClampMax = "1"))
	float VoidSaturation = 0.45f;

	// AlphaInTime / AlphaOutTime 在基类里是 protected，这里开两个只读口子给外部打印用（不改值）。
	float GetAlphaInTime() const { return AlphaInTime; }
	float GetAlphaOutTime() const { return AlphaOutTime; }

protected:
	/** 每帧被 APlayerCameraManager 调用，把本修改器的效果按 Alpha 混进后处理设置。 */
	virtual void ModifyPostProcess(float DeltaTime, float& PostProcessBlendWeight, FPostProcessSettings& PostProcessSettings) override;

private:
	/** 只让「修改器第一次真正生效」打一条日志，避免每帧刷屏。 */
	bool bLoggedFirstApply = false;
};
