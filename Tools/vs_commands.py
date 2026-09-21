"""List Visual Studio commands related to compilation / hot reload / live coding.

Used to discover whether the Live Coding (hot-patch) command is registered, so a
running Unreal Editor can be recompiled without closing it and without a full relink.

    python Tools/vs_commands.py [substring ...]

With no arguments it scans for: livecoding, hotreload, unreal, compile, rebuild.
"""

from __future__ import annotations

import re
import sys


def attach():
    import win32com.client as wc

    for progid in ("VisualStudio.DTE.17.0", "VisualStudio.DTE"):
        try:
            return wc.GetActiveObject(progid)
        except Exception:
            continue
    return None


def main() -> int:
    patterns = sys.argv[1:] or ["livecoding", "hotreload", "unreal", "compile", "rebuild"]
    rx = re.compile("|".join(re.escape(p) for p in patterns), re.IGNORECASE)

    dte = attach()
    if dte is None:
        print("FAIL: no running Visual Studio found")
        return 2

    try:
        commands = dte.Commands
        total = commands.Count
    except Exception as exc:
        print(f"FAIL: cannot read command table: {type(exc).__name__}: {exc}")
        return 2

    print(f"scanning {total} commands for {patterns} ...")
    hits = []
    for i in range(1, total + 1):
        try:
            cmd = commands.Item(i)
            name = cmd.Name
        except Exception:
            continue
        if name and rx.search(name):
            try:
                binding = cmd.Binding
            except Exception:
                binding = ""
            hits.append((name, binding))

    if not hits:
        print("no matching commands")
        return 1

    width = max(len(n) for n, _ in hits)
    for name, binding in sorted(hits):
        print(f"  {name.ljust(width)}  {binding}")
    print(f"--- {len(hits)} match(es) ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
