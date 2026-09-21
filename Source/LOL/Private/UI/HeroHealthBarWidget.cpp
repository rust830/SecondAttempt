// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroHealthBarWidget.h"

#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"

void UHeroHealthBarWidget::ApplyVitals(const FHUDVitalsView& Vitals)
{
	LastVitals = Vitals;

	const float Current = bIsEnergyBar ? Vitals.Energy : Vitals.Health;
	const float Max = bIsEnergyBar ? Vitals.MaxEnergy : Vitals.MaxHealth;
	const float Percent = bIsEnergyBar ? Vitals.EnergyPercent : Vitals.HealthPercent;

	if (Bar)
	{
		Bar->SetPercent(Percent);

		const bool bLow = LowValueThreshold > 0.f && Current <= LowValueThreshold;
		Bar->SetFillColorAndOpacity(bLow ? LowFillColor : FillColor);
	}

	if (NumericText && bShowNumericText)
	{
		// 整数显示：血条上出现小数没意义，而且每一帧长度都在变，反而更难读。
		NumericText->SetText(FText::Format(
			NSLOCTEXT("HeroHUD", "VitalsNumericFormat", "{0} / {1}"),
			FText::AsNumber(FMath::CeilToInt(Current)),
			FText::AsNumber(FMath::CeilToInt(Max))));
	}

	BP_OnVitalsChanged(Vitals, Percent, Current, Max);
}

float UHeroHealthBarWidget::GetLastPercent() const
{
	if (bIsEnergyBar)
	{
		return LastVitals.EnergyPercent;
	}
	return LastVitals.HealthPercent;
}
