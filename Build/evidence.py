"""Build provenance; records failure and never infers a clean tree from a Git error."""
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import time


def source_state(root):
    files = []
    for name in ("Engine", "Tests", "Build", "Tools", ".github"):
        for path in (root / name).rglob("*"):
            relative = path.relative_to(root).parts
            if path.is_file() and "__pycache__" not in relative and relative[:2] not in {("Build", "Artifacts"), ("Build", "Cache")}:
                files.append(path)
    for name in ("CMakeLists.txt", "CMakePresets.json", ".clang-format", ".editorconfig"):
        path = root / name
        if path.is_file():
            files.append(path)
    digest = hashlib.sha256()
    for path in sorted(files, key=lambda item: item.relative_to(root).as_posix()):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return {"algorithm": "sha256(path-NUL-sha256(file))", "digest": digest.hexdigest(), "fileCount": len(files)}


def git_state(root):
    state = {"head": None, "dirty": None}
    for name, arguments in (("head", ["rev-parse", "HEAD"]), ("status", ["status", "--porcelain=v1", "--untracked-files=all"])):
        try:
            result = subprocess.run(["git", "-C", str(root)] + arguments, capture_output=True, text=True)
        except OSError as error:
            state[name + "Error"] = str(error)
            continue
        if result.returncode:
            state[name + "Error"] = result.stderr.strip()
        elif name == "head":
            state["head"] = result.stdout.strip()
        else:
            state["dirty"] = bool(result.stdout)
            state["dirtyEntries"] = result.stdout.splitlines()
    return state


class BuildEvidence:
    def __init__(self, root, preset):
        self.root = root
        self.directory = root / "out" / preset
        self.environment = os.environ.copy()
        self.started = time.time_ns()
        self.error = None
        self.data = {"schema": 1, "preset": preset, "platform": platform.system(),
                     "architecture": platform.machine(), "seed": 7351,
                     "startedUnixNs": self.started, "sourceBefore": source_state(root),
                     "gitBefore": git_state(root), "steps": []}

    def run(self, command, env):
        self.environment = env
        if command[0] == "ctest":
            report = self.directory / "foundation-report.json"
            if report.exists():
                report.unlink()
        print("+", " ".join(command), flush=True)
        start = time.monotonic_ns()
        result = subprocess.run(command, cwd=self.root, env=env)
        self.data["steps"].append({"command": command, "exitCode": result.returncode,
                                   "durationNs": time.monotonic_ns() - start,
                                   "status": "PASS" if result.returncode == 0 else "FAIL"})
        result.check_returncode()

    def finish(self):
        cache = {}
        path = self.directory / "CMakeCache.txt"
        if path.is_file():
            for line in path.read_text(encoding="utf-8").splitlines():
                if "=" in line and not line.startswith(("#", "//")):
                    key, value = line.split("=", 1)
                    cache[key.split(":", 1)[0]] = value
        self.data["configuration"] = cache.get("CMAKE_BUILD_TYPE")
        self.data["target"] = "CoreFoundationTests" if cache.get("ROCKS_BUILD_OBJECT") == "OFF" else "FoundationTests"
        self.data["sanitizer"] = "address" if cache.get("ROCKS_ASAN") == "ON" else None
        self.data["architecture"] = cache.get("CMAKE_OSX_ARCHITECTURES") or self.environment.get("VSCMD_ARG_TGT_ARCH") or platform.machine()
        compiler = cache.get("CMAKE_CXX_COMPILER")
        self.data["compiler"] = compiler
        if compiler:
            compiler = shutil.which(compiler, path=self.environment.get("PATH")) or compiler
            try:
                result = subprocess.run([compiler, "--version"], env=self.environment, capture_output=True, text=True)
                self.data["compilerVersion"] = result.stdout.strip() if result.returncode == 0 else None
                if result.returncode:
                    self.data["compilerVersionError"] = result.stderr.strip()
            except OSError as error:
                self.data["compilerVersion"] = None
                self.data["compilerVersionError"] = str(error)
        self.data["sourceAfter"] = source_state(self.root)
        stable = self.data["sourceBefore"] == self.data["sourceAfter"]
        self.data["sourceStableDuringRun"] = stable
        self.data["gitAfter"] = git_state(self.root)
        self.data["finishedUnixNs"] = time.time_ns()
        self.data["status"] = "PASS" if self.error is None and stable else "FAIL"
        self.data["error"] = self.error if stable else "Source inputs changed during the build/test interval"
        test_steps = [s for s in self.data["steps"] if s["command"][0] == "ctest"]
        self.data["testsStatus"] = test_steps[-1]["status"] if test_steps else "NOT RUN"
        report = self.directory / "foundation-report.json"
        self.data["runnerReport"] = str(report.relative_to(self.root))
        current = report.is_file() and report.stat().st_mtime_ns >= self.started and bool(test_steps)
        self.data["runnerReportCurrent"] = current
        if current:
            self.data["runnerReportSha256"] = hashlib.sha256(report.read_bytes()).hexdigest()
        self.directory.mkdir(parents=True, exist_ok=True)
        (self.directory / "build-evidence.json").write_text(json.dumps(self.data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        if not stable and self.error is None:
            raise RuntimeError("Source changed during build/test; evidence marked FAIL")
