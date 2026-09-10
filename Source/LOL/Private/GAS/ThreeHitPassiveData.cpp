// Copyright Epic Games, Inc. All Rights Reserved.
#include "GAS/ThreeHitPassiveData.h"

UThreeHitPassiveData::UThreeHitPassiveData()
{
	// Tuning defaults only. Montages and DamageEffect are content references — assign them
	// on the DataAsset in the editor instead of hardcoding asset paths in C++.
	Stages.SetNum(3);
	Stages[0].DamageMultiplier = 1.f; Stages[1].DamageMultiplier = 1.05f; Stages[2].DamageMultiplier = 1.10f;
	Stages[1].bPerfectWindowEnablesNextHitKnockback = true;
	Stages[1].ChainWindowOpenTime = .22f; Stages[1].ChainWindowCloseTime = .34f;
}
