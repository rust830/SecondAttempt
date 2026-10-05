#include "GAS/CueCameraShake.h"

#include "Camera/CameraShakeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

namespace HeroCueCameraShake
{
	bool PlayLocalHitShake(AActor* MyTarget, TSubclassOf<UCameraShakeBase> Shake, float Scale)
	{
		if (!Shake)
		{
			return false;
		}

		const APawn* Instigator = Cast<APawn>(MyTarget);
		if (!Instigator || !Instigator->IsLocallyControlled())
		{
			// 远端角色（以及服务端上别人的角色）：不震。别人屏幕不该因为我打人而抖。
			return false;
		}

		APlayerController* PC = Cast<APlayerController>(Instigator->GetController());
		if (!PC || !PC->IsLocalController())
		{
			// 专用服务器 / 非本地连接：同上。
			return false;
		}

		PC->ClientStartCameraShake(Shake, Scale);
		return true;
	}
}
