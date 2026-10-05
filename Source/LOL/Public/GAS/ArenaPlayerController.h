// 斗魂竞技场：本地玩家的输入模式 / 冻结。
//
// ===========================================================================
// 【这个类存在的唯一理由：奖励界面在客户端点不了】
//
// 这个项目从来没有让玩家用鼠标点过 UI —— Config/DefaultInput.ini 里配的是
// CapturePermanently_IncludingInitialMouseDown + LockOnCapture，而全项目
// 没有任何一处调用过 SetInputMode / SetShowMouseCursor。也就是说：
// 光有一个画出来的三选一界面，鼠标是被捕获并隐藏的，谁都点不到它。
//
// 所以"进入奖励阶段"必须包含三件事，缺一不可：
//   ① SetInputMode(FInputModeUIOnly) —— 让游戏输入整个停下来（引擎那条路是
//      UGameViewportClient::SetIgnoreInput(true)，GameViewportClient 的 InputKey 直接早返回），
//      而 Slate 的命中测试不看这个开关，所以 widget 照样能点；
//   ② SetShowMouseCursor(true) —— 把鼠标放出来；
//   ③ SetIgnoreMoveInput / SetIgnoreLookInput —— 兜一层。UIOnly 已经够了，
//      但这一层能挡住"某个输入路径绕过 viewport"的情况，而且代价是零。
//
// 【为什么这件事必须由 PlayerController 做，不能由 GameMode 做】
//   - FInputModeUIOnly 那条路要 Cast<ULocalPlayer>(Player)（PlayerController.cpp:6454），
//     服务端对远端玩家的 PC 调它是个静默 no-op；
//   - SetIgnoreMoveInput 加的是【本机那个 Controller 上】的计数器，
//     listen server 上服务端调它只影响主机自己，客户端那份还是 0。
// 也就是说服务端根本没有能力冻结一个远端玩家的输入。唯一能做这件事的地方，
// 是那个玩家自己的客户端。所以 GameMode 只管复制相位，本类照着相位做反映。
//
// 【为什么是轮询相位，不是订阅 OnArenaPhaseChanged】
// GameState 在客户端是【后到】的（BeginPlay 时往往还是 null），
// 在 BeginPlay 里订阅会静默订不上，而在别处补订阅就要处理"已经错过了第一次变化"。
// 轮询一个枚举、变了才动手，代价可以忽略，而且没有时序假设。
// ===========================================================================

#pragma once

#include "CoreMinimal.h"
#include "LOLPlayerController.h"
#include "GAS/ArenaTypes.h"
#include "ArenaPlayerController.generated.h"

class UArenaHUDController;
class UUserWidget;

/**
 * 一个相位要的输入形态。
 *
 * 【为什么是三态而不是 bool】备战阶段（原"奖励选择"）加入"自由移动"之后，
 * "要不要 UI 输入"不再能表达全部需求：备战既要能走路（游戏输入），
 * 又要能点三选一（UI 输入 + 光标）。二值开关只能表达"冻结 / 不冻结"，
 * 三态才能表达"冻结 / 走动但能用鼠标 / 交给界面"。
 */
UENUM(BlueprintType)
enum class EArenaInputIntent : uint8
{
	/** 纯游戏输入：光标收起，鼠标锁回视角（战斗 / 结算 / 等待）。 */
	GameOnly		UMETA(DisplayName = "游戏输入"),

	/** 游戏 + UI 并行：能走动，光标可见可点三选一（备战）。 */
	GameAndUI		UMETA(DisplayName = "游戏+界面"),

	/** 输入全部交给界面（大场结束后的结果屏）。 */
	UIOnly			UMETA(DisplayName = "仅界面"),
};

/**
 * 竞技场的本地玩家控制器。
 *
 * 【继承 ALOLPlayerController 而不是 APlayerController】HUD 是在基类的 BeginPlay 里
 * 建的（CreateHUDForLocalPlayer，由 IsLocalPlayerController 门住），换基类就没有 HUD 了。
 * 竞技场的界面（奖励三选一 / 比分 / 结果）是 HUD 那一层的事，
 * 这个类负责两件都是"只有本地玩家做得了"的事：
 *   ① "玩家的鼠标和键盘现在该不该被游戏接收"（相位 → 输入模式）；
 *   ② 把【对手】喂给英雄 HUD 的目标框（见 SyncObservedTarget）—— 目标框那条链在
 *      UHeroHUDController / UHeroTargetFrameWidget 里已经全通，缺的只是"看谁"这一个输入。
 *
 * 【它还负责建竞技场自己那套 HUD】UArenaHUDController（翻译层）+ WBP_ArenaHUD（根控件）。
 * 为什么不塞进基类那个 HUD：基类那套是"每个关卡都有的英雄 HUD"，
 * 而这一套只在竞技场里有意义，两者绑在一起会让别的关卡也去建一个竞技场翻译层。
 */
UCLASS()
class LOL_API AArenaPlayerController : public ALOLPlayerController
{
	GENERATED_BODY()

public:
	AArenaPlayerController();

	/**
	 * 奖励界面在本地玩家身上跑。
	 *
	 * 【只有本地玩家会进来】引擎的注释写得很清楚：PlayerTick 只在
	 * PlayerController 有 PlayerInput 对象时才调用（PlayerController.cpp:2308），
	 * 也就是"服务端上那些不是本机控制的 PC 不会走这里"。
	 * 所以这里不需要再自己查一遍 IsLocalPlayerController —— 想加也可以，
	 * 但别把它当成必要条件。
	 */
	virtual void PlayerTick(float DeltaTime) override;

	/**
	 * 可选：奖励界面想让键盘 / 手柄也能选的话，把自己的 widget 交过来当焦点。
	 *
	 * 【不调用也能用】FInputModeUIOnly 不给焦点的话鼠标照样能点
	 * （Slate 的命中测试和焦点是两件事），只是键盘导航没有落点。
	 * 三选一本来就是个点鼠标的界面，所以这一项是可选的；
	 * 但主机/手柄玩家要靠它，做了就顺手调一下。
	 * 传 nullptr 表示撤销（界面被收起来时调）。
	 */
	UFUNCTION(BlueprintCallable, Category = "Arena|Input")
	void SetScreenFocusWidget(UUserWidget* InWidget);

	/** 现在光标/界面有没有参与输入（备战 GameAndUI 与结束 UIOnly 都算）。UI 想画个"点击继续"的提示时可以读它。 */
	UFUNCTION(BlueprintPure, Category = "Arena|Input")
	bool IsUIInputActive() const { return CurrentIntent != EArenaInputIntent::GameOnly; }

	/**
	 * 竞技场 HUD 翻译层。Widget / 蓝图从这里拿。
	 * 只有本地玩家且已创建时才非空（理由同基类的 GetHUDController）。
	 */
	UFUNCTION(BlueprintPure, Category = "Arena|HUD")
	UArenaHUDController* GetArenaHUDController() const { return ArenaHUDController; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * 把【对手】接进英雄 HUD 的目标框（UHeroHUDController 的 Observed 通道）。
	 *
	 * 【为什么需要有人做这件事】目标框那条链本身是完整的：控制器有 SetObservedTarget、
	 * 属性订阅、销毁通知，WBP 那侧有 UHeroTargetFrameWidget 负责填名字 / 血 / 能量 / 死亡压暗
	 * —— 唯一缺的是【谁来指定"看谁"】。控制器自己不知道对手是谁（它连竞技场都不认识，
	 * 别的关卡也在用它），所以这条线只能由认识竞技场的人接：就是这里。
	 *
	 * 【为什么放在 PlayerController 而不是 UArenaHUDController】两点：
	 *   ① PlayerTick 只对本地玩家跑（引擎只给有 PlayerInput 的 PC 调），正是该做本地 HUD 的地方；
	 *      而对手的 PlayerState 是服务端复制过来的，远端 / 服务端 PC 上做这件事没有意义。
	 *   ② 目标框属于英雄 HUD，而英雄 HUD 的翻译层挂在 ALOLPlayerController 上 ——
	 *      从这里两头都摸得到（GetHUDController + GetPlayerState）。
	 *
	 * 【幂等】只在"目标真的换了"时才调 SetObservedTarget。逐帧调其实也安全
	 *（控制器自己会短路成"只刷新不重绑"），但拿它当前的 ObservedTarget 比一下更省。
	 * 对手的 pawn 每回合会被重新生成（回合开始时的 RestartPlayer 补生），指针一变就会重接 ——
	 * 这正是要的行为：目标框跟着新 pawn 走，而不是冻在上一具尸体上。
	 */
	void SyncObservedTarget();

	/**
	 * 把输入模式同步到当前相位上。没有相位变化时什么都不做。
	 *
	 * 【幂等是靠 CurrentIntent 保证的，不是靠相位比较】相位每次变化都会调它，
	 * 而 SetIgnoreMoveInput 加的是【计数器】—— 同一个方向连调两次就再也解不开了
	 * （计数停在 1），表现是"打完这一局玩家永远动不了"。所以只在自己那份状态
	 * 真的翻转时才去碰引擎。
	 */
	void SyncInputModeToPhase();

	/** 真正动手切模式的地方。 */
	void ApplyInputMode(EArenaInputIntent Intent);

private:
	/**
	 * 一个相位该要什么输入形态。见 EArenaInputIntent 的注释。
	 *
	 * 【为什么还要 bRewardSelectionDone】备战相位内部还有两段：正在选奖励（要点鼠标）
	 * 和已经选完在等倒计时（什么都不用点）。只看相位的话这两段没法区分，
	 * 光标会在选完之后一直杵在屏幕中间挡视角。这个 bool 来自
	 * AArenaPlayerState::IsRewardSelectionDone()，为真就说明这一相位已经没有待点的东西了 ——
	 * 三选一和锻造器菜单挂在同一条 prompt 链上，链走完 RewardStep 才变 None，
	 * 所以关掉光标不会把锻造器一起废掉。
	 */
	static EArenaInputIntent PhaseInputIntent(EArenaPhase Phase, bool bRewardSelectionDone);

	/** 建翻译层（幂等，只给本地玩家）。 */
	void CreateArenaHUDControllerForLocalPlayer();

	/** 建竞技场 HUD 根控件并加进视口（幂等，只给本地玩家）。 */
	void CreateArenaHUDForLocalPlayer();

	/**
	 * 竞技场 HUD 翻译层。只有本地玩家且已创建时才非空。
	 *
	 * 【Transient 而不是普通成员】它是个 UObject：不加 UPROPERTY 的话会被 GC 掉，
	 * 而它被回收之后所有订阅了它的 Widget 都不会报错，只会静静地再也不更新
	 *（弱引用变 null 不触发任何东西）—— 那种症状极难查。
	 */
	UPROPERTY(Transient)
	TObjectPtr<UArenaHUDController> ArenaHUDController;

	/** 竞技场 HUD 根控件（WBP_ArenaHUD）。 */
	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> ArenaHUDWidget;

	/** 竞技场 HUD 根控件类。在 BP_ArenaPlayerController 上配。留空 = 没有竞技场界面。 */
	UPROPERTY(EditDefaultsOnly, Category = "Arena|HUD")
	TSubclassOf<UUserWidget> ArenaHUDWidgetClass;

	/**
	 * 竞技场 HUD 的 ZOrder。
	 *
	 * 【必须比英雄 HUD 高】英雄 HUD 是基类用默认 ZOrder（0）加进视口的，
	 * 两者如果同层，谁在上面取决于添加顺序 —— 表现是"三选一按钮点不动"，
	 * 因为英雄 HUD 那层挡在前面把点击吃掉了。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Arena|HUD")
	int32 ArenaHUDZOrder = 10;

	/** 现在处于哪种输入形态。初始 GameOnly，和引擎默认一致（GameOnly + 无鼠标）。 */
	EArenaInputIntent CurrentIntent = EArenaInputIntent::GameOnly;

	/** 有没有真的调过 SetInputMode。没调过就别在 EndPlay 里瞎还原。 */
	bool bInputModeTouched = false;

	/** 界面希望谁拿焦点。弱引用：界面被销毁时不要吊着它。 */
	TWeakObjectPtr<UUserWidget> ScreenFocusWidget;
};
