// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/MyDeathHarvestCameraModifier.h"
#include "Engine/Scene.h"
#include "Materials/MaterialInterface.h"

namespace
{
	// FMath::Lerp 对 FVector4(双精度) + float 的组合会模板推导失败（标量那侧推不出同一类型），
	// 和 MyStealthCameraModifier.cpp 里那个同名小工具是同一件事。
	FVector4 LerpVector4(const FVector4& From, const FVector4& To, float Alpha)
	{
		return From + (To - From) * (double)Alpha;
	}
}

UMyDeathHarvestCameraModifier::UMyDeathHarvestCameraModifier()
{
	// 消失淡入、现身淡出。淡出比淡入长一点：现身那一下人已经回到场上，屏幕别切得太硬。
	AlphaInTime = 0.25f;
	AlphaOutTime = 0.4f;
	// Priority 保持基类默认（127），不跟别的相机修改器抢顺序。
}

void UMyDeathHarvestCameraModifier::ModifyPostProcess(float DeltaTime, float& PostProcessBlendWeight, FPostProcessSettings& PP)
{
	// Alpha 是基类插值好的淡入淡出进度；淡到 0 就什么都不叠，让 PP 保持相机原样。
	if (Alpha <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// 这个权重是「整份 PP 设置要不要被采纳」的开关，不是强度：
	// UCameraModifier::ModifyCamera 里写着 `if (PPBlendWeight > 0.f) { CameraOwner->AddCachedPPBlend(...) }`，
	// 权重留在 0 → 下面设的所有 bOverride_xxx 连同 WeightedBlendables 会被整份丢掉，屏幕一点变化都没有。
	// （UMyStealthCameraModifier 踩过这个坑：它当初只在材质非空的分支里赋值，而材质没人配 → 内置参数全失效。）
	PostProcessBlendWeight = 1.f;

	if (!bLoggedFirstApply)
	{
		bLoggedFirstApply = true;
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 传送镜头已生效: Alpha=%.2f 材质=%s 淡入=%.2fs 淡出=%.2fs"),
			Alpha, *GetNameSafe(ScreenMaterial), AlphaInTime, AlphaOutTime);
	}

	// 全屏材质：按 Alpha 加权塞进 blendable 列表，混合交给引擎。
	// 材质里不要让权重参与运算（材质拿不到这个权重），淡入淡出靠 Weight 缩放整体不透明度。
	if (ScreenMaterial)
	{
		PP.WeightedBlendables.Array.Add(FWeightedBlendable(Alpha, ScreenMaterial.Get()));
	}

	// 内置参数：PP 每帧都是从相机基准值重新算的，所以这里不会累积，
	// 直接按 Alpha 从「当前值」往目标插一次即可。
	PP.bOverride_ColorSaturation = true;
	PP.ColorSaturation = LerpVector4(PP.ColorSaturation,
		FVector4(VoidSaturation, VoidSaturation, VoidSaturation, 1.0), Alpha);

	PP.bOverride_VignetteIntensity = true;
	PP.VignetteIntensity = FMath::Lerp(PP.VignetteIntensity, VoidVignette, Alpha);
}
