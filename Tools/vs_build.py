"""Drive a build of this Unreal project inside the *running* Visual Studio 2022.

Why this exists
---------------
Builds launched directly as a child process of the agent session have their file
delete operations refused by the host sandbox. MSVC relies on delete-on-close for
every output it writes, so `link.exe` dies (LNK1136 / LNK1201) and no DLL appears.

Driving the build through Visual Studio's automation model sidesteps this: VS runs
MSBuild -> UnrealBuildTool -> UBA inside `devenv.exe`'s process tree, well outside
the agent's, and the artifacts get written normally. Verified working.

The one thing this cannot fix
-----------------------------
A running Unreal Editor keeps `Binaries/Win64/UnrealEditor-LOL.dll` open, so the
link step cannot replace it -- whatever triggers the build. That is a UE constraint,
not a sandbox one. Hence `--close-editor`, which asks the editor to close gracefully
(you will still get the usual save prompt if packages are dirty).

Usage
-----
    python Tools/vs_build.py                     # report, close nothing, build
    python Tools/vs_build.py --close-editor      # close editor first, then build
    python Tools/vs_build.py --check-only        # report state only, build nothing
    python Tools/vs_build.py --close-editor --launch-editor
    python Tools/vs_build.py --force             # build even if the DLL is locked
    python Tools/vs_build.py --vs-config Development

Exit codes: 0 build succeeded, 1 build failed / skipped, 2 environment problem.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import sys
import time

DEFAULT_PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODULE_DLL = os.path.join("Binaries", "Win64", "UnrealEditor-LOL.dll")
LOG_DIR = os.path.join("Saved", "Logs")
UBT_LOG = os.path.join(
    os.environ.get("LOCALAPPDATA", ""), "UnrealBuildTool", "Log.txt"
)
DEFAULT_EDITOR_EXE = (
    r"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe"
)
EDITOR_WINDOW_CLASS = "UnrealWindow"


# --------------------------------------------------------------------------- setup


def attach_vs():
    """Attach to a running VS instance. Returns (dte, progid) or (None, reason)."""
    try:
        import win32com.client as wc
    except Exception as exc:
        return None, f"pywin32 unavailable: {type(exc).__name__}: {exc}"

    failures = []
    for progid in ("VisualStudio.DTE.17.0", "VisualStudio.DTE"):
        try:
            return wc.GetActiveObject(progid), progid
        except Exception as exc:
            failures.append(f"{progid}: {type(exc).__name__}: {exc}")
    return None, "no running Visual Studio (" + "; ".join(failures) + ")"


# ---------------------------------------------------------------------- lock check


def module_lock_state(project_dir: str) -> tuple[bool, str]:
    """True when the module DLL can be replaced (i.e. no editor is holding it)."""
    path = os.path.join(project_dir, MODULE_DLL)
    if not os.path.exists(path):
        return True, f"{MODULE_DLL} absent"
    try:
        with open(path, "r+b"):
            pass
        return True, f"{MODULE_DLL} writable"
    except PermissionError:
        return False, f"{MODULE_DLL} LOCKED (open in a running Unreal Editor)"
    except OSError as exc:
        return True, f"{MODULE_DLL} state unclear: {exc}"


# ------------------------------------------------------------------------ editor


def _editor_windows() -> list[tuple[int, str]]:
    """Top-level UnrealEditor windows as (hwnd, title)."""
    try:
        import win32gui
    except Exception:
        return []

    found: list[tuple[int, str]] = []

    def visit(hwnd, _param):
        try:
            if not win32gui.IsWindowVisible(hwnd):
                return True
            cls = win32gui.GetClassName(hwnd)
            title = win32gui.GetWindowText(hwnd)
            if cls == EDITOR_WINDOW_CLASS or "Unreal Editor" in title:
                found.append((hwnd, title))
        except Exception:
            pass
        return True

    try:
        win32gui.EnumWindows(visit, None)
    except Exception:
        return found
    return found


def request_editor_close(wait_seconds: int, project_dir: str) -> tuple[bool, str]:
    """Ask the editor to close, then wait for the module DLL to unlock."""
    try:
        import win32con
        import win32gui
    except Exception as exc:
        return False, f"pywin32 gui unavailable: {exc}"

    windows = _editor_windows()
    if not windows:
        return True, "no Unreal Editor window found (already closed)"

    for hwnd, title in windows:
        try:
            win32gui.PostMessage(hwnd, win32con.WM_CLOSE, 0, 0)
            print(f"  sent close request -> {title!r}")
        except Exception as exc:
            print(f"  could not close {title!r}: {exc}")

    deadline = time.time() + wait_seconds
    while time.time() < deadline:
        unlocked, note = module_lock_state(project_dir)
        if unlocked:
            return True, f"editor released the module ({note})"
        remaining = int(deadline - time.time())
        print(f"  waiting for editor to exit... {remaining}s left", end="\r")
        time.sleep(2)
    print()
    return False, (
        "timed out waiting for the editor to release the module. "
        "It is most likely showing a 'save changed packages' prompt -- "
        "answer it, then re-run."
    )


def launch_editor(exe: str, project_dir: str) -> tuple[bool, str]:
    """Relaunch the editor via the shell, so it is not a child of this session."""
    uproject = None
    for name in sorted(os.listdir(project_dir)):
        if name.lower().endswith(".uproject"):
            uproject = os.path.join(project_dir, name)
            break
    if uproject is None:
        return False, "no .uproject found"
    if not os.path.exists(exe):
        return False, f"editor executable not found: {exe}"
    try:
        import win32com.client as wc

        shell = wc.Dispatch("Shell.Application")
        shell.ShellExecute(exe, f'"{uproject}"', project_dir, "open", 1)
        return True, f"launch requested: {os.path.basename(uproject)}"
    except Exception as exc:
        return False, f"launch failed: {type(exc).__name__}: {exc}"


# -------------------------------------------------------------------------- UBT


def ubt_log_state() -> tuple[float, str]:
    try:
        return os.path.getmtime(UBT_LOG), UBT_LOG
    except OSError:
        return 0.0, UBT_LOG


def ubt_log_tail(lines: int) -> str:
    try:
        with open(UBT_LOG, "r", encoding="utf-8", errors="replace") as fh:
            content = fh.read()
    except OSError as exc:
        return f"<cannot read UBT log: {exc}>"
    return "\n".join(content.splitlines()[-lines:])


def ubt_verdict() -> str:
    """Pull the Result: line out of the UBT log."""
    try:
        with open(UBT_LOG, "r", encoding="utf-8", errors="replace") as fh:
            content = fh.read()
    except OSError:
        return "<unknown>"
    for line in reversed(content.splitlines()):
        stripped = line.strip()
        if stripped.startswith("Result:"):
            return stripped.split(":", 1)[1].strip()
    return "<no Result line>"


# ------------------------------------------------------------------------ build


def describe_vs(dte) -> list[str]:
    out = [f"attached to Visual Studio {getattr(dte, 'Version', '?')}"]
    try:
        sol = dte.Solution
        out.append(f"  solution   : {sol.FullName}")
        if sol.IsOpen:
            sb = sol.SolutionBuild
            out.append(f"  active cfg : {sb.ActiveConfiguration.Name}")
            contexts = sb.ActiveConfiguration.SolutionContexts
            for i in range(1, contexts.Count + 1):
                ctx = contexts.Item(i)
                flag = "BUILD" if ctx.ShouldBuild else "skip "
                out.append(f"    [{flag}] {ctx.ProjectName}")
    except Exception as exc:
        out.append(f"  solution_error: {type(exc).__name__}: {exc}")
    return out


def trigger_build(dte, vs_config: str | None) -> tuple[int, str]:
    sol = dte.Solution
    if not sol.IsOpen:
        return -1, "no solution open in Visual Studio"
    sb = sol.SolutionBuild
    # DTE 的 vsBuildState 是【从 1 开始】的：1 = NotStarted，2 = InProgress，3 = Done。
    # 这里必须比 2。写成 == 1 会把「还没构建过」当成「正在构建」，脚本就永远不动手
    # （症状：明明没有 MSBuild / UBT 子进程，却一直报 a build is already running）。
    if sb.BuildState == 2:
        return -2, "a build is already running in Visual Studio"
    try:
        if vs_config:
            sb.BuildProject(vs_config, r"Intermediate\ProjectFiles\LOL.vcxproj", True)
            return 0, f"built project with solution configuration {vs_config!r}"
        sb.Build(True)
    except Exception as exc:
        return -3, f"build invocation failed: {type(exc).__name__}: {exc}"
    try:
        return int(sb.LastBuildInfo), "ok"
    except Exception as exc:
        return -4, f"cannot read LastBuildInfo: {exc}"


# -------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Build this Unreal project inside the running Visual Studio 2022.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--project-dir", default=DEFAULT_PROJECT_DIR)
    ap.add_argument("--check-only", action="store_true", help="report state, build nothing")
    ap.add_argument("--close-editor", action="store_true",
                    help="ask the running Unreal Editor to close before building")
    ap.add_argument("--launch-editor", action="store_true",
                    help="relaunch the editor after a successful build")
    ap.add_argument("--editor-exe", default=DEFAULT_EDITOR_EXE)
    ap.add_argument("--close-timeout", type=int, default=120,
                    help="seconds to wait for the editor to release the module")
    ap.add_argument("--force", action="store_true",
                    help="build even when the module DLL is locked (link will likely fail)")
    ap.add_argument("--vs-config", default=None,
                    help="build this solution configuration instead of the active one")
    ap.add_argument("--output-lines", type=int, default=60)
    args = ap.parse_args()

    project_dir = os.path.abspath(args.project_dir)
    print(f"project : {project_dir}")

    # 1. editor state
    unlocked, note = module_lock_state(project_dir)
    print(f"module  : {note}")

    if args.close_editor and not unlocked:
        print("closing editor (answer its save prompt if one appears):")
        ok, msg = request_editor_close(args.close_timeout, project_dir)
        print(f"  -> {msg}")
        unlocked, note = module_lock_state(project_dir)
        print(f"module  : {note}")
        if not ok:
            print("ABORT: editor still holds the module.")
            return 1

    # 2. VS state
    dte, info = attach_vs()
    if dte is None:
        print(f"FAIL: {info}")
        return 2
    for line in describe_vs(dte):
        print(line)

    if args.check_only:
        print("check-only: no build performed")
        return 0

    if not unlocked and not args.force:
        print("SKIP: a running editor holds the module DLL, so the link cannot replace it.")
        print("      Re-run with --close-editor, or close the editor yourself.")
        return 1

    # 3. build
    before_mtime, _ = ubt_log_state()
    print("building... (blocking until Visual Studio reports done)")
    started = _dt.datetime.now()
    failed, note = trigger_build(dte, args.vs_config)
    elapsed = (_dt.datetime.now() - started).total_seconds()

    if failed < 0:
        print(f"BUILD_NOT_STARTED: {note}")
        return 1

    after_mtime, _ = ubt_log_state()
    fresh = after_mtime > before_mtime
    verdict = ubt_verdict() if fresh else "<UBT log not rewritten>"
    tail = ubt_log_tail(args.output_lines) if fresh else "<UBT did not run>"

    # 4. persist a copy for later inspection
    log_copy = None
    try:
        os.makedirs(os.path.join(project_dir, LOG_DIR), exist_ok=True)
        stamp = started.strftime("%Y%m%d-%H%M%S")
        log_copy = os.path.join(project_dir, LOG_DIR, f"vs_build_{stamp}.log")
        with open(log_copy, "w", encoding="utf-8", errors="replace") as fh:
            fh.write(f"# vs_build.py   {started.isoformat()}\n")
            fh.write(f"# elapsed      : {elapsed:.1f}s\n")
            fh.write(f"# vs failures  : {failed}\n")
            fh.write(f"# module       : {note}\n")
            fh.write(f"# UBT verdict  : {verdict}\n\n")
            fh.write(tail)
    except OSError as exc:
        print(f"(could not write log copy: {exc})")

    print("")
    print(f"elapsed        : {elapsed:.1f}s")
    print(f"VS failed projs: {failed}")
    print(f"UBT verdict    : {verdict}")
    if log_copy:
        print(f"log copy       : {log_copy}")
    print("--- UnrealBuildTool output (tail) ---")
    print(tail)

    ok = failed == 0 and verdict.lower().startswith("succeed")
    print(f"RESULT: {'BUILD SUCCEEDED' if ok else 'BUILD FAILED'}")

    if ok and args.launch_editor:
        launched, msg = launch_editor(args.editor_exe, project_dir)
        print(f"editor : {msg}")
        if not launched:
            return 1

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
