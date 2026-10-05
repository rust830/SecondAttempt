// 斗魂竞技场 / 通用：瞬时回血的唯一载体。
//
// ===========================================================================
// 【为什么是 C++ 类而不是资产】和 GE_Damage 同一个理由：回血量是算出来的
// （全能吸血 = 造成的伤害 × 面板），不是配出来的。GE 只当载体，
// 数值走 SetByCaller（Data.Heal），由施加点填好。
//
// 【谁在用】目前唯一的施加点是 UExecCalc_Damage 的吸血结算（⑤）：
// 伤害算完顺手给攻击者回血。以后做装备主动效果、海克斯「造成伤害回血」之类，
// 都走这一个类，别再建新的回血 GE 资产 —— 回血口径（受 HealShieldPower 加成？）
// 收在一处才不会漂。
//
// 【默认不受 HealShieldPower 加成】治疗与护盾强度那条属性现在还没有消费者链路，
// 先不做乘算；真要接的时候改 ExecCalc 里填数值的那一行，这个类不用动。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_Heal.generated.h"

UCLASS()
class LOL_API UGE_Heal : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_Heal(const FObjectInitializer& ObjectInitializer);
};
