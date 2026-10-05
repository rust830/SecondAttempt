// 一处「只震施法者本机」的镜头振动判定，供所有 GameplayCue 复用。

#pragma once

#include "CoreMinimal.h"

class AActor;
class UCameraShakeBase;

namespace HeroCueCameraShake
{
	/**
	 * 只在【执行这个 cue 的是本机玩家】时震动手持者的摄像机。返回是否真的震了。
	 *
	 * ── 为什么判定的是 MyTarget 而不是 Parameters.Instigator ──
	 * `MyTarget` 是「执行这个 cue 的那个 ASC 的 avatar」，由引擎在服务端和每台客户端上
	 * 各自解析成同一个角色（`AbilitySystemComponent.cpp` 的 `InvokeGameplayCueEvent` 里取
	 * `AbilityActorInfo->AvatarActor`）。而多播 RPC 跑在【施法者的 ASC】上
	 * （`GameplayCueManager::FlushPendingCues` → `Call_InvokeGameplayCueExecuted_WithParams`），
	 * 所以每一台机器上 `MyTarget` 都指向施法者本人。
	 *
	 * `Parameters.Instigator` 是要跨网络传过来的 actor 引用（weak ptr），
	 * 拿它做判定不可靠 —— 所以本项目统一一律用 `MyTarget`。
	 *
	 * ── 为什么要两层判断（IsLocallyControlled + IsLocalController）──
	 * 第一层「这个 Pawn 归本机管」过滤掉远端角色（服务端上别人的角色也不归它管）；
	 * 第二层「这个 Controller 的连接是本地的」过滤掉专用服务器 / 监听服务器上的
	 * 非本地 PlayerController。两层都过才震 ⇒ 两台机器打同一个人时各抖各的。
	 *
	 * ── 为什么只走 ClientStartCameraShake ──
	 * 它不改任何模拟状态，纯表现，PvP 里没有权威问题。
	 * ⚠️ 别换成「打击顿帧」：不能用世界级 `SetGlobalTimeDilation`（普攻一秒两下，
	 * 会把所有玩家一起卡住），也不能用角色级 `CustomTimeDilation`
	 * （连段窗口 / 完美窗口都按世界时间算，动画慢放但判定不慢）。
	 *
	 * @param Shake       抖动资产；空则什么都不做（不是错误，只是没东西可播）
	 * @param Scale       强度倍率。⚠️ UE 5.8 里这个参数对 WaveOscillator / PerlinNoise
	 *                    那套 pattern 是**死的** —— 本项目的抖动资产都不读它。
	 *                    传它只是为了「哪天换个会读它的资产」时不用改调用点，
	 *                    「重的那一击抖得更狠」请靠**换资产**而不是乘倍率。
	 */
	LOL_API bool PlayLocalHitShake(AActor* MyTarget, TSubclassOf<UCameraShakeBase> Shake, float Scale = 1.f);
}
