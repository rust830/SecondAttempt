# LOL 项目文件组织规范

## 目录结构

UE 模块标准布局：模块根的 `Public` / `Private` 下**按功能分子目录**，两边目录名一一对应。

```
Source/LOL/
  Public/
    GAS/            ← 所有 GAS 相关
    Animation/      ← 动画 notify 等
    Variant_Combat/         ┐
    Variant_Platforming/    ├ UE 模板自带示例，不参与整理
    Variant_SideScrolling/  ┘
    *.h             ← 框架类（GameMode / PlayerController / 关卡 Character）
  Private/
    GAS/
    Animation/
    Variant_*/
    *.cpp
```

## 规则

1. **`.h` 一律进 `Public/<功能>/`，`.cpp` 一律进 `Private/<功能>/`**，两边目录名保持一致。
2. **GAS 相关的全部进 `GAS/`。** 判据是「这个类是否直接持有或操作
   `AbilitySystemComponent` / `GameplayAbility` / `GameplayEffect` / `GameplayTag`」——
   是就进 `GAS/`，否就按它自己的功能分目录。

   当前 `GAS/` 的构成：

   | 前缀 / 类型 | 内容 |
   |---|---|
   | `GA_*` | GameplayAbility（技能实现） |
   | `GE_*` | GameplayEffect（状态 GE、冷却 GE、伤害 GE） |
   | `GC_*` | GameplayCue（表现层，Actor / Static 两种变体都有） |
   | `*Data` / `AbilitySet` | 配表 DataAsset |
   | `HeroCombatAttributeSet` / `MyAbilitySystemComponent` / `MyGameplayAbility` | GAS 基础设施 |
   | `LOLGameplayTags` | 原生 GameplayTag 声明 |
   | `HeroCombatCharacter` / `MyPlayerState` | 持有 ASC 的角色与 PlayerState |
   | `InputConfig` | `FAbilityInputAction`：输入 → `Ability.Slot.*` 标签的路由 |

3. **include 写全路径，不写相对路径**：`#include "GAS/GC_Stealth.h"`。
   模块根的 `Public/` 已在默认搜索路径里，所以从这里往下写就行。
4. **移动文件用 `git mv`**，保住文件历史。
5. **移动一个 UCLASS 文件不用动蓝图**：蓝图按类名引用（`/Script/LOL.类名`），
   和源文件路径无关。只有 C++ 的 `#include` 要跟着改。
6. 模板自带的 `Variant_*` 三个目录原样保留，不参与本项目的整理。

## 移动 / 改名后的自查

1. `grep -rn "旧文件名" Source/` —— 确认 include 全部改到位。
2. **关掉编辑器再编译**：改了 `UPROPERTY` / `UCLASS` 布局时热重载不可靠
   （会写成 `UnrealEditor-LOL-<n>.dll` 补丁名），必须重编 `UnrealEditor-LOL.dll`。
3. 打开一个引用了该类的蓝图，确认没有「找不到类」——正常情况下不该有，有就说明类名也改了。

## 新增 `.cpp` 之后的自查：unity build 重名

UE 默认开 unity build（`Intermediate/.../Module.LOL.N.cpp` 会把多个 `.cpp` 拼进同一个翻译单元）。
**匿名 namespace 挡不住这种冲突** —— 它只保证跨 TU 不冲突；合进一个 TU 之后，同签名的自由函数就是
重定义：

```
error C2084: 函数"`anonymous-namespace'::HasSocket(const USkeletalMeshComponent *, const FName &)"已有主体
```

**触发方式很隐蔽**：新增一个 `.cpp` 会让 UBT 重新分批，可能把原先不在同一批的两个文件凑到一起 ——
表现成「我只加了个新文件，别人早就写好的重名代码突然炸了」。

自查（列出会被 unity 撞上的内部函数名，有输出就是有重复）：

```bash
cd Source/LOL/Private
grep -rhoP '^\t(?:bool|void|float|double|int32|int|uint8|FString|const TCHAR\*)\s+\K\w+(?=\()' . --include=*.cpp \
  | sort | uniq -c | sort -rn | awk '$1>1'
```

改名时**新名字必须全局唯一**（加文件前缀，如 `SocketParticleHasSocket`）。
**不要**改成「换个具名 namespace」——具名 namespace 是外部链接，不同批次的同名 namespace 会在
**链接期**报 `LNK2005` 重复定义，比编译期更晚、更难查。

> 已处理的：`HasSocket` / `DescribeUserParameters` / `WarnIfMissingParameter`
> （涉及 `AnimNotifyState_SocketParticle.cpp` / `AnimNotifyState_BladeTrail.cpp` /
> `GA_ThreeHitPassive.cpp` / `GC_EmpoweredHit.cpp`，都加了文件前缀）。
> 这几份本质上是复制粘贴出来的同一段逻辑，更干净的做法是提成一个共享头 —— 改的时候一并考虑。

## 构建

**用 `Tools/vs_build.py`，不要直接跑 `Engine/Build/BatchFiles/Build.bat`。**

```bash
python Tools/vs_build.py --check-only                    # 只看状态，不构建
python Tools/vs_build.py --close-editor                  # 关编辑器 -> 构建
python Tools/vs_build.py --close-editor --launch-editor  # 构建完自动重开编辑器
```

道理：必须让 MSBuild → UnrealBuildTool → UBA 跑在 **VS 2022 的进程树**里，产物才能正常写盘。
在别的进程里直接跑 `Build.bat`，链接阶段会因为文件删除调用被拦而失败（`LNK1136` / `LNK1201`，
产物被写成 0 字节）。前提是 **VS 2022 开着并且载入了 `LOL.sln`**。

`--close-editor` 是必须的（不是可选项）：**编辑器运行时锁着 `UnrealEditor-LOL.dll`，链接替换不掉它。**
Live Coding 也救不了 —— 它加不了新的 `UCLASS` / `USTRUCT` 反射数据，所以新增类只能走
「关编辑器 → 重新链接 → 开编辑器」。

## Agent 协作约定

`Tools/` 下有一套驱动 VS 2022 的脚本，`Tools/README.md` 有清单。

**Agent 改文件时的默认动作：`prepare` → 编辑 → `diff`。**

```bash
python Tools/vs_show.py prepare <file>...   # 存基线 + 在 VS 里打开（改之前调）
python Tools/vs_show.py diff <file>         # 弹 VS 内置 Diff：基线 -> 当前
```

VS 的 `AutoloadExternalChanges` 默认开启（实测磁盘改写后约 0.5s 缓冲区自动刷新），
所以文件一旦 `prepare` 到编辑器里，后续改动是**实时可见**的。机制与边界见 `GAS_HUD_Setup.md` §13。
