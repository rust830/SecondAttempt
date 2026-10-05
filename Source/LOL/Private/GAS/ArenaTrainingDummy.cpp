// 斗魂竞技场：备战区的训练木桩的实现。设计说明见头文件。

#include "GAS/ArenaTrainingDummy.h"

#include "AbilitySystemComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/ConstructorHelpers.h"

#include "GAS/HeroCombatAttributeSet.h"
#include "GAS/MyAbilitySystemComponent.h"

AArenaTrainingDummy::AArenaTrainingDummy()
{
	// 它什么都不做：不移动、不放技能、也不需要每帧算什么。关掉 tick 省一遍空跑。
	//（骨骼网格组件的动画 tick 是组件自己的事，和这一行无关，动画照常播。）
	PrimaryActorTick.bCanEverTick = false;

	// 【为什么必须显式关掉 AI 接管】ACharacter 默认的 AIControllerClass 是 AAIController，
	// 而 Pawn 的 AutoPossessAI 默认会在"摆进关卡 / 被生成"时去造一个控制器来接管自己。
	// 那个控制器会给木桩挂上一个 PlayerState —— 而本项目的参赛者表就是按 PlayerState
	// 认人的，多出来一个身份不明的身份正是最容易出怪事的东西。
	// 木桩本来也不需要控制器（它不动、不放技能），所以这里把它钉死。
	AIControllerClass = nullptr;
	AutoPossessAI = EAutoPossessAI::Disabled;

	// ASC 与属性集都挂在这个 Actor 自己身上 —— 引擎的 ASC 初始化会按"Owner 就是 Avatar"
	// 这条路自动完成 InitAbilityActorInfo 和属性集发现，所以这里不调任何初始化。
	// 赋值给基类那个受保护成员是【必须】的：基类的 IsDead / PlayHitReact /
	// GetAbilitySystemComponent 读的都是它，而不是本类这一个。理由见头文件。
	OwnAbilitySystemComponent = CreateDefaultSubobject<UMyAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	OwnAbilitySystemComponent->SetIsReplicated(true);
	OwnAbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
	AbilitySystemComponent = OwnAbilitySystemComponent;

	CombatAttributes = CreateDefaultSubobject<UHeroCombatAttributeSet>(TEXT("CombatAttributes"));

	// -----------------------------------------------------------------------
	// 移动：它不会自己走，但必须"能被推动"。
	//
	// 【为什么不能是 MOVE_None】击退那条链最后落在这句上（见 UGEComponent_Knockback）：
	//   Character->LaunchCharacter(方向 * 水平冲量 + 上 * 竖直冲量, true, true)
	// LaunchCharacter 把冲量存进 PendingLaunchVelocity，由移动组件下一帧取用 ——
	// 移动组件不跑，冲量就是丢进垃圾桶，表现是"打上去人纹丝不动"。
	//
	// 【为什么必须打开 bRunPhysicsWithNoController】这是最容易漏的一条，而且是静默的：
	// 引擎在 PhysWalking / PhysFalling 的入口有这么一句（CharacterMovementComponent.cpp
	// 的 5662 / 5901 行）——
	//   if (!CharacterOwner || (!Controller && !bRunPhysicsWithNoController && ...))
	//   { Acceleration = 0; Velocity = 0; return; }
	// 也就是【没有控制器时，速度和加速度会被直接清零、连重力都不跑】。木桩刻意不带控制器
	// （理由见构造函数的上面一段），所以这个开关是必须的，不是可选优化。
	// 打开之后引擎还会顺手把初始移动模式设成 DefaultLandMovementMode（=Walking），
	// 所以这里不用再显式 SetMovementMode。
	//
	// 【为什么关 bOrientRotationToMovement】它管的是"朝向跟着速度转"。木桩该朝着出生时的
	// 方向站着，被推一下不能自己转身 —— 一转身，"正对你的靶子"就没了。
	// -----------------------------------------------------------------------
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->bRunPhysicsWithNoController = true;
		Movement->bOrientRotationToMovement = false;
	}

	// -----------------------------------------------------------------------
	// 外观：和玩家角色同一套。
	//
	// 【为什么在 C++ 里写死路径】木桩是"拿来试手感"的东西，它的价值就在于长得和
	// 对手一样 —— 打上去的高度、身位、受击动画全都对得上。这三个资产就是
	// BP_ThirdPersonCharacter 上那一套，写在这里等于"木桩默认就是玩家那个样子"，
	// 不需要谁记得去建一个蓝图再配一遍。
	// 想换一套（换英雄 / 换皮肤）就做本类的蓝图子类覆写这几个属性，再把
	// BP_ArenaGameMode 的 TrainingDummyClass 指过去 —— 那是这条路留着的原因。
	//
	// 【那两个相对变换不是拍的】Paragon 的模型朝向是 -Y、脚底不在网格原点，
	// 所以要转 -90 度、下沉 89 厘米才对得上胶囊体。数值抄自 BP_ThirdPersonCharacter
	// 的 Mesh 组件，别自己算 —— 算错了表现是"模型侧着站 / 陷进地里"。
	// -----------------------------------------------------------------------
	if (USkeletalMeshComponent* MeshComp = GetMesh())
	{
		static ConstructorHelpers::FObjectFinder<USkeletalMesh> DummyMeshFinder(
			TEXT("/Game/ParagonKallari/Characters/Heroes/Kallari/Skins/DeathLotus/Meshes/Kallari_DeathLotus.Kallari_DeathLotus"));
		if (DummyMeshFinder.Succeeded())
		{
			MeshComp->SetSkeletalMesh(DummyMeshFinder.Object);
		}

		static ConstructorHelpers::FClassFinder<UAnimInstance> DummyAnimFinder(
			TEXT("/Game/ParagonKallari/Characters/Heroes/Kallari/Kallari_AnimBlueprint.Kallari_AnimBlueprint_C"));
		if (DummyAnimFinder.Succeeded())
		{
			MeshComp->SetAnimInstanceClass(DummyAnimFinder.Class);
		}

		MeshComp->SetRelativeLocation(FVector(0.f, 0.f, -89.f));
		MeshComp->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	}

	// -----------------------------------------------------------------------
	// 受击蒙太奇：同样抄玩家那一套。
	//
	// 【为什么必须自己配一份】蒙太奇是 EditDefaultsOnly 的属性，玩家那几份配在
	// BP_ThirdPersonCharacter 的 CDO 上、跟着那个蓝图走，木桩拿不到。不配的话
	// ResolveHitReactMontage 返回空、PlayHitReact 静默直接返回 —— 又回到"打上去
	// 只有数字在掉"的样子。四个方向都要给全：该方向没配是不退回别的方向的
	//（拿正面动画去演背面挨打，比不播更别扭），漏一个方向就是那个方向永远没反应。
	//
	// 【冷却抄 0.2 而不是用基类默认的 0.4】目的就是"像打真人"—— 这个值决定了
	// 连段打上去时受击动画的密集程度，两边不一样的话木桩的手感会和真人差一截。
	// -----------------------------------------------------------------------
	static ConstructorHelpers::FObjectFinder<UAnimMontage> HitFrontFinder(
		TEXT("/Game/LOL/Animation/Kallari/AM_HitReact_Front.AM_HitReact_Front"));
	static ConstructorHelpers::FObjectFinder<UAnimMontage> HitBackFinder(
		TEXT("/Game/ParagonKallari/Characters/Heroes/Kallari/Animations/AM_Back.AM_Back"));
	static ConstructorHelpers::FObjectFinder<UAnimMontage> HitLeftFinder(
		TEXT("/Game/LOL/Animation/Kallari/AM_HitReact_Left.AM_HitReact_Left"));
	static ConstructorHelpers::FObjectFinder<UAnimMontage> HitRightFinder(
		TEXT("/Game/ParagonKallari/Characters/Heroes/Kallari/Animations/AM_Right.AM_Right"));

	if (HitFrontFinder.Succeeded()) { HitReactFrontMontage = HitFrontFinder.Object; }
	if (HitBackFinder.Succeeded())  { HitReactBackMontage  = HitBackFinder.Object; }
	if (HitLeftFinder.Succeeded())  { HitReactLeftMontage  = HitLeftFinder.Object; }
	if (HitRightFinder.Succeeded()) { HitReactRightMontage = HitRightFinder.Object; }

	HitReactCooldown = 0.2f;

	// 死亡蒙太奇不配：木桩打不死（见 DummyMaxHealth），配了也走不到。
}

void AArenaTrainingDummy::BeginPlay()
{
	Super::BeginPlay();

	// --- 贴地 ---
	//
	// 【为什么必须手动做这一下】出生点是"备战点 + 偏移"算出来的，z 直接抄备战点的 z
	// （那是【玩家胶囊中心】的高度，通常比地面高 100 左右），于是不贴地的话它的脚就停在
	// 离地十几厘米的地方【悬着】。开了重力之后它自己也会掉下来，但那要等几个物理步，
	// 而第一帧就已经看得见了。这里是同步摆正，一帧都不悬。
	//
	// 备战点摆在四角那块台子上时更明显：台面 z=200、备战点 z=300，悬空 12 厘米。
	//
	// 【为什么用胶囊半高反推】命中点 + 半高 = 胶囊中心该在的 z。半高是现读的，
	// 不写死 88 —— 角色蓝图上改过胶囊尺寸的话这里要跟着对。
	//
	// 【为什么放在权限检查【之前】】服务端和客户端各自都要摆正：
	// 位置本来会复制，但客户端在收到第一次位置复制之前会先显示出生点那一帧，
	// 那一帧就是悬空的。两边都贴一次，这一帧也不会有。
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		const FVector Loc = GetActorLocation();
		const float HalfHeight = Capsule->GetScaledCapsuleHalfHeight();

		// 从头顶上方 50 往下找，最远探到"出生点下方 3 米"。
		// 探不到就保持原位 —— 说明这一带没有地面，硬贴反而会贴到奇怪的东西上。
		const FVector Start(Loc.X, Loc.Y, Loc.Z + HalfHeight + 50.f);
		const FVector End(Loc.X, Loc.Y, Loc.Z - HalfHeight - 300.f);

		FCollisionQueryParams Params(TEXT("TrainingDummyGroundSnap"), /*bInTraceComplex=*/false, this);
		if (FHitResult Hit; GetWorld() && GetWorld()->LineTraceSingleByChannel(
			Hit, Start, End, ECC_Visibility, Params))
		{
			SetActorLocation(FVector(Loc.X, Loc.Y, Hit.ImpactPoint.Z + HalfHeight));
		}
	}

	// 数值只由权威端写。客户端上那份是复制过来的，这边再写一次是白写。
	if (!HasAuthority() || !AbilitySystemComponent)
	{
		return;
	}

	// 【为什么不用 ApplyChampionStats / ApplyStats】那条路要一个等级，而等级来自
	// PlayerState（AHeroCombatCharacter::ApplyChampionStats 里那条回退到 1 级的分支
	// 就是这么来的）。木桩没有 PlayerState，也没有"该是几级"这回事 ——
	// 它只有一个诉求：血厚到打不死。所以直接写基础值。
	AbilitySystemComponent->SetNumericAttributeBase(
		UHeroCombatAttributeSet::GetMaxHealthAttribute(), DummyMaxHealth);
	AbilitySystemComponent->SetNumericAttributeBase(
		UHeroCombatAttributeSet::GetHealthAttribute(), DummyMaxHealth);
}
