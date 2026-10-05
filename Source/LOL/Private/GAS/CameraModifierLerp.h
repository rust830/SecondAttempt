// CameraModifier 共用的 FVector4 lerp 小工具。
//
// 为什么单独成头：FMath::Lerp 对 FVector4（double 精度）+ float 的组合会模板推导失败
// （标量那侧推不出同一类型），所以需要这个显式 (double)Alpha 的版本。
//
// 原来 MyStealthCameraModifier.cpp 和 MyDeathHarvestCameraModifier.cpp 各自在匿名 namespace
// 里抄了一份同名函数，unity build 把两个文件合进同一个 TU 后就 C2084 重定义。
// 这里提成 inline 函数放共享头：inline 满足 ODR（链接期不冲突）、`#pragma once` 保证
// 同一 TU 只展开一次，所以不会再撞。CONVENTIONS.md 的 unity build 一节就是这么建议的。

#pragma once

#include "CoreMinimal.h"

inline FVector4 CameraLerpVector4(const FVector4& From, const FVector4& To, float Alpha)
{
	return From + (To - From) * (double)Alpha;
}
