# GAS → UMG HUD 数据管道 实施方案

## 0. 一句话方案

**真值只存在于 PlayerState 上的 ASC + `UHeroCombatAttributeSet`；`UHeroHUDController` 是唯一的翻译层，把「属性变化事件 / 标签计数事件」翻成 UI 语义（百分比 / 剩余秒数 / 灰化旗标）后广播；WBP 只做显示，头文件里连 `GameplayTagContainer.h` 都不出现。**

```
 UMyPlayerState  ── 单一真值源（唯一有状态的地方）
 ├─ UMyAbilitySystemComponent      ← 技能规格 / 冷却 GE / 标签容器
 └─ UHeroCombatAttributeSet        ← Health / MaxHealth / Energy / MaxEnergy / AbilityHaste
                 │
                 │  属性变化委托 GetGameplayAttributeValueChangeDelegate
                 │  标签计数事件 RegisterGameplayTagEvent(NewOrRemoved)
                 ▼
 UHeroHUDController（UObject，PC 的子对象）  ── 唯一的 GAS 语义出口
 │   · 翻译：float 秒 → 百分比；标签计数 → ESkillSlotState；Health → 0..1
 │   · 心跳：单个 30Hz 采样 timer（只在有槽在冷却时跑）
 │   · 快照：缓存一份当前完整状态，供新 Widget 拉取
 │
 │  BlueprintAssignable（签名里没有任何 GAS 类型）
 ▼
 WBP_HUD / WBP_SkillSlot / WBP_HealthBar        ── 零 GAS 依赖，只认 int32 槽位序号 + 表现数据
```

和格挡/隐身那套同构：**状态是标签（唯一事实来源），中间层只做翻译，表现层只做显示**。
多出来的一条纪律：**UI 层不许自己算状态** —— 不许 Tick 里调 `CanActivateAbility`，不许用 `Health == 0` 判死亡，不许自己递减冷却。

---

## 1. 你的六条硬约束：全部保留，其中两条变成"可执行判据"

先把结论放前面：你列的硬约束**一条都不改**。我只把其中两条从"注意事项"升级成"看一眼代码就知道有没有违反"的形状。

| 你的约束 | 落地成什么 | 怎么一眼验证 |
|---|---|---|
| 数据源用 `GetGameplayAttributeValueChangeDelegate`，不用 `OnRep_*` | `UHeroHUDController` 里所有属性绑定都走 `ASC->GetGameplayAttributeValueChangeDelegate(Getter())` | grep `OnRep_` 在 `HeroHUDController.cpp` 里应该**零命中** |
| 初始值必须显式给 | `PullHUDState()`（拉） + `BroadcastInitialValues()`（推），两者共用同一份快照 | 任何 Widget 的 `NativeConstruct` 里必须有 `PullHUDState()` |
| 绑定幂等 `TryBindHUD()`，挂三处 | 幂等键是"我现在绑的是不是**同一个 ASC**"，不是"我绑过没有" | 见 §3.1 |
| 只给本地玩家建 HUD | `APlayerController::IsLocalController()` 门住 Controller 和 Widget 的创建 | 门开在 PC，不在 Character |
| 冷却走 `Spec->Ability->GetCooldownTimeRemainingAndDuration()` | 封装成 `QueryCooldown(SlotIndex, OutRemaining, OutDuration)` | 这是唯一读冷却数值的地方 |
| 剩余秒数不复刻 | timer **只采样，不递减** | 见 §3.3，这是最容易写错的一条 |
| 标签是唯一真相 | 状态翻转只认标签 0→1 / 1→0；数字可以回弹，**状态不可以** | 见 §3.4 |
| 急速不重算 | Duration 直接用查询回来的值，不做任何 `× 100/(100+Haste)` | grep 不到 `AbilityHaste` 出现在 `HeroHUDController.cpp` 之外的翻译里 |
| 灰化不在 Tick 里调 `CanActivateAbility` | 用「属性事件 + 标签事件」合成旗标 | `HeroHUDController` 里不应出现 `CanActivateAbility` |
| Widget 头文件零 GAS include | 委托签名全用 `UI/HUDTypes.h` 里的 UI 语义类型 | grep 不到 `AbilitySystemComponent.h` / `GameplayTagContainer.h` 在 `UI/` 下 |

> **`LOL.Build.cs` 不用动。** `UMG` 和 `Slate` 已经在 `PublicDependencyModuleNames` 里了。

---

## 2. 我建议改的三处（架构层面）

你的方案里有三处会在第 2~4 步开始咬人，现在改成本最低。

### 2.1 一个共享心跳，不是六个 timer

你写的是"本地 30Hz timer 跑数字，有冷却才启、全好了停"。**如果一个槽一个 timer，每个 timer 的相位都不同**：同一帧里 Q 的秒数在 0.033s 更新、W 的在 0.066s 更新，视觉上是技能栏在"挨个跳"。而且"全好了就停"这个判断要写六遍。

改成：**`UHeroHUDController` 持有一个 `FTimerHandle CooldownTickHandle`**，由 `UpdateCooldownTicker()` 单点开关 —— 任一槽 `CooldownRemaining > 0` 或 `Recharging` 就 `SetTimer(..., 1/30.f, /*bLoop*/ true)`；全部归零就 `ClearTimer`。六条槽天然同帧，开关判断只有一处。

### 2.2 "订阅即拉取"取代"记得调 BroadcastInitialValues()"

你的约束是对的（绑定不触发初始值），但它依赖"记得在正确时机调"。而 **Widget 的创建时机和 Controller 的绑定时机是两条独立的时间轴**：`BroadcastInitialValues()` 跑在 Widget 构造之前的那一次就丢了；而 `AcknowledgePossession` 之后晚绑定的那次又拿不到。

做法：**`UHeroHUDController` 缓存一份 `FHUDSnapshot`（"当前完整状态"），并且把它当公共 API 暴露：**

```cpp
/** 拉：任意时刻、任意订阅者，调一次就拿全量当前状态。Widget 的 NativeConstruct 里调这个。 */
UFUNCTION(BlueprintCallable, Category="HUD") FHUDSnapshot PullHUDState() const;

/** 推：给"已经在场的订阅者"重放一遍全量。绑定成功 / 状态大变（换 PS、进化完成）时调。 */
UFUNCTION(BlueprintCallable, Category="HUD") void BroadcastInitialValues();
```

于是三件事一起解决：初始值不再靠"时机"、Widget 重建（切关卡后重新 `AddToViewport`）自动正确、`BroadcastInitialValues()` 退化成 `PullHUDState()` 的分发壳（语义不丢，实现只有一份）。

> **最小版本**：如果你觉得引入快照结构太重 —— 至少保留 `PullHUDState()` 这一个入口，别让"初始值"只能靠广播到达。

### 2.3 目标框需要第二条数据通道，你的架构里没有

HUDController 现在是**本地玩家自身**的翻译层。但目标框要的是**别人**的血。这两条通道的生命周期完全不同（自身跟随 PS，目标跟随鼠标选取、随时变、随时没）。

加一条 **Observed 通道**，模式和 `TryBindHUD` 一模一样：

```cpp
/** 幂等：先解绑旧目标、再绑新目标。传 nullptr = 收起目标框。 */
UFUNCTION(BlueprintCallable, Category="HUD") void SetObservedTarget(AActor* NewTarget);

/** 目标框全量变化（含"有没有目标"）。 */
UPROPERTY(BlueprintAssignable, Category="HUD") FOnTargetFrameChangedSignature OnTargetFrameChanged;
```

解绑一定要"先摘后挂"：目标被销毁、切到新目标、切回 `nullptr`，三种走的是同一条路径（`FDelegateHandle` 存一份，`UnbindObserved()` 幂等）。

**敌人头顶血条（Phase 5）不要走 HUDController。** 它是"每个敌人一个 Widget"，不是"本地玩家一个"。走 `UWidgetComponent` + 一个轻量 `UHeroOverlayHealthWidget`，在 `SetOwnerActor` 时绑目标 ASC 的 Health/MaxHealth 属性委托、`NativeDestruct` 时解绑。HUDController 在这件事里只负责一件事：**告诉本地玩家"你现在的目标是谁"**，把 `AActor*` 给出去就行了。

---

## 3. 关键设计点

### 3.1 `TryBindHUD()` 的幂等键必须是"ASC 身份"，不是 bool

三处调用点（PC 的 `BeginPlay` / `OnRep_PlayerState` / `AcknowledgePossession`）里，**后两处可能在 PS 被替换之后才跑**。用 `bool bBound` 的话，第二次进来会看到"绑过了"直接返回，而实际上绑在那个已经被销毁的 ASC 上 —— 表现是血条永远停在上一命的值。

```cpp
/** 当前绑着的那个 ASC。换 PS / 换 Pawn 时它会变，这就是幂等键。 */
UPROPERTY(Transient) TWeakObjectPtr<UMyAbilitySystemComponent> BoundASC;

void UHeroHUDController::TryBindHUD()
{
    UMyAbilitySystemComponent* ASC = ResolveASC();     // 从 OwnerPC → Pawn → PlayerState

    if (ASC == BoundASC.Get())
    {
        BroadcastInitialValues();                      // 同一个 ASC，只补一次初始值，不重复绑
        return;
    }

    UnbindAll();                                       // 换人了：标签事件 + 属性委托 + 目标框全摘
    BoundASC = ASC;

    if (!ASC) { BroadcastEmptyState(); return; }        // 绑不上 → 广播"空"，不要留旧值

    // ① 属性：血 / 能量
    ASC->GetGameplayAttributeValueChangeDelegate(UHeroCombatAttributeSet::GetHealthAttribute())
       .AddUObject(this, &UHeroHUDController::OnHealthAttributeChanged);
    // ... MaxHealth / Energy / MaxEnergy 同构

    // ② 标签：每个槽的冷却标签，NewOrRemoved = 只在 0↔1 触发（这就是"当开关"）
    for (const FHeroHUDSlotEntry& Entry : SlotConfig->Slots)
    {
        if (Entry.CooldownTag.IsValid())
        {
            TagHandles.Add(ASC->RegisterGameplayTagEvent(Entry.CooldownTag, EGameplayTagEventType::NewOrRemoved)
                               .AddUObject(this, &UHeroHUDController::OnCooldownTagChanged, Entry.SlotIndex));
        }
    }
    // ... State.Dead / State.Stunned / State.Silenced 各一个

    BroadcastInitialValues();
}
```

> **绑不上时广播"空快照"，不要留着上次的值。** 绑不上的窗口期（PS 还没到、ASC 还没 `InitAbilityActorInfo`）显示"血满、技能全灰、无目标"，比显示上一命的残值好得多。

`UnbindAll()` 用一个 `TArray<FDelegateHandle> TagHandles` + 属性委托句柄各一个，集中摘。**别把解绑散在各处** —— 漏一个就是"PS 换了但还收到旧 ASC 的事件"，症状极难查。

### 3.2 标签事件用 `NewOrRemoved`，正是"开关"语义

```cpp
ASC->RegisterGameplayTagEvent(Tag, EGameplayTagEventType::NewOrRemoved)
   .AddUObject(this, &UHeroHUDController::OnCooldownTagChanged, /*SlotIndex*/ 0);
```

`NewOrRemoved` 只在计数 0→1 和 1→0 时触发，中间的 1→2、2→1 不触发 —— 恰好就是你要的"开关"。回调签名是 `void(FGameplayTag, int32 NewCount)`，`NewCount > 0` = 进冷却，`== 0` = 冷却结束（或没进去）。

> 别用 `EGameplayTagEventType::AnyCountChange`。它会在每次标签计数变化时抖，包括"同一帧里被两次 GE 挂上"这种中间态。

### 3.3 timer 只采样，不递减（这条最容易写错）

**错**（看起来对，跑起来会漂）：

```cpp
void TickCooldowns()
{
    for (auto& Slot : Slots)
        if (Slot.Remaining > 0.f) Slot.Remaining -= 1.f / 30.f;   // ✗ 自己倒数
}
```

漂移来源有三处：timer 的实际触发间隔不是精确的 1/30；帧率抖动会让 timer 丢拍；**以及客户端预测回滚时本地那份冷却 GE 的剩余时长会变回去，而你的 float 只会往下走**。结果是 UI 上的数字和技能真实可用的时刻对不上，表现为"显示 0 了但按下去还是进冷却"。

**对**：

```cpp
void TickCooldowns()
{
    bool bAnyActive = false;
    for (FSkillSlotView& Slot : SlotCache)
    {
        if (!QueryCooldown(Slot.SlotIndex, Slot.CooldownRemaining, Slot.CooldownDuration)) continue;
        if (Slot.CooldownRemaining <= 0.f) continue;
        bAnyActive = true;
        Slot.CooldownPercent = (Slot.CooldownDuration > 0.f)
            ? FMath::Clamp(Slot.CooldownRemaining / Slot.CooldownDuration, 0.f, 1.f) : 0.f;
        OnSkillSlotChanged.Broadcast(Slot.SlotIndex, Slot);
    }
    if (!bAnyActive) UpdateCooldownTicker();   // 全好了 → 停心跳（下一次标签事件会再启）
}

/** 唯一的冷却数值来源。CDO 上安全：GetCooldownGameplayEffect() 读的是 CooldownGameplayEffectClass，不碰实例状态。 */
bool UHeroHUDController::QueryCooldown(int32 SlotIndex, float& OutRemaining, float& OutDuration) const
{
    UMyAbilitySystemComponent* ASC = BoundASC.Get();
    if (!ASC) return false;

    const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(ASC->GetHandleForSlot(SlotTagOf(SlotIndex)));
    if (!Spec || !Spec->Ability) return false;

    return Spec->Ability->GetCooldownTimeRemainingAndDuration(
        Spec->Handle, ASC->AbilityActorInfo.Get(), OutRemaining, OutDuration);
}
```

两个额外的好处：**急速在冷却中途变化时（以后的装备/buff）自动正确**，因为 Duration 每次都是现场查的；**不需要在标签 0→1 的那一刻去"猜"总时长**。

### 3.4 数字可以回弹，状态不可以

这是"标签是唯一真相"落地成代码时的具体形状，分开两条：

| 通道 | 谁驱动 | 回滚时怎么表现 |
|---|---|---|
| **数字 / 转圈进度**（连续量） | 30Hz 采样心跳 | 可以**回弹**（客户端预测被服务端拒绝时，剩余时长跳回去）。这是正确的。 |
| **可用 / 灰化**（离散状态） | `State.Cooldown.X` 标签 0↔1 | **不可以回弹。** 倒计时数到 0 **不点亮图标**，必须等标签真的落到 0。 |

反过来说：如果让 tick 在 `Remaining <= 0` 时顺手把状态翻成"可用"，那服务端拒绝激活（预测回滚）之后，图标会亮着但按下去被拒 —— 因为标签还在。**只有标签事件能翻状态**，tick 永远只改数字。

### 3.5 灰化用"旗标 + 优先级"，不是一个互斥枚举

"蓝不够"和"被沉默"可以**同时**成立。用单值 `ESkillSlotState` 的话，合成处会悄悄藏一条优先级规则，UI 侧和 tooltip 都看不出来。

```cpp
// UI/HUDTypes.h —— 纯 UI 语义，零 GAS include
UENUM(BlueprintType, meta=(Bitflags, UseEnumValuesAsMaskValuesInEditor="true"))
enum class ESkillSlotBlockReason : uint8
{
    None            = 0,
    NoAbility       = 1 << 0,   // 槽位上是空的
    Cooldown        = 1 << 1,   // State.Cooldown.X 计数 > 0
    NotEnoughEnergy = 1 << 2,   // Energy < Ability->ManaCost
    Silenced        = 1 << 3,   // State.Silenced
    Stunned         = 1 << 4,   // State.Stunned
    Dead            = 1 << 5,   // State.Dead
    BlockedByTags   = 1 << 6,   // 能力标签要求没满足，但 HUD 认不出具体是哪条（宁可灰错不可亮错）
};
ENUM_CLASS_FLAGS(ESkillSlotBlockReason);

/** UI 表现。Controller 按下面的优先级把旗标压成一个值，Widget 拿到就直接播。 */
UENUM(BlueprintType)
enum class ESkillSlotState : uint8
{
    Normal,     // 可放
    Cooled,     // 只是冷却中 —— 图标亮，上有转圈
    Greyed,     // 不可用但不是冷却 —— 整体压暗（蓝不够 / 被沉默 / 被晕 / 死亡）
    Disabled,   // 槽位空 / 没授权 —— 连图标都没有
};
```

**优先级（Controller 内，唯一一处）**：`Disabled > Greyed(Dead > Stunned > Silenced > NotEnoughEnergy) > Cooled > Normal`。

> **为什么要同时留 `BlockReasons`**：tooltip 要能说"被沉默，2.3 秒"，而不只是"灰的"。表现枚举给 Widget 播状态，旗标给 tooltip 说原因，两者都不占用对方的表达力。

> **死亡时冷却照常显示。** `Dead` 让整栏压暗，但冷却数字和转圈继续走 —— 这是两条独立通道，天然支持。别在 `Dead` 时把冷却清掉。

### 3.6 死亡判据用 `State.Dead`，不用 `Health == 0`

你写的"被晕/死：数据源 `State.Stunned` / `Health == 0`"，后半句建议改掉。

项目里已经写死过这条纪律 —— `AHeroCombatCharacter::IsDead()` 的注释：

> 读的是 ASC 上的 `State.Dead` 标签，不是 Health 数值：「能不能动」和「技能能不能放」必须是同一个判据。

HUD 如果是第二套判据，就会出现**技能栏亮着但按下去被拒**（`GE_Death` 还没挂上、或者刚到期的那一帧），或者反过来。HUD 必须跟技能走同一个判据：`State.Dead` 标签事件。`Health == 0` 只用于血条本身（画成空条）。

---

## 4. 三条数据通道

| 通道 | 数据对象 | 生命周期 | 事件源 | 宿主 |
|---|---|---|---|---|
| **Self** | 本地玩家自己的 PS（ASC + AttributeSet） | 跟 PS，整局不变（除非换 PS） | 属性委托 + 标签事件 + 30Hz 心跳 | `WBP_HUD` / `WBP_HealthBar` / `WBP_SkillSlot` |
| **Observed** | 当前目标（别人） | 随选目标变化、随时可为 `nullptr` | 重绑后的属性委托 + 标签事件 | `WBP_TargetFrame` |
| **Overlay** | 每个敌人自己 | 每个 Actor 一个 Widget | 直接绑目标 ASC 的属性委托 | `WBP_OverlayHealth`（`UWidgetComponent`） |

**只有 Self 通道走 HUDController。** Observed 是 HUDController 上的第二条（重）绑定；Overlay 完全独立、不经过 Controller —— 六个人打团 = 六次敌人血条更新，不该挤在本地玩家的一个心跳里。

---

## 5. 状态映射表（读写什么 / 什么时候重算）

| UI 表现 | 数据源（读什么） | 事件源（什么时候重算） |
|---|---|---|
| 血条 | `Health` / `MaxHealth` → 0..1 | `GetGameplayAttributeValueChangeDelegate(Health)` + `(MaxHealth)` |
| 能量条 | `Energy` / `MaxEnergy` → 0..1 | 同上两条 |
| 冷却转圈 + 秒数 | `Spec->Ability->GetCooldownTimeRemainingAndDuration()`（现场查，不缓存递减） | `RegisterGameplayTagEvent(State.Cooldown.X, NewOrRemoved)` 开/关心跳 + 30Hz 心跳刷新数字 |
| 蓝不够 | `Energy < Spec->Ability->ManaCost` | 属性委托（`Energy` / `MaxEnergy`）+ 冷却标签落地时重算一次 |
| 被沉默 | `State.Silenced` 计数 > 0 | `RegisterGameplayTagEvent(State.Silenced, NewOrRemoved)` |
| 被晕 | `State.Stunned` 计数 > 0 | 同上 |
| 死亡 | `State.Dead` 计数 > 0（**不是 `Health == 0`**） | 同上 |
| 充能 | `Charges` / `MaxCharges` 属性（Phase 6 才加） | 属性委托；UI 侧零特殊代码（字段先占好位，默认 1/1） |
| 目标框 | 目标 ASC 的 Health/MaxHealth + 名字 | `SetObservedTarget` 重绑后的属性委托 |

**六个槽 = `Ability.Slot.Q / W / E / R / D / F`。**
`Ability.Slot.Block`（右键格挡）和 `Ability.Slot.Passive` 不在六槽里，但**可以进 `Slots` 数组** —— 只是要配 `bHideWhenUnavailable`（见 §15）。格挡在 HUD 上通常是一个独立的指示器（读 `State.Cooldown.Block`），被动是一条"强化普攻亮起"的提示（读 `State.EmpoweredAttack`）；两者都复用同一条 Self 通道，只是 Widget 长得不一样。

**现状提醒**：目前有六条冷却标签（`Flash` / `ThrowDagger` / `Stealth` / `Block` / `Passive` / `DeathHarvest`），但六个主动槽里只有 4 条能映射上，所以映射表**必须允许 CooldownTag 为空**，空 = 这个槽没有冷却转圈。`State.Cooldown.Passive` 目前**没有任何能力在用**，是刻意预留的（见 §15）。

---

## 6. 可配置化：`UHeroHUDSlotConfig`

跟 `UAbilitySet` / `UInputConfig` 同风格，`UPrimaryDataAsset`，放 `GAS/`（它持有 `FGameplayTag`，按 `CONVENTIONS.md` 规则 2 归 `GAS/`）。

```cpp
USTRUCT(BlueprintType)
struct FHeroHUDSlotEntry
{
    GENERATED_BODY()
    /** 槽位标签：Ability.Slot.Q 等。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FGameplayTag SlotTag;
    /** 对应的冷却标签：State.Cooldown.Flash 等。留空 = 该槽不显示冷却转圈。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FGameplayTag CooldownTag;
    /** 种类：Active / Passive / Block。唯一影响是被动不画键位标注。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) EHeroHUDSlotKind Kind = EHeroHUDSlotKind::Active;
    /** 没能力时整格收起（预留位用），而不是画一个灰格子。默认 false。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) bool bHideWhenUnavailable = false;
    /** UI 侧静态表现。Widget 不认标签，只收这几个字段。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) TObjectPtr<UTexture2D> Icon;
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FText KeyLabel;      // "Q" / "W" / ...
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly) FText DisplayName;
};

UCLASS()
class LOL_API UHeroHUDSlotConfig final : public UPrimaryDataAsset
{
    GENERATED_BODY()
public:
    /** 【有序】= 技能栏从左到右的排列。数组顺序就是 UI 顺序，别的什么都不用排。 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="HUD") TArray<FHeroHUDSlotEntry> Slots;

    /** 槽位标签 → 槽位序号。第一次访问时从 Slots 建缓存。 */
    int32 IndexOfSlot(const FGameplayTag& SlotTag) const;
    const FHeroHUDSlotEntry* FindEntry(int32 SlotIndex) const;
};
```

**用有序 `TArray` 而不是 `TMap<FGameplayTag, FGameplayTag>`** —— 这一点很关键：`TMap` 的迭代顺序不稳定，槽位顺序就丢了，于是 UI 还得在蓝图里再排一次序。那样"SlotTag→CooldownTag 的映射"就有两份定义（C++ 一份、蓝图里那份顺序一份），迟早对不上。**排序也是这份 DataAsset 的职责**，`IndexOfSlot` 内部用一个缓存 `TMap` 做 O(1) 查，对外只暴露数组。

---

## 7. 类清单与文件落位

按 `CONVENTIONS.md`：持有/操作 ASC / GA / GE / GameplayTag 的进 `GAS/`，其余按功能分目录。UI 层不持有 ASC → 进新的 `UI/`（和 `Variant_*/UI/` 同级同级，但属于项目自己的代码）。

| 类 | 文件 | 蓝图 | 职责 |
|---|---|---|---|
| `ESkillSlotState` / `ESkillSlotBlockReason` | `Public/UI/HUDTypes.h` | — | **纯 UI 语义类型 + 全部委托声明。零 GAS include。** Widget 头文件只允许 include 这一个 |
| `FSkillSlotView` / `FHUDSnapshot` | `Public/UI/HUDTypes.h` | — | UI 语义的数据结构（`SlotIndex` / 百分比 / 秒数 / 图标 / 旗标），**不含任何 `FGameplayTag`** |
| `UHeroHUDController` | `Public/GAS/HeroHUDController.h` + `Private/GAS/HeroHUDController.cpp` | — | 翻译层。唯一的 GAS 语义出口 |
| `UHeroHUDSlotConfig` | `Public/GAS/HeroHUDSlotConfig.h` + `.cpp` | DA_HUDSlots | 槽位→冷却标签映射 + 顺序 + 图标 |
| `UHeroHUDWidget` | `Public/UI/HeroHUDWidget.h` + `.cpp` | `WBP_HUD` | 根容器。`NativeConstruct` 拉快照、建六个槽、订阅委托 |
| `UHeroSkillSlotWidget` | `Public/UI/HeroSkillSlotWidget.h` + `.cpp` | `WBP_SkillSlot` | 单个槽：图标、转圈、秒数、灰化。只认 `int32 SlotIndex` |
| `UHeroHealthBarWidget` | `Public/UI/HeroHealthBarWidget.h` + `.cpp` | `WBP_HealthBar` | 血条 / 能量条（一个类两个实例，`bIsEnergy` 开关） |
| `UHeroOverlayHealthWidget` | `Public/UI/HeroOverlayHealthWidget.h` + `.cpp` | `WBP_OverlayHealth` | 敌人头顶血条（`UWidgetComponent`），**不走 HUDController** |
| `UHeroOverlayHealthComponent` | `Public/GAS/HeroOverlayHealthComponent.h` + `.cpp` | 挂在人物 BP 上 | 头顶血条的迷你翻译层：绑 Owner 的属性 + **自动吸附到骨骼插槽**（§12.7） |
| `FHeroAttributeView` | `Public/GAS/HeroAttributeView.h` | — | **24 条属性的裸数值快照**。零 GAS include、零派生比率 |
| `UHeroAttributeLibrary` | `Public/GAS/HeroAttributeLibrary.h` + `.cpp` | — | 属性数值的唯一查询出口（静态函数库），任何 Actor 都能查（§14） |

**`ALOLPlayerController` 的改动**（三行 + 三个 override）：

```cpp
/** 本地玩家的 HUD 翻译层。只有 IsLocalController() 时才非空。 */
UPROPERTY(Transient) TObjectPtr<UHeroHUDController> HUDController;

UPROPERTY(EditDefaultsOnly, Category="HUD") TSubclassOf<UUserWidget> HUDWidgetClass;

virtual void BeginPlay() override;                 // 已有 → 加：建 Controller + Widget，TryBindHUD()
virtual void AcknowledgePossession(APawn* P) override;   // 新增 → TryBindHUD()
virtual void OnRep_PlayerState() override;               // 新增 → TryBindHUD()

/** 蓝图/Widget 拿 Controller 的入口。 */
UFUNCTION(BlueprintPure, Category="HUD") UHeroHUDController* GetHUDController() const { return HUDController; }
```

> **`ALOLPlayerController` 是 `UCLASS(abstract)`，本来就是给蓝图的 `BP_LOLPlayerController` 继承的** —— `HUDWidgetClass` 在蓝图上配，C++ 里不写死路径。

**`UI/HUDTypes.h` 里"零 GAS include"的强制手段**：Widget 的 `.h` 只 `#include "UI/HUDTypes.h"`。想在 CI 里卡住的话，一条 grep 就够：

```bash
grep -rn "AbilitySystemComponent.h\|GameplayTagContainer.h\|GameplayEffect.h" Source/LOL/Public/UI/ Source/LOL/Private/UI/
# 期望：零命中
```

---

## 8. 委托清单（BlueprintAssignable，签名里没有 GAS 类型）

```cpp
// ---- Self 通道 ----
/** 血 + 能量【一条】委托：拆成两条就多一个"忘了订阅"的理由。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHUDVitalsChangedSignature, const FHUDVitalsView&, Vitals);

/** 单个技能槽：秒数 + 转圈进度 + 状态 + 旗标一次给全。心跳和标签事件都走这一条。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSkillSlotChangedSignature, int32, SlotIndex, const FSkillSlotView&, View);

/** 绑定完成 / 换 PS 后重放全量。bBound=false 时 Widget 显示"未就绪"。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHUDReadySignature, bool, bBound);

// ---- Observed 通道 ----
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTargetFrameChangedSignature, const FTargetFrameView&, View);
```

**冷却和灰化合并成一条 `OnSkillSlotChanged`** —— `FSkillSlotView` 里同时带 `CooldownRemaining / CooldownPercent / State / BlockReasons`。六条委托会变成六个忘记订阅的理由；一条委托 + 一个"完整视图结构"是最难写错的形状。

---

## 9. 明确否决的做法

| 否决 | 理由 |
|---|---|
| 在 Widget 的 `NativeTick` 里调 `CanActivateAbility` | 它是**给服务端判定用的**，会在客户端对"还没同步过来的状态"给出答案；而且每帧 6 次，还会打日志。灰化必须由事件合成 |
| 用 `OnRep_Health` 之类的 `OnRep_*` 驱动 HUD | **listen server 上主机的 `OnRep` 不触发** —— 主机是权威端，属性是直接写进去的。症状是"主机看不到自己的血条动" |
| 用 `AActor::GetGameplayAttributeValueChangeDelegate` 之外的路子自己轮询属性 | 已经有事件了，轮询只会引入延迟和一帧的抖动 |
| 让 tick 在剩余时长为 0 时把图标点亮 | 预测回滚会让客户端出现"图标亮着但按下去被拒"。状态只能由标签事件翻（§3.4） |
| 每个敌人挂一个 `UHeroHUDController` | 六个人打团 = 六份绑定 + 六份快照。敌人头顶血条直接绑自己的 ASC，不经 Controller |
| `TMap<SlotTag, CooldownTag>` 当映射表 | 迭代顺序不稳定 = 槽位顺序丢失，UI 被迫再排一次序，映射变成两份（§6） |
| 在 HUD 里再算一次急速 `× 100/(100+Haste)` | `Duration` 已经是含急速的值，再乘一次就是双减（你的原话，这里只是把它变成一条评测点） |
| 死亡时把技能栏的冷却清空 | 死亡和冷却互相独立，`Dead` 只负责压暗整栏 |

---

## 10. 实施顺序与验收标准

### Phase 1 — HUDController + 血条 / 能量条

新建 `UI/HUDTypes.h`、`GAS/HeroHUDController`、`UI/HeroHealthBarWidget`、`UI/HeroHUDWidget` 骨架；PC 上建 Controller + Widget 并挂三个 `TryBindHUD()` 调用点。

**验收（这一步就是在验你说的两个坑）**：
1. **绑定时机**：`BeginPlay` 时 PS 还没到、`AcknowledgePossession` 才绑上 —— 血条要在绑定成功那一刻就显示正确值，不是从 0 慢慢涨上来。
2. **初始值**：把 `PullHUDState()` 从 `WBP_HUD::NativeConstruct` 里临时注释掉，血条应该**不动**（证明事件委托不触发初始值）；加回来就正确（证明拉取这条路是通的）。
3. **listen server**：主机自己走动受伤时血条要动（这是"不用 `OnRep_`"的验收点）。两个客户端各看各的，互不串。

### Phase 2 — 六个技能槽 + 冷却转圈（重头戏）

`UHeroHUDSlotConfig` + `DA_HUDSlots`；六个 `WBP_SkillSlot`；`QueryCooldown()` + 共享 30Hz 心跳；`FSkillSlotView` 全量广播。

**验收（2 客户端 PIE）**：
1. 客户端按 Q（本地预测进冷却）→ **服务端拒绝**（拉开距离 / 先被晕住）→ 客户端转圈要**回滚消失**、图标保持可用。
2. 客户端按 Q 成功 → 两端都转，数字**同帧**更新（这是"一个共享心跳"的验收点）。
3. 主机按 Q → 主机自己也看到转圈（这是"不用 `OnRep_`"的第二个验收点）。
4. 冷却跑完**不点亮**；标签落地才点亮（§3.4）。
5. 冷却中途改 `AbilityHaste` → 转圈速度跟着变（验证"现场查 Duration"而不是缓存递减）。

### Phase 3 — 灰化状态机

`BlockReasons` 合成 + 优先级 + tooltip。

**验收**：挂 `UGE_Silence` → 六个法系槽同时 `Greyed`，但普攻槽不动（项目里沉默不挡普攻）；`UGE_Stun` → 全灰；死亡 → 全灰压暗但**冷却数字照常在走**；蓝不够 → 只有 `Energy < ManaCost` 的那个槽灰（注：`ManaCost` 目前恒为 0，这条通道现在恒为 false，先写代码不验证）。

### Phase 4 — 目标框（Observed 通道）

`SetObservedTarget()` + `UnbindObserved()` + `WBP_TargetFrame`。

**验收**：选目标 → 显示；目标死亡 / 被销毁 → 自动收起（`TWeakObjectPtr` 天然处理）；连续快速切目标 20 次 → 不重不漏（这是"先摘后挂"的验收点，漏了就表现成血条乱跳）。

### Phase 5 — 敌人头顶血条

`UWidgetComponent` + `UHeroOverlayHealthWidget`，**独立于 HUDController**。

**验收**：三个敌人同时受伤，三条血条各自正确、互不影响；敌人销毁时 Widget 干净回收，没有委托悬挂警告。

### Phase 6 — 充能

`Charges` / `MaxCharges` 属性 + 恢复 GE + `State.Recharging.Charge.X` 标签。UI 侧只做一件事：把已经占好位的 `Charges/MaxCharges` 画成分段。等真有需要的英雄再做。

---

## 11. 已知缺口（这一轮不做，但要知道）

1. **目标框不含"蓝条 / buff 栏"** —— `SetObservedTarget` 的通道留好了，往上加 Widget 就行，不用改架构。
2. **冷却是"单层"的** —— 如果以后有"两层充能 + 各自的冷却"（比如 EZ 的 E），`FSkillSlotView` 的 `Charges` 字段先占位，但真正的多段显示要等 Phase 6。
3. **HUD 没有做"技能射程指示 / 施法条"** —— 这两个都是"世界空间表现"，归 GameplayCue，不归 HUD 数据管道。
4. **`PullHUDState()` 的 `FHUDSnapshot` 会随字段增长变胖** —— 如果最后超过几十个字段，拆成 `Self` / `Observed` 两份快照，别做成一个巨型结构体。

---

## 12. 编辑器配置步骤（C++ 已就绪，剩下的全在编辑器里）

C++ 侧已全部实现并**编译链接通过**（`Binaries/Win64/UnrealEditor-LOL.dll`），本节是"从零把资产和蓝图接起来"的操作清单。

> **本节是唯一需要手工做的部分。** C++ 不写死任何资产路径，所以下面每一项都得在编辑器里挂一次。
> 挂漏了不会编译报错 —— 表现是"血条不动 / 槽位不出来"，排查时**第一个要查的就是控件名拼写**（见 §12.9）。

### 12.1 资产总览

| 资产 | 父类 / 类型 | 被谁引用 |
|---|---|---|
| `DA_HUDSlots` | Data Asset → `HeroHUDSlotConfig` | `BP_LOLPlayerController::HUDSlotConfig` |
| `WBP_HealthBar` | Widget Blueprint → `HeroHealthBarWidget` | `WBP_HUD::HealthBar`、`WBP_OverlayHealth::HealthBar` |
| `WBP_EnergyBar` | Widget Blueprint → **`WBP_HealthBar`**（继承 WBP，不是 C++ 类） | `WBP_HUD::EnergyBar` |
| `WBP_SkillSlot` | Widget Blueprint → `HeroSkillSlotWidget` | `WBP_HUD::SkillSlotWidgetClass` |
| `WBP_HUD` | Widget Blueprint → `HeroHUDWidget` | `BP_LOLPlayerController::HUDWidgetClass` |
| `WBP_OverlayHealth` | Widget Blueprint → `HeroOverlayHealthWidget` | `HeroOverlayHealthComponent::WidgetClass` |

> 这些 C++ 类都是 `UCLASS(Abstract)`，所以只能被继承、不能被直接实例化 —— 这正是想要的行为。

**建法**：Content Browser → 右键 → **User Interface → Widget Blueprint** → 弹出 "Pick Parent Class" 窗口里**切到 "All Classes" 页搜类名**（这些类不在默认的常用列表里）。

### 12.2 `DA_HUDSlots`（槽位映射 + 顺序）

Content Browser → 右键 → **Miscellaneous → Data Asset** → 选 `HeroHUDSlotConfig` → 命名 `DA_HUDSlots`。

`Slots` 数组按**技能栏从左到右**的顺序填 6 条，每条填：

| 字段 | 填什么 |
|---|---|
| `SlotTag` | `Ability.Slot.Q` / `.W` / `.E` / `.R` / `.D` / `.F` |
| `CooldownTag` | 对应的 `State.Cooldown.*`，**可以留空** |
| `Kind` | `Active`（默认）/ `Passive` / `Block`。**被动必须选 `Passive`**，否则会画出一个没有意义的键位字母 |
| `bHideWhenUnavailable` | 六个主动槽留 `false`；**预留的被动 / 格挡槽勾上 `true`**（见 §15） |
| `Icon` | 该技能的图标 `Texture2D` |
| `KeyLabel` | `"Q"` / `"W"` / …（显示用，不是输入绑定）。`Kind=Passive` 时留空即可，C++ 会强制清掉 |
| `DisplayName` | 技能名（tooltip 用） |

> **数组顺序就是 UI 顺序**，数组下标就是 `FSkillSlotView::SlotIndex`。这是这份资产存在的意义 —— 别在蓝图里再排一次序。

**现有可用的标签（照抄，别手打）**：

- 槽位：`Ability.Slot.Q` / `Ability.Slot.W` / `Ability.Slot.E` / `Ability.Slot.R` / `Ability.Slot.D` / `Ability.Slot.F`（另有 `.Passive` / `.Block`，见 §15）
- 冷却：`State.Cooldown.Flash` / `.ThrowDagger` / `.Stealth` / `.Block` / `.Passive` / `.DeathHarvest`

> **六条主动槽里通常只有 4 条能映射到冷却** —— `Ability.Slot.D` / `.F` 一般是召唤师技能和鞋子，
> 各自有独立的冷却体系。**`CooldownTag` 留空是合法的**（该槽不显示转圈），
> 不要为了让表"填满"而乱配。这条在 §5 和 `HeroHUDSlotConfig.h` 的注释里都写了，很容易被忽略。
>
> `State.Cooldown.Block` 和 `.Passive` 都属于**不在六槽里**的两个槽，只在你按 §15 把
> `Ability.Slot.Block` / `.Passive` 加进 `Slots` 数组时才用得上。

**槽位顺序对着你现有的 `AbilityInputConfig` 抄** —— 那是"哪个键 → 哪个槽"的权威定义，别在这里另起一套。代码里只有一个锚点可参考：`ThrowDaggerAbility.cpp:112` 判断的是 `Ability.Slot.E`，所以投掷匕首在 E 槽。

### 12.3 `WBP_HealthBar` / `WBP_EnergyBar`

`WBP_HealthBar` 里放两个控件，**名字必须逐字一致**：

| 控件名 | 类型 | 说明 |
|---|---|---|
| `Bar` | ProgressBar | 主填充条 |
| `NumericText` | TextBlock | C++ 直接写 `"123 / 456"`；不想让它写就把 `bShowNumericText` 关掉，自己在蓝图排 |

Class Defaults（`HUD` 分类下）：

| 属性 | 建议值 |
|---|---|
| `bIsEnergyBar` | `false`（血）/ `true`（能量） |
| `LowValueThreshold` | 血条 `0.3`；**能量条留 `0` = 不启用**低值变色 |
| `FillColor` | 血条绿 / 能量条蓝 |
| `LowFillColor` | 低血时的红 |
| `bShowNumericText` | 按需 |

`WBP_EnergyBar` 直接**继承 `WBP_HealthBar`**（Content Browser 里对 `WBP_HealthBar` 右键 → Create Child Blueprint Class），只改 `bIsEnergyBar=true` 和颜色 —— 布局不用再画一遍。

蓝图事件 `BP_OnVitalsChanged(Vitals, Percent, Current, Max)` 用来做额外表现（描边、脉冲、分段线）。

### 12.4 `WBP_SkillSlot`

| 控件名 | 类型 | 说明 |
|---|---|---|
| `CooldownRing` | ProgressBar | **必须用 radial fill（转圈）材质的 ProgressBar** —— 用横向填充的条就不是转圈了。`Percent` 直接喂：`1` = 刚进 CD，`0` = 好了 |
| `CooldownText` | TextBlock | 剩余秒数。文本请取 `GetCooldownLabel()`，**别在蓝图里另写一套取整规则** |
| `KeyLabelText` | TextBlock | `Q` / `W` / …（C++ 从映射表的 `KeyLabel` 填） |
| `IconImage` | Image | 图标（C++ 用 `SetBrushFromTexture` 填） |
| `GreyedOverlay` | Image | 半透明黑遮罩，只在 `Greyed` / `Disabled` 时显示 |

Class Defaults：`ReadyIconTint`（可用）/ `BlockedIconTint`（灰化）/ `bShowCooldownText`。

蓝图事件 `BP_OnSlotViewChanged(View)` 做额外表现（可用时闪一下、CD 结束音效、以后的充能分段）。

> **一条容易写错的**：转圈的显示判定 C++ 用的是 `View.ShouldShowCooldown()`（= 标签在冷却 **且** 有总时长），
> **不是** `State == Cooled`。两者不等价 —— 死亡时 `State` 是 `Greyed` 但冷却数字照常要走。
> 蓝图不要自己再判一次，直接用 `GetSlotView()`。

### 12.5 `WBP_HUD`

| 控件名 | 类型 | 说明 |
|---|---|---|
| `HealthBar` | **`WBP_HealthBar`** | 类型必须是 `UHeroHealthBarWidget` 的子类，否则绑定会失败 |
| `EnergyBar` | **`WBP_EnergyBar`** | 同上 |
| `SkillSlotContainer` | `HorizontalBox` / `UniformGridPanel` / 任意 `UPanelWidget` | C++ 往里 `AddChild` 建槽。**留空 = 不自动建槽** |

Class Defaults：`SkillSlotWidgetClass` = `WBP_SkillSlot`。

蓝图事件：

| 事件 | 用途 |
|---|---|
| `BP_OnVitalsChanged` | 额外表现（整条 HUD 的血量脉冲等） |
| `BP_OnSkillSlotChanged` | 槽位变化（六槽的 C++ 子 Widget 已经在更新了，这里是补充） |
| `BP_OnTargetFrameChanged` | **重点**：目标框**没有**内置子 Widget —— 长什么样、放哪、显不显示，全在蓝图。`View.bHasTarget == false` 时把整个框收起来 |
| `BP_OnHUDReady` | `bBound == false` 时显示"未就绪"，**别显示空血条** |

> 槽位 Widget 是**运行时懒创建**的，所以设计器里看不到它们。要调槽位样式请直接编 `WBP_SkillSlot`。

### 12.6 `BP_LOLPlayerController`

打开你现有的 `BP_LOLPlayerController`（父类是 `ALOLPlayerController`）→ Class Defaults → **`HUD` 分类**：

- `HUD Widget Class` = `WBP_HUD`
- `HUD Slot Config` = `DA_HUDSlots`

**不需要写任何 HUD 逻辑**：Controller 和 Widget 的创建、三个绑定时机（`BeginPlay` / `OnRep_PlayerState` / `AcknowledgePossession`）、`EndPlay` 收尾，全在 C++ 里。也不需要用 `IsLocalPlayerController()` 再门一次 —— 已经门好了。

### 12.7 敌人头顶血条

1. 建 `WBP_OverlayHealth`（父类 `HeroOverlayHealthWidget`），里面放一个名为 `HealthBar` 的 `WBP_HealthBar` 实例（同样是同名绑定）。
2. 在 `BP_HeroCombatCharacter`（或任何需要头顶血条的 BP）上 **Add Component → HUD → Hero Overlay Health**，然后：

| 属性 | 值 |
|---|---|
| `Widget Class` | `WBP_OverlayHealth` |
| `Space` | **`Screen`** —— 用 `World` 的话血条会跟着角色一起旋转 |
| `Draw Size` | 按需（比如 120×12） |
| `Pivot` | `(0.5, 1.0)` —— 让条子底边贴住锚点，向上长 |
| `Owner Mesh Socket Name` | **`HealthBar`**（默认值，一般不用改）—— 见下 |
| `Socket Offset` | 留空。真要微调高低才填，比如 `(0, 0, 20)` |
| `Hide When Full Health` | 小兵 `true`、英雄 `false` |
| `Hide When Dead` | 按需 |

**挂点不用手填，组件自己吸附到骨骼插槽。** 角色的骨骼上留了名为 `HealthBar` 的插槽，`BeginPlay` 时组件会自己 `AttachToComponent(Owner 的 SkeletalMesh, ..., "HealthBar")`。所以：

- **不需要**在细节面板里选 `Parent Socket`，也**不需要**填相对位置 —— 位置由骨骼决定，跟着动画走。
- 插槽名不是硬编码：`Owner Mesh Socket Name` 是可改的属性，**留空（None）= 关掉吸附**，完全按蓝图里配的父子关系走（小兵 / 建筑这类不跟骨骼的情况用）。
- 它和 Transform 里的 `Parent Socket` **不是一回事**：`Parent Socket` 是引擎的 `USceneComponent::AttachSocketName`（实际结果，`AttachToComponent` 写它）；`Owner Mesh Socket Name` 是组件的「意图」（想吸附到哪）。名字没复用 `AttachSocketName` 是因为 UHT 不允许子类遮蔽父类成员，会直接报 `shadowing is not allowed`。
- 要微调高度改 `Socket Offset`，**别去动骨骼插槽** —— 那个插槽是给美术用的，挪它会波及所有引用它的地方。
- 吸附失败（没有 SkeletalMesh / 骨骼上没有这个插槽）时**只打 Warning 不吸附**，保留蓝图里配的挂位。这是故意的：`AttachToComponent` 传一个不存在的插槽名不会报错，而是静默挂到 mesh 原点，表现是「血条跑到脚底」—— 比不挂更难查。

它**完全不走 HUDController**：每个敌人各绑自己的 ASC 属性委托，五个人打团不会挤本地玩家那一个 30Hz 心跳。组件默认观察 `GetOwner()`，绝大多数情况不用改。

### 12.8 挂完之后的验收顺序

对着 §10 的验收点走，顺序建议：

1. **Phase 1 三条**：① `AcknowledgePossession` 才绑上时血条要立刻正确（不是从 0 涨）；② 把 `PullHUDState()` 临时注释掉血条应该**不动**（证明事件不触发初始值）；③ **listen server 主机自己受伤时血条要动**（这是"不用 `OnRep_`"的验收点）。
2. **Phase 2 的 2 客户端 PIE**：预测被拒 → 转圈回滚消失、图标保持亮；成功 → 两端同帧转；CD 跑完**不点亮**，标签落地才亮。
3. **Phase 3**：挂 `UGE_Silence` → 法系槽灰、普攻槽不动；`UGE_Stun` → 全灰；死亡 → 全灰压暗但**冷却数字照常在走**。
4. **Phase 4**：连续快速切目标 20 次不重不漏。
5. **Phase 5**：三个敌人同时受伤，三条血条各自正确。

### 12.9 排查：资产挂漏了不会报错

所有控件绑定用的都是 **`BindWidgetOptional`**（不是 `BindWidget`）—— 名字对不上就**静默跳过**，不报错、不开不了蓝图。这是故意的：HUD 排版会反复改，强制绑定的代价（改个名字就编译不过）比收益大。

代价是**"挂漏了"表现为无声的不动**。所以症状 → 排查顺序：

| 症状 | 先查 |
|---|---|
| 血条完全不动 | `WBP_HUD` 里控件是不是叫 `HealthBar` / `EnergyBar`；`WBP_HealthBar` 里是不是叫 `Bar` |
| 一个槽都不出现 | `WBP_HUD::SkillSlotWidgetClass` 填了没；`SkillSlotContainer` 有没有放 |
| 槽位出现但不变 | `WBP_SkillSlot` 里 5 个控件名拼写；`DA_HUDSlots` 的 `SlotTag` 是不是 `Ability.Slot.Q` 这种精确值 |
| 图标是空的 | `DA_HUDSlots` 里对应条目的 `Icon` 填了没；`WBP_SkillSlot::IconImage` 名字对不对 |
| 数字/转圈不出现 | `CooldownRing` 是不是 radial fill；`DA_HUDSlots` 里该槽的 `CooldownTag` 是不是空的 |
| 整个 HUD 不出现 | `BP_LOLPlayerController::HUDWidgetClass` 填了没；是不是在 listen server 的**远端客户端**窗口（那里不该有） |

---

## 13. 让 Agent 的改动"实时"出现在 VS 里

### 13.1 机制：VS 自己就会重载

VS 的 **工具 > 选项 > 环境 > 文档** 里这两个开关默认是开的：

- `检测环境外文件的变化`（`DetectFileChangesOutsideIDE`）= True
- `保存后自动加载更改`（`AutoloadExternalChanges`）= True

实测（建一个临时文件、在 VS 里打开、再在磁盘上改写）：**约 1 秒后编辑器缓冲区就变成新内容** —— 没有提示条、不用点确认。

所以"外部改动能被看到"这件事**本来就成立**，唯一条件是：**文件得在 VS 里开着**。

### 13.2 工具：`Tools/vs_show.py`

```bash
python Tools/vs_show.py prepare <file>...   # 存基线 + 在 VS 里打开（编辑前调这个）
python Tools/vs_show.py diff <file>         # 弹 VS 自带 Diff：基线 -> 当前
python Tools/vs_show.py open <file>...      # 只打开
python Tools/vs_show.py goto <file> --line 42
python Tools/vs_show.py reload <file>       # 强制重载（缓冲区有未保存改动会拒绝）
python Tools/vs_show.py status [<file>...]  # 开着没？脏没？多少行？
python Tools/vs_show.py open-changed --minutes 10   # 把最近改过的源文件全部打开
```

工作流是 **`prepare` → 编辑 → `diff`**。`prepare` 做两件事：把当前字节存到 `Saved/AgentDiff/`（作 diff 左侧），并把文件推到 VS 前台。之后编辑一落盘，VS 自己刷新；`diff` 再把 VS 内置的 Diff 窗口弹出来，左右就是"改之前 / 改之后"。

### 13.3 与 Claude Code 在 VS Code 里的差别

| 能力 | VS Code 扩展 | 这里 |
|---|---|---|
| 文件内容实时刷新 | 是 | **是**（VS 自带，约 1s） |
| 编辑器内 inline diff（行内红绿） | 是 | **否** —— VS 没给第三方 agent 这种插槽 |
| 独立 Diff 窗口 | — | **是**（`File.Compare`，整窗左右对比） |
| 新文件自动弹出 | 是 | 需显式 `open` / `open-changed` |

缺的那一条是架构性的：VS 2022 没有 Claude Code 那种"agent 直接往编辑器里画 diff"的接口。能做的最接近形态就是 `prepare` + `diff`：文件先在眼前，内容自己变，然后弹整窗 diff。

### 13.4 注意：VS 里挂着已经不存在的文件

`status` 会列出所有打开文档。本项目里有一批**指向旧路径**的残留标签，来自源码 `Private`/`Public` 重组之前：

- `Source\LOL\GAS\GA_DeathHarvest.cpp` / `.h`（现已在 `Private\GAS\` / `Public\GAS\`）
- `Source\LOL\ThrowDaggerAbility.h`（现已在 `Public\GAS\`）
- `Source\LOL\LOLCharacter.h`（现已在 `Public\`）

**不影响编译**，只是 VS 的打开标签留在老位置。看着碍眼就手动关掉。

---

## 14. 属性数值查询：`UHeroAttributeLibrary`

前面 13 节讲的是 **HUD**（血条怎么画、技能槽怎么灰）。这一节是另一个需求：**「某个角色现在攻击力 / 护甲 / 移动速度是多少」** —— 以后的属性面板、死亡回放、调试叠加层都用它。两者不重叠：HUD 要的是「画多长」，这里要的是「值是多少」。

### 14.1 一次调用拿全量

```cpp
// 蓝图里就是一个纯函数节点：Get Hero Attributes
FHeroAttributeView View = UHeroAttributeLibrary::GetHeroAttributes(SomeActor);
if (View.bValid)
{
    // View.AttackDamage / View.Armor / View.MoveSpeed / ...
}
```

`FHeroAttributeView`（`Public/GAS/HeroAttributeView.h`）= **`bValid` + 24 条属性的当前值**，字段顺序对齐 `UHeroCombatAttributeSet`（资源 6 → 输出 12 → 防御 3 → 通用 3）。

### 14.2 三条设计决定

**① 不带百分比。** 这个结构体回答「数值是多少」，不回答「进度条画多长」。要画条走 `FHUDVitalsView` —— 它带 `HealthPercent` / `EnergyPercent`，因为那是 **UI 语义**（1 = 满，直接喂 `ProgressBar`），而且是事件推的。给每条资源都挂一个 `*Percent` 会多出 6 个必须同步维护的字段，而「分母是什么」还会漂移（当前血 / 最大血？还是当前血 / 满级血？）。**百分比只属于进度条那一层。**

**② 不做 24 个独立 getter。** 一次调用 + 一个 `Break` 节点；加属性只改结构体。换成 24 个函数就是 24 个节点、24 次 ASC 子对象查找，而且每加一条属性都要补一个接口。

**③ 静态函数库，不是某个对象上的成员。** 挂 `UHeroHUDController` 上就只能查本地玩家（目标框 / 敌人 / 小兵都查不到），而且会把它从「UI 语义翻译层」变成属性大杂烩；挂 `PlayerState` 上则查不到小兵 / 建筑。传 Actor 进来的静态函数谁的数值都能查，没有状态、没有生命周期。

### 14.3 唯一入口，且只有一条找 ASC 的路

`UHeroAttributeLibrary` 内部只走 `UMyAbilitySystemComponent::FindAbilitySystemComponent` —— 和**选目标 / 伤害校验用的是同一条路**。别在别处再写一遍「怎么从 Actor 找 ASC」：蓝图库那条路在「接口 Cast 成功但返回 nullptr」时不会继续往下试（见 `MyAbilitySystemComponent.h` 里那段注释），口径不一致会出「客户端认得出、服务端认不出」的诡异不同步。

读不到 ASC / ASC 上没挂属性集时返回 **`bValid = false`**，其余字段是 0 —— **不是「血是 0」**。用法上应当直接跳过显示，而不是画一条空血条。

### 14.4 它只读，不监听

不缓存、不广播、不持有句柄。需要「变了通知我」的场景：

- HUD 用 → `UHeroHUDController` 的 `OnVitalsChanged` / `OnSkillSlotChanged`（已经处理好了绑定时机、重试、解绑）；
- 别的用途 → 自己对 `ASC->GetGameplayAttributeValueChangeDelegate(...)` 加委托。

**别把这个库扩成事件源。** 一旦它开始广播，就要处理订阅生命周期（谁绑的、什么时候解绑、ASC 被换了怎么办），那就等于把 `UHeroHUDController` 那一整套复杂度又抄了一遍。

---

## 15. 被动 / 格挡的预留位

### 15.1 结论：C++ 已经就绪，加被动不用回来改代码

技能栏本身是**完全数据驱动**的 —— `Slots` 数组里有几条就建几个槽，`SlotTag` 填什么就查什么能力。所以"支持被动"不需要新结构，只需要两个开关：`Kind`（画法）和 `bHideWhenUnavailable`（还没实装时别占地方）。

两个槽位标签**早就定义好了**，直接能用：

| 槽位标签 | 用途 | 现在的状态 |
|---|---|---|
| `Ability.Slot.Passive` | 被动（三连击已经挂在它上面，`RouteBasicAttackInput` 就是按它查句柄的） | **已在使用** |
| `Ability.Slot.Block` | 右键格挡 | **零引用** —— 标签在，但没有任何能力 / `UAbilitySet` 条目挂它 |

冷却标签六条齐了：`State.Cooldown.Flash / ThrowDagger / Stealth / Block / Passive / DeathHarvest`。其中 **`State.Cooldown.Passive` 是这次新增的预留项，目前没有能力在用**。

### 15.2 两步操作

**第一步：`DA_HUDSlots` 里加条目。**

| 字段 | `Passive` 条目 | `Block` 条目 |
|---|---|---|
| `SlotTag` | `Ability.Slot.Passive` | `Ability.Slot.Block` |
| `CooldownTag` | `State.Cooldown.Passive` | `State.Cooldown.Block` |
| `Kind` | **`Passive`** | `Block` |
| `bHideWhenUnavailable` | **`true`** | **`true`** |
| `KeyLabel` | 留空 | 留空（或右键图标，走 `BP_OnSlotViewChanged` 自己画） |

放在数组的哪一头由你定 —— **数组顺序就是 UI 顺序**，把被动放最前面就是"被动在技能栏最左边"，LoL 的做法。

**第二步（只有格挡需要）：让能力带上槽位标签。**

HUD 认槽位的方式是 `ASC->GetHandleForSlot(SlotTag)`，它扫的是 `AbilitySpec.DynamicAbilityTags` 里的 `Ability.Slot.` 前缀。这个标签是 `UAbilitySet` 授予时打上去的：

```cpp
// UAbilitySet::GiveToAbilitySystem
FGameplayAbilitySpec Spec(GA.Ability, GA.AbilityLevel);
if (GA.SlotTag.IsValid()) { Spec.DynamicAbilityTags.AddTag(GA.SlotTag); }
```

所以给格挡加槽位身份 = **在授予 `GA_Block` 的那条 `UAbilitySet` 条目里把 `SlotTag` 填成 `Ability.Slot.Block`**。不填的话 HUD 永远判 `NoAbility` —— 配了 `bHideWhenUnavailable=true` 就正好隐身（不会难看），但冷却转圈也永远不出现。

> ⚠️ 一个 `AbilitySet` 条目只能带一个 `SlotTag`。格挡现在应该还没有条目（走的是另一条路径），加的时候别顺手把它挂到六槽之一上。

### 15.3 为什么是 `bHideWhenUnavailable`，而不是"先不加这条"

因为**加被动那天你要回来改的地方不一样**：

- 先不加 → 以后加被动，要改 DA（加条目）+ 确认 Widget 分组 + 可能还要动布局；
- 现在预留 + 勾隐藏 → 以后加被动，**只在被动的 CD 配置那边指一下 `State.Cooldown.Passive`**，HUD 这边一个字不动。能力一授予，格子自己出现。

代价只有一个：DA 里多两条现在"看不见"的记录。这是划算的。

### 15.4 被动的 CD 怎么接（和主动技能完全同构）

被动一样走 GE 冷却那套，没有任何特殊路径：

1. 被动的 `UGameplayAbility::CooldownGameplayEffectClass` 指向一个 `GE_Cooldown_Passive`；
2. 那个 GE 的 `GrantedTags` 填 `State.Cooldown.Passive`（时长用 `SetByCaller` 或固定值）；
3. HUD 侧什么都不用做 —— `RebuildSlot` 已经在按 `CooldownTag` 查标签、按 `GetCooldownTimeRemainingAndDuration()` 采秒数。

**触发方式不同不影响显示**：主动技能是按键激活，被动可能是事件 / 定时器触发，但对 HUD 来说都是"某个时刻 `State.Cooldown.Passive` 挂上了，一段时间后摘掉"。

> 唯一的例外：如果被动用的是**自研的 timer 冷却**（不走 GAS 冷却 GE），HUD 就看不见 —— 因为 `QueryCooldown` 读的是 `Ability->GetCooldownTimeRemainingAndDuration()`，它只认 GE。要做那种被动，就得自己往 `FSkillSlotView` 推（或者改成 GE，推荐后者）。

### 15.5 Widget 侧已经处理好的两件事

不用改 `WBP_SkillSlot` 也能正确工作：

- **`bHidden` → `SetVisibility(Collapsed)`**（`UHeroSkillSlotWidget::ApplySlotView`）。恢复时回到的是**你在 WBP 里配的可见性**（首次应用时记下来的），不是硬写 `Visible` —— 所以 `HitTestInvisible` 之类不会被冲掉。
- **`Kind == Passive` → 强制清空 `KeyLabelText`**。被动按不出来，留着模板里的占位字母是错的；不管 `KeyLabel` 配了什么都会清掉。

想再加东西（被动图标加个"就绪"高光、格挡显示右键图标）走 `BP_OnSlotViewChanged(View)`，`View.Kind` 在里面读得到。

### 15.6 三个容易踩的坑

1. **`bHidden` 和 `State == Disabled` 不是一回事。** `Disabled` 说的是"槽位空"，UI 该画一个空框；`bHidden` 说的是"这条根本没打算让人看见"。两个判据分开写是有意的 —— 合并之后，以后调灰化优先级会顺手把"隐身槽位"的语义改掉。
2. **没能力 ≠ 该隐身。** 判据是 `IsReservedSlotHidden(Entry, bHasAbility)` = `Entry && Entry->bHideWhenUnavailable && !bHasAbility`。**没有条目时（越界 / 配置为空）不隐藏** —— 那是配置坏了，应该看得见才好排查，而不是安静地少一格。
3. **`Kind=Passive` 忘了配的表现**：格子上会多一个没有意义的键位字母（因为 `KeyLabel` 留空时 C++ 会保留 WBP 模板里的占位文字）。主动槽那条"只在 `KeyLabel` 非空时才写"的规则就是为了防止加载期闪空白，反而会在被动上暴露成这个症状。
