"""Self-test: does the running Visual Studio still auto-reload externally changed files?

`vs_show.py` relies on this behaviour, so it is worth being able to re-check it rather
than trusting the IDE defaults forever. The test is self-contained: it opens a scratch
file under Saved/Logs, confirms the editor buffer holds version 1, rewrites that file on
disk to version 2, then polls the *editor buffer* (not the disk) until it changes.

Nothing outside Saved/Logs is touched, and the scratch file is removed afterwards.

    python Tools/vs_live_check.py

Exit code 0 if the editor reloaded on its own, 1 if it did not, 2 if VS is not running.
"""

from __future__ import annotations

import os
import sys
import time

PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TARGET = os.path.join(PROJECT_DIR, "Saved", "Logs", "vs_live_demo.txt")

V1 = "VERSION-1-PLACEHOLDER-AAA\n"
V2 = "VERSION-2-PLACEHOLDER-BBB\n"
POLL_LIMIT = 20  # half-second ticks -> up to 10 s


def attach():
    import win32com.client as wc

    for progid in ("VisualStudio.DTE.17.0", "VisualStudio.DTE"):
        try:
            return wc.GetActiveObject(progid)
        except Exception:
            continue
    return None


def find_document(dte, path):
    """Open Document for `path`, or None. Must come from dte.Documents -- the object
    returned by ItemOperations.OpenFile is a Window and has no usable Object()."""
    want = os.path.normcase(os.path.abspath(path))
    docs = dte.Documents
    for i in range(1, docs.Count + 1):
        doc = docs.Item(i)
        if os.path.normcase(os.path.abspath(doc.FullName)) == want:
            return doc
    return None


def read_buffer(doc) -> str:
    td = doc.Object("TextDocument")
    return td.StartPoint.CreateEditPoint().GetText(td.EndPoint)


def main() -> int:
    dte = attach()
    if dte is None:
        print("FAIL: no running Visual Studio")
        return 2

    os.makedirs(os.path.dirname(TARGET), exist_ok=True)
    with open(TARGET, "w", encoding="utf-8") as fh:
        fh.write(V1)

    dte.ItemOperations.OpenFile(TARGET)
    time.sleep(1.5)
    doc = find_document(dte, TARGET)
    if doc is None:
        print("FAIL: the scratch document never appeared in the Documents collection")
        return 2

    print(f"1. opened {os.path.basename(TARGET)} in Visual Studio")
    holds_v1 = "VERSION-1" in read_buffer(doc)
    print(f"   buffer holds V1 : {holds_v1}")
    if not holds_v1:
        print("FAIL: editor buffer did not contain the initial content")
        return 2

    with open(TARGET, "w", encoding="utf-8") as fh:
        fh.write(V2)
    print("2. rewrote the file on disk -> V2")

    reloaded = False
    for tick in range(POLL_LIMIT):
        time.sleep(0.5)
        if "VERSION-2" in read_buffer(doc):
            reloaded = True
            print(f"   buffer shows V2 after ~{(tick + 1) * 0.5:.1f}s")
            break

    print(f"3. AUTO-RELOAD: {'YES' if reloaded else 'NO'}")
    if not reloaded:
        print(f"   buffer still: {read_buffer(doc).strip()!r}")
        print("   -> check Tools > Options > Environment > Documents >")
        print("      'Detect when file is changed outside the environment'")
        print("      'Auto-load changes, if saved'")

    try:
        doc.Close()
    except Exception as exc:
        print(f"4. could not close the scratch document: {exc}")
    try:
        os.remove(TARGET)
    except OSError:
        pass

    return 0 if reloaded else 1


if __name__ == "__main__":
    sys.exit(main())
