// 英雄定义：把「这个 Pawn 是哪个英雄」的所有数据收成一份资产。
//
// 【为什么现在才收】在这之前 per-champion 的东西只有两样（技能组 ChampionKit、数值 ChampionStats），
// 各自挂在 AHeroCombatCharacter 上就够了。第三样出现时再一个一个加字段，就会出现
// 「改英雄要翻四个地方、还总有一个忘改」。这个类就是那个收口点。
//
// 收的是【引用】，不是数据本身：技能组是自己的 UAbilitySet，数值是自己的 UHeroStatConfig。
// 那些资产都是独立可复用的（召唤师技能组也能给别的英雄用），塞进这里当内嵌结构会丢掉复用。
//
// 以后再出现的 per-champion 东西（专属 UI 配置、专属被动参数、头像/称号）往这里加字段，
// 不要再去 AHeroCombatCharacter 上开新 UPROPERTY。
//
// 放 GAS/ 而不是别处：它引用的 UAbilitySet / UHeroStatConfig 都是 GAS 资产（判据见 CONVENTIONS.md 规则 2）。

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "HeroDefinition.generated.h"

class UAbilitySet;
class UHeroStatConfig;

UCLASS()
class LOL_API UHeroDefinition final : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 英雄显示名。目标框 / 击杀提示 / 选人界面用得上，目前没有消费者。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hero")
	FText DisplayName;

	/**
	 * 技能组（被动 + QWER）。留空 = 这个英雄没有技能，但数值照常生效。
	 *
	 * 【只给服务端用】：授予走 GiveToAbilitySystem，只在权威端调一次。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hero")
	TObjectPtr<UAbilitySet> ChampionKit;

	/**
	 * 数值表（1 级值 + 每级成长）。留空 = 退回属性集里的内置兜底表。
	 *
	 * 注意兜底是「整份」兜底：配了一半的表不会和兜底表合并 —— 那会让「某几条属性忘了配」
	 * 表现成「悄悄用了默认值」，而不是一眼看得出来的 0。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Hero")
	TObjectPtr<UHeroStatConfig> Stats;
};
