import json
from pathlib import Path
import subprocess
import sys
import tempfile

tool = Path(__file__).resolve().parents[2] / "Tools/Scaffold/rocks.py"
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    plan = root / "plan.json"
    subprocess.run([sys.executable, str(tool), "template", "plan", "module", "Example", "--output", str(root), "--plan", str(plan)], check=True)
    original = plan.read_bytes()
    subprocess.run([sys.executable, str(tool), "template", "apply", "--plan", str(plan)], check=True)
    subprocess.run([sys.executable, str(tool), "template", "validate", "--plan", str(plan)], check=True)
    assert plan.read_bytes() == original
    rejected = subprocess.run([sys.executable, str(tool), "template", "apply", "--plan", str(plan)], capture_output=True)
    assert rejected.returncode != 0
    assert (root / "Example/Example.cpp").is_file()
    bad = json.loads(plan.read_text())
    bad["files"]["../escape.txt"] = "escape"
    plan.write_text(json.dumps(bad))
    assert subprocess.run([sys.executable, str(tool), "template", "apply", "--plan", str(plan)], capture_output=True).returncode != 0
    assert not (root / "escape.txt").exists()
print("Module scaffold plan/apply/validate, overwrite and traversal guards passed")
