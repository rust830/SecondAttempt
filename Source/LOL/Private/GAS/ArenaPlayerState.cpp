// 斗魂竞技场：参赛者 PlayerState 的实现。

#include "GAS/ArenaPlayerState.h"

#include "Engine/World.h"
#include "GAS/ArenaGameMode.h"
#include "GAS/ArenaLoadoutComponent.h"
#include "Net/UnrealNetwork.h"

AArenaPlayerState::AArenaPlayerState()
{
	// 装备栏挂在 PlayerState 上而不是 Pawn 上 —— 理由见 ArenaLoadoutComponent.h。
	Loadout = CreateDefaultSubobject<UArenaLoadoutComponent>(TEXT("ArenaLoadout"));

	// 【必须自己提频】APlayerState 的构造函数把 NetUpdateFrequency 设成 1 Hz ——
	// 那个值是给"进游戏时报一次名字"用的。这里不够，有两件事会直接被它拖坏：
	//   - PendingPrompt 是一次三选一：1 Hz 意味着发出去之后最多晚一秒才出现，
	//     AI 那边再等一两秒才作答，观感上像界面卡住了；
	//   - 对手的大场血量每回合都在变，1 Hz 会一跳一跳的（这条链在本模式里是新加的 ——
	//     以前 HUD 只绑自己的 ASC，所以 1 Hz 一直没暴露出来）。
	//
	// 10 Hz 是"看起来连续"的常用档位；下限设 2 是给带宽拥塞时的退化留个底，
	// 不设的话网络紧张时引擎可以把它压到很低。
	SetNetUpdateFrequency(10.f);
	SetMinNetUpdateFrequency(2.f);
}

void AArenaPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 全是公开信息（对手要看到你的血量和比分），所以都是 COND_None，
	// 不做 owner-only 优化。
	DOREPLIFETIME_CONDITION(AArenaPlayerState, MatchHealth, COND_None);
	DOREPLIFETIME_CONDITION(AArenaPlayerState, RoundsWon, COND_None);
	DOREPLIFETIME_CONDITION(AArenaPlayerState, RoundsLost, COND_None);
	DOREPLIFETIME_CONDITION(AArenaPlayerState, PendingPrompt, COND_None);
}

void AArenaPlayerState::BeginPlay()
{
	Super::BeginPlay();

	// 开局满血。放在 BeginPlay 而不是构造函数里，是因为 MaxMatchHealth 是
	// EditDefaultsOnly —— 蓝图里改大了上限，这里要跟着走，不能写死 100。
	// SetMatchHealth 自己会做幂等判断，所以默认 100/100 时这里什么都不发生。
	if (HasAuthority())
	{
		SetMatchHealth(MaxMatchHealth);
	}
}

// ---------------------------------------------------------------------------
// 大场血量
// ---------------------------------------------------------------------------

void AArenaPlayerState::SetMatchHealth(int32 NewHealth)
{
	// 大场血量是权威数据：客户端改它只会被下一次复制覆盖，
	// 表现就是"我这边显示赢了、别人那边没有"。所以挡掉并吵一声。
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] 非权威端调用了 SetMatchHealth(%d)，已忽略。大场血量只在服务端改。"),
			NewHealth);
		return;
	}

	// 夹到 [0, Max]：血量没有负数，也不该超过上限（超了血条会画到框外面去）。
	const int32 Clamped = FMath::Clamp(NewHealth, 0, MaxMatchHealth);
	if (Clamped == MatchHealth)
	{
		return;   // 幂等：没变就不广播
	}

	MatchHealth = Clamped;

	// 服务端不会收到自己的 OnRep_，所以这里显式广播一次，
	// 让两端走同一条路（先落到字段，再广播）。
	OnMatchStateChanged.Broadcast();
}

void AArenaPlayerState::ApplyMatchDamage(int32 Amount)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 ApplyMatchDamage，已忽略。"));
		return;
	}

	if (Amount <= 0)
	{
		// 需求里扣血是 15/30/40/50，永远是正的。传 0 或负数进来是调用方算错了，
		// 静默当 0 处理会让"这一回合白打了"看起来像正常结果。
		UE_LOG(LogTemp, Warning,
			TEXT("[Arena] ApplyMatchDamage 收到非正数 %d，已忽略。扣血量应该是正的。"),
			Amount);
		return;
	}

	SetMatchHealth(MatchHealth - Amount);
}

// ---------------------------------------------------------------------------
// 回合战绩
// ---------------------------------------------------------------------------

void AArenaPlayerState::RecordRoundResult(bool bWon)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 RecordRoundResult，已忽略。"));
		return;
	}

	if (bWon)
	{
		++RoundsWon;
	}
	else
	{
		++RoundsLost;
	}

	OnMatchStateChanged.Broadcast();
}

// ---------------------------------------------------------------------------
// 待选择
// ---------------------------------------------------------------------------

void AArenaPlayerState::SetPendingPrompt(const FArenaPendingPrompt& NewPrompt)
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 SetPendingPrompt，已忽略。"));
		return;
	}

	PendingPrompt = NewPrompt;
	OnPromptChanged.Broadcast();
}

void AArenaPlayerState::ResetRewardSelection()
{
	if (!HasAuthority())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Arena] 非权威端调用了 ResetRewardSelection，已忽略。"));
		return;
	}

	PendingPrompt = FArenaPendingPrompt();
	RewardStep = EArenaRewardStep::None;
	RemainingItemPicks = 0;
	PendingItemTier = EArenaItemTier::Legendary;

	OnPromptChanged.Broadcast();
}

void AArenaPlayerState::ServerSubmitChoice_Implementation(int32 OptionIndex)
{
	// Server RPC 的执行体必定在服务端。这里再查一次权威纯属防御 ——
	// 万一以后有人把这个函数体抽出去复用，漏了检查就是"客户端能发奖励"。
	if (!HasAuthority())
	{
		return;
	}

	if (AArenaGameMode* ArenaGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr)
	{
		ArenaGameMode->ResolveChoice(this, OptionIndex);
		return;
	}

	// 走到这里说明关卡用的不是 AArenaGameMode（比如忘了在 BP_ArenaGameMode 里
	// 换父类，或者 PIE 时开了别的关卡）。静默忽略的话，表现就是"点了三选一没反应"，
	// 所以要说清楚是 GameMode 不对。
	UE_LOG(LogTemp, Warning,
		TEXT("[Arena] %s 提交了选项 %d，但当前 GameMode 不是 AArenaGameMode —— 已忽略。"),
		*GetName(), OptionIndex);
}

void AArenaPlayerState::InitRerolls(int32 Charges)
{
	if (!HasAuthority())
	{
		return;
	}

	RerollsLeft = FMath::Max(0, Charges);
}

bool AArenaPlayerState::TryConsumeReroll()
{
	if (!HasAuthority())
	{
		return false;
	}

	if (RerollsLeft <= 0)
	{
		return false;
	}

	--RerollsLeft;
	return true;
}

void AArenaPlayerState::ServerRerollChoice_Implementation(int32 OptionIndex)
{
	// 同 ServerSubmitChoice：再查一次权威纯属防御。
	if (!HasAuthority())
	{
		return;
	}

	if (AArenaGameMode* ArenaGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AArenaGameMode>() : nullptr)
	{
		ArenaGameMode->RequestReroll(this, OptionIndex);
		return;
	}

	UE_LOG(LogTemp, Warning,
		TEXT("[Arena] %s 请求重随选项 %d，但当前 GameMode 不是 AArenaGameMode —— 已忽略。"),
		*GetName(), OptionIndex);
}

// ---------------------------------------------------------------------------
// OnRep
// ---------------------------------------------------------------------------

void AArenaPlayerState::OnRep_Prompt()
{
	// 只广播，不做任何加工 —— 加工会让"服务端算好的文案"变成"服务端算一半、客户端算一半"。
	OnPromptChanged.Broadcast();
}

void AArenaPlayerState::OnRep_MatchState()
{
	OnMatchStateChanged.Broadcast();
}
