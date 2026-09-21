// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestCast.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/MyDeathHarvestCameraModifier.h"
#include "Camera/CameraModifier.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"

AGC_DeathHarvestCast::AGC_DeathHarvestCast()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Cast;

	// 技能结束（正常或被取消）→ 服务器摘 cue → 这个 actor 自己销毁，不用手写清理。
	// 这条尤其关键：消失那条 cue 摘不干净 = 人一直隐形（还可能留着无碰撞和镜头）。
	bAutoDestroyOnRemove = true;

	// 这条 cue 只就地改角色的组件状态，不改自己的 transform，不需要附着到 owner。
	bAutoAttachToOwner = false;

	ScreenModifierClass = UMyDeathHarvestCameraModifier::StaticClass();
}

bool AGC_DeathHarvestCast::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return false;
	}

	VanishTarget = MyTarget;

	// ① 藏人 + 关碰撞。粒子放在后面生成（世界坐标、不挂在人身上），所以不受这一轮影响。
	ApplyVanishVisuals(MyTarget, /*bVanished=*/true);

	// ② 消失那一下的 burst + 音效。敌人也要看得见「她在这里消失了」，所以是世界坐标的独立发射器。
	if (VanishParticle)
	{
		UGameplayStatics::SpawnEmitterAtLocation(MyTarget->GetWorld(), VanishParticle,
			FTransform(MyTarget->GetActorRotation(), MyTarget->GetActorLocation()));
	}
	if (VanishSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, VanishSound, MyTarget->GetActorLocation());
	}

	// ③ 传送镜头只给主人：这条 cue 每台机器都会跑一次，只有施法者自己那一台该变屏幕。
	if (const APawn* Pawn = Cast<APawn>(MyTarget))
	{
		if (Pawn->IsLocallyControlled())
		{
			ApplyLocalScreen(MyTarget, /*bOn=*/true);
		}
	}

	return true;
}

bool AGC_DeathHarvestCast::WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	// 服务器那一端走的是这条（运行时 Add 的 cue）。转给 OnActive，理由见头文件。
	return OnActive_Implementation(MyTarget, Parameters);
}

bool AGC_DeathHarvestCast::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (IsValid(MyTarget))
	{
		// ① 先把人还回来，再放现身 burst：后生成的粒子不会被这一轮 Visibility 波及。
		ApplyVanishVisuals(MyTarget, /*bVanished=*/false);

		// ② 现身 burst 放的是角色的【当前】位置 —— GA 是先把人传送过去、再摘这条 cue 的。
		if (AppearParticle)
		{
			UGameplayStatics::SpawnEmitterAtLocation(MyTarget->GetWorld(), AppearParticle,
				FTransform(MyTarget->GetActorRotation(), MyTarget->GetActorLocation()));
		}
		if (AppearSound)
		{
			UGameplayStatics::PlaySoundAtLocation(this, AppearSound, MyTarget->GetActorLocation());
		}
	}

	VanishTarget = nullptr;

	// 这里不加 IsLocallyControlled 判断，而是靠 ApplyLocalScreen 内部「加过才摘」的记账：
	// 没加过（消失的是别人）就是 no-op，加过（消失的是本地控制的我）就一定摘干净。
	ApplyLocalScreen(MyTarget, /*bOn=*/false);

	return true;
}

void AGC_DeathHarvestCast::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 兜底：角色被销毁 / 切关卡时 OnRemove 不一定走到，不能把人留在隐形 + 无碰撞的状态里。
	if (AActor* Target = VanishTarget.Get())
	{
		ApplyVanishVisuals(Target, /*bVanished=*/false);
	}
	VanishTarget = nullptr;

	ApplyLocalScreen(nullptr, /*bOn=*/false);

	Super::EndPlay(EndPlayReason);
}

void AGC_DeathHarvestCast::ApplyVanishVisuals(AActor* Target, bool bVanished)
{
	if (!IsValid(Target))
	{
		return;
	}

	if (!bVanished)
	{
		// 只还原自己藏起来的那一批：原本就是隐藏的组件（别人出于别的理由关掉的）别顺手打开。
		for (const TWeakObjectPtr<UPrimitiveComponent>& Prim : HiddenPrimitives)
		{
			if (UPrimitiveComponent* Comp = Prim.Get())
			{
				Comp->SetVisibility(true);
			}
		}
		HiddenPrimitives.Reset();

		if (bCollisionDisabled)
		{
			bCollisionDisabled = false;
			// 只在「本来是开的」时才开回去：死了的人胶囊已经被 EnterDeathState 关掉了，
			// 这里无脑 SetActorEnableCollision(true) 会把尸体变回一根挡路的柱子。
			if (bSavedActorCollision)
			{
				Target->SetActorEnableCollision(true);
			}
		}
		return;
	}

	// 藏之前先清旧的（cue actor 会被回收复用，OnActive 可能不是第一次）。
	for (const TWeakObjectPtr<UPrimitiveComponent>& Prim : HiddenPrimitives)
	{
		if (UPrimitiveComponent* Comp = Prim.Get())
		{
			Comp->SetVisibility(true);
		}
	}
	HiddenPrimitives.Reset();

	// 【真的看不见】：不是 bOnlyOwnerSee（那只挡别人、自己照样看得见），是本地这台机器上直接不渲染。
	// 只记下「当时是可见的」那些，还原时才有据可依。
	TArray<UPrimitiveComponent*> Primitives;
	Target->GetComponents<UPrimitiveComponent>(Primitives);
	for (UPrimitiveComponent* Prim : Primitives)
	{
		if (Prim && Prim->IsVisible())
		{
			Prim->SetVisibility(false);
			HiddenPrimitives.Add(Prim);
		}
	}

	// 关碰撞：人虽然看不见了，身体还在原地挡路 / 还能被扫到。
	// 纯本地行为，但每台机器都会各自跑一遍这条 cue，所以各端表现一致。
	if (!bCollisionDisabled)
	{
		bSavedActorCollision = Target->GetActorEnableCollision();
		bCollisionDisabled = true;
	}
	Target->SetActorEnableCollision(false);

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] %s 进入消失：藏了 %d 个 primitive，碰撞关闭（原状态=%d）"),
		*GetNameSafe(Target), HiddenPrimitives.Num(), bSavedActorCollision ? 1 : 0);
}

void AGC_DeathHarvestCast::ApplyLocalScreen(AActor* Target, bool bOn)
{
	if (!ScreenModifierClass)
	{
		return;
	}

	if (!bOn)
	{
		// 只摘自己加的那一个。没加过就立刻走，绝不能顺着下面的兜底去解析相机：
		// 同一台机器上并存着别的角色的大招 cue 实例，它结束时 MyTarget 是别人，
		// 别人在这台机器上 GetController() 是空的 → 兜底 GetPlayerController(0) = 【我】的相机
		// → 按类找到的其实是【我】加的修改器，被它摘掉。
		if (!bAppliedLocalScreen)
		{
			return;
		}
		bAppliedLocalScreen = false;

		// 优先还给当初加的那个管理器：EndPlay 那条路径上 Target 已经空了，解析不出 PC。
		APlayerCameraManager* CamMgr = ScreenCameraManager.Get();
		ScreenCameraManager.Reset();

		if (!CamMgr)
		{
			if (const APawn* Pawn = Cast<APawn>(Target))
			{
				if (const APlayerController* PC = Cast<APlayerController>(Pawn->GetController()))
				{
					CamMgr = PC->PlayerCameraManager;
				}
			}
		}

		// 按 AlphaOutTime 淡出，淡完引擎自动把修改器从相机管理器摘掉，不用我们再管。
		if (CamMgr)
		{
			if (UCameraModifier* Existing = CamMgr->FindCameraModifierByClass(ScreenModifierClass))
			{
				Existing->DisableModifier(/*bImmediate=*/false);
			}
		}

		return;
	}

	// 优先用角色自己的 PlayerController（联机/分屏下比索引 0 正确）；
	// 拿不到再退回本地 0 号玩家（兜底路径上 MyTarget 可能已失效）。
	APlayerController* PC = nullptr;
	if (const APawn* Pawn = Cast<APawn>(Target))
	{
		PC = Cast<APlayerController>(Pawn->GetController());
	}
	if (!PC)
	{
		PC = UGameplayStatics::GetPlayerController(this, 0);
	}

	APlayerCameraManager* CamMgr = PC ? PC->PlayerCameraManager : nullptr;
	if (!CamMgr)
	{
		return;
	}

	// 防叠加：上一次淡出还没摘干净（连续放大招 / 预测重放）时先立刻移除同类修改器，
	// 否则两个修改器同时生效，暗角和褪色会翻倍。
	if (UCameraModifier* Existing = CamMgr->FindCameraModifierByClass(ScreenModifierClass))
	{
		CamMgr->RemoveCameraModifier(Existing);
	}

	// 修改器上的 ScreenMaterial / 参数都是 EditDefaultsOnly：只有蓝图子类的默认值改得动，
	// 直接 new 出来的原生实例读的是 C++ 默认值。所以要调效果就建 BP_DeathHarvestCameraModifier
	// 指到 ScreenModifierClass；不想建子类的话，cue 上的 ScreenMaterial 会在这里兜底推过去。
	ConfigureScreenModifier(Cast<UMyDeathHarvestCameraModifier>(CamMgr->AddNewCameraModifier(ScreenModifierClass)));

	// 记下来：只有加过的这份实例才有资格摘（理由见头文件里 bAppliedLocalScreen 的说明）。
	ScreenCameraManager = CamMgr;
	bAppliedLocalScreen = true;
}

void AGC_DeathHarvestCast::ConfigureScreenModifier(UMyDeathHarvestCameraModifier* Modifier)
{
	if (!Modifier)
	{
		// ScreenModifierClass 被换成了非 UMyDeathHarvestCameraModifier 的类：cue 上的东西推不过去。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 相机修改器不是 UMyDeathHarvestCameraModifier 的子类，cue 上的屏幕材质不会生效"));
		return;
	}

	// 蓝图子类里配过的材质优先，cue 上填的只当回退。
	if (!Modifier->ScreenMaterial && ScreenMaterial)
	{
		Modifier->ScreenMaterial = ScreenMaterial;
	}

	// 把最终生效的一整套参数打出来：改完位置不对时一眼能看出「引擎到底读到了谁的值」。
	UE_LOG(LogTemp, Warning,
		TEXT("[DeathHarvest] 传送镜头配置: 修改器=%s 材质=%s 暗角=%.2f 饱和度=%.2f 淡入=%.2fs 淡出=%.2fs"),
		*Modifier->GetClass()->GetName(), *GetNameSafe(Modifier->ScreenMaterial),
		Modifier->VoidVignette, Modifier->VoidSaturation,
		Modifier->GetAlphaInTime(), Modifier->GetAlphaOutTime());
}
