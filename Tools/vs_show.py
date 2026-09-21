"""Make the agent's file edits *visible* inside the running Visual Studio 2022.

Background
----------
VS Code users get an extension that renders an agent's edits as inline diffs. VS 2022
has no such extension hook, but it has three things that compose into an equivalent
experience, all reachable through the automation (DTE) model:

  1. External-change detection is already on by default
     (Tools > Options > Environment > Documents > "Auto-load changes, if saved"),
     so a file the editor has open is reloaded the moment it changes on disk.
  2. `File.Compare` / `Tools.DiffFiles` opens VS's own diff window between two files.
  3. `ItemOperations.OpenFile` + `TextSelection.GotoLine` bring a file to the front
     and put the caret on the line that matters.

The catch is (1) only applies to files the editor actually has open. Files the agent
creates are not open, so nobody sees them. Hence `open` and `snapshot` below.

Typical agent workflow
---------------------
    # before editing: remember the old bytes and make sure the file is on screen
    python Tools/vs_show.py snapshot Source/LOL/Public/UI/HUDTypes.h

    # ... agent writes the file ...

    # after editing: the buffer has already auto-reloaded; show exactly what changed
    python Tools/vs_show.py diff Source/LOL/Public/UI/HUDTypes.h

Subcommands
-----------
    prepare <file>...     snapshot the baseline AND open it on screen (do this BEFORE editing)
    open <file>...        open in the editor, optionally --line N
    snapshot <file>...    save the current bytes as the diff baseline
    diff <file>           VS diff window: baseline -> current
    goto <file> --line N  open and jump to a line
    reload <file>         force a buffer reload (refuses if the buffer is dirty)
    status [<file>...]    is it open? is it dirty? how many lines?
    open-changed          open every source file modified in the last --minutes

Verified behaviour (measured, not assumed)
------------------------------------------
With the default options above, Visual Studio reloaded an open file roughly 1 second
after it was rewritten on disk -- no command needed, no notification bar, no click.
The only requirement is that the file is already open, which is what `prepare`/`open`
take care of. So the agent's workflow is: prepare -> edit -> diff.

Exit codes: 0 ok, 1 refused (e.g. would discard unsaved edits), 2 environment problem.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import os
import shutil
import sys
import time

DEFAULT_PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE_DIR = os.path.join("Saved", "AgentDiff")

# Kinds accepted by ItemOperations.OpenFile. "Code" forces a source editor.
VIEW_CODE = "vsViewKindCode"
VIEW_PRIMARY = "vsViewKindPrimary"

CODE_SUFFIXES = (
    ".h", ".hpp", ".cpp", ".c", ".cs", ".inl", ".py", ".md", ".ini", ".json", ".txt",
)


# --------------------------------------------------------------------------- setup


def attach_vs():
    try:
        import win32com.client as wc
    except Exception as exc:
        return None, f"pywin32 unavailable: {type(exc).__name__}: {exc}"
    failures = []
    for progid in ("VisualStudio.DTE.17.0", "VisualStudio.DTE"):
        try:
            return wc.GetActiveObject(progid), progid
        except Exception as exc:
            failures.append(f"{progid}: {type(exc).__name__}")
    return None, "no running Visual Studio (" + "; ".join(failures) + ")"


def baseline_path(project_dir: str, target: str) -> str:
    """Where the diff baseline for `target` lives (mirrors the relative path)."""
    abs_target = os.path.abspath(target)
    try:
        rel = os.path.relpath(abs_target, project_dir)
    except ValueError:
        rel = os.path.basename(abs_target)
    if rel.startswith(".."):
        digest = hashlib.sha1(abs_target.encode("utf-8")).hexdigest()[:8]
        rel = f"{digest}__{os.path.basename(abs_target)}"
    return os.path.join(project_dir, BASELINE_DIR, rel + ".before")


def find_document(dte, target: str):
    """Return the DTE Document for `target` if it is open, else None."""
    want = os.path.normcase(os.path.abspath(target))
    try:
        docs = dte.Documents
        count = docs.Count
    except Exception:
        return None
    for i in range(1, count + 1):
        try:
            doc = docs.Item(i)
            if os.path.normcase(os.path.abspath(doc.FullName)) == want:
                return doc
        except Exception:
            continue
    return None


def buffer_text(doc) -> str | None:
    """Current in-editor text of an open document (not the on-disk bytes)."""
    try:
        td = doc.Object("TextDocument")
        return td.StartPoint.CreateEditPoint().GetText(td.EndPoint)
    except Exception:
        pass
    # Fallback: select-all and read. This moves the selection, so it is a last resort.
    try:
        doc.Activate()
        sel = doc.Selection
        sel.StartOfDocument(False)
        sel.EndOfDocument(True)
        return sel.Text
    except Exception:
        return None


def disk_text(path: str) -> str | None:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return None


def short(path: str, project_dir: str) -> str:
    try:
        return os.path.relpath(os.path.abspath(path), project_dir)
    except ValueError:
        return path


# ---------------------------------------------------------------------- subcommands


def cmd_open(dte, project_dir, targets, line=None, activate=True):
    ok = True
    for target in targets:
        path = os.path.abspath(target)
        if not os.path.exists(path):
            print(f"  MISSING  {short(path, project_dir)}")
            ok = False
            continue
        doc = find_document(dte, path)
        if doc is None:
            kind = VIEW_CODE if path.lower().endswith(CODE_SUFFIXES) else VIEW_PRIMARY
            try:
                dte.ItemOperations.OpenFile(path, kind)
            except Exception:
                try:
                    dte.ItemOperations.OpenFile(path)
                except Exception as exc:
                    print(f"  FAILED   {short(path, project_dir)}: {exc}")
                    ok = False
                    continue
            # OpenFile hands back a Window, not a Document. Re-resolve so callers get a
            # Document -- Document.Object("TextDocument") is what can read the buffer.
            for _ in range(10):
                doc = find_document(dte, path)
                if doc is not None:
                    break
                time.sleep(0.2)
            print(f"  opened   {short(path, project_dir)}")
        else:
            print(f"  present  {short(path, project_dir)}")
        if activate and doc is not None:
            try:
                doc.Activate()
            except Exception:
                pass
    if line is not None:
        try:
            sel = dte.ActiveDocument.Selection
            sel.GotoLine(int(line), False)
            sel.StartOfLine()
            print(f"  caret    -> line {line}")
        except Exception as exc:
            print(f"  caret    failed: {exc}")
            ok = False
    return ok


def cmd_snapshot(dte, project_dir, targets):
    ok = True
    for target in targets:
        path = os.path.abspath(target)
        if not os.path.exists(path):
            print(f"  MISSING  {short(path, project_dir)}")
            ok = False
            continue
        dest = baseline_path(project_dir, path)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        try:
            shutil.copyfile(path, dest)
        except OSError as exc:
            print(f"  FAILED   {short(path, project_dir)}: {exc}")
            ok = False
            continue
        size = os.path.getsize(path)
        digest = hashlib.sha1(open(path, "rb").read()).hexdigest()[:8]
        print(f"  baseline {short(path, project_dir)}  ({size} B, {digest})")
    return ok


def run_diff_command(dte, left: str, right: str) -> tuple[bool, str]:
    """Open VS's diff window between two files. Several command names exist."""
    left_q, right_q = f'"{left}"', f'"{right}"'
    for name in ("Tools.DiffFiles", "File.Compare", "Diff.CompareWith", "Diff.CompareSelected"):
        try:
            dte.ExecuteCommand(name, f"{left_q} {right_q}")
            return True, name
        except Exception:
            continue
    return False, "no diff command accepted the arguments"


def cmd_diff(dte, project_dir, targets):
    ok = True
    for target in targets:
        path = os.path.abspath(target)
        base = baseline_path(project_dir, path)
        if not os.path.exists(base):
            print(f"  NO BASE  {short(path, project_dir)} -- run `snapshot` before editing")
            ok = False
            continue
        if not os.path.exists(path):
            print(f"  MISSING  {short(path, project_dir)}")
            ok = False
            continue

        before, after = disk_text(base), disk_text(path)
        if before == after:
            print(f"  same     {short(path, project_dir)} (nothing changed since snapshot)")
            continue

        changed = sum(
            1 for a, b in zip(before.splitlines(), after.splitlines()) if a != b
        ) + abs(len(before.splitlines()) - len(after.splitlines()))

        shown, how = run_diff_command(dte, base, path)
        if shown:
            print(f"  diff     {short(path, project_dir)}  ~{changed} line(s)  via {how}")
        else:
            print(f"  DIFF FAILED {short(path, project_dir)}: {how}")
            ok = False
    return ok


def cmd_goto(dte, project_dir, targets, line):
    return cmd_open(dte, project_dir, targets, line=line)


def cmd_prepare(dte, project_dir, targets, line=None):
    """Snapshot the baseline AND put the file on screen. Call this before editing."""
    print("-- baseline --")
    ok = cmd_snapshot(dte, project_dir, targets)
    print("-- editor --")
    ok = cmd_open(dte, project_dir, targets, line=line) and ok
    if ok:
        print("  ready: the editor will reload this file by itself when it changes")
    return ok


def cmd_reload(dte, project_dir, targets):
    """Force VS to re-read from disk, unless that would discard unsaved edits."""
    ok = True
    for target in targets:
        path = os.path.abspath(target)
        doc = find_document(dte, path)
        if doc is None:
            print(f"  not open {short(path, project_dir)} (nothing to reload)")
            continue
        try:
            dirty = not bool(doc.Saved)
        except Exception:
            dirty = False
        if dirty:
            print(
                f"  REFUSED  {short(path, project_dir)} -- the editor has unsaved edits; "
                "saving or discarding them is your call, not mine"
            )
            ok = False
            continue
        try:
            doc.Close()
            dte.ItemOperations.OpenFile(path, VIEW_CODE)
            print(f"  reloaded {short(path, project_dir)}")
        except Exception as exc:
            print(f"  FAILED   {short(path, project_dir)}: {exc}")
            ok = False
    return ok


def cmd_status(dte, project_dir, targets):
    try:
        total = dte.Documents.Count
    except Exception as exc:
        print(f"cannot read documents: {exc}")
        return False
    print(f"Visual Studio has {total} document(s) open")

    if not targets:
        # Summarise: open project files, flag agent-written ones missing from the editor.
        open_paths = set()
        for i in range(1, total + 1):
            try:
                open_paths.add(os.path.normcase(os.path.abspath(dte.Documents.Item(i).FullName)))
            except Exception:
                continue
        src = os.path.join(project_dir, "Source")
        on_disk, missing = [], []
        for root, _dirs, files in os.walk(src):
            for name in files:
                if not name.lower().endswith((".h", ".cpp", ".cs")):
                    continue
                full = os.path.normcase(os.path.abspath(os.path.join(root, name)))
                (on_disk if full in open_paths else missing).append(
                    os.path.relpath(os.path.join(root, name), project_dir)
                )
        print(f"  source files open in the editor : {len(on_disk)}")
        print(f"  source files NOT open           : {len(missing)}")
        return True

    for target in targets:
        path = os.path.abspath(target)
        doc = find_document(dte, path)
        rel = short(path, project_dir)
        if doc is None:
            print(f"  CLOSED   {rel}")
            continue
        text = buffer_text(doc)
        dirty = "dirty" if not bool(getattr(doc, "Saved", True)) else "saved"
        n_lines = len(text.splitlines()) if text else -1
        print(f"  OPEN     {rel}  [{dirty}, {n_lines} lines in buffer]")
    return True


def cmd_open_changed(dte, project_dir, minutes):
    """Open source files modified within the last `minutes` — i.e. what the agent touched."""
    cutoff = time.time() - minutes * 60
    src = os.path.join(project_dir, "Source")
    picked = []
    for root, _dirs, files in os.walk(src):
        for name in files:
            if not name.lower().endswith((".h", ".cpp", ".cs")):
                continue
            full = os.path.join(root, name)
            if os.path.getmtime(full) >= cutoff:
                picked.append(full)
    if not picked:
        print(f"  no source file changed in the last {minutes} minute(s)")
        return True
    picked.sort(key=os.path.getmtime)
    print(f"  {len(picked)} file(s) changed in the last {minutes} minute(s):")
    return cmd_open(dte, project_dir, picked)


# -------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Show the agent's edits inside the running Visual Studio 2022.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("command", choices=[
        "prepare", "open", "snapshot", "diff", "goto", "reload", "status", "open-changed",
    ])
    ap.add_argument("files", nargs="*", help="project-relative or absolute paths")
    ap.add_argument("--project-dir", default=DEFAULT_PROJECT_DIR)
    ap.add_argument("--line", type=int, default=None, help="for `goto`: 1-based line")
    ap.add_argument("--minutes", type=int, default=30,
                    help="for `open-changed`: how far back to look")
    args = ap.parse_args()

    project_dir = os.path.abspath(args.project_dir)
    files = [os.path.join(project_dir, f) if not os.path.isabs(f) else f for f in args.files]

    dte, info = attach_vs()
    if dte is None:
        print(f"FAIL: {info}")
        return 2
    print(f"VS {getattr(dte, 'Version', '?')} | {os.path.basename(str(getattr(dte.Solution, 'FullName', '?')))}")

    if args.command == "prepare":
        if not files:
            print("FAIL: nothing to prepare")
            return 2
        ok = cmd_prepare(dte, project_dir, files, line=args.line)
    elif args.command == "open":
        if not files:
            print("FAIL: nothing to open")
            return 2
        ok = cmd_open(dte, project_dir, files)
    elif args.command == "snapshot":
        if not files:
            print("FAIL: nothing to snapshot")
            return 2
        ok = cmd_snapshot(dte, project_dir, files)
    elif args.command == "diff":
        if not files:
            print("FAIL: nothing to diff")
            return 2
        ok = cmd_diff(dte, project_dir, files)
    elif args.command == "goto":
        if not files or args.line is None:
            print("FAIL: goto needs a file and --line")
            return 2
        ok = cmd_goto(dte, project_dir, files, args.line)
    elif args.command == "reload":
        if not files:
            print("FAIL: nothing to reload")
            return 2
        ok = cmd_reload(dte, project_dir, files)
    elif args.command == "status":
        ok = cmd_status(dte, project_dir, files)
    else:  # open-changed
        ok = cmd_open_changed(dte, project_dir, args.minutes)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
