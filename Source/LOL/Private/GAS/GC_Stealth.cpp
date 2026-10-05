// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_Stealth.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/LOLGameplayTags.h"
#include "GAS/LocalPlayerUtils.h"
#include "GAS/MyStealthCameraModifier.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Camera/CameraModifier.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/SkeletalMesh.h"   // GetNameSafe(Mesh->GetSkeletalMeshAsset())：SkeletalMeshComponent.h 只前置声明 USkeletalMesh
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameplayEffectTypes.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "TimerManager.h"
#include "RenderUtils.h"
#include "Sound/SoundBase.h"

AGC_Stealth::AGC_Stealth()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_Stealth;

	// GE 移除时自动销毁本 cue actor，不用手写清理。
	bAutoDestroyOnRemove = true;

	// 不改角色 transform，只是就地改它的组件状态，不需要附着到 owner。
	bAutoAttachToOwner = false;

	ScreenModifierClass = UMyStealthCameraModifier::StaticClass();
}

bool AGC_Stealth::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return false;
	}

	// 记一份：cue 没有 attach 到 owner（bAutoAttachToOwner=false），后面算光环倍率时要靠它找 ASC。
	StealthTarget = MyTarget;

	ApplyMeshVisuals(MyTarget, /*bStealth=*/true);

	// 这里的粒子统一走 Cascade（UParticleSystem），包括下面那个要按隐身时长做缩放的倒计时光环
	// （Cascade 的 UParticleSystemComponent::CustomTimeDilation 就是干这个的，见 SpawnCountdown）。
	// 进入/破隐的一次性 burst 放在世界里（不设 OnlyOwnerSee），所以敌人也看得到「凭空消失/现身」的一下，
	// 和 LoL 的隐身入场反馈一致；持续期间的循环粒子才只给主人看（见下）。
	SpawnOneShotParticle(MyTarget, EnterParticle);
	UHeroAudioLibrary::PlayAt(MyTarget, LOLGameplayTags::Audio_StealthEnter, MyTarget->GetActorLocation());

	if (LoopParticle)
	{
		// 预测重放 / 连续隐身时 OnActive 可能被调第二次，先清旧的，避免挂两层。
		DestroyLoopParticle();

		// 挂到 mesh 上跟着身体走（Cascade 的 lens 类面片是朝向相机的，挂哪儿都不穿帮，挂 mesh 更贴人）。
		USceneComponent* AttachTo = MyTarget->GetRootComponent();
		if (const ACharacter* Character = Cast<ACharacter>(MyTarget))
		{
			if (Character->GetMesh())
			{
				AttachTo = Character->GetMesh();
			}
		}

		LoopParticleComp = UGameplayStatics::SpawnEmitterAttached(
			LoopParticle, AttachTo, NAME_None, FVector::ZeroVector, FRotator::ZeroRotator,
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false);

		if (LoopParticleComp)
		{
			// 本 cue 的设计是「隐身 = 敌人完全看不到」：循环粒子也只给主人渲染，
			// 否则敌人身上挂着一个常驻特效，等于自己暴露位置（想让敌人看到微光就改成 false）。
			LoopParticleComp->SetOnlyOwnerSee(true);
		}
	}

	// 刀根粒子和倒计时光环都生成在 ApplyMeshVisuals 之后，躲不掉那轮「所有 primitive 设成
	// OnlyOwnerSee」——两个 Spawn 里各自显式补一次，理由和上面的 LoopParticle 一样。
	USceneComponent* const AttachTo = ResolveAttachComponent(MyTarget);
	SpawnSwordParticles(MyTarget, AttachTo);
	SpawnUnarmedCharge(MyTarget, AttachTo);
	SpawnCountdown(AttachTo);

	// 屏幕效果只给本人：这个 cue 在每个客户端都会跑一次，但只有隐身者自己的机器该变屏幕。
	//
	// ★ 必须问「是不是本地玩家本人」，不能用 Pawn->IsLocallyControlled()：
	//   后者在 NM_Standalone 下对【所有】Controller（含 AI）都返回 true，
	//   于是单机打 AI 时「敌人隐身」会被当成「我隐身」，全屏框和潜行滤镜跑到玩家视角上。
	//   详见 GAS/LocalPlayerUtils.h。
	if (LOLLocalPlayer::IsLocalPlayerControlled(MyTarget))
	{
		ApplyLocalScreen(MyTarget, /*bOn=*/true);
	}

	return true;
}

bool AGC_Stealth::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (IsValid(MyTarget))
	{
		// 先还原网格状态，再放破隐粒子：后生成的组件不会被 ApplyMeshVisuals 设成 OnlyOwnerSee，
		// 破隐的 burst 全端可见（人已经重新露面了，没有藏的必要）。
		ApplyMeshVisuals(MyTarget, /*bStealth=*/false);
		SpawnOneShotParticle(MyTarget, ExitParticle);
		UHeroAudioLibrary::PlayAt(MyTarget, LOLGameplayTags::Audio_StealthExit, MyTarget->GetActorLocation());
	}

	DestroyLoopParticle();
	DestroySwordParticles();
	DestroyUnarmedCharge();
	DestroyCountdown();
	StealthTarget = nullptr;

	// 这里不加 IsLocallyControlled 判断，而是靠 ApplyLocalScreen 内部「加过才摘」的记账：
	// 没加过（隐身的是别人）就是 no-op，加过（隐身的是本地控制的我）就一定摘干净，
	// 两条都成立，也就换来了「破隐时屏幕一定恢复」。
	ApplyLocalScreen(MyTarget, /*bOn=*/false);

	return true;
}

void AGC_Stealth::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// 这个 tick 只为「等那份 GE 进到 ActiveGameplayEffects」而开：倍率一算出来 Tick 就被关掉
	// （见 TryScaleCountdownToStealth），不是常驻开销。
	TryScaleCountdownToStealth();
}

void AGC_Stealth::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 角色被销毁 / 切关卡时 OnRemove 不一定走到，循环粒子、刀根粒子、双手充能、光环和屏幕效果都不能留下。
	DestroyLoopParticle();
	DestroySwordParticles();
	DestroyUnarmedCharge();
	DestroyCountdown();
	ApplyLocalScreen(nullptr, /*bOn=*/false);
	Super::EndPlay(EndPlayReason);
}

void AGC_Stealth::ApplyMeshVisuals(AActor* Target, bool bStealth)
{
	// 所有可见 primitive（网格/胶囊/特效组件等）：只有主人视角渲染，其余客户端完全看不到。
	TArray<UPrimitiveComponent*> Primitives;
	Target->GetComponents<UPrimitiveComponent>(Primitives);
	for (UPrimitiveComponent* Prim : Primitives)
	{
		if (Prim)
		{
			Prim->SetOnlyOwnerSee(bStealth);
		}
	}

	// 网格组件（身体/武器）再叠一层半透明涂层，让主人看得见自己（材料可空）。
	TArray<UMeshComponent*> Meshes;
	Target->GetComponents<UMeshComponent>(Meshes);
	UMeshComponent* FirstSkeletalMesh = nullptr;
	for (UMeshComponent* Mesh : Meshes)
	{
		if (Mesh)
		{
			Mesh->SetOverlayMaterial(bStealth ? StealthOverlayMaterial.Get() : nullptr);

			if (!FirstSkeletalMesh && Mesh->IsA<USkeletalMeshComponent>())
			{
				FirstSkeletalMesh = Mesh;
			}
		}
	}

	// 涂层这一路太多「静默失效」了，进入隐身时把引擎关心的几个条件一次性打出来（只打一次）。
	if (bStealth && !bLoggedOverlayDiagnostic)
	{
		bLoggedOverlayDiagnostic = true;
		LogOverlayDiagnostic(FirstSkeletalMesh ? FirstSkeletalMesh : (Meshes.Num() > 0 ? Meshes[0] : nullptr));
	}
}

void AGC_Stealth::LogOverlayDiagnostic(UMeshComponent* Mesh)
{
	if (!StealthOverlayMaterial)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Stealth] 涂层没配：StealthOverlayMaterial 为空（要 Surface 域 + Translucent + Used with Skeletal Mesh 的材质）"));
		return;
	}

	const UMaterial* Base = StealthOverlayMaterial->GetMaterial();
	// CheckMaterialUsage_Concurrent 是非 const 成员，Get() 拿到的就是非 const 指针。
	const bool bSkinnedOK = StealthOverlayMaterial->CheckMaterialUsage_Concurrent(MATUSAGE_SkeletalMesh);
	// 混合模式是第二道门：UMeshComponent 的注释写的是「Translucent material to blend on top of this mesh」，
	// 网格会被画两遍（原材质 + 涂层）。不透明/遮罩的涂层要么把原网格整个盖掉、要么根本不混合。
	const bool bTranslucent = IsTranslucentBlendMode(*StealthOverlayMaterial);
	const UEnum* BlendEnum = StaticEnum<EBlendMode>();
	const FString BlendName = BlendEnum
		? BlendEnum->GetNameStringByValue((int64)StealthOverlayMaterial->GetBlendMode())
		: FString::FromInt((int32)StealthOverlayMaterial->GetBlendMode());

	// 着色模型是最后一道看不见的门：Substrate 下的 Thin Translucent 必须有 Front Material 节点才算「有表面」，
	// 没接的话材质画出来是空的 —— 前面所有检查全绿，屏幕上依然什么都没有。
	// 顺带一提：UMaterial::GetBlendMode() 会在正是这种材质上把混合模式报成 BLEND_TranslucentColoredTransmittance
	// （Material.cpp 的 GetBlendMode 里有这段改写），所以日志里看到这个名字基本就等同于「着色模型是 Thin Translucent」。
	const FMaterialShadingModelField ShadingModels = StealthOverlayMaterial->GetShadingModels();
	const EMaterialShadingModel FirstShadingModel = ShadingModels.CountShadingModels() > 0
		? ShadingModels.GetFirstShadingModel() : MSM_DefaultLit;
	const UEnum* ShadingModelEnum = StaticEnum<EMaterialShadingModel>();
	const FString ShadingModelName = ShadingModelEnum
		? ShadingModelEnum->GetNameStringByValue((int64)FirstShadingModel)
		: FString::FromInt((int32)FirstShadingModel);

	UE_LOG(LogTemp, Warning,
		TEXT("[Stealth] 涂层诊断: 材质=%s 域=%s(须 Surface) 混合模式=%s 半透明=%d(须 1) 着色模型=%s 骨骼网格用法=%d(须 1)"),
		*GetNameSafe(StealthOverlayMaterial.Get()),
		Base ? *MaterialDomainString(Base->MaterialDomain) : TEXT("无"),
		*BlendName, bTranslucent ? 1 : 0, *ShadingModelName, bSkinnedOK ? 1 : 0);

	if (FirstShadingModel == MSM_ThinTranslucent)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Stealth] ↑ 着色模型是 Thin Translucent（Substrate=%d）：Substrate 下这种材质要接 Front Material 节点才算有表面，没接就画不出任何东西。幽灵涂层请改成 Shading Model=Unlit + Blend Mode=Translucent（Emissive 接颜色、Opacity 接常量）"),
			Substrate::IsSubstrateEnabled() ? 1 : 0);
	}

	if (!Mesh)
	{
		return;
	}

	// 分槽涂层的优先级比全局高：SkeletalMeshSceneProxy 里取的是
	// 「PerSectionOverlayMaterial != nullptr ? 它 : 全局涂层」，所以网格/骨架资产自带分槽涂层时，
	// 我们设的全局涂层会被逐段顶掉（这种情况下得用 SetOverlayMaterial(M, /*bSetMaterialSlot=*/true, Slot)）。
	TArray<TObjectPtr<UMaterialInterface>> SlotOverlays;
	Mesh->GetMaterialSlotsOverlayMaterial(SlotOverlays);

	// SetOverlayMaterial 值相同就直接 return，所以这里读回来确认真的挂上了。
	UE_LOG(LogTemp, Warning, TEXT("[Stealth] 涂层落到 %s: 全局涂层=%s 分槽涂层=%d 个"),
		*Mesh->GetClass()->GetName(), *GetNameSafe(Mesh->GetOverlayMaterial()), SlotOverlays.Num());

	for (const TObjectPtr<UMaterialInterface>& SlotOverlay : SlotOverlays)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Stealth]   分槽涂层: %s"), *GetNameSafe(SlotOverlay.Get()));
	}
}

void AGC_Stealth::SpawnOneShotParticle(AActor* Target, UParticleSystem* Particle) const
{
	if (!Particle || !IsValid(Target))
	{
		return;
	}

	// 用 FTransform 重载（和 GC_ThrowDaggerHit 一致）；朝向用角色当前朝向，
	// lens 类粒子本来就是朝相机的，旋转不影响观感。
	UGameplayStatics::SpawnEmitterAtLocation(Target->GetWorld(), Particle,
		FTransform(Target->GetActorRotation(), Target->GetActorLocation()));
}

void AGC_Stealth::DestroyLoopParticle()
{
	if (LoopParticleComp)
	{
		LoopParticleComp->DestroyComponent();
		LoopParticleComp = nullptr;
	}
}

USceneComponent* AGC_Stealth::ResolveAttachComponent(AActor* Target) const
{
	if (!IsValid(Target))
	{
		return nullptr;
	}

	// 挂角色网格上而不是 root：粒子要跟着身体/手臂走，挂 root 会在转向时脱节。
	// （LoopParticle 里那段是同样的逻辑，没搬过来改，避免动到已经在跑的代码。）
	if (ACharacter* Character = Cast<ACharacter>(Target))
	{
		if (Character->GetMesh())
		{
			return Character->GetMesh();
		}
	}

	return Target->GetRootComponent();
}

void AGC_Stealth::SpawnSwordParticles(AActor* Target, USceneComponent* AttachTo)
{
	// 预测重放 / 连续隐身时 OnActive 可能被调第二次，先清旧的，避免挂两层（和 LoopParticle 一样）。
	DestroySwordParticles();

	if (!SwordParticle || !AttachTo)
	{
		return;
	}

	// 空手时刀已收鞘，刀根粒子必须停发——否则出现「没有刀却有刀光」的残留（本 bug 根因）。
	if (bSwordParticleRequiresArmed && IsTargetUnarmed(Target))
	{
		return;
	}

	// 左右各一份：右手插槽是镜像的，朝向要能单独补，不然同一个粒子挂上去会翻面（朝刀背而不是刀刃）。
	struct FSwordSide
	{
		FName Socket;
		FRotator Rotation;
	};
	const FSwordSide Sides[2] =
	{
		{ SwordSocketLeft,  SwordParticleRotationLeft },
		{ SwordSocketRight, SwordParticleRotationRight },
	};

	// 插槽不存在时 SpawnEmitterAttached 照样返回有效组件，只是把粒子挂在组件原点上 ——
	// 表现为「粒子从角色脚下冒出来」，看不出是名字写错了。和 BladeTrail / 完美窗口提示光一个道理。
	const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(AttachTo);
	for (const FSwordSide& Side : Sides)
	{
		if (Mesh && !(Mesh->DoesSocketExist(Side.Socket) || Mesh->GetBoneIndex(Side.Socket) != INDEX_NONE))
		{
			UE_LOG(LogTemp, Warning, TEXT("[Stealth] 刀根粒子：%s 上找不到插槽 %s → 这一侧不生成"),
				*GetNameSafe(Mesh->GetSkeletalMeshAsset()), *Side.Socket.ToString());
			continue;
		}

		// SnapToTarget 时 Rotation 落成组件的相对旋转（相对插槽），跟着刀走。
		UParticleSystemComponent* Comp = UGameplayStatics::SpawnEmitterAttached(
			SwordParticle, AttachTo, Side.Socket, FVector::ZeroVector, Side.Rotation,
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false);

		if (Comp)
		{
			// 生成在 ApplyMeshVisuals 之后，所以要自己补这一句：隐身期间刀上挂着常驻特效
			// 等于自己暴露位置（同 LoopParticle 上的理由）。
			Comp->SetOnlyOwnerSee(true);
			SwordParticleComps.Add(Comp);
		}
	}
}

bool AGC_Stealth::IsTargetUnarmed(AActor* Target) const
{
	// 判据与 GC_EmpoweredAttack::IsTargetUnarmed 一致：读 State.Form.Unarmed（GA_FormSwitch 切的 GE 授的标签）。
	// cue 在每个客户端各跑一遍，标签是复制的，两端读到的形态一致。
	const UAbilitySystemComponent* ASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
	return ASC && ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed);
}

void AGC_Stealth::DestroySwordParticles()
{
	for (TObjectPtr<UParticleSystemComponent>& Comp : SwordParticleComps)
	{
		if (Comp)
		{
			Comp->DestroyComponent();
		}
	}
	SwordParticleComps.Reset();
}

void AGC_Stealth::SpawnUnarmedCharge(AActor* Target, USceneComponent* AttachTo)
{
	// 预测重放 / 连续隐身：先清旧的再挂新的，避免手上叠两层火。
	DestroyUnarmedCharge();

	UNiagaraSystem* System = UnarmedChargeSystem.LoadSynchronous();
	if (!System || !AttachTo)
	{
		return;
	}

	// 只给空手形态挂：持刀时手上已经有刀光（SwordParticle 那条路径），
	// 两边都挂就变成「刀上有光、拳上也有光」。
	if (!IsTargetUnarmed(Target))
	{
		return;
	}

	const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(AttachTo);
	const FName Sockets[2] = { UnarmedSocketLeft, UnarmedSocketRight };

	for (const FName& Socket : Sockets)
	{
		// 插槽不存在时 SpawnSystemAttached 照样返回有效组件，只是挂在组件原点（角色脚底），
		// 表现为「火从脚下冒出来」——和刀根粒子一个道理，必须当场报出来。
		if (Mesh && !(Mesh->DoesSocketExist(Socket) || Mesh->GetBoneIndex(Socket) != INDEX_NONE))
		{
			UE_LOG(LogTemp, Warning, TEXT("[Stealth] 双手充能：%s 上找不到插槽 %s → 这一侧不生成"),
				*GetNameSafe(Mesh->GetSkeletalMeshAsset()), *Socket.ToString());
			continue;
		}

		// bAutoDestroy=false：销毁时机自己管（见 DestroyUnarmedCharge），
		// 交给 auto destroy 的话 emitter 是无限循环时永远等不到，每破一次隐泄漏一个组件。
		UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAttached(
			System, AttachTo, Socket, FVector::ZeroVector, FRotator::ZeroRotator,
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false, /*bAutoActivate=*/true,
			ENCPoolMethod::None, /*bPreCullCheck=*/false);

		if (Comp)
		{
			if (bUnarmedChargeOnlyOwnerSee)
			{
				Comp->SetOnlyOwnerSee(true);
			}
			UnarmedChargeComps.Add(Comp);
		}
	}
}

void AGC_Stealth::DestroyUnarmedCharge()
{
	for (TObjectPtr<UNiagaraComponent>& Comp : UnarmedChargeComps)
	{
		if (!Comp)
		{
			continue;
		}

		// 先停生成、让已生成的粒子自己走完寿命：直接 DestroyComponent 会把正在亮着的火苗
		// 硬切掉，破隐那一瞬间手上一闪。SetAutoDestroy 让组件在粒子收完之后自己销毁。
		Comp->Deactivate();
		Comp->SetAutoDestroy(true);

		// 兜底：NS 的 emitter 要是设成无限循环，auto destroy 永远等不到 ——
		// 那就每破一次隐泄漏一个常驻组件。延时强拆一个定时器，组件已经没了就是 no-op。
		// （不用 SetInactiveResponse：本机的 Niagara 插件源码没随引擎装，
		//   那个枚举的成员名没法核对，不想拿编译赌。）
		TWeakObjectPtr<UNiagaraComponent> WeakComp = Comp;
		if (UWorld* World = GetWorld())
		{
			FTimerHandle TimerHandle;
			World->GetTimerManager().SetTimer(TimerHandle, FTimerDelegate::CreateLambda([WeakComp]()
			{
				if (UNiagaraComponent* Alive = WeakComp.Get())
				{
					Alive->DestroyComponent();
				}
			}), UnarmedChargeTeardownDelay, /*bLoop=*/false);
		}
	}
	UnarmedChargeComps.Reset();
}

void AGC_Stealth::SpawnCountdown(USceneComponent* AttachTo)
{
	DestroyCountdown();

	UParticleSystem* Particle = CountdownParticle.LoadSynchronous();
	if (!Particle || !AttachTo)
	{
		return;
	}

	// bAutoDestroy=false：收尾由 OnRemove/EndPlay 控制（破隐时环要立刻断，不能让它自己播完）。
	// KeepRelativeOffset：CountdownRelativeOffset 是按相对挂点算的（和刀根粒子那边的 SnapToTarget 不同）。
	// 注意这里比刀根粒子那边多两个参数：SpawnEmitterAttached 的第 8 个参数是 EPSCPoolMethod，
	// 不是 bool —— 直接照抄 Niagara 那版 SpawnSystemAttached 的参数表会编译不过。
	CountdownComp = UGameplayStatics::SpawnEmitterAttached(
		Particle, AttachTo, CountdownAttachSocket, CountdownRelativeOffset, FRotator::ZeroRotator,
		EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false,
		EPSCPoolMethod::None, /*bAutoActivate=*/true);

	if (!CountdownComp)
	{
		// 模板为空 / 专用服务器 / 挂点无效时都是返回 nullptr，不报错。
		UE_LOG(LogTemp, Warning, TEXT("[Stealth] 倒计时光环：SpawnEmitterAttached 返回空（粒子无效，或挂点无效）"));
		return;
	}

	CountdownComp->SetOnlyOwnerSee(true);

	// 生成后单独设缩放（那个生成重载没有缩放参数）；Relative 缩放会连挂点自己的缩放一起乘。
	CountdownComp->SetRelativeScale3D(CountdownScale);

	// 倍率不在这里算：cue 有可能跑在那份 GE 进入 ActiveGameplayEffects 之前，这时候查不到时长。
	// 交给 Tick 每帧重试，拿到就锁住并关掉 tick。
	bCountdownScaled = false;
	SetActorTickEnabled(true);
	TryScaleCountdownToStealth();
}

void AGC_Stealth::DestroyCountdown()
{
	bCountdownScaled = false;
	SetActorTickEnabled(false);

	if (CountdownComp)
	{
		// 先停发射再销毁（DestroyComponent 本身就会让已生成的粒子立刻消失，
		// 这句只是把「停止发射」这一步明确写出来）。正常到期/破隐时环本来就走完了，主要处理提前破隐。
		CountdownComp->DeactivateSystem();
		CountdownComp->DestroyComponent();
		CountdownComp = nullptr;
	}
}

void AGC_Stealth::TryScaleCountdownToStealth()
{
	if (bCountdownScaled)
	{
		return;
	}

	if (!CountdownComp)
	{
		// 没生成出环来（属性没配 / SpawnEmitterAttached 返回了空）：没什么可算的，别让 tick 空转。
		SetActorTickEnabled(false);
		return;
	}

	if (CountdownParticleDuration <= 0.f)
	{
		// 标准时长为 0 = 配置成「不缩放」：不用等 GE，直接锁掉。
		bCountdownScaled = true;
		SetActorTickEnabled(false);
		return;
	}

	UAbilitySystemComponent* ASC = StealthTarget.IsValid()
		? UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(StealthTarget.Get())
		: nullptr;
	if (!ASC)
	{
		return;
	}

	// 必须用 Owning 版查询：State.Stealth 是 UTargetTagsGameplayEffectComponent 授的 Granted Tag，
	// MatchAnyEffectTags 只比对 asset tags，匹配不上 → 永远返回空、环一直按原速播。
	// （和 RemoveGrantedTagEffects / RemoveActiveEffectsWithTags 那个坑是同一个。）
	// 返回的 TPair 是 <剩余, 总时长>（GameplayEffect.cpp 的 GetActiveEffectsTimeRemainingAndDuration）。
	const TArray<TPair<float, float>> TimeAndDuration = ASC->GetActiveEffectsTimeRemainingAndDuration(
		FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(LOLGameplayTags::State_Stealth)));

	if (TimeAndDuration.Num() == 0)
	{
		// GE 还没进 ActiveGameplayEffects：下一帧再来。
		return;
	}

	const float Duration = TimeAndDuration[0].Value;

	bCountdownScaled = true;
	SetActorTickEnabled(false);

	// Duration <= 0 = 无限时长（SetByCaller Data_StealthDuration 没填上就会这样）：没有比例可算。
	if (Duration <= 0.f)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Stealth] 倒计时光环：State.Stealth 的时长是无限的（Data_StealthDuration 没填上？）→ 不缩放，环按原速播"));
		return;
	}

	// 把粒子系统的时间轴整体压/拉到隐身时长：Cascade 原速要 CountdownParticleDuration 秒，
	// 隐身只有 Duration 秒，倍率就是两者之比 —— 环正好在破隐/到期那一刻走完。和完美窗口提示光同一套算法。
	// 直接写成员：CustomTimeDilation 是 UParticleSystemComponent 的公开属性，
	// TickComponent 里 DeltaTime *= CustomTimeDilation，组件自己 tick 和世界粒子管理器批处理两条路径都认。
	const float Dilation = CountdownParticleDuration / Duration;
	CountdownComp->CustomTimeDilation = Dilation;

	// 诊断：倍率是算出来的，环没在破隐那一刻走完就看这一行 —— 是 Cascade 标准时长填错了还是隐身时长不对。
	UE_LOG(LogTemp, Warning, TEXT("[Stealth] 倒计时光环：%s 隐身=%.2fs Cascade标准时长=%.2fs → 播放倍率=%.3f"),
		*GetNameSafe(CountdownComp->GetFXSystemAsset()), Duration, CountdownParticleDuration, Dilation);
}

void AGC_Stealth::ConfigureScreenModifier(UMyStealthCameraModifier* Modifier)
{
	if (!Modifier)
	{
		// ScreenModifierClass 被换成了非 UMyStealthCameraModifier 的类：cue 上的东西推不过去，全靠那个类自己。
		UE_LOG(LogTemp, Warning, TEXT("[Stealth] 相机修改器不是 UMyStealthCameraModifier 的子类，cue 上的屏幕材质不会生效"));
		return;
	}

	// 蓝图子类里配过的材质优先，cue 上填的只当回退：于是「建了 BP_StealthCameraModifier 就在子类里调，
	// 懒得建就直接在 cue 上填」两条路都成立，不会互相覆盖。
	if (!Modifier->ScreenMaterial && ScreenMaterial)
	{
		Modifier->ScreenMaterial = ScreenMaterial;
	}

	// 全屏 blendable 必须是后处理域材质：网格域/贴花域的材料挂上去不报错，只是屏幕上什么都不出现。
	if (Modifier->ScreenMaterial)
	{
		const UMaterial* Base = Modifier->ScreenMaterial->GetMaterial();
		if (Base && Base->MaterialDomain != MD_PostProcess)
		{
			UE_LOG(LogTemp, Error,
				TEXT("[Stealth] 屏幕材质 %s 的域是 %s，不是 Post Process：当全屏 blendable 不会显示任何东西，换成后处理材质（如 M_StealthScreen）"),
				*GetNameSafe(Base), *MaterialDomainString(Base->MaterialDomain));
		}
	}

	// 把最终生效的一整套参数打出来：改完位置不对时一眼能看出「引擎到底读到了谁的值」。
	UE_LOG(LogTemp, Warning,
		TEXT("[Stealth] 屏幕配置: 修改器=%s 材质=%s 饱和=%.2f 色偏=(%.2f,%.2f,%.2f) 暗角=%.2f 色散=%.2f 淡入=%.2fs 淡出=%.2fs"),
		*Modifier->GetClass()->GetName(), *GetNameSafe(Modifier->ScreenMaterial),
		Modifier->StealthSaturation,
		Modifier->StealthColorGain.R, Modifier->StealthColorGain.G, Modifier->StealthColorGain.B,
		Modifier->StealthVignette, Modifier->StealthSceneFringe,
		Modifier->GetAlphaInTime(), Modifier->GetAlphaOutTime());
}

void AGC_Stealth::ApplyLocalScreen(AActor* Target, bool bOn)
{
	if (!ScreenModifierClass)
	{
		return;
	}

	if (!bOn)
	{
		// 只摘自己加的那一个。没加过就立刻走，绝不能顺着下面的兜底去解析相机：
		// 同一台机器上并存着别的角色（敌人）的隐身 cue 实例，它结束时 MyTarget 是别人，
		// 别人在这台机器上 GetController() 是空的 → 兜底 GetPlayerController(0) = 【我】的相机
		// → 按类找到的其实是【我】加的修改器，被它摘掉。
		// 表现就是联机下「主机的隐身结束，我的屏幕效果跟着没了，隐身本身还在」。
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
	// 拿不到再退回本地 0 号玩家（EndPlay 兜底时 MyTarget 可能已失效）。
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

	// 防叠加：上一次淡出还没摘干净（连续隐身 / 预测重放）时先立刻移除同类修改器，
	// 否则两个修改器同时生效，边缘框和滤镜会翻倍。
	if (UCameraModifier* Existing = CamMgr->FindCameraModifierByClass(ScreenModifierClass))
	{
		CamMgr->RemoveCameraModifier(Existing);
	}

	// 修改器上的 ScreenMaterial / 滤镜参数都是 EditDefaultsOnly：只有蓝图子类的默认值改得动，
	// 直接 new 出来的原生实例读的是 C++ 默认值。所以要调效果就建 BP_StealthCameraModifier
	// 指到 ScreenModifierClass；不想建子类的话，cue 上的 ScreenMaterial 会在这里兜底推过去。
	ConfigureScreenModifier(Cast<UMyStealthCameraModifier>(CamMgr->AddNewCameraModifier(ScreenModifierClass)));

	// 记下来：只有加过的这份实例才有资格摘（理由见头文件里 bAppliedLocalScreen 的说明）。
	ScreenCameraManager = CamMgr;
	bAppliedLocalScreen = true;
}
