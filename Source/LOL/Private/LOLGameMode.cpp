// Copyright Epic Games, Inc. All Rights Reserved.

#include "LOLGameMode.h"
#include "MyPlayerState.h"

ALOLGameMode::ALOLGameMode()
{
	// The existing ThirdPerson GameMode Blueprint inherits this default.
	PlayerStateClass = AMyPlayerState::StaticClass();
}
