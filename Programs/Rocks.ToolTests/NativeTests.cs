using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class NativeTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        LoggerOutputTests.Register(suite, options, environment);
        AlgorithmsCompilerTests.Register(suite, options, environment);
        string executable = Path.GetFullPath(options.Required("native-executable"));
        if (!File.Exists(executable)) throw new FileNotFoundException("Native test executable missing", executable);
        Task<ProcessResult> Run(params string[] args) => ProcessRunner.RunAsync(new(executable, args, options.Root,
            environment, Timeout: TimeSpan.FromSeconds(20)));
        foreach (string control in new[] { "check", "require" })
            suite.Add("Native.Negative." + control, async () =>
            {
                var result = await Run("--negative-control", control);
                Verify.Equal(1, result.ExitCode, "negative control exit");
                Verify.Contains("CHECK FAILED: false", result.StdErr, "negative control diagnostic");
            });
        foreach (string state in new[] { "reserved", "decommitted", "committed" })
            foreach (string access in new[] { "read", "write" })
            {
                string control = $"vm-{state}-{access}";
                suite.Add("Native.VirtualMemory." + control, async () =>
                {
                    // ASan 진단이 하드웨어 예외의 종료 상태를 가리지 않도록 해당 child만 조정한다.
                    var probeEnvironment = new Dictionary<string, string?>(environment);
                    probeEnvironment.TryGetValue("ASAN_OPTIONS", out string? asanOptions);
                    probeEnvironment["ASAN_OPTIONS"] = (asanOptions ?? "") + ":handle_segv=0:handle_sigbus=0";
                    var result = await ProcessRunner.RunAsync(new(executable,
                        ["--negative-control", control], options.Root, probeEnvironment,
                        Timeout: TimeSpan.FromSeconds(20)));
                    Verify.Contains("VM_ACCESS_READY", result.StdErr, "probe setup did not finish");
                    if (state == "committed")
                    {
                        Verify.Equal(0, result.ExitCode, "committed access must succeed");
                    }
                    else if (OperatingSystem.IsWindows())
                    {
                        Verify.Equal(unchecked((int)0xC0000005), result.ExitCode, "expected access violation");
                    }
                    else if (OperatingSystem.IsMacOS())
                    {
                        // .NET reports 128 + signal on Unix; Darwin SIGSEGV=11, SIGBUS=10.
                        Verify.That(result.ExitCode is 139 or 138, $"expected SIGSEGV/SIGBUS, got {result.ExitCode}");
                    }
                    else
                    {
                        throw new PlatformNotSupportedException("Virtual memory probe supports Windows and macOS");
                    }
                });
            }
        suite.Add("Native.InvalidFilterAndRepeat", async () =>
        {
            Verify.Equal(2, (await Run("--filter", "ThisTestDoesNotExist")).ExitCode, "unmatched test filter");
            Verify.Equal(2, (await Run("--repeat", "0")).ExitCode, "zero repeat");
        });
        suite.Add("Native.SortedUniqueInventory", async () =>
        {
            var result = await Run("--list");
            Verify.Equal(0, result.ExitCode, "list exit");
            string[] names = result.StdOut.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries);
            Verify.That(names.Length != 0 && names.SequenceEqual(names.Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal)), "list must be sorted/unique/nonempty");
        });
        suite.Add("Native.SeedRepeatJsonAndReportFailure", async () =>
        {
            using TemporaryDirectory temporary = new();
            string reportPath = temporary.FilePath("report.json");
            var result = await Run("--filter", "Harness_SeedIsAccessible", "--seed", "914", "--repeat", "3", "--json", reportPath);
            Verify.Equal(0, result.ExitCode, result.StdErr);
            using JsonDocument report = JsonDocument.Parse(File.ReadAllText(reportPath));
            Verify.Equal(914, report.RootElement.GetProperty("seed").GetInt32(), "seed");
            Verify.Equal(3, report.RootElement.GetProperty("repeat").GetInt32(), "repeat");
            Verify.Equal(0, report.RootElement.GetProperty("failures").GetInt32(), "failures");
            var tests = report.RootElement.GetProperty("tests");
            Verify.Equal(1, tests.GetArrayLength(), "filter case count");
            Verify.Equal("PASS", tests[0].GetProperty("status").GetString(), "case status");
            var unavailable = await Run("--filter", "Harness_SeedIsAccessible", "--json", temporary.Root);
            Verify.Equal(2, unavailable.ExitCode, "unwritable report exit");
            Verify.Contains("Cannot open JSON report", unavailable.StdErr, "unwritable report diagnostic");
            Verify.That(!unavailable.StdOut.Contains("[PASS]", StringComparison.Ordinal), "report failure must precede test pass output");
        });
        suite.Add("Native.ConcurrentFailureCounting", async () =>
        {
            var result = await Run("--negative-control", "parallel");
            Verify.Equal(1, result.ExitCode, "parallel negative exit");
            Verify.Equal(200, result.StdErr.Split("CHECK FAILED: parallel-negative-control", StringSplitOptions.None).Length - 1, "parallel failure count");
        });
        foreach ((string control, string diagnostic) in new[] { ("assert-stderr-lock", "Rocks invariant failed:"),
            ("thread-unjoined", "Thread"), ("name-live-lease", "NamePool destroyed with active operations or leases") })
            suite.Add("Native.Fatal." + control, async () =>
            {
                var result = await Run("--negative-control", control);
                Verify.That(result.ExitCode != 0, "fatal negative control succeeded");
                Verify.Contains(diagnostic, result.StdErr, "fatal diagnostic");
                if (control == "thread-unjoined")
                {
                    Verify.Contains("THREAD_UNJOINED_READY", result.StdErr, "thread completion marker");
                    const string ready = "THREAD_UNJOINED_READY";
                    const string fatal = "Thread destroyed without a successful Join";
                    Verify.Contains(fatal, result.StdErr, "unjoined diagnostic");
                    Verify.That(result.StdErr.IndexOf(ready, StringComparison.Ordinal) < result.StdErr.IndexOf(fatal, StringComparison.Ordinal), "completion marker must precede destructor diagnostic");
                    Verify.Equal(1, result.StdErr.Split(ready, StringSplitOptions.None).Length - 1, "completion marker count");
                    Verify.Equal(1, result.StdErr.Split(fatal, StringSplitOptions.None).Length - 1, "destructor diagnostic count");
                    Verify.That(!result.StdErr.Contains("THREAD_SETUP_", StringComparison.Ordinal), "thread setup failed");
                }
            });
        foreach ((string control, string diagnostic) in new[]
        {
            ("thread-unjoined-start-failure", "THREAD_SETUP_START_FAILED"),
            ("thread-unjoined-wait-failure", "THREAD_SETUP_WAIT_FAILED")
        })
            suite.Add("Native.Fatal." + control, async () =>
            {
                var result = await Run("--negative-control", control);
                Verify.That(result.ExitCode != 0, "thread setup control succeeded");
                Verify.Contains(diagnostic, result.StdErr, "thread setup diagnostic");
                Verify.That(!result.StdErr.Contains("THREAD_UNJOINED_READY", StringComparison.Ordinal), "setup failure appeared ready");
                Verify.That(!result.StdErr.Contains("Thread destroyed without a successful Join", StringComparison.Ordinal), "setup failure appeared as unjoined destructor");
            });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach (string control in new[] { "object-scope-lifo", "object-scope-thread" })
                suite.Add("Native.Fatal." + control, async () =>
                {
                    var result = await Run("--negative-control", control);
                    Verify.That(result.ExitCode != 0, "invalid scope lifetime did not fail");
                    Verify.Contains("ObjectAccessScope must close", result.StdErr, "scope fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach ((string control, string error) in new[] { ("object-status-stale", "StaleHandle"), ("object-status-context", "WrongContext") })
                suite.Add("Native.Fatal." + control, async () =>
                {
                    var result = await Run("--negative-control", control);
                    Verify.That(result.ExitCode != 0, "invalid status query did not fail");
                    Verify.Contains("Object::IsDestroyRequested failed: " + error, result.StdErr, "status fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach ((string control, string error) in new[] { ("object-type-stale", "StaleHandle"), ("object-type-context", "WrongContext") })
                suite.Add("Native.Fatal." + control, async () =>
                {
                    var result = await Run("--negative-control", control);
                    Verify.That(result.ExitCode != 0, "invalid type query did not fail");
                    Verify.Contains("Object::GetType failed: " + error, result.StdErr, "type fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase))
            suite.Add("Native.Fatal.empty-type-isa", async () =>
            {
                var result = await Run("--negative-control", "empty-type-isa");
                Verify.That(result.ExitCode != 0, "empty type IsA did not fail");
                Verify.Contains("TypeLease::IsA called on empty lease", result.StdErr, "empty type fatal diagnostic");
            });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach (string kind in new[] { "map", "set" })
                foreach (string operation in new[] { "erase", "clear", "reset" })
                {
                    string control = $"hash-{kind}-foreach-{operation}";
                    suite.Add("Native.Fatal." + control, async () =>
                    {
                        var result = await Run("--negative-control", control);
                        Verify.That(result.ExitCode != 0, "hash mutation during traversal did not fail");
                        Verify.Contains("HashMap structural mutation during iteration", result.StdErr, "hash mutation fatal diagnostic");
                    });
                }
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach (string control in new[] { "hash-map-eraseif-clear", "hash-set-eraseif-clear", "hash-map-factory-clear", "hash-set-union-erase" })
                suite.Add("Native.Fatal." + control, async () =>
                {
                    var result = await Run("--negative-control", control);
                    Verify.That(result.ExitCode != 0, "hash operation mutation did not fail");
                    Verify.Contains("HashMap structural mutation during iteration", result.StdErr, "hash operation fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach ((string control, string diagnostic) in new[]
            {
                ("allocator-arena-live", "ArenaAllocator destroyed with live allocations"),
                ("allocator-pool-live", "PoolAllocator destroyed with live allocations"),
                ("allocator-budget-live", "BudgetAllocator destroyed with active calls or live allocations"),
                ("allocator-external-overlap", "ExternalAllocator backend returned overlapping live allocation"),
                ("allocator-external-overflow", "ExternalAllocator backend returned overflowing address range"),
                ("allocator-external-metadata", "ExternalAllocator metadata allocator rejected owned record"),
                ("allocator-external-live", "ExternalAllocator destroyed with active calls or live allocations")
            })
                suite.Add("Native.Fatal." + control, async () =>
                {
                    var result = await Run("--negative-control", control);
                    Verify.That(result.ExitCode != 0, "allocator with live allocation did not fail");
                    Verify.Contains(diagnostic, result.StdErr, "allocator fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
        {
            foreach ((string control, string diagnostic, string offset) in new[]
            {
                ("allocator-debug-front", "DebugAllocator detected front guard corruption", "15"),
                ("allocator-debug-back", "DebugAllocator detected back guard corruption", "0"),
                ("allocator-debug-after-free", "DebugAllocator detected write after free", "0")
            })
                suite.Add("Native.Fatal." + control, async () =>
                {
                    var result = await Run("--negative-control", control);
                    Verify.That(result.ExitCode != 0, "debug allocator did not detect corruption");
                    Verify.Contains(diagnostic, result.StdErr, "debug corruption diagnostic");
                    Verify.Contains("address=0x", result.StdErr, "debug corruption address");
                    Verify.Contains("tag=439", result.StdErr, "debug corruption tag");
                    Verify.Contains("offset=" + offset, result.StdErr, "debug corruption offset");
                });
            suite.Add("Native.Fatal.allocator-debug-live", async () =>
            {
                var result = await Run("--negative-control", "allocator-debug-live");
                Verify.That(result.ExitCode != 0, "debug allocator with live allocation did not fail");
                Verify.Contains("DebugAllocator destroyed with live allocations", result.StdErr, "debug leak diagnostic");
            });
        }
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach ((string control, string diagnostic) in new[] {
                ("allocator-stack-live", "StackAllocator destroyed with live allocations"),
                ("allocator-stack-wrong-thread", "StackAllocator mark accessed on wrong thread or during reentrant call"),
                ("allocator-stack-array-growth", "Array storage was not owned by its allocator") })
                suite.Add("Native.Fatal." + control, async () =>
                {
                    var result = await Run("--negative-control", control);
                    Verify.That(result.ExitCode != 0, "invalid stack allocator usage did not fail");
                    Verify.Contains(diagnostic, result.StdErr, "stack allocator fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach ((int operation, string diagnostic) in new[] {
                (0, "BinnedAllocator backing returned invalid storage"),
                (1, "BinnedAllocator backing returned invalid storage"),
                (2, "BinnedAllocator backing returned overflowing address range"),
                (3, "BinnedAllocator backing returned overlapping storage"),
                (4, "BinnedAllocator backing returned overlapping storage"),
                (5, "BinnedAllocator backing returned overlapping storage"),
                (6, "BinnedAllocator backing returned overlapping storage") })
                suite.Add("Native.Fatal.allocator-binned-invalid-" + operation, async () =>
                {
                    var result = await Run("--negative-control", "allocator-binned-invalid-" + operation);
                    Verify.That(result.ExitCode != 0, "invalid backing allocation did not fail");
                    Verify.Contains(diagnostic, result.StdErr, "binned backing fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach ((int operation, string diagnostic) in new[] {
                (0, "FileSystem Open violated handle ownership contract"),
                (1, "FileSystem Open violated handle ownership contract"),
                (2, "FileHandle reported more bytes than requested"),
                (3, "File close failed during destruction"),
                (4, "File queried during reentrant callback"),
                (5, "FileHandle reported more bytes than requested"),
                (6, "FileHandle reported more bytes than requested"),
                (7, "FileHandle reported more bytes than requested") })
                suite.Add("Native.Fatal.platform-file-invalid-" + operation, async () =>
                {
                    var result = await Run("--negative-control", "platform-file-invalid-" + operation);
                    Verify.That(result.ExitCode != 0, "invalid file backend did not fail");
                    Verify.Contains(diagnostic, result.StdErr, "file backend fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach ((int operation, string diagnostic) in new[] {
                (0, "PlatformMath int32 conversion is NaN or out of range"),
                (1, "PlatformMath int32 conversion is NaN or out of range"),
                (2, "IntVector addition overflow"),
                (3, "Math batch input/output sizes differ"),
                (4, "Math batch input/output ranges partially overlap"),
                (5, "Matrix4 index out of range"),
                (6, "Rocks invariant failed: unitAxis.IsNormalized"),
                (7, "Rocks invariant failed: minimum <= maximum") })
                suite.Add("Native.Fatal.math-invalid-" + operation, async () =>
                {
                    // 계약 위반 값을 반환하며 정상 종료하면 안 된다. 진단 문구로 위반 종류를 구분한다.
                    var result = await Run("--negative-control", "math-invalid-" + operation);
                    Verify.That(result.ExitCode != 0, "math contract violation did not fail");
                    Verify.Contains(diagnostic, result.StdErr, "math fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            foreach ((string control, string diagnostic) in new[] {
                ("optional-empty-value", "Optional::Value called on an empty Optional"),
                ("unique-ptr-empty", "UniquePtr dereferenced while empty"),
                ("hash-map-iterator-modified", "HashMap iterator used after structural modification"),
                ("shared-ptr-empty", "SharedPtr dereferenced while empty"),
                ("shared-ref-from-empty", "SharedPtr::ToSharedRef called on an empty pointer"),
                ("sparse-array-free-index", "SparseArray index is not allocated"),
                ("slot-array-stale-handle", "SlotArray handle is stale or invalid"),
                ("time-span-overflow", "TimeSpan arithmetic overflow"),
                ("date-time-out-of-range", "DateTime arithmetic is outside the supported range"),
                ("variant-wrong-alternative", "Variant::Get called with a different alternative"),
                ("delegate-rebind-during-target-destruction", "Delegate rebound while its target is being destroyed"),
                ("unique-ptr-null-deleter", "UniquePtr adopted an object with a null deleter"),
                ("delegate-execute-unbound", "Delegate::Execute called on an unbound delegate"),
                ("delegate-rebind-during-execution", "Delegate rebound during its own execution"),
                ("random-invalid-0", "RandomStream::NextUInt32Below requires bound > 0"),
                ("random-invalid-1", "RandomStream::NextInt32InRange requires minimum <= maximum"),
                ("random-invalid-2", "RandomStream::NextFloatInRange requires finite minimum < maximum") })
                suite.Add("Native.Fatal." + control, async () =>
                {
                    var result = await Run("--negative-control", control);
                    Verify.That(result.ExitCode != 0, "empty value access did not fail");
                    Verify.Contains(diagnostic, result.StdErr, "empty value fatal diagnostic");
                });
        if (string.Equals(Path.GetFileNameWithoutExtension(executable), "FoundationTests", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(Path.GetFileNameWithoutExtension(executable), "CoreFoundationTests", StringComparison.OrdinalIgnoreCase))
            suite.Add("Native.Fatal.bit-array-invalid-index", async () =>
            {
                var result = await Run("--negative-control", "bit-array-invalid-index");
                Verify.That(result.ExitCode != 0, "out-of-range bit access did not fail");
                Verify.Contains("BitArray index is out of range", result.StdErr, "bit array fatal diagnostic");
            });
        if (options.Asan)
            suite.Add("Native.AsanDetectsActualUseAfterFree", async () =>
            {
                var result = await Run("--negative-control", "asan-uaf");
                Verify.That(result.ExitCode != 0, "ASan negative control succeeded");
                Verify.Contains("AddressSanitizer: heap-use-after-free", result.StdErr, "actual ASan report");
            });
        if (options.Optional("compiler") is string compiler)
        {
            suite.Add("Native.CompilerRejectsIntentionalWarning", () => WarningProbe.VerifyAsync(compiler, options.Root, environment));
            // 지시문 무효 확인의 음성 대조: 원본의 억제가 실제 경고(-Wunused-parameter)를 가리면 실패해야 하고,
            // 가리는 것이 없으면 통과해야 한다.
            suite.Add("Native.ThirdPartyDirectiveProbeControls", async () =>
            {
                foreach (bool hides in new[] { false, true })
                {
                    using TemporaryDirectory fixture = new();
                    string library = "Core/Source/ThirdParty/Fake";
                    string body = hides ? "int Fake(int unused) { return 0; }\n" : "int Fake(int used) { return used; }\n";
                    fixture.Write(library + "/fake-1.0/fake.c", "#pragma clang diagnostic ignored \"-Wunused-parameter\"\n" +
                                                                "#pragma warning(disable : 4100)\n" + body);
                    fixture.Write(library + "/ThirdParty.json", JsonSerializer.Serialize(new Dictionary<string, object>
                    {
                        ["schema"] = 1, ["name"] = "Fake", ["version"] = "1.0", ["upstreamDirectory"] = "fake-1.0",
                        ["source"] = new { url = "https://example.invalid/fake.tar.gz", sha256 = new string('0', 64), verification = "fixture" },
                        ["license"] = new { spdx = "BSD-2-Clause", upstreamExpression = "BSD-2-Clause", file = "fake-1.0/fake.c", binaryNoticeRequired = true },
                        ["translationUnits"] = new[] { "fake-1.0/fake.c" }, ["includeDirectories"] = Array.Empty<string>(),
                        ["defines"] = Array.Empty<string>(), ["platformDefines"] = new Dictionary<string, string[]>(),
                        ["consumerDefines"] = Array.Empty<string>(), ["adapterHeaders"] = new[] { "fake-1.0/fake.c" },
                        ["files"] = new Dictionary<string, string> { ["fake.c"] = new string('0', 64) },
                        ["pragmaDirectives"] = Array.Empty<object>(), ["notes"] = Array.Empty<string>()
                    }));
                    string source = Path.GetFullPath(fixture.FilePath(library + "/fake-1.0/fake.c"));
                    bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
                    string[] arguments = clangCl
                        ? [compiler, "/nologo", "/W4", "/WX", "/clang:-std=c17", "/c", "/TC", source, "/Fo" + fixture.FilePath("fake.obj")]
                        : [compiler, "-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-c", "-x", "c", source, "-o", fixture.FilePath("fake.o")];
                    fixture.Write("compile_commands.json", JsonSerializer.Serialize(new[] { new { directory = fixture.Root, file = source, arguments } }));
                    try
                    {
                        int verified = await ThirdPartyDirectiveProbe.VerifyAsync(fixture.Root, fixture.FilePath("compile_commands.json"), environment);
                        Verify.That(!hides && verified == 1, "directive probe missed a warning hidden by an upstream pragma");
                    }
                    catch (InvalidOperationException error) when (hides)
                    {
                        Verify.Contains("hide warnings", error.Message, "directive probe failure diagnostic");
                    }
                }
            });
        }
        if (options.Optional("compile-commands") is string database)
        {
            suite.Add("Native.CompileCommandsWarningPolicy", () =>
            {
                var failures = PolicyChecks.CompilationDatabase(database);
                Verify.That(failures.Count == 0, string.Join('\n', failures));
            });
            // 외부 원본의 진단 지시문을 지운 사본도 같은 인자로 경고 0이어야 한다(Compression.ko.md 3.6 규칙 2).
            suite.Add("Native.ThirdPartyDirectivesHideNoWarnings", async () =>
            {
                int verified = await ThirdPartyDirectiveProbe.VerifyAsync(options.Root, database, environment);
                Verify.That(verified != 0 || !File.ReadAllText(database).Contains("ThirdParty", StringComparison.Ordinal),
                    "third-party translation units in the compile database were not verified");
            });
        }
    }
}
