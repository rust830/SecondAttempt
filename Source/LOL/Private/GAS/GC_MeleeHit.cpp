// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_MeleeHit.h"
#include "Audio/HeroAudioLibrary.h"
#include "GAS/CueCameraShake.h"
#include "GAS/LOLGameplayTags.h"
#include "Camera/CameraShakeBase.h"
#include "Engine/World.h"
#include "GameplayEffectTypes.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Kismet/GameplayStatics.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"
#include "UObject/SoftObjectPath.h"

UGC_MeleeHit::UGC_MeleeHit()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_MeleeHit;

	// 默认指向 Paragon 那条近战命中特效。软引用：默认值只是路径，编辑器里随时换。
	DefaultFX = TSoftObjectPtr<UParticleSystem>(
		FSoftObjectPath(TEXT("/Game/ParagonKallari/FX/Particles/Kallari/Abilities/Primary/FX/P_Kallari_Melee_SucessfulImpact.P_Kallari_Melee_SucessfulImpact")));

	// 音效：这一类身上不再留音效属性，一律走 UHeroAudioConfig 的事件表（Audio.MeleeHit）。
	// 原来构造函数里硬编码的 Kallari_Ability_LMB_Engage 属于「默认值藏在 C++ 里、填漏了也看不出来」
	// 那一类问题（见 CodeReview/12_音效层.md），删掉。
}

/**
 * MyTarget 这个角色当前是不是空手形态（判据与 GA_AirAttack / GC_EmpoweredAttack 一致：
 * GA_FormSwitch 切的 GE 授的 State.Form.Unarmed）。
 */
static bool IsAvatarUnarmed(const AActor* Avatar)
{
	const IAbilitySystemInterface* ASCInterface = Cast<IAbilitySystemInterface>(Avatar);
	const UAbilitySystemComponent* ASC = ASCInterface ? ASCInterface->GetAbilitySystemComponent() : nullptr;
	return ASC && ASC->HasMatchingGameplayTag(LOLGameplayTags::State_Form_Unarmed);
}

bool UGC_MeleeHit::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// 命中类型由 ApplyServerHit 塞进 AggregatedTargetTags；逐个查，第一个命中的生效。
	// 用 GetGameplayTagArray 而不是直接 range-for 容器，避免依赖容器的迭代器接口。
	UParticleSystem* FX = nullptr;
	TArray<FGameplayTag> TargetTags;
	Parameters.AggregatedTargetTags.GetGameplayTagArray(TargetTags);
	for (const FGameplayTag& Tag : TargetTags)
	{
		if (const TObjectPtr<UParticleSystem>* Found = HitFXMap.Find(Tag))
		{
			FX = Found->Get();
			break;
		}
	}
	if (!FX)
	{
		FX = DefaultFX.LoadSynchronous();
	}

	// Niagara 通道：命中类型没配、或者根本没配 Cascade 时也要有打击表现。
	// 新特效（Kallari 紫色系）走这条 —— 普攻原来只有音效，走完这条才真的「打得响也打得亮」。
	UNiagaraSystem* NiagaraFX = DefaultNiagaraFX.LoadSynchronous();
	UNiagaraSystem* Ring = ShockwaveNiagara.LoadSynchronous();

	// 本段的打击分量：由 UGA_ThreeHitPassive::ExecuteMeleeHitCue 从 Stage.HitCueWeight 塞进来
	// （走 NormalizedMagnitude）。四段普攻共用这一个 cue，不给分量的话「收尾那记最重的蓄力拳」
	// 和「第一下轻拳」的环和震屏一模一样大。
	//
	// <= 0 一律按 1 算：NormalizedMagnitude 默认是 0，而项目里别的执行点
	// （GA_FormMelee）根本不填它 —— 把 0 当成「零倍」会让那些命中直接没有环和震屏。
	const float ImpactWeight = Parameters.NormalizedMagnitude > 0.f ? Parameters.NormalizedMagnitude : 1.f;

	// Location / Normal 由 ApplyServerHit 从 FHitResult 填好：命中点 + 命中面法线。
	// 用 Normal 决定朝向，粒子才会贴着目标身体/地面朝外喷，而不是永远世界朝前。
	const FRotator Rotation = Parameters.Normal.IsNearlyZero()
		? FRotator::ZeroRotator
		: Parameters.Normal.Rotation();

	if (FX)
	{
		UGameplayStatics::SpawnEmitterAtLocation(World, FX, FTransform(Rotation, Parameters.Location));
	}
	if (NiagaraFX)
	{
		UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			this, NiagaraFX, Parameters.Location, Rotation, FVector::OneVector,
			/*bAutoDestroy=*/true, /*bAutoActivate=*/true);
	}
	// ---- ② 冲击波环（可选）----------------------------------------------
	//
	// 和命中爆发共用同一个朝向（法线）：环要贴着命中面铺开，而不是永远朝世界上/朝前。
	if (Ring)
	{
		UNiagaraComponent* Comp = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
			World, Ring, Parameters.Location, Rotation, FVector::OneVector,
			/*bAutoDestroy=*/true, /*bAutoActivate=*/true);

		if (Comp)
		{
			// 【为什么不写 Niagara 用户参数】本 cue 用的 NS_Melee_Shockwave 实测一个用户参数都没有
			// （ListUserParameters 返回 0），User.RingScale 根本不存在 → SetFloatParameter 是静默 no-op，
			// 环从来没跟着打击分量变过大小。直接缩放组件则不依赖任何参数，换任何环系统都吃。
			//
			// XY 按分量放大、Z 只吃基础值：环是平铺的，Z 一起放大会把厚度也吹起来，
			// 到 2.5 倍时看着像根柱子而不是冲击波。
			const float RingScale = ShockwaveScale * FMath::Max(ImpactWeight, 0.01f);
			Comp->SetWorldScale3D(FVector(RingScale, RingScale, ShockwaveScale));
		}
	}

	// 音效从事件表里取，而【持剑三段 / 空手四段普攻共用这一个 cue】—— GameplayCueSet 只按
	// 弹射出来的那个 GameplayCue.MeleeHit 查表，不会给连段各跑一份。所以「这一下是第几段、
	// 是哪一形态」只能走 CueParameters + 攻击者身上现读的形态标签：
	//   · 段号   → GA_ThreeHitPassive::ExecuteMeleeHitCue 把 StageIndex（0 起）塞进 RawMagnitude（+1 → 1 起）；
	//   · 形态   → 读攻击者 ASC 上的 State.Form.Unarmed（上面那条 helper），分出 Boxing / 基础两条事件。
	// 两个维度一起交给 PlayAtStage 查连段分句表（ComboSoundByTag），那一段没配就退回
	// Audio.MeleeHit 的基础条目，不会静默无声。
	// RawMagnitude <= 0（别的近战能力 / 直接 Execute 没填）→ FMath::Max 钳回第 0 段，走同一套兜底。
	const int32 StageIndex = FMath::Max(FMath::RoundToInt(Parameters.RawMagnitude) - 1, 0);
	const FGameplayTag HitAudioEvent = IsAvatarUnarmed(MyTarget)
		? LOLGameplayTags::Audio_MeleeHitBoxing
		: LOLGameplayTags::Audio_MeleeHit;
	UHeroAudioLibrary::PlayAtStage(World, HitAudioEvent, StageIndex, Parameters.Location);

	// 镜头振动只给【打人的那一端】。判定规则（为什么用 MyTarget、为什么两层判断、
	// 为什么不能改成顿帧）见 CueCameraShake.h。
	//
	// 「重的那一击抖得更狠」靠【换资产】而不是乘倍率 —— ClientStartCameraShake 的
	// Scale 在 5.8 里是死的（本项目的抖动资产不读它），乘上去看着像在调强度，
	// 实际一个字节都不影响。
	const bool bHeavyHit = ImpactWeight >= HeavyShakeWeightThreshold;
	const TSubclassOf<UCameraShakeBase> Shake = (bHeavyHit && HeavyHitCameraShake) ? HeavyHitCameraShake : HitCameraShake;

	HeroCueCameraShake::PlayLocalHitShake(MyTarget, Shake, HitCameraShakeScale);

	// 全留空 = 这次命中没有任何表现，和加音效之前一样返回 false（不是错误，只是没东西可播）。
	// 音效不计入：它走全局事件表，有没有响不能拿来判定「这一击有没有表现」。
	return FX != nullptr || Shake != nullptr;
}
