"""Verify warning policy on owned source and actual generated compile commands."""
from pathlib import Path
import json
import re
import sys


def command_errors(command):
    failures = []
    if not re.search(r"(?:^|\s)(?:/WX|-Werror)(?=\s|$)", command):
        failures.append("missing warnings-as-errors")
    required = ("/W4",) if re.search(r"(?:^|\s)/WX(?=\s|$)", command) else ("-Wall", "-Wextra", "-Wpedantic")
    for flag in required:
        if not re.search(r"(?:^|\s)" + re.escape(flag) + r"(?=\s|$)", command):
            failures.append("missing required warning flag: " + flag)
    if re.search(r"(?:^|\s)(?:/WX-|/W[0-3]|/w(?:d\d+)?|(?:/clang:)?(?:-w|-Wno-[^\s]+))(?=\s|$)", command):
        failures.append("warning suppression or reduced warning level")
    if "_CRT_SECURE_NO_WARNINGS" in command or "_SCL_SECURE_NO_WARNINGS" in command:
        failures.append("global deprecation suppression")
    return failures


def source_errors(root):
    failures = []
    suppression = re.compile(
        r"^\s*#\s*pragma\s+(?:warning\s*\(\s*(?:disable|[0-3])\b|"
        r"(?:clang|GCC)\s+diagnostic\s+(?:ignored|warning)\b)|"
        r"^\s*#\s*define\s+(?:_CRT_SECURE_NO_WARNINGS|_SCL_SECURE_NO_WARNINGS)\b"
    )
    for directory in ("Engine", "Tests", "Tools"):
        for path in sorted((root / directory).rglob("*")):
            if path.suffix not in {".h", ".cpp", ".mm"}:
                continue
            for line, text in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
                if suppression.search(text):
                    failures.append(f"{path.relative_to(root)}:{line}: owned source suppresses warnings")
    return failures


def audit(root, database):
    failures = source_errors(root)
    entries = json.loads(database.read_text(encoding="utf-8-sig"))
    if not entries:
        failures.append("empty compilation database")
    for entry in entries:
        command = entry.get("command") or " ".join(entry["arguments"])
        failures.extend(f"{entry['file']}: {error}" for error in command_errors(command))
    return failures


if __name__ == "__main__":
    failures = audit(Path(sys.argv[1]), Path(sys.argv[2]))
    print("\n".join(failures) if failures else "Owned-source/compiler warning policy passed")
    sys.exit(bool(failures))
