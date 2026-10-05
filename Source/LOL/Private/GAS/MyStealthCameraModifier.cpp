// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/MyStealthCameraModifier.h"
#include "CameraModifierLerp.h"
#include "Engine/Scene.h"
#include "Materials/MaterialInterface.h"

UMyStealthCameraModifier::UMyStealthCameraModifier()
{
	// 进出隐身各 0.35s 的过渡，避免屏幕效果硬切。
	AlphaInTime = 0.35f;
	AlphaOutTime = 0.35f;
	// Priority 保持基类默认（127），不跟别的相机修改器抢顺序。
}

void UMyStealthCameraModifier::ModifyPostProcess(float DeltaTime, float& PostProcessBlendWeight, FPostProcessSettings& PP)
{
	// Alpha 是基类插值好的淡入淡出进度；淡到 0 就什么都不叠，让 PP 保持相机原样。
	if (Alpha <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// 这个权重是「整份 PP 设置要不要被采纳」的开关，不是强度：
	// UCameraModifier::ModifyCamera 里写着 `if (PPBlendWeight > 0.f) { CameraOwner->AddCachedPPBlend(...) }`，
	// 权重留在 0 → 下面设的所有 bOverride_xxx 连同 WeightedBlendables 会被整份丢掉，屏幕一点变化都没有。
	// 之前它只在 ScreenMaterial 非空的分支里赋值，而材质是 EditDefaultsOnly、没人建蓝图子类 → 永远是空的 →
	// 内置滤镜全部失效。所以这里无条件置 1：本修改器在生效期间就该被完整采纳，
	// 具体强弱交给 Alpha 和各自的 bOverride 目标值表达。
	PostProcessBlendWeight = 1.f;

	if (!bLoggedFirstApply)
	{
		bLoggedFirstApply = true;
		UE_LOG(LogTemp, Warning, TEXT("[Stealth] 相机修改器已生效: Alpha=%.2f ScreenMaterial=%s"),
			Alpha, *GetNameSafe(ScreenMaterial));
	}

	// 全屏边缘框材质：按 Alpha 加权塞进 blendable 列表，混合交给引擎。
	// 材质里不要让权重参与运算（材质拿不到这个权重），淡入淡出靠 Weight 缩放整体不透明度。
	if (ScreenMaterial)
	{
		PP.WeightedBlendables.Array.Add(FWeightedBlendable(Alpha, ScreenMaterial.Get()));
	}

	// 内置参数：注意 PP 每帧都是从相机基准值重新算的，所以这里不会累积，
	// 直接按 Alpha 从「当前值」往目标插一次即可。
	PP.bOverride_ColorSaturation = true;
	PP.ColorSaturation = CameraLerpVector4(PP.ColorSaturation,
		FVector4(StealthSaturation, StealthSaturation, StealthSaturation, 1.0), Alpha);

	PP.bOverride_ColorGain = true;
	PP.ColorGain = CameraLerpVector4(PP.ColorGain,
		FVector4(StealthColorGain.R, StealthColorGain.G, StealthColorGain.B, 1.0), Alpha);

	PP.bOverride_VignetteIntensity = true;
	PP.VignetteIntensity = FMath::Lerp(PP.VignetteIntensity, StealthVignette, Alpha);

	PP.bOverride_SceneFringeIntensity = true;
	PP.SceneFringeIntensity = FMath::Lerp(PP.SceneFringeIntensity, StealthSceneFringe, Alpha);
}
