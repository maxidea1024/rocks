using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;
using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

internal static partial class SelfTests
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
        FormatCheckTests.Register(suite, options);
        ProcessReviewTests.Register(suite, options);
        ProcessHardTimeoutReviewTests.Register(suite, options);
        // rocks::Array 인스턴스화 실패의 RMC2304 변환 위치: Core Array.h 오류에 딸린 note 중 이 모듈 입력의 첫 위치를 clang
        // (file:line:col)과 clang-cl(file(line,col)) 두 형식 모두에서 찾고, 상대 경로는 Clang 작업 디렉터리 기준이다. 입력이
        // 아닌 note는 건너뛰고, 첫 error가 Array.h가 아니거나 note 앞에 다른 진단(warning 포함)이 끼면 바꾸지 않는다(Docs/Design/
        // ReflectionFoundations.ko.md 5.6절).
        suite.Add("Meta.ArrayInstantiationSiteParsesClangAndClangClDiagnostics", () =>
        {
            using TemporaryDirectory temporary = new();
            string input = temporary.FilePath("Probe.h");
            string other = temporary.FilePath("Other.h");
            File.WriteAllText(input, "\n");
            File.WriteAllText(other, "\n");
            bool IsInput(string path) => string.Equals(Path.GetFullPath(path), Path.GetFullPath(input), StringComparison.OrdinalIgnoreCase);
            const string arrayHeader = "C:/work/rocks/Core/Source/Runtime/Core/Public/Core/Containers/Array.h";
            string clang = $"In file included from <stdin>:1:\n" +
                $"{arrayHeader}:18:19: error: static assertion failed\n{other}:4:5: note: in instantiation of template class 'rocks::Array<Holder>' requested here\n" +
                $"{input}:15:56: note: in instantiation of template class 'rocks::Array<rocks::ObjectRef<probe::Value>>' requested here\n" +
                $"{input}:20:3: note: a later note\n";
            Verify.That(CompilerDriver.ArrayInstantiationSite(clang, temporary.Root, IsInput) is (string file, 15, 56) && IsInput(file), "clang location format");
            string clangCl = $"{arrayHeader.Replace('/', '\\')}(18,19): error: static assertion failed\r\n" +
                "Probe.h(18,3): note: in instantiation of template class 'rocks::Array<probe::Holder>' requested here\r\n";
            Verify.That(CompilerDriver.ArrayInstantiationSite(clangCl, temporary.Root, IsInput) is (string relative, 18, 3) && IsInput(relative),
                "clang-cl location format with a path relative to the Clang working directory");
            Verify.That(CompilerDriver.ArrayInstantiationSite($"{other}:3:1: error: x\n{input}:9:9: note: y\n", temporary.Root, IsInput) is null,
                "notes of errors outside Array.h are not array element failures");
            Verify.That(CompilerDriver.ArrayInstantiationSite($"{other}:3:1: error: x\n{arrayHeader}:18:19: error: y\n{input}:9:9: note: z\n", temporary.Root, IsInput) is null,
                "an Array.h failure that is not the first error is not converted");
            Verify.That(CompilerDriver.ArrayInstantiationSite($"{arrayHeader}:18:19: error: x\n{other}:1:1: warning: y\n{input}:9:9: note: z\n", temporary.Root, IsInput) is null,
                "a warning ends the notes of the Array.h failure");
            Verify.That(CompilerDriver.ArrayInstantiationSite($"{arrayHeader}:18:19: error: x\n{other}:4:5: note: y\n", temporary.Root, IsInput) is null,
                "an array failure without a note in a module input keeps RMC3000");
        });
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
        suite.Add("NativeEnvironment.ResourceDirectoryProbeIsClangClOnly", async () =>
        {
            // 경로 구분자는 호스트마다 다르므로(POSIX에서 \는 이름의 일부) Path.Combine으로 만든다.
            Verify.That(NativeEnvironment.IsClangCl(Path.Combine("llvm", "bin", "clang-cl.exe")), "clang-cl with extension");
            Verify.That(NativeEnvironment.IsClangCl("CLANG-CL"), "clang-cl case-insensitive");
            Verify.That(!NativeEnvironment.IsClangCl(Path.Combine("ndk", "prebuilt", "windows-x86_64", "bin", "clang++.exe")), "NDK clang++");
            Verify.That(!NativeEnvironment.IsClangCl("/usr/bin/clang++"), "POSIX clang++");
            if (OperatingSystem.IsWindows())
            {
                // Android 교차 빌드의 NDK clang++에는 clang-cl 전용 resource-dir probe를 하지 않는다(B5 체인 회귀).
                string missing = Path.Combine(options.Root, "out", "no-such-ndk", "clang++.exe");
                var environment = await NativeEnvironment.CreateAsync(missing);
                Verify.That(environment.Count != 0, "environment for a non clang-cl compiler");
            }
        });
        suite.Add("Process.StandardInputIsNotShellInterpreted", async () =>
        {
            string input = "한글 🎮 $HOME & | ; \"quotes\"\nline two";
            var result = await ProcessRunner.RunAsync(SelfProcess.Command(["child", "--echo-input"], options.Root, input: input));
            Verify.Equal(0, result.ExitCode, "stdin child exit");
            Verify.Equal(input, result.StdOut, "stdin bytes/text changed");
        });
        RegisterPolicy(suite, options);
        RegisterScaffold(suite, options);
    }

    private static void NoFailures(IReadOnlyList<CheckFailure> failures) => Verify.That(failures.Count == 0, string.Join('\n', failures));

    private static string Hash(string text) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));
}
