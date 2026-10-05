// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/ThreeHitPassiveData.h"
#include "GAS/GE_KnockbackImpulse.h"
#include "Sound/SoundBase.h"
#include "UObject/SoftObjectPath.h"

UThreeHitPassiveData::UThreeHitPassiveData()
{
	// Tuning defaults only. Montages and DamageEffect are content references — assign them
	// on the DataAsset in the editor instead of hardcoding asset paths in C++.
	//
	// 音效是例外：它们只是「有个响」的反馈，不选也是这几个（Paragon 包里只有 Kallari 的语音，
	// 普攻那些蒙太奇里又一个 AnimNotify_PlaySound 都没配），所以给一组能直接出声的默认值，
	// 想要别的在编辑器里覆盖掉即可（软引用，默认值只是路径）。
	// 新加的属性会自动从 CDO 取这些默认值 —— 已经存在的 DS_Passive 里没有它们的序列化数据，
	// 加载时保持构造函数里的值，不用手动回填。
	// 新资产默认就带上纯冲量击退：空手四连拳这类「每一击都带位移」的连段本来就该推人，
	// 而 HitImpulse 默认 0 让它对老资产（持刀三连击）完全无感 —— 挂了 GE 也不会推。
	HitImpulseGE = UGE_KnockbackImpulse::StaticClass();

	Stages.SetNum(3);
	Stages[0].DamageMultiplier = 1.f; Stages[1].DamageMultiplier = 1.05f; Stages[2].DamageMultiplier = 1.10f;
	Stages[1].bPerfectWindowEnablesNextHitKnockback = true;
	Stages[1].ChainWindowOpenTime = .22f; Stages[1].ChainWindowCloseTime = .34f;
}
