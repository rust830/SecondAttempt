"""Probe whether a Visual Studio 2022 instance is running and reachable via COM (DTE).

Reports the attached instance's version, mode, the open solution, and the active
solution configuration / project build contexts. Read-only: performs no build and
mutates nothing.
"""

from __future__ import annotations

import sys


def _iter_contexts(sb):
    """SolutionContexts is a 1-based COM collection; yield (name, config, should_build)."""
    try:
        contexts = sb.ActiveConfiguration.SolutionContexts
    except Exception as exc:  # pragma: no cover - diagnostic path
        yield ("<error>", str(exc), False)
        return
    try:
        count = contexts.Count
    except Exception:
        count = 0
    for i in range(1, count + 1):
        try:
            ctx = contexts.Item(i)
            yield (ctx.ProjectName, ctx.ConfigurationName, bool(ctx.ShouldBuild))
        except Exception as exc:  # pragma: no cover
            yield ("<error>", str(exc), False)


def main() -> int:
    try:
        import win32com.client as wc
    except Exception as exc:
        print(f"IMPORT_FAIL: {type(exc).__name__}: {exc}")
        return 2

    for progid in ("VisualStudio.DTE.17.0", "VisualStudio.DTE"):
        try:
            dte = wc.GetActiveObject(progid)
        except Exception as exc:
            print(f"ATTACH_FAIL {progid}: {type(exc).__name__}: {exc}")
            continue

        print(f"ATTACH_OK {progid}")
        try:
            print(f"  Version = {dte.Version}")
            print(f"  Mode    = {dte.Mode}")
        except Exception as exc:
            print(f"  meta_error = {exc}")

        try:
            sol = dte.Solution
            print(f"  Solution.IsOpen = {sol.IsOpen}")
            print(f"  Solution.FullName = {sol.FullName!r}")
            if sol.IsOpen:
                sb = sol.SolutionBuild
                print(f"  BuildState = {sb.BuildState}")
                print(f"  ActiveConfiguration = {sb.ActiveConfiguration.Name}")
                for name, config, should_build in _iter_contexts(sb):
                    print(f"    ctx: {name} | {config} | build={should_build}")
                print(f"  LastBuildInfo = {sb.LastBuildInfo}")
        except Exception as exc:
            print(f"  solution_error = {type(exc).__name__}: {exc}")

        return 0

    print("NO_RUNNING_VS")
    return 1


if __name__ == "__main__":
    sys.exit(main())
