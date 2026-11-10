"""Reject OS dependencies outside the explicitly selected native backends."""
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
forbidden = re.compile(
    r"#\s*(?:if|ifdef|ifndef|elif)\b[^\n]*(?:_WIN32|_WIN64|__APPLE__|__linux__|ROCKS_PLATFORM_)"
    r"|#\s*include\s*[<\"](?:[Ww]indows|windowsx|synchapi|processthreadsapi|pthread|semaphore|unistd|mach/[^>\"]+)\.h[>\"]"
    r"|\b(?:CreateThread|WaitForSingleObject|pthread_create|pthread_join|pthread_mutex_lock|pthread_cond_wait|Sleep|nanosleep)\s*\("
)
failures = []
for folder in ("Engine", "Tests"):
    for path in sorted((root / folder).rglob("*")):
        if path.suffix not in {".h", ".cpp", ".mm"}:
            continue
        parts = path.relative_to(root).parts
        native_engine = parts[:3] == ("Engine", "PlatformBase", "Private") and parts[3] in {"Win64", "Mac"}
        native_fixture = parts[:3] == ("Tests", "Support", "Private") and parts[3] in {"Win64", "Mac"}
        if native_engine or native_fixture:
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
            if forbidden.search(line):
                failures.append(f"{path.relative_to(root)}:{number}: OS dependency outside native backend")
print("\n".join(failures) if failures else "Platform boundary audit passed")
sys.exit(bool(failures))
