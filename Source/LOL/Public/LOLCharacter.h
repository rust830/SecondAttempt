// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GAS/HeroCombatCharacter.h"
#include "Logging/LogMacros.h"
#include "LOLCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;

// ⚠️ 模板原本在这里声明了 `DECLARE_LOG_CATEGORY_EXTERN(LogTemplateCharacter, ...)`，
// 但整个模块里【没有对应的 DEFINE_LOG_CATEGORY】。UE 的宏只是声明一个 FLogCategory 的
// 引用，所以"声明缺定义"平时不报错 —— 直到有人真写 `UE_LOG(LogTemplateCharacter, ...)`
// 才会 LNK2019。既然零处使用，直接删掉声明，把这条引信拆了。
//
// ❗ 别把这个文件当"模板残留"整份删掉：`AArenaPlayerController` / `AArenaGameMode` 派生链、
// `BP_ThirdPerson*` 蓝图、`Config/DefaultEngine.ini` 里的 ActiveClassRedirects 都指向它。

/**
 *  A simple player-controllable third person character
 *  Implements a controllable orbiting camera
 */
UCLASS(abstract)
class ALOLCharacter : public AHeroCombatCharacter
{
	GENERATED_BODY()

	/** Camera boom positioning the camera behind the character */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;

	/** Follow camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> FollowCamera;

public:

	/** Constructor */
	ALOLCharacter();

public:

	/** Handles move inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoMove(float Right, float Forward);

	/** Handles look inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoLook(float Yaw, float Pitch);

	/** Handles jump pressed inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpStart();

	/** Handles jump pressed inputs from either controls or UI interfaces */
	UFUNCTION(BlueprintCallable, Category="Input")
	virtual void DoJumpEnd();

public:

	/** Returns CameraBoom subobject **/
	FORCEINLINE class USpringArmComponent* GetCameraBoom() const { return CameraBoom; }

	/**
	 * 【挡住镜头的"别人"】这个角色离「本机相机 → 本机角色」这条线段多近时，
	 * 本机客户端就把它的胶囊算成相机障碍（ECC_Camera 挡），让本机弹簧臂收短。
	 * 0 = 关掉这条规则（退回"只有世界几何能挡相机"）。
	 *
	 * 只在【本机】生效：碰撞响应不复制，别的玩家看不到、也影响不到命中判定 ——
	 * ECC_Camera 这个通道全工程只有弹簧臂探针在用（其余射线走 Visibility / Pawn）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Collision", meta = (ClampMin = "0.0", UIMax = "400.0"))
	float CameraOccluderRadius = 150.f;

	/**
	 * 【按相机距离淡出模型】只淡【别人】—— 自己的模型就是你要看的东西，本机角色永远不淡。
	 * 判据是"这个身体离「本机相机 → 本机角色」这条线段的距离"：贴脸、以及夹在
	 * 相机和本机角色中间的身体都算。60cm 全透明，150cm 全实心。
	 *
	 * 材质侧：CameraFade 这个 ScalarParameter 走的是 **MaterialAttributes 链**（乘在
	 * attributes 的 OpacityMask 上），不能直接往材质节点的 OpacityMask 引脚上接 ——
	 * 引擎里 `FMaterialCachedExpressionData::UpdatePropertyConnections` 在
	 * `bUseMaterialAttributes` 为真时会把所有属性连接全部跳过，接上去编译不报错但不生效。
	 */
	virtual void Tick(float DeltaSeconds) override;

	/** Returns FollowCamera subobject **/
	FORCEINLINE class UCameraComponent* GetFollowCamera() const { return FollowCamera; }
};
