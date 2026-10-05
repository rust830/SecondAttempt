// 单个技能槽：图标 / 冷却转圈 / 剩余秒数 / 灰化。
//
// 头文件零 GAS include。它只认两样东西：SlotIndex（第几个槽）和 FSkillSlotView（怎么显示）。
// 图标的来源、冷却的秒数从哪算出来、为什么灰 —— 一概不知道，也不该知道。
//
// 一条容易写错的规则：转圈的开关是 ShouldShowCooldown()（= 标签说在冷却 + 有总时长），
// 【不是】State == Cooled。两者不等价：死亡时 State 是 Greyed，但冷却数字照常要走。

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UI/HUDTypes.h"
#include "HeroSkillSlotWidget.generated.h"

class UImage;
class UMaterialInstanceDynamic;
class UProgressBar;
class UTextBlock;

UCLASS(Abstract)
class LOL_API UHeroSkillSlotWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** HUD 建完槽之后调一次，告诉它自己是第几个。 */
	UFUNCTION(BlueprintCallable, Category = "HUD")
	void SetSlotIndex(int32 InSlotIndex);

	UFUNCTION(BlueprintPure, Category = "HUD")
	int32 GetSlotIndex() const { return SlotIndex; }

	/** 当前视图。返回拷贝而不是 const& —— UFUNCTION 的返回值必须是值类型。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	FSkillSlotView GetSlotView() const { return SlotView; }

	/** 冷却秒数的显示文案。口径统一在 HeroHUD::FormatCooldownSeconds，别在蓝图里另写一套。 */
	UFUNCTION(BlueprintPure, Category = "HUD")
	FText GetCooldownLabel() const { return HeroHUD::FormatCooldownSeconds(SlotView.CooldownRemaining); }

	/**
	 * 视图发生了【冷数字以外】的变化：图标 / 键位 / 状态 / 灰化原因 / 显隐 / 充能。
	 *
	 * 这条是给「状态驱动」的表现用的（可用时压一下、不可用时抖一下…）。
	 * 它【不会】被 30Hz 心跳带着每帧重播 —— 只有数字在动的时候走下面那条。
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnSlotViewChanged(const FSkillSlotView& View);

	/**
	 * 只有冷却数字在动（30Hz 心跳）。
	 *
	 * 【和上一条拆开的原因】原来只有一个事件，冷却期间每 33ms 都会被调一次，
	 * 于是在里面写「CD 结束闪一下」就变成每帧闪一次 —— 表现被心跳带着走，
	 * 而且蓝图拿不到「上一帧是什么」，只能自己再存一份状态。
	 *
	 * 想画转圈 / 扫描条 / 秒数的额外动画都挂这条；不需要就留空，空实现不产生开销。
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnCooldownTick(const FSkillSlotView& View);

	/**
	 * 可用性状态翻转（Normal ⇄ Cooled ⇄ Greyed ⇄ Disabled 之间）。
	 *
	 * 【带旧值】：视图里只有新状态，而做「CD 好了闪一下」「刚被沉默压暗」这类表现
	 * 必须知道从哪来 —— 所以旧值在这里递给你，蓝图不用自己存一份上一帧。
	 *
	 * 首次拿到视图时不播（没有「上一个状态」可言，播了会在开局闪一下）。
	 */
	UFUNCTION(BlueprintImplementableEvent, Category = "HUD")
	void BP_OnSlotStateChanged(ESkillSlotState OldState, ESkillSlotState NewState);

	/** HUD 调这个。全量覆盖，不做增量 —— 增量是 bug 的温床，而这里的数据量小到没必要。 */
	void ApplySlotView(const FSkillSlotView& View);

protected:
	/** 只为了在 Widget 被销毁时停掉闪光定时器（虽然定时器绑的是 UObject，但显式收尾不靠猜）。 */
	virtual void NativeDestruct() override;

	// ---- 可选绑定：名字对上就自动接 ----

	/** 冷却转圈。用 radial fill 材质，Percent 直接喂给它（1 = 刚进 CD，0 = 好了）。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UProgressBar> CooldownRing;

	/**
	 * 冷却扫描遮罩（LoL 式：方形图标上一条线从 12 点顺时针扫过，扫过处变暗）。
	 *
	 * 用 Image + 自定义 radial-sweep 材质替代 ProgressBar 转圈时摆这个控件。
	 * C++ 只负责喂 Sweep 标量参数（0=全亮，1=全遮）和控制显隐，
	 * 方向 / 颜色 / 透明度是材质里的事（见 M_CooldownSweep 的 Custom 节点）。
	 * 和 CooldownRing 是【二选一】：WBP 里摆哪个 C++ 就驱动哪个，两个都摆会叠加显示。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> CooldownSweep;

	/** 剩余秒数文本。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> CooldownText;

	/** 键位标注（Q / W / E / R / D / F），文本来自映射表。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UTextBlock> KeyLabelText;

	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> IconImage;

	/** 灰化遮罩（半透明黑）。只在 Greyed / Disabled 时显示。 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> GreyedOverlay;

	/**
	 * 技能边框（材质 M_SkillFrame）。
	 *
	 * 【为什么边框必须是独立控件，不能复用 IconImage】IconImage 是 C++ 的图标通道：
	 * 每次 ApplySlotView 都 SetBrushFromTexture(View.Icon) 把 brush 的资源对象整个换掉，
	 * 而且 SetColorAndOpacity 会连边框一起压暗 —— 边框画在上面一定会被技能图标顶掉。
	 * 这里这张是 C++【只认参数、不碰 brush】的独立层。
	 *
	 * 摆位：IconImage 之上、CooldownSweep / 文字之下。名字必须是 SkillFrameImage。
	 *
	 * 坑：这张 Image 的 Brush 必须是【材质】M_SkillFrame（不是贴图）。
	 * UImage::GetDynamicMaterial() 在 brush 是贴图 / 空的时候返回 nullptr，
	 * 参数会全部静默写不进去 —— 表现是「边框在，但改颜色没反应」。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "HUD", meta = (BindWidgetOptional))
	TObjectPtr<UImage> SkillFrameImage;

	// ---- 表现参数 ----

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FLinearColor ReadyIconTint = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	FLinearColor BlockedIconTint = FLinearColor(0.35f, 0.35f, 0.35f, 1.f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD")
	bool bShowCooldownText = true;

	// -----------------------------------------------------------------------
	// 技能边框材质参数（M_SkillFrame）
	//
	// 【参数名大小写敏感】：SetScalarParameterValue / SetVectorParameterValue 是按字符串查参数的，
	// 写错一个字符不会报错、只是默默写不进去 —— 表现是「改了没反应」。
	// 名字以材质为准，下面这些常量【不允许】改名。
	//
	// 【分工：时间域归材质，状态域归 C++】
	//   · 材质（M_SkillFrame）自己用 Time + Sine 做呼吸振荡 —— 这是它已经在做的事，C++ 不插手；
	//   · C++ 只在两个时机写 Sweep：冷却期跟着冷却进度（搭已有的 30Hz 心跳）、CD 转好闪一秒（一次性定时器）。
	//     Custom 节点里 `band = 1 - saturate(abs(ang - Sweep)/0.04)` 直接吃这个值当流光的角度位置，
	//     材质【不会】自己动它（动了两边就是叠加相位，流光会变快一倍）。
	//   · 其余参数都是状态量：换状态时写一次。
	//
	// 【没有 NativeTick】边框不常驻每帧驱动，这是刻意的 —— 理由见下面「CD 转好的一次性闪光」。
	// -----------------------------------------------------------------------

	/**
	 * 边框主色：可用。
	 *
	 * 【想要闪光更狠就压这个值】主色会被 ×(1+光带×2) 和 ×GlowIntensity 乘，乘完超过 1 的部分
	 * 全部裁掉。默认值乘完已经过曝，所以「冷却色 vs 可用色」的区别其实也看不见（都被裁成白）。
	 * 把它压到三成左右（比如 (0.06,0.24,0.3)），静止的边框暗一档，换色的区别和闪光的余量就都回来了。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	FLinearColor FrameReadyColor = FLinearColor(0.2f, 0.8f, 1.0f, 1.f);

	/** 边框主色：冷却中。裁切的事见 FrameReadyColor 的注释 —— 这条同理。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	FLinearColor FrameCooldownColor = FLinearColor(0.25f, 0.45f, 0.9f, 1.f);

	/**
	 * 边框主色：不可用。Greyed（沉默 / 蓝不够 / 死亡）和 Disabled（槽位空）共用。
	 * 空槽本来就不该抢注意力，不值得再多一个颜色。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	FLinearColor FrameBlockedColor = FLinearColor(0.35f, 0.35f, 0.35f, 1.f);

	// 下面四个平时是外观常量（MID 建好时写一次、闪光收尾时再写回来）。
	// 默认值和材质里的默认值一致 —— 想在 C++ 侧统一调美术就改这里，不想动就留着。
	// GlowIntensity / GlowRange 例外：闪光期间它们会被临时推高（见 FrameFlashGlowRange）。

	/** 边框宽度（UV 空间）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameBorderWidth = 3.f;

	/** 圆角半径（UV 空间）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameCornerRadius = 0.1f;

	/** 外发光强度。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameGlowIntensity = 2.f;

	/** 外发光衰减范围（UV 空间）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameGlowRange = 0.05f;

	/**
	 * 呼吸强度：可用时。**默认 0 = 不呼吸**。
	 *
	 * 【Pulse 在材质里到底是什么】(M_SkillFrame 的最后一步)
	 *   PulseMul   = sin(Time × 6.28318) × 0.5 + 0.5   // 0~1 正弦，周期正好 1 秒
	 *   FinalPulse = Lerp(A=1.0, B=PulseMul, Alpha=Pulse)
	 *   Emissive   = (BorderColor + GlowColor) × FinalPulse
	 *
	 * 于是有两个必须记住的推论：
	 *   ① Pulse = 0 → FinalPulse ≡ 1（最亮且静止）；Pulse 越大只是把亮度往 PulseMul 拉，
	 *      也就是【只可能变暗 + 变成 1 秒一次的正弦抖动】—— 它永远顶不过 1。
	 *      所以「让边框亮一下」这件事 Pulse 做不到，只能靠颜色（RimColor）或光带（SweepBand）。
	 *   ② 想让边框静止，就必须把它设成 0，没有第二个办法。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameReadyPulse = 0.f;

	/**
	 * 呼吸强度：冷却中。**默认 0**（同上：只要 > 0，材质那 1 秒一次的正弦就会让边框一直抖）。
	 * 冷却期的存在感靠 FrameCooldownColor 的颜色 + 跟着进度走的光带，不靠抖。
	 * 想要冷却期也呼吸，再把它调上去。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameCooldownPulse = 0.f;

	// -----------------------------------------------------------------------
	// CD 转好的一次性闪光
	//
	// 【为什么不做常驻流光】常驻流光必须每帧推 Sweep（要靠 NativeTick），六个槽位一直在跑，
	// 而「CD 好了」这个信息其实只需要在发生的那一瞬间说一次。于是拆成两半：
	//   · 冷却期：Sweep 跟着冷却进度走 —— 搭在【已有的 30Hz 心跳】上，零额外成本；
	//   · 转好那一下：起一个 1 秒的定时器，边框提亮到 FrameFlashColor + 光带绕一圈 —— 总共只跑约 30 次。
	//     【为什么不是拉 Pulse】：材质的最后一步是 Lerp(A=1.0, B=PulseMul, Alpha=Pulse)，
	//      Pulse 只能把亮度往下压（连带的 1 秒一次正弦就是「冷却期一直在闪」的来源），
	//      而转好时 Pulse=0 已经是全亮 —— 想要更亮只能走颜色和光带。详见 FrameReadyPulse 的注释。
	// 两半加起来都没有常驻 tick。这条路上的「时间」只有闪光那 1 秒存在。
	// -----------------------------------------------------------------------

	/**
	 * 闪光峰值【颜色】。包络走到峰值时 RimColor 插值到这里。
	 *
	 * 【为什么闪光只能走颜色 + 外发光，不能靠提亮】
	 *   ① Pulse 只会把 Emissive 往 0 压（Lerp 的 A 端就是 1.0），转好时 Pulse=0 已经是全亮；
	 *   ② 颜色也没有余量：GlowColor = RimColor × GlowMask × GlowIntensity(=2)，
	 *      蓝通道 1.0×2 就已经 > 1 —— UMG 走 LDR，超过 1 一路裁到 1，
	 *      所以边框现在整圈是【过曝的白】，把 RimColor 提到白只有红通道 0.6→1 的差别（几乎看不见），
	 *      光带的 ×3 更是整个被裁掉。
	 *   于是剩下能看见的只有：色相（衰减区那些没被裁的像素）+ 几何（FrameFlashGlowRange）。
	 *
	 * 默认纯白而不是 > 1 的 HDR 色：反正会被裁，写 1.2 和写 1 一样。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	FLinearColor FrameFlashColor = FLinearColor::White;

	/**
	 * 闪光峰值：外发光衰减范围（GlowRange）。**这是闪光里唯一裁不掉的那一维** ——
	 * GlowMask = exp(-max(SDF,0)/GlowRange)，把它从 0.05 拉到 0.18 是【几何变化】：
	 * 一圈原本 alpha=0 的像素变得有颜色，亮度裁不裁都看得见。
	 * 表现就是外发光「张开再收回」，配合主色转白和光带绕圈。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameFlashGlowRange = 0.18f;

	/**
	 * 闪光峰值：外发光强度。GlowIntensity 是纯乘数，衰减区（GlowMask 很小）那些像素
	 * 乘完仍然 < 1 —— 所以推高它是在【没被裁的那部分】上加亮，能看见。
	 * 换成"更亮"的观感主要靠它，或者把静止色压暗（那样余量更大，见 FrameReadyColor 的注）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameFlashGlowIntensity = 4.f;

	/** 闪光时长（秒）。<= 0 = 不闪。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameFlashDuration = 1.f;

	/** 闪光的刷新间隔（秒）。默认 ≈ 30Hz，和冷却心跳一个量级（1 秒 30 次）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "HUD|Frame")
	float FrameFlashInterval = 0.033f;

	// ---- 当前视图（蓝图只读） ----

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	FSkillSlotView SlotView;

	UPROPERTY(BlueprintReadOnly, Category = "HUD")
	int32 SlotIndex = INDEX_NONE;

	/** CooldownSweep 的动态材质实例缓存（每次 SetScalarParameterValue 前 GetDynamicMaterial 太浪费）。 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CooldownSweepMID;

	// ---- 技能边框（M_SkillFrame）----

	/** 边框 MID 的懒创建 + 外观常量的一次性写入。brush 不是材质时返回 nullptr（静默失败的那个坑）。 */
	UMaterialInstanceDynamic* EnsureSkillFrameMID();

	/**
	 * 写外观常量（BorderWidth / CornerRadius / GlowIntensity / GlowRange）。
	 * MID 建好时写一次，闪光收尾时再写回来 —— GlowIntensity / GlowRange 会被闪光临时推高，
	 * 不写回来就会留在闪光的峰值上（中途被状态变化打断时尤其明显）。
	 */
	void WriteFrameAppearanceConstants();

	/** 边框：按状态写 RimColor / 呼吸强度 / 冷却期光带位置 / 外观常量初始化。 */
	void ApplyFrameState(const FSkillSlotView& View);

	/** 起一次 CD 转好的闪光（从 Cooled 翻到 Normal 时调）。 */
	void StartFrameFlash();

	/** 闪光定时器的回调：推进包络 + 让光带绕一圈；走完自己收尾。 */
	void TickFrameFlash();

	/** 收掉闪光并把稳态值写回去（中途被别的状态推翻时也要调）。 */
	void StopFrameFlash();

	/** SkillFrameImage 的动态材质实例缓存（和 CooldownSweepMID 同一个理由）。 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> SkillFrameMID;

	/**
	 * 闪光是否正在播。它同时是「RimColor / Pulse / Sweep 现在归谁管」的开关：
	 * 为 true 时 ApplyFrameState 不写这三个参数，免得把闪光冲掉。
	 */
	bool bFrameFlashing = false;

	/**
	 * 当前状态的稳态颜色（ApplyFrameState 每次算出来存一份）。
	 *
	 * 闪光要从稳态色插值到峰值再插回来，而闪光期间 ApplyFrameState 是不写参数的 ——
	 * 不存这份，闪光就不知道自己该从哪个颜色起步。
	 */
	FLinearColor FrameSteadyColor = FLinearColor::White;

	/** 闪光已经播了多久（秒）。 */
	float FrameFlashElapsed = 0.f;

	/** 闪光定时器。只在闪光那 1 秒里存在，平时是空的。 */
	FTimerHandle FrameFlashHandle;

	/**
	 * WBP 里配的可见性（首次 ApplySlotView 时记下来）。
	 *
	 * 为什么记而不是写死 Visible：`bHidden` 只负责「收起来」，恢复时要回到美术在 WBP 里
	 * 配的那个值 —— 他可能故意设成 HitTestInvisible / SelfHitTestInvisible。
	 * 直接写 Visible 会把设计意图冲掉，而且这种冲突只在运行时看得见。
	 */
	ESlateVisibility AuthoredVisibility = ESlateVisibility::Visible;

	bool bAuthoredVisibilityCaptured = false;

	/**
	 * 上一份视图是否已经存在。
	 * 用它区分「第一次拿到状态」和「从某个状态翻转过来」—— 首次不该播状态翻转事件，
	 * 否则开局会闪一下（默认值 Disabled → 真实状态 被当成一次翻转）。
	 */
	bool bHasAppliedView = false;
};
