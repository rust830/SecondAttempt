// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroTargetFrameWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"

#include "UI/HeroHealthBarWidget.h"

void UHeroTargetFrameWidget::ResolveOptionalChildren()
{
	// 一次就够：控件树在这个 Widget 的生命周期里不会变（换布局等于换一个 WBP 实例）。
	// 不置位就返回的那条路是留给"树还没建好"的 —— 那种情况下一次都不算扫过。
	if (bOptionalChildrenResolved)
	{
		return;
	}

	if (!WidgetTree)
	{
		return;
	}

	bOptionalChildrenResolved = true;

	WidgetTree->ForEachWidget([this](UWidget* Widget)
	{
		UHeroHealthBarWidget* Bar = Cast<UHeroHealthBarWidget>(Widget);
		if (!Bar)
		{
			return;
		}

		// bIsEnergyBar 决定这一条归谁。两个属性各自判一次空 ——
		// 树里只有一条的时候，另一条保持空，和"WBP 里没摆"完全一样。
		if (Bar->IsEnergyBar())
		{
			if (!EnergyBar) { EnergyBar = Bar; }
		}
		else if (!HealthBar)
		{
			HealthBar = Bar;
		}
	});
}

void UHeroTargetFrameWidget::ApplyTargetFrame(const FTargetFrameView& View)
{
	// 【放在最前面】没有目标时会从下面直接 return，接不到控件的话"有目标"那一帧也没人填。
	// 它自己是幂等的（接上了就直接返回），所以每次都调不会白扫树。
	ResolveOptionalChildren();

	TargetFrame = View;

	// ---------------------------------------------------------------------
	// 显隐：没有目标就整个收起。这是语义性的（没目标就不该出现在屏幕上），
	// 所以不记录「美术配的可见性」—— 目标框显示时就是完整显示。
	// HitTestInvisible：目标框在屏幕中央，不该挡住鼠标点击（同头顶血条的处理）。
	// ---------------------------------------------------------------------
	if (!View.bHasTarget)
	{
		SetVisibility(ESlateVisibility::Collapsed);
		BP_OnTargetFrameChanged(View);
		return;
	}

	SetVisibility(ESlateVisibility::HitTestInvisible);

	// ---------------------------------------------------------------------
	// 名字
	// ---------------------------------------------------------------------
	if (NameText)
	{
		NameText->SetText(View.DisplayName);
	}

	// ---------------------------------------------------------------------
	// 血 / 能量：转成 FHUDVitalsView 喂给复用的条。哪条显示哪一对数值由条自己的
	// bIsEnergyBar 决定 —— 这里不替它选，和 WBP_HUD 里的 HealthBar / EnergyBar 完全一致。
	// ---------------------------------------------------------------------
	FHUDVitalsView Vitals;
	Vitals.Health = View.Health;
	Vitals.MaxHealth = View.MaxHealth;
	Vitals.HealthPercent = View.HealthPercent;
	Vitals.Energy = View.Energy;
	Vitals.MaxEnergy = View.MaxEnergy;
	Vitals.EnergyPercent = View.EnergyPercent;

	if (HealthBar)
	{
		HealthBar->ApplyVitals(Vitals);
	}

	if (EnergyBar && bShowEnergyBar)
	{
		EnergyBar->ApplyVitals(Vitals);
	}

	// ---------------------------------------------------------------------
	// 死亡压暗
	// ---------------------------------------------------------------------
	if (DeadOverlay)
	{
		DeadOverlay->SetVisibility(View.bIsDead ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}

	BP_OnTargetFrameChanged(View);
}
