using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// self 시험 중 저장소 정책 검사(PolicyChecks)다: 저장소 전체 실행과 검사마다의 양성·음성 대조.
internal static partial class SelfTests
{
    // 정책 시험(Policy.*)을 원래 순서로 등록한다. 서드파티 정책 시험(ThirdPartyPolicyTests)도 원래 자리에서
    // 등록한다.
    private static void RegisterPolicy(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryRuntime", () => NoFailures(PolicyChecks.Runtime(options.Root)));
        suite.Add("Policy.RepositoryReflectionAnnotations", () => NoFailures(PolicyChecks.ReflectionAnnotations(options.Root)));
        ThirdPartyPolicyTests.Register(suite, options);
        ThirdPartyKindTests.Register(suite);
        suite.Add("Policy.RepositoryPlatform", () => NoFailures(PolicyChecks.PlatformBoundary(options.Root)));
        suite.Add("Policy.RepositoryTargetMacros", () => NoFailures(PolicyChecks.TargetMacros(options.Root)));
        suite.Add("Policy.RepositoryTargetIntrinsics", () => NoFailures(PolicyChecks.TargetIntrinsics(options.Root)));
        suite.Add("Policy.RepositoryTestAccess", () => NoFailures(PolicyChecks.TestAccess(options.Root)));
        suite.Add("Policy.RepositoryWarnings", () => NoFailures(PolicyChecks.SourceWarnings(options.Root)));
        suite.Add("Policy.RepositoryOptimizationOverrides", () => NoFailures(PolicyChecks.OptimizationOverrides(options.Root)));
        suite.Add("Policy.OptimizationOverrideControls", () =>
        {
            using TemporaryDirectory temporary = new();
            // 정의 헤더와 테스트 소스는 허용한다.
            temporary.Write("Core/Source/Runtime/Base/Public/Base/Compiler.h", "#define ROCKS_DISABLE_OPTIMIZATION _Pragma(\"clang optimize off\")\n");
            temporary.Write("Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "ROCKS_DISABLE_OPTIMIZATION\nvoid F() {}\nROCKS_ENABLE_OPTIMIZATION\n");
            temporary.Write("Engine/Source/Runtime/Probe/Private/Clean.cpp", "// ROCKS_DISABLE_OPTIMIZATIONS is a different name\nvoid G() {}\n");
            NoFailures(PolicyChecks.OptimizationOverrides(temporary.Root));
            temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", "// CodeOptimization.Always\nOptimization => CodeOptimization.Always;\n");
            NoFailures(PolicyChecks.OptimizationOverrides(temporary.Root));
            foreach (string source in new[] { "ROCKS_DISABLE_OPTIMIZATION", "ROCKS_ENABLE_OPTIMIZATION", "#pragma clang optimize off",
                "#  pragma  clang optimize on", "_Pragma(\"clang optimize off\")", "#pragma optimize(\"\", off)",
                "[[clang::optnone]] void F();", "__attribute__((optnone)) void F();" })
            {
                temporary.Write("Engine/Source/Runtime/Probe/Private/Probe.cpp", source + "\n");
                Verify.Equal(1, PolicyChecks.OptimizationOverrides(temporary.Root).Count, "optimization override escaped audit: " + source);
            }

            temporary.Write("Engine/Source/Runtime/Probe/Private/Probe.cpp", "void F();\n");
            temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", "protected override CodeOptimization Optimization => CodeOptimization.Never;\n");
            Verify.Equal(1, PolicyChecks.OptimizationOverrides(temporary.Root).Count, "committed Never optimization rule escaped audit");
            temporary.Write("Engine/Source/Runtime/Probe/Probe.Build.cs", "// rules\n");

            // 강제 인라인은 테스트 소스에서도 거부한다. 단어만 언급한 주석은 허용한다.
            temporary.Write("Engine/Source/Runtime/Probe/Private/Probe.cpp", "// UE FORCEINLINE과 always_inline은 쓰지 않는다\nvoid F();\n");
            NoFailures(PolicyChecks.OptimizationOverrides(temporary.Root));
            foreach (string source in new[] { "inline __attribute__((always_inline)) void F();", "__attribute__ ((__always_inline__)) void F();",
                "[[clang::always_inline]] void F();", "[[gnu::always_inline]] void F();", "__forceinline void F();" })
            {
                foreach (string file in new[] { "Engine/Source/Runtime/Probe/Private/Probe.cpp", "Core/Source/Runtime/Core/Tests/ProbeTests.cpp" })
                {
                    temporary.Write("Engine/Source/Runtime/Probe/Private/Probe.cpp", "void F();\n");
                    temporary.Write("Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "void G();\n");
                    temporary.Write(file, source + "\n");
                    var failures = PolicyChecks.OptimizationOverrides(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].ToString().Contains("forced-inline", StringComparison.Ordinal),
                        $"forced inline escaped audit in {file}: {source}");
                }
            }
        });
        suite.Add("Policy.RepositoryCompilerBuiltins", () => NoFailures(PolicyChecks.CompilerBuiltins(options.Root)));
        suite.Add("Policy.RepositoryUnicodeTables", () =>
        {
            NoFailures(UnicodeTables.Check(options.Root));
            // 생성기는 결정적이다(두 번 만든 결과가 같은 byte).
            Verify.Equal(UnicodeTables.Generate(options.Root), UnicodeTables.Generate(options.Root), "Unicode table generator is not deterministic");
        });
        suite.Add("Policy.RepositoryMathTables", () =>
        {
            NoFailures(MathTables.Check(options.Root));
            Verify.Equal(MathTables.Generate(), MathTables.Generate(), "sRGB table generator is not deterministic");
        });
        suite.Add("Policy.DeterministicMathSources", () =>
        {
            NoFailures(PolicyChecks.DeterministicMathSources(options.Root));
            using TemporaryDirectory temporary = new();
            const string module = "Core/Source/Runtime/DeterministicMath";
            temporary.Write(module + "/Private/Good.cpp",
                "#include \"SrgbTables.generated.h\"\n#include <DeterministicMath/DeterministicMath.h>\n#include <Platform/Memory.h>\n");
            temporary.Write(module + "/Public/DeterministicMath/Good.h",
                "#pragma once\n#include <Base/Types.h>\ninline constexpr uint32 Version = 1;\ndouble Exp2(double x) noexcept;\n");
            NoFailures(PolicyChecks.DeterministicMathSources(temporary.Root));
            // PlatformMath(header inline 근사 명령)·CRT 수학 헤더, 확장 정밀도, 공개 헤더의 함수 본문.
            temporary.Write(module + "/Private/Leak.cpp",
                "#include <Platform/PlatformMath.h>\n#include <math.h>\nlong  double Wide();\n");
            temporary.Write(module + "/Public/DeterministicMath/Inline.h",
                "#pragma once\n#include <Platform/Memory.h>\ninline double Twice(double x) noexcept { return x + x; }\n");
            var failures = PolicyChecks.DeterministicMathSources(temporary.Root);
            Verify.Equal(5, failures.Count, "deterministic math source violations: " + string.Join("; ", failures));
        });
        suite.Add("Policy.CrtNumberConversions", () =>
        {
            NoFailures(PolicyChecks.CrtNumberConversions(options.Root));
            using TemporaryDirectory temporary = new();
            // 양성: 이름마다 한 줄(해석·wide·POSIX·MSVC 변형·scanf·locale·cvt, 전역 한정, 함수 포인터, 여러 줄 호출).
            string[] positives =
            [
                "double a = strtod(text, &end);", "float b = strtof (text, nullptr);", "long double c = ::strtold(t, 0);",
                "auto d = strtod_l(t, 0, locale);", "auto e = _strtod_l(t, 0, locale);", "auto f = atof(t);", "auto g = _wtof(w);",
                "auto h = wcstod(w, 0);", "sscanf(t, \"%d\", &i);", "sscanf_s(t, \"%d\", &i);", "vsscanf(t, f, args);",
                "setlocale(LC_ALL, \"\");", "auto* k = localeconv();", "uselocale(l);", "_configthreadlocale(1);",
                "_gcvt(x, 17, buffer);", "auto pointer = &strtod;", "auto call = strtod", "snprintf(b, n, \"%.17g\", x);",
                "printf(\"%f\\n\", x);",
            ];
            var positiveText = string.Join('\n', positives) + "\n    (text, nullptr);\n";
            temporary.Write("Core/Source/Runtime/Fake/Private/Parse.cpp", positiveText);
            // 음성: 정수 strtol, 정수·문자열 printf, 주석·문자열 안의 이름, 멤버·한정 이름, 시험 소스.
            temporary.Write("Engine/Source/Developer/Fake/Private/Clean.cpp", string.Join('\n',
                "long i = strtol(text, nullptr, 10);", "snprintf(b, n, \"[%s][%u] \", name, id);", "// strtod()와 달리 정확하다",
                "/* atof(x) */ const char* s = \"atof(\";", "parser.strtod(text);", "value = Parser::atof(text);", "reader->sscanf(x);",
                "int strtodCount = 0;") + "\n");
            temporary.Write("Core/Source/Runtime/Fake/Tests/ParseTests.cpp", "double a = strtod(text, &end);\n");
            var failures = PolicyChecks.CrtNumberConversions(temporary.Root);
            var expected = Enumerable.Range(1, positives.Length).Select(line => ("Core/Source/Runtime/Fake/Private/Parse.cpp", line)).ToHashSet();
            var actual = failures.Select(failure => (failure.Path, failure.Line)).ToHashSet();
            Verify.That(actual.SetEquals(expected), "CRT number conversion lines: " + string.Join("; ", failures));
        });
        suite.Add("Policy.DeterministicMathObjectRules", () =>
        {
            // 외부 심볼: 코드 생성 지원 심볼(COFF·ELF·Mach-O 이름)은 허용, CRT 수학·다른 모듈 함수는 거부.
            foreach (string allowed in new[] { "_fltused", "__security_check_cookie", "__chkstk", "___chkstk_darwin",
                         "__stack_chk_fail", "___stack_chk_guard", "memcpy", "_memset", "_bzero", "___bzero" })
            {
                Verify.That(DeterministicMathObjectCheck.IsAllowedSymbol(allowed), "allowed symbol rejected: " + allowed);
            }

            foreach (string rejected in new[] { "sin", "_sin", "pow", "_exp2", "floor", "fmod", "__exp_finite", "sqrtf", "fma",
                         "__powidf2", "__imp_sin", "sin@GLIBC_2.2.5", "?Pow@PlatformMath@platform@rocks@@SANNN@Z",
                         "__ZN5rocks8platform12PlatformMath3PowEdd", "fltused", "__memcpy", "___cxa_atexit", "__asan_init" })
            {
                Verify.That(!DeterministicMathObjectCheck.IsAllowedSymbol(rejected), "forbidden symbol accepted: " + rejected);
            }
            // Asan 구성에서만: sanitizer 계측과 Darwin module dtor 등록.
            foreach (string sanitizer in new[] { "__asan_report_load8", "___asan_init", "___cxa_atexit", "___dso_handle", "__ubsan_handle_add_overflow" })
            {
                Verify.That(DeterministicMathObjectCheck.IsAllowedSymbol(sanitizer, sanitizer: true), "sanitizer symbol rejected: " + sanitizer);
                Verify.That(!DeterministicMathObjectCheck.IsAllowedSymbol(sanitizer), "sanitizer symbol accepted outside Asan: " + sanitizer);
            }

            Verify.That(!DeterministicMathObjectCheck.IsAllowedSymbol("_sin", sanitizer: true), "CRT symbol accepted under Asan");
            // 금지 명령: 융합 곱셈-덧셈(두 아키텍처), AArch64 근사, x86 근사·x87. 아키텍처는 "file format" 줄로 정한다.
            string arm = string.Join('\n', "probe.o:\tfile format mach-o arm64", "",
                "  14: fmadd d0, d1, d2, d3", "  18: fnmsub d0, d1, d2, d3", "  1c: fmla v0.2d, v1.2d, v2.2d",
                "  20: fmla.2d v0, v1, v2", "  24: frsqrte d0, d1", "  28: frecps s0, s1, s2", "  2c: frecpx d0, d1",
                "0000000000000030 <?Exp2@deterministic@rocks@@YANN@Z>:", "  30: fmul d0, d1, d2", "  34: fadd d0, d0, d3",
                "  38: fsqrt d0, d1", "  3c: fmov d0, #1.00000000", "  40: fmaxnm d0, d1, d2", "  44: frintn d0, d1",
                "  48: bl 0x0 <fmla>", "  4c: madd x0, x1, x2, x3");
            Verify.Equal(7, DeterministicMathObjectCheck.ForbiddenInstructions(arm).Count, "AArch64 forbidden instructions");
            string x86 = string.Join('\n', "probe.obj:\tfile format coff-x86-64", "",
                "  10: vfmadd231sd %xmm2, %xmm1, %xmm0", "  14: vfnmadd213ps %ymm1, %ymm2, %ymm3", "  18: rsqrtss %xmm1, %xmm0",
                "  1c: vrcp14pd %zmm1, %zmm0", "  20: fldt 0x10(%rsp)", "  24: fmulp %st, %st(1)",
                "  28: mulsd %xmm1, %xmm0", "  2c: addsd %xmm2, %xmm0", "  30: sqrtsd %xmm1, %xmm0", "  34: vpmadd52luq %zmm1, %zmm2, %zmm3",
                "  38: callq 0x0 <fldt_helper>", "  3c: movq %fs:0x28, %rax");
            Verify.Equal(6, DeterministicMathObjectCheck.ForbiddenInstructions(x86).Count, "x86 forbidden instructions");
            // LLVM bitcode(raw·wrapper)는 건너뛰고 COFF·Mach-O·ELF 기계어는 검사한다.
            Verify.That(DeterministicMathObjectCheck.IsBitcode([0x42, 0x43, 0xC0, 0xDE]), "raw bitcode not detected");
            Verify.That(DeterministicMathObjectCheck.IsBitcode([0xDE, 0xC0, 0x17, 0x0B]), "bitcode wrapper not detected");
            foreach (byte[] machine in new byte[][] { [0x64, 0x86, 0x05, 0x00], [0xCF, 0xFA, 0xED, 0xFE], [0x7F, 0x45, 0x4C, 0x46], [0x42, 0x43] })
            {
                Verify.That(!DeterministicMathObjectCheck.IsBitcode(machine), "machine object treated as bitcode");
            }
        });
        suite.Add("Policy.RepositoryNumberTables", () =>
        {
            // policy 명령은 CI에서 돌지 않으므로 self 시험이 생성 표의 최신성을 본다.
            NoFailures(NumberTables.Check(options.Root));
            string generated = NumberTables.Generate();
            Verify.Equal(generated, NumberTables.Generate(), "decimal table generator is not deterministic");
            using TemporaryDirectory temporary = new();
            temporary.Write(NumberTables.OutputPath, generated);
            NoFailures(NumberTables.Check(temporary.Root));
            temporary.Write(NumberTables.OutputPath, generated.Replace("0x0000000000000001u", "0x0000000000000002u"));
            Verify.That(NumberTables.Check(temporary.Root).Count == 1, "edited decimal tables were not detected");
        });
        suite.Add("Policy.UnicodeTablesDetectEdits", () =>
        {
            // 손으로 고친 생성 파일과 없는 파일을 잡는다. 원본은 저장소의 UCD를 그대로 복사한다.
            using TemporaryDirectory temporary = new();
            string library = Path.Combine(ThirdPartyManifest.CoreThirdPartyRoot, UnicodeTables.LibraryName);
            foreach (string file in Directory.EnumerateFiles(Path.Combine(options.Root, library), "*", SearchOption.AllDirectories))
            {
                string relative = Path.GetRelativePath(options.Root, file).Replace('\\', '/');
                Directory.CreateDirectory(Path.GetDirectoryName(temporary.FilePath(relative))!);
                File.Copy(file, temporary.FilePath(relative));
            }

            Verify.That(UnicodeTables.Check(temporary.Root).Count == 1, "missing generated tables escaped audit");
            Verify.That(UnicodeTables.Run(temporary.Root, write: true), "generator must write missing tables");
            NoFailures(UnicodeTables.Check(temporary.Root));
            File.AppendAllText(temporary.FilePath(UnicodeTables.OutputPath), "// edited\n");
            Verify.That(UnicodeTables.Check(temporary.Root).Count == 1, "edited generated tables escaped audit");
        });
        suite.Add("Policy.CompilerBuiltinControls", () =>
        {
            using TemporaryDirectory temporary = new();
            // 지정 헤더는 자기 계열만 쓴다. 기능 검사 매크로와 builtin이 아닌 이름은 허용한다.
            const string memory = "Core/Source/Runtime/PlatformBase/Public/Platform/Memory.h";
            const string atomics = "Core/Source/Runtime/PlatformBase/Public/Platform/Atomics.h";
            temporary.Write(memory, "inline void C(void* d, const void* s, usize n) { __builtin_memcpy(d, s, n); }\n" +
                "constexpr usize L(const char* t) { return __builtin_strlen(t); }\n");
            temporary.Write(atomics, "enum class MemoryOrder { Relaxed = __ATOMIC_RELAXED };\n" +
                "template <class T> T Load(const T* a) { T r; __atomic_load(a, &r, __ATOMIC_ACQUIRE); return r; }\n");
            temporary.Write("Core/Source/Runtime/Base/Public/Base/Compiler.h", "#define ROCKS_UNREACHABLE() __builtin_unreachable()\n");
            temporary.Write("Engine/Source/Runtime/Probe/Private/Clean.cpp",
                "#if __has_builtin(x)\n#endif\nvoid builtin_memcpy(); int my__builtin_x;\n");
            NoFailures(PolicyChecks.CompilerBuiltins(temporary.Root));

            // 지정 헤더라도 다른 계열은 위반이다.
            foreach (string source in new[] { "__atomic_fetch_add(&x, 1, 0);", "__builtin_bswap32(x);" })
            {
                temporary.Write(memory, source + "\n");
                Verify.Equal(1, PolicyChecks.CompilerBuiltins(temporary.Root).Count, "helper allowance too wide: " + source);
            }

            temporary.Write(memory, "#pragma once\n");

            // 그 밖의 소스는 테스트까지 모두 위반이다.
            foreach (string source in new[] { "__builtin_memcpy(d, s, n);", "x = __builtin_bit_cast(float, y);",
                "__atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);", "__sync_synchronize();",
                "__c11_atomic_load(&x, 0);", "int order = __ATOMIC_SEQ_CST;", "auto o = __builtin_offsetof(T, m);" })
            {
                foreach (string file in new[] { "Engine/Source/Runtime/Probe/Private/Probe.cpp",
                    "Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "Programs/Sandbox/Private/Probe.h" })
                {
                    temporary.Write(file, "// first line\n" + source + "\n");
                    var failures = PolicyChecks.CompilerBuiltins(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].Line == 2 &&
                        failures[0].ToString().Contains("compiler-builtin", StringComparison.Ordinal),
                        $"builtin escaped audit in {file}: {source}");
                    temporary.Write(file, "// clean\n");
                }
            }
        });
        suite.Add("Policy.CompilerTypeTraitControls", () =>
        {
            using TemporaryDirectory temporary = new();
            const string helper = "Core/Source/Runtime/Base/Public/Base/TypeTraits.h";
            // 검사기 시험도 통째 면제하지 않는다. 위반 토큰은 조립하고 생성된 문자열을 실제로 검사한다.
            string[] spellings = ["is_base_of", "is_future_trait", "remove_cvref", "remove_future_qualifier", "add_pointer",
                "make_integer_seq", "make_signed", "has_trivial_copy", "has_nothrow_assign", "has_virtual_destructor",
                "has_unique_object_representations", "array_rank", "array_extent", "underlying_type", "decay",
                "type_pack_element", "reference_constructs_from_temporary", "reference_converts_from_temporary"];
            string[] consumers = ["Core/Source/Runtime/Core/Public/Core/Bad.h", "Core/Source/Runtime/Core/Tests/Bad.cpp",
                "Engine/Source/Runtime/Bad/Public/Bad.hpp", "Engine/Source/Runtime/Bad/Private/Bad.inl",
                "Programs/Sandbox/Private/Bad.inc", "Programs/Scaffold/Templates/Bad.h.in",
                "Programs/Rocks.MetaCompiler/Bad.cs", "Programs/NewGenerator/Emitter.cs", "Programs/Rocks.ToolTests/Bad.cs"];
            foreach (string spelling in spellings)
            {
                string intrinsic = "__" + spelling;
                string source = $"static_assert({intrinsic}(T));";
                Verify.Equal(intrinsic, CompilerTypeTraits.FirstUse(source), "trait family not recognized");
                temporary.Write(helper, source + "\n");
                NoFailures(PolicyChecks.CompilerBuiltins(temporary.Root));
                foreach (string file in consumers)
                {
                    temporary.Write(file, "// first line\n" + source + "\n");
                    var failures = PolicyChecks.CompilerBuiltins(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].Path == file && failures[0].Line == 2 &&
                        failures[0].ToString().Contains("compiler-type-trait", StringComparison.Ordinal),
                        $"trait escaped audit in {file}: {intrinsic}");
                    temporary.Write(file, "// clean\n");
                }
            }

            // 기존 Core 헤더나 비슷한 이름의 파일은 구현 헤더 예외를 받지 않는다.
            foreach (string file in new[] { "Core/Source/Runtime/Core/Public/Core/Traits.h",
                "Core/Source/Runtime/Base/Public/Base/OtherTypeTraits.h", "Programs/Rocks.Tooling.Common/OtherCompilerTypeTraits.cs" })
            {
                temporary.Write(file, "static_assert(" + "__" + "is_same(int, int));\n");
                Verify.Equal(1, PolicyChecks.CompilerBuiltins(temporary.Root).Count, "trait helper allowance too wide: " + file);
                temporary.Write(file, "// clean\n");
            }

            foreach (string allowed in new[] { "#if __has_builtin(x)\n#endif", "#if __has_feature(x)\n#endif", "#if __is_identifier(x)\n#endif",
                "::rocks::IsBaseOfV<Base, Derived>", "::rocks::RemoveCvRefT<T>", "my" + "__" + "is_base_of" })
            {
                Verify.That(CompilerTypeTraits.FirstUse(allowed) is null, "non-trait token rejected: " + allowed);
            }

            temporary.Write(helper, "using Value = decltype(__c11_atomic_load(pointer, 0));\n");
            NoFailures(PolicyChecks.CompilerBuiltins(temporary.Root));
            temporary.Write(helper, "__builtin_memcpy(destination, source, count);\n");
            Verify.Equal(1, PolicyChecks.CompilerBuiltins(temporary.Root).Count, "traits header accepted an unrelated builtin");
        });
        suite.Add("Policy.GeneratedCppBuiltinControls", () =>
        {
            foreach (string spelling in new[] { "is_future_trait", "remove_cv", "builtin_memcpy", "atomic_load",
                "sync_synchronize", "c11_atomic_load", "ATOMIC_RELAXED" })
            {
                string intrinsic = "__" + spelling;
                // 개별 C# 조각에는 완성된 내장 이름이 없어도 합성 C++ 경계에서 검출한다.
                string generated = "static_assert(" + "__" + spelling + "(T));\n";
                Verify.Equal(intrinsic, CompilerTypeTraits.FirstGeneratedBuiltinUse(generated), "generated builtin not rejected");
                string label = "const char* label = \"" + intrinsic + "\";\n";
                string raw = "const char* raw = R\"tag(" + intrinsic + ")tag\";\n";
                string comments = "// " + intrinsic + "\n/* " + intrinsic + " */\n";
                Verify.That(CompilerTypeTraits.FirstGeneratedBuiltinUse(label + raw + comments) is null,
                    "generated text or comment mistaken for a builtin call");
                Verify.Equal(intrinsic, CompilerTypeTraits.FirstGeneratedBuiltinUse(label + raw + comments + generated),
                    "generated builtin after a literal was skipped");
                Verify.Equal(intrinsic, CompilerTypeTraits.FirstGeneratedBuiltinUse("constexpr int a = 1'000;\n" + generated +
                    "constexpr int b = 0xA'B;\n"), "digit separators mistaken for a character literal spanning the builtin");
                string include = "#include <Probe/" + intrinsic + ".h>\n";
                Verify.That(CompilerTypeTraits.FirstGeneratedBuiltinUse(include) is null,
                    "header name mistaken for a builtin call");
                Verify.Equal(intrinsic, CompilerTypeTraits.FirstGeneratedBuiltinUse(include + generated),
                    "builtin after an include was skipped");
            }

            Verify.That(CompilerTypeTraits.FirstGeneratedBuiltinUse("#if __has_builtin(x)\n#endif\n#if __is_identifier(x)\n#endif\n" +
                "static_assert(::rocks::IsSameV<int, int>);\n") is null, "public helper or feature check rejected");
        });
        suite.Add("Policy.RepositoryRawLimitLiterals", () => NoFailures(PolicyChecks.RawLimitLiterals(options.Root)));
        suite.Add("Policy.RawLimitLiteralControls", () =>
        {
            using TemporaryDirectory temporary = new();
            const string probe = "Core/Source/Runtime/Core/Private/Probe.cpp";

            // 이름 있는 한계, 비트 연산자 피연산자(마스크), 작은 폭 마스크, 해시 초기값, 실수 경계, 이름이 비슷한 매크로,
            // 주석·문자열 안의 철자, 외부 API 형식의 -1, 0이 아닌 값의 비트 반전, 시험 소스, 등록한 허용 철자는 허용한다.
            temporary.Write(probe, string.Join('\n',
                "if (slot->roots == NumericLimits<uint32>::Max()) { return; }",
                "const usize end = IndexNone; const usize top = MaxSize;",
                "const uint64 low = value & 0xFFFFFFFFu; bits |= 0x7fffffffu; x = 0xffffffffu ^ y;",
                "mask &= 0xffffffffffffffffull; const uint32 m = (a & 0x7fffffff) | 0xffffffffu & b;",
                "const uint32 b = 0xffffu; const uint8 c = 0xff; const uint64 seed = 0xcbf29ce484222325ull;",
                "const uint64 near = 0xfffffffff0ull; const uint64 high = 0xFFFFFFFF00000000ull;",
                "if (value >= -2147483648.0 && value <= 2147483647.0) { }",
                "static_assert(LZ4F_HEADER_SIZE_MAX <= 19 && FLT_MAX_EXP == 128 && PATH_MAX > 0);",
                "// UINT32_MAX·0xffffffffu·~0u·static_cast<usize>(-1)는 주석이다.",
                "const char* text = \"SIZE_MAX 4294967295\";",
                "if (ResumeThread(thread) == static_cast<DWORD>(-1)) { }",
                "const uint32 notAll = ~mask; const int64 n = ~static_cast<int64>(value); const auto f = ~0x0fu;",
                "const usize at = (end + alignof(uint32) - 1) & ~(alignof(uint32) - 1); usize s = sizeof (uint64) - 1;",
                ""));
            temporary.Write("Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "uint32 x = 0xffffffffu; usize y = SIZE_MAX;\n");
            temporary.Write("Core/Source/Runtime/PlatformBase/Public/Platform/Math/VectorRegisterSse.h",
                "return _mm_and_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)));\n");
            NoFailures(PolicyChecks.RawLimitLiterals(temporary.Root));

            // 날 철자는 런타임·도구 소스 어디서든 위반이다. 허용 파일이라도 등록하지 않은 철자는 위반이다. &&·||의
            // 피연산자는 비트 마스크가 아니다.
            foreach (string source in new[] { "if (slot->roots == 0xffffffffu) { }", "x = 0xFFFFFFFF;",
                "const uint64 max = 0xffffffffffffffffull;", "if (n > 0x7fffffff) { }", "y = 0x7FFFFFFFFFFFFFFFLL;",
                "if (v >= 2147483647ll) { }", "z = 4294967295u;", "w = 18446744073709551615ull;", "q = 9223372036854775807;",
                "if (a && 0x7fffffff) { }", "if (a || 0xffffffffu) { }", "u = UINT32_MAX;", "s = SIZE_MAX;", "i = INT_MAX;",
                "j = INT32_MIN;", "f = FLT_MAX;", "d = -DBL_MAX;", "p = SSIZE_MAX;", "l = LLONG_MIN;",
                "k = static_cast<usize>(-1);", "k = static_cast< uint64 >( - 1 );", "k = static_cast<unsigned long long>(-1);",
                "k = usize(-1);", "k = uint32{-1};", "k = (size_t)-1;", "k = ~0u;", "k = ~0ULL;", "k = ~0;", "k = ~usize(0);",
                "k = ~uint64{0};", "k = ~uint32{};", "k = ~static_cast<uint32>(0);" })
            {
                foreach (string file in new[] { probe, "Engine/Source/Runtime/Probe/Private/Probe.h",
                    "Programs/Sandbox/Private/Probe.cpp", "Core/Source/Developer/Probe/Private/Probe.cpp",
                    "Core/Source/Runtime/PlatformBase/Public/Platform/Math/VectorRegisterSse.h" })
                {
                    temporary.Write(file, "// first line\n" + source + "\n");
                    var failures = PolicyChecks.RawLimitLiterals(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].Line == 2 &&
                        failures[0].ToString().Contains("raw-limit-literal", StringComparison.Ordinal),
                        $"raw limit literal escaped audit in {file}: {source}");
                    temporary.Write(file, "// clean\n");
                }
            }
        });
        suite.Add("Policy.RepositoryCompileTimeMessages", () => NoFailures(PolicyChecks.CompileTimeMessages(options.Root)));
        suite.Add("Policy.CompileTimeMessageControls", () =>
        {
            using TemporaryDirectory temporary = new();
            const string probe = "Core/Source/Runtime/Core/Private/Probe.cpp";

            // 영어 메시지, 메시지 없는 static_assert, 한국어 주석(문장 안·위·#error 줄 끝), 괄호를 담은 영어 문자열,
            // 다른 문자열 안의 한국어는 허용한다.
            temporary.Write(probe, string.Join('\n',
                "static_assert(sizeof(int) == 4, \"int must be 32-bit\");",
                "static_assert(IsTrivialV<T>); // 한국어 주석",
                "// static_assert(false, \"한국어\")는 주석이다.",
                "static_assert(Check(\")\"), \"English (with parentheses)\"); /* 설명 */",
                "#error \"Rocks requires C++23\" // 한국어 주석",
                "const char* text = \"한국어 문자열\";",
                "static_assert(sizeof(long) == 8,",
                "              \"long must be 64-bit\");",
                ""));
            NoFailures(PolicyChecks.CompileTimeMessages(temporary.Root));

            // 한국어 메시지는 static_assert(여러 줄 포함)·#error(줄 이음 포함) 어디서든 위반이다.
            foreach (string source in new[] { "static_assert(A, \"한국어 메시지\");",
                "static_assert(sizeof(T) > 0, \"T는 완전한 타입이어야 한다\");", "#error \"Rocks는 Clang만 지원한다\"",
                "#  error 한국어 문구", "#error \\\n    \"정확히 하나가 1이어야 한다\"" })
            {
                foreach (string file in new[] { probe, "Core/Source/Runtime/Core/Tests/ProbeTests.cpp",
                    "Engine/Source/Runtime/Probe/Public/Probe.h", "Programs/Sandbox/Private/Probe.cpp" })
                {
                    temporary.Write(file, "// first line\n" + source + "\n");
                    var failures = PolicyChecks.CompileTimeMessages(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].Line == 2 &&
                        failures[0].ToString().Contains("compile-time-message-english", StringComparison.Ordinal),
                        $"Korean compile-time message escaped audit in {file}: {source}");
                    temporary.Write(file, "// clean\n");
                }
            }
        });
        suite.Add("Policy.RepositoryInitializerListDeductions",
            () => NoFailures(PolicyChecks.InitializerListDeductions(options.Root)));
        suite.Add("Policy.InitializerListDeductionControls", () =>
        {
            using TemporaryDirectory temporary = new();
            const string probe = "Core/Source/Runtime/Core/Tests/ProbeTests.cpp";

            // 지역 배열 범위, 일반 for 문, 같은 줄의 case 라벨 블록, 형식 이름이 붙은 중괄호 임시값, auto 직접 초기화,
            // 주석·문자열 안의 철자는 허용한다.
            temporary.Write(probe, string.Join('\n',
                "const bool cases[] = {true, false}; for (bool x : cases) { }",
                "for (usize i = 0; i < n; ++i) { }",
                "for (auto& item : items) { switch (item) { case 1: { break; } } }",
                "for (const Pair& pair : Span<const Pair>{pairs, 2}) { }",
                "auto value{1}; auto copy = value; const auto view = Span<const int>{data, 2};",
                "// for (bool x : {true, false})와 auto x = {1, 2}는 주석이다.",
                "const char* text = \"for (int v : {1, 2}) auto y = {3};\";",
                ""));
            NoFailures(PolicyChecks.InitializerListDeductions(temporary.Root));

            // 중괄호 목록 범위와 auto 복사 초기화는 런타임·도구·시험 소스 어디서든 위반이다.
            foreach (string source in new[] { "for (bool hasBase : {false, true}) {",
                "for (ObjectGraphStage* stage : {&first, &second}) {", "for(int v:{1,2}){}",
                "for (const auto& [key, value] : {Pair{1, 2}}) {", "auto values = {1, 2, 3};",
                "const auto& list = {a, b};", "auto* pointers = {&a};" })
            {
                foreach (string file in new[] { probe, "Core/Source/Runtime/Core/Private/Probe.cpp",
                    "Engine/Source/Runtime/Probe/Public/Probe.h", "Programs/Sandbox/Private/Probe.cpp" })
                {
                    temporary.Write(file, "// first line\n" + source + "\n");
                    var failures = PolicyChecks.InitializerListDeductions(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].Line == 2 &&
                        failures[0].ToString().Contains("initializer-list-deduction", StringComparison.Ordinal),
                        $"initializer_list deduction escaped audit in {file}: {source}");
                    temporary.Write(file, "// clean\n");
                }
            }
        });
        suite.Add("Policy.RepositoryPythonFreeMainPath", () => NoFailures(PolicyChecks.PythonFreeMainPath(options.Root)));
        suite.Add("Policy.RuntimePositiveAndNegativeControls", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Core/Source/Runtime/Core/Public/Probe.h", "#pragma once\nnamespace rocks { class Array; }\n");
            NoFailures(PolicyChecks.Runtime(temporary.Root));
            foreach (string banned in new[] { "#include <vector>", "# include \"string\"", "std :: vector<int> value;", "#include <filesystem>" })
            {
                temporary.Write("Core/Source/Runtime/Core/Public/Probe.h", "// first line\n" + banned + "\n");
                var failures = PolicyChecks.Runtime(temporary.Root);
                Verify.Equal(1, failures.Count, "runtime negative count");
                Verify.Equal(2, failures[0].Line, "diagnostic line");
            }
        });
        suite.Add("Policy.RuntimeExcludesModuleTestsButCoversProgramsAndPlugins", () =>
        {
            // 모듈 Tests/와 테스트 runner는 STL 정책 밖이다(STL 호환 테스트가 STL을 쓴다).
            using TemporaryDirectory temporary = new();
            temporary.Write("Core/Source/Runtime/Core/Tests/StlProbe.cpp", "#include <vector>\n");
            temporary.Write("Programs/FoundationTests/Private/StlProbe.cpp", "#include <vector>\n");
            NoFailures(PolicyChecks.Runtime(temporary.Root));
            // 플러그인과 프로그램의 런타임 소스는 검사한다.
            temporary.Write("Engine/Plugins/Sample/Example/Source/Example/Private/Probe.cpp", "#include <vector>\n");
            temporary.Write("Programs/Commandlet/Private/Common/Probe.cpp", "#include <map>\n");
            Verify.Equal(2, PolicyChecks.Runtime(temporary.Root).Count, "plugin or program STL escaped runtime policy");
        });
        // 리플렉션 배열 컨테이너의 명시적 인스턴스화·extern template은 Clang이 필드를 작성 철자로 출력하게 할 수 있으므로
        // 시험·Developer 소스를 포함해 거부한다(Docs/Design/ReflectionFoundations.ko.md 5.6절). 특수화·일반 사용은 받는다.
        suite.Add("Policy.RuntimeRejectsReflectionContainerInstantiation", () =>
        {
            using TemporaryDirectory temporary = new();
            const string probe = "Core/Source/Developer/Fixtures/Public/Probe.h";
            // 특수화·부분 특수화·일반 사용, 이름만 비슷한 타입, 주석과 문자열·raw 문자열 안의 예시는 받는다.
            temporary.Write(probe, "#pragma once\ntemplate <> struct ArrayField<rocks::Array<int>> {};\n" +
                "template <class T> class Array<T*> {};\nrocks::Array<int> values;\ntemplate class ValueArray<int>;\n" +
                "rocks::WeakRef<Node> weak;\ntemplate class WeakRefHolder<int>;\nrocks::ClassRef<Node> kind;\n" +
                "// template class rocks::Array<int>;\n/* extern template\n   class rocks::ObjectRef<Node>; */\n" +
                "const char* text = \"template class rocks::Array<int>;\";\nconst char* raw = R\"x(template class rocks::SoftObjectRef<Node>;)x\";\n" +
                "constexpr int big = 1'000; const char quote = '\"';\n");
            NoFailures(PolicyChecks.Runtime(temporary.Root));
            foreach (string banned in new[] { "template class rocks::Array<int>;", "extern template class rocks::Array<rocks::String>;",
                "template struct ::rocks::ObjectRefArray<Node>;", "extern  template  class StructRefArray<Slot>;", "template class Array <float>;",
                "template class rocks::ObjectRef<Node>;", "extern template class rocks::SoftObjectRef<>;",
                "template class rocks::WeakRef<Node>;", "extern template class ::rocks::WeakRef<>;",
                "template class rocks::ClassRef<Node>;", "extern template struct ::rocks::ClassRef<Node>;",
                "template class rocks::ObjectRefMap<int, Node>;", "extern template class ::rocks::ObjectRefMap<rocks::Name>;",
                "extern template\n    class\n    rocks::Array<int>;", "template class /* note */ rocks::StructRefArray<Slot>;" })
            {
                temporary.Write(probe, "// first line\n" + banned + "\n");
                var failures = PolicyChecks.Runtime(temporary.Root);
                Verify.Equal(1, failures.Count, "container instantiation escaped audit: " + banned);
                Verify.Equal("reflection-container-instantiation", failures[0].Rule, "container instantiation rule");
                Verify.Equal(2, failures[0].Line, "container instantiation line");
            }

            // 시험 소스도 검사한다(리플렉션 헤더가 시험 모듈에 있을 수 있다).
            temporary.Write(probe, "#pragma once\n");
            temporary.Write("Core/Source/Runtime/Core/Tests/Probe.cpp", "template class rocks::Array<int>;\n");
            Verify.Equal(1, PolicyChecks.Runtime(temporary.Root).Count, "test source container instantiation escaped audit");
        });
        // MetaCompiler가 보지 않는 리플렉션 필드 annotation — ROCKS_NATIVE_TYPE 클래스 안, 어느 모듈 Metadata에도 없는 파일 — 은
        // attribute로만 남아 GC가 추적하지도 저장하지도 않으므로 거부하고, 읽을 수 없는(리터럴이 아닌) Metadata 목록은 규칙 파일
        // 위치에 보고한다(Docs/Design/ReflectionFoundations.ko.md 9.1절 B8a).
        suite.Add("Policy.ReflectionAnnotationsRejectNativeTypeAndUnlisted", () =>
        {
            using TemporaryDirectory temporary = new();
            const string module = "Core/Source/Developer/Fixtures";
            const string game = "Templates/Game/Sample";
            const string automatic = "Core/Source/Developer/AutoProperty";
            const string dynamic = "Core/Source/Developer/Dynamic";
            // 목록은 SourceModuleRules의 `Metadata =>`, 게임 모듈 초기화의 `Metadata =`, 자동 프로퍼티 `{ get; } =`의 문자열
            // 리터럴이다. 규칙 파일 주석 안 목록, 목록 안 줄·블록 주석 항목, ImportedMetadata는 항목이 아니다.
            temporary.Write(module + "/Fixtures.Build.cs",
                "public sealed class Fixtures : CoreModuleRules\n{\n    // Metadata => [\"Fixtures/Commented.h\"];\n" +
                "    protected override IReadOnlyList<string> Metadata =>\n        [\"Fixtures/Listed.h\", // \"Fixtures/LineComment.h\"\n" +
                "            /* \"Fixtures/BlockComment.h\" */ \"Fixtures/Native.h\"];\n}\n");
            temporary.Write(game + "/Game.Build.cs", "Metadata = [\"GameObject.h\"],\nImportedMetadata = [\"Other.h\"],\n");
            temporary.Write(automatic + "/AutoProperty.Build.cs",
                "protected override IReadOnlyList<string> Metadata { get; } = [\"AutoProperty/Auto.h\",];\n");
            const string dynamicRules = dynamic + "/Dynamic.Build.cs";
            void WriteDynamic(string definition) => temporary.Write(dynamicRules,
                "public sealed class Dynamic : CoreModuleRules\n{\n    protected override IReadOnlyList<string> Metadata" + definition + "\n}\n");
            WriteDynamic(" => [\"Dynamic/Value.h\"];");
            // 목록에 있는 헤더의 리플렉션 클래스·중첩 struct, 매크로 정의(이어진 줄 포함)·주석·문자열 안의 이름은 받는다.
            const string reflected = "#pragma once\nROCKS_CLASS()\nstruct Listed : rocks::Object {\n    ROCKS_PROPERTY() int value = 0;\n" +
                "    ROCKS_NATIVE() int cache = 0;\n    ROCKS_STRUCT()\n    struct Inner {\n        ROCKS_PROPERTY() int x = 0;\n    };\n};\n";
            foreach (string file in new[] { module + "/Public/Fixtures/Listed.h", game + "/Source/GameObject.h",
                automatic + "/Public/AutoProperty/Auto.h", dynamic + "/Public/Dynamic/Value.h" })
            {
                temporary.Write(file, reflected);
            }

            temporary.Write("Core/Source/Runtime/MetaSchema/Public/Meta/Annotations.h",
                "#define ROCKS_PROPERTY(...) [[clang::annotate(\"rocks\")]]\n#define ROCKS_NATIVE(...) \\\n    ROCKS_PROPERTY()\n" +
                "// 사용 예: ROCKS_PROPERTY() int x;\nconst char* text = \"ROCKS_NATIVE()\";\n");
            // native 클래스의 일반 멤버, native 클래스 안 중첩 struct의 멤버(중첩 struct로 판정), 같은 파일에서 native 클래스 본문
            // 밖의 리플렉션 클래스, 본문 주석에만 ROCKS_NATIVE_TYPE이 있는 클래스는 받는다.
            const string native = "#pragma once\nclass Native : public rocks::Object {\n    ROCKS_NATIVE_TYPE(Native, rocks::Object, 1ull, 2ull)\n\n" +
                "public:\n    void Trace(rocks::TraceVisitor&) const noexcept {\n    }\n\n    int state = 0;\n};\n";
            string nested = native.Replace("    int state = 0;\n",
                "    int state = 0;\n    struct Inner {\n        ROCKS_PROPERTY() int x = 0;\n    };\n", StringComparison.Ordinal);
            temporary.Write(module + "/Public/Fixtures/Native.h", nested +
                "ROCKS_CLASS()\nstruct Reflected : rocks::Object {\n    ROCKS_PROPERTY() int value = 0;\n};\n" +
                "struct Commented : rocks::Object {\n    // ROCKS_NATIVE_TYPE(Commented, rocks::Object, 3ull, 4ull)\n    ROCKS_PROPERTY() int value = 0;\n};\n");
            NoFailures(PolicyChecks.ReflectionAnnotations(temporary.Root));

            // native 클래스 본문 안(멤버 함수 본문 뒤 포함)의 annotation은 목록에 있는 헤더여도 native-type이다.
            temporary.Write(module + "/Public/Fixtures/Native.h", native.Replace("    int state = 0;\n",
                "    int state = 0;\n    ROCKS_PROPERTY() rocks::ObjectRef<Native> next;\n    ROCKS_NATIVE() int cache = 0;\n", StringComparison.Ordinal));
            var nativeType = PolicyChecks.ReflectionAnnotations(temporary.Root);
            Verify.That(nativeType.Count == 2 && nativeType.All(failure => failure.Rule == "reflection-annotation-native-type") &&
                nativeType[0].Line == 10 && nativeType[1].Line == 11, "native class annotations: " + string.Join("; ", nativeType));
            temporary.Write(module + "/Public/Fixtures/Native.h", native);

            // 목록 밖 파일: 시험 .cpp의 리플렉션 클래스, 같은 모듈의 목록에 없는 헤더, 규칙 파일 주석·목록 안 주석에만 있는 철자,
            // 다른 모듈 폴더의 같은 철자, ImportedMetadata 철자. .cpp 안 native 클래스의 annotation은 native-type 하나로만 보고하고
            // 그 안 중첩 struct의 annotation은 (목록 밖이므로) unlisted다.
            string[] unlisted = [module + "/Public/Fixtures/Unlisted.h", module + "/Public/Fixtures/Commented.h",
                module + "/Public/Fixtures/LineComment.h", module + "/Public/Fixtures/BlockComment.h",
                "Core/Source/Developer/Other/Public/Fixtures/Listed.h", game + "/Source/Other.h", "Core/Source/Runtime/Core/Tests/ProbeTests.cpp"];
            foreach (string file in unlisted)
            {
                temporary.Write(file, "// first line\n" + reflected[reflected.IndexOf("ROCKS_CLASS", StringComparison.Ordinal)..]);
            }

            const string probe = "Engine/Source/Runtime/Probe/Tests/ProbeTests.cpp";
            temporary.Write(probe, nested.Replace("    int state = 0;\n", "    ROCKS_PROPERTY() int state = 0;\n", StringComparison.Ordinal));
            var expected = unlisted.SelectMany(file => new[] { 4, 5, 8 }.Select(line => (file, line, "reflection-annotation-unlisted")))
                .Append((probe, 9, "reflection-annotation-native-type")).Append((probe, 11, "reflection-annotation-unlisted")).ToHashSet();
            void Expect(HashSet<(string, int, string)> wanted, string label)
            {
                var failures = PolicyChecks.ReflectionAnnotations(temporary.Root);
                var actual = failures.Select(failure => (failure.Path, failure.Line, failure.Rule)).ToHashSet();
                Verify.That(failures.Count == wanted.Count && actual.SetEquals(wanted), label + ": " + string.Join("; ", failures));
            }

            Expect(expected, "reflection annotation failures");

            // 리터럴 목록이 아닌 Metadata는 규칙 파일의 Metadata 줄에 보고하고, 그 모듈 헤더는 unlisted로 오해하지 않는다.
            foreach (string definition in new[] { " => Headers;", " => new[] { \"Dynamic/Value.h\" };", " => [.. Shared, \"Dynamic/Value.h\"];",
                " => [\"Dynamic/Value.h\"].ToArray();", " => [$\"{Name}/Value.h\"];", " { get => Headers; }", " => MetadataOf(\"Dynamic\");" })
            {
                WriteDynamic(definition);
                Expect([.. expected, (dynamicRules, 3, "reflection-metadata-not-literal")], "non-literal Metadata" + definition);
            }

            WriteDynamic(" { get; init; } = [\"Dynamic/Value.h\"];");
            Expect(expected, "auto-property literal Metadata");
        });
        suite.Add("Policy.RuntimeTupleProtocolAllowanceIsNarrow", () =>
        {
            using TemporaryDirectory temporary = new();
            const string tuple = "Core/Source/Runtime/Core/Public/Core/Tuple.h";
            temporary.Write(tuple, "#pragma once\n#include <utility>\n" +
                "template <typename... T> struct std::tuple_size<rocks::Tuple<T...>> {};\n" +
                "template <decltype(sizeof(0)) I, typename... T> struct std :: tuple_element<I, rocks::Tuple<T...>> {};\n");
            NoFailures(PolicyChecks.Runtime(temporary.Root));
            // 같은 파일이라도 다른 std 사용·다른 STL 헤더는 위반이다.
            foreach (string banned in new[] { "std::vector<int> value;", "#include <tuple>",
                "struct std::tuple_size<X> : std::integral_constant<int, 1> {};" })
            {
                temporary.Write(tuple, "#pragma once\n" + banned + "\n");
                Verify.Equal(1, PolicyChecks.Runtime(temporary.Root).Count, "tuple allowance too wide: " + banned);
            }
            // 다른 파일에서는 tuple protocol 이름과 <utility>도 위반이다.
            temporary.Write(tuple, "#pragma once\n");
            temporary.Write("Core/Source/Runtime/Core/Public/Core/Other.h", "#include <utility>\nstruct std::tuple_size<int>;\n");
            Verify.Equal(2, PolicyChecks.Runtime(temporary.Root).Count, "tuple allowance leaked to other file");
        });
        suite.Add("Policy.PlatformPreciseBackendBoundary", () =>
        {
            using TemporaryDirectory temporary = new();
            foreach (string source in new[] { "#if defined(_WIN32)\n#endif", "#include <Windows.h>", "#include <pthread.h>", "void f(){CreateThread();}", "void f(){nanosleep();}" })
            {
                temporary.Write("Core/Source/Runtime/Core/Tests/Probe.cpp", source);
                Verify.That(PolicyChecks.PlatformBoundary(temporary.Root).Count != 0, "public platform regression accepted");
            }

            temporary.Write("Core/Source/Runtime/Core/Tests/Probe.cpp", "#pragma once\n");
            temporary.Write("Core/Source/Runtime/PlatformBase/Private/Win64/Probe.cpp", "#include <Windows.h>\n");
            temporary.Write("Core/Source/Developer/TestSupport/Private/Mac/Probe.mm", "#include <pthread.h>\n");
            NoFailures(PolicyChecks.PlatformBoundary(temporary.Root));
            temporary.Write("Core/Source/Runtime/PlatformBase/Private/Win64Wrong/Probe.cpp", "#include <Windows.h>\n");
            Verify.Equal(1, PolicyChecks.PlatformBoundary(temporary.Root).Count, "prefix lookalike must not be allowlisted");
        });
        suite.Add("Policy.TargetMacroControls", () =>
        {
            using TemporaryDirectory temporary = new();
            // 정의 헤더는 원시 매크로로 ROCKS_ARCH_*·ROCKS_PLATFORM_*를 정하고 검증한다. 두 검사 모두 허용한다.
            temporary.Write("Core/Source/Runtime/Base/Public/Base/Compiler.h",
                "#if defined(__x86_64__) || defined(_M_X64)\n#endif\n#if ROCKS_PLATFORM_MAC && !defined(__APPLE__)\n#endif\n");
            // Compiler.h를 먼저 포함하면 ROCKS_ARCH_*·기능 매크로·if constexpr 상수는 어디서나 쓸 수 있다. 주석의 언급은 코드가 아니다.
            const string probe = "Core/Source/Runtime/Core/Tests/Probe.cpp";
            temporary.Write(probe, "#include <Base/Compiler.h>\n// __APPLE__ is decided by Compiler.h\n#if ROCKS_ARCH_ARM64 && \\\n" +
                "    ROCKS_PLATFORM_MAC\n#endif\n#if defined(__ARM_FEATURE_AES)\n#endif\nif constexpr (rocks::IsMac) {}\n");
            NoFailures(PolicyChecks.TargetMacros(temporary.Root));
            NoFailures(PolicyChecks.PlatformBoundary(temporary.Root));

            foreach (string source in new[] { "#if defined(__aarch64__)\n#endif", "#ifdef _M_X64\n#endif", "#elif defined(__APPLE__)",
                "#if !defined(_WIN32)\n#endif", "#if __ANDROID__ && 1\n#endif", "#ifndef __linux__\n#endif",
                // 줄 이음·C++23 #elifdef·변형 철자·빌드가 넣는 WIN32·Apple SDK 매크로·#define 별칭도 잡는다.
                "#if ROCKS_PLATFORM_MAC && \\\n    defined(__aarch64__)\n#endif", "#if 0\n#elifdef __APPLE__\n#endif", "#if __arm64\n#endif",
                "#ifdef WIN32\n#endif", "#if TARGET_OS_IPHONE\n#endif", "#define IS_APPLE __APPLE__", "#if __MACH__\n#endif" })
            {
                // native backend도 원시 매크로 대신 ROCKS_ARCH_*·ROCKS_PLATFORM_*를 쓴다.
                temporary.Write("Core/Source/Runtime/PlatformBase/Private/Android/Probe.cpp", "#include <Base/Compiler.h>\n" + source + "\n");
                Verify.Equal(1, PolicyChecks.TargetMacros(temporary.Root).Count, "raw target macro accepted: " + source);
            }

            temporary.Write("Core/Source/Runtime/PlatformBase/Private/Android/Probe.cpp", "#pragma once\n");
            // 항상 정의된 Rocks 매크로를 정의 여부로 묻는 형태와, Compiler.h보다 앞선 사용은 거부한다.
            foreach (string source in new[] { "#include <Base/Compiler.h>\n#ifdef ROCKS_PLATFORM_MAC\n#endif",
                "#include <Base/Compiler.h>\n#if defined(ROCKS_BUILD_DEBUG)\n#endif", "#include <Base/Compiler.h>\n#if 1 && \\\n defined ROCKS_ARCH_X64\n#endif",
                "#if ROCKS_ARCH_X64\n#endif\n#include <Base/Compiler.h>", "#include <Base/Types.h>\n#if ROCKS_PLATFORM_ANDROID\n#endif" })
            {
                temporary.Write(probe, source + "\n");
                Verify.Equal(1, PolicyChecks.TargetMacros(temporary.Root).Count, "Rocks target macro misuse accepted: " + source);
            }

            // 정의 헤더와 이름만 비슷한 경로는 예외가 아니다.
            temporary.Write(probe, "#pragma once\n");
            temporary.Write("Core/Source/Runtime/Base/Public/Base/CompilerExtra.h", "#if defined(__x86_64__)\n#endif\n");
            Verify.Equal(1, PolicyChecks.TargetMacros(temporary.Root).Count, "definition header allowance too wide");

            // 정의 헤더라도 OS 헤더·API는 platform-boundary가 막는다(모든 번역 단위가 포함한다).
            temporary.Write("Core/Source/Runtime/Base/Public/Base/Compiler.h", "#if ROCKS_PLATFORM_WINDOWS && !defined(_WIN32)\n#endif\n#include <Windows.h>\n");
            Verify.Equal(1, PolicyChecks.PlatformBoundary(temporary.Root).Count, "OS header accepted in Compiler.h");
        });
        suite.Add("Policy.TargetIntrinsicControls", () =>
        {
            using TemporaryDirectory temporary = new();
            const string compiler = "Core/Source/Runtime/Base/Public/Base/Compiler.h";
            const string aes = "Core/Source/Runtime/Crypto/Private/Aes/AesKernels.cpp";
            const string sha = "Core/Source/Runtime/Core/Private/Encoding/Sha256Hardware.cpp";
            const string probe = "Engine/Source/Runtime/Probe/Private/Probe.cpp";
            const string test = "Core/Source/Runtime/Crypto/Tests/ProbeTests.cpp";
            const string math = "Core/Source/Runtime/PlatformBase/Public/Platform/Math/ClangPlatformMath.h";
            // 정의 헤더는 target 속성으로 매크로를 정의하고, 계열 디렉터리는 자기 계열의 매크로·intrinsics를 쓴다. 주석·리터럴(raw
            // 포함)의 언급, 범용 SIMD의 비슷한 이름, 이름만 비슷한 식별자, target이라는 이름의 함수·속성 인자 안의 호출은 대상이 아니다.
            temporary.Write(compiler, "#define ROCKS_TARGET_X64_AES __attribute__((target(\"aes,avx,pclmul\")))\n" +
                "#define ROCKS_TARGET_ARM64_AES __attribute__((target(\"neon,aes,fuse-aes\")))\n");
            temporary.Write(aes, "ROCKS_TARGET_X64_AES static __m128i R(__m128i b, __m128i k) noexcept {\n" +
                "    return _mm_xor_si128(_mm_aesenc_si128(b, k), _mm_clmulepi64_si128(b, k, 0x11));\n}\n" +
                "ROCKS_TARGET_ARM64_AES uint8x16_t A(uint8x16_t b, uint8x16_t k) { return vaesmcq_u8(vaeseq_u8(b, k)); }\n" +
                "poly128_t M(poly64_t a, poly64_t b) { return vmull_high_p64(a, b) ^ vmull_p64(a, b); }\n");
            temporary.Write(sha, "uint32x4_t H(uint32x4_t a, uint32x4_t b, uint32x4_t w) { return vsha256hq_u32(a, b, w); }\n" +
                "__m128i S(__m128i a, __m128i b, __m128i k) { return _mm_sha256rnds2_epu32(a, b, k); }\n");
            temporary.Write(test, "// _mm_aesenc_si128, vsha256hq_u32, __attribute__((target(\"aes\"))) are only named here\n" +
                "const char* names = \"_mm_aesenc_si128 ROCKS_TARGET_X64_AES #pragma clang attribute\";\n" +
                "const char* attribute = \"__attribute__((target(\\\"aes\\\"))) [[gnu::target(\\\"aes\\\")]]\";\n" +
                "const char* raw = R\"(\n#pragma clang attribute push(X, apply_to = function)\n_Pragma(\"clang attribute pop\")\n)\";\n" +
                "__m128i X(__m128i a, __m128i b) { return _mm_shldi_epi64(_mm_shuffle_epi8(a, b), b, 1); }\n" +
                "poly16x8_t P(poly8x16_t a, poly8x16_t b) { return vmull_high_p8(a, b); }\n" +
                "uint8x16_t Q(uint8x16_t a, uint8x16_t b) { return veorq_u8(a, b); }\n" +
                "int my_mm_aesenc_si128 = 0; int target(int); int t = target(1);\n" +
                "[[nodiscard]] int target2(int); __attribute__((noinline)) void target(long);\n" +
                "void Assume(int x) { [[assume(target(x) > 0)]]; }\n");
            temporary.Write(probe, "void Clean();\n");
            temporary.Write(math, "inline int Count(unsigned x) { return __builtin_popcount(x); }\n");
            NoFailures(PolicyChecks.TargetIntrinsics(temporary.Root));

            // target 속성을 직접 거는 형태(여러 줄·줄 이음·_Pragma·__pragma 포함)는 계열 디렉터리와 시험 소스를 포함해 Compiler.h 밖
            // 어디서나 거부한다. #pragma clang attribute는 내용과 관계없이 거부한다.
            foreach (string source in new[] { "__attribute__((target(\"aes\"))) void F();",
                "__attribute__ (( __target__ (\"avx2\") )) void F();", "__attribute__((__target__(\"aes\"))) void F();",
                "__attribute__((noinline, target(\"sse4.2\"))) void F();", "__attribute__((section(\"x\"), target(\"aes\"))) void F();",
                "[[gnu::target(\"aes\")]] void F();", "[[clang::target(\"avx2\")]] void F();", "[[using gnu: target(\"aes\")]] void F();",
                "__attribute__((target_clones(\"avx2\", \"default\"))) void F();", "__attribute__((target_version(\"sve\"))) void F();",
                "__attribute__((cpu_specific(haswell))) void F();", "[[clang::cpu_dispatch(generic, haswell)]] void F();",
                "__declspec(cpu_specific(westmere)) void F();",
                "#pragma clang attribute push(__attribute__((target(\"aes\"))), apply_to = function)",
                "#pragma clang attribute push(ROCKS_X, apply_to = function)", "#pragma clang attribute pop",
                "#  pragma GCC target(\"aes\")", "#pragma GCC push_options",
                "_Pragma(\"clang attribute push(__attribute__((target(\\\"aes\\\"))), apply_to = function)\")",
                "__pragma(clang attribute push(X, apply_to = function))",
                "__attribute__((noinline, nodebug,\n               target(\"aes,pclmul\"))) void F();",
                "[[gnu::noinline,\n  gnu::target(\"aes\")]] void G();",
                "#define ROCKS_PUSH_AES \\\n    _Pragma(        \\\n        \"clang attribute push(X, apply_to = function)\")" })
            {
                foreach (string file in new[] { probe, test, aes })
                {
                    string original = File.ReadAllText(temporary.FilePath(file));
                    temporary.Write(file, original + source + "\n");
                    int expectedLine = original.Count(character => character == '\n') + 1;
                    var failures = PolicyChecks.TargetIntrinsics(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].Rule == "target-attribute" &&
                        (failures[0].Line == expectedLine || source.StartsWith("#define", StringComparison.Ordinal)),
                        $"target attribute escaped audit in {file} (expected line {expectedLine}): {source}\n{string.Join('\n', failures)}");
                    temporary.Write(file, original);
                }
            }

            // 계열의 target 매크로·intrinsics(builtin 형태 포함)는 그 계열 디렉터리 밖(시험·이름만 비슷한 디렉터리·다른 계열 디렉터리·
            // builtin을 모두 허용받은 PlatformMath 헤더)에서 거부한다.
            string[] aesFamily = ["ROCKS_TARGET_X64_AES void F();", "ROCKS_TARGET_ARM64_AES void F();", "x = _mm_aesenc_si128(a, k);",
                "x = _mm_aeskeygenassist_si128(k, 1);", "x = _mm256_aesenc_epi128(a, k);", "x = _mm_clmulepi64_si128(a, b, 0);",
                "x = _mm512_clmulepi64_epi128(a, b, 0);", "x = _mm_aesenc128kl_u8(&o, a, h);", "_mm_loadiwkey(c, a, b, d);",
                "x = _mm_encodekey256_u32(c, a, b, h);", "x = vaeseq_u8(a, k);", "x = vaesdq_u8(a, k);", "x = vaesimcq_u8(a);",
                "x = vmull_p64(a, b);", "x = vmull_high_p64(a, b);", "x = __builtin_ia32_aesenc128(a, k);",
                "x = __builtin_ia32_pclmulqdq128(a, b, 0);", "x = __builtin_neon_vaeseq_u8(a, k);"];
            string[] shaFamily = ["x = _mm_sha256rnds2_epu32(a, b, k);", "x = _mm_sha1msg1_epu32(a, b);", "x = vsha256hq_u32(a, b, w);",
                "x = vsha1cq_u32(a, e, w);", "x = vsha512hq_u64(a, b, c);", "x = veor3q_u8(a, b, c);", "x = vrax1q_u64(a, b);",
                "x = vsm3ss1q_u32(a, b, c);", "x = __builtin_ia32_sha256rnds2(a, b, k);", "x = __builtin_neon_vsha256hq_u32(a, b, w);"];
            string lookalike = "Core/Source/Runtime/Crypto/Private/AesExtra/Probe.cpp";
            foreach ((string[] sources, string[] files) in new[] { (aesFamily, new[] { probe, test, lookalike, sha, math }),
                (shaFamily, new[] { probe, test, aes, math }) })
            {
                foreach (string source in sources)
                {
                    foreach (string file in files)
                    {
                        string original = File.Exists(temporary.FilePath(file)) ? File.ReadAllText(temporary.FilePath(file)) : "";
                        temporary.Write(file, original + source + "\n");
                        var failures = PolicyChecks.TargetIntrinsics(temporary.Root);
                        Verify.That(failures.Count == 1 && failures[0].Rule == "target-intrinsics",
                            $"intrinsic escaped audit in {file}: {source}\n{string.Join('\n', failures)}");
                        temporary.Write(file, original);
                    }
                }
            }

            // 어느 계열에도 속하지 않은 ROCKS_TARGET_* 매크로는 정의(Compiler.h)·사용 모두 거부한다(계열 디렉터리 안에서도).
            foreach (string source in new[] { "ROCKS_TARGET_X64_SHA void G();", "ROCKS_TARGET_X64_AES_VAES void F();" })
            {
                foreach (string file in new[] { probe, aes, sha })
                {
                    string original = File.ReadAllText(temporary.FilePath(file));
                    temporary.Write(file, original + source + "\n");
                    var failures = PolicyChecks.TargetIntrinsics(temporary.Root);
                    Verify.That(failures.Count == 1 && failures[0].Rule == "target-intrinsics" &&
                        failures[0].Message.Contains("not assigned", StringComparison.Ordinal),
                        $"unassigned target macro accepted in {file}: {source}\n{string.Join('\n', failures)}");
                    temporary.Write(file, original);
                }
            }

            string definitions = File.ReadAllText(temporary.FilePath(compiler));
            temporary.Write(compiler, definitions + "#define ROCKS_TARGET_X64_SHA __attribute__((target(\"sha\")))\n");
            var unassigned = PolicyChecks.TargetIntrinsics(temporary.Root);
            Verify.That(unassigned.Count == 1 && unassigned[0].Rule == "target-intrinsics" && unassigned[0].Line == 3,
                "unassigned target macro definition accepted in Compiler.h\n" + string.Join('\n', unassigned));

            // 정의 헤더도 intrinsics는 쓰지 못한다(매크로 정의만 예외다).
            temporary.Write(compiler, definitions + "inline __m128i E(__m128i a, __m128i k) { return _mm_aesenc_si128(a, k); }\n");
            var inCompiler = PolicyChecks.TargetIntrinsics(temporary.Root);
            Verify.That(inCompiler.Count == 1 && inCompiler[0].Rule == "target-intrinsics",
                "intrinsic accepted in Compiler.h\n" + string.Join('\n', inCompiler));
        });
        suite.Add("Policy.TestAccessControls", () =>
        {
            using TemporaryDirectory temporary = new();
            const string access = "Core/Source/Runtime/Crypto/Private/AeadSequenceTestAccess.h";
            const string module = "Core/Source/Runtime/Crypto/Private/Aead.cpp";
            const string sodium = "Core/Source/Runtime/Crypto/Private/Sodium/SodiumRuntime.cpp";
            const string probe = "Engine/Source/Runtime/Probe/Private/Probe.cpp";
            const string moduleTests = "Core/Source/Runtime/Crypto/Tests/AeadTests.cpp";
            // 모듈 Tests/ 소스는 접근점을 include하고, 시험 소스(Tests/·FoundationTests·TestSupport)는 ROCKS_TESTING을 쓴다. 접근점끼리의
            // include, 주석·리터럴(raw 포함)의 언급, 이름이 TestAccess.h로 끝나지 않는 내부 헤더, Crypto 초기화의 libsodium 전역 설정은
            // 대상이 아니다.
            temporary.Write(access, "#pragma once\n");
            temporary.Write("Core/Source/Runtime/Crypto/Private/OtherTestAccess.h", "#pragma once\n#include \"AeadSequenceTestAccess.h\"\n");
            temporary.Write(moduleTests, "#include <Crypto/Private/AeadSequenceTestAccess.h>\n#if ROCKS_TESTING\n#endif\n");
            temporary.Write("Programs/FoundationTests/Private/Main.cpp", "#if ROCKS_TESTING\n#endif\n");
            temporary.Write("Core/Source/Developer/TestSupport/Private/Win64/Probe.cpp", "#if ROCKS_TESTING\n#endif\n");
            temporary.Write(module, "#include \"KeyAccess.h\"\n// AeadSequenceTestAccess.h, ROCKS_TESTING, randombytes_set_implementation\n" +
                "const char* n = \"ROCKS_TESTING AeadSequenceTestAccess.h sodium_set_misuse_handler\";\n" +
                "const char* raw = R\"(\n#include \"AeadSequenceTestAccess.h\"\n)\";\n");
            temporary.Write(sodium, "void Init() { randombytes_set_implementation(&impl); sodium_set_misuse_handler(&Misuse); }\n");
            temporary.Write(probe, "void Clean();\n");
            NoFailures(PolicyChecks.TestAccess(temporary.Root));

            void ExpectOne(string file, string source, string rule)
            {
                string original = File.Exists(temporary.FilePath(file)) ? File.ReadAllText(temporary.FilePath(file)) : "";
                temporary.Write(file, original + source + "\n");
                var failures = PolicyChecks.TestAccess(temporary.Root);
                Verify.That(failures.Count == 1 && failures[0].Rule == rule &&
                    failures[0].Line == original.Count(character => character == '\n') + 1,
                    $"{rule} escaped audit in {file}: {source}\n{string.Join('\n', failures)}");
                temporary.Write(file, original);
            }

            // 시험 소스 밖(모듈 구현·모듈 Private/Tests/·다른 모듈·시험 지원 코드·이름만 비슷한 runner 경로)의 접근점 include는 거부한다.
            foreach (string source in new[] { "#include \"AeadSequenceTestAccess.h\"", "#  include <Crypto/Private/AeadSequenceTestAccess.h>",
                "#include_next <Crypto/Private/AeadSequenceTestAccess.h>", "#import \"AeadSequenceTestAccess.h\"" })
            {
                foreach (string file in new[] { module, "Core/Source/Runtime/Crypto/Private/Tests/Hook.cpp", probe,
                    "Core/Source/Developer/TestSupport/Private/Win64/Other.cpp", "Programs/FoundationTestsX/Private/Main.cpp" })
                {
                    ExpectOne(file, source, "test-access-include");
                }
            }

            // 접근점은 모듈 Private/에만 둔다(공개 include 경로 안의 Private/도 아니다).
            foreach (string header in new[] { "Core/Source/Runtime/Crypto/Public/Crypto/AeadTestAccess.h",
                "Core/Source/Runtime/Crypto/Public/Crypto/Private/AeadTestAccess.h" })
            {
                temporary.Write(header, "#pragma once\n");
                var location = PolicyChecks.TestAccess(temporary.Root);
                Verify.That(location.Count == 1 && location[0].Rule == "test-access-location",
                    $"public test access header accepted: {header}\n{string.Join('\n', location)}");
                File.Delete(temporary.FilePath(header));
            }

            // 시험 소스·TestSupport 밖(모듈 구현·Private/Tests/·접근점 헤더·이름만 비슷한 TestSupport 경로)의 ROCKS_TESTING은 거부한다.
            foreach (string source in new[] { "#if ROCKS_TESTING", "#ifdef ROCKS_TESTING", "#if defined(ROCKS_TESTING)",
                "if constexpr (ROCKS_TESTING != 0) {}" })
            {
                foreach (string file in new[] { module, "Core/Source/Runtime/Crypto/Private/Tests/Hook.cpp", access, probe,
                    "Core/Source/Developer/TestSupportX/Private/Probe.cpp" })
                {
                    ExpectOne(file, source, "testing-macro");
                }
            }

            // libsodium 전역 설정은 Crypto 초기화 밖에서 부르지 않는다(시험 포함).
            foreach (string source in new[] { "randombytes_set_implementation(&fixed);", "sodium_set_misuse_handler(nullptr);" })
            {
                foreach (string file in new[] { module, moduleTests, probe })
                {
                    ExpectOne(file, source, "global-hook");
                }
            }
        });
        suite.Add("Policy.CompilerFlagsAndResponseFileControls", () =>
        {
            foreach (string valid in new[] { "clang-cl /W4 /WX /c probe.cpp", "clang++ -Wall -Wextra -Wpedantic -Werror -c probe.cpp",
                "\"C:\\compiler tools\\clang-cl.exe\" \"/W4\" \"/WX\" /c probe.cpp" })
            {
                NoFailures(PolicyChecks.CompilerCommand(valid));
            }

            foreach (string invalid in new[] { "clang-cl /W4 /c probe.cpp", "clang++ -Wall -c probe.cpp",
                "clang-cl /W4 /WX /WX-", "clang-cl /W4 /WX /wd4101", "clang-cl /W4 /WX /W0",
                "clang++ -Wall -Wextra -Wpedantic -Werror -Wno-error=unused-variable", "clang++ -Wall -Wextra -Wpedantic -Werror -w",
                "clang-cl /W4 /WX /D_CRT_SECURE_NO_WARNINGS", "clang++ -Wall -Werror", "clang++ -Wall -Wextra -Werror",
                "clang++ -Wall -Wpedantic -Werror", "clang-cl /W4 /WX /clang:-Wno-error=unused-variable", "clang-cl /W4 /WX @hidden.rsp",
                "clang-cl \"/W4 /WX" })
            {
                Verify.That(PolicyChecks.CompilerCommand(invalid).Count != 0, "invalid warning profile accepted: " + invalid);
            }
        });
        suite.Add("Policy.SourceSuppressionControls", () =>
        {
            using TemporaryDirectory temporary = new();
            foreach (string source in new[] { "#pragma warning(disable: 4101)", "#pragma clang diagnostic ignored \"-Wunused-variable\"",
                "#pragma GCC diagnostic ignored \"-Wunused-variable\"", "#define _CRT_SECURE_NO_WARNINGS", "#define _SCL_SECURE_NO_WARNINGS",
                "#pragma clang diagnostic warning \"-Wunused-variable\"", "#pragma GCC diagnostic warning \"-Wunused-variable\"",
                "#define QUIET _Pragma(\"clang diagnostic ignored \\\"-Wunused-variable\\\"\")",
                "#define QUIET __pragma(warning(disable: 4101))" })
            {
                temporary.Write("Engine/Probe.cpp", source);
                Verify.That(PolicyChecks.SourceWarnings(temporary.Root).Count != 0, "warning suppression escaped audit");
            }
        });
        suite.Add("Policy.CompilationDatabaseUsesStructuredArguments", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("compile_commands.json", "[]");
            Verify.That(PolicyChecks.CompilationDatabase(temporary.FilePath("compile_commands.json")).Count != 0, "empty compilation database accepted");
            temporary.Write("compile_commands.json", JsonSerializer.Serialize(new[] { new { file = "a.cpp", arguments = new[] { "clang-cl", "/W4", "/WX", "/clang:-std=c++23", "/c", "/TP", "a.cpp" } } }));
            NoFailures(PolicyChecks.CompilationDatabase(temporary.FilePath("compile_commands.json")));
            temporary.Write("compile_commands.json", "[{\"file\":\"a.cpp\",\"command\":\"clang-cl /W4 /c a.cpp\"}]");
            Verify.That(PolicyChecks.CompilationDatabase(temporary.FilePath("compile_commands.json")).Count != 0, "missing WX in database accepted");
        });
        suite.Add("Policy.PythonEntryDetectionAllowsWheelDownloadReferences", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Build/InstallFormatter.ps1", "$uri = 'https://files.pythonhosted.org/clang_format-21.1.6-py2.py3-none-win_amd64.whl'\n");
            temporary.Write("Build/reference.py", "import subprocess\n");
            NoFailures(PolicyChecks.PythonFreeMainPath(temporary.Root));
            temporary.Write(".github/workflows/build.yml", "run: python Build/build.py\n");
            Verify.That(PolicyChecks.PythonFreeMainPath(temporary.Root).Count != 0, "Python workflow entry escaped audit");
            temporary.Write(".github/workflows/build.yml", "run: dotnet run --project Programs/Rocks.Cli\n");
            temporary.Write("Programs/Rocks.Cli/Probe.cs", "var spec = new ProcessSpec(\"python.exe\", args, root);\n");
            Verify.That(PolicyChecks.PythonFreeMainPath(temporary.Root).Count != 0, "Python process invocation escaped audit");
        });
    }
}
