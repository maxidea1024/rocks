using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;
using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

internal static class SelfTests
{
    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    public static void Register(TestSuite suite, Options options)
    {
        BoundedFileTests.Register(suite);
        ApplePropertyLocationTests.Register(suite);
        IdentityInventoryTests.Register(suite);
        StableIdManifestTests.Register(suite);
        StableIdMergeTests.Register(suite);
        IdentityGitMergeTests.Register(suite, options);
        ProgramsPolicyReviewTests.Register(suite);
        ProcessReviewTests.Register(suite, options);
        ProcessHardTimeoutReviewTests.Register(suite, options);
        suite.Add("MetaPrefix.CommentLineSpliceIsRejectedDirectly", () =>
        {
            void CheckGap(string gap, bool allowed)
            {
                foreach ((string name, PrefixMarker marker) in new[] { ("ROCKS_CLASS", PrefixMarker.Class), ("ROCKS_STRUCT", PrefixMarker.Struct) })
                {
                    string source = name + "()" + gap + "class Object {};";
                    byte[] bytes = Encoding.UTF8.GetBytes(source);
                    int end = Encoding.UTF8.GetByteCount(source[..source.IndexOf("class Object", StringComparison.Ordinal)]);
                    if (allowed)
                    {
                        PrefixSourceVerifier.Verify(bytes, 0, end, marker);
                    }
                    else
                    {
                        Verify.Throws<MetadataException>(() => PrefixSourceVerifier.Verify(bytes, 0, end, marker));
                    }
                }

                // 마커 종류와 원문 매크로가 다르면(ROCKS_STRUCT 자리의 ROCKS_CLASS) 거부한다.
                string mismatched = "ROCKS_CLASS()" + gap + "class Object {};";
                int mismatchedEnd = Encoding.UTF8.GetByteCount(mismatched[..mismatched.IndexOf("class Object", StringComparison.Ordinal)]);
                Verify.Throws<MetadataException>(() => PrefixSourceVerifier.Verify(Encoding.UTF8.GetBytes(mismatched), 0, mismatchedEnd, PrefixMarker.Struct));
            }

            CheckGap("\n// ordinary \\ path #if\n", true);
            CheckGap("\n/* ordinary \\ path #include */\n", true);
            CheckGap("\n// line splice \\\n", false);
            CheckGap("\n// line splice \\\r\n", false);
            CheckGap("\n/* block splice \\\n continuation */\n", false);
            CheckGap("\n/* block splice \\\r\n continuation */\n", false);
            CheckGap("\n/* split terminator *\\\n/ #if 0\n class hidden {};\n#endif\n/* */\n", false);
        });
        suite.Add("TestSuite.ParallelRunKeepsNameOrderBoundAndExclusiveIsolation", async () =>
        {
            using TemporaryDirectory temporary = new();
            object gate = new();
            int active = 0;
            int peak = 0;
            bool overlapped = false;
            List<string> exclusiveRuns = [];
            async Task Body(bool exclusive, bool fail)
            {
                lock (gate)
                {
                    ++active;
                    peak = Math.Max(peak, active);
                    overlapped |= exclusive && active != 1;
                }

                await Task.Delay(20);
                lock (gate)
                {
                    overlapped |= exclusive && active != 1;
                    --active;
                }

                if (fail)
                {
                    throw new InvalidOperationException("expected inner failure");
                }
            }

            using StringWriter innerOutput = new();
            using StringWriter innerErrors = new();
            TestSuite inner = new(innerOutput, innerErrors);
            foreach (string name in new[] { "g", "c", "e", "a", "f", "b", "d", "h" })
            {
                inner.Add("Inner." + name, () => Body(false, name == "e"));
            }

            inner.Add("Inner.x1", async () =>
            {
                await Body(true, false);
                exclusiveRuns.Add("x1");
            }, exclusive: true);
            inner.Add("Inner.x0", async () =>
            {
                await Body(true, false);
                exclusiveRuns.Add("x0");
            }, exclusive: true);
            string report = temporary.FilePath("report.json");
            Verify.Equal(1, await inner.RunAsync(report, 7351, parallelism: 3), "inner failure exit code");
            Verify.That(peak > 1 && peak <= 3, $"parallel peak out of range: {peak}");
            Verify.That(!overlapped, "exclusive test overlapped another test");
            Verify.Equal("x0,x1", string.Join(',', exclusiveRuns), "exclusive tests run one at a time in name order");
            using JsonDocument document = JsonDocument.Parse(File.ReadAllBytes(report));
            string[] names = [.. document.RootElement.GetProperty("tests").EnumerateArray().Select(test => test.GetProperty("name").GetString()!)];
            Verify.Equal("Inner.a,Inner.b,Inner.c,Inner.d,Inner.e,Inner.f,Inner.g,Inner.h,Inner.x0,Inner.x1", string.Join(',', names), "report name order");
            Verify.Equal(1, document.RootElement.GetProperty("failures").GetInt32(), "report failure count");
            Verify.Contains("[FAIL] Inner.e", innerOutput.ToString(), "inner result line");
            Verify.Contains("expected inner failure", innerErrors.ToString(), "inner failure detail");
            Verify.Throws<ArgumentOutOfRangeException>(() => inner.RunAsync(null, 7351, parallelism: 0).GetAwaiter().GetResult());
        });
        suite.Add("Process.LargeUnicodePipesDrainWithoutDeadlock", async () =>
        {
            var result = await ProcessRunner.RunAsync(SelfProcess.Command(["child", "--large-output"], options.Root));
            Verify.Equal(17, result.ExitCode, "child failure exit");
            Verify.That(result.StdOut.Length >= 256 * 1024 && result.StdErr.Length >= 256 * 1024, "pipe output was truncated");
            Verify.Contains("출력🎮", result.StdOut, "stdout Unicode");
            Verify.Contains("오류🎮", result.StdErr, "stderr Unicode");
        });
        suite.Add("Process.StandardInputIsNotShellInterpreted", async () =>
        {
            string input = "한글 🎮 $HOME & | ; \"quotes\"\nline two";
            var result = await ProcessRunner.RunAsync(SelfProcess.Command(["child", "--echo-input"], options.Root, input: input));
            Verify.Equal(0, result.ExitCode, "stdin child exit");
            Verify.Equal(input, result.StdOut, "stdin bytes/text changed");
        });
        suite.Add("Policy.RepositoryRuntime", () => NoFailures(PolicyChecks.Runtime(options.Root)));
        ThirdPartyPolicyTests.Register(suite, options);
        suite.Add("Policy.RepositoryPlatform", () => NoFailures(PolicyChecks.PlatformBoundary(options.Root)));
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
            string library = Path.Combine(ThirdPartyManifest.ThirdPartyRoot, UnicodeTables.LibraryName);
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
        suite.Add("Scaffold.PlanApplyValidatePreservesPlanAndRejectsOverwrite", () =>
        {
            using TemporaryDirectory temporary = new();
            string plan = temporary.FilePath("plan.json");
            var created = ScaffoldService.PlanModule(options.Root, "Example", temporary.Root, plan);
            byte[] original = File.ReadAllBytes(plan);
            ScaffoldService.Apply(plan);
            ScaffoldService.ValidateOutput(plan);
            Verify.That(File.ReadAllBytes(plan).AsSpan().SequenceEqual(original), "apply/validate changed the reviewed plan");
            Verify.That(File.Exists(Path.Combine(created.Target, "Example.cpp")), "module source missing");
            Verify.Throws<IOException>(() => ScaffoldService.Apply(plan));
            Verify.Throws<IOException>(() => ScaffoldService.PlanModule(options.Root, "Other", temporary.Root, plan));
            Verify.That(!Directory.EnumerateDirectories(temporary.Root, "*.stage-*").Any(), "owned staging directory leaked");
        });
        suite.Add("Scaffold.TraversalCollisionsAndHashTamperAreRejected", () =>
        {
            using TemporaryDirectory temporary = new();
            var original = ScaffoldService.PlanModule(options.Root, "Safe", temporary.Root, temporary.FilePath("plan.json"));
            foreach (string relative in new[] { "../escape.txt", "/escape.txt", "a\\b.txt", "C:escape.txt", "./x", "a//x", "CON.txt", "a. ", "AUX", "a/../x" })
            {
                var modified = original with { Files = new(StringComparer.Ordinal) { [relative] = "escape" }, Hashes = new(StringComparer.Ordinal) { [relative] = Hash("escape") } };
                Verify.Throws<InvalidDataException>(() => ScaffoldService.ValidatePlan(modified));
            }

            var changed = original with { Files = new(original.Files, StringComparer.Ordinal) };
            changed.Files[changed.Files.Keys.First()] += "tamper";
            Verify.Throws<InvalidDataException>(() => ScaffoldService.ValidatePlan(changed));
            var collision = original with { Files = new() { ["a"] = "", ["a/b"] = "" }, Hashes = new() { ["a"] = Hash(""), ["a/b"] = Hash("") } };
            Verify.Throws<InvalidDataException>(() => ScaffoldService.ValidatePlan(collision));
            Verify.That(!Directory.Exists(original.Target), "invalid plan created output");
        });
        suite.Add("Scaffold.MalformedJsonAndOutputMismatchAreRejected", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("duplicate.json", "{\"schema\":1,\"schema\":1}");
            Verify.Throws<InvalidDataException>(() => ScaffoldService.ReadPlan(temporary.FilePath("duplicate.json")));
            var plan = ScaffoldService.PlanModule(options.Root, "Example", temporary.Root, temporary.FilePath("plan.json"));
            ScaffoldService.Apply(temporary.FilePath("plan.json"));
            File.AppendAllText(Path.Combine(plan.Target, "Example.cpp"), "modified\n");
            Verify.Throws<InvalidDataException>(() => ScaffoldService.ValidateOutput(temporary.FilePath("plan.json")));
        });
        suite.Add("Scaffold.FailedStagingDoesNotPublishOrLeak", () =>
        {
            using TemporaryDirectory temporary = new();
            var original = ScaffoldService.PlanModule(options.Root, "Example", temporary.Root, temporary.FilePath("original.json"));
            string oversizedName = new('x', 300);
            var failed = original with { Files = new() { [oversizedName] = "value" }, Hashes = new() { [oversizedName] = Hash("value") } };
            temporary.Write("failed.json", JsonSerializer.Serialize(failed, Json));
            Verify.Throws<IOException>(() => ScaffoldService.Apply(temporary.FilePath("failed.json")));
            Verify.That(!Directory.Exists(original.Target), "failed staging published target");
            Verify.That(!Directory.EnumerateDirectories(temporary.Root, "*.stage-*").Any(), "failed staging leaked directory");
        });
        suite.Add("Scaffold.CliUsesCSharpEntryPoint", async () =>
        {
            using TemporaryDirectory temporary = new();
            string plan = temporary.FilePath("cli.json");
            var created = await ProcessRunner.RunAsync(SelfProcess.Command(["scaffold", "plan", "--root", options.Root,
                "--name", "CliExample", "--output", temporary.Root, "--plan", plan], options.Root));
            Verify.Equal(0, created.ExitCode, created.StdErr);
            Verify.Equal(0, (await ProcessRunner.RunAsync(SelfProcess.Command(["scaffold", "apply", "--plan", plan], options.Root))).ExitCode, "CLI apply");
            Verify.Equal(0, (await ProcessRunner.RunAsync(SelfProcess.Command(["scaffold", "validate", "--plan", plan], options.Root))).ExitCode, "CLI validate");
        });
        suite.Add("Evidence.SnapshotTracksInputContentAndExcludesGeneratedFiles", () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "before\n");
            temporary.Write("Templates/Game/Source/Main.cpp", "template\n");
            var initial = SourceSnapshot.Capture(temporary.Root);
            temporary.Write("out/ignored.json", "generated\n");
            temporary.Write("Programs/Tool/bin/ignored.dll", "generated\n");
            temporary.Write("Build/Cache/ignored", "cache\n");
            Verify.Equal(initial, SourceSnapshot.Capture(temporary.Root), "generated artifact altered input digest");
            temporary.Write("Engine/source.h", "after\n");
            Verify.That(initial != SourceSnapshot.Capture(temporary.Root), "changed source content escaped snapshot");
        });
        suite.Add("Evidence.Exit17AndUnknownGitAreNeverSuccess", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            var evidence = await BuildEvidence.StartAsync(temporary.Root, "negative/exit17");
            var child = await evidence.RunAsync(SelfProcess.Command(["child", "--exit-code", "17"], temporary.Root));
            Verify.Equal(17, child.ExitCode, "child exit");
            var report = await evidence.FinishAsync();
            Verify.Equal("FAIL", report.Status, "failed step status without caller error");
            Verify.Equal(17, report.Steps[0].ExitCode, "failure provenance");
            Verify.Equal("NOT RUN", report.TestsStatus, "tests were not executed");
            Verify.That(report.GitAfter?.Dirty is null && !report.RunnerReportCurrent, "unknown Git/stale report claimed clean/current");
            Verify.That(File.Exists(evidence.EvidencePath), "failure evidence missing");
        });
        suite.Add("Evidence.SourceMutationProducesPersistedFailure", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "before\n");
            var evidence = await BuildEvidence.StartAsync(temporary.Root, "mutation");
            temporary.Write("Engine/source.h", "after\n");
            var report = await evidence.FinishAsync();
            Verify.That(report.Status == "FAIL" && !report.SourceStableDuringRun, "source mutation passed evidence");
            using JsonDocument persisted = JsonDocument.Parse(File.ReadAllText(evidence.EvidencePath));
            Verify.Equal("FAIL", persisted.RootElement.GetProperty("status").GetString(), "persisted status");
        });
        suite.Add("Evidence.StaleMissingAndFailingReportsCannotPass", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            foreach (string mode in new[] { "missing", "stale", "malformed", "failed", "inconsistent" })
            {
                var evidence = await BuildEvidence.StartAsync(temporary.Root, mode);
                evidence.BeginTests();
                string reportPath = Path.Combine(evidence.OutputDirectory, "foundation-report.json");
                if (mode != "missing")
                {
                    Directory.CreateDirectory(evidence.OutputDirectory);
                    File.WriteAllText(reportPath, mode == "malformed" ? "{" :
                        JsonSerializer.Serialize(new
                        {
                            failures = mode == "failed" ? 1 : 0,
                            tests = new[] { new { name = "case", status = mode is "failed" or "inconsistent" ? "FAIL" : "PASS" } }
                        }));
                    if (mode == "stale")
                    {
                        File.SetLastWriteTimeUtc(reportPath, DateTime.UnixEpoch.AddYears(30));
                    }
                }

                evidence.RecordStep("tests", ["native"], 0, TimeSpan.Zero, tests: true);
                var report = await evidence.FinishAsync();
                Verify.Equal("FAIL", report.Status, mode + " evidence status");
                if (mode is "missing" or "stale" or "malformed")
                {
                    Verify.That(!report.RunnerReportCurrent, "invalid report current");
                }
            }
        });
        suite.Add("Evidence.SnapshotFailureDoesNotKeepOldPass", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            string evidencePath = temporary.FilePath("out/current/build-evidence.json");
            Directory.CreateDirectory(Path.GetDirectoryName(evidencePath)!);
            File.WriteAllText(evidencePath, "{\"status\":\"PASS\"}");
            using (var locked = new FileStream(temporary.FilePath("Engine/source.h"), FileMode.Open, FileAccess.ReadWrite, FileShare.None))
            {
                await Verify.ThrowsAsync<IOException>(async () => { await BuildEvidence.StartAsync(temporary.Root, "current"); });
            }

            using var evidence = JsonDocument.Parse(File.ReadAllText(evidencePath));
            Verify.Equal("RUNNING", evidence.RootElement.GetProperty("status").GetString(), "snapshot failure left old PASS");
        });
        suite.Add("Evidence.CurrentPassingReportAndLifecycleGuards", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            string evidencePath = temporary.FilePath("out/current/build-evidence.json");
            Directory.CreateDirectory(Path.GetDirectoryName(evidencePath)!);
            File.WriteAllText(evidencePath, "{\"status\":\"PASS\"}");
            var evidence = await BuildEvidence.StartAsync(temporary.Root, "current");
            using (var running = JsonDocument.Parse(File.ReadAllText(evidencePath)))
            {
                Verify.Equal("RUNNING", running.RootElement.GetProperty("status").GetString(), "previous PASS retained on start");
            }

            evidence.SetConfiguration("Debug", "FoundationTests", null, "test-compiler", "fixture-version");
            Directory.CreateDirectory(evidence.OutputDirectory);
            string reportPath = Path.Combine(evidence.OutputDirectory, "foundation-report.json");
            File.WriteAllText(reportPath, "old report");
            evidence.BeginTests(reportPath);
            Verify.That(!File.Exists(reportPath), "old report was not removed before test execution");
            File.WriteAllText(reportPath, "{\"failures\":0,\"tests\":[{\"name\":\"probe\",\"status\":\"PASS\"}]}");
            evidence.RecordStep("tests", ["native"], 0, TimeSpan.FromMilliseconds(1), tests: true);
            var report = await evidence.FinishAsync();
            Verify.That(report.Status == "PASS" && report.RunnerReportCurrent && report.RunnerReportSha256?.Length == 64, "valid current report did not pass");
            using (var completed = JsonDocument.Parse(File.ReadAllText(evidencePath)))
            {
                Verify.Equal("PASS", completed.RootElement.GetProperty("status").GetString(), "final evidence was not published");
            }

            Verify.Throws<InvalidOperationException>(() => evidence.RecordStep("late", [], 0, TimeSpan.Zero));
            Verify.Throws<InvalidOperationException>(() => evidence.BeginTests());
            await Verify.ThrowsAsync<InvalidOperationException>(async () => { await evidence.FinishAsync(); });
        });
        suite.Add("Evidence.UnownedOutputPathsAreRejected", async () =>
        {
            using TemporaryDirectory temporary = new();
            temporary.Write("Engine/source.h", "input\n");
            await Verify.ThrowsAsync<ArgumentException>(async () => { await BuildEvidence.StartAsync(temporary.Root, "../escape"); });
            var evidence = await BuildEvidence.StartAsync(temporary.Root, "safe");
            Verify.Throws<ArgumentException>(() => evidence.BeginTests(temporary.FilePath("outside.json")));
            Verify.That(!File.Exists(temporary.FilePath("outside.json")), "outside report was created");
        });
    }

    private static void NoFailures(IReadOnlyList<CheckFailure> failures) => Verify.That(failures.Count == 0, string.Join('\n', failures));

    private static string Hash(string text) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
}
