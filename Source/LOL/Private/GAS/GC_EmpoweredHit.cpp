// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_EmpoweredHit.h"
#include "GAS/LOLGameplayTags.h"
#include "Engine/World.h"
#include "GameplayEffectTypes.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Sound/SoundBase.h"
#include "UObject/SoftObjectPath.h"

namespace
{
	/** 把系统里现有的 User.* 参数拼成一行，参数名对不上时直接打出来对照。 */
	FString DescribeUserParameters(UNiagaraSystem* System)
	{
		TArray<FNiagaraVariable> Parameters;
		System->GetExposedParameters().GetParameters(Parameters);

		TArray<FString> Names;
		for (const FNiagaraVariable& Parameter : Parameters)
		{
			const FString Name = Parameter.GetName().ToString();
			if (Name.StartsWith(TEXT("User.")))
			{
				Names.Add(Name);
			}
		}
		return Names.Num() > 0 ? FString::Join(Names, TEXT(", ")) : FString(TEXT("（一个都没有）"));
	}

	/**
	 * 参数名对不上时 SetVariablePosition 是静默 no-op（NiagaraComponent.cpp 里会退化成 SetVariableVec3，
	 * 名字还是找不到就什么都不做）→ NS 生成了但位置参数没喂进去，粒子会趴在世界原点炸，看不出原因。
	 * 和 UAnimNotifyState_BladeTrail / UGA_ThreeHitPassive 里那两份判断同一个道理。
	 */
	void WarnIfMissingParameter(UNiagaraSystem* System, const FName& ParameterName)
	{
		TArray<FNiagaraVariable> Parameters;
		System->GetExposedParameters().GetParameters(Parameters);

		const bool bFound = Parameters.ContainsByPredicate(
			[&ParameterName](const FNiagaraVariable& Parameter) { return Parameter.GetName() == ParameterName; });
		if (!bFound)
		{
			UE_LOG(LogTemp, Warning, TEXT("[EmpoweredHit] NS %s 里没有参数 %s → 命中点喂不进去。系统里现有的 user parameter：%s"),
				*GetNameSafe(System), *ParameterName.ToString(), *DescribeUserParameters(System));
		}
	}
}

UGC_EmpoweredHit::UGC_EmpoweredHit()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_EmpoweredHit;

	// 默认指向项目里那个 NS。用软引用：不赋值就保持默认，编辑器里也能随手换成别的
	// （和 UAnimNotifyState_BladeTrail::TrailSystem 同一种写法）。
	HitSystem = TSoftObjectPtr<UNiagaraSystem>(FSoftObjectPath(TEXT("/Game/LOL/Niagara/NS_PerfectSuccess.NS_PerfectSuccess")));
}

bool UGC_EmpoweredHit::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Location / Normal 由 UGA_ThreeHitPassive 从 FHitResult 填好：命中点 + 命中面法线。
	const FVector Location = Parameters.Location;
	const FRotator Rotation = Parameters.Normal.IsNearlyZero()
		? FRotator::ZeroRotator
		: Parameters.Normal.Rotation();

	UNiagaraSystem* System = HitSystem.LoadSynchronous();
	if (System)
	{
		WarnIfMissingParameter(System, ImpactParameter);

		// 爆在命中点上，不挂到谁身上：这一下就是「打实了」的即时反馈，挂在目标身上会跟着跑。
		// bAutoDestroy 让它自己收（NS 按 burst 配就行；emitter 要是设成无限循环会漏组件，
		// 但和 BladeTrail 那边不同，这里没有明确的「该收尾了」的时刻，不适合再挂兜底定时器）。
		UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World, System, Location, Rotation, FVector::OneVector, /*bAutoDestroy=*/true, /*bAutoActivate=*/true);
		if (Component)
		{
			// 组件已经生成在命中点上了，再喂一次 ImpactPos：NS 里那个 emitter 不管是读参数还是读组件原点，
			// 拿到的都是同一个位置。
			Component->SetVariablePosition(ImpactParameter, Location);
		}
	}
	else if (!bLoggedMissingSystem)
	{
		bLoggedMissingSystem = true;
		UE_LOG(LogTemp, Warning, TEXT("[EmpoweredHit] 命中特效没配：%s 上的 HitSystem 是空的（软引用加载失败也一样）→ 强化那一击命中了不会有额外表现"),
			*GetNameSafe(this));
	}

	if (USoundBase* Sound = HitSound.LoadSynchronous())
	{
		UGameplayStatics::PlaySoundAtLocation(World, Sound, Location);
	}

	return true;
}
