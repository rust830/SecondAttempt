// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MyCharacter.generated.h"

class UCameraComponent;
class USpringArmComponent;
class UInputAction;

/**
 * ⚠️ 【模板残留，零引用 —— 保留是有意的】
 *
 * 这是 ThirdPerson 模板搭新项目时生成的第二个角色类，和 `ALOLCharacter`（真正的
 * 玩家角色，继承 `AHeroCombatCharacter`）**没有任何继承或引用关系**。全项目零处
 * `AMyCharacter` / `MyCharacter` 的 C++ 引用，`Content/` 下也没有蓝图引用它
 * （可以 `load_class` 复核）。它只有一个空 Tick 和一个空 BeginPlay。
 *
 * 【为什么不删 —— 按项目决策"死代码不删，要留架构空间"】
 * 它是 UE 模板的"最小可玩第三人称角色"参照：SpringArm + 相机跟随的最小实现。
 * 将来若要做【非 GAS 的普通角色】（比如观战位、竞技场里的 NPC 木桩靶、
 * 载具驾驶员），从这里复制一份比从 `ALOLCharacter` 复制省事得多 ——
 * 后者带着 141 根骨骼的 GAS 属性集、形态标签、Cooldown GE，复制过去要拆的东西太多。
 *
 * ⚠️ 别把它当"没用的东西"顺手删掉：上面那个复制场景就是它的存在理由。
 * 但也【别拿它当玩家角色】—— 它没有 GAS、没有输入绑定、Tick 是空的，
 * 派生类什么都要自己补。
 */
UCLASS()
class LOL_API AMyCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	// Sets default values for this character's properties
	AMyCharacter();
	UPROPERTY(BlueprintReadWrite,EditAnywhere,Category="Camera")
	TObjectPtr<UCameraComponent> FollowCamera;
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Camera")
	TObjectPtr<USpringArmComponent> CameraBoom;



protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Called to bind functionality to input
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

};
