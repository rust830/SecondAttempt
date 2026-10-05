// Fill out your copyright notice in the Description page of Project Settings.

#include "UI/HeroSkillSlotWidget.h"

#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TimerManager.h"

void UHeroSkillSlotWidget::SetSlotIndex(int32 InSlotIndex)
{
	SlotIndex = InSlotIndex;
}

void UHeroSkillSlotWidget::ApplySlotView(const FSkillSlotView& View)
{
	// 先留一份旧值：下面第一行就把 SlotView 覆盖了，而「上一帧是什么」决定了要不要播
	// 状态翻转事件 —— 这也是蓝图拿不到的、最容易让表现逻辑写错的那份信息。
	const FSkillSlotView Previous = SlotView;
	const bool bHadPrevious = bHasAppliedView;

	SlotView = View;
	bHasAppliedView = true;

	// ---------------------------------------------------------------------
	// 可见性：预留位（被动 / 格挡，配了 bHideWhenUnavailable 而能力还没授予）整格收起。
	//
	// 只在【第一次】记下 WBP 里配的可见性，恢复时回到它，而不是硬写 Visible ——
	// 否则会把美术在 WBP 里设的 HitTestInvisible 之类冲掉。
	//
	// 判据是 Controller 算好的 View.bHidden，这里不做第二次判断（同 ResolveSlotState 的理由）。
	// ---------------------------------------------------------------------
	if (!bAuthoredVisibilityCaptured)
	{
		AuthoredVisibility = GetVisibility();
		bAuthoredVisibilityCaptured = true;
	}
	SetVisibility(View.bHidden ? ESlateVisibility::Collapsed : AuthoredVisibility);

	if (IconImage && View.Icon)
	{
		// 只在有图标时设置：Icon 为空（还没绑上 ASC / 配置没填）时保留 WBP 里的占位图，
		// 不然加载期会先闪一下空白。
		IconImage->SetBrushFromTexture(View.Icon, /*bMatchSize=*/false);
	}

	if (KeyLabelText)
	{
		// 被动【按不出来】，所以不画键位：不管 WBP 模板里留了什么占位文字，一律清掉。
		// 主动 / 格挡走原来的规则 —— 只在 KeyLabel 非空时写，空的时候保留模板占位，
		// 免得到配置加载完之前先闪一下空白（见 IconImage 那条同款注释）。
		if (View.Kind == EHeroHUDSlotKind::Passive)
		{
			KeyLabelText->SetText(FText::GetEmpty());
		}
		else if (!View.KeyLabel.IsEmpty())
		{
			KeyLabelText->SetText(View.KeyLabel);
		}
	}

	// ---------------------------------------------------------------------
	// 转圈：判断条件是 ShouldShowCooldown()，不是 State == Cooled。
	//
	// 两者不等价，而且差别正好是最容易漏的那个：死亡时 State 是 Greyed，
	// 但冷却数字和转圈都要照常走（死亡和冷却是两条独立通道）。
	// ---------------------------------------------------------------------
	const bool bShowCooldown = View.ShouldShowCooldown();

	if (CooldownRing)
	{
		CooldownRing->SetVisibility(bShowCooldown ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowCooldown)
		{
			CooldownRing->SetPercent(View.CooldownPercent);
		}
	}

	// ---------------------------------------------------------------------
	// 扫描遮罩（LoL 式）。和 CooldownRing 二选一：WBP 里摆哪个就驱动哪个。
	//
	// Sweep = 1 - CooldownPercent：
	//   刚进 CD（percent=1）→ Sweep=0，整格全亮，扫描线刚从 12 点出发；
	//   过一半（percent=0.5）→ Sweep=0.5，遮掉半块；
	//   快结束（percent→0）→ Sweep→1，几乎全遮 —— 但那帧 bShowCooldown 已是 false，整块已隐藏。
	// ---------------------------------------------------------------------
	if (CooldownSweep)
	{
		CooldownSweep->SetVisibility(bShowCooldown ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (bShowCooldown)
		{
			if (!CooldownSweepMID)
			{
				CooldownSweepMID = CooldownSweep->GetDynamicMaterial();
			}
			if (CooldownSweepMID)
			{
				CooldownSweepMID->SetScalarParameterValue(TEXT("Sweep"), 1.0f - View.CooldownPercent);
			}
		}
	}

	if (CooldownText)
	{
		CooldownText->SetText(
			bShowCooldown && bShowCooldownText ? HeroHUD::FormatCooldownSeconds(View.CooldownRemaining) : FText::GetEmpty());
	}

	const bool bBlocked = View.State == ESkillSlotState::Greyed || View.State == ESkillSlotState::Disabled;

	if (GreyedOverlay)
	{
		GreyedOverlay->SetVisibility(bBlocked ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
	}

	if (IconImage)
	{
		IconImage->SetColorAndOpacity(bBlocked ? BlockedIconTint : ReadyIconTint);
	}

	// ---------------------------------------------------------------------
	// 边框：颜色 / 呼吸强度 / 冷却期的光带位置，全在状态这一条路上（没有常驻 tick）。
	//
	// 放在这里而不是并进上面的 bBlocked：bBlocked 只有「灰 / 不灰」两态，
	// 而边框想区分的是「冷却中」和「不可用」——那是三种颜色，比 bBlocked 细一档。
	// ---------------------------------------------------------------------
	ApplyFrameState(View);

	// CD 转好那一下闪一秒。
	//
	// 【判据为什么是 Cooled → Normal】只有这一种翻转意味着「冷却结束、技能回来了」——
	// 值得招一次眼。其他翻转（沉默结束、蓝够了、复活、刚出生第一次拿到视图）都只是
	// 「现在能放了」，本身没有时间含义，闪了反而会让人以为技能刚转好。
	//
	// bHadPrevious 一并卡掉「首次拿到视图」：那时 Previous 是默认值（Disabled / Cooled 都有可能），
	// 不卡的话开局会有槽位无条件闪一下。
	if (bHadPrevious && Previous.State == ESkillSlotState::Cooled && View.State == ESkillSlotState::Normal)
	{
		StartFrameFlash();
	}

	// ---------------------------------------------------------------------
	// 蓝图分流：语义变化 / 纯数字心跳 / 状态翻转，三条路各走各的。
	//
	// 【为什么不全塞进一个事件】30Hz 心跳期间只有一个事件的话，每 33ms 都会被调一次，
	// 蓝图里任何「闪一下」的表现都会变成每帧重播。分开之后：
	//   · 语义变化（图标、灰化、显隐…）→ BP_OnSlotViewChanged，频率等于游戏事件频率；
	//   · 只有数字动（心跳）→ BP_OnCooldownTick；
	//   · 状态翻转 → BP_OnSlotStateChanged，带旧值。
	//
	// 状态翻转和语义变化可能同时发生，两条都播 —— 它们的读者是两段不同的表现逻辑。
	// ---------------------------------------------------------------------
	if (!bHadPrevious || !View.EqualSemantics(Previous))
	{
		BP_OnSlotViewChanged(View);
	}
	else if (View.IsCoolingDown())
	{
		// 走到这里说明只有冷却数字变了。不在冷却还走到这里 = 什么都没变（EqualsForUI 挡过），
		// 不必播。
		BP_OnCooldownTick(View);
	}

	if (bHadPrevious && View.State != Previous.State)
	{
		BP_OnSlotStateChanged(Previous.State, View.State);
	}
}

// ===========================================================================
// 技能边框（M_SkillFrame）
//
// 【没有任何常驻的时间驱动】这一节里没有 NativeTick。动效只有两个来源：
//   · 冷却期：光带位置 = 冷却进度。搭在 Controller 已有的 30Hz 心跳上（冷却中就一定在跳），
//     所以对 HUD 来说是零额外成本 —— 心跳停了说明没槽在冷却，也就没有光带要画；
//   · CD 转好：一次性定时器闪 1 秒（约 30 次回调），播完自己停。
//
// 【呼吸为什么不在 C++】材质里已经有 Time + Sine 在做振荡，Pulse 对它来说是【强度】。
// C++ 再算一份包络就是双驱动：振幅会以两个频率一起抖，看起来很脏。
// ===========================================================================

void UHeroSkillSlotWidget::NativeDestruct()
{
	// 只是显式收尾。定时器绑的是 UObject，Widget 先死也不会回调到野指针上，
	// 但「留着一条没人管的定时器」在排查别的问题时是纯噪音。
	StopFrameFlash();

	Super::NativeDestruct();
}

UMaterialInstanceDynamic* UHeroSkillSlotWidget::EnsureSkillFrameMID()
{
	if (SkillFrameMID || !SkillFrameImage)
	{
		return SkillFrameMID;
	}

	// brush 不是材质（是贴图 / 空）时这里返回 nullptr —— 这是【静默失败】：
	// 边框照样画得出来（贴图直接显示），但下面所有参数都写不进去。
	// 所以「边框在、但改颜色没反应」第一个要查的就是 WBP 里这张 Image 的 Brush 类型。
	SkillFrameMID = SkillFrameImage->GetDynamicMaterial();
	if (!SkillFrameMID)
	{
		return nullptr;
	}

	// 外观常量只写一次（闪光收尾时会再调一次写回来）。
	WriteFrameAppearanceConstants();

	return SkillFrameMID;
}

void UHeroSkillSlotWidget::WriteFrameAppearanceConstants()
{
	if (!SkillFrameMID)
	{
		return;
	}

	// 想统一在 C++ 侧调美术就改上面那几个 UPROPERTY（默认值和材质一致）。
	SkillFrameMID->SetScalarParameterValue(TEXT("BorderWidth"), FrameBorderWidth);
	SkillFrameMID->SetScalarParameterValue(TEXT("CornerRadius"), FrameCornerRadius);
	SkillFrameMID->SetScalarParameterValue(TEXT("GlowIntensity"), FrameGlowIntensity);
	SkillFrameMID->SetScalarParameterValue(TEXT("GlowRange"), FrameGlowRange);
}

void UHeroSkillSlotWidget::ApplyFrameState(const FSkillSlotView& View)
{
	UMaterialInstanceDynamic* MID = EnsureSkillFrameMID();
	if (!MID)
	{
		return;
	}

	// 三种颜色覆盖四个状态：Greyed（沉默 / 蓝不够 / 死亡）和 Disabled（槽位空）共用「不可用」色。
	// 判据直接用 View.State，不在这里重新推一遍 —— 同 ResolveSlotState 的理由：优先级只有一处。
	FLinearColor RimColor = FrameReadyColor;
	float Pulse = FrameReadyPulse;

	switch (View.State)
	{
	case ESkillSlotState::Cooled:
		RimColor = FrameCooldownColor;
		Pulse = FrameCooldownPulse;
		break;

	case ESkillSlotState::Greyed:
	case ESkillSlotState::Disabled:
		RimColor = FrameBlockedColor;
		// 不可用的时候还在呼吸就是噪音：放不出来还一闪一闪地招人看，只会烦。
		Pulse = 0.f;
		break;

	default:
		// Normal：保持可用态的配置值。
		break;
	}

	// 存一份稳态颜色：闪光要以它做插值的起点（见 TickFrameFlash）。存了再让位，
	// 不然闪光期间来一次 ApplySlotView 会把起点冲成上一帧的颜色。
	FrameSteadyColor = RimColor;

	// 闪光正在播的时候，RimColor / Pulse / Sweep 归 TickFrameFlash 管 —— 这里写会把闪光冲掉。
	// 这不是假想情况：闪光那 1 秒里 ApplySlotView 完全可能被别的变化再调一次
	//（比如 Q 刚转好就被沉默、就死了，或者别的语义变化顺带走到这里）。
	// 真要撞上就让位：闪光只属于「可用」状态，状态一变就把它收掉。
	if (bFrameFlashing)
	{
		if (View.State != ESkillSlotState::Normal)
		{
			StopFrameFlash();
		}
		else
		{
			return;
		}
	}

	MID->SetVectorParameterValue(TEXT("RimColor"), RimColor);

	// 光带位置：冷却期跟着冷却进度走（1 = 刚进 CD，0 = 好了），其余状态停在 0。
	//
	// 【为什么这条不用额外 tick】冷却期 Controller 的 30Hz 心跳本来就在调 ApplySlotView
	//（见 UHeroHUDController::SampleCooldowns），光带位置顺手就更新了 ——
	// 心跳停 = 没有槽在冷却 = 也没有光带要画，两边天然对齐。
	float Sweep = 0.f;
	if (View.State == ESkillSlotState::Cooled && View.CooldownDuration > 0.f)
	{
		Sweep = 1.f - FMath::Clamp(View.CooldownPercent, 0.f, 1.f);
	}
	MID->SetScalarParameterValue(TEXT("Sweep"), Sweep);

	// Pulse 是【强度】，写常量就对了（振荡在材质里）。
	// 状态一变就覆盖一次，所以不会出现「已经灰了边框还亮着」的残留。
	MID->SetScalarParameterValue(TEXT("Pulse"), Pulse);
}

void UHeroSkillSlotWidget::StartFrameFlash()
{
	UMaterialInstanceDynamic* MID = EnsureSkillFrameMID();
	if (!MID || FrameFlashDuration <= 0.f || FrameFlashInterval <= 0.f)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 连着两次转好（比如连按两次技能）：让新的这次从头播，不叠加。
	World->GetTimerManager().ClearTimer(FrameFlashHandle);

	bFrameFlashing = true;
	FrameFlashElapsed = 0.f;

	// 立刻先写一帧：SetTimer 的首次回调在下一帧，中间那一帧会露出稳态值（Pulse=0），
	// 表现是「先暗一下再亮」—— 闪光的起手就丢了。
	TickFrameFlash();

	World->GetTimerManager().SetTimer(
		FrameFlashHandle, this, &UHeroSkillSlotWidget::TickFrameFlash, FrameFlashInterval, /*bLoop=*/true);
}

void UHeroSkillSlotWidget::TickFrameFlash()
{
	if (!SkillFrameMID)
	{
		// 材质没了（Widget 被回收之类）：直接收尾，别留个定时器空转。
		StopFrameFlash();
		return;
	}

	const float Duration = FMath::Max(FrameFlashDuration, KINDA_SMALL_NUMBER);
	const float Progress = FMath::Clamp(FrameFlashElapsed / Duration, 0.f, 1.f);

	// 包络：前 15% 快速拉起，之后平方衰减。
	// 【为什么不用对称的 sin】「CD 好了」是个事件，要的是快起慢落；
	// 对称曲线看起来像在呼吸，不像在提示。平方衰减则让尾巴收得干净 ——
	// 线性收尾会在最后一刻还剩一点亮度然后「啪」地断掉。
	constexpr float AttackRatio = 0.15f;
	float Envelope;
	if (Progress < AttackRatio)
	{
		Envelope = Progress / AttackRatio;
	}
	else
	{
		const float Decay = 1.f - (Progress - AttackRatio) / (1.f - AttackRatio);
		Envelope = Decay * Decay;
	}

	// 闪光期间把呼吸压到 0。
	//
	// 【Pulse=0 不是「关掉闪光」，恰恰是「最亮」】材质的最后一步是
	//   Emissive = (BorderColor + GlowColor) × Lerp(A=1.0, B=PulseMul, Alpha=Pulse)
	// Pulse=0 时那个 Lerp 恒等于 1；Pulse 越大只是把亮度拉向 PulseMul（0~1 的 1 秒正弦）。
	// 也就是说 Pulse 只会让边框变暗 + 抖，永远顶不过 1 —— 用它做闪光包络，
	// 峰值会落在正弦的波谷上（那一下反而变暗），转好那一秒看起来就是「闪得更乱」，之后归零反而最亮。
	//
	// 状态本来配了呼吸（FrameReadyPulse > 0）时也一起压掉：闪光是「清亮地响一下」，
	// 那 1 秒里不该有别的频率在抖。
	SkillFrameMID->SetScalarParameterValue(TEXT("Pulse"), 0.f);

	// 提亮走颜色：稳态色 → 峰值色，跟着包络走。材质里颜色没有振荡，一定是干净的快起慢落。
	SkillFrameMID->SetVectorParameterValue(TEXT("RimColor"), FMath::Lerp(FrameSteadyColor, FrameFlashColor, Envelope));

	// 外发光跟着一起张开 / 变亮 —— 这是闪光里【唯一裁不掉】的那部分：
	// GlowMask = exp(-max(SDF,0)/GlowRange) 是几何，把 GlowRange 从 0.05 推到 0.18
	// 会让一圈原本 alpha=0 的像素亮起来，LDR 裁切对它无效。
	// GlowIntensity 则是在衰减区（GlowMask 小、乘完仍 < 1）加亮 —— 颜色被裁满之后，亮度只能从这里出。
	SkillFrameMID->SetScalarParameterValue(
		TEXT("GlowRange"), FMath::Lerp(FrameGlowRange, FrameFlashGlowRange, Envelope));
	SkillFrameMID->SetScalarParameterValue(
		TEXT("GlowIntensity"), FMath::Lerp(FrameGlowIntensity, FrameFlashGlowIntensity, Envelope));

	// 光带在闪光这一秒里绕边框一圈（0 → 1），和包络同一个进度。
	// 材质里 BorderColor = RimColor × BorderMask × (1 + SweepBand × 2) ——
	// 光带扫过的那一段是 3 倍亮度，不过在主色已经过曝的地方会被裁掉，
	// 实际看得见的是它扫过衰减区、以及它压暗→提亮的对比。
	// 起点由 Custom 节点里的 atan2 公式决定（Sweep=0 那个角），不是固定的 12 点。
	SkillFrameMID->SetScalarParameterValue(TEXT("Sweep"), Progress);

	FrameFlashElapsed += FrameFlashInterval;

	if (FrameFlashElapsed > Duration)
	{
		// 稳态值由 StopFrameFlash 统一写回，这里不自己写。
		StopFrameFlash();
	}
}

void UHeroSkillSlotWidget::StopFrameFlash()
{
	if (!bFrameFlashing)
	{
		return;
	}

	// 先清标志再写状态：ApplyFrameState 开头有个「闪光让位」分支，
	// 顺序反了会变成 StopFrameFlash → ApplyFrameState → StopFrameFlash 的自我调用。
	bFrameFlashing = false;
	FrameFlashElapsed = 0.f;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(FrameFlashHandle);
	}

	// 闪光推高的 GlowIntensity / GlowRange 要先写回基准值 —— 中途被打断时（状态变了、
	// Widget 拆了）这些值会停在包络的中间，外发光就一直是张开的。
	WriteFrameAppearanceConstants();

	// 再把稳态值写回去。用当前 SlotView，不重新推导 —— 这里要的就是「这一帧本来的样子」。
	ApplyFrameState(SlotView);
}
