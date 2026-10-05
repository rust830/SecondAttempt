// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"       // ⚠️ 不是 "DataAsset.h"（那路径在 5.8 里不存在，UHT/UBT 直接 C1083）
#include "GameplayTagContainer.h"   // FGameplayTag —— 不在 CoreMinimal 里，必须显式引
#include "Sound/SoundBase.h"

#include "HeroAudioConfig.generated.h"

/**
 * 全项目音效的【唯一配置源】（2026-10-04，见 CodeReview/12_音效层.md）。
 *
 * 【它解决的是哪一类 bug】
 * 收敛之前 19 个音效槽位散在 16 个 C++ 类里，一半默认值硬编码在构造函数、一半靠人手在
 * 蓝图里点。填漏的那 8 个（闪避 / 格挡 / 破隐 / 退出强化 / 收割现形 / 收割爆裂 /
 * 飞踢命中 ×2）运行时是静默的：PlaySoundAtLocation 收到 nullptr 直接 no-op，
 * 连一行日志都没有 ⇒ 表现只能是「这技能没声音」，得开 PIE 一个个试才找得到。
 *
 * 【语义】
 *   事件 tag → 音效资产。发音效的地方只说「我现在要发 Audio.MeleeHit 这个音」，
 *   不关心是哪个文件、将来换成什么资源。「哪个事件配哪个音」全部收在这里。
 *   SoundOverride 是单点例外入口：某个 GC / 能力想自己钉死一个音，填它，
 *   填了就优先于表里那条（空 = 走表）。
 *
 * 【资产】内容侧落在 /Game/LOL/Audio/DA_HeroAudioConfig。
 *   一律配 SoundCue：Paragon 的 cue 内部挂的是 Kallari_*_NNN_Dialogue 多句随机，
 *   直接指单条 Wavs 下的 SoundWave 会丢掉变体随机（收敛前就有 5 处这么干）。
 *
 * 【为什么表值是硬引用（TObjectPtr）而不是 TSoftObjectPtr —— 2026-10-05】
 *   这张表是全项目音效的【唯一】引用点：GC 身上不挂音效、也没有蓝图硬引用它，
 *   cook 的引用图里看不到任何一条音效 ⇒ 打包版整条链全哑（软引用不被 cook 跟随，
 *   5.8 的 FPrimaryAssetRules 也没有"连软引用一起 cook"的开关）。改成硬引用后：
 *   cook 跟着表走，缺资产在 cook 时直接报错，而不是打包版运行时无声。
 *   打包体积代价可忽略 —— 表里就 20 几条 cue/wave，语音数据本身照常流送。
 *   配置资产自身的进包靠 DefaultGame.ini 的 DirectoriesToAlwaysCook=/Game/LOL/Audio。
 */
/**
 * 连段分句的值类型。单独包一层 struct 不是多此一举：UHT 不允许容器直接做 TMap 的值
 * （TArray<TSoftObjectPtr<USoundBase>> 会报 "can not be used as a value in a TMap"），
 * 套一层 USTRUCT 就合法了，编辑器里表现成一个可展开的列表。
 */
USTRUCT(BlueprintType)
struct FHeroAudioComboStage
{
	GENERATED_BODY()

	/**
	 * 下标 = 段号（0 起）。空数组 = 这个事件没有连段分句，全走基础条目。
	 *
	 * 段数不写死在数组长度里：普攻持剑是三段、空手四连拳是四段，都走同一个键
	 * （Audio.PassiveAttack / Audio.PassiveAttack.Boxing），谁短谁就只用得到前几段，
	 * 越界的下标由 UHeroAudioLibrary::PlayAtStage 兜到基础条目，不会崩也不会静默。
	 * 想给某一形态换成另一套音色，加一条 *Boxing 变体键就行，不改代码。
	 *
	 * 硬引用（见类注释）：这些条目只被本表引用，软引用不会进 cook 的引用图。
	 */
	UPROPERTY(EditAnywhere, Category = "Hero Audio")
	TArray<TObjectPtr<USoundBase>> Sounds;
};

/**
 * 命中音的次层描述（角色语气词 + 音量比例）。为什么单独一层 struct：
 * 1) UHT 不允许容器直接做 TMap 的值；2) 音量比例要跟音一起配，散在两个表里迟早对不上。
 */
USTRUCT(BlueprintType)
struct FHeroAudioSecondaryLayer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Hero Audio")
	TObjectPtr<USoundBase> Sound;

	/** 次层音量比例（1.0 = 和主层一样响）。0 = 相当于没配这层。 */
	UPROPERTY(EditAnywhere, Category = "Hero Audio")
	float Volume = 0.4f;
};

UCLASS(BlueprintType)
class LOL_API UHeroAudioConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	/** 音效事件（Audio.*）→ 音效资产。硬引用（见类注释）；空条目 UHeroAudioLibrary 会按 tag 打一次 warning。 */
	UPROPERTY(EditDefaultsOnly, Category = "Hero Audio")
	TMap<FGameplayTag, TObjectPtr<USoundBase>> SoundByTag;

	/** 兜底音：表里没登记（或登记了但没填）的事件都退回这里。可空 —— 空就当「这个事件没声」。 */
	UPROPERTY(EditDefaultsOnly, Category = "Hero Audio")
	TObjectPtr<USoundBase> FallbackSound;

	/**
	 * 连段分句表：外层键 = 事件标签（现在只有 Audio.MeleeHit 在用），内层下标 = 段号（0 起）。
	 *
	 * 【为什么单独一张表，而不是往 SoundByTag 里塞 Audio.MeleeHit.Stage1 这种子标签】
	 * FGameplayTag 只能由 C++ 原生注册（UE_DEFINE_GAMEPLAY_TAG）或项目设置登记，
	 * UE 5.8 的 Python 侧造不出 tag 对象 ⇒ 脚本没法往 SoundByTag 里灌新键；
	 * 而且「第几段」本来就是数组语义，用下标比拆成三条独立标签更贴近连段的本意。
	 *
	 * 【谁在用】UGC_MeleeHit 从 CueParameters.RawMagnitude 里拿段号（GA_ThreeHitPassive
	 * 塞进去的，1 起）来查这里；段号越界 / 这条没配 → 退回 SoundByTag[Audio.MeleeHit]。
	 *
	 * EditAnywhere 而不是 EditDefaultsOnly：DataAsset 资产本身就是实例，Python 的
	 * set_editor_property 只能写「允许实例编辑」的属性（SoundByTag 那条能写通是特例——
	 * 资产的 archetype 是 CDO；struct 里的嵌套属性没有这条豁免，实测会被拒）。
	 */
	UPROPERTY(EditAnywhere, Category = "Hero Audio")
	TMap<FGameplayTag, FHeroAudioComboStage> ComboSoundByTag;

	/**
	 * 命中音的【次层】：主层 = SoundByTag / ComboSoundByTag 里的事件音（物理打击声，1.0 满音量），
	 * 次层 = 这里配的角色语气词，按 Volume 的比例压低着同时播。
	 *
	 * 【分清主次】打击音满音量负责「打中了」，语气词压到 0.4 左右只做「人出了力」的点缀；
	 * 反过来配就会回到「打一下吼一声」的老毛病（见 §14）。键 = 事件标签（和主层同一个键），
	 * 没登记的事件只有主层，行为不变。
	 */
	UPROPERTY(EditAnywhere, Category = "Hero Audio")
	TMap<FGameplayTag, FHeroAudioSecondaryLayer> SecondaryLayerByTag;
};
