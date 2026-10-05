// Fill out your copyright notice in the Description page of Project Settings.

#include "Animation/HeroEvadeAnimDriver.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "UObject/UnrealType.h"

namespace
{
	/**
	 * 按属性类型写一个浮点值。蓝图 float 就是 FFloatProperty；double 一起兜住是为了
	 * 万一 ABP 里那个变量被改成 double 也不至于静默不写。
	 *
	 * 名字带 EvadeAnimDriver 前缀：匿名 namespace 挡不住 unity build 的重名
	 * （多个 .cpp 会被拼进同一个翻译单元），见 CONVENTIONS.md。
	 */
	void EvadeAnimDriver_SetFloat(FProperty* Prop, UObject* Object, float Value)
	{
		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			FloatProp->SetPropertyValue_InContainer(Object, Value);
		}
		else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			DoubleProp->SetPropertyValue_InContainer(Object, static_cast<double>(Value));
		}
	}
}

UHeroEvadeAnimDriver::UHeroEvadeAnimDriver()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 没在闪避时不需要 tick，起手时再打开 —— 省掉每帧一次空转。
	PrimaryComponentTick.bStartWithTickEnabled = false;
	// 必须在网格求值动画【之前】把变量写好，见类注释。
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

void UHeroEvadeAnimDriver::BeginPlay()
{
	Super::BeginPlay();

	// 把本组件排在骨骼网格之前：两者都在 TG_PrePhysics，靠显式前置依赖定序。
	// 不做的话"写变量"可能落在动画求值之后，表现为起手 / 收尾各闪一帧。
	if (const ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		if (USkeletalMeshComponent* Mesh = Character->GetMesh())
		{
			AddTickPrerequisiteComponent(Mesh);
		}
	}
}

void UHeroEvadeAnimDriver::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 角色销毁 / 关卡卸载兜底：把 bIsEvading 落回去。不落的话 AnimInstance 会带着
	// "正在闪避" 的旧值被复用（重生 / 对象池），表现成角色一出生就摆着闪避姿势。
	StopEvade();

	Super::EndPlay(EndPlayReason);
}

void UHeroEvadeAnimDriver::PlayEvade(bool bForward, float Duration)
{
	EvadeDuration = Duration > 0.f ? Duration : DefaultEvadeBlendDuration;
	EvadeElapsed = 0.f;
	EvadeProgress = 0.f;
	// BS 的 Y 轴就是方向角：0 = 前，180 = 后。左右不单独占档
	//（和原来的蒙太奇版一致，左/右折进"前"）。
	EvadeDirection = bForward ? 0.f : 180.f;
	bEvading = true;

	// 起手帧立刻写一次，不等下一次 tick —— 否则起手要多等一帧才看得到。
	if (UAnimInstance* Anim = ResolveAnimInstance())
	{
		RebuildBinding(Anim);
		WriteEvadeState(true, EvadeProgress, EvadeDirection);
	}

	SetComponentTickEnabled(true);
}

void UHeroEvadeAnimDriver::StopEvade()
{
	// 幂等：没在闪避时是 no-op（EndPlay 会无条件调一次）。
	if (!bEvading)
	{
		return;
	}

	bEvading = false;
	SetComponentTickEnabled(false);

	if (UAnimInstance* Anim = ResolveAnimInstance())
	{
		// 只落 bIsEvading，progress 留在当前值：自然播完时它是 1（末姿态），
		// 中途被打断时是打断那一帧的进度。两种情况下 ABP 都是从"当前姿态"开始混出，
		// 不会突然跳回起点 —— 这也正是把 progress 与 bool 分开写的原因。
		WriteEvadeState(false, EvadeProgress, EvadeDirection);
	}
}

void UHeroEvadeAnimDriver::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEvading)
	{
		return;
	}

	EvadeElapsed += DeltaTime;
	EvadeProgress = EvadeDuration > UE_KINDA_SMALL_NUMBER
		? FMath::Clamp(EvadeElapsed / EvadeDuration, 0.f, 1.f)
		: 1.f;

	if (UAnimInstance* Anim = ResolveAnimInstance())
	{
		WriteEvadeState(true, EvadeProgress, EvadeDirection);
	}

	if (EvadeProgress >= 1.f)
	{
		// 走完：把 bIsEvading 落回 false，剩下的混出交给 ABP 的 BlendListByBool
		//（blend_time 0.1），不用在这里做淡出。
		StopEvade();
	}
}

UAnimInstance* UHeroEvadeAnimDriver::ResolveAnimInstance()
{
	if (UAnimInstance* Cached = CachedAnimInstance.Get())
	{
		return Cached;
	}

	// 每帧懒取一次：AnimInstance 会被重建（重生 / 换皮 / 换 ABP），
	// 只缓存一次的话在那之后会永远拿到 null。
	const ACharacter* Character = Cast<ACharacter>(GetOwner());
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;

	CachedAnimInstance = Anim;
	if (!Anim)
	{
		// 实例没了：绑定和"已经吵过一次"的旗标一起清掉，
		// 等新实例来了重新解析 / 允许重新报错。
		BoundAnimClass.Reset();
		PropIsEvading = nullptr;
		PropProgress = nullptr;
		PropDirection = nullptr;
		bWarnedMissingVars = false;
	}

	return Anim;
}

void UHeroEvadeAnimDriver::RebuildBinding(UAnimInstance* Anim)
{
	if (!Anim)
	{
		return;
	}

	UClass* AnimClass = Anim->GetClass();

	// 同一个类，且要么已经绑好、要么已经吵过一次 —— 都不用重找。
	//（不做这个判断的话就是每帧三次按名字的线性查找。）
	if (BoundAnimClass.Get() == AnimClass && (HasBinding() || bWarnedMissingVars))
	{
		return;
	}

	BoundAnimClass = AnimClass;
	PropIsEvading = nullptr;
	PropProgress = nullptr;
	PropDirection = nullptr;

	// 这三个是 ABP 自己的蓝图变量，只能按名字反射拿 —— 库存 Paragon 的 ABP
	// 原生父类就是 UAnimInstance，项目加不了原生成员。见类注释。
	// 类型必须对得上：bIsEvading = bool，EvadeProgress / EvadeDirection = float。
	PropIsEvading = AnimClass->FindPropertyByName(FName(TEXT("bIsEvading")));
	PropProgress = AnimClass->FindPropertyByName(FName(TEXT("EvadeProgress")));
	PropDirection = AnimClass->FindPropertyByName(FName(TEXT("EvadeDirection")));

	if (HasBinding())
	{
		bWarnedMissingVars = false;
		return;
	}

	// 缺变量 = 整条链哑掉（动画不动、也没有任何报错），必须吵一次让人能定位。
	if (!bWarnedMissingVars)
	{
		bWarnedMissingVars = true;
		UE_LOG(LogTemp, Warning,
			TEXT("[EvadeAnim] %s 上没凑齐 BS_Evade 的三条驱动变量（bIsEvading=%d EvadeProgress=%d EvadeDirection=%d）。")
			TEXT("闪避 BlendSpace 不会动 —— 去对应的 ABP 里确认这三个蓝图变量存在且类型是 bool/float/float。"),
			*GetNameSafe(AnimClass),
			PropIsEvading ? 1 : 0,
			PropProgress ? 1 : 0,
			PropDirection ? 1 : 0);
	}
}

void UHeroEvadeAnimDriver::WriteEvadeState(bool bIsEvading, float Progress, float Direction)
{
	// 绑定不完整时静默跳过：RebuildBinding 已经吵过一次，逐帧再报没有意义。
	if (!HasBinding())
	{
		return;
	}

	UAnimInstance* Anim = CachedAnimInstance.Get();
	if (!Anim)
	{
		return;
	}

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(PropIsEvading))
	{
		BoolProp->SetPropertyValue_InContainer(Anim, bIsEvading);
	}
	EvadeAnimDriver_SetFloat(PropProgress, Anim, Progress);
	EvadeAnimDriver_SetFloat(PropDirection, Anim, Direction);
}
