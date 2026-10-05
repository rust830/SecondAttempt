// Fill out your copyright notice in the Description page Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "SocketSide.generated.h"

/**
 * 挂哪只手（和 BladeTrail 的 EBladeTrailSide 同义，只是这里没有"拖尾"的语义）。
 *
 * 【为什么单独拎一个头文件】最初它写在 AnimNotifyState_SocketParticle.h 里，
 * 新的 AnimNotifyState_SocketNiagara 也想用同一个枚举 —— 直接 include 那个头文件会
 * 让「Niagara 拖尾」强依赖「Cascade 粒子」这两个互不相干的类。
 * 枚举是纯值类型、动画资产里按【名字】序列化，所以把它提出来：
 *   · 老文件改成 include 本头文件，对外可见性一点没变（老 cpp / 老资产都不用改）；
 *   · 新文件只 include 本头文件，不认识 SocketParticle 的存在。
 *
 * ⚠️ 不要给枚举改名/换顺序：已经挂在 montage 上的 notify 资产存的是"名字+值"，
 * 改名会让旧动画资产读不回来。
 */
UENUM()
enum class ESocketParticleSide : uint8
{
	Left  UMETA(DisplayName = "Left (左手)"),
	Right UMETA(DisplayName = "Right (右手)"),
	Both  UMETA(DisplayName = "Both (双手)"),
};
