import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Build"))
from evidence import BuildEvidence

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    (root / "Engine").mkdir()
    source = root / "Engine/source.h"
    source.write_text("before\n")
    proof = BuildEvidence(root, "expected-failure")
    try:
        proof.run([sys.executable, "-c", "raise SystemExit(17)"], proof.environment)
    except subprocess.CalledProcessError as error:
        proof.error = str(error)
    else:
        raise AssertionError("Negative control unexpectedly succeeded")
    proof.finish()
    report = json.loads((root / "out/expected-failure/build-evidence.json").read_text())
    assert report["status"] == "FAIL" and report["steps"][0]["exitCode"] == 17
    assert report["testsStatus"] == "NOT RUN" and not report["runnerReportCurrent"]
    assert report["gitAfter"]["dirty"] is None
    changed = BuildEvidence(root, "source-change")
    source.write_text("after\n")
    try:
        changed.finish()
    except RuntimeError:
        pass
    else:
        raise AssertionError("Source mutation was not rejected")
    report = json.loads((root / "out/source-change/build-evidence.json").read_text())
    assert report["status"] == "FAIL" and not report["sourceStableDuringRun"]
print("Build failure provenance, unknown Git state and changed-source rejection passed")
