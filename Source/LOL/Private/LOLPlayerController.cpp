// Copyright Epic Games, Inc. All Rights Reserved.


#include "LOLPlayerController.h"
#include "LOLCharacter.h"
#include "GAS/HeroHUDController.h"
#include "GAS/HeroHUDSlotConfig.h"
#include "GAS/InputConfig.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedInputComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "InputActionValue.h"
#include "Blueprint/UserWidget.h"
#include "LOL.h"
#include "Widgets/Input/SVirtualJoystick.h"

void ALOLPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// HUD 要在触控控件之前建：两者 z-order 都是 0，同层时后加的在上，
	// 所以 HUD 在下面 —— 移动端的虚拟摇杆才不会被 HUD 挡住。
	CreateHUDForLocalPlayer();

	// only spawn touch controls on local player controllers
	if (IsLocalPlayerController() && ShouldUseTouchControls())
	{
		// spawn the mobile controls widget
		MobileControlsWidget = CreateWidget<UUserWidget>(this, MobileControlsWidgetClass);

		if (MobileControlsWidget)
		{
			// add the controls to the player screen
			MobileControlsWidget->AddToPlayerScreen(0);

		} else {

			UE_LOG(LogLOL, Error, TEXT("Could not spawn mobile controls widget."));

		}

	}
}

void ALOLPlayerController::CreateHUDForLocalPlayer()
{
	// 门在这里，不在 Character 上：IsLocalPlayerController() 是 listen server 上
	// 区分「主机玩家」和「远端客户端玩家」的唯一正确判据。
	// 只建一次：PC 的 BeginPlay 只会跑一次，但重进关卡时会换新 PC，所以不用额外状态。
	if (!IsLocalPlayerController() || HUDController)
	{
		return;
	}

	// Outer 必须是这个 PC —— UHeroHUDController::ResolveSelfASC 是顺着 Outer 找 PlayerState 的。
	HUDController = NewObject<UHeroHUDController>(this, TEXT("HeroHUDController"));
	HUDController->SetSlotConfig(HUDSlotConfig);

	// 先绑、再建 Widget。
	//
	// 反过来也能work（Widget 会先订阅再拉取，也能补齐状态），但会多一次可见的跳变：
	// Widget 第一次 NativeConstruct 拉到的是「未就绪」快照，得等 OnHUDReady 才修正。
	// 先绑的话第一份快照就是真实状态。
	HUDController->TryBindHUD();

	if (HUDWidgetClass)
	{
		HUDWidget = CreateWidget<UUserWidget>(this, HUDWidgetClass);

		if (HUDWidget)
		{
			HUDWidget->AddToPlayerScreen(0);
		}
		else
		{
			UE_LOG(LogLOL, Error, TEXT("Could not spawn HUD widget: %s"), *GetNameSafe(HUDWidgetClass));
		}
	}
}

void ALOLPlayerController::NotifyHUDTryBind()
{
	// 幂等性全在 UHeroHUDController::TryBindHUD 里（幂等键是「绑的是不是同一个 ASC」），
	// 所以这三个调用点直接调、不用各自判断。
	if (HUDController)
	{
		HUDController->TryBindHUD();
	}
}

void ALOLPlayerController::AcknowledgePossession(APawn* P)
{
	Super::AcknowledgePossession(P);

	// 客户端路径。主机的 AcknowledgePossession 不触发 —— 主机靠 BeginPlay + Controller 的有界重试。
	NotifyHUDTryBind();
}

void ALOLPlayerController::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	// 客户端路径：PS 复制下来之后 ASC 才真的能用。
	NotifyHUDTryBind();
}

void ALOLPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 先让 Controller 清干净：它持有标签事件句柄和定时器，
	// 不主动解绑的话 Widget 会挂在已经销毁的 ASC 上。
	if (HUDController)
	{
		HUDController->ShutdownHUD();
		HUDController = nullptr;
	}

	HUDWidget = nullptr;

	Super::EndPlay(EndPlayReason);
}

void ALOLPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// only add IMCs for local player controllers
	if (IsLocalPlayerController())
	{
		// Add Input Mapping Contexts
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			for (TObjectPtr<UInputMappingContext> CurrentContext : DefaultMappingContexts)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}

			// only add these IMCs if we're not using mobile touch input
			if (!ShouldUseTouchControls())
			{
				for (TObjectPtr<UInputMappingContext> CurrentContext : MobileExcludedMappingContexts)
				{
					Subsystem->AddMappingContext(CurrentContext, 0);
				}
			}
		}

		// Set up action bindings
		if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
		{
			// Jumping
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &ALOLPlayerController::JumpStarted);
			EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ALOLPlayerController::JumpEnded);

			// Moving
			EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ALOLPlayerController::Move);
			EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ALOLPlayerController::Look);

			// Looking
			EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ALOLPlayerController::Look);

			// Ability slots (QWER / DF), driven by the InputConfig data asset.
			if (AbilityInputConfig)
			{
				for (const FAbilityInputAction& Entry : AbilityInputConfig->AbilityInputActions)
				{
					if (Entry.InputAction && Entry.SlotTag.IsValid())
					{
						FGameplayTag Tag = Entry.SlotTag;
						// 用 Started 而不是 Triggered：数字按键用 Triggered 在某些 InputAction 配置下会一次按下触发两次，
						// 导致「按一下 E → 进瞄准 → 立刻再按事件取消」。Started 对数字按键保证每按一次只回调一次。
						EnhancedInputComponent->BindAction(Entry.InputAction, ETriggerEvent::Started, this, &ThisClass::AbilityInputStarted, Tag);
						// 松开那一下也要：R 的「按住选目标」靠它收尾（松开 = 这次不当数）。
						// 对别的槽位是空转 —— AHeroCombatCharacter::AbilityInputTagReleased 只认自己开着的那个窗口。
						EnhancedInputComponent->BindAction(Entry.InputAction, ETriggerEvent::Completed, this, &ThisClass::AbilityInputCompleted, Tag);
					}
				}
			}

			// Intentionally raw: this makes LMB available in the shipped ThirdPerson mapping immediately.
			EnhancedInputComponent->BindAction(BasicAttackAction, ETriggerEvent::Started, this, &ALOLPlayerController::BasicAttackStarted);
		}
		else
		{
			UE_LOG(LogLOL, Error, TEXT("'%s' Failed to find an Enhanced Input component! This template is built to use the Enhanced Input system. If you intend to use the legacy system, then you will need to update this C++ file."), *GetNameSafe(this));
		}
	}
}

bool ALOLPlayerController::ShouldUseTouchControls() const
{
	// are we on a mobile platform? Should we force touch?
	return SVirtualJoystick::ShouldDisplayTouchInterface() || bForceTouchControls;
}

void ALOLPlayerController::Move(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D MovementVector = Value.Get<FVector2D>();

	if (ALOLCharacter* MyCharacter = GetPawn<ALOLCharacter>())
	{
		MyCharacter->DoMove(MovementVector.X, MovementVector.Y);
	}
}

void ALOLPlayerController::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (ALOLCharacter* MyCharacter = GetPawn<ALOLCharacter>())
	{
		MyCharacter->DoLook(LookAxisVector.X, LookAxisVector.Y);
	}
}

void ALOLPlayerController::JumpStarted()
{
	if (ALOLCharacter* MyCharacter = GetPawn<ALOLCharacter>())
	{
		MyCharacter->DoJumpStart();
	}
}

void ALOLPlayerController::JumpEnded()
{
	if (ALOLCharacter* MyCharacter = GetPawn<ALOLCharacter>())
	{
		MyCharacter->DoJumpEnd();
	}
}

void ALOLPlayerController::BasicAttackStarted()
{
	if (ALOLCharacter* MyCharacter = GetPawn<ALOLCharacter>())
	{
		MyCharacter->BasicAttackPressed();
	}
}

void ALOLPlayerController::AbilityInputStarted(FGameplayTag SlotTag)
{
	if (ALOLCharacter* MyCharacter = GetPawn<ALOLCharacter>())
	{
		MyCharacter->AbilityInputTagPressed(SlotTag);
	}
}

void ALOLPlayerController::AbilityInputCompleted(FGameplayTag SlotTag)
{
	// Started 的反面。用 Completed 而不是 Released：数字键（bool 值）的 Released 只在「触发之后」
	// 才会来，而 Completed 在按下-抬起这一对里一定会来一次 —— 我们要的是「键抬起来了」这个事实。
	if (ALOLCharacter* MyCharacter = GetPawn<ALOLCharacter>())
	{
		MyCharacter->AbilityInputTagReleased(SlotTag);
	}
}
