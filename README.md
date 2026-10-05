# LOL — UE 5.8 LoL 风格 3D 竞技场

主控英雄是刺客 **Kallari**（Paragon 素材）。战斗逻辑**全部**构建在 **Gameplay Ability System (GAS)** 之上，
另有一套自研的 **Arena（竞技场 1v1 回合制）** 模式。

架构一句话：**GAS 负责"发生了什么"，HUD 语义层负责"这意味着什么"，UMG 只负责"长什么样"——
三层之间只传 POD 视图结构，UI 层零 GAS 依赖。**

> **`Content/` 不在仓库里。** Paragon 素材包加二进制 `.uasset` 体积过大，有意忽略；
> clone 下来是"能读能搜但跑不起来"的空壳工程，要跑得自己补 `Content/`。

---

## 分层架构

```
┌──────────────────────────────────────────────────────────────────┐
│ 数据层（DataAsset 驱动，零硬编码数值）                              │
│   UHeroDefinition / UHeroStatConfig / UHeroAnimationSet            │
│   UAbilitySet / UInputConfig / UHeroHUDSlotConfig                  │
│   UThreeHitPassiveData / UDeathHarvestData                         │
│   UArenaRewardPool / UArenaItemData / UArenaAugmentData            │
└──────────────────────────────────────────────────────────────────┘
┌──────────────────────────────────────────────────────────────────┐
│ GAS 层（Source/LOL/GAS）                                           │
│   ASC 挂 PlayerState → 属性集 / 标签 / GE / GA / GC                │
│   伤害唯一结算点：UExecCalc_Damage                                  │
│   格挡唯一减免点：UBlockComponent::TryMitigateIncomingDamage        │
└──────────────────────────────────────────────────────────────────┘
                 ↓  FHUDSnapshot / FSkillSlotView / FHeroAttributeView（纯 POD）
┌──────────────────────────────────────────────────────────────────┐
│ HUD 语义层（UHeroHUDController / UArenaHUDController）              │
│   唯一"把 GAS 语义翻译成 UI 语义"的地方；30Hz 冷却采样              │
└──────────────────────────────────────────────────────────────────┘
                 ↓  Apply*(View)
┌──────────────────────────────────────────────────────────────────┐
│ UMG 层（Source/LOL/UI）                                            │
│   零 GAS 依赖；只认 F*View 契约；槽位身份一律 int32                │
└──────────────────────────────────────────────────────────────────┘
```

---

## 源码地图

```
Source/LOL/
├── Public/ · Private/
│   ├── GAS/                     项目主体
│   │   ├── 核心：MyAbilitySystemComponent / MyGameplayAbility / MyPlayerState
│   │   │        HeroCombatAttributeSet / HeroCombatCharacter / LOLGameplayTags
│   │   ├── 结算：ExecCalc_Damage / BlockComponent / GEComponent_Knockback
│   │   ├── 技能：GA_* (13)  GE_* (28)  GC_* (21)  CameraModifier (2)
│   │   ├── Arena：ArenaGameMode / GameState / PlayerState / PlayerController
│   │   │          ArenaBotController / ArenaLoadoutComponent / ArenaRewardPool
│   │   ├── HUD 语义：HeroHUDController / HeroHUDSlotConfig / HUDAttributeBinding
│   │   │             HeroAttributeView / HeroOverlayHealthComponent
│   │   └── DataAsset：HeroDefinition / HeroStatConfig / HeroAnimationSet
│   │                  ThreeHitPassiveData / DeathHarvestData / Arena*
│   ├── UI/                      Widget + HUDTypes.h / ArenaViewTypes.h
│   ├── Animation/               AnimNotify + HeroEvadeAnimDriver
│   ├── Audio/                   HeroAudioConfig / HeroAudioLibrary
│   ├── Camera/                  LOLCameraBoom
│   └── LOLCharacter / LOLGameMode / LOLPlayerController
│                                ← Arena 的 PC/GM 派生自它们
Config/                          DefaultEngine / DefaultGame / DefaultInput / DefaultGameplayTags
```

---

## GAS 技能体系

| 技能 | 槽位/触发 | 冷却 GE（时长） | 命中判定 | 伤害（SetByCaller） | 控制 |
|---|---|---|---|---|---|
| **GA_ThreeHitPassive** | `Event.Input.BasicAttack` | 无 | 球扫 r=`TraceRadius` | Mult×Empower / `Data.BasicAttack` + `Data.CanCrit=1` | 强化击 Knockback |
| **GA_Block** | 右键，LocalPredicted | `GE_BlockCooldown` 6s | 被动窗口 | — | `GE_Blocking` 0.4s |
| **GA_Stealth** (Q) | Q，LocalPredicted | `GE_StealthCooldown` 10s | — | — | 结束挂 `GE_EmpoweredAttack` 3s |
| **GA_Dodge** (W) | W，LocalPredicted | `GE_DodgeCooldown` 10s | Sweep r=90 | Mult 1.2 / Physical | Knockback 600 / Launch −600 |
| **GA_GroundDodge** | 按键 | `GE_GroundDodgeCooldown` 5s | 纯位移 | — | — |
| **GA_Flash** | 按键 | `GE_FlashCooldown` 300s | 胶囊扫掠，Range 425 | — | — |
| **ThrowDaggerAbility** (E) | E + 确认 | `GE_ThrowDaggerCooldown` 6s | 投射物球 r=12 | Mult 0 / Flat 80 | — |
| **GA_DeathHarvest** (R) | ServerInitiated + 手动选目标 | `GE_DeathHarvestCooldown` 90s | 转圈期逐次扫 | Mult/Flat/`MissingHealthBonus` | 眩晕 0.5s（只第一跳） |
| **GA_FormSwitch** | `Ability.Slot.Form` | 无（CD=0） | — | — | — |
| **GA_SpinSlash / GA_TurnSlash** | 继承 `GA_FormMelee`（Abstract） | 8s / 9s | 球扫 r=220 / 200 | Mult 1.2+Flat25 / 1.1+Flat20 | 第 2 段 KnockUp / Knockback |

`GA_FormMelee` 是近战模板基类：一个 Montage 多段，每段带不同 `ImpactTag`；
数值为空或负表示继承上一段，但 `ControlGE` 为空表示**无控制**。
`LaunchTarget` 是虚函数，由子类决定击飞位移。

### 形态切换链路

```
按键 → AddLoose(State.Form.Switching) 防重入 → CommitAbility(CD=0)
     → 播 ToUnarmed / ToArmedMontage      ← 先播动画
     → OnMontageFinished → ApplyForm()    ← 播完才切标签
     → Infinite GE: GE_FormUnarmed 授 State.Form.Unarmed
     → EndAbility 摘 Switching + SetSwordForceVisible(false)
```

形态标签走 Infinite GE（`GE_FormUnarmed`）携带，各端 ABP 读自己的 ASC 都能看到。

### GE 分类

- **冷却族**：`HasDuration` + `DurationMagnitude = SetByCaller(Data.Cooldown)`
  + `UTargetTagsGameplayEffectComponent` 授 `State.Cooldown.X`。
- **硬控族**：`GE_Stun` / `GE_Knockback` / `GE_KnockUp` 带
  `UCancelAbilityTagsGameplayEffectComponent`（两容器留空 = 取消全部）。
  `GE_Silence` 不带 Cancel 组件——沉默不打断已在施放的技能。
- **GE_Slow**：`ModifierOp = MultiplyCompound`。
- **GE_Damage**：`Instant`，零 Modifier，只挂 `UExecCalc_Damage`。
- **GE_ArenaItem**：`Infinite` + 18 条 SetByCaller 修正符，一份 GE 服务所有装备。

### 伤害结算流程

属性共 27 条（`UHeroCombatAttributeSet`），默认英雄表 `GHeroStatTable` 用成员函数指针索引。
`UExecCalc_Damage` 是**唯一**伤害结算点，顺序：

```
State.Invulnerable 门
  → AD × Data.DamageMultiplier + Data.FlatDamage
  → 斩杀 Data.MissingHealthBonus（乘法）
  → 暴击 Data.CanCrit 且 FRand() < CritChance（先暴击后减伤）
  → 抗性 R×(1-PctPen) - FlatPen
      DamageTaken = R≥0 ? 100/(100+R) : 2 - 100/(100-R)     ← 负抗性用延伸公式
  → UBlockComponent::TryMitigateIncomingDamage              ← 唯一减免入口
  → 写 -Damage 到 Health（不在此钳 0，交给 PreAttributeBaseChange）
  → 吸血 Omnivamp + (BasicAttack ? LifeSteal : 0)，生成 GE_Heal
```

---

## Arena 竞技场模式（1v1 回合制）

### 比赛相位机

```
WaitingToStart ─(两人到齐)→ StartRound(1)
      ┌──────────────────────────────────────────────┐
      ↓                                              │
  RewardSelection（备战 30s，挂 State.Invulnerable）   │
      │ BeginRewardSelection 给两人各发一份待选        │
      ↓ 两人 RewardStep 都 == None？→ TryBeginCombat  │
  Combat（无倒计时，每 0.1s 轮询生死）                 │
      ↓ 有人 IsDead() / Pawn 消失 > 0.5s
  Settlement（6s，扣大场血、记战绩、判阈值）
      └──── 未结束 → StartRound(N+1) ─────────────────┘
MatchEnd（终态）
```

### 奖励流程机（服务端专用，不复制）

```
RoundReward ─→ ItemOffer（装备三选一）─→ 次数耗尽 → ForgeMenu（锻造器，一次抽 2 条属性）
                                                     └→ 链末端 ResetRewardSelection → None
```

### 关键数值（C++ 默认，可在 `BP_ArenaGameMode` 覆盖）

| 项 | 值 |
|---|---|
| 回合奖励 | R1 传说装备/锻造器×5、R2 海克斯白银、R3 棱彩锻造器、R4 海克斯黄金、R5 棱彩分支、R6 无；之后 `(N-7)%3` 循环 |
| 扣血分档 | (1,15) (5,30) (9,40) (13,50) |
| 胜负阈值 | 大场血 100，`FirstThresholdWins=6`（6:0 或 0:6 直接结束） |
| 三选一 / 重随 | 3 张 / 整场共享 4 次 |
| 等级 | 初始 3，每回合 +3，上限 18 |
| 备战 / 结算 | 30s（超时沿链随机代选，最多 16 轮）/ 6s |
| 装备栏 | 6 格，满后 `ReplaceOldest`（FIFO 顶掉下标 0） |

### 类职责

- **`AArenaGameMode`**：唯一"懂规则"的地方——回合表、抽签、扣血表、相位推进、Bot 生成。
  重写 `ChoosePlayerStart`：战斗点过滤掉 `PrepStart*`，备战点只认 `PrepStart*`，
  两套互补且都按名字排序保证稳定。
- **`AArenaGameState`**：只当比分牌。`PhaseEndServerTime` 存**绝对时刻**而非剩余秒数，
  一次复制管全程，客户端 `GetServerWorldTimeSeconds()` 一减即得。
  相位变更时服务端与客户端都显式走一次 `OnRep_X()`，两端同一条路。
- **`AArenaPlayerState`**：大场血 + 战绩 + `PendingPrompt` + Loadout 组件。
  `NetUpdateFrequency` 从 1Hz 提到 10Hz。
- **`UArenaLoadoutComponent`**：挂 PlayerState 而非 Pawn，要活过 Pawn 重建。
  海克斯三通道：① `Modifiers`→`UGE_ArenaItem` ② `SpecialEffect`(GE 子类) ③ `GrantAbilitySet`。
  撤销走 `ClearAbility(句柄)`。
- **`AArenaBotController`**：**不用行为树 / NavMesh**，纯代码 Tick 状态机。战斗期
  `DecisionInterval=0.25s` 重算移动（`PreferredRange=180±90`、每 4s 换向、
  `AttackInterval=1.1`、`SkillInterval=2.5`）；选卡优先级 Grant > Forge(0.5) > Finish，
  挑装备纯随机不估值。
- **`AArenaPlayerController`**：唯一职责是**输入模式**。三态：备战 `GameAndUI` /
  战斗 `GameOnly` / 结束 `UIOnly`，靠 `CurrentIntent` 保证幂等。

**只传下标不传载荷**：两条 Server RPC（`ServerSubmitChoice` / `ServerRerollChoice`）只发 `int32`，
内容由服务端从自己的待选里取。

---

## UI 层：两条同构的三层管道

```
GAS（ASC / 属性集 / 标签 / GE）
   ↓  UHeroHUDController（住 GAS/，唯一 GAS 语义出口，30Hz 冷却心跳）
   ↓  FHUDSnapshot / FSkillSlotView / FHUDVitalsView / FTargetFrameView
UHeroHUDWidget（根，唯一持有 Controller）→ 子 Widget 只认 Apply*(View)

AArenaGameState / PlayerState / LoadoutComponent（已复制的 POD）
   ↓  UArenaHUDController（住 UI/，一次都不碰 ASC，0.25s 心跳 + EqualsForUI 去重）
   ↓  FArenaMatchView / FArenaPromptView / FArenaLoadoutView / FArenaRoundTrackerView
UArenaHUDWidget（根，唯一持有 Controller）→ 子 Widget 只认 Apply*(View)
```

两条约束贯穿全层：

1. **`EnsureXxx(N)`**：只管建结构、不管尺寸。数量对得上直接复用。
   数组一律 `SetNum` 不用 `Add`，保证"下标 = 槽位号"。
2. **`ApplyXxxLayout`**：只管定尺寸 / 对齐 / 间距，每次视图变化都调、必须幂等。
   `FindContainerSlot` 向上走到 `Parent == Container`，不写死层数。

### Widget 一览

| Widget | 职责 | 构造方式 |
|---|---|---|
| `UHeroHUDWidget` | HUD 根，订阅 5 条委托 + `PullHUDState`，分发 | 混合：血条等 `BindWidgetOptional`，**技能槽程序化** |
| `UHeroHealthBarWidget` | 资源条（血 / 能量二用）；全层唯一有 NativeTick 的（白条残影缓动） | BindWidgetOptional + MID |
| `UHeroSkillSlotWidget` | 图标 / 转圈 / 秒数 / 灰化 / 边框；无 tick，CD 转好起一次性定时器闪一次 | BindWidgetOptional |
| `UHeroTargetFrameWidget` | 目标框，复用两个 HealthBar | BindWidgetOptional |
| `UHeroOverlayHealthWidget` | 敌人头顶血条（不经 Controller，由 `HeroOverlayHealthComponent` 直推） | BindWidgetOptional |
| `UHeroAttributePanelWidget` | 属性面板两形态（常驻 8 条 / 展开 168px） | **程序化** `EnsureEntryWidget` + `ReflowRows` |
| `UArenaHUDWidget` | 竞技场根 | BindWidgetOptional |
| `UArenaRewardScreenWidget` | 三选一排卡 | **程序化** `EnsureCardCount` |
| `UArenaLoadoutBarWidget` / `RoundTrackerWidget` | 装备栏 / 回合线 | **程序化** |
| `UArenaMatchStatusWidget` | 回合 / 相位 / 倒计时 / 双方大场血 | BindWidgetOptional + `UArenaHUDLayoutConfig` |
| `Announce` / `Loading` / `ChoiceCard` / `LoadoutEntry` | VS 介绍 / 选手卡 / 单卡 / 单格 | BindWidgetOptional |

灰化判据用 **`Ability->DoesAbilitySatisfyTagRequirements(*ASC)`**，
读的是能力自己声明的 `ActivationBlockedTags`。

---

## 网络与复制

| 通道 | 内容 |
|---|---|
| 属性 | 27 条 `DOREPLIFETIME_CONDITION_NOTIFY(COND_None, REPNOTIFY_Always)` |
| 状态标签 | active GE 的 FastArray（Granted Tag） |
| GameState | `ArenaPhase` / `RoundNumber` / `PhaseEndServerTime` / `Contenders` / `Winner` / `RoundPlan` / `LastRound*` |
| PlayerState | `MatchHealth` / `RoundsWon` / `RoundsLost` / `PendingPrompt`；Loadout 组件自复制三个数组 |
| RPC | 6 条技能镜像（`ServerSubmit*Input`）+ 2 条 Arena（都是 Server + Reliable） |

约定：两端都要看到的行为标签走 GE 的 `TargetTagsGameplayEffectComponent` 携带；
跨网络传递的 DataAsset 一律 `TSoftObjectPtr`。

死亡链路：

```
PostGameplayEffectExecute（权威，全项目唯一死亡判定）→ OnOutOfHealth
  → GE_Death(SetByCaller Data.RespawnDelay=5) → State.Dead 复制 → OnDeadTagChanged
  → EnterDeathState（布娃娃组件级 SetSimulatePhysics 或播方向死亡蒙太奇，二选一互斥）
```

---

## 资产组织（Content/LOL）

| 目录 | 内容 |
|---|---|
| `Blueprints/` | `BP_GA_*`（11 个技能 BP）、`BP_Arena*`（4）、`BP_MyPlayerState`、`BP_ThrowDaggerProjectile`、`BP_CameraShake_*` |
| `Data/` | `DA_Hero_Kallari`、`DA_HeroStat_Kallari`、`DS_KallariAnimSet`、`DS_Passive`、`DS_DeathHarvest`、`DA_SKillSlots`、`DA_HeroAttributePanel`；`Data/Arena/` 下 9 海克斯 + 11 装备 + `DA_Arena_RewardPool` |
| `Animation/Kallari/` | `AM_Death_A/B`、`AM_Evade_Fwd/Bwd`、`AM_Form_*`（收 / 拔刀）、`AM_HitReact_{Front,Back,Left,Right}`、`AM_ThrowDagger`、`BS_Evade` |
| `Niagara/` | `NS_DodgeKick_*`（三代迭代）、`NS_BladeTrail`、`NS_EmpoweredTrail`、`NS_PerfectWindow`；基材 `SM_DodgeKick_LegCylinder/AirRing/FootShell` |
| `UI/` | `WBP_HUDWidget`、`WBP_Skills`、`WBP_HealthBar`、`WBP_EnegryBar`、`WBP_HeroTarget`、`WBP_AttributePanel/Entry`；`UI/Arena/` 下 8 个 |
| `Maps/` | `Arena_1v1.umap` |
| `Input/` | `IA_{Q,W,E,R,Shift,1,2,BasicAttack,Block,Transform,Summon1,ToggleAttributePanel}` + `IC_Ability` + `IMC_Default` |

其余为 Paragon 素材包：`ParagonKallari`（主控）、`ParagonCrunch`、`ParagonSparrow`、`ParagonSunWukong`。
