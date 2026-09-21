# 右键「格挡」完整实现方案

## 0. 一句话方案

**右键开一个由 GE 时长承载的窗口；所有伤害都走同一个伤害 GE，由 `UExecCalc_Damage` 唯一结算；结算时发现窗口还开着 → 判定格挡成功 → 摘窗口、吃掉这一击、挂免疫 GE。防护罩粒子挂在免疫 GE 的 GameplayCue 上，免疫到期 = 粒子消失。**

```
右键 ──► GA_Block ──► GE_Blocking（Duration = 窗口时长，授予 State.Blocking）
                            │
                            ▼  窗口开着 = 标签在
   ┌──────────────── 唯一伤害结算点 UExecCalc_Damage::Execute_Implementation ────────────────┐
   │  所有伤害都是 GE_Damage 的一发 Spec（近战 / 匕首 / 以后第 N 个技能，无一例外）              │
   │                                                                                        │
   │  ① 原始伤害 = 攻击者 AttackDamage × Data.DamageMultiplier + Data.FlatDamage              │
   │  ② 抗性减免：按 Spec 上的 Damage.Physical / Damage.Magic / Damage.True 挑护甲或魔抗，     │
   │     扣掉穿透后套 x/(x+100)；与其它减免逐项相乘                                         │
   │  ③ 格挡判定 UBlockComponent::TryMitigateIncomingDamage（静态入口，见 3.4）：             │
   │        有 State.BlockImmune ？ → 是 → 伤害 ×0，结束                                     │
   │        有 State.Blocking    ？ → 否 → 不减免，放行                                      │
   │                              → 是 → 格挡成功：                                          │
   │                                     ① 摘 GE_Blocking（窗口只吃一次）                     │
   │                                     ② 这一击伤害 × BlockedDamageMultiplier               │
   │                                     ③ 挂 GE_BlockImmune（授予 State.BlockImmune）        │
   │                                        └─ 上面挂着 GameplayCue.Block → GC_Block         │
   │                                             → 防护罩 NS 挂到角色身上，免疫结束自动消失    │
   │  ④ 把最终伤害写成目标 Health 的负向 modifier                                              │
   └────────────────────────────────────────────────────────────────────────────────────────┘
```

和隐身那套同构：**状态是 GE 标签（唯一事实来源），能力只负责提交 CD + 挂 GE，表现走 cue**。
唯一多出来的一条纪律：**伤害不许再在能力里算、不许再走 `ApplyPointDamage`，一律走 `GE_Damage`**（见 1.2）。

---

## 1. 为什么这么设计（以及为什么不那么做）

### 1.1 窗口时长 = `GE_Blocking` 的 Duration，不开任何定时器

判定就是一句 `HasMatchingGameplayTag(State.Blocking)`。窗口从 GE 挂上那一刻开始算，到期引擎自己摘。

对比「能力里开个 `FTimerHandle`」：定时器在客户端和服务端各跑一份、会漂移；能力 `EndAbility` 之后还得记得清；预测回滚时状态对不上。用 GE 时长这些问题全都不存在，而且是**网络同步的**（客户端预测挂上、服务端权威那份复制回来 catch-up 合并 —— 就是你们 `ApplyEmpoweredAttack` 注释里写的那套）。

> **联网时的边界：服务端判定宽限 RTT/2（已定，见 3.4）**
>
> 判定跑在服务端，而服务端的窗口从「`ServerTryActivateAbility` RPC 到达」那一刻才开始 —— 比客户端本地预测晚半个 RTT。于是「卡着窗口最后一帧按右键」在客户端看起来成功、在服务端会判失败。**放宽方式：服务端的判定边界在窗口关闭后再多认 RTT/2。**
>
> 关键在**宽限加在判定层，不是加在 GE 时长上**（`BlockWindow + RTT/2` 是错的写法）：
> - GE 时长是**两端各自施加**的。只给服务端那份加长，两端就出现时长不一致的预测副本，要靠 catch-up 去合并，合并不掉时客户端的 `State.Blocking` 标签会和服务端差半个 RTT。
> - 而宽限本来就是**纯服务端的事**：客户端不判伤害，标签多留半秒没有任何意义，只会让「举盾」架势多摆半个 RTT。
> - 所以 `GE_Blocking` 两端时长完全一致（都填 `BlockWindow`），宽限只活在 `UBlockComponent` 的判定里：**窗口关闭后 RTT/2 内到达的伤害照样算格挡成功**，且只认一次（见 3.4 的 `WindowCloseServerTime`）。
>
> **RTT 从哪来**：`APlayerState::GetPingInMilliseconds()`，**只在服务端有效** —— `ExactPing` 不是 UPROPERTY（不复制），客户端拿到的是 `CompressedPing * 4`（4ms 量化，还在 `bShouldUpdateReplicatedPing` 关掉时是脏值）。要加宽的就是**挨打那一方**的延迟，所以取格挡者自己的 PlayerState。
>
> **这套补偿补不到的**：伤害**比窗口先到**（`D < S`）的那半边。那要求服务端在伤害已经落地之后回溯撤销 —— 需要一份「最近 N 毫秒的伤害记录 + 回血」才能做，属于延迟补偿，这一轮不做（见第 8 节）。实际对局里玩家只能预判着按（看到挥砍再按必然晚一个 RTT），所以**主要是「按早了、窗口刚关」这一侧在丢判定，RTT/2 正好补的就是它**。
>
> **不要试图在客户端本地判定** —— 伤害只在服务端结算，本地判定只会制造两端不一致。

### 1.2 唯一伤害结算点 = `UExecCalc_Damage`（已定）

这是本方案唯一需要动架构的地方，也是「健壮」的落点。

**现状（动手前先看清楚，代码里查到的，不是推测）：**

| 事实 | 证据 |
|---|---|
| 近战今天**根本没走 GE**，100% 在跑 `ApplyPointDamage` 回退 | `DS_Passive.DamageEffect` 没赋值 —— 资产里搜不到任何 `GE_` 引用；`Content/LOL/Blueprints/GAS/GE/` 下只有三个冷却/瞄准 GE，没有伤害 GE |
| 匕首命中**不产生任何伤害** | `BP_GA_ThrowDagger.DamageGE` 也没赋值（只引用了瞄准 GE），`OnOverlap` 里 `if (DamageGE)` 直接跳过 |
| 项目里**没有任何生命属性** | 唯一的 `UAttributeSet` 子类是 `UHeroCombatAttributeSet`，只有攻速/攻击力五项；全项目搜不到 `Health` |

也就是说：**伤害管线现在是空的**。`ApplyPointDamage` 打的是 Variant_Combat 那套 `TakeDamage`（敌人/木桩有 `MaxHP`，没有 ASC）。格挡要「吃掉伤害」，前提是伤害真的有个落点。

**所以这一轮的改动是：把伤害管线立起来，并且只立一个结算点。**

- 新建 `UExecCalc_Damage`：**全项目唯一的伤害结算处** —— 算原始伤害、算抗性减免、调格挡判定、写出 Health 的负向 modifier。伤害公式只存在于这一个文件里。
- 新建 `UGE_Damage`：Instant GE，**没有任何 Modifier**，只挂 `Executions = [UExecCalc_Damage]`。它是所有伤害的唯一载体。
- `UBlockComponent` 仍然提供那个**静态入口**（3.4），但它**只被 ExecCalc 调用**，不再是「每个伤害点上贴三行」。

为什么仍然是静态入口而不是成员函数：沿用你们已经验证过的 `UMyAbilitySystemComponent::RemoveGrantedTagEffects` 的写法 —— **手里只有裸 ASC 指针也能用**。ExecCalc 里拿到的本来就是 `ExecutionParams.GetTargetAbilitySystemComponent()`，拿不到目标身上的组件引用。

```cpp
/** 就地改写 InOutDamage（乘以减免系数）。返回 true = 这一击被完全吃掉。 */
static bool TryMitigateIncomingDamage(UAbilitySystemComponent* TargetASC, AActor* DamageSource, float& InOutDamage);
```

**每个伤害点从此只需要做一件事：填 SetByCaller + 施加 `GE_Damage`。** 没有三行判定、没有分支、没有「忘了调」的可能 —— 漏调判定的唯一方式是「不走 GE_Damage 而是自己造伤害」，而那件事在代码里已经没有第二条路了。

> **代价（想清楚再动手）：近战不再能打到没有 ASC 的目标。**
> `ApplyPointDamage` 那条回退分支要删掉，Variant_Combat 的敌人/木桩（`ACombatCharacter` / `ACombatEnemy`，靠 `MaxHP` + `TakeDamage`）从此不吃近战伤害。这是「全部统一」的必然结果 —— 留着它，ExecCalc 就拦不住那条路，格挡对它们就是失效的，而且是静默失效。
> 真要保留它们，正确做法是给它们也挂 ASC + 属性集，而不是把回退分支留着。

> **ExecCalc 里调 ASC 改状态安全吗？安全，有引擎源码依据。**
> 格挡成功要摘 `GE_Blocking`、挂 `GE_BlockImmune` —— 这是在 ExecCalc 里改活动 GE 列表。`FActiveGameplayEffectsContainer::ApplyGameplayEffectSpec` 全程持 `GAMEPLAYEFFECT_SCOPE_LOCK()`（`FScopedActiveGameplayEffectLock`），而该锁的注释写得很清楚：
> *"This scope lock will queue deletions and additions until after the scope is over."*
> 即增删都会排队到本次施加结束后再真正落地，不会踩坏正在遍历的活动 GE 列表。所以判定整块搬进 ExecCalc 是安全的。
> 副作用要知道：**这两个状态变更在本次伤害 GE 应用完之前不会生效**（标签还在），但判定只做一次，不影响结果。

### 1.3 防护罩挂在 `GE_BlockImmune` 的 cue 上，不要手动 Spawn

`GE_BlockImmune` 上挂 `GameplayCue.Block`。于是：

- 免疫挂上 → `OnActive` → 生成防护罩
- 免疫到期/被驱散 → `OnRemove` → 收掉防护罩

**三者天然对齐，不需要任何外部清理调用。** 而且 cue 在每个客户端各跑一遍，天然多端一致。

对比「格挡成功那一刻 `ExecuteGameplayCue` 一次性炸一下」：那样防护罩是一次性的，还得自己写定时器管什么时候收，而且免疫被提前驱散时收不掉。

cue 用 **Actor 版**（`AGameplayCueNotify_Actor`，像 `AGC_Stealth` 那样）—— 因为防护罩要在免疫期间**持续存在**，需要一个组件持有它。一次性闪光才用 Static 版（像 `GC_EmpoweredHit`）。

### 1.4 免疫判定和格挡判定在同一个函数里

只有一个伤害入口，所以免疫不可能被漏掉。放格挡判定**前面**：免疫期间再挨打不该再触发一次格挡。

---

## 2. 代码改动清单

| 操作 | 文件 | 说明 |
|---|---|---|
| 改 | `Source/LOL/Public/GAS/HeroCombatAttributeSet.h` / `.cpp` | **补全属性集**（3.0，按 `属性集.txt`） |
| 新建 | `Source/LOL/Public/GAS/GE_Damage.h` / `.cpp` | **唯一伤害载体**：Instant、无 Modifier、只挂 ExecCalc |
| 新建 | `Source/LOL/Public/GAS/ExecCalc_Damage.h` / `.cpp` | **唯一伤害结算点**（3.1，核心） |
| 改 | `Source/LOL/Public/GAS/LOLGameplayTags.h` / `.cpp` | 加原生 tag（见下） |
| 新建 | `Source/LOL/Public/GAS/GE_Blocking.h` / `.cpp` | 格挡窗口（时长 = 窗口） |
| 新建 | `Source/LOL/Public/GAS/GE_BlockImmune.h` / `.cpp` | 免疫（时长 = 免疫时长，挂 cue） |
| 新建 | `Source/LOL/Public/GAS/GE_BlockCooldown.h` / `.cpp` | CD |
| 新建 | `Source/LOL/Public/GAS/GA_Block.h` / `.cpp` | 右键触发的薄能力 |
| 新建 | `Source/LOL/Public/GAS/GC_Block.h` / `.cpp` | 防护罩表现（Actor 版 cue） |
| 新建 | `Source/LOL/Public/GAS/BlockComponent.h` / `.cpp` | 减免判定（3.4） |
| 改 | `Source/LOL/Public/GAS/HeroCombatCharacter.h` / `.cpp` | 挂 `UBlockComponent` + 把 ASC 交给它绑定 |
| 改 | `Source/LOL/Private/GAS/GA_ThreeHitPassive.cpp` | 删 `ApplyPointDamage` 回退，改走 `GE_Damage`（3.6） |
| 改 | `Source/LOL/Private/GAS/ThrowDaggerProjectile.cpp` | 改走 `GE_Damage` 的同一套 SetByCaller（3.6） |
| 改 | `Source/LOL/Private/GAS/MyGameplayAbility.cpp` | 冷却吃 `AbilityHaste`（3.0） |

**`ALOLPlayerController` 不用改**：`SetupInputComponent` 里那个槽位循环是遍历 `AbilityInputConfig->AbilityInputActions` 绑的，数据资产加一条就自动接上。

### 新增原生 tag（不用改 `DefaultGameplayTags.ini`，和现有做法一致）

```
Data.DamageMultiplier       攻击力加成（SetByCaller，乘在攻击者 AttackDamage 上）
Data.FlatDamage             固定伤害（SetByCaller，直接加；和上面的倍率并存）
Data.BlockWindow            窗口时长（SetByCaller）
Data.BlockImmuneDuration    免疫时长（SetByCaller）
Damage.Physical             物理伤害（伤害 GE 的动态资产标签，见 3.1）
Damage.Magic                魔法伤害
Damage.True                 真实伤害（无视抗性）
State.Blocking              窗口开着（GE_Blocking 授予）
State.BlockImmune           免疫中（GE_BlockImmune 授予）
State.Cooldown.Block        CD
Ability.Slot.Block          右键槽位
GameplayCue.Block           → BP 必须命名为 GC_Block
```

> `Data.Damage`（老标签）**保留但不再有人填**：它的语义是「绝对伤害值」，新口径下伤害由 ExecCalc 算。留着只是不想为删一个 tag 触发全项目重编译，新代码一律不要用它。

> **右键目前是空闲的**：扫过 `Content` 下所有 `.uasset`，`RightMouseButton` 只在 `Variant_Combat/IMC_Combat`（你们没用到）里出现。`IMC_MouseLook` 绑的是 `Mouse2D` 不是 RMB，不冲突。

---

## 3. 逐文件要点

### 3.0 `UHeroCombatAttributeSet`：按 `属性集.txt` 补全

现在只有攻速/攻击力五项，伤害没有落点。按 `Desktop/属性集.txt` 补齐：

| 属性 | 基础 | 每级 | 说明 |
|---|---|---|---|
| `Health` / `MaxHealth` | 600 | +119 | ExecCalc 的唯一输出目标 |
| `HealthRegen` | 9 | +0.9 | **每 5 秒** |
| `Energy` / `MaxEnergy` | 200 | — | 消耗资源（`ManaCost` 目前是空的，等成本 GE） |
| `EnergyRegen` | 50 | — | **每 5 秒** |
| `AttackDamage` | 62 | +3.3 | 由 60 改为 62 |
| `BaseAttackSpeed` / `AttackSpeedRatio` | 0.625 | — | 由 0.658 改为 0.625 |
| `BonusAttackSpeedPercent` | 0 | +3.2% | 每级加 0.032 |
| `FinalAttackSpeed` | 派生 | — | `BaseAttackSpeed + AttackSpeedRatio × BonusAttackSpeedPercent` |
| `Armor` | 23 | +4.7 | 物理免伤 |
| `MagicResist` | 37 | +2.05 | 魔法免伤 |
| `MoveSpeed` | 345 | — | 目前只是数据，没接 `MaxWalkSpeed` |
| `AttackRange` | 125 | — | 目前只是数据 |
| `Omnivamp` / `LifeSteal` | 0 | — | 目前只是数据（回血逻辑见第 8 节） |
| `FlatArmorPen` / `PercentArmorPen` | 0 | — | 先百分比后固定 |
| `FlatMagicPen` / `PercentMagicPen` | 0 | — | 同上 |
| `Tenacity` | 0 | — | 控制时长 = `(1 - 韧性) × 原时长`，等控制效果 |
| `AbilityHaste` | 0 | — | **这一轮就接上**：CD = `CooldownDuration × 100 / (100 + AbilityHaste)` |
| `HealShieldPower` | 0 | — | 目前只是数据 |

要点：

- **每级成长的数值全部集中在 `.cpp` 的一张静态表里**（一个 `Base + Growth` 的结构数组），`ApplyChampionLevel(int32 Level)` 按等级重算基础值。等级系统还没有，默认 `1` 级（= 只吃基础值），行为和现在一致。
- **`PreAttributeChange` 里钳制**：`Health ∈ [0, MaxHealth]`、`Energy ∈ [0, MaxEnergy]`、攻速下限 0.01、攻击力/移速下限 0。**伤害扣血靠的就是 `Health` 的下限 0** —— 不钳制的话负血会一路传下去，HUD 上难看，之后做「死亡」判定也会判错。
- **改了攻速的基础值（0.658 → 0.625）要注意**：`DS_Passive.ReferenceAttackSpeed` 默认也是 0.658，`播放倍率 = FinalAttackSpeed / ReferenceAttackSpeed`。两边不同步的话，普攻动画会整体快 5%。**在编辑器里把 `DS_Passive` 的 `ReferenceAttackSpeed` 同步改成 0.625**，或者反过来把代码默认值留在 0.658 —— 二选一，别只改一边。
- `AbilityHaste` 接在 `UMyGameplayAbility::ApplyCooldown` 里（`CooldownDuration` 是 SetByCaller 填进去的，所以把填进去的值先算一遍就行）。**属性集拿不到时按 0 处理**（返回原 CD），不要让「PlayerState 还没就绪」变成「技能没 CD」。

### 3.1 `UGE_Damage` + `UExecCalc_Damage`（核心）

**`UGE_Damage`：Instant、一个 Modifier 都不配，只挂 ExecCalc。**

```cpp
UGE_Damage::UGE_Damage(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
    DurationPolicy = EGameplayEffectDurationType::Instant;

    // 唯一的作用：把伤害交给 UExecCalc_Damage 结算。
    // 这里【不】配任何 Modifier —— 伤害不是一条固定的 -X 曲线，它要经过
    // 「攻击力 × 倍率 + 固定值 → 抗性 → 格挡」这一串计算，只有 ExecCalc 拿得到全部输入。
    FGameplayEffectExecutionDefinition Execution;
    Execution.CalculationClass = UExecCalc_Damage::StaticClass();
    Executions.Add(Execution);
}
```

> **Instant + 只有 ExecCalc、没有 Modifier** 是完全合法的：ExecCalc 通过 `OutExecutionOutput.AddOutputModifier` 自己吐 modifier。这也是 Lyra / ActionRPG 的标准写法。

**`UExecCalc_Damage`：伤害公式的唯一住处。**

```cpp
UCLASS()
class LOL_API UExecCalc_Damage : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()
public:
    UExecCalc_Damage();
    // 注意是 Execute_Implementation（蓝图原生事件），不是 Execute —— 覆写 Execute 会编译不过。
    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
        FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};
```

构造函数里**必须**把要读的属性登记成捕获定义，否则读出来恒为 0（静默，见下面的坑）：

```cpp
struct FDamageStatics
{
    DECLARE_ATTRIBUTE_CAPTUREDEF(AttackDamage);
    DECLARE_ATTRIBUTE_CAPTUREDEF(Armor);
    DECLARE_ATTRIBUTE_CAPTUREDEF(MagicResist);
    DECLARE_ATTRIBUTE_CAPTUREDEF(FlatArmorPen);
    DECLARE_ATTRIBUTE_CAPTUREDEF(PercentArmorPen);
    DECLARE_ATTRIBUTE_CAPTUREDEF(FlatMagicPen);
    DECLARE_ATTRIBUTE_CAPTUREDEF(PercentMagicPen);

    FDamageStatics()
    {
        // 攻击力从【施加者】身上抓，其余全部从【目标】身上抓。
        DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, AttackDamage, Source, false);
        DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, Armor, Target, false);
        ...
    }
};

static const FDamageStatics& DamageStatics()
{
    static FDamageStatics Statics;
    return Statics;
}

UExecCalc_Damage::UExecCalc_Damage()
{
    RelevantAttributesToCapture.Add(DamageStatics().AttackDamageDef);
    RelevantAttributesToCapture.Add(DamageStatics().ArmorDef);
    ...
}
```

`Execute_Implementation` 的骨架：

```cpp
void UExecCalc_Damage::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
    FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    UAbilitySystemComponent* SourceASC = ExecutionParams.GetSourceAbilitySystemComponent();
    UAbilitySystemComponent* TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();
    const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
    if (!SourceASC || !TargetASC) return;

    FAggregatorEvaluateParameters EvalParams;
    EvalParams.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
    EvalParams.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

    auto Capture = [&](const FGameplayEffectAttributeCaptureDefinition& Def, bool bSource) -> float
    {
        float Value = 0.f;
        ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(Def, EvalParams, Value);
        return Value;
    };

    // ① 原始伤害 = 攻击者攻击力 × 攻击力加成 + 固定伤害
    //    两个 SetByCaller 都【必须给默认值 0】：没填过的话 GetSetByCallerMagnitude 会打一条 warning 并返回默认值。
    const float AttackDamage = Capture(DamageStatics().AttackDamageDef, true);
    const float Multiplier   = Spec.GetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, false, 0.f);
    const float FlatDamage   = Spec.GetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, false, 0.f);
    float Damage = AttackDamage * Multiplier + FlatDamage;

    // ② 抗性减免：伤害类型由施加方打在 Spec 的动态资产标签上（同一个 GE 服务所有伤害，靠标签分流）
    const FGameplayTagContainer& AssetTags = Spec.GetDynamicAssetTags();
    if (!AssetTags.HasTag(LOLGameplayTags::Damage_True))
    {
        const bool bPhysical = !AssetTags.HasTag(LOLGameplayTags::Damage_Magic);
        float Resist  = Capture(bPhysical ? ...ArmorDef : ...MagicResistDef, false);
        const float FlatPen = Capture(bPhysical ? ...FlatArmorPenDef : ...FlatMagicPenDef, false);
        const float PctPen  = Capture(bPhysical ? ...PercentArmorPenDef : ...PercentMagicPenDef, false);

        // 先百分比、后固定
        Resist = Resist * (1.f - FMath::Clamp(PctPen, 0.f, 1.f)) - FlatPen;
        // x/(x+100) 的免伤 → 承伤系数。抗性为负时用 LoL 的延伸公式，否则 x 接近 -100 会除爆。
        const float DamageTaken = Resist >= 0.f ? 100.f / (100.f + Resist) : 2.f - 100.f / (100.f - Resist);
        Damage *= DamageTaken;
    }

    // ③ 格挡 / 免疫 —— 全项目唯一的减免入口，就这一行
    UBlockComponent::TryMitigateIncomingDamage(TargetASC, Spec.GetContext().GetEffectCauser(), Damage);

    // ④ 落地
    if (Damage > 0.f)
    {
        OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
            UHeroCombatAttributeSet::GetHealthAttribute(), EGameplayModOp::Additive, -Damage));
    }

    UE_LOG(LogTemp, Warning, TEXT("[Damage] 攻=%.1f 倍率=%.2f 固定=%.1f → 结算=%.1f 目标=%s"),
        AttackDamage, Multiplier, FlatDamage, Damage, *GetNameSafe(TargetASC->GetAvatarActor()));
}
```

坑（都是静默的，全部要说清楚）：

1. **`RelevantAttributesToCapture` 忘了登记 = 读出来恒为 0，不报错。** 表现是「伤害永远只有固定值那一部分」或者「抗性永远不减伤」。改了这个结构体记得同步构造函数。
2. **`GetSetByCallerMagnitude` 不带默认值**在没填过时会打 warning 并返回 0 —— 传 `false, 0.f` 明确表示「允许没填」。
3. **伤害类型走动态资产标签**，不是给每种伤害建一个 GE。`Spec.AddDynamicAssetTag()` 是引擎提供的入口（`FGameplayEffectSpec`），ExecCalc 里用 `GetDynamicAssetTags()` 读。**没打任何类型标签时按物理处理**（近战是绝大多数）。
4. **`Spec.GetContext().GetEffectCauser()` / `GetInstigatorAbilitySystemComponent()` 必须指向真正的施加者**：`MakeOutgoingSpec` 出来的 Spec，源和目标**全部**由 EffectContext 决定（`FGameplayEffectSpec` 构造函数只存 context，不存 ASC）。`UAbilitySystemComponent::MakeEffectContext()` 已经 `AddInstigator(AbilityActorInfo->OwnerActor, AbilityActorInfo->AvatarActor)`，所以在**施加者自己的 ASC** 上调它 → instigator=施加者 PlayerState、causer=施加者角色，近战什么都不用补。
   ⚠️ 反例是匕首：它在 `TargetASC->MakeEffectContext()`（**目标的** ASC）上造 context，拿到的是目标自己，必须紧跟一句 `Context.AddInstigator(GetInstigator(), this)` 覆盖掉，否则 ExecCalc 会拿**被打的人**的攻击力算伤害。判据是「这一句的 ASC 是谁的」，不是「有没有调 AddInstigator」。
5. **`Health` 被扣到负数没关系**：`PreAttributeChange` 会钳到 0。别在 ExecCalc 里再钳一次，两处钳制迟早对不上。

### 3.2 `UGE_Blocking`（照抄 `UGE_Stealth` 的写法）

```cpp
UGE_Blocking::UGE_Blocking(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
    DurationPolicy = EGameplayEffectDurationType::HasDuration;

    // 窗口时长由 GA_Block 填（SetByCaller 没有默认值，谁都不填 = 时长 0 = 立刻过期）
    FSetByCallerFloat CallerMagnitude;
    CallerMagnitude.DataTag = LOLGameplayTags::Data_BlockWindow;
    DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

    UTargetTagsGameplayEffectComponent* TargetTags =
        ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
    GEComponents.Add(TargetTags);
    FInheritedTagContainer GrantedTags;
    GrantedTags.AddTag(LOLGameplayTags::State_Blocking);
    TargetTags->SetAndApplyTargetTagChanges(GrantedTags);

    // 可选：「举盾」架势的持续表现放这里（施法蒙太奇不放这儿，见 3.4）
}
```

### 3.3 `UGE_BlockImmune`

和上面同构，只是 `DataTag = Data_BlockImmuneDuration`、授予 `State.BlockImmune`，外加：

```cpp
    // 防护罩挂在这里：免疫挂上 → OnActive，免疫到期/被驱散 → OnRemove。引擎最清楚什么时候该收。
    GameplayCues.Add(FGameplayEffectCue(LOLGameplayTags::GameplayCue_Block, 0.f, 0.f));
```

### 3.4 `UBlockComponent`（核心）

```cpp
UCLASS(ClassGroup=(LOL), meta=(BlueprintSpawnableComponent))
class LOL_API UBlockComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UBlockComponent();

    /** 格挡成功时挂的免疫 GE。默认 UGE_BlockImmune。 */
    UPROPERTY(EditDefaultsOnly, Category="Block") TSubclassOf<UGameplayEffect> ImmuneEffect;

    /** 免疫持续时间（秒）。 */
    UPROPERTY(EditDefaultsOnly, Category="Block", meta=(ClampMin="0")) float ImmuneDuration = 1.5f;

    /** 被格挡那一击的伤害倍率（作为减免系数乘上去）。0 = 完全抵消（默认）。 */
    UPROPERTY(EditDefaultsOnly, Category="Block", meta=(ClampMin="0")) float BlockedDamageMultiplier = 0.f;

    /** 免疫期间的伤害倍率。0 = 完全免伤（默认）。 */
    UPROPERTY(EditDefaultsOnly, Category="Block", meta=(ClampMin="0")) float ImmuneDamageMultiplier = 0.f;

    /** 是否只挡正面来的伤害。默认关（宽松 > 误判）。 */
    UPROPERTY(EditDefaultsOnly, Category="Block|Facing") bool bRequireFacingAttacker = false;
    UPROPERTY(EditDefaultsOnly, Category="Block|Facing",
        meta=(ClampMin="0", ClampMax="180", EditCondition="bRequireFacingAttacker"))
    float FacingHalfAngleDeg = 90.f;

    // ---- 联网宽限（见 1.1）----

    /** 服务端判定宽限：窗口关闭后再多认 RTT/2。关掉 = 严格按 GE 时长判。 */
    UPROPERTY(EditDefaultsOnly, Category="Block|Net") bool bCompensatePing = true;

    /** 宽限上限（秒）。防止高延迟/异常 ping 把「窗口」拉成一个可以常驻的状态。 */
    UPROPERTY(EditDefaultsOnly, Category="Block|Net", meta=(ClampMin="0", Units="s"))
    float MaxGraceSeconds = 0.15f;

    /**
     * ★ 唯一减免入口。只由 UExecCalc_Damage 调用，不要在任何伤害点手动调（见 1.2）。
     *
     * 【故意不是 UFUNCTION】：暴露给蓝图等于开了第二个减免入口 —— 有人从 BP 里调一次，
     * 伤害就被减两遍（或者绕开 ExecCalc 的顺序）。这是结算管线内部的一步，不是工具箱。
     */
    static bool TryMitigateIncomingDamage(UAbilitySystemComponent* TargetASC, AActor* DamageSource, float& InOutDamage);

    /**
     * 把 ASC 交给组件并开始跟踪窗口开关时刻。
     * 由 AHeroCombatCharacter::InitializeAbilityActorInfo 调 —— ASC 是 PlayerState 上来的，
     * 组件自己 BeginPlay 时它可能还没就绪（PossessedBy / OnRep_PlayerState 的时序不保证）。
     */
    void BindToAbilitySystem(UAbilitySystemComponent* InASC);

private:
    bool OnBlockSucceeded(AActor* DamageSource, float& InOutDamage);
    bool IsInFront(AActor* DamageSource) const;
    float GetGraceSeconds() const;
    static UBlockComponent* FindOn(UAbilitySystemComponent* ASC);

    /** State.Blocking 计数变化：0→1 开窗，1→0 记下关闭时刻并放开一次宽限。 */
    void OnBlockingTagChanged(const FGameplayTag Tag, int32 NewCount);

    UPROPERTY(Transient) TObjectPtr<UAbilitySystemComponent> CachedASC;
    FDelegateHandle BlockingTagDelegateHandle;

    /** 窗口关闭的服务端世界时刻。宽限判定的起点。 */
    float WindowCloseServerTime = -FLT_MAX;

    /** 这次关窗之后的那一份宽限还没被用掉。用掉即作废，防止一次窗口连挡两下。 */
    bool bGraceAvailable = false;
};
```

`.cpp` 主体：

```cpp
bool UBlockComponent::TryMitigateIncomingDamage(UAbilitySystemComponent* TargetASC, AActor* DamageSource, float& InOutDamage)
{
    // 契约：InOutDamage 是唯一输出，函数一定就地改写它。
    // 返回值只是给调用方（ExecCalc）判断「要不要再输出 modifier」用的快捷方式，不要只看返回值。
    if (!TargetASC || InOutDamage <= 0.f) return false;

    UBlockComponent* Block = FindOn(TargetASC);
    if (!Block) return false;   // 没挂组件 = 这个角色根本不会格挡/免疫，不需要任何特判

    // 1) 免疫优先：免疫期间再挨打不该再触发一次格挡。
    if (TargetASC->HasMatchingGameplayTag(LOLGameplayTags::State_BlockImmune))
    {
        InOutDamage *= Block->ImmuneDamageMultiplier;
        return InOutDamage <= 0.f;
    }

    // 2) 窗口：标签在 = 窗口还开着（时长就是 GE_Blocking 的 Duration）。
    const bool bWindowOpen = TargetASC->HasMatchingGameplayTag(LOLGameplayTags::State_Blocking);

    // 3) 宽限：窗口刚关 RTT/2 内到达的伤害也算挡到（见 1.1）。
    //    这一层的存在意义就是「判定比 GE 时长宽一点」，所以它必须活在这里、不能靠把 GE 时长加长。
    bool bInGrace = false;
    if (!bWindowOpen && Block->bGraceAvailable)
    {
        const UWorld* World = Block->GetWorld();
        const float Now = World ? World->GetTimeSeconds() : 0.f;
        bInGrace = (Now - Block->WindowCloseServerTime) <= Block->GetGraceSeconds();
    }

    if (!bWindowOpen && !bInGrace) return false;

    if (Block->bRequireFacingAttacker && !Block->IsInFront(DamageSource))
    {
        return false;
    }

    // 宽限被用掉就作废：一次窗口只挡一下。
    if (bInGrace)
    {
        Block->bGraceAvailable = false;
    }

    return Block->OnBlockSucceeded(DamageSource, InOutDamage);
}

float UBlockComponent::GetGraceSeconds() const
{
    if (!bCompensatePing) return 0.f;

    // 取【挨打那一方】的 ping。GetPingInMilliseconds 只在服务端返回真实值（ExactPing），
    // 但整个判定也只在服务端跑，所以没问题；没有 PlayerState（AI/训练假人）就当 0 延迟。
    const APawn* Pawn = Cast<APawn>(GetOwner());
    const APlayerState* PS = Pawn ? Pawn->GetPlayerState() : nullptr;
    const float PingSeconds = PS ? PS->GetPingInMilliseconds() * 0.001f : 0.f;

    return FMath::Clamp(PingSeconds * 0.5f, 0.f, MaxGraceSeconds);
}

void UBlockComponent::BindToAbilitySystem(UAbilitySystemComponent* InASC)
{
    if (CachedASC == InASC) return;
    if (CachedASC && BlockingTagDelegateHandle.IsValid())
    {
        CachedASC->UnregisterGameplayTagEvent(BlockingTagDelegateHandle,
            LOLGameplayTags::State_Blocking, EGameplayTagEventType::NewOrRemoved);
        BlockingTagDelegateHandle.Reset();
    }

    CachedASC = InASC;
    if (!CachedASC) return;

    BlockingTagDelegateHandle = CachedASC->RegisterGameplayTagEvent(
        LOLGameplayTags::State_Blocking, EGameplayTagEventType::NewOrRemoved)
        .AddUObject(this, &UBlockComponent::OnBlockingTagChanged);
}

void UBlockComponent::OnBlockingTagChanged(const FGameplayTag Tag, int32 NewCount)
{
    if (NewCount > 0)
    {
        // 开窗：宽限作废（还在窗口里，用不上）。
        bGraceAvailable = false;
        return;
    }

    // 关窗：记下时刻并放开一次宽限。GetWorld()->GetTimeSeconds() 两端各自算各自的，
    // 但只有服务端那份会被读到（判定只在服务端跑）。
    if (const UWorld* World = GetWorld())
    {
        WindowCloseServerTime = World->GetTimeSeconds();
        bGraceAvailable = true;
    }
}

bool UBlockComponent::OnBlockSucceeded(AActor* DamageSource, float& InOutDamage)
{
    UAbilitySystemComponent* ASC = CachedASC;
    // 只在权威端结算。非权威端必须【早退且不改数值】：本地把伤害改成 0，服务端照扣，两边就不一致了。
    if (!ASC || !ASC->IsOwnerActorAuthoritative()) return false;

    // ① 窗口只吃一次：立刻摘掉 GE_Blocking。
    //    复用你们那个「非权威端也真的摘」的入口，客户端那份预测副本走同一条路。
    //    ⚠️ 这里是【在 ExecCalc 里】改活动 GE 列表：安全，ApplyGameplayEffectSpec 全程持
    //    FScopedActiveGameplayEffectLock，增删会排队到本次施加结束后落地（见 1.2 的引擎依据）。
    UMyAbilitySystemComponent::RemoveGrantedTagEffects(
        ASC, FGameplayTagContainer(LOLGameplayTags::State_Blocking));

    // ② 这一击的减免
    InOutDamage *= BlockedDamageMultiplier;

    // ③ 挂免疫（刷新式：先摘后挂，和 ApplyEmpoweredAttack 一致，任何时刻最多一层）
    if (ImmuneEffect)
    {
        FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
        Context.AddSourceObject(this);
        Context.AddInstigator(GetOwner(), GetOwner());
        FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(ImmuneEffect, 1.f, Context);
        if (Spec.IsValid())
        {
            Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_BlockImmuneDuration, ImmuneDuration);
            UMyAbilitySystemComponent::RemoveGrantedTagEffects(
                ASC, FGameplayTagContainer(LOLGameplayTags::State_BlockImmune));
            ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
        }
        else
        {
            // 不静默：免疫挂不上 = 防护罩不出现、后续伤害也不免，屏幕上看起来就是「格挡没生效」。
            UE_LOG(LogTemp, Warning, TEXT("[Block] 免疫 GE Spec 无效（%s）→ 本次格挡没有免疫"), *GetNameSafe(ImmuneEffect));
        }
    }

    UE_LOG(LogTemp, Warning, TEXT("[Block] 格挡成功: 剩余伤害=%.1f 免疫=%.2fs 来源=%s"),
        InOutDamage, ImmuneDuration, *GetNameSafe(DamageSource));

    return InOutDamage <= 0.f;
}

UBlockComponent* UBlockComponent::FindOn(UAbilitySystemComponent* ASC)
{
    // 注意是 AvatarActor 不是 OwnerActor：ASC 挂在 PlayerState 上，
    // 组件挂在角色身上，走 OwnerActor 永远找不到。
    AActor* Avatar = ASC ? ASC->GetAvatarActor() : nullptr;
    return Avatar ? Avatar->FindComponentByClass<UBlockComponent>() : nullptr;
}
```

> `ApplyGameplayEffectSpecToSelf` 不带预测键 —— 服务端权威端 `HasNetworkAuthorityToApplyGameplayEffect` 恒真，没问题。

同时在 `AHeroCombatCharacter` 里加两处：

```cpp
// 构造函数
BlockComponent = CreateDefaultSubobject<UBlockComponent>(TEXT("BlockComponent"));

// InitializeAbilityActorInfo() 末尾（ASC 拿到之后）
if (BlockComponent) { BlockComponent->BindToAbilitySystem(AbilitySystemComponent); }
```

> **`FindOn` 为什么不用 `GetOwnerActor()`**：ASC 挂在 `AMyPlayerState` 上（`AbilitySystemComponent` 是 PlayerState 的成员），而 `UBlockComponent` 挂在角色上。用 `GetOwnerActor()` 找组件会永远返回 null —— 而且是静默的（`FindComponentByClass` 找不到就返回 null，格挡从此再也不生效）。

### 3.5 `UGA_Block`

```cpp
UGA_Block::UGA_Block()
{
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;   // 和 Flash/Stealth 一致
    ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

    bBreaksStealthOnCast = true;   // 默认值就是 true，格挡算「施放技能」→ 会破隐

    BlockingGE = UGE_Blocking::StaticClass();
    CooldownDuration = 6.f;
    CooldownGameplayEffectClass = UGE_BlockCooldown::StaticClass();

    BlockWindow    = 0.4f;   // 窗口  (测试值，BP 子类里调)
    BlockImmune    = 1.5f;   // 免疫  ← 注意：只有免疫时长在 BlockComponent 上，能力不碰
}
```

`ActivateAbility` 就是 `GA_Stealth` 的骨架：

```cpp
if (!CommitAbility(...)) { EndAbility(..., true); return; }

// 开窗：GE 上挂了 cue（可选），窗口时长由 SetByCaller 填，GE 授予 State.Blocking。
FGameplayEffectSpecHandle Spec = MakeOutgoingGameplayEffectSpec(BlockingGE, GetAbilityLevel());
if (!Spec.IsValid()) { EndAbility(..., true); return; }
Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_BlockWindow, BlockWindow);
ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, Spec);

// 施法动作留在能力里（理由和 GA_Stealth 一样：要跟着预测时机在本地立刻播，不受 cue 的网络路径影响）
if (BlockMontage) { Character->PlayAnimMontage(BlockMontage); }

// 能力立刻结束：窗口的状态全在 GE 上，能力不需要活到窗口关闭。
EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
```

> **动画素材已经有了**：`/Game/ParagonKallari/Characters/Heroes/Kallari/Animations/Ability_Block`（是 `AnimSequence`，要像 `AM_ThrowDagger` 那样包一层 AnimMontage）。音效：`/Game/ParagonKallari/Audio/Cues/Kallari_Effort_Block`。

### 3.6 两个伤害点的改动

两个伤害点都**不再自己算伤害、也不再调格挡**（格挡是 ExecCalc 内部第 ③ 步，见 1.2）。它们现在只干一件事：**把「原始伤害参数」塞进 `GE_Damage` 的 SetByCaller，扔给 ExecCalc**。

#### 近战：`UGA_ThreeHitPassive::ApplyServerHit`

删掉 `ApplyPointDamage` 回退分支和本地 `Damage` 计算，循环体变成：

```cpp
for (const FHitResult& Hit : Hits)
{
    AActor* Target = Hit.GetActor();
    if (!Target) continue;

    UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
    // 没有 ASC 的目标【不再造成伤害】：伤害已经统一到 GE_Damage + ExecCalc，
    // 没有 ASC 就没有执行点。原来的 ApplyPointDamage 回退分支删掉（见 1.2）。
    if (TargetASC && PassiveData && PassiveData->DamageEffect)
    {
        FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
        Context.AddHitResult(Hit);
        // 不补 AddInstigator：MakeEffectContext 在【施加者自己的 ASC】上调，
        // 已经把 instigator 设成 PlayerState、causer 设成角色（见下面的坑 4）。

        FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(
            PassiveData->DamageEffect, GetAbilityLevel(), Context);
        if (Spec.IsValid())
        {
            // ① 攻击者面板攻击力由 ExecCalc 自己捕获，这里【不传】——
            //    传进去就等于伤害公式有两份真相，改一处漏一处。
            // ② 这里只传「本次攻击的倍率」和「额外固定值」。
            Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier,
                Stage.DamageMultiplier * (bStageEmpowered ? PassiveData->EmpowerDamageMultiplier : 1.f));
            Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, 0.f);

            SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
        }
        else
        {
            // 不静默：Spec 无效 = 这一击一点伤害都没有，屏幕上看起来像「打空了」。
            UE_LOG(LogTemp, Warning, TEXT("[Melee] DamageEffect(%s) 的 Spec 无效 → 本次命中无伤害"),
                *GetNameSafe(PassiveData->DamageEffect));
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[Melee] 目标 %s 没有 ASC（或 DamageEffect 没配）→ 本次命中无伤害"),
            *GetNameSafe(Target));
    }

    /* ...原有的击退 LaunchCharacter 保持不动... */
}
```

改动清单：

| 原来 | 现在 |
|---|---|
| 本地算 `const float Damage = AttackDamage * Stage.DamageMultiplier * ...` | 删掉。倍率走 `Data.DamageMultiplier` |
| `const float Damage` 的 `const` | 不用改了，整个变量没了 |
| `if (!TargetASC \|\| !PassiveData->DamageEffect) { ApplyPointDamage(...); continue; }` | 删掉分支，改成打警告日志 |
| `SetSetByCallerMagnitude(DamageSetByCallerTag, Damage)` | 换成 `Data.DamageMultiplier` + `Data.FlatDamage` 两条 |
| `Context` 只有 `AddHitResult` | 不动。`MakeEffectContext` 已经补好 instigator/causer（坑 4） |

> `DamageSetByCallerTag` 这个 UPROPERTY 就没用了 —— 一并删掉，别留着让人以为还有第二条路。

#### 远程：`AThrowDaggerProjectile::OnOverlap`

```cpp
if (DamageGE && TargetASC)
{
    FGameplayEffectContextHandle Context = InstigatorASC->MakeEffectContext();
    Context.AddHitResult(Hit);
    Context.AddInstigator(GetInstigator(), this);

    FGameplayEffectSpecHandle Spec = InstigatorASC->MakeOutgoingSpec(DamageGE, 1.f, Context);
    if (Spec.IsValid())
    {
        // 匕首是纯固定伤害：倍率给 0（不吃攻击力），数值走 FlatDamage。
        // 想让匕首也吃攻击力加成，就在这里填倍率（=AD 系数），别改 ExecCalc 的公式。
        Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, 0.f);
        Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, DamageAmount);
        InstigatorASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);
    }
}
```

> 近战走倍率、远程走固定值，**两条路共用同一个 ExecCalc 公式**：`原始 = AttackDamage × 倍率 + 固定值`。这就是「统一」的落点 —— 以后加新技能只要挑一种喂参数，不用再碰伤害公式。

---

## 4. 材质 → Niagara：怎么做

### 4.1 先搞清楚一个问题：材质本身不用变

「把材质变成 Niagara 粒子」不需要转换材质。变的只有两件事：

1. **它在 Niagara 里挂到哪个 Renderer 上**（决定它是「贴在几何体上的罩子」还是「永远朝屏幕的贴片」）
2. **勾哪个 usage 标志**（不勾编辑器里通常也能看到，打包后容易丢 shader）

然后通过 Renderer 的 **Dynamic Material Parameters** 让 Niagara 的参数驱动材质里的 Scalar/Vector 参数 —— 这才是「材质变成粒子」的核心机制。

### 4.2 先确认材质的域

打开材质 → Details → **Material Domain 必须是 `Surface`**。`Deferred Decal` / `Light Function` / `Post Process` / `UI` 这几个域在 Niagara 的 renderer 上**不显示**（不报错，就是什么都没有）。

顺带一提，你们 `Content/LOL/Material` 下现有的三个：

| 材质 | 用途 |
|---|---|
| `Niagara/M_Glow` | 有 `ParticleColor` + `DynamicParameter` 节点 → **是 Niagara sprite 材质**，`NS_PerfectSuccess` 正在用 |
| `M_StealthHero` | `Fresnel` + `Panner` + `TextureSample` → Surface 域涂层材质 |
| `M_Test` | 只有一个 `VectorParameter` → 最简单的 Surface 材质 |

你说的那个防护罩材质如果是新建在别处的，按下面 4.5 的清单对一遍。

### 4.3 做法 A：球形 / 半球防护罩（推荐）

**现成的网格素材，不用自己做：**

| 网格 | 路径 |
|---|---|
| 半球罩（最像防护罩） | `/Game/ParagonSparrow/FX/Meshes/Heroes/SM_Sparrow_SlowDome` |
| 半球 | `/Game/ParagonSunWukong/FX/Meshes/Shapes/SM_HalfSphere_100` |
| 单位球（半径 100cm @ scale 1） | `/Game/ParagonSunWukong/FX/Meshes/Shapes/SM_Sphere_Scale_Unit_100` |
| 冲击环（消失时的爆环） | `/Game/ParagonSunWukong/FX/Meshes/Shapes/SM_ShockRing` |

**步骤：**

1. 新建 `NS_BlockShield`
2. 加一个 emitter，**关键设置（这是个必踩的坑）**：
   `Emitter State` → **`Life Cycle Mode = Infinite`**。
   默认是 `Once`，emitter 播完就停了 —— 免疫还没结束罩子就没了。
3. **Renderer = `Mesh Renderer`**：
   - `Mesh` = `SM_Sparrow_SlowDome`
   - `Material` = 你的材质
   - `Sort Mode` = `Never`（透明罩子别参与半透明排序，否则和角色穿插闪烁）
   - `Cast Shadows` = `false`
   - `Scale` 绑 `Particles.Scale`，在 `Initialize Particle` 里给个能包住角色的值（Kallari 高约 180cm，罩子半径 60~80cm 好看）
   - pivot 不在中心的话用 renderer 的 `Pivot Offset` 修正
4. **让罩子跟着角色走**：NS 的 `Emitter Properties` → **`Local Space = true`**；cue 里用
   `UNiagaraFunctionLibrary::SpawnSystemAttached(System, Character->GetMesh(), NAME_None, FVector::ZeroVector, FRotator::ZeroRotator, EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false)`
5. **让材质动起来**（可选但强烈建议）：
   - NS 里加一个 `User.ShieldOpacity`（float，默认 1）
   - Mesh Renderer → `Dynamic Material Parameters` → 加一条：`Parameter Name` = 材质里那个 ScalarParameter 的名字（比如 `Opacity`），`Binding` = `User.ShieldOpacity`
   - 材质里把 `Fresnel` 的 `OneMinus` 接 Opacity → **边缘实、中间透**，这就是防护罩最关键的观感。
     （你们的 `M_StealthHero` 里已经在用 `Fresnel`，节点怎么接可以直接照抄）
6. **出现/消失**：
   - 出现：`Initialize Particle` 的 `Scale` 用曲线从 0.5 弹到 1.0（「撑开」的感觉）
   - 消失：cue 的 `OnRemove` 里把 `User.ShieldOpacity` 从 1 插值到 0，播完 `DestroyComponent`；或者先简单点，直接销毁跑通再做好看

### 4.4 做法 B：Sprite 光晕（做「格挡成功那一下」的闪光）

Renderer = `Sprite Renderer`，材质直接用 `M_Glow`（`NS_PerfectSuccess` 就是这么用的，照抄即可）。

适合一次性反馈，**不适合当持续防护罩**（永远朝相机的贴片包不住角色）。

### 4.5 做法 C：干脆不进 Niagara，直接上球体组件

`AGC_Block::OnActive` 里 `NewObject<UStaticMeshComponent>` + `SetStaticMesh(SM_Sparrow_SlowDome)` + 你的材质 + `CreateDynamicMaterialInstance` 做渐隐。

好处：材质直接能用、零 Niagara 配置。坏处：没有粒子感、不会自己消散。
**先用这个把整条链跑通，再迭代成做法 A** 是完全可以的。

### 4.6 静默失效自查清单

按你们 `AGC_Stealth::LogOverlayDiagnostic` 那套思路，在 `AGC_Block::OnActive` 里对防护罩材质打一行诊断（`MaterialShared.h` 你们在 `GC_Stealth.cpp` 里已经 include 过了）：

```cpp
UE_LOG(LogTemp, Warning,
    TEXT("[Block] 防护罩材质 %s: 域=%s(须 Surface) 混合模式=%s 着色模型=%s NiagaraMesh用法=%d NiagaraSprite用法=%d"),
    *GetNameSafe(ShieldMaterial),
    Base ? *MaterialDomainString(Base->MaterialDomain) : TEXT("无"),
    *BlendName, *ShadingModelName,
    ShieldMaterial->CheckMaterialUsage_Concurrent(MATUSAGE_NiagaraMeshParticles) ? 1 : 0,
    ShieldMaterial->CheckMaterialUsage_Concurrent(MATUSAGE_NiagaraSprites) ? 1 : 0);
```

四道看不见的门，任何一道没过都是「什么都不显示、也不报错」：

1. **域不是 `Surface`** → renderer 直接跳过
2. **没勾对应的 usage**：Mesh Renderer 要勾 `Used with Niagara Mesh Particles`，Sprite Renderer 要勾 `Used with Niagara Sprites`
3. **着色模型不是 `Unlit`** —— 尤其注意 Substrate 下的 `Thin Translucent`（你们在隐身涂层上已经踩过这个坑，`GC_Stealth` 里那段注释就是）：要有 Front Material 节点才算「有表面」，没接的话前面全绿、屏幕上依然什么都没有
4. **改了 usage / 着色模型之后没重新编译保存材质**，编辑器里看的还是旧的

---

## 5. 编辑器配置步骤

### 5.1 输入

1. 新建 `IA_Block`（Enhanced Input Action，`Value Type = Digital (bool)`，按键 `RightMouseButton`）
2. `IMC_Default` 加一条映射：`IA_Block` → `RightMouseButton`
3. `IC_Ability` 加一条：`InputAction = IA_Block`，`SlotTag = Ability.Slot.Block`

> `ALOLPlayerController` 不用动 —— 槽位绑定是数据驱动遍历的。

### 5.2 技能授权

`AS_HeroKit` 的 `GrantAbility` 加一条：

- `SlotTag` = `Ability.Slot.Block`
- `Ability` = `GA_Block`（或 `BP_GA_Block`）
- `AbilityLevel` = 1

授权后会打 `[Passive] OnGiveAbility ... 标签: Ability.Slot.Block`，确认进了 `SlotAbilityMap`。

### 5.3 蓝图子类

| 资产 | 父类 | 必须叫这个名字？ |
|---|---|---|
| `BP_GA_Block` | `GA_Block` | 不用，只要在 `AS_HeroKit` 里指对 |
| `GC_Block` | `AGC_Block` | **必须**，不然引擎会把 `GameplayCueTag` 按类名重推成无效标签、cue 静默不触发 |
| `AM_Block` | — | 由 `Ability_Block` 建 AnimMontage |

> `GE_Blocking` / `GE_BlockImmune` / `GE_BlockCooldown` 直接用 C++ 类就行（像 `UGE_Stealth` 那样），**不用建 BP 子类** —— cue 是在 C++ 构造函数里挂的。

### 5.4 参数建议值

| 参数 | 位置 | 建议 | 理由 |
|---|---|---|---|
| 窗口 | `GA_Block::BlockWindow` | `0.4s` | 靠反应，不是靠预判 |
| 免疫 | `BlockComponent::ImmuneDuration` | `1.5s` | |
| CD | `GA_Block::CooldownDuration` | `6~8s` | **必须 ≥ 窗口 + 免疫**，否则免疫还没完又格挡成功，防护罩会闪一下 |
| 格挡减免 | `BlockedDamageMultiplier` | `0.0` | 完全抵消 |
| 免疫减免 | `ImmuneDamageMultiplier` | `0.0` | 完全免伤 |
| 宽限上限 | `BlockComponent::MaxGraceSeconds` | `0.15` | 见 1.1。0.15s 够覆盖 300ms ping |
| 宽限开关 | `BlockComponent::bCompensatePing` | `true` | 局域网调试时可关掉看严格判定的差异 |

### 5.5 伤害管线接线（这次新增的活）

**① `UGE_Damage` 不用建 BP 资产** —— C++ 类直接用（和 `UGE_Stealth` 一样，Executor 在构造函数里填）。

**② 把两个伤害点的 GE 槽位指到 `UGE_Damage`：**

| 槽位 | 在哪 | 值 |
|---|---|---|
| `DS_Passive.DamageEffect` | `DA_Passive_ThreeHit`（或角色的 `PassiveData`） | `UGE_Damage` |
| `GA_ThrowDagger.DamageGE` | `BP_GA_ThrowDagger` | `UGE_Damage` |

> 这两个槽位现在**都是空的** —— 也就是说改之前近战靠 `ApplyPointDamage`，匕首一点伤害都没有（见 1.2 的现状表）。指上去之后伤害才第一次真正走 GAS。

**③ 属性初始化**：这次已经写进 C++ 了 —— `HeroCombatAttributeSet.cpp` 顶部的 `GHeroStatTable`（1 级值 + 每级成长），构造函数按它 `Init`，`ApplyChampionLevel` 也读它。**没有 `InitStats` 这种东西，也不用在编辑器里填**：基础值只有这一个来源，别在 BP 里再覆盖一遍（覆盖了就是两份真相）。

**④ `ReferenceAttackSpeed` 对齐**：属性集写的是 **0.625**，代码里现在是 **0.658**。`RecalculateFinalAttackSpeed()` 的公式是 `当前攻速 / ReferenceAttackSpeed`，这个基准值直接决定动画快慢。要么改代码对齐 0.625，要么把策划表当成笔误 —— **这次按 0.625 改**（以表为准，代码是可改的），改完近战连段的动画速度会变快约 5%，看录像确认可接受。

**⑤ `AbilityHaste` 接进 CD**：3.0 里新增的 `AbilityHaste` 属性，要在 `UMyGameplayAbility::ApplyCooldown` 里换算成 `CD / (1 + AbilityHaste/100)`。不接的话属性存在但没用，以后调技能急速会「改了没反应」。

---

## 6. 验证清单（按顺序）

### 6.1 先验伤害管线（格挡依赖它）

格挡是 ExecCalc 里的第 ③ 步，**伤害管线没通，格挡永远不该有日志**。所以顺序不能反：

1. **编译**通过。
2. `DS_Passive.DamageEffect` / `BP_GA_ThrowDagger.DamageGE` 都指向 `UGE_Damage`。
3. **打一下近战** → `[ExecCalc_Damage]` 那行日志出现，`原始伤害` 约等于 `62 × 第一段倍率`。
   - 没有日志 → GE 没走上 ExecCalc：查 `DamageEffect` 槽位、查 `UGE_Damage` 的 Executions 数组。
   - 原始伤害是 0 → `AttackDamage` 捕获失败或属性没初始化：查 3.1 的坑 ② 和 3.0 的 `InitStats`。
4. **扔匕首** → 日志 `原始伤害 ≈ 80`（固定值那条路）。
5. **Health 真的掉了** → 这是伤害管线的终点。数字对不上就回头调 3.0 的抗性表。

### 6.2 再验格挡

6. 进游戏，看 `[Passive] OnGiveAbility ... 标签: Ability.Slot.Block`。
7. **按右键** → `[ThrowDagger] ASC 收到槽位输入: Ability.Slot.Block`，CD 进 `State.Cooldown.Block`。
8. **窗口期内挨一下** → `[Block] 格挡成功`，伤害为 0，防护罩出现。
9. **窗口期外挨一下** → 没有格挡日志，正常吃伤害。
10. **免疫期内再挨打** → 伤害还是 0，**但不再触发一次格挡**（不该刷出第二个防护罩）。
11. **免疫到期** → 防护罩自动消失，无残留组件。
12. **连按两次右键**（等 CD）→ 干净地再来一次，无状态泄漏。
13. **隐身中按右键** → `[Stealth] 施放技能 Ability.Slot.Block 破隐`。

### 6.3 最后验联网宽限（1.1 那条的唯一验收方式）

单机跑这条**永远看不出区别** —— ping 是 0，宽限是 0。必须两个 PIE 窗口 + 模拟延迟：

14. PIE 设置里给客户端加 **200ms 延迟**（`Play Number of Clients = 2` + `Network Emulation` → `Custom`，Latency 200）。
15. A 格挡，B 在 A 的窗口**刚关**的那一瞬间打一下 —— 需要在 B 那边故意提前一点点打。
16. 期望：**格挡成功日志出现了**，且 `剩余伤害=0`。
    - 完全没出现 → `bCompensatePing` 没开、或者 ping 读到的不是 A 的（`GetGraceSeconds` 里拿到别的 PlayerState 了）。
    - 出现了两次 → `bGraceAvailable` 没被消费掉，检查 `TryMitigateIncomingDamage` 里那个 `Block->bGraceAvailable = false`。
17. 关掉 `bCompensatePing` 重跑 15 → **应该挡不住**。这一步是反向验证：如果关掉之后照样挡住，说明判定的并不是宽限，而是别的东西（比如 `GE_Blocking` 时长被谁改长了）。

> 每步不符合预期，先看对应日志有没有走到，再往下查 —— 别猜。

---

## 7. 拍板记录（已定）

这两条原来是「待处理」，现在按你的口径定下来了，实现按这个走：

1. **判定边界服务端加宽 RTT/2** —— 定。
   实现落在 `UBlockComponent::GetGraceSeconds()` + `WindowCloseServerTime`（3.4），**不去动 `GE_Blocking` 的时长**。理由见 1.1：GE 时长是表现和逻辑共用的，加长它会让「看起来还能挡」和「实际不能挡」错位，而且改错了边 —— 该补偿的是判定层，不是状态层。
2. **`UGameplayEffectExecutionCalculation` 作为唯一伤害结算点** —— 定。
   `UExecCalc_Damage` 是唯一的伤害公式所在地（3.1），近战删掉 `ApplyPointDamage` 回退、改走 `GE_Damage`（3.6）。执行体**全权算**：攻击者的 `AttackDamage` 由 ExecCalc 自己捕获，伤害点只喂倍率/固定值，不喂面板值。

顺带定下来的（原来标着「待定」，实现需要）：

3. **格挡那一击完全抵消**（`BlockedDamageMultiplier = 0`），免疫期完全免伤。以后要改成「减半」「只免物理」再动这两个字段。
4. **不做方向性**：默认 `bRequireFacingAttacker = false`，背对也能挡。开关留着，`FacingHalfAngleDeg = 90` 就是正前 180°。
5. **属性集按 `属性集.txt` 全量落**（3.0），`ReferenceAttackSpeed` 以表为准改成 `0.625`。

---

## 8. 明确不做（扩展项）

- **格挡反击 / 弹反**：格挡成功时给攻击方一个硬直或伤害。需要攻击方也有可被打断的状态机。
- **「伤害先到、窗口后开」那半边的延迟补偿**：3.4 的宽限只补了「窗口刚关」这一侧（见 1.1 的头部缺口）。另一侧要补就得给伤害 GE 记录施加时刻再回查，代价是这个字段得跨网络传。现在不做。
- **引擎原生的 `UImmunityGameplayEffectComponent`**：挂在 `GE_BlockImmune` 上按 tag 拦 GE，比手写判定更硬，而且能拦到「伤害之外」的 GE。等伤害管线稳定了可以考虑替换掉 `TryMitigateIncomingDamage` 里的免疫分支。
- **吸血 / 全能吸血 / 生命偷取**：`属性集.txt` 里有（全能吸血 0、生命偷取带标签区分），但需要「伤害结算结果回传给攻击者」这条链路 —— ExecCalc 现在只输出给目标。等 `UExecCalc_Damage` 里能安全地往攻击者身上反写时再做。
- **穿透的成长来源**：公式已经落进 `UExecCalc_Damage`（先百分比后固定，负抗性走 LoL 的延伸公式），四个穿透属性也都在属性集里。缺的只是「谁会去改这四个属性」——装备/符文系统还没做，所以现在它们恒为 0，等于没有穿透。加装备时往这四个属性上挂 GE 即可，伤害公式不用再动。
- **格挡成功的屏幕反馈**（顿帧、镜头轻推）：可以照抄 `UMyStealthCameraModifier` 的做法。
- **AI 也会格挡**：`CombatAIController` 里按条件触发同一个 `GA_Block` 即可，整套逻辑不用改。
