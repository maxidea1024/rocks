"""Prove both policy detection and rejection by the configured native compiler."""
from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("warning_policy", ROOT / "Build/audit_warning_policy.py")
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    build = Path(sys.argv[1])
    cmake = sys.argv[2]
    for good in ("clang-cl /W4 /WX /c probe.cpp", "clang++ -Wall -Wextra -Wpedantic -Werror -c probe.cpp"):
        check(not policy.command_errors(good), "valid warning profile rejected")
    for bad in (
        "clang-cl /W4 /c probe.cpp", "clang++ -Wall -c probe.cpp",
        "clang-cl /W4 /WX /WX- /c probe.cpp", "clang-cl /W4 /WX /wd4101 /c probe.cpp",
        "clang++ -Wall -Werror -Wno-error=unused-variable -c probe.cpp",
        "clang++ -Wall -Werror -w -c probe.cpp", "clang-cl /W4 /WX /W0 /c probe.cpp",
        "clang-cl /W4 /WX /D_CRT_SECURE_NO_WARNINGS /c probe.cpp",
        "clang++ -Wall -Werror -c probe.cpp",
        "clang++ -Wall -Wextra -Werror -c probe.cpp",
        "clang++ -Wall -Wpedantic -Werror -c probe.cpp",
        "clang-cl /W4 /WX /clang:-Wno-error=unused-variable /c probe.cpp",
    ):
        check(policy.command_errors(bad), f"invalid warning profile accepted: {bad}")
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        source = root / "Engine/Probe.cpp"
        source.parent.mkdir()
        for suppressed in (
            "#pragma warning(disable: 4101)", '#pragma clang diagnostic ignored "-Wunused-variable"',
            '#pragma GCC diagnostic ignored "-Wunused-variable"', "#define _CRT_SECURE_NO_WARNINGS",
            '#pragma clang diagnostic warning "-Wunused-variable"',
            '#pragma GCC diagnostic warning "-Wunused-variable"',
        ):
            source.write_text(suppressed, encoding="utf-8")
            check(policy.source_errors(root), "owned source suppression escaped audit")
    failures = policy.audit(ROOT, build / "compile_commands.json")
    check(not failures, "\n".join(failures))

    # Force a fresh compiler invocation; a previous probe object cannot satisfy this control.
    for target, source, expected_success in (
        ("rocks_warning_clean_probe", "clean.cpp", True),
        ("rocks_warning_reject_probe", "warning.cpp", False),
    ):
        (build / "WarningChecks" / source).touch()
        result = subprocess.run(
            [cmake, "--build", str(build), "--target", target],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace", timeout=60,
        )
        if expected_success:
            check(result.returncode == 0, "clean compiler control failed:\n" + result.stdout)
        else:
            check(result.returncode != 0, "compiler accepted an intentional warning")
            diagnostic = result.stdout
            check("unusedWarningProbe" in diagnostic and
                  ("-Werror" in diagnostic or "C2220" in diagnostic),
                  "compile failed without the required warning-as-error diagnostic:\n" + diagnostic)
            print(diagnostic)
    print("Warning policy: flag/source negative controls and native compiler rejection passed")


if __name__ == "__main__":
    main()
