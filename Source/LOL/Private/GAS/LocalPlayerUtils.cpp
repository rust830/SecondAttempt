// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/LocalPlayerUtils.h"

#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"

namespace LOLLocalPlayer
{
	bool IsLocalPlayerControlled(const AActor* Target)
	{
		const APawn* Pawn = Cast<APawn>(Target);
		if (!Pawn)
		{
			return false;
		}

		// IsLocalPlayerController 是 AController 上的 inline 函数（Controller.h:420-423），
		// 不用先 Cast 成 APlayerController；对 Bot 它会因为 bIsPlayerController=false 直接返回 false。
		const AController* Controller = Pawn->GetController();
		return Controller && Controller->IsLocalPlayerController();
	}
}
