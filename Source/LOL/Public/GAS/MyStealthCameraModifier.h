// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "MyStealthCameraModifier.generated.h"

class UMaterialInterface;

/**
 * 隐身时的本地屏幕表现：全屏边缘框（自定义后处理材质）+ 潜行滤镜（内置参数）。
 *
 * 为什么用 CameraModifier 而不是直接改 PlayerCameraManager 的 PostProcessSettings：
 * 淡入淡出、优先级、以及「谁最后写谁生效」这套顺序引擎已经管好了，我们只负责按 Alpha 叠加。
 * Alpha 由基类按 AlphaInTime/AlphaOutTime 插值，DisableModifier() 淡完会自动从相机管理器摘掉，
 * 所以调用方只需要 Add / Disable 两个动作，不用自己写清理。
 *
 * 只有本地控制端才会挂这个修改器（见 AGC_Stealth），敌人隐身不会改你的屏幕。
 */
UCLASS()
class LOL_API UMyStealthCameraModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	UMyStealthCameraModifier();

	/** 画边缘框的全屏后处理材质。留空则只吃下面几个内置参数。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Screen")
	TObjectPtr<UMaterialInterface> ScreenMaterial;

	/** 饱和度目标（0 = 全灰，1 = 原色）。潜行感靠压低饱和度。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Screen", meta = (ClampMin = "0", ClampMax = "1"))
	float StealthSaturation = 0.35f;

	/** 色调偏移目标，往冷蓝压。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Screen")
	FLinearColor StealthColorGain = FLinearColor(0.55f, 0.75f, 1.f, 1.f);

	/** 暗角强度目标（0~1）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Screen", meta = (ClampMin = "0", ClampMax = "1"))
	float StealthVignette = 0.8f;

	/** 色散强度目标（cm 级），轻微色边。 */
	UPROPERTY(EditDefaultsOnly, Category = "Stealth|Screen", meta = (ClampMin = "0"))
	float StealthSceneFringe = 2.5f;

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
