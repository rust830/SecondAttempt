// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroHealthBarWidget.h"

#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Materials/MaterialInstanceDynamic.h"

void UHeroHealthBarWidget::ApplyVitals(const FHUDVitalsView& Vitals)
{
	LastVitals = Vitals;

	const float Current = bIsEnergyBar ? Vitals.Energy : Vitals.Health;
	const float Max = bIsEnergyBar ? Vitals.MaxEnergy : Vitals.MaxHealth;
	const float Percent = bIsEnergyBar ? Vitals.EnergyPercent : Vitals.HealthPercent;

	if (Bar)
	{
		Bar->SetPercent(Percent);

		// 比的是比例不是绝对值：这条血条上其它东西（Percent）都是 0~1，
		// 拿 Current（1200 这种）去比会变成「几乎永远不触发」，MaxHealth 一改更没法看。
		const bool bLow = LowValueThreshold > 0.f && Percent <= LowValueThreshold;
		Bar->SetFillColorAndOpacity(bLow ? LowFillColor : FillColor);
	}

	if (NumericText && bShowNumericText)
	{
		// 整数显示：血条上出现小数没意义，而且每一帧长度都在变，反而更难读。
		const int32 CurrentInt = FMath::CeilToInt(Current);
		const int32 MaxInt = FMath::CeilToInt(Max);

		// 整数没变就不重建文本 —— FText::Format + AsNumber 是这一路里最贵的一步，
		// 而血条的整数显示在挨打时大多数帧是不变的（头顶血条 × N 个敌人时更明显）。
		if (CurrentInt != LastNumericCurrent || MaxInt != LastNumericMax)
		{
			LastNumericCurrent = CurrentInt;
			LastNumericMax = MaxInt;

			if (NumericFormatPattern.IsEmpty())
			{
				NumericFormatPattern = NSLOCTEXT("HeroHUD", "VitalsNumericFormat", "{0} / {1}");
			}

			NumericText->SetText(FText::Format(
				NumericFormatPattern, FText::AsNumber(CurrentInt), FText::AsNumber(MaxInt)));
		}
	}

	BP_OnVitalsChanged(Vitals, Percent, Current, Max);

	// 材质条：把比例喂给 Fill；白条残影的目标值也存成 Fill，真正的缓动在 NativeTick。
	if (FillImage)
	{
		if (UMaterialInstanceDynamic* Mid = FillImage->GetDynamicMaterial())
		{
			Mid->SetScalarParameterValue(TEXT("Fill"), Percent);
		}
	}
}

void UHeroHealthBarWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// 【白条要每帧缓动，不能只放在 ApplyVitals 里】掉血之后如果不再触发属性变化，
	// Ghost 就停在半路 —— 白条永远缩不回去。缓动的目标始终是当前的比例。
	if (FillImage)
	{
		const float Target = bIsEnergyBar ? LastVitals.EnergyPercent : LastVitals.HealthPercent;
		GhostPercent = FMath::FInterpTo(GhostPercent, Target, InDeltaTime, 2.5f);

		if (UMaterialInstanceDynamic* Mid = FillImage->GetDynamicMaterial())
		{
			Mid->SetScalarParameterValue(TEXT("Ghost"), GhostPercent);
		}
	}
}

float UHeroHealthBarWidget::GetLastPercent() const
{
	if (bIsEnergyBar)
	{
		return LastVitals.EnergyPercent;
	}
	return LastVitals.HealthPercent;
}
