"""Lexical guard for owned runtime sources, not a transitive binary-dependency proof."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
banned = re.compile(r"#\s*include\s*[<\"](?:vector|string|array|deque|list|map|set|unordered_map|unordered_set|memory|functional|thread|mutex|future|iostream|fstream|sstream|algorithm|ranges|span|filesystem)[>\"]|\bstd\s*::")
failures = []
for path in sorted((root / "Engine").rglob("*")):
    if path.suffix not in {".h", ".cpp", ".mm"}:
        continue
    for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if banned.search(line):
            failures.append(f"{path.relative_to(root)}:{number}: forbidden runtime STL use")
print("\n".join(failures) if failures else "Owned runtime lexical STL audit passed")
sys.exit(bool(failures))
