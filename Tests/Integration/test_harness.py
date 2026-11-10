import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary = sys.argv[1]
for control in ("check", "require"):
    result = subprocess.run([binary, "--negative-control", control], text=True, capture_output=True)
    assert result.returncode == 1, (control, result.returncode, result.stdout, result.stderr)
    assert "CHECK FAILED: false" in result.stderr
assert subprocess.run([binary, "--filter", "ThisTestDoesNotExist"], capture_output=True).returncode == 2
assert subprocess.run([binary, "--repeat", "0"], capture_output=True).returncode == 2
listed = subprocess.check_output([binary, "--list"], text=True).splitlines()
assert listed and listed == sorted(set(listed))
with tempfile.TemporaryDirectory() as directory:
    output = Path(directory) / "report.json"
    subprocess.run([binary, "--filter", "Harness_SeedIsAccessible", "--seed", "914", "--repeat", "3", "--json", str(output)], check=True, capture_output=True)
    report = json.loads(output.read_text())
    assert report["seed"] == 914 and report["repeat"] == 3 and report["failures"] == 0
    assert len(report["tests"]) == 1 and report["tests"][0]["status"] == "PASS"
    # 디렉터리는 보고서 파일로 열 수 없다. 결과 파일 생성 실패를 성공으로 감추지 않는다.
    unavailable = subprocess.run([binary, "--filter", "Harness_SeedIsAccessible", "--json", directory], text=True, capture_output=True, timeout=10)
    assert unavailable.returncode == 2, (unavailable.returncode, unavailable.stdout, unavailable.stderr)
    assert "Cannot open JSON report" in unavailable.stderr
    assert "[PASS]" not in unavailable.stdout
print("Harness failure controls, CLI and JSON report passed")

parallel = subprocess.run([binary, "--negative-control", "parallel"], text=True, capture_output=True, timeout=10)
assert parallel.returncode == 1, (parallel.returncode, parallel.stderr)
assert parallel.stderr.count("CHECK FAILED: parallel-negative-control") == 200
fatal = subprocess.run([binary, "--negative-control", "assert-stderr-lock"], text=True, capture_output=True, timeout=10)
assert fatal.returncode != 0, fatal.returncode
assert "Rocks invariant failed:" in fatal.stderr
print("Concurrent failure counting and native assert under locked stderr passed")
