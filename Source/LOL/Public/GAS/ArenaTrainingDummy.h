// 斗魂竞技场：备战区的训练木桩。
//
// ===========================================================================
// 【它是什么】备战阶段立在备战区里的一个靶子：不还手、不会死，用来试连招和手感。
// 它不参与比赛 —— 不是参赛者、没有 PlayerState、不在任何 contender 表里。
//
// 【为什么继承 AHeroCombatCharacter】受击表现那条链是写死在它身上的：
// UGC_HitReact 一进来就 Cast<AHeroCombatCharacter>(MyTarget)，不是这个类就直接
// return false —— 连"记下这一下从哪来"都不做。所以木桩只要不是它，蒙太奇配得再全
// 也【一下都不会播】，而且不报任何错：表现是"打上去只有数字在掉，人纹丝不动"。
// 继承过来之后受击 / 状态蒙太奇 / 击退表现全是现成的，GC_HitReact 一行都不用改。
//
// （原本继承的是 ACharacter。那条路唯一的理由是「ASC 从 PlayerState 取，而木桩没有
//  PlayerState」—— 那件事在下面这一段解决了，不必再用换基类去绕。）
//
// 【继承它之后 ASC 从哪来】基类的 InitializeAbilityActorInfo 从 GetPlayerState 取，
// 木桩没有 PlayerState。所以本类【自己持一个 ASC，并在构造函数里就把它赋给基类那个
// 受保护成员 AbilitySystemComponent】—— 基类里所有读 ASC 的地方（IsDead /
// PlayHitReact / GetAbilitySystemComponent）于是全都通，基类一行都不用动。
//
// 【为什么在构造函数里赋，而不是 BeginPlay】ASC 是组件，它的 InitializeComponent 会
// 按 OwnerActor==AvatarActor==木桩 自己完成 InitAbilityActorInfo（对方实现
// IAbilitySystemInterface 且返回的就是这个组件）并发现同一 Owner 下的属性集 ——
// 而那一刻【早于 BeginPlay】。构造函数里赋值，这条路和"ASC 原生就长在自己身上"完全等价。
// 顺序上也安全：基类构造先跑、本类后跑，所以这里是覆盖基类，不会被基类覆盖回去。
//
// 【为什么不能让引擎给木桩配 AIController】有控制器就会有 PlayerState，而本项目的
// 参赛者表是按 PlayerState 认人的 —— 场上会多出一个身份不明的参赛者。
// 构造函数把 AIControllerClass 钉成 nullptr 就是为了这个（ACharacter 的默认值是
// AAIController + AutoPossessAI）。木桩本来也不需要控制器：它不动、不放技能。
//
// 【它不做什么】不主动移动、不放技能、不还手，也不死 —— 见 DummyMaxHealth。
//
// 【但它【会】被击退推着走】这是刻意的：击退（UGEComponent_Knockback → LaunchCharacter）
// 走的是移动组件，木桩一动不动地钉在地上就没法验证打击手感了。让它能动需要两件事，
// 都在构造函数里（见那边的注释）：① 不能待在 MOVE_None；② 必须打开
// bRunPhysicsWithNoController —— 没有控制器时引擎会直接把速度清零，连重力都不跑。
//
// 代价是它可能被推出备战台（台子半径 300，木桩离弧边只有约 65cm，一次击退就够）。
// 【被推下去就待在下面】—— 不配任何"送回原位"的兜底：击退能把它打出台子，
// 这件事本身就是击退力度的反馈，硬拽回来反而把这个反馈抹掉了。台子外是 z=0 的地面
// （±2000 的 Floor），掉不到世界外，KillZ 也够远，所以不管它也不会出问题。
// 想让它摔不下去就该在【玩法层】给台子加围栏，而不是在木桩身上打补丁。
//
// 【外观】网格 / 动画蓝图 / 受击蒙太奇在构造函数里按玩家角色那一套填好，
// 目的就是「打上去和打真人一样」。想换一套就做本类的蓝图子类覆写这些属性，再把
// BP_ArenaGameMode 上的 TrainingDummyClass 指过去（UCLASS(Blueprintable) 为这条路留着）。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "GAS/HeroCombatCharacter.h"
#include "ArenaTrainingDummy.generated.h"

class UHeroCombatAttributeSet;
class UMyAbilitySystemComponent;

UCLASS(Blueprintable)
class LOL_API AArenaTrainingDummy : public AHeroCombatCharacter
{
	GENERATED_BODY()

public:
	AArenaTrainingDummy();

protected:
	virtual void BeginPlay() override;

	/**
	 * 木桩自己的 ASC。
	 *
	 * 【为什么要单独留一个指针】基类那个 AbilitySystemComponent 是 protected 的，
	 * 本类在构造函数里把它指到这里来。留一个自己的强引用是为了不让它只被基类那个
	 * 「等 PlayerState 来填」的成员吊着 —— 这里它是被 CreateDefaultSubobject 建出来的，
	 * 生命周期本来就跟着 Actor，多一个指针只是让"这个 ASC 是木桩自己的"这件事写在明面上。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Arena|TrainingDummy")
	TObjectPtr<UMyAbilitySystemComponent> OwnAbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Arena|TrainingDummy")
	TObjectPtr<UHeroCombatAttributeSet> CombatAttributes;

	/**
	 * 木桩的血量上限（出生时也按它填满当前血量）。
	 *
	 * 【为什么是一个天文数字，而不是给它挂 State.Invulnerable】无敌那个标签是
	 * ExecCalc_Damage 的第一道门，命中就整条伤害直接 return —— 那样你连"我这一套
	 * 打下去到底掉了多少血"都测不出来，而试伤害正是木桩存在的理由。
	 * 给一个大血量则是伤害照常结算、只是永远到不了 0。
	 *
	 * 【1e7 这个数是怎么挑的】它在 float 里是精确值（24 位尾数能精确表示到约 1677 万），
	 * 所以不会出现"显示成 9999999.5"这类尾数误差。按本项目单次伤害几百的量级，
	 * 打满十万次也掉不到零头。
	 *
	 * 【为什么不需要再补一道"不死"】基类那套死亡链路是 HandleOutOfHealth 触发的，
	 * 而它只在 InitializeAbilityActorInfo 里挂上 —— 那个函数要有 PlayerState 才会走到底，
	 * 木桩永远走不到。所以即便血真的被打到 0，也只是站着不动。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Arena|TrainingDummy", meta = (ClampMin = "1"))
	float DummyMaxHealth = 1.0e7f;
};
