"""Probe what the running Visual Studio 2022 exposes for *showing* file edits.

Question being answered: can the agent make its edits visible inside the VS editor
the way an editor extension would -- reload the buffer, pop a diff, jump to a line?

This script is read-only apart from optionally listing state; it changes nothing.

    python Tools/vs_caps.py
"""

from __future__ import annotations

import re
import sys


def attach():
    import win32com.client as wc

    for progid in ("VisualStudio.DTE.17.0", "VisualStudio.DTE"):
        try:
            return wc.GetActiveObject(progid), progid
        except Exception:
            continue
    return None, None


def hdr(text: str) -> None:
    print("")
    print("=" * 78)
    print(text)
    print("=" * 78)


def scan_commands(dte, patterns):
    rx = re.compile("|".join(re.escape(p) for p in patterns), re.IGNORECASE)
    try:
        commands = dte.Commands
        total = commands.Count
    except Exception as exc:
        print(f"  cannot read command table: {type(exc).__name__}: {exc}")
        return
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
        print(f"  no command matches {patterns}")
        return
    width = max(len(n) for n, _ in hits)
    for name, binding in sorted(hits):
        print(f"  {name.ljust(width)}  {binding}")
    print(f"  --- {len(hits)} match(es) of {total} scanned ---")


def open_documents(dte):
    try:
        docs = dte.Documents
        n = docs.Count
    except Exception as exc:
        print(f"  cannot read documents: {type(exc).__name__}: {exc}")
        return
    print(f"  {n} document(s) open")
    for i in range(1, n + 1):
        try:
            d = docs.Item(i)
            try:
                saved = d.Saved
            except Exception:
                saved = "?"
            print(f"    {'*' if saved is False else ' '} {d.FullName}")
        except Exception as exc:
            print(f"    <error: {exc}>")


def dump_properties(dte, category, page):
    """Dump a DTE options page so we can find the file-change-detection switches."""
    try:
        props = dte.Properties(category, page)
    except Exception as exc:
        print(f"  {category}\\{page}: unavailable ({type(exc).__name__}: {exc})")
        return
    try:
        n = props.Count
    except Exception as exc:
        print(f"  {category}\\{page}: no count ({exc})")
        return
    print(f"  {category}\\{page}: {n} item(s)")
    for i in range(1, n + 1):
        try:
            p = props.Item(i)
            print(f"    {p.Name} = {p.Value!r}")
        except Exception as exc:
            print(f"    <item {i}: {exc}>")


def main() -> int:
    dte, progid = attach()
    if dte is None:
        print("FAIL: no running Visual Studio found")
        return 2
    print(f"attached via {progid}, VS {getattr(dte, 'Version', '?')}")

    hdr("1. commands: reload / refresh an already-open file")
    scan_commands(dte, ["reload", "refresh"])

    hdr("2. commands: diff / compare files")
    scan_commands(dte, ["diff", "compare"])

    hdr("3. commands: open / navigate / go to line")
    scan_commands(dte, ["openfile", "file.open", "gotoline", "navigate", "goto"])

    hdr("4. currently open documents")
    open_documents(dte)

    hdr("5. options page: Environment > Documents (external-change detection)")
    dump_properties(dte, "Environment", "Documents")

    hdr("6. options page: TextEditor > General")
    dump_properties(dte, "TextEditor", "General")

    hdr("7. item operations surface (open / navigate)")
    io = dte.ItemOperations
    for attr in (
        "OpenFile",
        "Navigate",
        "AddNewItem",
        "NewFile",
        "IsFileOpen",
    ):
        print(f"  ItemOperations.{attr}: {'yes' if hasattr(io, attr) else 'NO'}")

    print("")
    return 0


if __name__ == "__main__":
    sys.exit(main())
