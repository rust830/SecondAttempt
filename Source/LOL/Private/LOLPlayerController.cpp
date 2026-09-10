// Copyright Epic Games, Inc. All Rights Reserved.


#include "LOLPlayerController.h"
#include "LOLCharacter.h"
#include "InputConfig.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedInputComponent.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "InputActionValue.h"
#include "Blueprint/UserWidget.h"
#include "LOL.h"
#include "Widgets/Input/SVirtualJoystick.h"

void ALOLPlayerController::BeginPlay()
{
	Super::BeginPlay();

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
