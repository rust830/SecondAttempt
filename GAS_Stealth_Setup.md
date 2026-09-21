# Q 技能「隐身」完整实现步骤

## 0. 一句话方案

**状态与表现分离**：隐身是一个复制的 GameplayEffect 标签 `State.Stealth`（唯一事实来源）；渲染表现由 `UStealthComponent` 订阅该标签在本地驱动。能力 `GA_Stealth` 只做「提交 CD + 挂 GE」，很薄。

```
按 Q → ASC 路由到 Ability.Slot.Q → GA_Stealth.ActivateAbility
       ├─ CommitAbility（进 CD）
       ├─ 挂 GE_Stealth（SetByCaller Data.StealthDuration）→ 授予 State.Stealth
       └─ EndAbility（状态全由 GE 承载）

State.Stealth 标签变化 ──► UStealthComponent（每个客户端本地）
                            ├─ 进：SetOnlyOwnerSee(true) + 半透明 overlay（主人看幽灵，敌人看不见）
                            └─ 出：SetOnlyOwnerSee(false) + 清 overlay

破隐：
  普攻        → AHeroCombatCharacter::BasicAttackPressed 拦截 → RemoveActiveEffectsWithTags({State.Stealth})
  施放其他技能 → UMyAbilitySystemComponent::AbilityInputTagPressed 中央 hook（bBreaksStealthOnCast）
  持续时间到   → GE 到期自动移除 tag
```

破隐统一走 `RemoveActiveEffectsWithTags({State.Stealth})`：移除 GE 后 tag 计数归零 → 组件回调自动还原渲染。单一事实来源、天然幂等。

## 代码改动清单（已完成）

| 操作 | 文件 | 说明 |
|---|---|---|
| 改 | `Source/LOL/Public/GAS/LOLGameplayTags.h` / `.cpp` | 加 3 个原生 tag |
| 新建 | `Source/LOL/Public/GAS/GE_Stealth.h` / `.cpp` | 隐身持续 GE |
| 新建 | `Source/LOL/Public/GAS/GE_StealthCooldown.h` / `.cpp` | 冷却 GE |
| 新建 | `Source/LOL/Public/GAS/GA_Stealth.h` / `.cpp` | Q 技能 |
| 新建 | `Source/LOL/Public/StealthComponent.h` / `.cpp` | 渲染表现状态机 |
| 改 | `Source/LOL/Public/GAS/MyGameplayAbility.h` | 加 `bBreaksStealthOnCast` |
| 改 | `Source/LOL/Public/GAS/HeroCombatCharacter.h` / `.cpp` | 挂 StealthComponent + 普攻破隐 |
| 改 | `Source/LOL/Private/GAS/MyAbilitySystemComponent.cpp` | 中央施法破隐 hook |

原生 tag 已加：`Data.StealthDuration`、`State.Stealth`、`State.Cooldown.Stealth`。**不用改 `DefaultGameplayTags.ini`**（和 `Ability_Slot_*`、`State_Cooldown_*` 一致）。

---

## 第 1 步：编辑器资产配置

### 1a. ChampionKit（英雄的 AbilitySet 数据资产）

在 `GrantAbility` 加一条：

- `SlotTag` = `Ability.Slot.Q`
- `Ability` = `GA_Stealth`（或其蓝图子类）
- `AbilityLevel` = 1

> 授权后 `OnGiveAbility` 会打 `[Passive] OnGiveAbility ... 标签: Ability.Slot.Q`，确认 Q 槽位进了 `SlotAbilityMap`。

### 1b. `DA_InputConfig`

确认 `AbilityInputActions` 里有一条 `InputAction = IA_Q` → `SlotTag = Ability.Slot.Q`（让 Q 键路由到 Q 槽位）。没有就新建一个 `IA_Q`（Enhanced Input Action，数字按键 `Q`）并加进来。

### 1c.（可选）半透明幽灵材质 `M_Stealth`

不填也能跑（主人看到原样、敌人不可见）。想要 LoL 那种「自己看半透明」效果：

1. 新建 Material `M_Stealth`：Blend Mode = `Translucent`、Lighting Model = `Unlit`、给个淡蓝/淡紫色 Emissive、`Opacity` 填 0.3~0.5。
2. 在角色的 `HeroCombat` 里选中 `StealthComponent`，把 `StealthMaterial` 设成 `M_Stealth`。

> 注意：`UStealthComponent` 用的是 `SetOverlayMaterial`（叠加一层，不动原材质），退出时 `SetOverlayMaterial(nullptr)` 还原，无需保存/恢复原材质，天然无泄漏。

### 1d.（可选）表现

`GA_Stealth` 蓝图子类（或 C++ 默认值）里可填：`StealthMontage`（进入隐身动画）、`StealthNiagara` / `StealthSound`（进入隐身粒子/音效）。均为纯表现，可空。

---

## 第 2 步：验证清单（按顺序）

1. **编译**通过。
2. 进游戏，看 `[Passive] OnGiveAbility ... 标签: Ability.Slot.Q`。
3. **按 Q** → 角色进隐身：自己视角能看到（半透明 overlay），CD 进 `State.Cooldown.Stealth`。
4. **两个 PIE 窗口验证可见性**：主人窗口可见（半透明），另一窗口（模拟敌人）不可见。
5. **隐身中普攻** → 日志 `[Stealth] 普攻破隐`，mesh 恢复。
6. **隐身中按 W/E** → 日志 `[Stealth] 施放技能 Ability.Slot.W 破隐`，mesh 恢复。
7. **隐身中再按 Q** → 不误破（`bBreaksStealthOnCast=false`，且 CD 内会挡掉）。
8. **等到持续结束**（默认 8s）→ 自然破隐，无残留隐藏、无悬垂委托。
9. **连续两次 Q**（等 CD 后）→ 干净地再次隐身，无状态泄漏。

每步若不符合预期，先看对应 `[Stealth]` 日志打点有没有走到，再往下查——别猜。

---

## 明确不做（扩展项）

- **隐身移速加成**：需新增 `MoveSpeed` 属性 + CharacterMovement 集成。
- **AI 感知/索敌对隐身处理**：`CombatAIController` 可挂 `State.Stealth` 判断跳过目标。
- **受到伤害破隐**：需伤害接收链路 hook（本方案破隐规则为普攻 + 施法，已确认）。
- **PvP 服务器权威确认**：当前单机验证（`HasAuthority()` 恒 true）。联网时 GE 由 LocalPredicted 能力预测 + 服务器确认回滚，GAS 已处理；若要更强的服务器校验，参考 `GAS_ThrowDagger_Setup.md` 第 8 步的 RPC 模式。
