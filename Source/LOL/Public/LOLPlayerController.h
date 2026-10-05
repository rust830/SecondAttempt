// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GameplayTagContainer.h"
#include "LOLPlayerController.generated.h"

class UInputMappingContext;
class UUserWidget;
class UInputAction;
class UInputConfig;
class UHeroAttributePanelConfig;
class UHeroHUDController;
class UHeroHUDSlotConfig;
struct FInputActionValue;

/**
 *  Basic PlayerController class for a third person game
 *  Manages input mappings and action bindings
 */
UCLASS(abstract)
class ALOLPlayerController : public APlayerController
{
	GENERATED_BODY()

protected:

	/** Jump Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> JumpAction;

	/** Move Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MoveAction;

	/** Look Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> LookAction;

	/** Mouse Look Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MouseLookAction;

	/** Basic attack (LMB) Input Action */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> BasicAttackAction;

	/**
	 * 展开 / 收起属性面板（建议映射到 C 键）。
	 *
	 * 留空 = 这个键不工作，其余照常 —— 和别的 InputAction 一样，不配就是没绑定，
	 * 不需要额外的开关变量。
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> ToggleAttributePanelAction;

	/** Input Mapping Contexts */
	UPROPERTY(EditAnywhere, Category ="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/** Input Mapping Contexts */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> MobileExcludedMappingContexts;

	/** Mobile controls widget to spawn */
	UPROPERTY(EditAnywhere, Category="Input|Touch Controls")
	TSubclassOf<UUserWidget> MobileControlsWidgetClass;

	/** Pointer to the mobile controls widget */
	UPROPERTY()
	TObjectPtr<UUserWidget> MobileControlsWidget;

	/** If true, the player will use UMG touch controls even if not playing on mobile platforms */
	UPROPERTY(EditAnywhere, Config, Category = "Input|Touch Controls")
	bool bForceTouchControls = false;

	/** 技能槽位输入映射（QWER/DF → 槽位标签）。 */
	UPROPERTY(EditAnywhere, Category = "Input|Abilities")
	TObjectPtr<UInputConfig> AbilityInputConfig;

	// -----------------------------------------------------------------------
	// HUD
	//
	// HUD 只给本地玩家建：Controller 和 Widget 都在 CreateHUDForLocalPlayer 里创建，
	// 由 IsLocalPlayerController() 门住。listen server 上远端客户端的 PC 也会跑 BeginPlay，
	// 不门住就会给每个玩家各建一份（主机还会看到别人的血条）。
	// -----------------------------------------------------------------------

	/** HUD 翻译层。只有本地玩家且已创建时才非空。 */
	UPROPERTY(Transient)
	TObjectPtr<UHeroHUDController> HUDController;

	/** HUD 根 Widget（WBP_HUD）。 */
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> HUDWidget;

	/** HUD 根 Widget 类。在 BP_LOLPlayerController 上配。 */
	UPROPERTY(EditDefaultsOnly, Category = "HUD")
	TSubclassOf<UUserWidget> HUDWidgetClass;

	/**
	 * 技能栏配置：槽位顺序 + 槽位标签 → 冷却标签的映射 + 图标 / 键位 / 名字。
	 * 不写死在 C++，理由同 AbilitySet / InputConfig。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "HUD")
	TObjectPtr<UHeroHUDSlotConfig> HUDSlotConfig;

	/**
	 * 属性面板配置：显示哪些属性、按什么顺序、常驻哪几条。
	 * 同时决定 HUD 订阅哪几条属性（见 UHeroHUDController::BindSelf），所以【不能为空】——
	 * 为空时面板是空的，Controller 会打一条 Warning。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "HUD")
	TObjectPtr<UHeroAttributePanelConfig> AttributePanelConfig;

public:

	// -----------------------------------------------------------------------
	// HUD 对外入口
	//
	// Widget / 蓝图只能从这里拿翻译层 —— 不能反过来从 Widget 去摸 PlayerState / ASC。
	// 放在 public 而不是 protected：HeroHUDWidget 的 NativeConstruct 要调它。
	// -----------------------------------------------------------------------

	/** Widget / 蓝图拿 HUD 翻译层的入口。没建（非本地玩家 / 还没建）时返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	UHeroHUDController* GetHUDController() const { return HUDController; }

protected:

	/** Gameplay initialization */
	virtual void BeginPlay() override;

	/** Input mapping context setup */
	virtual void SetupInputComponent() override;

	/**
	 * HUD 绑定的时机之一：客户端拿到 Pawn 之后 PS 可能才刚到位。
	 * 主机（listen server）不会走这里 —— 主机的 AcknowledgePossession 不触发，
	 * 所以还有 BeginPlay 和 Controller 内部的有界重试兜底。
	 */
	virtual void AcknowledgePossession(APawn* P) override;

	/** HUD 绑定的时机之二：客户端的 PS 是复制过来的，这里才是第一次真正能用。 */
	virtual void OnRep_PlayerState() override;

	/** 收尾：清定时器、解绑所有委托，不然 Widget 会挂在已销毁的 ASC 上。 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Returns true if the player should use UMG touch controls */
	bool ShouldUseTouchControls() const;

private:

	/** 只给本地玩家建 HUD（幂等）。 */
	void CreateHUDForLocalPlayer();

	/** 三个绑定时机都调它，幂等性由 UHeroHUDController::TryBindHUD 保证。 */
	void NotifyHUDTryBind();

	/** Called for movement input */
	void Move(const FInputActionValue& Value);

	/** Called for looking input */
	void Look(const FInputActionValue& Value);

	/** Called for jump pressed input */
	void JumpStarted();

	/** Called for jump released input */
	void JumpEnded();

	/** Raw mouse fallback so the current template map works without creating another Input Action asset. */
	void BasicAttackStarted();

	/**
	 * 属性面板开关（ToggleAttributePanelAction，Started）。
	 *
	 * HUD 还没建 / 没挂面板时静默 return —— 和 NotifyHUDTryBind 一样是幂等的空操作，
	 * 不值得为「开局第一帧按了 C」报警告。
	 */
	void ToggleAttributePanelStarted();

	/** Called for ability slot input (QWER/DF) */
	void AbilityInputStarted(FGameplayTag SlotTag);

	/** 同一个槽位键抬起（ETriggerEvent::Completed）。「按住选目标」那条链靠它收尾。 */
	void AbilityInputCompleted(FGameplayTag SlotTag);

};
