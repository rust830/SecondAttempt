// 斗魂竞技场：AI 画像存档的实现。
//
// 这个类没有行为 —— 它的全部价值是"一个能被 USaveGame 序列化的容器"。
// 留一个空的 .cpp 是为了和项目里其它 UCLASS 保持一致（CONVENTIONS.md 的
// Public/Private 一一对应），并且给 UHT 一个稳定的编译单元。

#include "GAS/ArenaAISaveGame.h"
