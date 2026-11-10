import subprocess
import sys

result = subprocess.run([sys.argv[1], "--negative-control", "asan-uaf"], text=True, capture_output=True, timeout=15)
assert result.returncode != 0, result.returncode
assert "AddressSanitizer: heap-use-after-free" in result.stderr, result.stderr
print("AddressSanitizer detected the intentional child-process use-after-free")
