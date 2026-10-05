// Copyright Epic Games, Inc. All Rights Reserved.

#include "LOLCharacter.h"
#include "Camera/LOLCameraBoom.h"
#include "Engine/LocalPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "LOL.h"

ALOLCharacter::ALOLCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);

	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f);

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 500.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// 【起步/刹车的对称性】AHeroCombatCharacter 构造里把 MaxAcceleration 设成
	// MoveAccelSpeed(=1200)，让 MoveSpeed 属性的语义是「目标最大速度」而不是「瞬时速度」。
	// 这里配套把刹车调软一点：刹得比重加速快会显得「松手就黏在地上」，
	// 2000（引擎常用值）配 345 的最高速约 0.17s 停住，偏快、偏硬。
	// 1600 约 0.22s，起步和刹车的「重量感」更接近。
	GetCharacterMovement()->BrakingDecelerationWalking = 1600.f;

	// Create a camera boom (pulls in towards the player if there is a collision)
	//
	// 用 ULOLCameraBoom（见 Camera/LOLCameraBoom.h）而不是原生 USpringArmComponent：
	// 原生「撞到/离开障碍」都是【一帧硬切】，位移技能扫过柱子、门框、箱子时相机就在
	// 全长和命中点之间反复瞬移 —— 画面被挡一下的那种「一闪」是它造成的。
	// 子类里收拢仍然瞬时（不能把相机留在墙里）、展开改成平滑，并且加了最小臂长，
	// 贴墙时也不让相机缩进角色自己的身体里。
	CameraBoom = CreateDefaultSubobject<ULOLCameraBoom>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 400.0f;
	CameraBoom->bUsePawnControlRotation = true;

	// 【越肩偏移】—— 让角色从屏幕正中间挪开，屏幕中心才是空出来给准心的。
	//
	// 轴的含义（SocketOffset 在弹簧臂的旋转空间里，而弹簧臂跟着控制旋转走）：
	//   Y = 右，相机往右挪 ⇒ 角色在画面里偏向左边，中心空出来。这是常规越肩视角：
	//       400cm 处 60cm ≈ 8.5°，约画幅的 19%，不算夸张。
	//   Z = 上，见下面那段 —— 这个是重点，不是可选项。
	//   X 别动：那是沿弹簧臂方向，等于偷偷改 TargetArmLength。
	//
	// 【Z 为什么是 60 而不是 0】弹簧臂挂在 RootComponent 上，而 UE 角色的胶囊原点在
	// 【胶囊中心】不是脚底：半高 90cm ⇒ 枢轴就在离地 90cm。Kallari 高 182cm，
	// 所以 90cm 正好是【髋部】—— 相机在髋部平视，屏幕中心那条线就是髋部那条线，
	// 表现就是准心横在角色髋部旁边，像贴着地面在看。抬高 60cm 到 150cm（胸口/肩高）
	// 之后，准心那条线才落在「肩线」上，读数才是正常的越肩视角。
	//
	// 【Z 不能再往上抬】准星射线（AHeroCombatCharacter::TraceManualTargetUnderCrosshair）
	// 是从相机沿视线方向的一条【直线】，pitch=0 时它全程保持在相机高度、不随距离下降。
	// 相机一旦高过角色头顶（182cm），这条线在远处就从所有人头顶飞过去 ——
	// 「按住 R 点不中人」就是这么来的。150cm 在胸口，远近都还在身体里，安全。
	//
	// 换肩（左肩）就把 Y 取负。数值嫌多嫌少直接在这里调，或者到角色 BP 的
	// Components 面板里覆盖 CameraBoom 的同名属性。
	CameraBoom->SocketOffset = FVector(0.0f, 60.0f, 60.0f);

	// 【相机被别人挡住时自动拉近】—— 多人里你贴到别人脸上时，对方的相机后撤而不是被你的模型糊住。
	// SpringArm 本来就忽略 Owner，所以不会顶走你自己的相机。
	CameraBoom->bDoCollisionTest = true;
	CameraBoom->ProbeChannel = ECC_Camera;
	CameraBoom->ProbeSize = 12.0f;

	// 【其他角色不挡相机】—— 贴脸时对方身体不再触发 SpringArm 收短，
	// 相机保持 400cm 不塌 ⇒ "摄像机穿过自身角色" 和 "被对方角色挡住" 一起消失。
	// 相机仍然会被【世界几何】（墙/地形）挡住而拉近，这个保留。
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	GetMesh()->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);

	// Create a follow camera
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false;

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character)
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)
}

void ALOLCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// 【只淡“别人”】自己的模型不能淡 —— 它就是你要看的东西。
	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (PC == nullptr) return;

	const APawn* LocalPawn = PC->GetPawn();
	if (LocalPawn == nullptr || LocalPawn == this) return;

	// 本机相机的真实位置（就是 FollowCamera 落在的地方）。
	// 用相机而不是角色的眼睛高度：挡镜头的判据全在这条视线上。
	FVector CamLoc;
	FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);

	const FVector PawnLoc = LocalPawn->GetActorLocation();
	const FVector MyLoc = GetActorLocation();

	// 【为什么量"到线段"而不是"到角色"】相机挂在本机角色【身后】400cm。
	// 真正挡镜头的是夹在「相机」和「本机角色」之间的那个身体 —— 位移技能扫过去、
	// 贴到镜头上那一下，它离本机角色还有 350cm+，按"到角色的距离"永远够不到阈值；
	// 按"到 [相机 → 本机角色] 这条线段"的距离算，它就在 0 上。
	//
	// 站在角色【前面】的人不挡相机，所以取两者较小值：前面的身体仍然走老规则
	// （贴脸淡出），夹在中间的身体走新规则。
	const FVector Seg = PawnLoc - CamLoc;
	const float SegLenSq = Seg.SizeSquared();
	const float T = (SegLenSq > 1.f) ? FVector::DotProduct(MyLoc - CamLoc, Seg) / SegLenSq : 0.f;
	const float ToSegment = FMath::PointDistToSegment(MyLoc, CamLoc, PawnLoc);
	const float Dist = FMath::Min(FVector::Dist(PawnLoc, MyLoc), ToSegment);

	// 60cm 全透明，150cm 全实心 —— 贴脸就淡，拉开就回来
	const float Fade = FMath::Clamp((Dist - 60.f) / 90.f, 0.f, 1.f);
	GetMesh()->SetScalarParameterValueOnMaterials(TEXT("CameraFade"), Fade);

	// 【夹在中间的身体，让本机相机收短】
	//
	// 只动胶囊对 ECC_Camera 的响应。这个通道全工程【只有弹簧臂探针】在用
	// （其余射线走 Visibility / Pawn），所以这里按帧改它不会牵连命中判定、瞄准、
	// 伤害、网络同步 —— 碰不到别的系统。碰撞响应本身也不复制：这只是本机内存里的
	// 一份数据，表现纯本机，别人看不到。
	//
	// T 必须落在 (0,1)：只有【相机和本机角色之间】的身体才挡得住相机。
	// 站在角色前面（T<0）、跑到相机背后（T>1）的人不在视野里，不该把镜头拽近。
	// 胶囊的响应没变化时 SetCollisionResponseToChannel 会直接返回，不产生每帧开销。
	if (CameraOccluderRadius > 0.f)
	{
		const bool bInTheWay = (T > 0.f) && (T < 1.f) && (ToSegment < CameraOccluderRadius);
		GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Camera, bInTheWay ? ECR_Block : ECR_Ignore);
	}
}

void ALOLCharacter::DoMove(float Right, float Forward)
{
	// 死了不能走。技能那一侧由 UMyGameplayAbility 的 ActivationBlockedTags(State.Dead) 挡，
	// 移动不走 GAS，所以这道门只能在这里。复活后 State.Dead 被 GE 摘掉，这里自动放行。
	if (IsDead()) return;

	// ★ 硬控期间不能走。技能那一侧由 UMyGameplayAbility 的 ActivationBlockedTags 挡掉了
	//   （State.Stunned / State.Knockback / State.KnockUp），但移动不走 GAS —— 不给这道门，
	//   被击退的 0.5s 里玩家可以用 MaxWalkSpeed=500 把击退速度直接覆盖掉，表现就是「击退不明显」。
	//
	//   为什么用 StopMovementImmediately 而不是只 return：不动输入只是不再加新的加速度，
	//   被 LaunchCharacter 打出来的速度还在，地面 GroundFriction=8 会把它慢慢磨掉 ——
	//   那还是"能自己走"。这里直接把速度清零，位移完全交给击退冲量本身（抛物线飞出去）。
	//
	//   注意：这段只在【有输入】时执行。玩家不按键就不会走到这里，所以冲量一打完就自然停住，
	//   不会每帧被清零（那样只能原地不动，又走向另一个极端）。
	if (IsHardControlled())
	{
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
		}
		return;
	}

	if (GetController() != nullptr)
	{
		// find out which way is forward
		const FRotator Rotation = GetController()->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		// get forward vector
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);

		// get right vector
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// add movement
		AddMovementInput(ForwardDirection, Forward);
		AddMovementInput(RightDirection, Right);
	}
}

void ALOLCharacter::DoLook(float Yaw, float Pitch)
{
	if (GetController() != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(Yaw);
		AddControllerPitchInput(Pitch);
	}
}

void ALOLCharacter::DoJumpStart()
{
	// 死了不能跳。理由同 DoMove。
	if (IsDead()) return;

	// 硬控期间不能跳。理由同 DoMove（跳跃也是移动，不受 GAS 的 ActivationBlockedTags 管）。
	if (IsHardControlled()) return;

	// 闪避窗口内空格 = 二段 evade（拦截跳跃）。没开窗口时照常跳。
	if (TryRouteDodgeEvade()) return;

	// signal the character to jump
	Jump();
}

void ALOLCharacter::DoJumpEnd()
{
	// signal the character to stop jumping
	StopJumping();
}
