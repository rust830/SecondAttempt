// 三选一卡片。设计意图全在头文件里。

#include "UI/ArenaChoiceCardWidget.h"

#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"

// 斗魂风格卡片底图的默认路径。可以在 WBP / 实例的 Arena|UI|Skin 分类下整体换皮。
namespace
{
	const TCHAR* const ArenaCardFramePath_Silver = TEXT("/Game/LOL/UI/Arena/Textures/T_ArenaCard_Silver.T_ArenaCard_Silver");
	const TCHAR* const ArenaCardFramePath_Gold = TEXT("/Game/LOL/UI/Arena/Textures/T_ArenaCard_Gold.T_ArenaCard_Gold");
	const TCHAR* const ArenaCardFramePath_Prismatic = TEXT("/Game/LOL/UI/Arena/Textures/T_ArenaCard_Prismatic.T_ArenaCard_Prismatic");
	const TCHAR* const ArenaCardFramePath_Finish = TEXT("/Game/LOL/UI/Arena/Textures/T_ArenaBtnFinish.T_ArenaBtnFinish");

	UTexture2D* LoadTierTexture(const TSoftObjectPtr<UTexture2D>& Soft, TObjectPtr<UTexture2D>& Cache)
	{
		if (Cache)
		{
			return Cache;
		}

		if (Soft.IsNull())
		{
			return nullptr;
		}

		// ApplyCard 是低频事件（待选变化时才来），LoadSynchronous 的代价可以接受；
		// 取到后缓存进 Transient 属性，之后连资产注册表都不用查。
		Cache = Soft.LoadSynchronous();
		return Cache;
	}
}

UArenaChoiceCardWidget::UArenaChoiceCardWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SilverFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(ArenaCardFramePath_Silver));
	GoldFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(ArenaCardFramePath_Gold));
	PrismaticFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(ArenaCardFramePath_Prismatic));
	FinishFrame = TSoftObjectPtr<UTexture2D>(FSoftObjectPath(ArenaCardFramePath_Finish));
}

UTexture2D* UArenaChoiceCardWidget::ResolveTierTexture(EArenaCardTier Tier)
{
	switch (Tier)
	{
	case EArenaCardTier::Silver:
		return LoadTierTexture(SilverFrame, CachedSilver);
	case EArenaCardTier::Gold:
		return LoadTierTexture(GoldFrame, CachedGold);
	case EArenaCardTier::Prismatic:
		return LoadTierTexture(PrismaticFrame, CachedPrismatic);
	case EArenaCardTier::None:
		return Card.bIsFinishAction ? LoadTierTexture(FinishFrame, CachedFinish) : LoadTierTexture(GoldFrame, CachedGold);
	}

	return nullptr;
}

void UArenaChoiceCardWidget::NativeConstruct()
{
	Super::NativeConstruct();

	// 设计器里一行逻辑都不要跑：否则每打开一次这个 WBP 就是一次空指针解引用。
	// （同 UHeroHUDWidget::NativeConstruct。）
	if (IsDesignTime())
	{
		return;
	}

	if (CardButton)
	{
		// 动态委托只能 AddDynamic 绑（UButton::OnClicked 是 BlueprintAssignable 的多播）。
		CardButton->OnClicked.AddDynamic(this, &UArenaChoiceCardWidget::HandleCardButtonClicked);
	}

	if (RerollButton)
	{
		RerollButton->OnClicked.AddDynamic(this, &UArenaChoiceCardWidget::HandleRerollButtonClicked);
	}
}

void UArenaChoiceCardWidget::NativeDestruct()
{
	if (RerollButton)
	{
		RerollButton->OnClicked.RemoveDynamic(this, &UArenaChoiceCardWidget::HandleRerollButtonClicked);
	}

	Super::NativeDestruct();
}

void UArenaChoiceCardWidget::HandleCardButtonClicked()
{
	// 没灌过视图就点不动 —— 但按钮可能是 enabled 的（比如 WBP 里默认就是这么配的）。
	// INDEX_NONE 报上去的话，服务端会拿它去查待选然后记一条 Warning，
	// 那种日志会盖住真正的问题，所以在这里就断掉。
	if (Card.OptionIndex == INDEX_NONE)
	{
		return;
	}

	OnCardClicked.Broadcast(Card.OptionIndex);
}

void UArenaChoiceCardWidget::HandleRerollButtonClicked()
{
	if (Card.OptionIndex == INDEX_NONE || !Card.bRerollable)
	{
		return;
	}

	OnRerollClicked.Broadcast(Card.OptionIndex);
}

void UArenaChoiceCardWidget::ApplyCard(const FArenaChoiceCardView& InCard)
{
	// 内容没变就不重画。第一次灌（Card 还是默认值，OptionIndex 是 INDEX_NONE）必然不等，
	// 所以"新建出来的卡片一定能被灌满"这件事不受影响。
	if (Card.EqualsForUI(InCard))
	{
		return;
	}

	Card = InCard;

	if (LabelText)
	{
		LabelText->SetText(Card.Label);
	}

	if (DescriptionText)
	{
		DescriptionText->SetText(Card.Description);
	}

	if (IconImage)
	{
		// bMatchSize = false：图标按 WBP 里摆好的尺寸画。
		// 传 true 的话它会用贴图自身的尺寸反过来改控件尺寸，把布局顶乱。
		IconImage->SetBrushFromTexture(Card.Icon, /*bMatchSize=*/false);

		// 没配图标的卡（合法情况）把图片收起来。留着的话是一个默认白色的方块，
		// 看起来像"图标加载失败"。
		IconImage->SetVisibility(Card.Icon
			? ESlateVisibility::HitTestInvisible
			: ESlateVisibility::Collapsed);
	}

	if (CardButton)
	{
		// 服务端会把不合法的选项拒掉并记 Warning（见 AArenaGameMode::ResolveChoice），
		// 所以"能不能点"在拿到视图时就已经定了 —— 按它设 disabled，
		// 玩家就不会点到一个没有反应的按钮。
		CardButton->SetIsEnabled(Card.bInteractable);
	}

	// 品质底图。没配 FrameImage 的 WBP 直接跳过 —— 底图是可选装饰。
	if (FrameImage)
	{
		if (UTexture2D* Frame = ResolveTierTexture(Card.Tier))
		{
			FrameImage->SetBrushFromTexture(Frame, /*bMatchSize=*/false);
			FrameImage->SetVisibility(ESlateVisibility::HitTestInvisible);
		}
		else
		{
			FrameImage->SetVisibility(ESlateVisibility::Collapsed);
		}
	}

	// 品质名角标。None 档没有品质可标 —— 收起来，别画一个空角标。
	if (TierText)
	{
		switch (Card.Tier)
		{
		case EArenaCardTier::Silver:
			TierText->SetText(NSLOCTEXT("ArenaHUD", "CardTierSilver", "白银"));
			TierText->SetVisibility(ESlateVisibility::HitTestInvisible);
			break;
		case EArenaCardTier::Gold:
			TierText->SetText(NSLOCTEXT("ArenaHUD", "CardTierGold", "黄金"));
			TierText->SetVisibility(ESlateVisibility::HitTestInvisible);
			break;
		case EArenaCardTier::Prismatic:
			TierText->SetText(NSLOCTEXT("ArenaHUD", "CardTierPrismatic", "棱彩"));
			TierText->SetVisibility(ESlateVisibility::HitTestInvisible);
			break;
		case EArenaCardTier::None:
		default:
			TierText->SetVisibility(ESlateVisibility::Collapsed);
			break;
		}
	}

	// 重随按钮：可重随且还有次数才亮；次数显示跟着整场共享的那个数走。
	if (RerollButton)
	{
		RerollButton->SetIsEnabled(Card.bRerollable && Card.RerollsLeft > 0);
		RerollButton->SetVisibility(Card.bRerollable
			? ESlateVisibility::Visible
			: ESlateVisibility::Collapsed);
	}

	if (RerollCountText)
	{
		RerollCountText->SetText(FText::Format(
			NSLOCTEXT("ArenaHUD", "CardRerollFmt", "重随 ({0})"),
			FText::AsNumber(FMath::Max(0, Card.RerollsLeft))));
	}

	// 放最后：让蓝图在"已经知道全部数据"之后再决定额外表现，不会先闪一下。
	BP_OnCardChanged(Card);
}
