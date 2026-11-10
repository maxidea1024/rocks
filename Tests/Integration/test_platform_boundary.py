"""Negative controls ensure the source guard rejects common API regressions."""
from pathlib import Path
import subprocess
import sys
import tempfile

audit = Path(__file__).resolve().parents[2] / "Build/audit_platform_boundary.py"
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    public = root / "Engine/Core/Public/Core/Fixture.h"
    public.parent.mkdir(parents=True)
    public.write_text("#pragma once\nclass Thread;\n", encoding="utf-8")
    assert subprocess.run([sys.executable, str(audit), str(root)], capture_output=True).returncode == 0
    for source in ("#if defined(_WIN32)\n#endif\n", "#include <Windows.h>\n", "#include <pthread.h>\n", "void f() { CreateThread(); }\n"):
        public.write_text(source, encoding="utf-8")
        outcome = subprocess.run([sys.executable, str(audit), str(root)], capture_output=True, text=True)
        assert outcome.returncode == 1 and "outside native backend" in outcome.stdout
    public.write_text("#pragma once\n", encoding="utf-8")
    native = root / "Engine/PlatformBase/Private/Win64/Fixture.cpp"
    native.parent.mkdir(parents=True)
    native.write_text("#include <Windows.h>\n", encoding="utf-8")
    assert subprocess.run([sys.executable, str(audit), str(root)], capture_output=True).returncode == 0
print("Platform source boundary positive and negative controls passed")
