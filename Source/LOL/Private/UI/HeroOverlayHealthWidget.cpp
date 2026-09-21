// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroOverlayHealthWidget.h"

#include "UI/HeroHealthBarWidget.h"

void UHeroOverlayHealthWidget::ApplyOverlayVitals(
	const FHUDVitalsView& Vitals, bool bIsDead, bool bHideWhenFullHealth, bool bHideWhenDead)
{
	// 可见性先算：隐藏的时候还把子控件刷一遍是白做功，而且会让「淡出动画」的起点不对。
	const bool bFull = Vitals.MaxHealth > 0.f && Vitals.Health >= Vitals.MaxHealth;
	const bool bVisible = !(bHideWhenDead && bIsDead) && !(bHideWhenFullHealth && bFull);

	SetVisibility(bVisible ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);

	if (bVisible && HealthBar)
	{
		// 复用 HUD 血条那套渲染：ApplyVitals 只认 FHUDVitalsView，
		// 它不知道自己在头顶还是在屏幕底下，也不需要知道。
		HealthBar->ApplyVitals(Vitals);
	}

	BP_OnOverlayVitalsChanged(Vitals, bIsDead);
}
