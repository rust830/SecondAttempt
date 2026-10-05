// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"

class AActor;

/**
 * 判断「某个 Pawn 是不是【本地玩家本人】在控制」。
 *
 * 用途：把只该出现在本人屏幕上的表现（全屏后处理、相机修改器、只给自己的轮廓/音效）
 * 限制在真正该看到它的那台机器上。
 *
 * ===========================================================================
 * ★ 为什么不能直接用 Pawn->IsLocallyControlled()
 * ===========================================================================
 * APawn::IsLocallyControlled() 只是转发给 AController::IsLocalController()
 * （Pawn.cpp:272-275：`return ( GetController() && GetController()->IsLocalController() );`），
 * 而后者在 NM_Standalone 下【无条件返回 true】：
 *
 *     // Controller.cpp:90-98
 *     bool AController::IsLocalController() const
 *     {
 *         const ENetMode NetMode = GetNetMode();
 *         if (NetMode == NM_Standalone)
 *         {
 *             // Not networked.
 *             return true;      // ← 单机下【所有】Controller 都走这一支
 *         }
 *         ...
 *     }
 *
 * 它回答的问题是「这台机器上是谁在驱动这个 pawn」——单机的 AI 当然也算「本机在驱动」，
 * 所以对 Bot（AArenaBotController，继承链是 AAIController → AController，
 * 见 AIController.h:87 —— 注意 AAIController【不是】APlayerController 的子类）
 * 它也返回 true。
 *
 * 后果：GameplayCue 在每个客户端各跑一遍，Bot 那台的判断也过 →
 * 「敌人隐身」被当成「我隐身」，屏幕后处理/相机修改器挂到玩家自己的相机上。
 * 表现就是【单机打 AI 时，只该给本人的效果全部公用】。
 * 这个 bug 在联机下不复现（NM_Client 下分支不同），所以很容易被当成偶发。
 *
 * ===========================================================================
 * ★ 用它问的是另一个问题：「这个 pawn 是不是本地玩家的？」
 * ===========================================================================
 * AController::IsLocalPlayerController() = IsPlayerController() && IsLocalController()
 * （Controller.h:420-423），其中：
 *   - bIsPlayerController 只有 APlayerController 的构造函数置 true（PlayerController.cpp:236）；
 *   - AController 的构造函数置 false（Controller.cpp:74）；
 *   - AAIController 全类不碰它 → Bot 恒为 false。
 *
 * 各端结果（都正确）：
 *   单机里我自己的角色 → true      单机里 Bot 的角色 → false
 *   主机上我自己的角色 → true      主机上远端客户端的角色 → false
 *   客户端上我自己的角色 → true    客户端上别人的角色 / 专用服务器 → false
 *
 * ===========================================================================
 * ⚠ 反过来也要注意：有些判断问的其实是另一个问题
 * ===========================================================================
 * 如果代码想问的是「这台机器上谁在驱动这个 pawn 的输入」，那【就该】继续用
 * IsLocallyControlled()。典型反例是 UGA_Dodge 里挂 State.Dodge.Active 那处：
 * Bot 走的是和真人完全一样的 BasicAttackPressed，靠这个本地标签把左键改判成「踢」——
 * 改成 IsLocalPlayerControlled 会让 Bot 不会派生踢。所以那处【不要】动。
 *
 * 一句话：问「谁该看见」用这个；问「谁在输入」用 IsLocallyControlled()。
 */
namespace LOLLocalPlayer
{
	/** Target 是不是【本地玩家本人】控制的 Pawn（非 Pawn / 无 Controller / Bot → false）。 */
	LOL_API bool IsLocalPlayerControlled(const AActor* Target);
}
