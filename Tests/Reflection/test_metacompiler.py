# Historical fixture only: the maintained metadata compiler and tests are C#/.NET.
"""Run real Clang AST diagnostics and deterministic-output regressions."""
from pathlib import Path
import argparse
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "Tools/MetaCompiler/metacompiler.py"
FIXTURE = ROOT / "Tests/Reflection/ReflectionFixture.h"
COMPILER = None
CLANG_ARGS = []
TYPE_ID = "b1000000000000000000000000000001"
FIELD_ID = "b1000000000000000000000000000011"
PREAMBLE = """#include <Meta/Annotations.h>
#include <Object/ObjectRefs.h>
#include <Object/Object.h>
#include <Core/Text/String.h>
namespace probe {
"""


def declaration(field=None, version="1", bases="public rocks::Object", extra=""):
    if field is None:
        field = f'ROCKS_REFLECT_FIELD("{FIELD_ID}") rocks::int32 count = 0;'
    return (PREAMBLE + f'ROCKS_REFLECT_TYPE("{TYPE_ID}", {version})\nstruct Value : {bases} {{\n'
            + field + "\n" + extra + "\n};\n}\n")


class MetaCompilerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="rocks-meta-")
        self.root = Path(self.directory.name)
        self.input = self.root / "Probe.h"
        self.output = self.root / "Probe.generated.h"

    def tearDown(self):
        self.directory.cleanup()

    def run_generator(self, *, source=None, fixture=False, extra=(), expected=None):
        if source is not None:
            self.input.write_text(source, encoding="utf-8", newline="\n")
        command = [sys.executable, str(GENERATOR), "--input", str(FIXTURE if fixture else self.input),
                   "--include", "Tests/Reflection/ReflectionFixture.h" if fixture else "Probe.h",
                   "--output", str(self.output)]
        if COMPILER:
            command += ["--compiler", COMPILER]
        command += ["--clang-arg=" + argument for argument in CLANG_ARGS]
        completed = subprocess.run(command + list(extra), cwd=ROOT, capture_output=True,
                                   text=True, encoding="utf-8", timeout=40)
        if expected is None:
            self.assertEqual(completed.returncode, 0, completed.stderr)
        else:
            self.assertNotEqual(completed.returncode, 0, completed.stdout)
            self.assertIn(expected, completed.stderr)
        return completed

    def reject(self, source, diagnostic):
        sentinel = b"previous accepted output\n"
        self.output.write_bytes(sentinel)
        self.run_generator(source=source, expected=diagnostic)
        self.assertEqual(self.output.read_bytes(), sentinel)
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_fixture_determinism_no_rewrite_and_check(self):
        self.run_generator(fixture=True)
        expected = self.output.read_bytes()
        self.assertNotIn(b"\r", expected)
        self.assertIn(b"GeneratedType<::rocks::reflection_fixture::Node>", expected)
        # A fixed past timestamp removes clock resolution/sleep from this oracle.
        os.utime(self.output, ns=(1000000000000000000, 1000000000000000000))
        timestamp = self.output.stat().st_mtime_ns
        self.run_generator(fixture=True)
        self.assertEqual(self.output.read_bytes(), expected)
        self.assertEqual(self.output.stat().st_mtime_ns, timestamp)
        self.run_generator(fixture=True, extra=("--check",))
        self.assertEqual(self.output.stat().st_mtime_ns, timestamp)
        self.output.write_bytes(b"stale\n")
        self.run_generator(fixture=True, extra=("--check",), expected="generated output is stale")
        self.assertEqual(self.output.read_bytes(), b"stale\n")

    def test_evaluated_constant_version(self):
        self.run_generator(source=declaration(version="1 + 2"))
        self.assertIn(b"schema{Id(), 3,", self.output.read_bytes())

    def test_negative_and_oversize_version(self):
        for version in ("-1", "0", "4294967296ull"):
            with self.subTest(version=version):
                self.reject(declaration(version=version), "schema version must be 1..UINT32_MAX")

    def test_missing_field_annotation(self):
        self.reject(declaration("rocks::int32 count = 0;"), "every nonstatic field requires")

    def test_duplicate_stable_id(self):
        field = f'ROCKS_REFLECT_FIELD("{TYPE_ID}") rocks::int32 count = 0;'
        self.reject(declaration(field), "duplicate stable ID")

    def test_invalid_and_zero_stable_id(self):
        for identifier in ("not-a-guid", "00000000000000000000000000000000"):
            with self.subTest(identifier=identifier):
                field = f'ROCKS_REFLECT_FIELD("{identifier}") rocks::int32 count = 0;'
                self.reject(declaration(field), "stable GUID")

    def test_pointer_and_cv_field_rejected(self):
        for spelling in ("rocks::int32* count = nullptr", "const rocks::int32 count = 0"):
            with self.subTest(spelling=spelling):
                self.reject(declaration(f'ROCKS_REFLECT_FIELD("{FIELD_ID}") {spelling};'),
                            "unsupported field type")

    def test_private_and_bitfield_rejected(self):
        self.reject(declaration(f'private: ROCKS_REFLECT_FIELD("{FIELD_ID}") rocks::int32 count = 0;',
                                extra="public: rocks::int32 Read() const noexcept { return count; }"),
                    "only named public non-bitfield")
        self.reject(declaration(f'ROCKS_REFLECT_FIELD("{FIELD_ID}") unsigned int count : 1;'),
                    "only named public non-bitfield")

    def test_static_annotated_field_rejected(self):
        self.reject(declaration(f'ROCKS_REFLECT_FIELD("{FIELD_ID}") static inline rocks::int32 count = 0;'),
                    "static fields cannot carry")

    def test_manual_trace_rejected(self):
        self.reject(declaration(extra="void Trace() noexcept {}"), "manual Trace conflicts")

    def test_virtual_and_multiple_inheritance_rejected(self):
        self.reject(declaration(bases="public virtual rocks::Object"), "public nonvirtual")
        source = declaration(bases="public rocks::Object, public Other")
        source = source.replace("namespace probe {", "namespace probe { struct Other {};", 1)
        self.reject(source, "public nonvirtual")

    def test_unregistered_base_and_reference_rejected(self):
        source = declaration(bases="public Hidden")
        source = source.replace("namespace probe {", "namespace probe { struct Hidden : rocks::Object {};", 1)
        self.reject(source, "is not reflected in this unit")
        source = declaration(f'ROCKS_REFLECT_FIELD("{FIELD_ID}") rocks::ObjectRef<Hidden> next;')
        source = source.replace("namespace probe {", "namespace probe { struct Hidden : rocks::Object {};", 1)
        self.reject(source, "is not reflected in this unit")

    def test_clang_syntax_failure_preserves_previous_output(self):
        self.reject(declaration("this is not valid cxx;"), "Clang rejected reflection input")

    def test_diagnostic_downgrade_and_response_file_override_rejected(self):
        for argument in ("-Wno-error", "/clang:-Wno-error", "-w", "/W0", "@hidden.rsp"):
            with self.subTest(argument=argument):
                self.run_generator(source=declaration(), extra=("--clang-arg=" + argument,),
                                   expected="unsupported compiler diagnostics/response-file override")
        self.assertFalse(self.output.exists())

    def test_string_pointer_expression_cannot_change_id(self):
        field = f'ROCKS_REFLECT_FIELD(&"x{FIELD_ID}"[1]) rocks::int32 count = 0;'
        self.reject(declaration(field), "stable ID must be a GUID string literal")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler")
    parser.add_argument("--clang-arg", action="append", default=[])
    parsed = parser.parse_args()
    COMPILER = parsed.compiler
    CLANG_ARGS = parsed.clang_arg
    unittest.main(argv=[sys.argv[0]], verbosity=2)
