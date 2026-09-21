// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/ThreeHitPassiveData.h"
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
	PerfectWindowSuccessSound = TSoftObjectPtr<USoundBase>(
		FSoftObjectPath(TEXT("/Game/ParagonKallari/Audio/Cues/Kallari_Ability_ScoredCrit.Kallari_Ability_ScoredCrit")));
	AttackSound = TSoftObjectPtr<USoundBase>(
		FSoftObjectPath(TEXT("/Game/ParagonKallari/Audio/Cues/Kallari_Effort_Swing.Kallari_Effort_Swing")));
	EmpoweredAttackSound = TSoftObjectPtr<USoundBase>(
		FSoftObjectPath(TEXT("/Game/ParagonKallari/Audio/Cues/Kallari_Effort_Ability_Primary_Strike.Kallari_Effort_Ability_Primary_Strike")));

	Stages.SetNum(3);
	Stages[0].DamageMultiplier = 1.f; Stages[1].DamageMultiplier = 1.05f; Stages[2].DamageMultiplier = 1.10f;
	Stages[1].bPerfectWindowEnablesNextHitKnockback = true;
	Stages[1].ChainWindowOpenTime = .22f; Stages[1].ChainWindowCloseTime = .34f;
}
