# Tools — 驱动 VS 2022 的脚本

这些脚本不属于 UE 模块（在 `Source/` 之外，UBT 不会编译它们）。
共同点：都通过 **COM 自动化（DTE）驱动你已经打开的 Visual Studio 2022**，而不是自己起进程。

## 为什么是"驱动 VS"而不是"自己跑构建"

在本机环境下，凡是**由 Agent 会话直接生成的子进程**，写产物时的文件删除调用会被拦：

```
UbaSessionServer - SetFileInformationByHandle (FileDispositionInfo) failed ... (Access is denied.)
LNK1136: 库无效或损坏          <- .lib 被写成 0 字节
LNK1201: 写入程序数据库失败     <- .pdb 写不出来
```

MSVC 的 `link.exe` / Unreal Build Accelerator 靠 delete-on-close 写产物，正好踩在这个调用上。
这**不是文件锁**（用 `mv` 验证过文件没被占用），也和 `dangerouslyDisableSandbox`、换 PowerShell 通道无关。

**解法**：让 MSBuild → UnrealBuildTool → UBA 跑在 `devenv.exe` 的进程树里。删改产物的是 VS、不是 Agent，就绕过去了。

前提：**VS 2022 开着，并且载入了 `LOL.sln`**（不是 `.slnx` —— DTE 认 `.sln`）。

## 清单

| 脚本 | 用途 |
|---|---|
| `vs_build.py` | **主力构建工具。** 附着 VS → 检测 DLL 锁 →（可选）关编辑器 → 触发构建 → 读 UBT 日志出结论 |
| `vs_show.py` | **让改动在 VS 里可见。** 打开文件、弹 VS 内置 Diff、跳行、查状态 |
| `vs_live_check.py` | 自检：验证 VS 仍会自动重载外部改动（把 IDE 默认值当永久事实会翻车） |
| `vs_caps.py` | 探测这台 VS 暴露了哪些能力（命令表扫描 + 选项页转储） |
| `vs_commands.py` | 早期脚本：枚举命令表找 Live Coding 命令（结论：**没有**，5487 条全扫过） |
| `vs_probe.py` | 早期脚本：读解决方案与配置上下文 |
| `build-lol.bat` | 双击即用的 `vs_build.py` 包装 |

都用 **anaconda python** 跑（自带 pywin32）：

```bash
C:/Users/qqq/anaconda3/python.exe Tools/vs_build.py --check-only
```

> 别用 `~/.workbuddy/binaries/python/` 下那个 managed python —— 它没有 `win32com`；
> 往 `~/.workbuddy/binaries/python/envs/` 建 venv 会**静默失败**（exit 0 但目录是空的）。

## `vs_build.py`

```bash
python Tools/vs_build.py --check-only                    # 只看状态，什么都不动
python Tools/vs_build.py                                 # 构建（DLL 被锁时会跳过）
python Tools/vs_build.py --force                         # 明知被锁也构建（链接必失败）
python Tools/vs_build.py --close-editor                  # 先优雅关编辑器再构建
python Tools/vs_build.py --close-editor --launch-editor  # 构建完自动重开编辑器
python Tools/vs_build.py --vs-config Development          # 指定解决方案配置
```

退出码：0 成功、1 失败/跳过、2 环境问题。

`--close-editor` 向窗口类 **`UnrealWindow`** 发 `WM_CLOSE`（按类不按标题 —— 编辑器标题是本地化的，
比如 `LOL - 虚幻编辑器`），然后轮询 DLL 是否解锁；有脏包会弹保存提示，等到超时就中止。

**看 `%LOCALAPPDATA%\UnrealBuildTool\Log.txt`，不要看 VS 的 Output 面板** ——
面板读不出来（late-bound COM 拿不到 `ToolWindows`），而且 UBT 自己的日志能比对 mtime。
这点很重要：**VS 对"已最新"的项目也照样会调 UBT**（1.65s 跑完），所以**"没重编" ≠ "没执行"**。

## `vs_show.py`

```bash
python Tools/vs_show.py prepare <file>...    # 存基线 + 在 VS 里打开（改之前调这个）
python Tools/vs_show.py diff <file>          # 弹 VS 内置 Diff：基线 -> 当前
python Tools/vs_show.py open <file>...
python Tools/vs_show.py goto <file> --line 42
python Tools/vs_show.py reload <file>        # 强制重载；缓冲区脏则拒绝
python Tools/vs_show.py status [<file>...]   # 开着没 / 脏没 / 多少行
python Tools/vs_show.py open-changed --minutes 10
```

**工作流：`prepare` → 编辑 → `diff`。** 基线存在 `Saved/AgentDiff/<镜像相对路径>.before`。

为什么这样能"实时可见"：VS 的 `Tools > Options > Environment > Documents` 里
`DetectFileChangesOutsideIDE` 与 `AutoloadExternalChanges` 默认都是开的，
实测**磁盘改写后约 0.5s 编辑器缓冲区自动刷新**（无提示条、无需点击）。
唯一的条件是**文件得已经在 VS 里打开** —— 这就是 `prepare` 的作用。

`reload` 在缓冲区有未保存改动时**拒绝执行**：丢弃未保存内容是用户自己的决定，脚本不代劳。

### 两个实现上的坑

1. **`ItemOperations.OpenFile(...)` 返回的是 `Window`，不是 `Document`。**
   对它调 `.Object("TextDocument")` 报 `DISP_E_MEMBERNOTFOUND (-2147352573)`。
   要读缓冲区必须先从 `dte.Documents.Item(i)` 拿 `Document`：

   ```python
   td = doc.Object("TextDocument")
   text = td.StartPoint.CreateEditPoint().GetText(td.EndPoint)
   ```
2. **别对同一个文件并行发多个编辑操作** —— 会互相覆盖。串行。

### 忘了 `prepare` 就先改了文件？用 git 把基线补回来

`diff` 比的是 `Saved/AgentDiff/<相对路径>.before`（`prepare` / `snapshot` 存下来的）和磁盘当前内容。
**先编辑后 prepare 是没用的** —— 那会把改完的内容当成基线，`diff` 显示"零处不同"。

只要文件在 git 里、且 `HEAD` 就是编辑前的状态，基线可以现造：

```bash
dst="Saved/AgentDiff/$f.before"
mkdir -p "$(dirname "$dst")"
git show "HEAD:$f" > "$dst"
python Tools/vs_show.py diff "$f"
```

**行尾不会成为问题**：`disk_text()` 用 `open(..., encoding="utf-8")` 读，Python 的 universal newlines
会把 CRLF 归一成 LF，所以 LF 的 git 对象和 CRLF 的工作区文件能正常比对（否则会整篇标红）。

前提是那个文件**相对 HEAD 没有再叠加别的未提交改动** —— 有的话基线会带上它们，diff 就不再是"本次改动"。
先 `git diff --numstat HEAD -- <file>` 看一眼这个文件干不干净。

## 边界

- VS 2022 **没有**给第三方 agent 的 inline diff 扩展接口。能做到的最接近形态就是
  `prepare` + `diff`（文件先在眼前、内容自己变、再弹整窗 diff）。
- 没有后台文件监视进程。那种进程会在 Agent 这一轮结束时被杀掉，做成"看着在跑、实际已死"更糟。
  所以用每轮的 `open` / `open-changed` 代替。
- `vs_show.py status` 可能列出**指向已不存在路径**的标签（源码 `Private`/`Public` 重组之前的残留）。
  不影响编译，手动关掉即可。
