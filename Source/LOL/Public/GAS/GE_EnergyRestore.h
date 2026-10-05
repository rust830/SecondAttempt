// 瞬时回蓝的唯一载体（完美闪避的奖励）。
//
// 【为什么是 C++ 类而不是资产】和 GE_Heal 同一个理由：回多少是算出来的
//（现在就是完美闪避的一个配置值，以后可能跟着装备/等级走），GE 只当载体、
// 数值走 SetByCaller(Data.EnergyRestore)，由施加点填好。
//
// 【为什么不复用 GE_Heal】GE_Heal 那条 modifier 写死的是 Health 属性，
// 拿它回蓝等于给血条加能量。两个属性各一个类，比给 GE_Heal 加个「回哪条属性」的开关清楚。

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_EnergyRestore.generated.h"

UCLASS()
class LOL_API UGE_EnergyRestore : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_EnergyRestore(const FObjectInitializer& ObjectInitializer);
};
