# Death Harvest（R 大招）实现方案 + 代码

Kallari 的 R：锁定一个敌方英雄 → 起手 → 开传送门 → 从目标身后现身 → 原地转圈 AOE → 收招。
本文是动工前的完整方案：决策、相位表、**全部新增代码**、编辑器侧清单、取消矩阵、验证清单。

---

## ★★ v2 变更（当前实现的真实状态，先看这一节）
（**变更 6 / 7 是最近一轮加的**（起手慢放 + 命中顿帧 / 转圈粒子双手），比前面几条新。
变更 6 描述的 P3.5「先站住再转」那版已经被同一节里的当前实现取代 —— 直接看变更 6。
**v5 又动了变更 6**：打击感（顿 + 伤害 + 震）整体推后 `SpinImpactDelay`（默认 0.25s），
落在"刀真正扫起来"那一帧；甩镜加了逐段日志。
**v6 修了客户端两件"本端没有"**：① 施法者自己那台**根本没被传送**（自治代理不吃服务器的
`SetActorLocation`），现在本端用同一个 `ComputeDestination` 自己补一次；② 甩镜方向改成
「落点 → 敌人」，不再依赖"自己"那个坐标。见 §6.1 / §6.4 / §6.8。
**v7 补上第三件同类**：③ 本端那份"关掉自动转向"从来没关过 → 移动组件纠偏时把 actor 的朝向
写回【传送前】那份，表现是**位置对、朝向不对**。见 §6.9。）

**为什么要改**：v1 假设「起手有动画」，用蒙太奇 notify 推相位。实际资产是
**起手 = 消失一下 + 一个特效**（P1~P3 根本没有蒙太奇在播），notify 无处可挂 →
相位改成**时长驱动**。**代码以 `Source/` 为准，下面 §5 的代码清单是 v1 的历史记录**，
和当前实现有出入的地方一律以源码为准。

### 变更 1：相位由时长推，不再由 notify 推

`UAbilityTask_WaitDelay` × 2（服务器）负责 P1→P2→P3。四条 `Event.DeathHarvest.*` 标签和
四个 `WaitGameplayEvent` 任务**在相位推进上退役**（标签本身还留在 `LOLGameplayTags.h` 里，
`UAnimNotify_SendGameplayEvent` 也还在，但本技能不再用它们）。

| # | 相位 | v2 驱动 | 服务器 | 各端看到 |
|---|---|---|---|---|
| P1 | 消失 | `StartVanishPhase()` | 锁移动 + `Add` cue `…Cast` | 人**真的看不见**（`SetVisibility(false)`）+ actor 碰撞关闭 + 消失 burst；主人那台再叠传送镜头 |
| P2 | 开门 | `WaitDelay(VanishToPortalDelay)` | 算落点 → `Add` cue `…Portal`（带坐标） | 门开在落点，朝向目标 |
| P3 | 现身 | `WaitDelay(PortalToAppearDelay)` | `SetActorLocation/Rotation` → `Remove` 上面两条 cue | 恢复可见/碰撞 + 现身 burst；本人镜头淡出。**施法者自己那台由 `ApplyClientTeleport` 用同一个 `ComputeDestination` 把自己摆到同一个落点**（§6.8） |
| P4 起手 | 现身**同一帧**开转 | 立刻 `StartSpinVisuals()`（**没有**中间那段空等，字幕在播） | `Add` cue `…Spin` + 播蒙太奇 + `BeginSpinLead()`（甩镜 + 排推后） | 蒙太奇正常速度播；施法者本机镜头开始甩到「落点 → 敌人」那条线（§6.4） |
| P4 命中 | 起手后 `SpinImpactDelay` 秒 = **刀真正扫起来那一帧** | `StartSpinPulses()`（第一跳立刻打）| `Repeat` 伤害跳；第一跳真打到人 → `BeginHitStop()`，世界时间冻 `HitStopTime` 秒 | **她的动画**慢 `SpinSlowTime` 秒（`CustomTimeDilation`）+ 全场顿同一帧 + 施法者本机镜头震一下（`GC_DeathHarvest_Hit` 的 `CameraShake`） |
| P4 转圈 | 持续转圈 | `Repeat` 每 `SpinInterval` 一跳 | 只结算伤害（cue 和蒙太奇已在起手那帧做过） | 循环粒子 + 每跳 `…Hit`（含镜头震动） |
| P5 | 收招 | `OnMontageFinished` / 被打断 | `EndAbility` 里收移动锁、朝向、标签 | cue 随技能结束自动摘 |

客户端那一份（`ServerInitiated` 的第二次 `ActivateAbility`）现在做**四件**事：
锁移动 + 按 `VanishToPortalDelay + PortalToAppearDelay` 对齐、到点先关掉本端自动转向
（`ApplySpinRotationOverride`，§6.9）+ 把自己摆到落点（`ApplyClientTeleport`，§6.8）
+ 同一帧播转圈蒙太奇。
锁移动是 v2 新增的：`MovementMode` 不复制给自治代理，不锁的话本端会「一边隐形一边被拖着走」。
本端传送是 v6 新增的：坐标也不复制给自治代理 —— 不补的话**主机看她是好的，她自己屏幕上在消失的原处转**。
本端关自动转向是 v7 新增的：那两个开关也不复制 —— 不关的话**位置对、朝向不对**（纠偏会拿本端存的旧朝向写回）。

### 变更 2（★ 联机必看）：三条状态 cue 都必须把 `WhileActive` 转发到 `OnActive`

运行时 `K2_AddGameplayCueWithParams` 挂的 cue，**服务器那一端收到的是 `WhileActive`，
客户端收到的才是 `OnActive`**（`AbilitySystemComponent.cpp:1601-1605`，
"Call on server here, clients get it from repnotify"）。而
`AGameplayCueNotify_Actor::WhileActive_Implementation` 是**空实现、不转发**
（`GameplayCueNotify_Actor.cpp:341-349`）。

只写 `OnActive` 的后果：**主机自己屏幕上没有表现，客户端却看得到，引擎一句错都不报**。
`GC_DeathHarvestCast` / `…Portal` / `…Spin` 三个类都已经加了转发（各自头文件里都有这段引用）。

顺带记下另一半：`ExecuteGameplayCue` 走的是 `Executed` → `OnExecute`，基类返回 `false` ——
**Actor 版 cue 用 Execute 发就是彻底哑的**（v1 的传送门就是这么没的）。所以有生命周期的一律用
`Add` / `Remove`，一次性的（`…Hit`）才用 `Execute`。

### 变更 3：`…Cast` 这条 cue 现在同时管「藏人」和「消失/现身两个 burst」

`GameplayCue.DeathHarvest.TeleportOut` / `.Appear` 两条 **execute cue 退役**（本来也没有对应的 BP）。
起手那一下的特效 + 消失期间的隐形 + 碰撞开关 + 传送镜头全在 `AGC_DeathHarvestCast` 一条 cue 里，
理由：它们共享同一个生命周期，分成两条就会有「一条摘了另一条没摘」的中间态。

- **藏人**用 `UPrimitiveComponent::SetVisibility(false)`（**每台机器上都藏，包括主人自己**）——
  不是 `bOnlyOwnerSee`，那只挡别人。
- **碰撞**按用户决定：消失期间 `SetActorEnableCollision(false)`，还原时只在「本来是开的」时才开回去
  （死了的人胶囊已经被 `EnterDeathState` 关掉了，无脑开回去会把尸体变回一根柱子）。
- 还原只还原**自己藏过的那一批**（`HiddenPrimitives` 记账），别人的隐藏状态不动。

### 变更 4：转圈不做代码旋转

`bRootMotionSpin` / `SpinRate` 两个配置项**已删**。原因是那个组合是个陷阱：
`bRootMotionSpin = false`（默认值）时代码会逐帧 `AddActorWorldRotation`，而动画**自己也转**
（转的是根骨骼，和 root motion 无关）→ **转两倍速**。现在代码全程不碰 actor 的 yaw，
`ApplySpinRotationOverride` 只剩下「关掉移动组件的自动转向」这一件事。

### 变更 5：`UDeathHarvestData` 的字段增删

- 删：`CastSound`（音效由 `…Cast` cue 播 —— 服务器单方面 `PlaySoundAtLocation` 只有主机听得到）、
  `bRootMotionSpin`、`SpinRate`。
- 增：`VanishToPortalDelay`（默认 0.5s）、`PortalToAppearDelay`（默认 0.6s），Category `Phase`。
- `MontageStartSection` 现在应当**指向转圈那一节**（蒙太奇只盖住转圈，收招也在那一节里）。

**编辑器侧待办**（代码改完不会自动生效的部分）：

1. `DS_DeathHarvest`：确认删掉的字段没有遗留、各项时长填好、`MontageStartSection` 指到转圈节。
   **v5 新增 `SpinImpactDelay`（默认 0.25s）** —— 就是"顿/震落在哪一帧"，只调这一个值。
2. `GC_DeathHarvest_Cast` BP：配 `VanishParticle` / `AppearParticle` / `VanishSound` / `AppearSound` / `ScreenMaterial`。
3. `GC_DeathHarvest_Portal` / `_Spin` BP：本来就该配的粒子/音效，这次没有变化。
4. `GC_DeathHarvest_Hit`：**没有 BP**，要新建（名字必须一字不差，点换下划线）。

### 变更 6：P4 起手慢放 + 命中顿帧

> **这一节在 v4 被重写过，v5 又挪了一次打击感的落点。**
> v3：现身之后先站住 `AppearToSpinDelay`（0.45s）再起转，期间把**世界**调慢 —— 那 0.45s 里
> **没有任何动画在播**，人被慢放拉长成一个定格的站立姿势（"提前现身在原地傻站"）。
> v4：改成现身同一帧起转 + 慢放盖转圈头几帧 —— 但**转圈蒙太奇只有一段 `Spin`**，
> 它开头那段是抬刀/转上身，刀真正扫出去在这之后，于是"顿 + 震"落在了蒙太奇第 0 帧上，
> 表现是「现身之后卡了一下，然后才开始转」——那一顿和刀的加速度完全对不上。
> v5：**蒙太奇还是现身同一帧起播（不傻站），但打击感整体推后到刀扫起来那一帧。**

#### 6.1 现身同一帧开转，打击感晚 `SpinImpactDelay` 秒

`OnPortalFinished()`（服务器）和 `OnClientDelayFinished()`（客户端）里那段 `WaitDelay` 依然是**删掉的**，
起手还是「传送 + 播蒙太奇 + 转圈 cue + 甩镜」同一帧（`StartSpinVisuals()` / `PlayClientMontage()`）。
变的只是**打击感**：`BeginSpinLead()` 现在只做「起甩镜 + 排 `SpinImpactDelay`」，
慢放 + 第一跳伤害挪到 `OnSpinImpact()`（= `OnSpinImpactDelayFinished()`）里。

为什么非挪不可：`AM_DeathHarvest` 里**只有一段 `Spin`**（覆盖整条 `Ability_Ultimate`），
没有第二个段名可以指，所以"刀什么时候扫起来"这件事在代码里只能靠一个**时长**表达 ——
就是 `SpinImpactDelay`。这一段里蒙太奇是**正常速度**播的，所以既不会傻站，慢放也不会被浪费在抬刀上。

`UAbilityTask_Repeat::Activate()` 是**先 `PerformAction()` 再 `SetTimer`**（`AbilityTask_Repeat.cpp:35`），
所以「起伤害跳」= 第一跳已经打出去了，顿帧和镜头震动（挂在第一跳的命中 cue 上）也就在同一帧。
落点在目标身后 140cm、扫描半径 320cm —— **正常情况下第一跳必中**，所以那一下最重。

`SpinImpactDelay` 默认 **0.25s**，需要照着手感调：
- 顿/震**还是偏早**（刀还没扫到）→ 调大；**偏晚**（刀已经扫过去了）→ 调小。填 **0** = 回到 v4 的行为。
- ⚠️ 这段时间是从转圈总时长里**扣掉**的：`SpinImpactDelay + SpinCount × SpinInterval` 必须
  ≤ 蒙太奇长度，否则最后几跳会被收招掐掉 —— 现象只是"转得少了两下"，很难查。
  所以服务器起手时会打一条对账日志（`转圈时长账：推后 0.25s + 8 跳 × 0.25s = 2.25s，蒙太奇 X.XXs（够用/★ 超了）`）。

甩镜**不跟着推后**：它留在现身那一帧开始，因为镜头要 0.45s 才收敛，正好在刀扫过来之前对准目标。

#### 6.2 起手慢放用的是**角色的** `CustomTimeDilation`，不是世界时间

这是这一版的关键改动。`FActorComponentTickFunction::ExecuteTick` 里组件 tick 的 delta 会乘上
owner 的 `CustomTimeDilation`（`Actor.h:4890`：`DeltaTime * (MyOwner ? MyOwner->CustomTimeDilation : 1.f)`），
而骨骼网格的动画正是走组件 tick 推进的 —— 所以往角色身上写这一个值，**只有她的动画慢下来**，
别的玩家、粒子、世界计时器、`Repeat` 的伤害节奏统统不受影响。于是：

- 这里填的是**真实秒**，不需要 `ScaledWorldSeconds` 那一层折算（世界计时器根本不看它）；
- 漏还原的后果只是「她一直慢动作」，不是「整局卡在四分之一速」；
- 不碰 `WorldSettings`，主机自己放招也不会把远端客户端拖下水。

⚠️ `CustomTimeDilation` **不复制**（`Actor.h:797` 只有 `BlueprintReadWrite`，没有 `Replicated`），
所以**每台跑这个技能实例的机器各设一次**。跑这个实例的只有两台：服务器（权威）和施法者本端
（`ClientActivateAbilitySucceed` 后本地那次）—— 两台都设所以两边同步；只有**旁观者**看不到这段慢放，
差 ≈ `SpinSlowTime × (1 - SpinSlowScale)`（默认约 0.13s）。想让旁观者也看到就得再加一条多播 RPC。

⚠️ **服务器那一份也必须设**，不能因为"服务器不上屏"就跳过：服务器的蒙太奇不慢的话，它会比施法者本机
早一截广播 `OnMontageFinished` → `EndAbility`，`bStopWhenAbilityEnds` 会把人家还差一截的动画直接掐掉。
所以 `BeginSpinSlowMotion` 里**没有** `IsLocallyControlled` 判据（甩镜那里有，理由不同）。

#### 6.3 命中顿帧反过来**必须是世界的**

只慢她自己的话敌人照常动，看起来不像"打中了"而像"她卡了"。所以顿帧走
`UGameplayStatics::SetGlobalTimeDilation`，服务器按下去 → `WorldSettings::TimeDilation`（`transient, replicated`）
复制到全场 → 每一端顿的是同一帧，连被打的人屏幕也一起顿。挂在 `ApplySpinDamage` 的**返回值**上：
只有真的造成了伤害才顿（空挥不顿），`OnSpinPulse_Server` 里用 `|=` 收集 —— **不能短路**，
短路会让同一次脉冲里后面的目标连伤害都不结算。

⚠️ 这次等待时长**必须**折算：世界计时器跟的是膨胀后的 delta（`LevelTick.cpp:1596` 乘、`:1816` 喂给
TimerManager），冻帧期间「0.07 世界秒」要跑 1.4 真实秒。`ScaledWorldSeconds` 就是给这一处用的，
而且**必须在 `SetGlobalTimeDilation` 之后**调用。两个慢放窗口重叠时（默认就是）起手那段的实际时长
会被顿帧拉长一点（0.15 → 约 0.22 真实秒），因为它的 `WaitDelay` 也是世界秒。

#### 6.4 甩镜

`PC->SetControlRotation(RInterpTo(...))` 每帧推，摇臂是 `bUsePawnControlRotation` 自动跟着转
（`LOLCharacter.cpp:39`）。插值用**真实** delta（`UWorld::GetRealTimeSeconds`），否则慢放期间镜头会跟着变慢；
结束时刻记在真实时间轴上（`CameraAlignEndRealTime`），所以两种慢放都不会让这段甩镜变长或变短。
逐帧驱动是 `SetTimerForNextTick` 自递归 —— **`SetTimer(..., Rate=0, bLoop=true)` 那个"每帧"写法是假的**
（`InternalSetTimer` 对 `InRate<=0` 直接 `Invalidate`，一次都不触发），别改回去。
只在施法者本机（`IsLocallyControlled`）做 —— 服务器的 control rotation 是被那位客户端的 `ServerMove`
驱动的，写了下一帧就被覆盖。不屏蔽视角输入：甩镜期间玩家自己还能转镜头，转完接着甩（用户明确要的）。

**瞄准方向的锚点（v6 修的就是这里）**：本来写的是「自己 → `LockedTarget`」，
但**客户端那一份的"自己"是不可靠的**：传送是服务器权威的，而自主代理（本端玩家自己那个 pawn）
**不一定套用**服务器 P3 那次 `SetActorLocation` —— 本端坐标还停在**消失点**时，这条线算出来就是
"消失前那条线"，而玩家的相机本来就在那条线上（TA 就是对着敌人才点的 R）→ `RInterpTo` 前后只差零点几度 →
**表现就是"客户端现身之后转摄像机没生效"**。主机上坐标真的变了，所以这个问题只在客户端出现。

v6 换成不依赖"本端坐标搬没搬"的算法 —— **落点 → 敌人**：落点在敌人身后一个 `BehindDistance`
（`ComputeDestination` 的候选表），所以这个方向 = **来路的反方向**，用「消失点 + 敌人当前位置」就能算，
两端数据一致、结果一致。判据是"她到底动没动"（本端坐标离消失点 < 10cm = 没动）：
动了（主机 / 被纠正过的客户端）就用真实几何 `敌人 - 本端坐标`（退让落点也精确），没动就反推 `消失点 - 敌人位置`。

消失点两端各记一份（服务器 `StartVanishPhase`、客户端 `PlayClientTimeline`），
`bHasVanishLocation` 和 `bHitStopDone` 一样要在 `ActivateAbility` 开头清一次（`InstancedPerActor` 复用）。
剩下的退路：目标为空 → 用「来路」（这条线穿过敌人；但本端坐标也停了的话会退化成零向量，
所以第一帧日志专门打了目标名）；连消失点都没有 → 本端前向。

**客户端甩镜"没生效"怎么查**：这条路有四种同样安静的死法（不是本机 / 数值没配 / 目标没拿到 /
技能提前收招），另外还有第五种——**方向算出来跟当前朝向几乎相同**（本端坐标没搬，见上）。
v5 起每条都打了日志，一次跑就能分辨：

| 日志 | 含义 |
| --- | --- |
| `甩镜跳过：这台机器上施法者不是本机控制` | 这条在客户端的日志里出现 = 判据反了（`IsLocallyControlled` 那条链） |
| `甩镜跳过：数据没配（时长=… 速度=…）` | DS 资产上 `CameraAlignTime`/`CameraAlignSpeed` 被填成 0 |
| `甩镜开始：… 目标=X` | 走到了。**`目标=None` = `LockedTarget` 在客户端是空的**（`TriggerEventData->Target` 没带过来）→ 那就只能走"来路"退路 |
| `甩镜第一帧：… 本端(x,y) 消失点(x,y) 传送已套用=N …` | **`传送已套用=0` = 本端坐标还停在消失点**（走的是反推那条，v6 之前就是这种）。`写后`≠`读回` = 中间有人把 control rotation 写回去了 |
| `甩镜结束：N 帧，yaw 停在 A（目标 B，差 C；最后一帧写前 D）` | **`写前 D ≈ A` 且离 B 还很远 = 每帧都被写回去了**（不是方向算错，得去查谁在写 `ControlRotation`）；**N 只有个位数** = 帧数不够，镜头没转到位就被收了；`差 C` 还大 = 速度太小或时间太短 |
| `甩镜被掐断：还剩 Xs` | 镜头不是甩完的，是**技能本端提前收招/被打断**了 —— 这时候该查的是相位，不是镜头 |

#### 6.5 字段

`Category="SpinLead"`：`SpinImpactDelay`(0.25s) / `SpinSlowTime`(0.15s) / `SpinSlowScale`(0.15) /
`bSlowMotionEnabled`(true) / `CameraAlignTime`(0.45s) / `CameraAlignSpeed`(9)。
`Category="HitStop"`：`HitStopTime`(0.07s) / `HitStopScale`(0.05) / `bHitStopEveryPulse`(false)。
v3 的 `AppearToSpinDelay` / `SlowMotionScale` 已删（DS 资产里那两条旧值会被自动忽略）。

- `bSlowMotionEnabled` 关掉 = 命中不放慢 + 不顿帧（甩镜和震动照旧），拿来 A/B。
- `bHitStopEveryPulse` 打开 = 8 跳每跳命中都顿 —— 会把整段转圈明显拉长（8 × 0.07s 真实时间），
  想要"连击感"再开。
- 调手感的方向：**越顿越爽** → 缩短 `SpinSlowTime` + 压小 `SpinSlowScale`；
  **落点不对** → 只动 `SpinImpactDelay`（它决定"什么时候顿"，上面两个决定"顿多狠"）。

#### 6.6 两个时间流速都必须在 `EndAbility` 里兜底还原

`EndHitStop()` + `EndSpinLead()` 幂等，就放在 `StopSpin()` 前面。技能被打断（死亡/被控）时唯一
走得到的地方就是那儿，漏了的表现：`EndHitStop` → 「整个世界卡在 0.05 速」；`EndSpinSlowMotion`
→ 「她本人一直慢动作，走路出刀都像卡带」。

`bHitStopDone` 还要在**每次 `ActivateAbility` 开头**清一次：技能是 `InstancedPerActor`，
整个对局复用同一个 UObject，不清的表现是「第一次放大招手感对，后面每一次都不顿」。

#### 6.7 镜头震动

挂在 `UGC_DeathHarvestBurst::CameraShake` 上（`GC_DeathHarvest_Hit` BP 里配
`BP_CameraShake_Hit_Player` 就行），在 `OnExecute_Implementation` 里判
「`MyTarget` 是本地控制的 Pawn」才 `ClientStartCameraShake`。用 `MyTarget` 而不是
`Parameters.Instigator`：cue 的 `MyTarget` 就是那条 cue 的 ASC 的 avatar
（`AbilitySystemComponent.cpp:1507`），而多播 RPC 是在**施法者的 ASC** 上跑的
（`GameplayCueManager::FlushPendingCues` → `Call_InvokeGameplayCueExecuted_WithParams`），
所以每台机器上 `MyTarget` 都是施法者本人，不依赖参数里那些 weak ptr 的复制。

这个字段加在基类上，三个 burst（现身/命中/传送消失）都能配 —— 只有配了的才会震。

#### 6.8 施法者自己那台必须自己补一次传送（v6）

**现象**：主机看她是好的，她自己屏幕上她**在消失的原处转**（门开在落点、人没过去）。
这不是表现问题，是坐标根本没到本端。

**为什么到不了**：自治代理（本端玩家自己那个 pawn）的坐标是**自己权威**的 ——
服务器 P3 那次 `SetActorLocation` 改的是服务器那一份，正常要靠**下一次 `ServerMove` 的纠偏**
（`ClientAdjustPosition`）传回来；而 P1 起移动就是 `MOVE_None`，本端根本不发 `ServerMove`，纠偏永远不来。
（v1 不锁移动时反而"偶尔是对的"，代价是隐形期间被玩家拖着走 —— 那是另一个毛病。）

**怎么补**：`OnClientDelayFinished` 里、起手之前调 `ApplyClientTeleport()`：
用**和服务器同一个** `ComputeDestination` 在【本端现身那一帧】把落点重算一遍，再照服务器那三行
（补半个胶囊高 + 只取 Yaw）摆到自己 pawn 上。
- 它**只吃目标和场景**（不吃"自己"），两端输入一致 → 正常情况（身后站得下 = 候选表第一项）结果必然相同；
- 唯一位移源是目标那台机器上还在插值的坐标/朝向 —— 只有"身后被挡、退到别的候选位"且刚好卡在判定边缘
  才会差一个候选位，那点误差由 `EndAbility` 之后移动组件的纠偏抹掉（表现是轻轻一顿）；
- 日志：`客户端本端传送 @ (x, y, z) 朝向 N`，拿服务器那条 `P3 现身 @ (x, y, z) 朝向 N` 对一眼：
  坐标该重合、朝向该同值。落点算不出来（`ComputeDestination=false`）或目标为空时会打
  `客户端本端传送跳过：…`，那就要先查目标为什么没到本端。

**顺带解决了甩镜的方向**：坐标摆过去之后，§6.4 里"她到底动没动"那条判据在客户端也成立，
瞄准方向走的是和主机一模一样的真实几何。反推那条退路留着兜底（本端传送没走成时用）。

#### 6.9 本端也要关掉自动转向，否则纠偏会把朝向写回去（v7）

**现象**：§6.8 修完，主机看她是对的（在落点、面向敌人），**她自己屏幕上位置对、朝向不对** ——
人站在落点上，脸却朝着消失前那个方向。

**谁写的朝向**：坐标对了之后，朝向还被移动组件的纠偏写了一次。服务器发纠偏时**从来不带朝向**
（`UCharacterMovementComponent::ShouldCorrectRotation()` 默认 `false`，见 CharacterMovementComponent.h:1137），
于是本端退回用【自己上一次移动里存的朝向】：

```cpp
// CharacterMovementComponent.cpp:11316（ClientAdjustPosition_Implementation）
if (CharacterMovementCVars::bUseLastGoodRotationDuringCorrection        // CVar 默认 1
    && (bOrientRotationToMovement || bUseControllerDesiredRotation)     // ★ 判据就是这两个开关
    && (!OptionalRotation.IsSet() && ClientData->LastAckedMove.IsValid()))
{
    OptionalRotation = ClientData->LastAckedMove->SavedRotation;        // = 本端【传送前】的 actor 朝向
}
// 下面紧跟着：
UpdatedComponent->SetWorldLocationAndRotation(WorldShiftedNewLocation, OptionalRotation.GetValue(), ...);
```
（`SavedRotation` 是 `FSavedMove_Character::SetMoveFor` 里那句 `SavedRotation = Character->GetActorRotation()` 存下来的，
CharacterMovementComponent.cpp:12860。）

**为什么只有客户端中招**：这段 fallback 的判据 `bOrientRotationToMovement` 是**每台机器各自的**
移动组件属性、不复制 —— 服务器那份在 P4 起手被 `ApplySpinRotationOverride(true)` 关成了 `false`
（走不进这段），本端那份一直是 `ALOLCharacter` 的默认值 `true`（走得进）。同一次纠偏于是变成
「位置按服务器的落点摆 ✓、朝向按本端自己存的旧值写回 ✗」。

**怎么修**：本端在【本端现身那一帧】也调一次 `ApplySpinRotationOverride(true)`，
**放在 `ApplyClientTeleport()` 之前**（顺序反了的话，这一帧迟到的纠偏照样能把它盖掉）；
还原照旧走 `StopSpin()` —— 两端各还原自己那一份。

**验证**：`客户端本端传送 @ (x, y, z) 朝向 N` 与服务器 `P3 现身 @ (x, y, z) 朝向 N` 应该坐标重合、
朝向同值；转圈全程本端朝向不该再动（转的是动画、不是 actor）。

### 变更 7：`GC_DeathHarvestSpin` 的循环粒子改成双手

原来只有一个 `AttachSocket`，粒子只挂在单手。现在左右各生成一份，字段照
`GC_Stealth` 的刀根粒子那套拆开：`LoopSocketLeft` / `LoopSocketRight`（默认就是
`sword_base_l` / `sword_base_r`）、`RelativeOffsetLeft` / `RelativeOffsetRight`、
`ParticleRotationLeft` / `ParticleRotationRight`，`Scale` 共用。
右手插槽是左手的镜像，朝向/偏移反了就在右边那两个字段里补（通常是绕刀刃长轴转 180°）。
插槽名写错时那一侧**不生成**并打一条 warning（`SpawnEmitterAttached` 找不到插槽也照样返回有效组件，
只是把粒子挂在组件原点上 —— 表现是「粒子从脚下冒出来」，看不出是名字错了）。

⚠️ **这次的字段改名会让 BP 里已存的旧值失效**：`AttachSocket` / `RelativeOffset` 两个属性没了，
`GC_DeathHarvest_Spin` BP 要重新配插槽和偏移（改名前那个 BP 本来也没配任何值，见下方待办）。

---

## ★ 动工前先看：对原方案的四处修正

前三条是查引擎源码查出来的，不是你拍脑袋拍出来的东西——第四条是实现细节。

> **另有一条「装的时候才会炸」的坑单列在 §11（Cascade 组件没有 `SetAutoDestroy`）**，
> 因为它不属于"方案错了"，属于"本文档里的代码片段抄不下来"。装之前先扫一眼 §11。

### 修正 1（重要）：`ServerOnly` 会让本人看不到自己的大招动画 → 改用 `ServerInitiated`

原方案选 `ServerOnly`，理由是"客户端纯观众，本人晚一个 RTT 而已"。**后半句不成立：不是晚一个 RTT，是完全没有。**

两条证据，缺一不可（都能在引擎里一行行看到）：

1. **服务器不会告诉本端"我激活了"**：
   `AbilitySystemComponent_Abilities.cpp` 里发 `ClientActivateAbilitySucceed` 的那行有
   `Ability->GetNetExecutionPolicy() != EGameplayAbilityNetExecutionPolicy::ServerOnly` ——
   ServerOnly 被显式排除，客户端永远不会 `CallActivateAbility`，本端就没有任何东西在播蒙太奇。
2. **蒙太奇复制刚好也不给本端**：
   `UAbilitySystemComponent::PlayMontageInternal` 里那句注释是 "Replicate for **non-owners**"，
   而接收端的 `OnRep_ReplicatedAnimMontage` 整个函数体包在 `if (!AbilityActorInfo->IsLocallyControlled())` 里。
   → **模拟代理（其他玩家）看得到，本人看不到。**

所以 `ServerOnly` 的结果是：别人看你放了个大招，你自己屏幕上什么都没发生。

**改用 `ServerInitiated`。** 它就是原方案里被称作"进阶做法、第一版别碰"的那个混合模型，
但**不需要你写任何 RPC** —— 引擎自带：

| | ServerOnly | **ServerInitiated** |
|---|---|---|
| 谁先激活 | 服务器 | 服务器（时间轴仍在服务器，相位通知只在服务器有意义） |
| 本端客户端 | 不跑 `ActivateAbility` | 收到 `ClientActivateAbilitySucceed` 后**本地再跑一次** |
| 本人动画 | ❌ 没有 | ✅ 本地播蒙太奇 |
| 模拟代理动画 | ✅ 蒙太奇复制 | ✅ 蒙太奇复制 |
| 技能结束 | — | ✅ 服务器发 `ClientEndAbility`/`ClientCancelAbility`（`ReplicateEndOrCancelAbility` 里 ServerInitiated 在列） |

原方案列的三条"ServerOnly 理由"在 ServerInitiated 下**一条都没丢**：

- **传送不能预测** → 落点仍然只有服务器算（`HasAuthority()` 门住），没有预测、没有回滚。
  （v6 起客户端那份实例会在【本端现身那一帧】用**同一个** `ComputeDestination` 把自己摆到同一个落点上 ——
  那不是预测，是把服务器已经发生的事在本端复现一遍，理由见 §6.8。）
- **多相位 cue 会双份** → 所有 cue 仍然只在服务器 `ExecuteGameplayCue` / `AddGameplayCue`，
  客户端那份实例一行 cue 都不执行（见下面 `ActivateAbility` 的客户端分支）。**双份的根源不是执行策略，是"两端都调了 cue"，门住就没有。**
- **单一时间轴** → 相位推进的任务只在服务器创建；客户端那份实例唯一的动作是 `PlayMontage`。

代价只有一个，写进代码注释里了：**客户端那份实例绝对不能调 `CommitAbility`**，
否则冷却 GE 会在本端再挂一份（服务端那份会复制过来），本端 CD 会比服务端长一个 RTT。
所以 `ActivateAbility` 一进来就按 `HasAuthority()` 分成两条路，客户端那条只播蒙太奇。

### 修正 2：蒙太奇通知**不是**"只在服务器触发"

原方案说"服务器独占时间轴之后，蒙太奇上的通知只在服务器触发"。**不对：通知在每个播放这条蒙太奇的机器上都会触发**（服务器 + 本人 + 其他玩家，因为蒙太奇在所有这些机器上都在播）。

这不是坏消息，反而是好消息——插件自带的两个 cue 通知是**本地执行、不走网络**的：

```cpp
// AnimNotify_GameplayCue.cpp:81   （Burst）
ProcessGameplayCue(&UGameplayCueManager::ExecuteGameplayCue_NonReplicated, ...);
// AnimNotify_GameplayCue.cpp:116  （State/Looping）
ProcessGameplayCue(&UGameplayCueManager::AddGameplayCue_NonReplicated, ...);
```

`ExecuteGameplayCue_NonReplicated` 只在本机跑一次 → **每台机器各播一份，天生不会双份**，
还天然跟着各自机器上的蒙太奇走。所以规则应该是这样分的：

| 表现 | 走什么 | 为什么 |
|---|---|---|
| 纯自身、不需要服务器算坐标的（起手特效、武器粒子） | **蒙太奇通知**（本地执行，每端各一份） | 不需要 RPC，各端各自跟自己的动画，最准 |
| 坐标/法线得由服务器算的（传送门落点、现身点、每跳命中点） | **服务器 `ExecuteGameplayCue(Parameters.Location)`** | 通知里没有这个坐标，带不过去 |
| 要跟技能生命周期绑的循环表现 | **服务器 `K2_AddGameplayCueWithParams(..., bRemoveOnAbilityEnd=true)`** | 技能一结束（正常或被取消）引擎自动摘干净 |

我们自己写的相位通知（`AnimNotify_SendGameplayEvent`）**必须 `HasAuthority()` 门住**：
客户端那份蒙太奇也会触发它，门住之后客户端上就是个空操作，只有服务器在推进相位。

### 修正 3：`SetActorRotation` 用**只有 Yaw** 的版本

原方案写的是 `(Target->GetActorLocation() - Dest).Rotation()`，这带俯仰。
角色胶囊的 up 轴一旦歪掉，移动组件下一帧会把 pitch/roll 抹平 —— 表现就是"落地瞬间抖一下/瞬移一下"。
改成取 `Yaw` 重新拼一个 `FRotator`。代码见 §5.4。

### 修正 4（小）：三个 Static burst cue 合成一个基类 + 三个空壳

`GC_DeathHarvest_Appear` / `_Hit` / `_TeleportOut` 除了粒子和音效没有任何区别，
三份 45 行的重复文件不值当。改成一个 `UGC_DeathHarvestBurst` 基类（实现 `OnExecute`）+
三个只有构造函数（设各自的 `GameplayCueTag`）的子类。
**注意不能用一个通用类配三个蓝图**：cue 的标签是按**类名**推导的（见 `LOLGameplayTags.h:70-72`），
同一个 C++ 类派生出的三个蓝图会被引擎重新推导成同一个（无效）标签。所以三个子类必须存在，只是它们很短。

---

## 0. 一句话方案

一个 **`ServerInitiated`** 的 `UGA_DeathHarvest`（就是现在那个空壳），用**蒙太奇通知**推进 5 个相位；
**所有世界改动和所有服务器算得出坐标的 GameplayCue 都只在服务器上发生**，客户端那份实例只播蒙太奇；
伤害沿用 `GE_Damage` + `UExecCalc_Damage`，「基于已损失生命值增加」作为 ExecCalc 里的一条系数，不在技能里算。

---

## 1. 定调决策

### 1.1 网络策略 = ServerInitiated
见上面修正 1。要点：服务器先激活并独占时间轴，客户端跟着本地跑一次但只播蒙太奇，所有状态改动 `HasAuthority()` 门住。

### 1.2 相位由蒙太奇通知驱动，不写死秒数

- **纯表现的相位**（武器粒子、起手自身光效）→ 插件自带的 `UAnimNotify_GameplayCue`（Burst）/
  `UAnimNotifyState_GameplayCueState`（Looping），现成的，不用写代码，而且是本地执行不复制。
- **要改游戏状态的相位**（传送、开始/结束转圈）→ 引擎没有现成的 `AnimNotify_GameplayEvent`
  （已确认，GAS 插件里只有 `AnimNotify_GameplayCue.h` 一个 notify 文件），自己写一个十行的 notify 调
  `UAbilitySystemBlueprintLibrary::SendGameplayEventToActor`，技能那头用 `UAbilityTask_WaitGameplayEvent` 接。
  放在 `Public/Animation/` 下，和 `AnimNotifyState_BladeTrail` 做邻居。

这样美术改蒙太奇时长，代码不用跟着改——这是这套架构里"健壮"的主要落点。

### 1.3 目标锁定不引入 TargetData / TargetActor

项目里现在一处 `FGameplayAbilityTargetData` 都没有。关键观察：**只有服务器需要知道锁的是谁**。
客户端要的只是"传送门在哪个坐标"，那个坐标是 cue 参数带过去的。
所以：服务器上取 `GetActorInfo().PlayerController->GetControlRotation()` → 沿朝向做一次球形扫描 →
候选按「有 ASC 且 `Health > 0` → 视线无遮挡 → 夹角最小 → 距离最近」排序取第一个。
零新增网络代码，零新增类。

---

## 2. 相位表

| # | 相位 | 驱动 | 服务器做什么 | 所有客户端看到 | 资产 |
|---|---|---|---|---|---|
| P0 | 锁定 | 输入 R | 选目标、校验、`CommitAbility` | — | — |
| P1 | 起手 | 蒙太奇 Cast 段 | 播蒙太奇、锁移动、断隐身 | `GC.DeathHarvest.Cast`（自身） | `P_Ultimate_Warmup_Limbs`、`P_Ultimate_Weapon_Charge` |
| P2 | 开门 | notify → `Event.DeathHarvest.Portal` | 算落点 → 扫描校验 → 发 cue | `GC.…Portal`（落点）+ `GC.…TeleportOut`（原地） | `P_Ult_Teleport_Enter`、`P_Ultimate_Portal` |
| P3 | 现身 | notify → `Event.DeathHarvest.Teleport` | `SetActorLocation` + `SetActorRotation`（面向目标） | `GC.…Appear`（落点） | `P_Ult_Teleport_Exit`、`P_BackJets_Trail_Burst_Ult` |
| P4 | 转圈 | notify → `Event.DeathHarvest.SpinStart` | `UAbilityTask_Repeat`，每跳球形扫描 → `GE_Damage` | `GC.…Spin`（循环，挂技能）+ 每跳 `GC.…Hit` | `P_Ultimate_Rush_Wind`、`P_Kallari_Meele_SucessfulImpact` |
| P5 | 收招 | 蒙太奇结束 / 被取消 | 恢复移动、`EndAbility`、冷却 | cue 随技能结束自动摘 | — |

**cue 的生命周期全部绑在技能上**：循环的那条用 `K2_AddGameplayCueWithParams(Tag, Params, /*bRemoveOnAbilityEnd=*/true)`
——技能一结束（正常结束或被取消）引擎自动摘干净，不需要写任何清理代码。
这正是 `GAS_Block_Setup.md` §1.3「挂在 GE 的 cue 上，不要手动 Spawn」那条原则的同一个思路：
**让生命周期跟着状态走，而不是跟着调用走。**

---

## 3. Tag 清单

```
State.DeathHarvest.Casting          // 技能期间挂自己：挡重入
State.Cooldown.DeathHarvest         // 冷却（照 GE_StealthCooldown 的写法）
Event.DeathHarvest.Portal           // 蒙太奇通知 → 技能
Event.DeathHarvest.Teleport
Event.DeathHarvest.SpinStart
Event.DeathHarvest.SpinEnd
Data.MissingHealthBonus             // SetByCaller：已损失生命加成系数
GameplayCue.DeathHarvest.Cast       // → BP: GC_DeathHarvest_Cast
GameplayCue.DeathHarvest.TeleportOut// → BP: GC_DeathHarvest_TeleportOut
GameplayCue.DeathHarvest.Portal     // → BP: GC_DeathHarvest_Portal
GameplayCue.DeathHarvest.Appear     // → BP: GC_DeathHarvest_Appear
GameplayCue.DeathHarvest.Spin       // → BP: GC_DeathHarvest_Spin
GameplayCue.DeathHarvest.Hit        // → BP: GC_DeathHarvest_Hit
```

`Ability.Slot.R` 和 `Ability.Type.Ultimate` 已经存在，直接用（`LOLGameplayTags.h:56,61`）。

⚠️ **cue 的 BP 命名要守 `LOLGameplayTags.h:70-72` 自己写下的规矩**：点换下划线，一个字都不能差。
否则引擎会把继承来的 `GameplayCueTag` 清掉、按类名重新推导出一个无效标签，**cue 静默不触发**。
（顺带：`Content/LOL/Blueprints/GAS/GC/` 里的 `GC_ThreeHit_Kallari` 不符合这条规矩——它的父类是
`GC_ThrowDaggerHit`，对应的 tag 应该是 `GameplayCue.ThrowDagger.Hit` → `GC_ThrowDagger_Hit`。
跟这次改动无关，但值得核一下匕首命中特效是不是一直没出来。）

---

## 4. 文件清单

| 文件 | 状态 |
|---|---|
| `Public/GAS/LOLGameplayTags.h` / `Private/GAS/LOLGameplayTags.cpp` | 增补（§5.1） |
| `Public/GAS/DeathHarvestData.h` | 新增（§5.2） |
| `Public/GAS/GA_DeathHarvest.h` / `Private/GAS/GA_DeathHarvest.cpp` | **`git mv` 过来的空壳**，现在是 `Source/LOL/GAS/`，违反 CONVENTIONS 规则 1（§5.3） |
| `Public/GAS/GE_DeathHarvestCooldown.h` / `.cpp` | 新增（§5.4） |
| `Public/GAS/ExecCalc_Damage.h` / `.cpp` | 增补（§5.5） |
| `Public/Animation/AnimNotify_SendGameplayEvent.h` / `.cpp` | 新增（§5.6） |
| `Public/Animation/AnimNotifyState_SocketParticle.h` / `.cpp` | 新增（§5.7） |
| `Public/GAS/GC_DeathHarvestCast.h` / `.cpp` | 新增，Actor 版（§5.8） |
| `Public/GAS/GC_DeathHarvestPortal.h` / `.cpp` | 新增，Actor 版（§5.9） |
| `Public/GAS/GC_DeathHarvestSpin.h` / `.cpp` | 新增，Actor 版（§5.10） |
| `Public/GAS/GC_DeathHarvestBurst.h` / `.cpp` | 新增，Static 版基类 + Appear/Hit/TeleportOut 三个子类（§5.11） |

**复用不改**：`UExecCalc_Damage` 只加捕获和系数（§5.5），`UMyStealthCameraModifier` 的镜头写法留到以后。

编辑器资产清单见 §6。

---

## 5. 代码

### 5.1 `LOLGameplayTags.h` / `.cpp` 增补

`Public/GAS/LOLGameplayTags.h` —— 加在 `GameplayCue_Block` 那一行后面：

```cpp
	// ---------------------------------------------------------------------
	// Death Harvest（R 大招，见 GAS_DeathHarvest_Setup.md）
	// ---------------------------------------------------------------------

	/** 技能期间挂在自己身上：挡重入（技能构造函数里进 ActivationBlockedTags）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_DeathHarvest_Casting);
	/** 冷却（照 State.Cooldown.Stealth 的写法，由 UGE_DeathHarvestCooldown 授予）。 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Cooldown_DeathHarvest);

	// 蒙太奇通知推相位用的事件标签（UAnimNotify_SendGameplayEvent → UAbilityTask_WaitGameplayEvent）。
	// 只在服务器有意义：通知里 HasAuthority() 门住，客户端那份蒙太奇触发了也是空操作。
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_Portal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_Teleport);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_SpinStart);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Event_DeathHarvest_SpinEnd);

	/**
	 * 已损失生命加成系数（SetByCaller）。这里只喂数，公式在 UExecCalc_Damage：
	 * 目标每损失 100% 生命，这一下最多多打 MissingHealthBonus（0.5 = 残血时 ×1.5）。
	 * 不填（或 0）= 没这条加成，对近战/匕首那两条老路径完全无影响。
	 */
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_MissingHealthBonus);

	// Death Harvest 的六个 cue。BP 命名规矩同上面那行注释（点换下划线）：
	//   GameplayCue.DeathHarvest.Portal → GC_DeathHarvest_Portal
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Cast);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_TeleportOut);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Portal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Appear);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Spin);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_DeathHarvest_Hit);
```

`Private/GAS/LOLGameplayTags.cpp` —— 对应加一组 `UE_DEFINE_GAMEPLAY_TAG`（照文件里现有那批的写法）：

```cpp
UE_DEFINE_GAMEPLAY_TAG(State_DeathHarvest_Casting, "State.DeathHarvest.Casting");
UE_DEFINE_GAMEPLAY_TAG(State_Cooldown_DeathHarvest, "State.Cooldown.DeathHarvest");

UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_Portal, "Event.DeathHarvest.Portal");
UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_Teleport, "Event.DeathHarvest.Teleport");
UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_SpinStart, "Event.DeathHarvest.SpinStart");
UE_DEFINE_GAMEPLAY_TAG(Event_DeathHarvest_SpinEnd, "Event.DeathHarvest.SpinEnd");

UE_DEFINE_GAMEPLAY_TAG(Data_MissingHealthBonus, "Data.MissingHealthBonus");

UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Cast, "GameplayCue.DeathHarvest.Cast");
UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_TeleportOut, "GameplayCue.DeathHarvest.TeleportOut");
UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Portal, "GameplayCue.DeathHarvest.Portal");
UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Appear, "GameplayCue.DeathHarvest.Appear");
UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Spin, "GameplayCue.DeathHarvest.Spin");
UE_DEFINE_GAMEPLAY_TAG(GameplayCue_DeathHarvest_Hit, "GameplayCue.DeathHarvest.Hit");
```

> 原生标签注册在 DLL 加载时生效，早于一切 CDO 构造 —— 所以技能的构造函数里可以直接用
> `LOLGameplayTags::State_DeathHarvest_Casting`，不会掉进「CDO 构造阶段字符串查标签返回 None」那个坑。

### 5.2 `Public/GAS/DeathHarvestData.h`（新增）

```cpp
// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DeathHarvestData.generated.h"

class UAnimMontage;
class UGameplayEffect;
class USoundBase;

/**
 * Death Harvest（R 大招）的全部可调数值，仿 UThreeHitPassiveData。
 *
 * 为什么放 DataAsset 而不是技能的 UPROPERTY：一个英雄一份数值，换英雄/调平衡不碰 C++ 也不碰 BP 图，
 * 而且和 UThreeHitPassiveData / 破隐那套「数值只在一个地方」的纪律一致。
 */
UCLASS(BlueprintType)
class LOL_API UDeathHarvestData final : public UDataAsset
{
	GENERATED_BODY()
public:
	// ---------------------------------------------------------------------
	// 动画
	// ---------------------------------------------------------------------

	/** 整条大招蒙太奇。相位全靠它上面的 notify 推（见 GAS_DeathHarvest_Setup.md §1.2）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Anim")
	TObjectPtr<UAnimMontage> Montage;

	/** 从哪一段开始播（留空 = 从头）。循环段/收招段都在蒙太奇里，代码不认段名。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Anim")
	FName MontageStartSection = NAME_None;

	// ---------------------------------------------------------------------
	// P0 锁定
	// ---------------------------------------------------------------------

	/** 锁定最大距离（cm）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Lock", meta=(ClampMin="0", Units="cm"))
	float LockRange = 1400.f;

	/** 锁定锥的半角（度）。以 control rotation 的朝向为轴。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Lock", meta=(ClampMin="0", ClampMax="180", Units="deg"))
	float LockHalfAngle = 60.f;

	/** 锁定时球形扫描的半径：贴脸的目标也能被扫到（只做一条射线会漏掉完全重合的情况）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Lock", meta=(ClampMin="0", Units="cm"))
	float LockSphereRadius = 60.f;

	// ---------------------------------------------------------------------
	// P2 落点（目标身后）
	// ---------------------------------------------------------------------

	/** 落点在目标身后多远（cm）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Blink", meta=(ClampMin="0", Units="cm"))
	float BehindDistance = 140.f;

	/** 贴地检测：从落点上方这么多开始往下打（cm）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Blink", meta=(ClampMin="0", Units="cm"))
	float GroundTraceUp = 150.f;

	/** 贴地检测：往下打这么多（cm）。打不中地面就用原始 Z（悬空/浮空平台会碰到）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Blink", meta=(ClampMin="0", Units="cm"))
	float GroundTraceDown = 400.f;

	// ---------------------------------------------------------------------
	// P4 转圈
	// ---------------------------------------------------------------------

	/** 每两跳之间的间隔（秒）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Spin", meta=(ClampMin="0.01", Units="s"))
	float SpinInterval = 0.25f;

	/** 一共跳几次。跳完伤害就停（蒙太奇可能还在播收招）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Spin", meta=(ClampMin="1"))
	int32 SpinCount = 8;

	/** 每跳的球形扫描半径（cm）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Spin", meta=(ClampMin="0", Units="cm"))
	float SpinHitRadius = 320.f;

	/**
	 * true = 蒙太奇自带 root motion 的 yaw，代码【不】驱动旋转（最干净，一根手指都不用动）。
	 * false = 代码按 SpinRate 每帧 AddActorWorldRotation。
	 *
	 * ⚠️ 编辑器里打开 Ability_Ultimate 看一眼 root motion 有没有 yaw 再决定，别猜。
	 * 选错的表现：true 但动画没转 → 人站着不动地打范围伤害；false 但动画自带转 → 转两倍速。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Spin")
	bool bRootMotionSpin = false;

	/** 代码驱动旋转时的角速度（度/秒）。360 = 一秒一圈。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Spin", meta=(ClampMin="0", Units="deg/s"))
	float SpinRate = 540.f;

	// ---------------------------------------------------------------------
	// 伤害
	// ---------------------------------------------------------------------

	/** Instant GE，靠 SetByCaller 吃 Data.DamageMultiplier / Data.FlatDamage / Data.MissingHealthBonus。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage")
	TSubclassOf<UGameplayEffect> DamageEffect;

	/** 攻击力倍率（原始伤害 = 攻击者 AttackDamage × 这个 + FlatDamage）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage", meta=(ClampMin="0"))
	float DamageMultiplier = 1.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage", meta=(ClampMin="0"))
	float FlatDamage = 0.f;

	/**
	 * 斩杀系数：目标每损失 100% 生命，这一下最多多打这么多（0.5 = 残血时 ×1.5）。
	 * ★ 加成对象是【目标的】已损失生命值，公式在 UExecCalc_Damage（§5.5）。
	 * 想改成施法者的，把 ExecCalc 里那两条捕获从 Target 改成 Source 即可，一行的事。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Damage", meta=(ClampMin="0"))
	float MissingHealthBonus = 0.5f;

	// ---------------------------------------------------------------------
	// 音效（蒙太奇里没配 AnimNotify_PlaySound 时在这里补）
	// ---------------------------------------------------------------------

	/** 起手那一下的语音/音效。留空 = 无声。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Sound")
	TSoftObjectPtr<USoundBase> CastSound;
};
```

### 5.3 `Public/GAS/GA_DeathHarvest.h` / `Private/GAS/GA_DeathHarvest.cpp`

先搬文件，保住历史：

```bash
git mv Source/LOL/GAS/GA_DeathHarvest.h Source/LOL/Public/GAS/GA_DeathHarvest.h
git mv Source/LOL/GAS/GA_DeathHarvest.cpp Source/LOL/Private/GAS/GA_DeathHarvest.cpp
grep -rn "GAS/GA_DeathHarvest" Source/     # 确认 include 没有旧路径（规则 6 的自查）
```

`Public/GAS/GA_DeathHarvest.h`：

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GAS/MyGameplayAbility.h"
#include "GameplayEffectTypes.h"
#include "GA_DeathHarvest.generated.h"

class ACharacter;
class UAbilityTask_PlayMontageAndWait;
class UAbilityTask_Repeat;
class UAbilityTask_WaitGameplayEvent;
class UAnimMontage;
class UDeathHarvestData;
class UGameplayEffect;
class USkeletalMeshComponent;
struct FGameplayEventData;

/**
 * R 大招「Death Harvest」：锁定 → 起手 → 开传送门 → 从目标身后现身 → 原地转圈 AOE → 收招。
 *
 * 网络模型（全项目唯一的例外，见 GAS_DeathHarvest_Setup.md §1.1）：
 *  NetExecutionPolicy = ServerInitiated。服务器先激活、独占时间轴；确认之后引擎会给本端客户端
 *  发 ClientActivateAbilitySucceed，本端跟着本地再跑一次 ActivateAbility ——
 *  客户端这一次【只播蒙太奇】，其余一律 HasAuthority() 门住。
 *
 *  为什么不用 ServerOnly：ServerOnly 时引擎不给本端发 ClientActivateAbilitySucceed
 *  （AbilitySystemComponent_Abilities.cpp 里 `!= ServerOnly` 那个判断），而蒙太奇复制是
 *  "for non-owners"（OnRep_ReplicatedAnimMontage 整个函数包在 !IsLocallyControlled() 里），
 *  两头都堵死 → 【本人看不到自己的大招动画】。ServerInitiated 三个好处一个不少：
 *  传送仍然只有服务器动、cue 仍然只有服务器发、相位通知仍然只在服务器有意义。
 *
 *  代价（写死在代码里了）：客户端那份实例【绝对不能】调 CommitAbility —— 否则冷却 GE 会在本端
 *  再挂一份（服务端那份还会复制过来），本端 CD 比服务端长一个 RTT。见 ActivateAbility 的分支。
 */
UCLASS(Blueprintable)
class LOL_API UGA_DeathHarvest : public UMyGameplayAbility
{
	GENERATED_BODY()
public:
	UGA_DeathHarvest();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

	/** 全部可调数值（一个英雄一份，见 UDeathHarvestData 的注释）。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category="Config")
	TObjectPtr<UDeathHarvestData> AbilityData;

protected:
	// --- 相位（全部由蒙太奇上的 AnimNotify_SendGameplayEvent 触发，且只在服务器接得到） ---

	UFUNCTION() void OnPortalEvent(FGameplayEventData Payload);
	UFUNCTION() void OnTeleportEvent(FGameplayEventData Payload);
	UFUNCTION() void OnSpinStartEvent(FGameplayEventData Payload);
	UFUNCTION() void OnSpinEndEvent(FGameplayEventData Payload);
	/** 蒙太奇播完/被打断。客户端那份【不】结束技能，收尾由服务器发 ClientEndAbility。 */
	UFUNCTION() void OnMontageFinished();

	// --- 转圈伤害跳（UAbilityTask_Repeat） ---
	UFUNCTION() void OnSpinPulse(int32 ActionNumber);

private:
	// ---------------------------------------------------------------------
	// 服务器侧
	// ---------------------------------------------------------------------

	/** P0：沿 control rotation 的朝向做球形扫描，按「有 ASC 且活着 → 无遮挡 → 夹角最小 → 最近」选一个。 */
	AActor* SelectTarget() const;

	/**
	 * P2：算落点。目标身后 → 贴地 → 胶囊扫描判站得下。
	 * 身后站不下就按候选表退让：身后 → 左后 → 右后 → 正前 → 目标正上方。
	 * 返回 false = 全废（调用方取消技能并退冷却）。
	 */
	bool ComputeDestination(AActor* Target, FVector& OutDestination) const;

	/** 落点能不能站下（胶囊扫描，照 GA_Flash::TryFindBlinkDestination 的写法）。 */
	bool IsSpotFree(const FVector& Location, const AActor* IgnoredActor) const;

	/** 服务器发一条带坐标的 cue。所有客户端各播一次（ExecuteGameplayCue 会多播）。 */
	void ExecuteLocationCue(const FGameplayTag CueTag, const FVector& Location, const FVector& Normal = FVector::ZeroVector) const;

	/** 一跳：球形扫描 → 每个目标填 spec + 施加 + 命中 cue。 */
	void OnSpinPulse_Server();
	void ApplySpinDamage(AActor* Target, const FHitResult& Hit);

	void StartSpin();
	void StopSpin();

	/** 锁移动（StopMovementImmediately + MOVE_None），恢复时还原原模式。 */
	void ApplyMovementLock(bool bLock);

	/** 转圈期间的旋转：关掉移动组件的自动转向 + 按 SpinRate 每帧转（bRootMotionSpin 时整个跳过）。 */
	void ApplySpinRotationOverride(bool bOn);
	void TickSpinYaw();

	/** 技能失败收尾：退冷却 + EndAbility(cancelled)。落点全废/目标丢失都走这里。 */
	void CancelAsFailed();

	/** 客户端那一次：只播蒙太奇，别的一律不做。 */
	void PlayClientMontage();
	/** 服务器那一次：播蒙太奇 + 挂四个相位监听任务。 */
	void PlayServerMontage();

	USkeletalMeshComponent* GetAvatarMesh() const;

	// ---------------------------------------------------------------------
	// 状态
	// ---------------------------------------------------------------------

	/** 锁定的目标。弱引用：目标中途被销毁时 Get() 返回 null，每个相位边界都重校验一次。 */
	TWeakObjectPtr<AActor> LockedTarget;

	/** P2 算出来的落点，P3 用。只在服务器有意义（客户端不传送）。 */
	FVector PortalLocation = FVector::ZeroVector;

	UPROPERTY(Transient) TObjectPtr<UAbilityTask_PlayMontageAndWait> MontageTask;
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitGameplayEvent> PortalTask;
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitGameplayEvent> TeleportTask;
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitGameplayEvent> SpinStartTask;
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_WaitGameplayEvent> SpinEndTask;

	/** 伤害跳。用任务而不是裸 FTimerHandle：任务跟着技能一起死，天生不会有孤儿计时器。 */
	UPROPERTY(Transient) TObjectPtr<UAbilityTask_Repeat> SpinTask;

	/**
	 * 逐帧转 yaw 用的 timer。
	 *
	 * 这里【没有】用 UAbilityTask_Repeat：转 yaw 是连续量，不是"跳 N 次"的语义，
	 * Repeat 那个 TotalActionCount 在这儿只会变成一个魔数。所以退回裸 timer ——
	 * 唯一的纪律是 EndAbility 里一定要 ClearTimer（转圈被硬停时不会有人来关它）。
	 */
	FTimerHandle SpinYawTimer;
	bool bSpinYawActive = false;

	/** 锁移动前记下的模式，恢复时用（默认值只为让"没锁过就恢复"是安全的）。 */
	EMovementMode SavedMovementMode = MOVE_Walking;
	bool bMovementLocked = false;

	/** 旋转覆盖是否生效 + 被覆盖前的原值（照 ThrowDaggerAbility 那套，只在真关过时才恢复）。 */
	bool bOrientRotationOverridden = false;
	bool bSavedOrientRotationToMovement = true;
	bool bSavedUseControllerRotationYaw = false;

	/** State.DeathHarvest.Casting 是不是我挂的（只挂/摘自己挂的那一份）。 */
	bool bCastingTagAdded = false;
};
```

`Private/GAS/GA_DeathHarvest.cpp`：

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GA_DeathHarvest.h"
#include "GAS/DeathHarvestData.h"
#include "GAS/GE_DeathHarvestCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "Abilities/Tasks/AbilityTask_PlayMontageAndWait.h"
#include "Abilities/Tasks/AbilityTask_Repeat.h"
#include "Abilities/Tasks/AbilityTask_WaitGameplayEvent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimMontage.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"

UGA_DeathHarvest::UGA_DeathHarvest()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// ★ 全项目唯一的 ServerInitiated，理由见头文件注释 / GAS_DeathHarvest_Setup.md §1.1。
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerInitiated;

	ActivationPolicy = EMyAbilityActivationPolicy::OnInputTriggered;

	CooldownDuration = 90.f;
	CooldownGameplayEffectClass = UGE_DeathHarvestCooldown::StaticClass();

	// 大招标签：以后「施法者死亡/被控时取消所有大招」那条路要 CancelAbilitiesWithTag(Ability.Type.Ultimate)，
	// 现在项目里还没有那条路（见 GAS_DeathHarvest_Setup.md §7 的架构缺口），但标签先挂上。
	AbilityTags.AddTag(LOLGameplayTags::Ability_Type_Ultimate);

	// 挡重入。技能期间挂自己的 State.DeathHarvest.Casting（服务器本地标签，见 ActivateAbility）。
	// 冷却 GE 其实也挡得住，但冷却要等 CommitAbility 之后才有 —— 这两个覆盖的时间窗不一样。
	ActivationBlockedTags.AddTag(LOLGameplayTags::State_DeathHarvest_Casting);

	// 从隐身开 R：走的还是 MyAbilitySystemComponent::AbilityInputTagPressed 那条集中消费的路，
	// 顺序天然是「破隐 burst → 起手」，这里不用写任何额外代码（GAS_DeathHarvest_Setup.md §5.1）。
	bBreaksStealthOnCast = true;
}

// =============================================================================
// 激活
// =============================================================================

void UGA_DeathHarvest::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}

	// ---------------------------------------------------------------------
	// 客户端这一次：只播蒙太奇。
	//
	// ServerInitiated 下服务器激活成功后会告诉本端，本端在这里本地跑一次 ActivateAbility。
	// 这一次除蒙太奇之外【什么都不做】：
	//   - 不能 CommitAbility → 否则冷却 GE 在本端再挂一份，本端 CD 比服务端长一个 RTT；
	//   - 不能动坐标 → 传送是服务器权威的（和 GA_Flash 一致）；
	//   - 不能发 cue → 服务器 ExecuteGameplayCue 会多播过来，本端再发一次就是两份。
	// 技能什么时候结束也由服务器说了算（服务器 EndAbility 会发 ClientEndAbility）。
	// ---------------------------------------------------------------------
	if (!Avatar->HasAuthority())
	{
		PlayClientMontage();
		return;
	}

	// ---------------------------------------------------------------------
	// 下面全部是服务器
	// ---------------------------------------------------------------------

	// P0 锁定。【必须在 CommitAbility 之前】：没选中目标就是"这次没放出去"，不该进冷却。
	AActor* Target = SelectTarget();
	if (!Target)
	{
		// 不打冷却、不进相位，直接结束。bWasCancelled=true 和 GA_Flash 里 CommitAbility 失败那条一致。
		UE_LOG(LogTemp, Log, TEXT("[DeathHarvest] 没锁到目标（范围 %.0f / 半角 %.0f°）→ 不激活"),
			AbilityData ? AbilityData->LockRange : 0.f, AbilityData ? AbilityData->LockHalfAngle : 0.f);
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	LockedTarget = Target;

	// 挡重入的标签。放在【服务器本地】是够的：客户端再按 R 只是发一个 ServerTryActivateAbility 请求，
	// 判定在服务器做（ActivationBlockedTags 命中 → 拒绝），客户端本地有没有这个标签无所谓。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->AddLooseGameplayTag(LOLGameplayTags::State_DeathHarvest_Casting);
		bCastingTagAdded = true;
	}

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 锁定目标 %s（距离 %.0f）"),
		*GetNameSafe(Target), FVector::Dist(Avatar->GetActorLocation(), Target->GetActorLocation()));

	PlayServerMontage();
}

void UGA_DeathHarvest::PlayClientMontage()
{
	if (!AbilityData || !AbilityData->Montage)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 客户端：AbilityData/Montage 没配 → 本人这一侧看不到动画"));
		return;
	}

	// 本端自己播一条蒙太奇。bStopWhenAbilityEnds=true：服务器 EndAbility 后本端这条会被停掉。
	UAbilityTask_PlayMontageAndWait* Task = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, NAME_None, AbilityData->Montage, 1.f, AbilityData->MontageStartSection, /*bStopWhenAbilityEnds=*/true);

	// 刻意【不】绑 OnCompleted/OnInterrupted：客户端蒙太奇播完不代表技能结束（时间轴在服务器），
	// 在这儿 EndAbility 会变成客户端抢先结束技能。收尾只认服务器的 ClientEndAbility。
	Task->ReadyForActivation();
}

void UGA_DeathHarvest::PlayServerMontage()
{
	if (!AbilityData || !AbilityData->Montage)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] Montage 没配 → 相位推进不了，直接结束"));
		CancelAsFailed();
		return;
	}

	// 四个相位监听任务先挂上，再播蒙太奇：蒙太奇第一帧就可能命中 notify，
	// 反过来写会丢事件（EventReceived 是一次性的，OnlyTriggerOnce=true）。
	auto WaitFor = [this](const FGameplayTag EventTag) -> UAbilityTask_WaitGameplayEvent*
	{
		return UAbilityTask_WaitGameplayEvent::WaitGameplayEvent(this, EventTag, nullptr, /*OnlyTriggerOnce=*/true, /*OnlyMatchExact=*/true);
	};

	PortalTask = WaitFor(LOLGameplayTags::Event_DeathHarvest_Portal);
	PortalTask->EventReceived.AddDynamic(this, &UGA_DeathHarvest::OnPortalEvent);
	PortalTask->ReadyForActivation();

	TeleportTask = WaitFor(LOLGameplayTags::Event_DeathHarvest_Teleport);
	TeleportTask->EventReceived.AddDynamic(this, &UGA_DeathHarvest::OnTeleportEvent);
	TeleportTask->ReadyForActivation();

	SpinStartTask = WaitFor(LOLGameplayTags::Event_DeathHarvest_SpinStart);
	SpinStartTask->EventReceived.AddDynamic(this, &UGA_DeathHarvest::OnSpinStartEvent);
	SpinStartTask->ReadyForActivation();

	SpinEndTask = WaitFor(LOLGameplayTags::Event_DeathHarvest_SpinEnd);
	SpinEndTask->EventReceived.AddDynamic(this, &UGA_DeathHarvest::OnSpinEndEvent);
	SpinEndTask->ReadyForActivation();

	MontageTask = UAbilityTask_PlayMontageAndWait::CreatePlayMontageAndWaitProxy(
		this, NAME_None, AbilityData->Montage, 1.f, AbilityData->MontageStartSection, /*bStopWhenAbilityEnds=*/true);
	MontageTask->OnCompleted.AddDynamic(this, &UGA_DeathHarvest::OnMontageFinished);
	MontageTask->OnBlendOut.AddDynamic(this, &UGA_DeathHarvest::OnMontageFinished);
	MontageTask->OnInterrupted.AddDynamic(this, &UGA_DeathHarvest::OnMontageFinished);
	MontageTask->OnCancelled.AddDynamic(this, &UGA_DeathHarvest::OnMontageFinished);
	MontageTask->ReadyForActivation();

	if (AbilityData->CastSound.IsNull() == false)
	{
		if (USoundBase* Sound = AbilityData->CastSound.LoadSynchronous())
		{
			if (AActor* Avatar = GetAvatarActorFromActorInfo())
			{
				UGameplayStatics::PlaySoundAtLocation(this, Sound, Avatar->GetActorLocation());
			}
		}
	}
}

void UGA_DeathHarvest::OnMontageFinished()
{
	// 客户端那份蒙太奇结束不代表技能结束（时间轴在服务器）——收尾只认服务器发来的 ClientEndAbility。
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->HasAuthority())
	{
		return;
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
		/*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}

// =============================================================================
// P0 锁定
// =============================================================================

AActor* UGA_DeathHarvest::SelectTarget() const
{
	const AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !AbilityData)
	{
		return nullptr;
	}

	// control rotation 跟着移动 RPC 复制到服务器，这里读到的是当前值 —— 不需要任何 TargetData。
	FVector Forward = Avatar->GetActorForwardVector();
	if (const FGameplayAbilityActorInfo* Info = GetCurrentActorInfo())
	{
		if (const AController* Controller = Info->PlayerController.Get())
		{
			Forward = FRotationMatrix(FRotator(0.f, Controller->GetControlRotation().Yaw, 0.f)).GetUnitAxis(EAxis::X);
		}
	}

	const FVector Start = Avatar->GetActorLocation();
	const FVector End = Start + Forward * AbilityData->LockRange;

	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeathHarvestLock), false, Avatar);
	const FCollisionShape Shape = FCollisionShape::MakeSphere(AbilityData->LockSphereRadius);

	if (!GetWorld() || !GetWorld()->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, ECC_Pawn, Shape, Params))
	{
		return nullptr;
	}

	const float CosHalfAngle = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(AbilityData->LockHalfAngle, 0.f, 180.f)));

	AActor* Best = nullptr;
	float BestScore = -1.f;

	TSet<AActor*> Seen;
	for (const FHitResult& Hit : Hits)
	{
		AActor* Candidate = Hit.GetActor();
		if (!Candidate || Candidate == Avatar || Seen.Contains(Candidate))
		{
			continue;
		}
		Seen.Add(Candidate);

		// ① 有 ASC 且还活着。没有 ASC 的东西（木桩/场景）直接出局 —— 这个技能只对人放。
		UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Candidate);
		if (!TargetASC)
		{
			continue;
		}
		// 活着 = 取得到 Health 且 > 0。取不到（bFound=false，属性集还没就绪）时当活着，
		// 别在属性集刚初始化那一瞬间把所有人都判死。
		bool bFound = false;
		const float Health = TargetASC->GetGameplayAttributeValue(UHeroCombatAttributeSet::GetHealthAttribute(), bFound);
		if (bFound && Health <= 0.f)
		{
			continue;
		}

		// ② 视线无遮挡：从胸口到胸口一条射线，挡住的不算（不然后排的人会被"透视"锁上）。
		const FVector EyeLoc = Start + FVector(0.f, 0.f, 60.f);
		const FVector TargetLoc = Candidate->GetActorLocation() + FVector(0.f, 0.f, 60.f);
		FCollisionQueryParams LosParams(SCENE_QUERY_STAT(DeathHarvestLockLos), false, Avatar);
		LosParams.AddIgnoredActor(Candidate);
		FHitResult LosHit;
		if (GetWorld()->LineTraceSingleByChannel(LosHit, EyeLoc, TargetLoc, ECC_Visibility, LosParams))
		{
			continue;
		}

		// ③ 夹角最小 → ④ 距离最近。合成一个分数：夹角越小分越高，同角度时近的赢。
		const FVector ToTarget = (Candidate->GetActorLocation() - Start).GetSafeNormal();
		const float Cos = FVector::DotProduct(Forward, ToTarget);
		if (Cos < CosHalfAngle)
		{
			continue;
		}
		const float Distance = FVector::Dist(Start, Candidate->GetActorLocation());
		const float Score = Cos * 10000.f - Distance;   // 夹角优先，距离只做同角度的 tie-break

		if (Score > BestScore)
		{
			BestScore = Score;
			Best = Candidate;
		}
	}

	return Best;
}

// =============================================================================
// P2 开门 / P3 现身
// =============================================================================

void UGA_DeathHarvest::OnPortalEvent(FGameplayEventData Payload)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	AActor* Target = LockedTarget.Get();

	// 每个相位边界都重校验一次：目标可能在起手这段时间里死了/被销毁了。
	if (!Avatar || !Avatar->HasAuthority() || !Target)
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 开门时目标已失效 → 取消"));
		CancelAsFailed();
		return;
	}

	FVector Dest;
	if (!ComputeDestination(Target, Dest))
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 落点全被挡（目标 %s）→ 取消并退冷却"), *GetNameSafe(Target));
		CancelAsFailed();
		return;
	}

	PortalLocation = Dest;

	// 传送门开在落点上，朝向目标（法线当她"从门里看出去"的方向）。
	// ⚠️ 坐标必须这样带过去：Actor 版 cue 的生成位置取的是 TargetActor 的坐标
	// （GameplayCueManager.cpp:528 用的是 TargetActor->GetActorLocation()），不是 Parameters.Location，
	// 所以 cue 的 OnActive 里还得自己 SetActorLocation 挪一次 —— 见 GC_DeathHarvestPortal。
	const FVector ToTarget = (Target->GetActorLocation() - Dest).GetSafeNormal();
	ExecuteLocationCue(LOLGameplayTags::GameplayCue_DeathHarvest_Portal, Dest, ToTarget);

	// 她原地消失的那一下（在老位置，不是落点）。
	ExecuteLocationCue(LOLGameplayTags::GameplayCue_DeathHarvest_TeleportOut, Avatar->GetActorLocation());

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 开门 @ (%.0f, %.0f, %.0f)，目标 %s"),
		Dest.X, Dest.Y, Dest.Z, *GetNameSafe(Target));
}

void UGA_DeathHarvest::OnTeleportEvent(FGameplayEventData Payload)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	AActor* Target = LockedTarget.Get();
	if (!Avatar || !Avatar->HasAuthority() || !Target)
	{
		CancelAsFailed();
		return;
	}

	ACharacter* Character = Cast<ACharacter>(Avatar);
	if (!Character)
	{
		CancelAsFailed();
		return;
	}

	// 只有服务器动坐标，和 GA_Flash.cpp:41-53 一字不差地一致。
	Character->SetActorLocation(PortalLocation, /*bSweep=*/false, nullptr, ETeleportType::None);

	// ★ 只取 Yaw：带上俯仰的话胶囊的 up 轴会歪，移动组件下一帧把它抹平 —— 表现是"落地瞬间抖一下"。
	const FVector ToTarget = Target->GetActorLocation() - PortalLocation;
	Character->SetActorRotation(FRotator(0.f, ToTarget.Rotation().Yaw, 0.f));

	ExecuteLocationCue(LOLGameplayTags::GameplayCue_DeathHarvest_Appear, PortalLocation);

	UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 现身 @ (%.0f, %.0f, %.0f)"), PortalLocation.X, PortalLocation.Y, PortalLocation.Z);
}

bool UGA_DeathHarvest::ComputeDestination(AActor* Target, FVector& OutDestination) const
{
	const AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Target || !AbilityData || !GetWorld())
	{
		return false;
	}

	const FVector TargetLoc = Target->GetActorLocation();
	const FVector TargetForward = Target->GetActorForwardVector();
	const FVector TargetRight = Target->GetActorRightVector();

	// 候选表：身后 → 左后 → 右后 → 正前 → 目标正上方。全废才返回 false。
	const float Behind = AbilityData->BehindDistance;
	const TArray<FVector> Candidates =
	{
		TargetLoc - TargetForward * Behind,
		TargetLoc - TargetForward * Behind + TargetRight * Behind,
		TargetLoc - TargetForward * Behind - TargetRight * Behind,
		TargetLoc + TargetForward * Behind,
		TargetLoc + FVector(0.f, 0.f, Behind * 2.f),
	};

	for (const FVector& Candidate : Candidates)
	{
		FVector Grounded = Candidate;

		// 贴地：从候选点上方往下打一条，打中地面就落在地面上 —— 别用目标自身的 Z
		// （目标站在斜坡/台阶上时，那个 Z 会让落点悬空或陷进地里）。
		const FVector TraceStart = Candidate + FVector(0.f, 0.f, AbilityData->GroundTraceUp);
		const FVector TraceEnd = Candidate - FVector(0.f, 0.f, AbilityData->GroundTraceDown);
		FCollisionQueryParams GroundParams(SCENE_QUERY_STAT(DeathHarvestGround), false, Avatar);
		GroundParams.AddIgnoredActor(Target);
		FHitResult GroundHit;
		if (GetWorld()->LineTraceSingleByChannel(GroundHit, TraceStart, TraceEnd, ECC_Visibility, GroundParams))
		{
			Grounded = GroundHit.Location;
		}

		// 站得下吗（胶囊扫描，照 GA_Flash::TryFindBlinkDestination）。
		if (IsSpotFree(Grounded, Avatar))
		{
			OutDestination = Grounded;
			return true;
		}
	}

	return false;
}

bool UGA_DeathHarvest::IsSpotFree(const FVector& Location, const AActor* IgnoredActor) const
{
	const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	if (!Character || !GetWorld())
	{
		return false;
	}

	const UCapsuleComponent* Capsule = Character->GetCapsuleComponent();
	const float Radius = Capsule ? Capsule->GetScaledCapsuleRadius() : 34.f;
	const float HalfHeight = Capsule ? Capsule->GetScaledCapsuleHalfHeight() : 88.f;

	// 胶囊扫掠：把一个胶囊"放"在这个位置，看它跟世界有没有重叠。
	// 用 sweep（起点=终点）而不是 overlap：和 GA_Flash 走同一条通道（ECC_Pawn），行为一致。
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeathHarvestSpotCheck), false, IgnoredActor);
	Params.AddIgnoredActor(IgnoredActor);

	FHitResult Hit;
	const FVector Center = Location + FVector(0.f, 0.f, HalfHeight);
	return !GetWorld()->SweepSingleByChannel(
		Hit, Center, Center, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, HalfHeight), Params);
}

// =============================================================================
// P4 转圈
// =============================================================================

void UGA_DeathHarvest::OnSpinStartEvent(FGameplayEventData Payload)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->HasAuthority())
	{
		return;
	}
	StartSpin();
}

void UGA_DeathHarvest::OnSpinEndEvent(FGameplayEventData Payload)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->HasAuthority())
	{
		return;
	}
	StopSpin();
}

void UGA_DeathHarvest::StartSpin()
{
	if (!AbilityData || SpinTask)
	{
		return;
	}

	// 锁移动。ServerInitiated 下服务器锁住就是真的锁住（客户端那份实例不碰移动组件）。
	ApplyMovementLock(true);
	ApplySpinRotationOverride(true);

	// 循环表现挂技能上：技能一结束（正常结束或被取消）引擎自动摘干净，一行清理代码都不用写。
	// 循环的那条走 Add（挂上去等移除），一次性的那几条走 Execute（见 ExecuteLocationCue）。
	if (AActor* Avatar = GetAvatarActorFromActorInfo())
	{
		FGameplayCueParameters Params;
		Params.Instigator = Avatar;
		Params.EffectCauser = Avatar;
		Params.Location = Avatar->GetActorLocation();
		K2_AddGameplayCueWithParams(LOLGameplayTags::GameplayCue_DeathHarvest_Spin, Params, /*bRemoveOnAbilityEnd=*/true);
	}

	// 伤害跳用 Repeat 任务，不用裸 FTimerHandle：任务跟着技能一起死，天生不会有孤儿计时器。
	// （ThrowDaggerAbility 用的是裸 FTimerHandle，所以它得在 EndAbility 里自己清 —— 那是要避免的写法。）
	SpinTask = UAbilityTask_Repeat::RepeatAction(this, AbilityData->SpinInterval, AbilityData->SpinCount);
	SpinTask->OnPerformAction.AddDynamic(this, &UGA_DeathHarvest::OnSpinPulse);
	SpinTask->ReadyForActivation();
}

void UGA_DeathHarvest::StopSpin()
{
	if (SpinTask)
	{
		SpinTask->EndTask();
		SpinTask = nullptr;
	}

	ApplySpinRotationOverride(false);
	ApplyMovementLock(false);

	// 收招时把朝向收在目标那边，别停下来背对着人。
	if (AActor* Target = LockedTarget.Get())
	{
		if (AActor* Avatar = GetAvatarActorFromActorInfo())
		{
			const FVector ToTarget = Target->GetActorLocation() - Avatar->GetActorLocation();
			Avatar->SetActorRotation(FRotator(0.f, ToTarget.Rotation().Yaw, 0.f));
		}
	}
}

void UGA_DeathHarvest::OnSpinPulse(int32 ActionNumber)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->HasAuthority() || !AbilityData)
	{
		return;
	}
	OnSpinPulse_Server();
}

void UGA_DeathHarvest::OnSpinPulse_Server()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !GetWorld() || !AbilityData)
	{
		return;
	}

	// 绕自身的球形扫描（抄 GA_ThreeHitPassive.cpp:424-429 的形状，只是把"沿朝向扫"换成"原地扫"）。
	TArray<FHitResult> Hits;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(DeathHarvestSpin), false, Avatar);
	const FVector Center = Avatar->GetActorLocation();
	const FCollisionShape Shape = FCollisionShape::MakeSphere(AbilityData->SpinHitRadius);

	if (!GetWorld()->SweepMultiByChannel(Hits, Center, Center, FQuat::Identity, ECC_Pawn, Shape, Params))
	{
		return;
	}

	// 同一次脉冲里同一个目标可能被返回多次（多段重叠），去重。
	// 目标在转圈中死亡【不中断】：这是范围伤害，继续打完（见 §7 的取消矩阵）。
	TSet<AActor*> Damaged;
	for (const FHitResult& Hit : Hits)
	{
		AActor* Target = Hit.GetActor();
		if (!Target || Target == Avatar || Damaged.Contains(Target))
		{
			continue;
		}
		Damaged.Add(Target);
		ApplySpinDamage(Target, Hit);
	}
}

void UGA_DeathHarvest::ApplySpinDamage(AActor* Target, const FHitResult& Hit)
{
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
	if (!SourceASC || !TargetASC || !AbilityData->DamageEffect)
	{
		// 不静默：Spec 无效 = 这一下一点伤害都没有，屏幕上看起来像"打空了"。
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] 目标 %s 没有 ASC（或 DamageEffect 没配）→ 本次命中无伤害"),
			*GetNameSafe(Target));
		return;
	}

	// 这里【只喂参数，不算伤害】：公式只在 UExecCalc_Damage 里（见 GAS_Block_Setup.md §3.6）。
	FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
	Context.AddHitResult(Hit);

	FGameplayEffectSpecHandle Spec = SourceASC->MakeOutgoingSpec(AbilityData->DamageEffect, GetAbilityLevel(), Context);
	if (!Spec.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[DeathHarvest] DamageEffect(%s) 的 Spec 无效 → 本次命中无伤害"),
			*GetNameSafe(AbilityData->DamageEffect));
		return;
	}

	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_DamageMultiplier, AbilityData->DamageMultiplier);
	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_FlatDamage, AbilityData->FlatDamage);
	// 斩杀系数：目标越残血打得越疼。公式在 UExecCalc_Damage（§5.5），这边只喂系数。
	Spec.Data->SetSetByCallerMagnitude(LOLGameplayTags::Data_MissingHealthBonus, AbilityData->MissingHealthBonus);
	// 物理伤害。不挂类型标签时 ExecCalc 也按物理算，显式挂上是为了以后加魔法大招时不用回头猜默认值。
	Spec.Data->AddDynamicAssetTag(LOLGameplayTags::Damage_Physical);

	SourceASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), TargetASC);

	// 命中表现走 cue：这个函数只在服务端跑，直接 Spawn 的话粒子只有主机看得到
	// （GC_ThrowDaggerHit 修掉的是同一个坑）。
	ExecuteLocationCue(LOLGameplayTags::GameplayCue_DeathHarvest_Hit, Hit.ImpactPoint, Hit.ImpactNormal);
}

// =============================================================================
// 移动 / 旋转 / 表现
// =============================================================================

void UGA_DeathHarvest::ApplyMovementLock(bool bLock)
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	UCharacterMovementComponent* MoveComp = Character ? Character->GetCharacterMovement() : nullptr;
	if (!MoveComp)
	{
		return;
	}

	if (bLock)
	{
		if (!bMovementLocked)
		{
			SavedMovementMode = MoveComp->MovementMode;
			bMovementLocked = true;
		}
		MoveComp->StopMovementImmediately();
		MoveComp->DisableMovement();     // = SetMovementMode(MOVE_None)
	}
	else if (bMovementLocked)
	{
		MoveComp->SetMovementMode(SavedMovementMode);
		bMovementLocked = false;
	}
}

void UGA_DeathHarvest::ApplySpinRotationOverride(bool bOn)
{
	ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo());
	UCharacterMovementComponent* MoveComp = Character ? Character->GetCharacterMovement() : nullptr;
	if (!Character || !MoveComp || !AbilityData)
	{
		return;
	}

	if (!bOn)
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(SpinYawTimer);
		}
		bSpinYawActive = false;

		// 只在真正关过的时候恢复，避免取消/提前结束路径把默认值误写回去
		// （照 ThrowDaggerAbility.cpp:302 那个 bOrientRotationOverridden 的写法）。
		if (bOrientRotationOverridden)
		{
			MoveComp->bOrientRotationToMovement = bSavedOrientRotationToMovement;
			Character->bUseControllerRotationYaw = bSavedUseControllerRotationYaw;
			bOrientRotationOverridden = false;
		}
		return;
	}

	// 动画自带 root motion 的 yaw → 代码什么都不用做，最干净。
	if (AbilityData->bRootMotionSpin)
	{
		return;
	}

	// 关掉两个开关，否则移动组件会逐帧跟我们的 AddActorWorldRotation 抢旋转（抖/转两下）：
	//   bOrientRotationToMovement（ALOLCharacter 默认 true）
	//   bUseControllerRotationYaw（ALOLCharacter 默认 false，但别人身上的默认值不一定）
	if (!bOrientRotationOverridden)
	{
		bSavedOrientRotationToMovement = MoveComp->bOrientRotationToMovement;
		bSavedUseControllerRotationYaw = Character->bUseControllerRotationYaw;
		bOrientRotationOverridden = true;
	}
	MoveComp->bOrientRotationToMovement = false;
	Character->bUseControllerRotationYaw = false;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(SpinYawTimer, this, &UGA_DeathHarvest::TickSpinYaw,
			/*InRate=*/0.f, /*bLoop=*/true);   // 0 = 每帧
		bSpinYawActive = true;
	}
}

void UGA_DeathHarvest::TickSpinYaw()
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	if (!Avatar || !Avatar->HasAuthority() || !AbilityData || !GetWorld())
	{
		return;
	}
	Avatar->AddActorWorldRotation(FRotator(0.f, AbilityData->SpinRate * GetWorld()->GetDeltaSeconds(), 0.f));
}

void UGA_DeathHarvest::ExecuteLocationCue(const FGameplayTag CueTag, const FVector& Location, const FVector& Normal) const
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo();
	if (!Avatar || !ASC || !Avatar->HasAuthority())
	{
		// 客户端不发 cue：服务器 ExecuteGameplayCue 会把参数多播到每一端各自跑一遍，
		// 本端再发一次就是两份（原方案担心的"双份"就是这个，门在这里就够了）。
		return;
	}

	FGameplayCueParameters Params;
	Params.Location = Location;
	Params.Normal = Normal;
	Params.Instigator = Avatar;
	Params.EffectCauser = Avatar;
	ASC->ExecuteGameplayCue(CueTag, Params);
}

void UGA_DeathHarvest::CancelAsFailed()
{
	// 退冷却：落点全废 / 目标丢失时这次技能等于没放出去（见 §7 取消矩阵）。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		FGameplayTagContainer CooldownTags;
		CooldownTags.AddTag(LOLGameplayTags::State_Cooldown_DeathHarvest);
		ASC->RemoveActiveEffectsWithGrantedTags(CooldownTags);
	}

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo,
		/*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
}

USkeletalMeshComponent* UGA_DeathHarvest::GetAvatarMesh() const
{
	if (const ACharacter* Character = Cast<ACharacter>(GetAvatarActorFromActorInfo()))
	{
		return Character->GetMesh();
	}
	return nullptr;
}

// =============================================================================
// 收尾
// =============================================================================

void UGA_DeathHarvest::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	AActor* Avatar = GetAvatarActorFromActorInfo();
	const bool bServer = Avatar && Avatar->HasAuthority();

	if (bServer)
	{
		// ⚠️ 这里是"被打断/施法者死亡"唯一能走到的地方（蒙太奇被硬停时后面的 notify 一个都不会来），
		// 所以每一样都必须在【这里】收干净。两个函数都是幂等的。
		StopSpin();
		ApplyMovementLock(false);

		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().ClearTimer(SpinYawTimer);
		}
		bSpinYawActive = false;

		if (bCastingTagAdded)
		{
			if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
			{
				ASC->RemoveLooseGameplayTag(LOLGameplayTags::State_DeathHarvest_Casting);
			}
			bCastingTagAdded = false;
		}

		// 循环 cue（GC.DeathHarvest.Spin）不用管：它是 K2_AddGameplayCueWithParams 挂上来的，
		// Super::EndAbility 会按 bRemoveOnAbilityEnd 自动摘掉。
	}

	LockedTarget.Reset();
	PortalLocation = FVector::ZeroVector;

	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}
```

> 用到 `UGameplayStatics` 的两处记得 include `<Kismet/GameplayStatics.h>`，
> `UHeroCombatAttributeSet` 那处 include `"GAS/HeroCombatAttributeSet.h"`，`USoundBase` include `"Sound/SoundBase.h"`。

### 5.4 `Public/GAS/GE_DeathHarvestCooldown.h` / `.cpp`（新增，照 `GE_StealthCooldown` 抄）

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GE_DeathHarvestCooldown.generated.h"

/** 大招冷却：持续型 GE，时长由 Data.Cooldown(SetByCaller) 决定，期间持有 State.Cooldown.DeathHarvest。 */
UCLASS()
class LOL_API UGE_DeathHarvestCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGE_DeathHarvestCooldown(const FObjectInitializer& ObjectInitializer);
};
```

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GE_DeathHarvestCooldown.h"
#include "GAS/LOLGameplayTags.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

UGE_DeathHarvestCooldown::UGE_DeathHarvestCooldown(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;

	// 时长用 SetByCaller，实际值由 UMyGameplayAbility::ApplyCooldown 填入（CooldownDuration，
	// 已经按技能急速换算过）。
	FSetByCallerFloat CallerMagnitude;
	CallerMagnitude.DataTag = LOLGameplayTags::Data_Cooldown;
	DurationMagnitude = FGameplayEffectModifierMagnitude(CallerMagnitude);

	// ⚠️ 冷却标签必须走 UTargetTagsGameplayEffectComponent：GetGrantedTags 只从 GEComponents 聚合，
	// 老的 InheritableOwnedTagsContainer 已经废弃（见 ue58-gas-granted-tags-component 那条）。
	UTargetTagsGameplayEffectComponent* TargetTags =
		ObjectInitializer.CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(this, TEXT("TargetTags"));
	GEComponents.Add(TargetTags);

	FInheritedTagContainer GrantedTags;
	GrantedTags.AddTag(LOLGameplayTags::State_Cooldown_DeathHarvest);
	TargetTags->SetAndApplyTargetTagChanges(GrantedTags);
}
```

### 5.5 `UExecCalc_Damage` 增补（三处，一个文件）

**① 捕获定义表加两条**（`ExecCalc_Damage.cpp` 的 `FDamageStatics`）：

```cpp
struct FDamageStatics
{
	// ... 现有的七条 ...
	DECLARE_ATTRIBUTE_CAPTUREDEF(Health);
	DECLARE_ATTRIBUTE_CAPTUREDEF(MaxHealth);

	FDamageStatics()
	{
		// ... 现有的 ...
		// 已损失生命加成从【目标】身上抓：目标是敌人，残血打得疼（GAS_DeathHarvest_Setup.md §5.6）。
		// 想改成"施法者残血加伤"，把这两行的 Target 改成 Source 就行，别的地方不用动。
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, Health, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UHeroCombatAttributeSet, MaxHealth, Target, false);
	}
};
```

**② 构造函数里登记**（漏掉这步的表现是"读出来恒为 0、不报错"，文件顶部那段注释已经写明）：

```cpp
	RelevantAttributesToCapture.Add(DamageStatics().HealthDef);
	RelevantAttributesToCapture.Add(DamageStatics().MaxHealthDef);
```

**③ 公式插在抗性减免之前**（`Damage` 算出来之后、`const FGameplayTagContainer& AssetTags` 那一段之前）：

```cpp
	// ①b 已损失生命加成（斩杀型）。满血 = 0 加成，残血最多 ×(1 + MissingHealthBonus)。
	//
	// 【乘法不加法】：和上面 AttackDamage × Multiplier + Flat 的公式不打架，
	// 同一个技能以后还能被别的系数接着叠。
	// WarnIfNotFound=false 和现有三个 SetByCaller 一致 —— 近战/匕首那两条老路径根本不填这个标签，
	// 填 true 的话它们每次命中都会刷一条警告。
	const float MissingHealthBonus = Spec.GetSetByCallerMagnitude(LOLGameplayTags::Data_MissingHealthBonus, /*WarnIfNotFound=*/false, 0.f);
	if (MissingHealthBonus > 0.f)
	{
		// Health 是【这次结算之前】的值：ExecCalc 跑的时候负向 modifier 还没落地。
		const float TargetHealth = Capture(DamageStatics().HealthDef);
		const float TargetMaxHealth = Capture(DamageStatics().MaxHealthDef);
		if (TargetMaxHealth > 0.f)
		{
			const float MissingPct = 1.f - FMath::Clamp(TargetHealth / TargetMaxHealth, 0.f, 1.f);
			Damage *= 1.f + MissingPct * MissingHealthBonus;
		}
	}
```

底部那条日志顺手带上加成，验伤害的时候一眼能看出来（§8 第 4 条就靠它）：

```cpp
	UE_LOG(LogTemp, Warning, TEXT("[Damage] 攻=%.1f 倍率=%.2f 固定=%.1f 斩杀=%.2f → 结算=%.1f 类型=%s 目标=%s"),
		AttackDamage, Multiplier, FlatDamage, MissingHealthBonus, Damage,
		bTrueDamage ? TEXT("真实") : (bMagic ? TEXT("魔法") : TEXT("物理")),
		*GetNameSafe(TargetASC->GetAvatarActor()));
```

> 加成的上限靠配置（`MissingHealthBonus` 是数值资产上的一个百分比），**不在代码里再夹一次** ——
> 两处夹迟早对不上，和文件里"Health 别在 ExecCalc 里再钳一次"是同一条纪律。

### 5.6 `Public/Animation/AnimNotify_SendGameplayEvent.h` / `.cpp`（新增）

```cpp
// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "GameplayTagContainer.h"
#include "AnimNotify_SendGameplayEvent.generated.h"

/**
 * 把蒙太奇上的一个时刻变成一次 GameplayEvent，推技能相位。
 *
 * 为什么自己写：GAS 插件只有 AnimNotify_GameplayCue 一个 notify，没有发 GameplayEvent 的。
 *
 * ⚠️ 通知会在【每一台播放这条蒙太奇的机器】上触发（服务器 + 本人 + 其他玩家），
 * 不是"只在服务器"。所以 bServerOnly 默认 true：客户端那份蒙太奇触发了也直接返回。
 * 相位推进只认服务器那一次 —— 这正是"服务器独占时间轴"想要的效果。
 */
UCLASS(meta = (DisplayName = "Send Gameplay Event"))
class LOL_API UAnimNotify_SendGameplayEvent : public UAnimNotify
{
	GENERATED_BODY()
public:
	explicit UAnimNotify_SendGameplayEvent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;

	/** 发哪个事件（技能那头用 UAbilityTask_WaitGameplayEvent 同名标签接）。 */
	UPROPERTY(EditAnywhere, Category="GameplayEvent", meta=(GameplayTagFilter="Event"))
	FGameplayTag EventTag;

	/**
	 * true = 只在服务器发（默认，相位推进用）。
	 * false = 各端都发（本地表现类的事件才需要，本项目的相位推进不要用它）。
	 */
	UPROPERTY(EditAnywhere, Category="GameplayEvent")
	bool bServerOnly = true;
};
```

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "Animation/AnimNotify_SendGameplayEvent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Actor.h"

UAnimNotify_SendGameplayEvent::UAnimNotify_SendGameplayEvent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UAnimNotify_SendGameplayEvent::Notify(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::Notify(MeshComp, Animation, EventReference);

	if (!MeshComp || !EventTag.IsValid())
	{
		// 标签没配 = 这个相位永远不会推进，动画上完全看不出来（只是技能卡在那儿）。
		UE_LOG(LogTemp, Warning, TEXT("[SendGameplayEvent] 通知没配 EventTag（动画 %s）→ 这次通知什么都不做"),
			*GetNameSafe(Animation));
		return;
	}

	AActor* Owner = MeshComp->GetOwner();
	if (!Owner)
	{
		return;
	}

	// 目的端过滤。客户端的蒙太奇也会触发本通知，那一次是空操作（技能实例在客户端只管播动画）。
	if (bServerOnly && !Owner->HasAuthority())
	{
		return;
	}

	FGameplayEventData Payload;
	Payload.EventTag = EventTag;
	Payload.Instigator = Owner;
	Payload.Target = Owner;
	Payload.EventMagnitude = 0.f;

	UAbilitySystemBlueprintLibrary::SendGameplayEventToActor(Owner, EventTag, Payload);
}

FString UAnimNotify_SendGameplayEvent::GetNotifyName_Implementation() const
{
	return EventTag.IsValid() ? FString::Printf(TEXT("SendEvent: %s"), *EventTag.ToString()) : FString(TEXT("SendEvent (未配标签)"));
}
```

### 5.7 `Public/Animation/AnimNotifyState_SocketParticle.h` / `.cpp`（新增）

结构照抄 `UAnimNotifyState_BladeTrail`，把 Niagara 换成 Cascade。

```cpp
// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AnimNotifyState_SocketParticle.generated.h"

class UParticleSystem;
class UParticleSystemComponent;
class USkeletalMeshComponent;

/** 挂哪只手（和 BladeTrail 的 EBladeTrailSide 同义，只是这里没有"拖尾"的语义）。 */
UENUM()
enum class ESocketParticleSide : uint8
{
	Left  UMETA(DisplayName = "Left (左手)"),
	Right UMETA(DisplayName = "Right (右手)"),
	Both  UMETA(DisplayName = "Both (双手)"),
};

/**
 * 在动画区间里往插槽上挂一个 Cascade 粒子（起 → 挂上，止 → 摘掉）。
 *
 * 为什么是 notify 而不是 GameplayCue：蒙太奇在【每一台机器】上都会播（见
 * GAS_DeathHarvest_Setup.md §1.2），notify 于是天然在每台机器各跑一次 ——
 * 正好是"每个人都该看到刀上挂特效"想要的，不需要任何复制代码。
 *
 * ⚠️ 这个对象是蒙太奇资产里的实例，**所有角色共用同一个**，所以生成出来的组件必须按
 * MeshComp 分开存（ActiveParticles），不能放成员变量里 —— 否则两个人同时转圈会互相顶掉。
 *
 * ⚠️ bOnlyOwnerSee 默认 false（敌人也该看到刀光）。这和 GC_Stealth 的 SwordParticle 正好相反：
 * 隐身时刀上的特效必须只有主人可见，转圈时人已经现身了。抄那一段的时候最容易抄错的就是这个值。
 */
UCLASS(meta = (DisplayName = "Socket Particle (Cascade)"))
class LOL_API UAnimNotifyState_SocketParticle : public UAnimNotifyState
{
	GENERATED_BODY()
public:
	virtual void NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, float TotalDuration, const FAnimNotifyEventReference& EventReference) override;
	virtual void NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation, const FAnimNotifyEventReference& EventReference) override;
	virtual FString GetNotifyName_Implementation() const override;

	/** 挂在插槽上的 Cascade 粒子（武器蓄能那套：P_Ultimate_Weapon_Charge）。 */
	UPROPERTY(EditAnywhere, Category="SocketParticle")
	TSoftObjectPtr<UParticleSystem> Particle;

	UPROPERTY(EditAnywhere, Category="SocketParticle")
	ESocketParticleSide Side = ESocketParticleSide::Both;

	/** Kallari 是双刀，插槽名和 GC_Stealth 里那对保持一致。 */
	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FName SocketLeft = TEXT("sword_base_l");

	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FName SocketRight = TEXT("sword_base_r");

	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FVector RelativeOffset = FVector::ZeroVector;

	/**
	 * 粒子相对插槽的朝向（EAttachLocation::SnapToTarget：直接落成组件的相对旋转，跟着刀走）。
	 * 左右分开配，因为右手插槽一般是左手的镜像 —— 右手反了就绕刀刃长轴转 180°。
	 * 和 GC_Stealth.h 里 SwordParticleRotation{Left,Right} 是同一个坑、同一套解法。
	 */
	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FRotator RelativeRotationLeft = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, Category="SocketParticle")
	FRotator RelativeRotationRight = FRotator::ZeroRotator;

	/** true = 只有本人看得见。转圈期间【必须】是 false（人已经现身了，敌人该看到刀光）。 */
	UPROPERTY(EditAnywhere, Category="SocketParticle")
	bool bOnlyOwnerSee = false;

	/**
	 * NotifyEnd 之后等多久强制销毁组件。
	 * 正常路径是 Deactivate() + bAutoDestroy 让粒子自然收掉；但 Cascade 的 emitter 要是设成
	 * 无限循环，auto destroy 永远等不到 —— 那样每转一次就泄漏一个常驻组件，所以留个兜底。
	 */
	UPROPERTY(EditAnywhere, Category="SocketParticle", meta=(ClampMin="0.1", Units="s"))
	float TeardownDelay = 1.f;

private:
	/** 一次 notify 在某条 mesh 上生成的所有组件（Both 时两个）。 */
	struct FActiveSocketParticles
	{
		TArray<TObjectPtr<UParticleSystemComponent>> Comps;
	};

	/** 按 mesh 分开存：notify 实例是所有角色共用的。键用弱引用，mesh 没了自动失效。 */
	TMap<TWeakObjectPtr<USkeletalMeshComponent>, FActiveSocketParticles> ActiveParticles;

	/** 生成一侧。插槽不存在时打日志并返回 nullptr（GetSocketLocation 会静默返回组件位置）。 */
	UParticleSystemComponent* SpawnOne(USkeletalMeshComponent* MeshComp, UParticleSystem* System, bool bRight) const;

	/** 停掉一个组件并挂上兜底销毁的定时器。 */
	static void Teardown(UParticleSystemComponent* Component, float DelaySeconds);
};
```

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "Animation/AnimNotifyState_SocketParticle.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "TimerManager.h"

namespace
{
	/** 插槽和骨骼都算数：GetSocketLocation 对骨骼名同样有效。 */
	bool HasSocket(const USkeletalMeshComponent* MeshComp, const FName& SocketName)
	{
		return MeshComp->DoesSocketExist(SocketName) || MeshComp->GetBoneIndex(SocketName) != INDEX_NONE;
	}
}

void UAnimNotifyState_SocketParticle::NotifyBegin(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	float TotalDuration, const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyBegin(MeshComp, Animation, TotalDuration, EventReference);

	if (!MeshComp)
	{
		return;
	}

	// 角色在 notify 期间被销毁时 NotifyEnd 不一定跑得到，顺手清掉失效的键（弱引用，不会解野指针）。
	for (auto It = ActiveParticles.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	// 专用服务器没有渲染，不生成。
	UWorld* World = MeshComp->GetWorld();
	if (!World || World->GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	UParticleSystem* System = Particle.LoadSynchronous();
	if (!System)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SocketParticle] Particle 没配（或软引用加载失败）→ %s 上不生成"),
			*GetNameSafe(MeshComp->GetSkeletalMeshAsset()));
		return;
	}

	FActiveSocketParticles& Entry = ActiveParticles.Add(MeshComp);
	if (Side == ESocketParticleSide::Left || Side == ESocketParticleSide::Both)
	{
		if (UParticleSystemComponent* Comp = SpawnOne(MeshComp, System, /*bRight=*/false))
		{
			Entry.Comps.Add(Comp);
		}
	}
	if (Side == ESocketParticleSide::Right || Side == ESocketParticleSide::Both)
	{
		if (UParticleSystemComponent* Comp = SpawnOne(MeshComp, System, /*bRight=*/true))
		{
			Entry.Comps.Add(Comp);
		}
	}

	if (Entry.Comps.Num() == 0)
	{
		ActiveParticles.Remove(MeshComp);
	}
}

UParticleSystemComponent* UAnimNotifyState_SocketParticle::SpawnOne(USkeletalMeshComponent* MeshComp, UParticleSystem* System, bool bRight) const
{
	const FName SocketName = bRight ? SocketRight : SocketLeft;
	if (!HasSocket(MeshComp, SocketName))
	{
		// 插槽不存在时 SpawnEmitterAttached 会静默把粒子挂在组件原点 —— 看不出是配置错的，必须当场报出来。
		UE_LOG(LogTemp, Warning, TEXT("[SocketParticle] %s 上找不到插槽 %s → 这一侧不生成"),
			*GetNameSafe(MeshComp->GetSkeletalMeshAsset()), *SocketName.ToString());
		return nullptr;
	}

	const FRotator Rotation = bRight ? RelativeRotationRight : RelativeRotationLeft;

	// SnapToTarget：Location/Rotation 落成【相对插槽】的变换（跟着刀走，不跟着角色转身）。
	// bAutoDestroy=false：生命周期由 NotifyEnd 的 Teardown 统一管，不给引擎留一个"自己决定什么时候死"的分支。
	UParticleSystemComponent* Comp = UGameplayStatics::SpawnEmitterAttached(
		System, MeshComp, SocketName, RelativeOffset, Rotation, FVector(1.f),
		EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false, EPSCPoolMethod::None, /*bAutoActivate=*/true);

	if (!Comp)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SocketParticle] SpawnEmitterAttached 返回空（系统无效，或被可扩展性预剔除挡掉）"));
		return nullptr;
	}

	// ★ 转圈时是 false：人已经现身了，敌人该看到刀上的光。
	// （SCS_Stealth 的 SwordParticle 是 true —— 那一段抄过来时最容易抄错这个值。）
	Comp->bOnlyOwnerSee = bOnlyOwnerSee;

	return Comp;
}

void UAnimNotifyState_SocketParticle::NotifyEnd(USkeletalMeshComponent* MeshComp, UAnimSequenceBase* Animation,
	const FAnimNotifyEventReference& EventReference)
{
	Super::NotifyEnd(MeshComp, Animation, EventReference);

	if (!MeshComp)
	{
		return;
	}

	FActiveSocketParticles Entry;
	if (!ActiveParticles.RemoveAndCopyValue(MeshComp, Entry))
	{
		return;
	}

	for (UParticleSystemComponent* Comp : Entry.Comps)
	{
		Teardown(Comp, TeardownDelay);
	}
}

void UAnimNotifyState_SocketParticle::Teardown(UParticleSystemComponent* Component, float DelaySeconds)
{
	if (!Component)
	{
		return;
	}

	// 先停发射：已经生成的那部分按粒子自己的寿命自然收掉，比直接 Destroy 好看。
	Component->Deactivate();
	// bAutoDestroy 是个公开位域，不是函数 —— Cascade 没有 SetAutoDestroy（见 §11）。
	Component->bAutoDestroy = true;

	// 兜底：emitter 要是设成无限循环，auto destroy 永远等不到 —— 每转一次泄漏一个常驻组件。
	if (UWorld* World = Component->GetWorld())
	{
		TWeakObjectPtr<UParticleSystemComponent> WeakComponent(Component);
		FTimerHandle TeardownTimer;
		World->GetTimerManager().SetTimer(TeardownTimer, FTimerDelegate::CreateWeakLambda(Component, [WeakComponent]()
		{
			if (UParticleSystemComponent* StillAlive = WeakComponent.Get())
			{
				StillAlive->DestroyComponent();
			}
		}), DelaySeconds, /*bLoop=*/false);
	}
}

FString UAnimNotifyState_SocketParticle::GetNotifyName_Implementation() const
{
	switch (Side)
	{
	case ESocketParticleSide::Left:  return TEXT("SocketParticle (左手)");
	case ESocketParticleSide::Both:  return TEXT("SocketParticle (双手)");
	default:                         return TEXT("SocketParticle (右手)");
	}
}
```

### 5.8 `Public/GAS/GC_DeathHarvestCast.h` / `.cpp`（Actor 版，起手）

```cpp
// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_DeathHarvestCast.generated.h"

class UCameraModifier;
class UMaterialInterface;
class UParticleSystem;
class UParticleSystemComponent;
class USoundBase;

/**
 * 起手表现：挂在施法者身上的循环粒子 + 一次性音效，技能结束（或被打断）自动收掉。
 *
 * 直接用插件自带的 UAnimNotifyState_GameplayCueState 也行（纯自身、本地执行），
 * 但起手还要挂大招镜头（见 GAS_DeathHarvest_Setup.md §9，第一版不做）——那个必须由 cue 的
 * OnActive 去推，所以这里统一走 cue，留个 ScreenModifierClass 的位子。
 *
 * 蓝图子类必须命名为 GC_DeathHarvest_Cast。
 */
UCLASS()
class LOL_API AGC_DeathHarvestCast : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()
public:
	AGC_DeathHarvestCast();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 起手期间挂在身上转的循环粒子（Cascade）。Paragon: P_Ultimate_Warmup_Limbs。 */
	UPROPERTY(EditDefaultsOnly, Category="Cast") TObjectPtr<UParticleSystem> LoopParticle;

	/** 循环粒子挂哪个插槽/骨骼（留空 = 网格原点）。 */
	UPROPERTY(EditDefaultsOnly, Category="Cast") FName AttachSocket = NAME_None;

	/** 起手的一次性音效（可空）。 */
	UPROPERTY(EditDefaultsOnly, Category="Cast") TObjectPtr<USoundBase> CastSound;

	// --- 大招镜头（第一版不做，位子先留着）---
	// 做法照 UMyStealthCameraModifier + AGC_Stealth::ApplyLocalScreen（GC_Stealth.cpp:524-563）：
	// 本地相机的 PlayerCameraManager 上 AddNewCameraModifier，材质在 OnActive 里推给实例
	// （不建子类就永远是空的，所以材质要放 cue 上）。
	UPROPERTY(EditDefaultsOnly, Category="Cast|Screen") TSubclassOf<UCameraModifier> ScreenModifierClass;
	UPROPERTY(EditDefaultsOnly, Category="Cast|Screen") TObjectPtr<UMaterialInterface> ScreenMaterial;

private:
	UPROPERTY(Transient) TObjectPtr<UParticleSystemComponent> LoopComp;
	void DestroyLoop();
};
```

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestCast.h"
#include "GAS/LOLGameplayTags.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"

AGC_DeathHarvestCast::AGC_DeathHarvestCast()
{
	// CDO 构造阶段字符串查标签拿不到（返回 None），必须直接用原生标签对象。
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Cast;

	// 技能结束（正常或被取消）→ 服务器摘 cue → 这个 actor 自己销毁，不用手写清理。
	bAutoDestroyOnRemove = true;

	// 起手特效挂在人身上：角色移动/转身要跟着走。
	bAutoAttachToOwner = true;
}

bool AGC_DeathHarvestCast::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return false;
	}

	// 预测重放 / 技能被刷新时 OnActive 可能被调第二次，先清旧的，避免叠两层。
	DestroyLoop();

	USceneComponent* AttachTo = nullptr;
	if (const ACharacter* Character = Cast<ACharacter>(MyTarget))
	{
		AttachTo = Character->GetMesh() ? static_cast<USceneComponent*>(Character->GetMesh()) : nullptr;
	}
	if (!AttachTo)
	{
		AttachTo = MyTarget->GetRootComponent();
	}

	if (LoopParticle && AttachTo)
	{
		LoopComp = UGameplayStatics::SpawnEmitterAttached(
			LoopParticle, AttachTo, AttachSocket, FVector::ZeroVector, FRotator::ZeroRotator, FVector(1.f),
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false);
	}

	if (CastSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, CastSound, MyTarget->GetActorLocation());
	}

	return LoopComp != nullptr || CastSound != nullptr;
}

bool AGC_DeathHarvestCast::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	DestroyLoop();
	return true;
}

void AGC_DeathHarvestCast::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 兜底：角色被销毁 / 切关卡时 OnRemove 不一定走到。
	DestroyLoop();
	Super::EndPlay(EndPlayReason);
}

void AGC_DeathHarvestCast::DestroyLoop()
{
	if (LoopComp)
	{
		LoopComp->Deactivate();
		// 位域，不是函数（见 §11）。
		LoopComp->bAutoDestroy = true;
		LoopComp->DestroyComponent();
		LoopComp = nullptr;
	}
}
```

### 5.9 `Public/GAS/GC_DeathHarvestPortal.h` / `.cpp`（Actor 版，传送门）

```cpp
// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_DeathHarvestPortal.generated.h"

class UParticleSystem;
class UParticleSystemComponent;
class USoundBase;

/**
 * 传送门：开在【服务器算出来的落点】上，技能推进到下一个相位时关门。
 *
 * ⚠️ 这个类必须自己 SetActorLocation —— Actor 版 cue 的生成位置取的是 TargetActor 的坐标
 * （GameplayCueManager.cpp:528 用的是 TargetActor->GetActorLocation()），不是 Parameters.Location。
 * 不写这一行的表现是"传送门开在自己脚下"，而且不报任何错。
 *
 * 蓝图子类必须命名为 GC_DeathHarvest_Portal。
 */
UCLASS()
class LOL_API AGC_DeathHarvestPortal : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()
public:
	AGC_DeathHarvestPortal();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 门开着期间的门体循环粒子（Cascade）。Paragon: P_Ult_Teleport_Enter / P_Ultimate_Portal。 */
	UPROPERTY(EditDefaultsOnly, Category="Portal") TObjectPtr<UParticleSystem> OpenParticle;

	/** 关门的一次性粒子。由 OnRemove 以【独立发射器】生成，所以它比本 actor 活得久。 */
	UPROPERTY(EditDefaultsOnly, Category="Portal") TObjectPtr<UParticleSystem> CloseParticle;

	UPROPERTY(EditDefaultsOnly, Category="Portal") TObjectPtr<USoundBase> OpenSound;
	UPROPERTY(EditDefaultsOnly, Category="Portal") TObjectPtr<USoundBase> CloseSound;

	/** 门体的相对偏移（想把门压到地面上时用）。 */
	UPROPERTY(EditDefaultsOnly, Category="Portal") FVector RelativeOffset = FVector::ZeroVector;

private:
	UPROPERTY(Transient) TObjectPtr<UParticleSystemComponent> LoopComp;
	void DestroyLoop();
};
```

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestPortal.h"
#include "GAS/LOLGameplayTags.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"

AGC_DeathHarvestPortal::AGC_DeathHarvestPortal()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Portal;

	// 技能结束时服务器 K2_RemoveGameplayCue → 本 actor 销毁。关门的表现已经在这一刻用
	// 独立发射器放出去了（见 OnRemove），所以这里可以立刻销毁。
	bAutoDestroyOnRemove = true;

	// 门开在世界坐标上，不跟着人走。
	bAutoAttachToOwner = false;
}

bool AGC_DeathHarvestPortal::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return false;
	}

	// 先清旧的（cue actor 会被回收复用，OnActive 可能不是第一次）。
	DestroyLoop();

	// ★ 这一行是这个类存在的理由：把门挪到服务器算出来的落点。
	//   参数由 UGA_DeathHarvest::ExecuteLocationCue 填（Location = 落点，Normal = 落点到目标的朝向）。
	SetActorLocation(Parameters.Location + RelativeOffset);
	if (!Parameters.Normal.IsNearlyZero())
	{
		SetActorRotation(Parameters.Normal.Rotation());
	}

	if (OpenParticle)
	{
		LoopComp = UGameplayStatics::SpawnEmitterAttached(
			OpenParticle, GetRootComponent(), NAME_None, FVector::ZeroVector, FRotator::ZeroRotator, FVector(1.f),
			EAttachLocation::KeepRelativeOffset, /*bAutoDestroy=*/false);
	}

	if (OpenSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, OpenSound, Parameters.Location);
	}

	return LoopComp != nullptr || OpenSound != nullptr;
}

bool AGC_DeathHarvestPortal::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	DestroyLoop();

	// 关门的一次性粒子。用独立发射器（不是挂在自己身上的组件）：
	// 本 actor 马上就要销毁了，挂在它身上的粒子会跟着一起没。
	if (CloseParticle)
	{
		UGameplayStatics::SpawnEmitterAtLocation(GetWorld(), CloseParticle, GetActorTransform());
	}
	if (CloseSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, CloseSound, GetActorLocation());
	}

	return true;
}

void AGC_DeathHarvestPortal::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyLoop();
	Super::EndPlay(EndPlayReason);
}

void AGC_DeathHarvestPortal::DestroyLoop()
{
	if (LoopComp)
	{
		LoopComp->Deactivate();
		// 位域，不是函数（见 §11）。
		LoopComp->bAutoDestroy = true;
		LoopComp->DestroyComponent();
		LoopComp = nullptr;
	}
}
```

### 5.10 `Public/GAS/GC_DeathHarvestSpin.h` / `.cpp`（Actor 版，转圈）

```cpp
// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"
#include "GC_DeathHarvestSpin.generated.h"

class UParticleSystem;
class UParticleSystemComponent;
class USoundBase;

/**
 * 转圈期间挂在身上的循环表现。
 *
 * 生命周期由 UGA_DeathHarvest 用 K2_AddGameplayCueWithParams(..., bRemoveOnAbilityEnd=true) 挂上，
 * 技能一结束（正常结束或被取消）引擎自动摘 —— 所以这里不需要任何"技能结束时清一下"的代码，
 * OnRemove / EndPlay 只需要管自己生成的那个组件。
 *
 * 蓝图子类必须命名为 GC_DeathHarvest_Spin。
 */
UCLASS()
class LOL_API AGC_DeathHarvestSpin : public AGameplayCueNotify_Actor
{
	GENERATED_BODY()
public:
	AGC_DeathHarvestSpin();

	virtual bool OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 转圈期间绕身的循环粒子（Cascade）。Paragon: P_Ultimate_Rush_Wind。 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") TObjectPtr<UParticleSystem> LoopParticle;

	/** 循环粒子挂哪个插槽/骨骼（留空 = 网格原点，跟着角色转身转）。 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") FName AttachSocket = NAME_None;

	/** 相对挂点的偏移 / 缩放。 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") FVector RelativeOffset = FVector::ZeroVector;
	UPROPERTY(EditDefaultsOnly, Category="Spin") FVector Scale = FVector(1.f);

	/** 起转的龙吼/音效（可空）。 */
	UPROPERTY(EditDefaultsOnly, Category="Spin") TObjectPtr<USoundBase> SpinSound;

private:
	UPROPERTY(Transient) TObjectPtr<UParticleSystemComponent> LoopComp;
	void DestroyLoop();
};
```

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestSpin.h"
#include "GAS/LOLGameplayTags.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Sound/SoundBase.h"

AGC_DeathHarvestSpin::AGC_DeathHarvestSpin()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Spin;
	bAutoDestroyOnRemove = true;
	bAutoAttachToOwner = true;
}

bool AGC_DeathHarvestSpin::OnActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	if (!IsValid(MyTarget))
	{
		return false;
	}

	DestroyLoop();

	USceneComponent* AttachTo = nullptr;
	if (const ACharacter* Character = Cast<ACharacter>(MyTarget))
	{
		AttachTo = Character->GetMesh() ? static_cast<USceneComponent*>(Character->GetMesh()) : nullptr;
	}
	if (!AttachTo)
	{
		AttachTo = MyTarget->GetRootComponent();
	}

	if (LoopParticle && AttachTo)
	{
		LoopComp = UGameplayStatics::SpawnEmitterAttached(
			LoopParticle, AttachTo, AttachSocket, RelativeOffset, FRotator::ZeroRotator, Scale,
			EAttachLocation::SnapToTarget, /*bAutoDestroy=*/false);
	}

	if (SpinSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, SpinSound, MyTarget->GetActorLocation());
	}

	return LoopComp != nullptr;
}

bool AGC_DeathHarvestSpin::OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters)
{
	DestroyLoop();
	return true;
}

void AGC_DeathHarvestSpin::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	DestroyLoop();
	Super::EndPlay(EndPlayReason);
}

void AGC_DeathHarvestSpin::DestroyLoop()
{
	if (LoopComp)
	{
		LoopComp->Deactivate();
		// 位域，不是函数（见 §11）。
		LoopComp->bAutoDestroy = true;
		LoopComp->DestroyComponent();
		LoopComp = nullptr;
	}
}
```

### 5.11 `Public/GAS/GC_DeathHarvestBurst.h` / `.cpp`（Static 版：基类 + 三个空壳）

```cpp
// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"
#include "GC_DeathHarvestBurst.generated.h"

class UParticleSystem;
class USoundBase;

/**
 * 三个一次性 burst（现身 / 命中 / 原地消失）共用的实现：在 Parameters.Location 放一个粒子 + 一声音效，
 * 朝向用 Parameters.Normal（命中点贴着墙面/地面朝外喷）。
 *
 * 为什么三个类都留着而不是共用一个：cue 的标签是按【类名】推导的
 * （LOLGameplayTags.h:70-72），同一个 C++ 类派出来的三个蓝图会被引擎重新推导成同一个无效标签。
 * 所以子类必须存在，只是它们每个只有四行（构造函数里设自己的 GameplayCueTag）。
 *
 * 这些 cue 都由 UGA_DeathHarvest 在服务器 ExecuteGameplayCue → 多播到各客户端各自播一次
 * （和 GC_ThrowDaggerHit 同一条路：命中结算只在服务端跑，就地 Spawn 的话粒子只有主机看得到）。
 */
UCLASS(Abstract)
class LOL_API UGC_DeathHarvestBurst : public UGameplayCueNotify_Static
{
	GENERATED_BODY()
public:
	virtual bool OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

	/** 一次性粒子（Cascade）。 */
	UPROPERTY(EditDefaultsOnly, Category="Burst")
	TSoftObjectPtr<UParticleSystem> Particle;

	/** 一次性音效（可空）。用软引用：默认值只是路径，编辑器里随时换。 */
	UPROPERTY(EditDefaultsOnly, Category="Burst")
	TSoftObjectPtr<USoundBase> Sound;

	/** 粒子相对命中点抬高多少（有些特效原点在脚底）。 */
	UPROPERTY(EditDefaultsOnly, Category="Burst")
	FVector LocationOffset = FVector::ZeroVector;
};

/** 现身：她从门里出来那一下（落点）。BP: GC_DeathHarvest_Appear。 */
UCLASS()
class LOL_API UGC_DeathHarvestAppear : public UGC_DeathHarvestBurst
{
	GENERATED_BODY()
public:
	UGC_DeathHarvestAppear();
};

/** 每跳命中：范围伤害打到人身上（命中点 + 法线）。BP: GC_DeathHarvest_Hit。 */
UCLASS()
class LOL_API UGC_DeathHarvestHit : public UGC_DeathHarvestBurst
{
	GENERATED_BODY()
public:
	UGC_DeathHarvestHit();
};

/** 原地消失：开门的同一时刻她在老位置消失那一下。BP: GC_DeathHarvest_TeleportOut。 */
UCLASS()
class LOL_API UGC_DeathHarvestTeleportOut : public UGC_DeathHarvestBurst
{
	GENERATED_BODY()
public:
	UGC_DeathHarvestTeleportOut();
};
```

```cpp
// Fill out your copyright notice in the Description page of Project Settings.

#include "GAS/GC_DeathHarvestBurst.h"
#include "GAS/LOLGameplayTags.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Particles/ParticleSystem.h"
#include "Sound/SoundBase.h"

bool UGC_DeathHarvestBurst::OnExecute_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	// Location / Normal 由 UGA_DeathHarvest::ExecuteLocationCue 填好：位置 + 朝向。
	// 用 Normal 决定朝向，粒子才会贴着墙面/地面朝外喷，而不是永远世界朝前。
	const FRotator Rotation = Parameters.Normal.IsNearlyZero() ? FRotator::ZeroRotator : Parameters.Normal.Rotation();
	const FVector Location = Parameters.Location + LocationOffset;

	// 音效先取：粒子没配但音效配了，这一下也不算"没有表现"。
	UParticleSystem* FX = Particle.LoadSynchronous();
	USoundBase* SoundAsset = Sound.LoadSynchronous();

	if (FX)
	{
		UGameplayStatics::SpawnEmitterAtLocation(World, FX, FTransform(Rotation, Location));
	}
	if (SoundAsset)
	{
		UGameplayStatics::PlaySoundAtLocation(World, SoundAsset, Location);
	}

	// 两个都留空 = 这次没有表现，返回 false（不是错误，只是没东西可播）。
	return FX != nullptr || SoundAsset != nullptr;
}

// --- 三个子类：只有标签不同 ---
// CDO 构造阶段字符串查标签拿不到（返回 None），所以必须直接用原生标签对象。

UGC_DeathHarvestAppear::UGC_DeathHarvestAppear()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Appear;
}

UGC_DeathHarvestHit::UGC_DeathHarvestHit()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_Hit;
}

UGC_DeathHarvestTeleportOut::UGC_DeathHarvestTeleportOut()
{
	GameplayCueTag = LOLGameplayTags::GameplayCue_DeathHarvest_TeleportOut;
}
```

---

## 6. 编辑器侧清单（代码编过之后）

1. **`BP_GA_DeathHarvest`**：父类 `GA_DeathHarvest`；`AbilityData` 指向新建的 `DA_DeathHarvest`；
   `Montage` 填大招蒙太奇；`DamageEffect` 填 `GE_Damage`（和近战/匕首共用同一个）。
2. **`DA_DeathHarvest`**：按 §5.2 的字段填数值。
   - `bRootMotionSpin` 先打开 `Ability_Ultimate` 看 root motion 有没有 yaw 再决定（§9 待确认项）。
3. **六个 cue 蓝图**（父类见括号），命名一个字都不能错：
   `GC_DeathHarvest_Cast`（`GC_DeathHarvestCast`）、`GC_DeathHarvest_Portal`（`GC_DeathHarvestPortal`）、
   `GC_DeathHarvest_Spin`（`GC_DeathHarvestSpin`）、`GC_DeathHarvest_Appear` / `_Hit` / `_TeleportOut`
   （分别对应 `GC_DeathHarvestBurst.h` 里的三个子类）。粒子/音效在 BP 里填（C++ 里刻意没写死资产路径）。
   - `GC_DeathHarvest_Spin`：**`LoopParticle` 到现在都还是空的**（资产里查过，一个资源引用都没存），
     要填 `P_Ultimate_Rush_Wind`，并确认左/右插槽名（默认 `sword_base_l` / `sword_base_r`）
     真的在这个角色的骨架上 —— 名字不对那一侧不生成，日志里有 warning。
   - `GC_DeathHarvest_Appear` / `_Hit` / `_TeleportOut`：父类上新增了 `CameraShake`（可空）——
     三个都能配，只有配了的才震，而且只在**施法者本机**震。想让打击感起来，至少给
     `GC_DeathHarvest_Hit` 配上 `BP_CameraShake_Hit_Player`（`Content/Variant_Combat/Blueprints/`
     里已经有这个资产，现在项目里没有任何代码引用它）。
4. **`IA_R`** → `Ability.Slot.R`，加进 `IC_Ability` 的映射。
5. **`AS_HeroKit`** 的 `GrantAbility` 加一行：`SlotTag = Ability.Slot.R`，`Ability = BP_GA_DeathHarvest`。
6. **蒙太奇**上放通知：
   - 起手段：`AnimNotifyState_SocketParticle`（双手，`P_Ultimate_Weapon_Charge`，`bOnlyOwnerSee=false`）。
   - 开门帧：`AnimNotify_SendGameplayEvent` → `Event.DeathHarvest.Portal`。
   - 现身帧：`AnimNotify_SendGameplayEvent` → `Event.DeathHarvest.Teleport`。
   - 转圈段第一帧：`AnimNotify_SendGameplayEvent` → `Event.DeathHarvest.SpinStart`；循环段结束：`...SpinEnd`。
   - 收招段：不需要通知（蒙太奇播完就收）。
7. **`GE_Damage`** 不用改（三个 SetByCaller 都是可选填的）。

---

## 7. 取消 / 打断矩阵

| 情况 | 处理 | 代码位置 |
|---|---|---|
| 锁定阶段无目标 | 不激活（`CommitAbility` **之前**就返回，所以不进冷却） | `ActivateAbility` 的 P0 段 |
| 落点全被挡 | `CancelAsFailed()`：退冷却 + `EndAbility(cancelled)` | `OnPortalEvent` → `ComputeDestination` 返回 false |
| 目标在开门前死亡/失效 | 同上；`TWeakObjectPtr` + 每个相位边界重校验 | `OnPortalEvent` / `OnTeleportEvent` 开头 |
| 目标在转圈中死亡 | **不中断**，转圈是范围伤害，继续打完 | `OnSpinPulse_Server` 不查目标 |
| 施法者再次按 R | 冷却 GE + `ActivationBlockedTags = State.DeathHarvest.Casting` 双保险 | 构造函数 |
| 施法者中途被打断/死亡 | `EndAbility` 里统一收：停伤害跳、恢复移动/旋转、摘 Casting 标签、清 timer；循环 cue 由引擎按 `bRemoveOnAbilityEnd` 自动摘 | `EndAbility` |

### ✅ 架构缺口已填（走的是 B）

> 这一节原来写的是"项目里没有 `State.Dead`、也没有任何 CC tag，'施法者死亡时取消大招'没人触发"，
> 给了 A / B 两条路选。**已按 B 实现完毕，构建绿**。下面是落地后的实际情况，别照着旧文字再实现一遍。

现在有的东西：

| 标签 | 谁授予 | 挡谁 |
|---|---|---|
| `State.Dead` | `UGE_Death` | 所有技能（含普攻）+ 移动 / 跳跃 |
| `State.Stunned` | `UGE_Stun` | 所有技能（含普攻） |
| `State.Silenced` | `UGE_Silence` | **只有法术**，普攻刻意不挡（LoL 语义） |

**挡人的声明分两层，不要写到别处去**：

- **基类默认** —— `UMyGameplayAbility` 的构造函数往 `ActivationBlockedTags` 里加 `State.Dead` + `State.Stunned`。
  所有技能自动继承，上表前两行不用每个技能各写一遍。
- **技能按需** —— `State.Silenced` 各自加在五个法术技能的构造函数里
  （`GA_Stealth` / `GA_Block` / `GA_Flash` / `UThrowDaggerAbility` / `GA_DeathHarvest`）。
  **`UGA_ThreeHitPassive` 刻意不加** —— 加错的表现是"被沉默之后连平 A 都打不出来"。

判定读的是 ASC 的 `OwnedGameplayTags`，所以 GE 授予的标签和 `AddLooseGameplayTag` 都算
（`GameplayAbility.cpp`：`CheckForBlocked(AbilitySystemComponent.GetOwnedGameplayTags(), ActivationBlockedTags)`）。
普攻虽然走的是 `AHeroCombatCharacter::RouteBasicAttackInput` 里直接 `TryActivateAbility`，
一样会过 `CanActivateAbility → CheckForBlocked`，所以死亡 / 眩晕连平 A 一起挡掉，**输入层不用再挡一次**。

> 用基类默认值有一个会静默失效的点：**如果某个 `BP_GA_*` 序列化过 `ActivationBlockedTags`，BP 的值会盖掉 C++ 的。**
> 已经验过：项目里六个 `BP_GA_*` 一个都没序列化这个属性，所以基类默认全部生效。
> 以后新加技能时如果发现"死亡挡不住它"，先查这个。

**打断（"把正在放的掐掉"）走的是另一条路 —— GE 组件，不是 `ActivationBlockedTags`**：

- `UGE_Death` / `UGE_Stun` 上挂了 `UCancelAbilityTagsGameplayEffectComponent`，两个容器都留空
  = `CancelAbilities(nullptr, nullptr)` = 取消全部已激活技能。上面那张表里
  "施法者中途被打断/死亡"那一行的触发者就是它。
  ⚠️ 该组件第一行是 `if (!ActiveGEContainer.OwnerIsNetAuthority) return;` —— **只在权威端真的取消**，
  所以挂这两个 GE 的地方必须是服务端。
- `UGE_Silence` **故意没有** Cancel 组件：沉默是"不让开新的"，不是"打断正在放的"。
  给它也加上的表现是"被沉默瞬间大招自己断了"，和 LoL 不一样。

**死亡链路（一条，没有第二份计时）**：

```
Health 归零 → UHeroCombatAttributeSet::PostGameplayEffectExecute（仅权威端广播）
→ OnOutOfHealth → AHeroCombatCharacter::HandleOutOfHealth 挂 UGE_Death
→ 各端收到 State.Dead 标签事件 → EnterDeathState（布娃娃 + 停移动 + 关胶囊碰撞）
→ UGE_Death 到期摘标签 → ExitDeathState（还原 + 回 PlayerStart + 回满血）
```

复活时长只有 `AHeroCombatCharacter::RespawnDelay` 一个来源，触发点就是 GE 到期。
**不要再另开 `FTimerHandle` 计时**，两份计时迟早对不上。

B 落地前这里其实有两个坑，顺带记一下：

- **全项目原本没有 `PostGameplayEffectExecute`**。血归零只被 `HeroCombatAttributeSet.cpp` 的
  `ClampAttribute` 钳到 0，没有任何人知道这件事发生过 —— 所以 A 那条"小改"其实也得先加这个钩子，
  否则 `State.Dead` 永远没人挂。这是当时推荐 A 时低估的地方。
- **`UGA_Stealth::EndAbility` 原来不摘 `State.Stealth` 的 GE**（只靠"标签归零"清，而外部取消不走那条路）。
  不修的话死亡取消能力 → 能力没了、隐身还在 → **隐身死、隐身复活**。已修。
  和 `UThrowDaggerAbility::EndAbility → ExitAimingState` 是同一条纪律，属于 `GA_Stealth` 原来漏了。
- **`UGE_Death` 还要剥掉"已经挂上、没人再管"的状态 GE**（`State.Blocking` / `State.BlockImmune` /
  `State.EmpoweredAttack`）—— 授予它们的技能早就结束了（`GA_Block` 是薄能力），取消组件管不到。
  ⚠️ 用的是 `URemoveOtherGameplayEffectComponent`，**`GEComponents` 的数组顺序有讲究：
  Cancel 必须排在 Remove 前面**，反过来 `State.Stealth` 被摘会触发标签归零回调，给死人挂一份强化普攻。

**B 新增 / 改动的文件**（不在 §4 那张表里，那是大招的文件清单）：

- 新增：`Public|Private/GAS/GE_Death.*`、`GE_Stun.*`、`GE_Silence.*`
- 改动：`LOLGameplayTags.h/.cpp`、`HeroCombatAttributeSet.h/.cpp`、`HeroCombatCharacter.h/.cpp`、
  `MyGameplayAbility.h/.cpp`、`LOLCharacter.cpp`、五个法术技能的 `.cpp`

**死亡这套不需要任何编辑器接线** —— `AHeroCombatCharacter::DeathEffect` 默认就是 `UGE_Death`，
没有 BP 要建、没有资产要填。复活点走 `AGameModeBase::FindPlayerStart`。

`GE_Stun` / `GE_Silence` 目前**没有任何东西会施加它们**，是给以后的技能备的基础设施。

---

## 8. 验证清单

1. **PIE 双端（两个 player）**：
   - 本人：按下 R **看得到自己的动画**（看不到 → 有人把 `NetExecutionPolicy` 改回 `ServerOnly` 了）。
   - 双方：传送门和现身特效的**位置一致**（不一致 → cue 参数没带 Location，或者 Portal cue 忘了 `SetActorLocation`）。
2. **目标站墙边**：落点退让逻辑生效，不会卡进墙里；全废时技能取消且**冷却被退掉**。
3. **打断测试（§7 的 B 已落地，这条现在能真跑）**：转圈中途把施法者打死 →
   cue 全摘、蒙太奇停、移动和旋转恢复正常（不是卡着不能动）、布娃娃倒下去、
   `RespawnDelay` 秒后回 PlayerStart 满血、下一个 R 能正常起手。
   这条最能验"健壮"，一次跑完能同时覆盖死亡链路和大招的取消分支。
   PIE 单人不满足条件（没人能杀你）—— 用 **Play → Number of Players = 2**，
   或者临时把 `GHeroStatTable` 里 1 级 Health 调成 1，让对面平 A 一下就够。
   顺带看日志三行：`[Death] ... 生命归零` → `[Death] ... 挂上死亡状态` → `[Death] ... 复活`。
   **只有第一行、没有第二行** = `HandleOutOfHealth` 没被调到（`InitializeAbilityActorInfo` 的订阅没挂上）。
   **三行都没有** = 伤害根本没结算到 Health。
4. **死亡清状态**：隐身途中被打死 → 复活时**不是隐身的**（验 `GA_Stealth::EndAbility` 那个修复）；
   举盾途中被打死 → 复活时身上没有 `State.Blocking`（验 `GE_Death` 的 Remove 组件）。
5. **伤害**：同一个目标满血打一次、残血打一次，看日志里两次的 `斩杀=` 和 `结算=`，
   差值符合 `1 + MissingPct × MissingBonus`。
6. **武器粒子**：转圈起止和动画对齐；**敌人视角能看见刀上特效**（看不见就是把 `bOnlyOwnerSee` 抄成 `true` 了）。
7. **断线/重连**（可选）：客户端重连后处于转圈相位时不会卡在 MOVE_None。
8. **起手打击感落点 + 命中顿帧**（PIE 两端各看一次）：
   - **施法者自己那台看到的和主机看到的是同一个位置**：她现身应该**就在门前**（目标身后 140cm），
     不是在她消失的地方转（§6.8）。日志对一眼：客户端 `客户端本端传送 @ (x, y, z) 朝向 N` 和主机
     `P3 现身 @ (x, y, z) 朝向 N` 应该坐标重合（差半高以内）、朝向同值。
   - **位置对但朝向不对**（人站在门前，脸却朝着消失前那个方向）= 本端那份自动转向没关掉，
     移动组件的纠偏把她朝向写回旧值了（§6.9）。看那两条日志的 `朝向`：本端现身那一帧写对了、
     之后被拽走 → 就是这条；两边一开始就差着 → 那是落点/目标那侧的问题。
   - 客户端当施法者：现身**那一帧**人就开始转（不再有站着不动的空档），**抬刀那段是正常速度**，
     等刀真正扫出去的那一瞬才「顿」——同一帧世界**整个冻一下** + 屏幕震一下；镜头从现身起就甩向
     「落点 → 敌人」那条线（客户端上应该是明显的一大甩：她是从敌人身后的门里出来的，
     现身那一帧相机还朝着"消失前那个方向"），刀扫出来的时候已经对准了。
     日志：`转圈时长账：推后 0.25s + 8 跳 × 0.25s = 2.25s，蒙太奇 X.XXs（够用）`、
     `甩镜开始：… 目标=X` → `甩镜第一帧（看 传送已套用）`→ `甩镜结束（看帧数和最后一帧写前）`、
     `P4 起手慢放 0.15s：角色速率 1.00 → 0.15`、
     `命中顿帧 0.070s（真实）：世界时间 1.00 → 0.05`、`命中顿帧结束：世界时间还原成 1.00`，
     以及 `EndAbility` 里那行的 `打击推后=1 起手慢放=1 顿帧=1 伤害跳=1`。
   - **顿/震的落点不对**（还是落在抬刀上，或者晚到刀已经扫过去了）→ 只调 `SpinImpactDelay`，
     别动 `SpinSlowTime`/`HitStopTime`（那两个管"顿多狠"，不管"什么时候顿"）。
   - **转得少了两下** = `转圈时长账` 那条报「★ 超了」，把 `SpinImpactDelay` 或 `SpinInterval` 调小。
   - **客户端镜头没甩** → 照 §6.4 那张表逐行对日志（表里六行覆盖了全部四种死法）。
   - **旁观者**那台机器上她起手是正常速度（`CustomTimeDilation` 不复制），但**顿帧是全场一起顿的** ——
     两边不一样是设计如此，不是 bug。
   - **顿帧没还原**（转完圈世界还是 0.05 速）= `EndAbility` 里的 `EndHitStop()` 被挪走了或提前 return 了。
     这是这次改动里**唯一会污染整局**的失败模式，技能中途被打断（把自己打死）也必须还原。
   - **她一直慢动作**（放完大招走路像卡带）= `EndSpinSlowMotion()` 没跑到，同理在 `EndAbility` 里。
   - **第二趟大招不顿帧** = `ActivateAbility` 开头那句 `bHitStopDone = false` 被删了
     （`InstancedPerActor` 复用同一个对象）。
   - 镜头甩得不够/过冲 → 调 `CameraAlignSpeed`；不想让镜头转 → `CameraAlignTime` 填 0。
   - 手感调法：想更"顿" → 缩短 `SpinSlowTime` + 压小 `SpinSlowScale`；想每跳都顿 → 打开 `bHitStopEveryPulse`。
   - 把 `bSlowMotionEnabled` 关掉跑一次，可以 A/B 出「慢放」这一层的贡献（甩镜和震动不受它影响）。

---

## 9. 明确不做（第一版范围外）

- **目标指示器 / HUD 血条标记**（`Ability_MarkedForDeath_Targeting` 那条动画留着以后做）
- **蓄力 / 可取消的锁定阶段**（Paragon 原版有，你们的描述里没有）
- **大招镜头**：`P_Kallari_Portal_CameraEffect` 资产存在，做法是照
  `UMyStealthCameraModifier` + `AGC_Stealth::ApplyLocalScreen`（`GC_Stealth.cpp:524-563`）那套。
  `AGC_DeathHarvestCast` 里已经留了 `ScreenModifierClass` / `ScreenMaterial` 两个位子，但它是独立一块，别塞进第一版。
- **友方目标过滤**：现在会打到范围内所有有 ASC 的东西（和 `GA_ThreeHitPassive::ApplyServerHit` 一致 ——
  全项目目前都没有队伍判定）。

---

## 10. 开工前待确认

- [x] **已损失生命值指目标的还是自己的** → **目标的（敌方）**，斩杀型。
      改成施法者的就是 §5.5 那两条捕获从 `Target` 改成 `Source`，一行的事。
- [x] **`Ability_Ultimate` 那条动画是不是转体** → **没有启用 RootMotion**，
      所以转体角度不来自动画，`bRootMotionSpin` 保持 `false`（**这已经是 C++ 的默认值，不用改任何东西**）。
      BP 里记得别把它勾上；勾上的表现是"转两圈"（动画自己转 + 代码再转一次）。
- [x] **§7 的架构缺口走 A 还是 B** → **走 B，已实现完毕、构建绿**（详见 §7）。
      A 当时被低估了：它也得先加 `PostGameplayEffectExecute`，否则 `State.Dead` 根本没地方挂。

---

## 11. ⚠️ 装的时候才会炸：Cascade 组件没有 `SetAutoDestroy`

**症状**（照抄本文档旧版片段会撞上）：

```
error C2039: "SetAutoDestroy": 不是 "UParticleSystemComponent" 的成员
```

位置是 §5.7 `UAnimNotifyState_SocketParticle::Teardown` 和 §5.8 / §5.9 / §5.10 三个
`AGC_DeathHarvest*::DestroyLoop`，一共四处，就是那些 `XxxComp->SetAutoDestroy(true);`。

**原因**：`SetAutoDestroy(bool)` 是 **Niagara 专有**的 API，Cascade 没有。

| 组件 | 怎么设自动销毁 | 依据 |
|---|---|---|
| `UNiagaraComponent` | `Comp->SetAutoDestroy(true);` | `NiagaraComponent.h:403` |
| `UParticleSystemComponent`（Cascade） | `Comp->bAutoDestroy = true;` | `ParticleSystemComponent.h:600` |

Cascade 那边 `bAutoDestroy` 是个 **public 位域**（`uint8 bAutoDestroy : 1;`，紧跟着的 602 行就是 `private:`）。
位域没有 setter，直接赋值 —— 所以那一行长得像"绕过封装"，其实是唯一写法。

**为什么会踩**：§5.7 开头写的是"结构照抄 `UAnimNotifyState_BladeTrail`"，但
**BladeTrail 用的是 Niagara**，它那份代码里 `SetAutoDestroy` 合法 —— 而 §5.7 同时又要求
"把 Niagara 换成 Cascade"。抄结构没问题，**偏偏这一行抄不过来**。
§5.8–§5.10 那三个 GC 是同一批 Cascade 组件，跟着一起错了。

**别拿"项目里已经有这个写法"当依据**：`GA_ThreeHitPassive.cpp:667` 也调了 `SetAutoDestroy`，
但那个成员是 `TObjectPtr<UNiagaraComponent>`（`.h:85`），编译得过。
判断依据只有一个 —— **那一处的组件类型**，不是"别处这么写没事"。

**改法**：四处 `SetAutoDestroy(true)` → `bAutoDestroy = true`，本文档里的代码片段已经是改过的。

> 这个坑跟"方案对不对"无关，纯粹是**文档里的代码片段抄不下来**，所以单列在这里，
> 没混进开头那四条"方案修正"里。
