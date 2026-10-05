// 形态切换 GE 的实现。理由见头文件。

#include "GAS/GE_FormUnarmed.h"
#include "GAS/LOLGameplayTags.h"

UGE_FormUnarmed::UGE_FormUnarmed()
{
	// 形态一直成立到下次切换 ⇒ Infinite。
	// ★ 别改成 HasDuration：FSetByCallerFloat 不填的话时长算出来是 0，
	//   GE 挂上立刻过期 ⇒ 标签闪一下就没 ⇒ 手势做完了人还是持刀姿势，
	//   而且**没有任何日志**（形态那套的手感会退化成「动画播了但姿势没变」）。
	DurationPolicy = EGameplayEffectDurationType::Infinite;

	// UE5.3+：授予标签要通过 TargetTagsGameplayEffectComponent。
	//
	// ⚠️ 两种 CreateDefaultSubobject 别混：
	//   UObject 的成员版  → CreateDefaultSubobject<T>(FName 子对象名, 标志, Outer)
	//   FObjectInitializer 的 → ObjectInitializer.CreateDefaultSubobject<T>(this, FName 子对象名)
	// 两者参数顺序不同（第一个是「名字」还是「Outer」），传错就是 C2672 找不到重载。
	// 这里用 UObject 的成员版（不需要把构造函数改成带 FObjectInitializer 的形式）。
	TargetTags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Form_Unarmed);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

	// 不挂 GameplayCue：形态切换的表现完全由 montage 承担（UGA_FormSwitch 播收刀/拔刀），
	// 而那段 montage 和这个 GE 的生命周期【不是一回事】—— GE 是「切完之后一直挂着」，
	// 动画是「切换过程那两秒」。把 cue 挂这里会在切换动画播完之后又触发一次。
}
