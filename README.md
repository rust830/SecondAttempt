# LOL

UE 5.8 的 LoL 风格 MOBA 项目，玩法逻辑全部构建在 **Gameplay Ability System (GAS)** 之上。

## 在手机上读这个仓库

| 想干什么 | 打开 |
|---|---|
| 读代码 —— 文件树、跨文件搜索、语法高亮 | [`github.dev/rust830/SecondAttempt`](https://github.dev/rust830/SecondAttempt) |
| 读设计文档 —— 目录树、全文搜索、深色模式 | [文档站](https://rust830.github.io/SecondAttempt/) |
| 看提交历史 / 逐行 diff | GitHub 官方 App |

`github.dev` 是 GitHub 自带的 VS Code Web，浏览器直接打开、不用装任何东西。横屏 + 双指缩放就能读，比在 GitHub 页面上一层层点目录强很多 —— 而且有**跨文件搜索**。

> **`Content/` 不在仓库里。** 7.5 GB 的 Paragon 素材包加二进制 `.uasset` 是有意忽略的，clone 下来是"能读能搜但跑不起来"的空壳工程，要跑得自己补 `Content/`。

## 文档

| 文档 | 内容 |
|---|---|
| [`CONVENTIONS.md`](CONVENTIONS.md) | **先读这个。** 目录归属、命名、构建方式 —— 新文件放错目录会被指出来 |
| [`GAS_HUD_Setup.md`](GAS_HUD_Setup.md) | HUD 三层管道（PlayerState → HUDController → WBP）、槽位配置、编辑器挂载步骤 |
| [`GAS_Block_Setup.md`](GAS_Block_Setup.md) | 右键格挡 |
| [`GAS_Stealth_Setup.md`](GAS_Stealth_Setup.md) | 隐身 |
| [`GAS_ThreeHitPassive_Setup.md`](GAS_ThreeHitPassive_Setup.md) | 三连击被动 |
| [`GAS_ThrowDagger_Setup.md`](GAS_ThrowDagger_Setup.md) | 投掷匕首 |
| [`GAS_DeathHarvest_Setup.md`](GAS_DeathHarvest_Setup.md) | 死亡收割 |
| [`Tools/README.md`](Tools/README.md) | VS 2022 COM 桥的工具清单与坑位 |

## 代码结构

```
Source/LOL/
├── Public/ · Private/
│   ├── GAS/          技能、属性集、ASC、HUD 语义层、GameplayTags
│   ├── UI/           UMG 控件（零 GAS 依赖，只认 FHUD*View 契约）
│   ├── Animation/    动画通知
│   └── Variant_*/    引擎模板自带，未改动
Config/               DefaultEngine / DefaultGame / DefaultInput ...
Tools/                vs_build.py（构建）、vs_show.py（VS 内弹 diff）
```

## 构建

```bash
python Tools/vs_build.py --close-editor
```

**不要直接跑 `Engine/Build/BatchFiles/Build.bat`。** 两条硬约束：

- 编辑器运行时锁着 `Binaries/Win64/UnrealEditor-LOL.dll`，链接替换不掉它 —— 必须先把编辑器关掉。
- 新增 `UCLASS` / `USTRUCT` 时 Live Coding 救不了（它加不了反射数据），只能"关编辑器 → 重链 → 开编辑器"。

原理和全部坑位记录在 [`Tools/README.md`](Tools/README.md) 与 `CONVENTIONS.md`。

## 文档站

本站由根目录的 Markdown 自动生成：

```bash
pip install mkdocs-material jieba
python Tools/sync_docs.py    # 把根目录的文档同步进 docs/
mkdocs serve                 # 本地预览 http://127.0.0.1:8000
```

**文档的真值源永远在仓库根目录**，`docs/` 是构建中间产物（已 gitignore），别手动改里面的文件 —— 下次同步会被覆盖。
