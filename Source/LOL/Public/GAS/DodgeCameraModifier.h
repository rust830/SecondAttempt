// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Camera/CameraModifier.h"
#include "DodgeCameraModifier.generated.h"

/**
 * 完美闪避的镜头推近（FOV）。只有本地控制端会挂它，敌人闪避不会改你的镜头。
 *
 * 【为什么是 CameraModifier 而不是直接改相机组件】和 MyStealthCameraModifier 同一个理由：
 * 淡入淡出、优先级、以及「谁最后写谁生效」这套顺序引擎已经管好了 —— 我们只按基类插好的
 * Alpha 叠一个偏移，DisableModifier() 淡完自己从相机管理器摘掉，调用方只管 Add / Disable。
 *
 * 【为什么改 FOV 走 ModifyCamera 而不是 ModifyPostProcess】
 * 后处理设置里【没有】FOV 这一项（FPostProcessSettings 里不存在 bOverride_FieldOfView，
 * UE 5.8 的 Scene.h/CameraTypes.h 里都搜不到）。能改视野的只有这条：
 * UCameraModifier::ModifyCamera(float, FMinimalViewInfo&) —— 相机管理器每帧调它，
 * InOutPOV.FOV 就是这一帧真正拿去渲染的视野。
 */
UCLASS()
class LOL_API UDodgeCameraModifier : public UCameraModifier
{
	GENERATED_BODY()

public:
	UDodgeCameraModifier();

	/** 推近多少度（正数 = 视野变窄 = 画面拉近）。 */
	UPROPERTY(EditDefaultsOnly, Category = "Dodge|Camera", meta = (ClampMin = "0"))
	float ZoomInDegrees = 12.f;

protected:
	/**
	 * 每帧被 APlayerCameraManager 调用。返回 false = 继续往下跑别的修改器
	 *（返回 true 会把后面的整条链截断）。
	 */
	virtual bool ModifyCamera(float DeltaTime, struct FMinimalViewInfo& InOutPOV) override;

private:
	/** 只在第一次真正生效时打一条日志，别每帧刷屏。 */
	bool bLoggedFirstApply = false;
};
