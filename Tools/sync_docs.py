#!/usr/bin/env python3
"""把仓库根目录的 Markdown 同步进 docs/，供 MkDocs 构建。

为什么是"拷贝"而不是把文档搬进 docs/：
    `GAS_*_Setup.md` 放在项目根目录是既定约定（见 CONVENTIONS.md），
    而 MkDocs 需要一个 docs_dir。所以这里做一层单向拷贝 ——
    真值源永远在仓库根目录，docs/ 只是构建中间产物。

docs/ 已在 .gitignore 里。**别手动改 docs/ 里的文件**，改动会在下次同步时被覆盖。

用法：
    python Tools/sync_docs.py     # 然后 mkdocs serve / mkdocs build
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"

# 同名直接拷贝的源（glob 形式）。
GLOBS = ("GAS_*.md", "CONVENTIONS.md")

# 需要改名的源：仓库里的名字 -> 站点里的名字。
# README.md 同时是 GitHub 首页和站点首页，所以映射成 index.md。
RENAMES = {
    "README.md": "index.md",
    "Tools/README.md": "Tools-README.md",
}


def collect_sources() -> list[tuple[Path, str]]:
    pairs: list[tuple[Path, str]] = []
    for pattern in GLOBS:
        for src in sorted(ROOT.glob(pattern)):
            pairs.append((src, src.name))
    for src_rel, dst_name in RENAMES.items():
        src = ROOT / src_rel
        if src.is_file():
            pairs.append((src, dst_name))
    return pairs


def main() -> int:
    pairs = collect_sources()
    if not pairs:
        print("[sync_docs] 一个 Markdown 都没找到，检查 GLOBS / RENAMES 配置", file=sys.stderr)
        return 1

    # 目标文件都来自上面的 source 列表，所以文档总数是不变的。
    # 这里只做「假设的个数 != 实际找到的个数」的粗校验：漏掉一个通常是改名或移位置了。
    expected_files = len(GLOBS)  # GAS_*.md 是 glob，个数不定，只用来兜底提示
    if len(pairs) < expected_files:
        print(f"[sync_docs] 只匹配到 {len(pairs)} 个文件，是不是文档被移走了？", file=sys.stderr)

    DOCS.mkdir(exist_ok=True)

    # 清掉上一轮留下的、这一轮不再产出的页面 —— 否则 mkdocs 会拿它当孤儿页面报警告。
    expected = {dst for _, dst in pairs}
    for stale in DOCS.glob("*.md"):
        if stale.name not in expected:
            stale.unlink()
            print(f"[sync_docs] 移除过期页面  docs/{stale.name}")

    for src, dst_name in pairs:
        shutil.copy2(src, DOCS / dst_name)
        print(f"[sync_docs] {src.relative_to(ROOT)}  ->  docs/{dst_name}")

    print(f"[sync_docs] 完成，共 {len(pairs)} 个页面")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
