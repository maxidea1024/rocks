import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--check", action="store_true")
args = parser.parse_args()
formatter = os.environ.get("ROCKS_CLANG_FORMAT", "clang-format")
version = subprocess.check_output([formatter, "--version"], text=True)
if "version 21.1.6" not in version:
    sys.exit("Rocks requires clang-format 21.1.6; set ROCKS_CLANG_FORMAT to that binary")
files = [str(p) for folder in ("Engine", "Tests") for p in sorted((ROOT / folder).rglob("*")) if p.suffix in {".h", ".cpp", ".mm"}]
for start in range(0, len(files), 40):
    command = [formatter, "--dry-run", "--Werror"] if args.check else [formatter, "-i"]
    subprocess.run(command + files[start:start + 40], cwd=ROOT, check=True)
