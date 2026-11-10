"""Native-host Foundation build. Imported Windows environment is never printed."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import json
from evidence import BuildEvidence

ROOT = Path(__file__).resolve().parents[1]


def environment():
    env = os.environ.copy()
    if sys.platform != "win32":
        return env
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError("Visual Studio C++ tools and Windows SDK are required")
    installation = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"], text=True).strip()
    if not installation:
        raise RuntimeError("No Visual Studio installation with C++ tools")
    script = Path(installation) / "Common7/Tools/VsDevCmd.bat"
    command = f'"{script}" -no_logo -arch=x64 -host_arch=x64 >nul && set'
    command_line = '"' + os.environ.get("COMSPEC", "cmd.exe") + '" /d /s /c "' + command + '"'
    output = subprocess.check_output(command_line, text=True)
    for line in output.splitlines():
        key, separator, value = line.partition("=")
        if separator and key:
            env[key.upper()] = value
    if shutil.which("ninja", path=env.get("PATH")) is None:
        ninja = Path(installation) / "Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
        if not (ninja / "ninja.exe").is_file():
            raise RuntimeError("Ninja is required (install VS C++ CMake tools or add Ninja to PATH)")
        env["PATH"] = str(ninja) + os.pathsep + env.get("PATH", "")
    compiler = shutil.which("clang-cl", path=env.get("PATH"))
    if compiler:
        resource = Path(subprocess.check_output([compiler, "/clang:-print-resource-dir"], env=env, text=True).strip())
        for directory in (resource / "lib/x86_64-unknown-windows-msvc", resource / "lib/windows"):
            if any(directory.glob("clang_rt.asan_dynamic*.dll")):
                env["PATH"] = str(directory) + os.pathsep + env.get("PATH", "")
    return env


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preset", default="windows-clang-debug" if sys.platform == "win32" else "macos-clang-debug")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--skip-tests", action="store_true")
    args = parser.parse_args()
    presets = json.loads((ROOT / "CMakePresets.json").read_text(encoding="utf-8-sig"))
    allowed = {p["name"] for p in presets["configurePresets"] if not p.get("hidden")}
    if args.preset not in allowed:
        raise RuntimeError("Unknown preset")
    evidence = BuildEvidence(ROOT, args.preset)
    try:
        env = environment()
        evidence.run(["cmake", "--preset", args.preset, f"-DPython3_EXECUTABLE={sys.executable}"], env)
        if not args.configure_only:
            evidence.run(["cmake", "--build", "--preset", args.preset], env)
            if not args.skip_tests:
                evidence.run(["ctest", "--preset", args.preset], env)
    except BaseException as error:
        evidence.error = str(error)
        raise
    finally:
        evidence.finish()



if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
