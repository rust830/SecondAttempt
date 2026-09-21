// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroHUDWidget.h"

#include "Blueprint/UserWidget.h"
#include "Components/PanelWidget.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"

#include "GAS/HeroHUDController.h"
#include "LOLPlayerController.h"
#include "UI/HeroHealthBarWidget.h"
#include "UI/HeroSkillSlotWidget.h"

// ===========================================================================
// 生命周期
// ===========================================================================

void UHeroHUDWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// 设计器里一行逻辑都不要跑：否则每打开一次 WBP_HUD 就是一堆空指针。
	if (IsDesignTime())
	{
		return;
	}

	BindToController();
}

void UHeroHUDWidget::NativeDestruct()
{
	StopControllerRetry();
	UnbindFromController();

	Super::NativeDestruct();
}

void UHeroHUDWidget::RefreshFromController()
{
	// 没绑上就当成一次「重来」——覆盖「Widget 建的时候 PC 还没建 Controller」这种情况。
	if (!HUDController)
	{
		BindToController();
		return;
	}

	ApplySnapshot(HUDController->PullHUDState());
}

// ===========================================================================
// 绑定
// ===========================================================================

void UHeroHUDWidget::BindToController()
{
	if (HUDController)
	{
		return;
	}

	ALOLPlayerController* PC = Cast<ALOLPlayerController>(GetOwningPlayer());
	if (!PC)
	{
		// 不是玩家 HUD（比如挂在 WidgetComponent 上给 AI 用）。不是错误，直接不管。
		return;
	}

	UHeroHUDController* Controller = PC->GetHUDController();
	if (!Controller)
	{
		// PC 还没建 Controller。PC 会先建 Controller 再建 Widget，所以正常路径上不会走到这里；
		// 走到这里说明这个 WBP_HUD 是被别处手工 Add 出来的，等一下就好。
		StartControllerRetry();
		return;
	}

	HUDController = Controller;
	StopControllerRetry();

	// 顺序：先订阅、再拉取。
	// 订阅管「之后的变化」，拉取管「此刻的状态」；反过来的话，两者之间发出的变化会丢。
	HUDController->OnVitalsChanged.AddDynamic(this, &UHeroHUDWidget::HandleVitalsChanged);
	HUDController->OnSkillSlotChanged.AddDynamic(this, &UHeroHUDWidget::HandleSkillSlotChanged);
	HUDController->OnTargetFrameChanged.AddDynamic(this, &UHeroHUDWidget::HandleTargetFrameChanged);
	HUDController->OnHUDReady.AddDynamic(this, &UHeroHUDWidget::HandleHUDReady);

	// 【订阅即拉取】。这就是「绑定不触发初始值」那个坑的正解：
	// 不管 Controller 是在这个 Widget 之前还是之后绑上 ASC 的，这里一定拿得到当前全量。
	// 所以 Controller 那边【不】需要「记得在正确时机 BroadcastInitialValues()」。
	ApplySnapshot(HUDController->PullHUDState());
}

void UHeroHUDWidget::UnbindFromController()
{
	if (!HUDController)
	{
		return;
	}

	HUDController->OnVitalsChanged.RemoveDynamic(this, &UHeroHUDWidget::HandleVitalsChanged);
	HUDController->OnSkillSlotChanged.RemoveDynamic(this, &UHeroHUDWidget::HandleSkillSlotChanged);
	HUDController->OnTargetFrameChanged.RemoveDynamic(this, &UHeroHUDWidget::HandleTargetFrameChanged);
	HUDController->OnHUDReady.RemoveDynamic(this, &UHeroHUDWidget::HandleHUDReady);

	HUDController = nullptr;
}

void UHeroHUDWidget::StartControllerRetry()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	if (++ControllerRetryCount > MaxControllerRetries)
	{
		ControllerRetryCount = 0;
		UE_LOG(LogTemp, Warning,
			TEXT("UHeroHUDWidget: %d 次重试后仍然拿不到 UHeroHUDController。")
			TEXT("这个 WBP_HUD 多半不是 ALOLPlayerController 建的。"),
			MaxControllerRetries);
		return;
	}

	World->GetTimerManager().SetTimer(
		ControllerRetryHandle, this, &UHeroHUDWidget::RefreshFromController, ControllerRetryInterval, /*bLoop=*/false);
}

void UHeroHUDWidget::StopControllerRetry()
{
	ControllerRetryCount = 0;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ControllerRetryHandle);
	}
}

// ===========================================================================
// 应用状态
// ===========================================================================

void UHeroHUDWidget::ApplySnapshot(const FHUDSnapshot& Snapshot)
{
	HandleVitalsChanged(Snapshot.Vitals);
	HandleTargetFrameChanged(Snapshot.Target);

	for (const FSkillSlotView& View : Snapshot.Slots)
	{
		HandleSkillSlotChanged(View.SlotIndex, View);
	}

	// 放最后：让蓝图在「已经知道全部状态」之后再决定显示/隐藏，不会先闪一下。
	HandleHUDReady(Snapshot.bBound);
}

void UHeroHUDWidget::HandleVitalsChanged(const FHUDVitalsView& Vitals)
{
	// 一个视图推给两条条：哪个是血、哪个是能量由条自己做主（bIsEnergyBar）。
	if (HealthBar)
	{
		HealthBar->ApplyVitals(Vitals);
	}
	if (EnergyBar)
	{
		EnergyBar->ApplyVitals(Vitals);
	}

	BP_OnVitalsChanged(Vitals);
}

void UHeroHUDWidget::HandleSkillSlotChanged(int32 SlotIndex, const FSkillSlotView& View)
{
	if (UHeroSkillSlotWidget* SlotWidget = EnsureSlotWidget(SlotIndex))
	{
		SlotWidget->ApplySlotView(View);
	}

	BP_OnSkillSlotChanged(SlotIndex, View);
}

void UHeroHUDWidget::HandleTargetFrameChanged(const FTargetFrameView& View)
{
	// 目标框没有内置子 Widget：它长什么样、放哪、什么时候收起来，全由蓝图决定。
	BP_OnTargetFrameChanged(View);
}

void UHeroHUDWidget::HandleHUDReady(bool bBound)
{
	BP_OnHUDReady(bBound);
}

UHeroSkillSlotWidget* UHeroHUDWidget::EnsureSlotWidget(int32 SlotIndex)
{
	if (SlotIndex == INDEX_NONE)
	{
		return nullptr;
	}

	if (SlotWidgets.IsValidIndex(SlotIndex) && SlotWidgets[SlotIndex])
	{
		return SlotWidgets[SlotIndex];
	}

	if (!SkillSlotContainer || !SkillSlotWidgetClass)
	{
		return nullptr;
	}

	UHeroSkillSlotWidget* NewWidget = CreateWidget<UHeroSkillSlotWidget>(GetOwningPlayer(), SkillSlotWidgetClass);
	if (!NewWidget)
	{
		return nullptr;
	}

	NewWidget->SetSlotIndex(SlotIndex);
	SkillSlotContainer->AddChild(NewWidget);

	// 用 SetNum 而不是 Add：槽位号必须等于数组下标。
	// 一旦用 Add，中间的空位就会把下标整体左移，「按 SlotIndex 找 Widget」全部错位。
	if (!SlotWidgets.IsValidIndex(SlotIndex))
	{
		SlotWidgets.SetNum(SlotIndex + 1);
	}
	SlotWidgets[SlotIndex] = NewWidget;

	return NewWidget;
}
