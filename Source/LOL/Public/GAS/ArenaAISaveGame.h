// 斗魂竞技场：AI 对玩家行为画像的存档。
//
// ===========================================================================
// 【为什么需要跨局存档 —— 这是"进化"能不能成立的前提】
// 一局 1V1 只有几个回合，任何一个行为比率在这么短的样本里都是噪声。
// 想让 Bot "学会你这个人的打法"，观测必须跨越很多局累积，那就必须落盘。
//
// 于是它同时带来了一个设计约束：FArenaPlayerProfile 从此是一个【存档 schema】，
// 字段只能增不能改语义（旧存档读进来新字段拿默认值，能活；改了语义则读到的是
// 一份意思已经变了的数据，比读不到更糟）。版本号见 FArenaPlayerProfile::SchemaVersion。
//
// 【为什么不是 SaveGame 的其它形态】用引擎的 USaveGame + UGameplayStatics 是
// 最省事且最不容易出错的路径：序列化、槽位管理、跨平台路径全部由引擎负责。
// 自己写 ini/json 要多处理"文件在哪、什么时候刷盘、并发写"，没有收益。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "GAS/ArenaAITypes.h"
#include "ArenaAISaveGame.generated.h"

UCLASS()
class LOL_API UArenaAISaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** 累积下来的玩家画像。 */
	UPROPERTY(BlueprintReadOnly, Category = "Arena|AI")
	FArenaPlayerProfile Profile;
};
