using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Rocks.ToolTests;

/// <summary>
/// 공통 실행 계층(Launch, Docs/Design/CrashHandlingAndLaunch.ko.md LC3)의 자식 프로세스 시험이다. 시험 실행 파일(Programs/LaunchTests:
/// LaunchProbe·LaunchWindowedProbe·LaunchEmbeddedHost)은 native 시험 실행 파일 옆에 있다. 유니코드 인자, 정상 시작·종료(도우미가
/// 남지 않음), 명령줄 캡처 실패(초기화 실패), 종료 코드, 정적 소멸 한 번, 크래시 메타데이터의 빌드 식별·인자 수, Windows GUI
/// subsystem, 내장 호스트를 단언한다. 자식마다 제한 시간을 둔다.
/// </summary>
internal static partial class LaunchTests
{
    private static readonly TimeSpan ChildTimeout = TimeSpan.FromSeconds(60);

    // 크래시 도우미는 부모가 끝난 뒤에도 덤프를 쓸 수 있다. 시험은 도우미 종료를 기다린 뒤 파일을 본다.
    private static readonly TimeSpan HelperTimeout = TimeSpan.FromSeconds(120);

    // LaunchProbe.h·Launch/Application.h의 종료 코드다.
    private const int ProbeUsageExitCode = 64;

    private const int CommandLineFailureExitCode = 1;

    private const int TeardownExitCode = 7;

    // LaunchProbe.h의 allocator-leak 종료 코드다.
    private const int AllocatorLeakExitCode = 9;

    // LaunchProbe.h의 signal-* 환경 종료 코드다.
    private const int SignalIgnoredExitCode = 65;

    private const int SharedConsoleExitCode = 66;

    // POSIX 셸 종료 코드(128 + 신호 번호)다. SIGBUS는 macOS 10, Linux·Android 7이다.
    private const int PosixInterruptExitCode = 128 + 2;

    private const int PosixAbortExitCode = 128 + 6;

    private const int PosixSegmentationFaultExitCode = 128 + 11;

    private const int PosixBusErrorExitCode = 128 + 10;

    private const int PosixBusErrorLinuxExitCode = 128 + 7;

    private const int HostHandledExitCode = 77;

    private const uint AccessViolation = 0xC0000005;

    private const uint RocksFatal = 0xE0524B46;

    private const string StaticTeardownLine = "LAUNCH STATIC TEARDOWN";

    [GeneratedRegex(@"LAUNCH ARG (\d+) ([0-9a-f]*)")]
    private static partial Regex ArgumentLine();

    [GeneratedRegex(@"LAUNCH EXE ([0-9a-f]*)")]
    private static partial Regex ExecutableLine();

    [GeneratedRegex(@"LAUNCH FOLDER ([0-9a-f]*)")]
    private static partial Regex FolderLine();

    [GeneratedRegex(@"LAUNCH HELPER PID (\d+)")]
    private static partial Regex HelperLine();

    private sealed record LaunchResult(int ExitCode, string StdErr, bool HelperExitedWithParent);

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        string directory = Path.GetDirectoryName(Path.GetFullPath(options.Required("native-executable")))!;
        bool windows = OperatingSystem.IsWindows();
        // 덤프·도우미는 Windows에서만, Asan 구성은 빼고 기대한다(설계 13.2절). Asan의 하드웨어 예외는 ASan이 보고한 뒤 death
        // callback이 Fatal 경로로 끝낸다(종료 코드는 Rocks Fatal 코드).
        bool dumpsExpected = windows && !options.Asan;
        // 덤프가 없을 때 접근 위반 크래시의 종료 코드다. Windows Asan은 death callback의 Rocks Fatal 코드다. POSIX Asan은 ASan
        // 보고 뒤 death callback의 Fatal 경로라 SIGABRT만 받고, 그 밖의 POSIX는 원래 신호(SIGSEGV·SIGBUS)다.
        bool PosixOrFatalCrashExit(int exitCode)
        {
            if (windows)
            {
                return unchecked((uint)exitCode) == RocksFatal;
            }

            if (options.Asan)
            {
                return exitCode == PosixAbortExitCode;
            }

            return exitCode is PosixSegmentationFaultExitCode or PosixBusErrorExitCode or PosixBusErrorLinuxExitCode;
        }

        // 크래시 수집 없이 난 Fatal의 종료 코드다(Windows는 Rocks Fatal 코드, POSIX는 SIGABRT).
        bool FatalExit(int exitCode)
        {
            return windows ? unchecked((uint)exitCode) == RocksFatal : exitCode == PosixAbortExitCode;
        }

        string Program(string name) => Path.Combine(directory, name + (windows ? ".exe" : ""));
        string console = Program("LaunchProbe");
        string windowed = Program("LaunchWindowedProbe");
        string embedded = Program("LaunchEmbeddedHost");

        // 자식을 띄우고 stderr를 줄 단위로 읽는다. 도우미 pid 줄을 읽는 순간 Process 객체를 잡아(pid 재사용 방지) 부모가 끝난 직후
        // 도우미가 이미 끝났는지 본다(정상 종료는 도우미를 먼저 끝낸다).
        async Task<LaunchResult> Run(string executable, params string[] arguments)
        {
            Verify.That(File.Exists(executable), "Launch test executable missing: " + executable);
            var start = new ProcessStartInfo(executable)
            {
                WorkingDirectory = options.Root,
                UseShellExecute = false,
                RedirectStandardError = true,
                RedirectStandardInput = true,
                CreateNoWindow = true,
                StandardErrorEncoding = new UTF8Encoding(false)
            };
            start.Environment.Clear();
            foreach (var pair in environment)
            {
                start.Environment[pair.Key] = pair.Value;
            }

            foreach (string argument in arguments)
            {
                start.ArgumentList.Add(argument);
            }

            using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start " + executable);
            Process? helper = null;
            try
            {
                process.StandardInput.Close();
                var text = new StringBuilder();
                async Task ReadErrors()
                {
                    while (await process.StandardError.ReadLineAsync() is { } line)
                    {
                        text.Append(line).Append('\n');
                        Match match = HelperLine().Match(line);
                        if (match.Success && int.Parse(match.Groups[1].Value) != 0)
                        {
                            try
                            {
                                helper = Process.GetProcessById(int.Parse(match.Groups[1].Value));
                            }
                            catch (ArgumentException)
                            {
                                helper = null;
                            }
                        }
                    }
                }

                await Task.WhenAll(ReadErrors(), process.WaitForExitAsync()).WaitAsync(ChildTimeout);
                bool helperExited = helper == null || helper.HasExited;
                if (helper != null)
                {
                    await helper.WaitForExitAsync().WaitAsync(HelperTimeout);
                }

                return new LaunchResult(process.ExitCode, text.ToString(), helperExited);
            }
            finally
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                }

                helper?.Dispose();
            }
        }

        // 인자를 hex 줄로 돌려받아 UTF-8 byte가 그대로인지 본다. 실행 파일 경로는 OS가 준 절대 경로다.
        async Task VerifyArguments(string executable, string name)
        {
            string[] arguments = ["args", "한글 인자", "😀", "a\"b c", "", "trailing\\", "--literal"];
            LaunchResult result = await Run(executable, arguments);
            Verify.Equal(0, result.ExitCode, name + " args exit\n" + result.StdErr);
            MatchCollection lines = ArgumentLine().Matches(result.StdErr);
            Verify.Equal(arguments.Length, lines.Count, name + " argument lines\n" + result.StdErr);
            for (int index = 0; index < arguments.Length; ++index)
            {
                Verify.Equal(index.ToString(System.Globalization.CultureInfo.InvariantCulture), lines[index].Groups[1].Value, name + " argument index");
                Verify.Equal(Convert.ToHexStringLower(Encoding.UTF8.GetBytes(arguments[index])), lines[index].Groups[2].Value,
                    name + " argument " + index + " bytes");
            }

            Match exe = ExecutableLine().Match(result.StdErr);
            Verify.That(exe.Success, name + " executable line missing\n" + result.StdErr);
            string path = Encoding.UTF8.GetString(Convert.FromHexString(exe.Groups[1].Value));
            Verify.That(string.Equals(Path.GetFullPath(path), Path.GetFullPath(executable),
                windows ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal), name + " executable path: " + path);
        }

        suite.Add("Native.Launch.unicode-arguments", () => VerifyArguments(console, "LaunchProbe"));

        // 본문의 반환값이 종료 코드다. mode가 없거나 모르면 사용법 종료 코드다.
        suite.Add("Native.Launch.exit-codes", async () =>
        {
            Verify.Equal(42, (await Run(console, "exit", "42")).ExitCode, "exit 42");
            Verify.Equal(0, (await Run(console, "exit", "0")).ExitCode, "exit 0");
            Verify.Equal(ProbeUsageExitCode, (await Run(console)).ExitCode, "no mode");
            Verify.Equal(ProbeUsageExitCode, (await Run(console, "unknown-mode")).ExitCode, "unknown mode");
        });

        // 정상 시작·종료: 크래시 수집과 도우미(Full), 설정이 끈 수집(Off), 도우미 없는 수집(InProcess), 도구 로그. 정상 종료는
        // 도우미를 먼저 끝낸다(부모가 끝난 직후 도우미가 남아 있지 않다).
        suite.Add("Native.Launch.start-and-stop", async () =>
        {
            LaunchResult full = await Run(console, "state");
            Verify.Equal(0, full.ExitCode, "state exit\n" + full.StdErr);
            Verify.Contains("LAUNCH STATE crash=1 helper=" + (dumpsExpected ? "1" : "0") + " toolLog=0 terminationHandler=0", full.StdErr,
                "Full state");
            Verify.That(full.HelperExitedWithParent, "normal exit left the crash helper running");
            LaunchResult off = await Run(console, "--crash-off", "state");
            Verify.Contains("LAUNCH STATE crash=0 helper=0 toolLog=0", off.StdErr, "Off state");
            LaunchResult inProcess = await Run(console, "--in-process", "--terminate", "state");
            Verify.Contains("LAUNCH STATE crash=1 helper=0 toolLog=0 terminationHandler=1", inProcess.StdErr, "InProcess state");
            LaunchResult log = await Run(console, "--tool-log", "log");
            Verify.Equal(0, log.ExitCode, "tool log exit\n" + log.StdErr);
            Verify.Contains("launch probe tool log line", log.StdErr, "tool log line");
            Verify.Contains("LaunchProbe", log.StdErr, "tool log category");
        });

        // 정적 소멸은 정상 종료에서 한 번 돈다. RequestExitWithoutStaticTeardown이면 정리 뒤 _Exit로 끝나 돌지 않는다.
        suite.Add("Native.Launch.static-teardown", async () =>
        {
            LaunchResult normal = await Run(console, "teardown");
            Verify.Equal(TeardownExitCode, normal.ExitCode, "teardown exit\n" + normal.StdErr);
            Verify.Equal(1, Regex.Matches(normal.StdErr, StaticTeardownLine).Count, "static teardown runs once\n" + normal.StdErr);
            LaunchResult skipped = await Run(console, "no-teardown");
            Verify.Equal(TeardownExitCode, skipped.ExitCode, "no-teardown exit\n" + skipped.StdErr);
            Verify.That(!skipped.StdErr.Contains(StaticTeardownLine, StringComparison.Ordinal), "static teardown ran after _Exit");
            Verify.That(skipped.HelperExitedWithParent, "no-teardown exit left the crash helper running");
        });

        // 범용 할당자(GC 규모 설계 18.10절): 기본은 Launch의 시스템 할당자 위에 둔 BinnedAllocator다 — 작은 요청은 시스템 할당자에
        // 닿지 않고 큰 요청만 닿는다. System 설정이면 두 접근자가 같은 객체다. 본문이 할당을 남기면 Launch의 정리가 Fatal이고, 정적
        // 소멸 없이 끝내는 경로는 할당자를 파괴하지 않아 Fatal 없이 본문의 종료 코드로 끝난다.
        suite.Add("Native.Launch.default-allocator", async () =>
        {
            LaunchResult binned = await Run(console, "--crash-off", "allocator");
            Verify.Equal(0, binned.ExitCode, "default allocator exit\n" + binned.StdErr);
            Verify.Contains("LAUNCH ALLOCATOR kind=binned distinct=1 smallInSystem=0 largeInSystem=1 clean=1", binned.StdErr,
                "default allocator is the binned allocator");
            LaunchResult system = await Run(console, "--crash-off", "--system-allocator", "allocator");
            Verify.Equal(0, system.ExitCode, "system allocator exit\n" + system.StdErr);
            Verify.Contains("LAUNCH ALLOCATOR kind=system distinct=0 smallInSystem=64 largeInSystem=1 clean=1", system.StdErr,
                "system setting hands out the system allocator");
            LaunchResult leaked = await Run(console, "--crash-off", "allocator-leak");
            Verify.Contains("LAUNCH ALLOCATION LEFT", leaked.StdErr, "leak probe body");
            Verify.That(FatalExit(leaked.ExitCode), "leaked allocation did not end in a fatal exit: 0x" + leaked.ExitCode.ToString("X8") + "\n" + leaked.StdErr);
            Verify.Contains("BinnedAllocator destroyed with live allocations", leaked.StdErr, "binned allocator leak diagnostic");
            LaunchResult systemLeaked = await Run(console, "--crash-off", "--system-allocator", "allocator-leak");
            Verify.That(FatalExit(systemLeaked.ExitCode),
                "leaked system allocation did not end in a fatal exit: 0x" + systemLeaked.ExitCode.ToString("X8") + "\n" + systemLeaked.StdErr);
            Verify.Contains("SystemAllocator destroyed with live allocations", systemLeaked.StdErr, "system allocator leak diagnostic");
            LaunchResult skipped = await Run(console, "--crash-off", "allocator-leak-no-teardown");
            Verify.Equal(AllocatorLeakExitCode, skipped.ExitCode, "no-teardown leak exit\n" + skipped.StdErr);
            Verify.That(!skipped.StdErr.Contains("destroyed with live allocations", StringComparison.Ordinal),
                "no-teardown exit destroyed the default allocator\n" + skipped.StdErr);
        });

        // 명령줄 캡처 실패(초기화 실패)는 본문을 부르지 않고 종료 코드 1과 원인 한 줄이다. 시작한 단계(이른 크래시 수집)만 정리한다.
        suite.Add("Native.Launch.command-line-failure", async () =>
        {
            if (windows)
            {
                // 짝 없는 UTF-16 surrogate는 UTF-8로 바꿀 수 없다.
                LaunchResult invalid = await Run(console, "args", "bad\uD800");
                Verify.Equal(CommandLineFailureExitCode, invalid.ExitCode, "invalid UTF-16 exit\n" + invalid.StdErr);
                Verify.Contains("Rocks launch: the command line arguments could not be captured (InvalidArgument)", invalid.StdErr,
                    "invalid UTF-16 diagnostic");
                Verify.That(!invalid.StdErr.Contains("LAUNCH ARG", StringComparison.Ordinal), "body ran after a capture failure");
            }

            // 상한(32768개·2 MiB)은 OS 명령줄 한계(Windows 32767 UTF-16 단위)보다 커서 자식으로는 넘길 수 없다. 많은 빈 인자가
            // 그대로 오는지만 보고, 상한 초과는 지역 저장소 시험(ProcessCommandLine_EnforcesLimits)이 본다.
            string[] many = ["args", .. Enumerable.Repeat("", 4097)];
            LaunchResult manyArguments = await Run(console, many);
            Verify.Equal(0, manyArguments.ExitCode, "4097 empty arguments exit\n" + manyArguments.StdErr);
            Verify.Equal(many.Length, ArgumentLine().Matches(manyArguments.StdErr).Count, "4097 empty arguments lines");
        });

        // 설정 단계(configure 뒤)의 경고: 잘못된 예산은 떼기 전에 거절하고 이전 설정(Full)을 유지한다. 없는 도우미 경로는 시작 실패
        // 경고를 남긴다(크래시 때 프로세스 안 덤프).
        suite.Add("Native.Launch.settings-warnings", async () =>
        {
            LaunchResult badBudget = await Run(console, "--bad-budget", "state");
            Verify.Equal(0, badBudget.ExitCode, "bad budget exit\n" + badBudget.StdErr);
            Verify.Contains("The crash reporting settings are invalid; keeping the previous ones", badBudget.StdErr, "bad budget warning");
            Verify.Contains("LAUNCH STATE crash=1 helper=" + (dumpsExpected ? "1" : "0"), badBudget.StdErr, "bad budget keeps crash reporting");
            if (dumpsExpected)
            {
                using var temporary = new TemporaryDirectory();
                LaunchResult missing = await Run(console, "--crash-helper", temporary.FilePath("missing-helper.exe"), "state");
                Verify.Equal(0, missing.ExitCode, "missing helper exit\n" + missing.StdErr);
                Verify.Contains("The crash dump helper could not start; crashes will be written in process", missing.StdErr,
                    "missing helper warning");
                Verify.Contains("LAUNCH STATE crash=1 helper=0", missing.StdErr, "missing helper state");
            }
        });

        // 새 설정(InProcess)과 새 폴더로 다시 걸기가 실패하면(상대 폴더가 작업 폴더와 합쳐 상한을 넘음) 이전 설정·이전 폴더로
        // 되돌려 걸고, 새 폴더는 따로 시도해 경고한다. 수집기는 남는다.
        suite.Add("Native.Launch.settings-rollback", async () =>
        {
            string longRelative = string.Join('/', Enumerable.Repeat("rollback-segment", 60));
            LaunchResult rolled = await Run(console, "--in-process", "--crash-dir", longRelative, "state");
            Verify.Equal(0, rolled.ExitCode, "rollback exit\n" + rolled.StdErr);
            Verify.Contains("The crash reporting settings could not be installed; restoring the previous ones", rolled.StdErr,
                "rollback warning");
            Verify.Contains("keeping the previous one", rolled.StdErr, "rollback directory warning");
            Verify.Contains("LAUNCH STATE crash=1 helper=" + (dumpsExpected ? "1" : "0"), rolled.StdErr, "rollback keeps crash reporting");
            // 되돌린 뒤의 크래시 폴더는 이전(기본) 폴더 아래다(새 긴 폴더가 아니다).
            Match folder = FolderLine().Match(rolled.StdErr);
            Verify.That(folder.Success, "rollback folder line missing\n" + rolled.StdErr);
            string path = Encoding.UTF8.GetString(Convert.FromHexString(folder.Groups[1].Value));
            Verify.That(!path.Contains("rollback-segment", StringComparison.Ordinal) &&
                path.Replace('\\', '/').Contains("/Saved/Crashes/LaunchProbe-", StringComparison.Ordinal), "rollback folder: " + path);
        });

        // 실제 OS 종료 요청(Windows 콘솔 Ctrl-Break, POSIX SIGINT, 설계 20절·24.2절 5). 처리기가 있으면 첫 요청은 원자 표시만
        // 남기고 프로그램이 이어 간다. 두 번째 요청과 처리기가 없는 경우는 OS 기본 동작으로 바로 끝난다(Windows
        // STATUS_CONTROL_C_EXIT, POSIX 128+SIGINT).
        suite.Add("Native.Launch.termination-signal", async () =>
        {
            int interrupted = windows ? unchecked((int)0xC000013A) : PosixInterruptExitCode;
            // 부모(harness)가 SIGINT를 무시로 물려받은 환경(nohup·백그라운드)이면 자식도 무시를 물려받아 Launch가 처리기를
            // 걸지 않는다. .NET은 자식의 신호 처분을 고칠 수 없으므로 probe가 configure에서 기본 동작으로 되돌린다
            // (--reset-sigint). 그런데도 무시로 남았으면(종료 코드 SignalIgnoredExitCode) 되돌리기의 회귀이므로 실패다. Windows에서
            // 콘솔을 나눠 쓰면 실패다(다른 프로세스에 Ctrl-Break가 간다).
            LaunchResult handled = await Run(console, "--reset-sigint", "--terminate", "signal-self");
            Verify.That(handled.ExitCode != SignalIgnoredExitCode, "SIGINT stayed ignored after --reset-sigint\n" + handled.StdErr);
            Verify.That(handled.ExitCode != SharedConsoleExitCode, "the probe console is shared with other processes\n" + handled.StdErr);
            Verify.Equal(0, handled.ExitCode, "handled termination exit\n" + handled.StdErr);
            Verify.Contains("LAUNCH TERMINATION REQUESTED", handled.StdErr, "termination flag line");
            LaunchResult twice = await Run(console, "--reset-sigint", "--terminate", "signal-twice");
            Verify.Contains("LAUNCH TERMINATION REQUESTED", twice.StdErr, "first request line");
            Verify.Equal(interrupted, twice.ExitCode, "second request exit\n" + twice.StdErr);
            LaunchResult unhandled = await Run(console, "--reset-sigint", "signal-self");
            Verify.Equal(interrupted, unhandled.ExitCode, "unhandled termination exit\n" + unhandled.StdErr);
            Verify.That(!unhandled.StdErr.Contains("LAUNCH TERMINATION REQUESTED", StringComparison.Ordinal), "unhandled request set the flag");
        });

        // 상속된 SIGINT 무시는 Launch가 보존한다(nohup 등, 설계 25절 LC3 리뷰). probe가 configure에서 SIGINT를 무시로 둔
        // 뒤(--ignore-sigint) --terminate로 시작하면 Launch는 처리기를 걸지 않으므로 signal-self가 보내기 전에 무시 처분을 보고
        // SignalIgnoredExitCode로 끝난다. 처리기를 걸었으면 신호를 보내 0으로 끝난다. Windows Ctrl-Break에는 상속되는 무시가 없다.
        if (!windows)
        {
            suite.Add("Native.Launch.sigint-ignored-preserved", async () =>
            {
                LaunchResult result = await Run(console, "--ignore-sigint", "--terminate", "signal-self");
                Verify.Equal(SignalIgnoredExitCode, result.ExitCode, "ignored SIGINT exit\n" + result.StdErr);
                Verify.Contains("LAUNCH SIGINT IGNORED", result.StdErr, "ignored SIGINT line");
            });
        }

        // 설치는 잘못된 옵션을 InvalidArgument로 거절하고 아무것도 걸지 않는다(수집기가 없는 프로세스에서).
        suite.Add("Native.Launch.install-invalid", async () =>
        {
            LaunchResult result = await Run(console, "--crash-off", "install-invalid");
            Verify.Equal(0, result.ExitCode, "install-invalid exit\n" + result.StdErr);
            Verify.Contains("LAUNCH INSTALL REJECTED", result.StdErr, "install-invalid line");
        });

        // 설정이 수집을 끈 뒤에도 RequestExitWithoutStaticTeardown은 정적 소멸 없이 끝낸다.
        suite.Add("Native.Launch.off-no-teardown", async () =>
        {
            LaunchResult skipped = await Run(console, "--crash-off", "no-teardown");
            Verify.Equal(TeardownExitCode, skipped.ExitCode, "off no-teardown exit\n" + skipped.StdErr);
            Verify.That(!skipped.StdErr.Contains(StaticTeardownLine, StringComparison.Ordinal), "static teardown ran after _Exit");
        });

        // 설정 단계가 수집기를 다시 건 뒤(Full → InProcess, 다른 폴더)의 실제 크래시 보고: 새 폴더에 프로세스 안 덤프가 있고
        // helper는 null이다.
        suite.Add("Native.Launch.reinstalled-crash", async () =>
        {
            using var temporary = new TemporaryDirectory();
            string root = temporary.FilePath("Crashes");
            LaunchResult result = await Run(console, "--in-process", "--crash-dir", root, "crash");
            if (dumpsExpected)
            {
                Verify.Equal(unchecked((int)AccessViolation), result.ExitCode, "in-process crash exit\n" + result.StdErr);
            }
            else
            {
                Verify.That(PosixOrFatalCrashExit(result.ExitCode), "in-process crash signal exit " + result.ExitCode + "\n" + result.StdErr);
            }

            string[] folders = Directory.GetDirectories(root, "LaunchProbe-*");
            Verify.Equal(1, folders.Length, "in-process crash folders under " + root + "\n" + result.StdErr);
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folders[0], "CrashInfo.json")));
            Verify.Equal(JsonValueKind.Null, metadata.RootElement.GetProperty("helper").ValueKind,
                "in-process helper: " + metadata.RootElement.GetProperty("helper"));
            if (dumpsExpected)
            {
                Verify.That(File.Exists(Path.Combine(folders[0], "Minidump.dmp")), "in-process Minidump.dmp missing");
                using var dump = File.OpenRead(Path.Combine(folders[0], "Minidump.dmp"));
                var magic = new byte[4];
                Verify.Equal(4, dump.Read(magic, 0, 4), "dump header");
                Verify.Equal("MDMP", Encoding.ASCII.GetString(magic), "dump signature");
            }
        });

        // 본문 크래시: 크래시 폴더의 CrashInfo.json에 Launch가 넘긴 빌드 식별(엔진 버전·도구 빌드 ID)·인자 수·단계가 있고, 도우미가
        // 덤프를 쓴다. PDB 경로는 파일 이름만이다(/PDBALTPATH).
        suite.Add("Native.Launch.crash-metadata", async () =>
        {
            using var temporary = new TemporaryDirectory();
            string root = temporary.FilePath("Crashes");
            LaunchResult result = await Run(console, "--crash-dir", root, "crash");
            if (dumpsExpected)
            {
                Verify.Equal(unchecked((int)AccessViolation), result.ExitCode, "crash exit\n" + result.StdErr);
            }
            else
            {
                Verify.That(PosixOrFatalCrashExit(result.ExitCode), "crash signal exit " + result.ExitCode + "\n" + result.StdErr);
            }

            string[] folders = Directory.GetDirectories(root, "LaunchProbe-*");
            Verify.Equal(1, folders.Length, "crash folders under " + root + "\n" + result.StdErr);
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folders[0], "CrashInfo.json")));
            JsonElement json = metadata.RootElement;
            Verify.Equal("LaunchProbe", json.GetProperty("program").GetString(), "program");
            Verify.Equal(3, json.GetProperty("arguments").GetProperty("count").GetInt32(), "arguments.count");
            Verify.Equal("Running", json.GetProperty("phase").GetString(), "phase");
            Verify.Equal("LaunchProbe.Crash", json.GetProperty("stage").GetString(), "stage");
            JsonElement build = json.GetProperty("build");
            Verify.That(Regex.IsMatch(build.GetProperty("engineVersion").GetString() ?? "", @"^\d+\.\d+\.\d+$"), "build.engineVersion: " + build);
            Verify.That(Regex.IsMatch(build.GetProperty("toolBuildId").GetString() ?? "", "^[0-9a-f]{64}$"), "build.toolBuildId: " + build);
            if (dumpsExpected)
            {
                Verify.Equal("Running", json.GetProperty("helper").GetProperty("status").GetString(), "helper status");
                Verify.That(File.Exists(Path.Combine(folders[0], "Minidump.dmp")), "Minidump.dmp missing");
                Verify.Equal("LaunchProbe.pdb", json.GetProperty("module").GetProperty("pdbName").GetString(), "module.pdbName");
            }
        });

        if (!windows)
        {
            return;
        }

        // GUI subsystem 실행 파일(wWinMain, 콘솔 창 없음)도 같은 인자를 받는다. PE 머리의 subsystem과 CodeView PDB 이름(파일 이름만)을
        // 확인한다.
        suite.Add("Native.Launch.windowed-subsystem", async () =>
        {
            foreach ((string path, Subsystem expected, string pdb) in new[] {
                (console, Subsystem.WindowsCui, "LaunchProbe.pdb"),
                (windowed, Subsystem.WindowsGui, "LaunchWindowedProbe.pdb") })
            {
                using var stream = File.OpenRead(path);
                using var reader = new PEReader(stream);
                Verify.Equal(expected, reader.PEHeaders.PEHeader!.Subsystem, path + " subsystem");
                DebugDirectoryEntry codeView = reader.ReadDebugDirectory().Single(entry => entry.Type == DebugDirectoryEntryType.CodeView);
                Verify.Equal(pdb, reader.ReadCodeViewDebugDirectoryData(codeView).Path, path + " CodeView PDB path");
            }

            await VerifyArguments(windowed, "LaunchWindowedProbe");
            Verify.Equal(42, (await Run(windowed, "exit", "42")).ExitCode, "windowed exit 42");
        });

        // 내장 호스트: Rocks는 호스트의 처리되지 않은 예외 filter를 바꾸지 않고, 크래시를 보고(CrashInfo.json, 덤프 없음)한 뒤 호스트
        // filter로 넘긴다. 프로세스 종료는 호스트가 정한다(77). Fatal도 같은 예외로 호스트에 간다.
        suite.Add("Native.Launch.embedded-host", async () =>
        {
            LaunchResult normal = await Run(embedded, "exit", "5");
            Verify.Equal(5, normal.ExitCode, "embedded exit\n" + normal.StdErr);
            Verify.Contains("LAUNCH HOST FILTER INTACT", normal.StdErr, "host filter replaced");
            LaunchResult state = await Run(embedded, "state");
            Verify.Contains("LAUNCH STATE crash=1 helper=0", state.StdErr, "embedded state");
            if (options.Asan)
            {
                // Asan 구성은 ASan runtime이 호스트보다 먼저 접근 위반을 받아 보고하고 끝낸다(호스트 filter까지 가지 않는다). 내장
                // 진입은 death callback을 걸지 않는다(호스트 프로세스 정책). 크래시 전달 단언은 Asan이 아닌 구성에서만 한다.
                return;
            }

            using var temporary = new TemporaryDirectory();
            string root = temporary.FilePath("Crashes");
            LaunchResult crash = await Run(embedded, "--crash-dir", root, "crash");
            Verify.Equal(HostHandledExitCode, crash.ExitCode, "embedded crash exit\n" + crash.StdErr);
            Verify.Contains("LAUNCH HOST FILTER 0xC0000005", crash.StdErr, "host filter did not receive the crash");
            string[] folders = Directory.GetDirectories(root, "LaunchEmbeddedHost-*");
            Verify.Equal(1, folders.Length, "embedded crash folders\n" + crash.StdErr);
            using (var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folders[0], "CrashInfo.json"))))
            {
                Verify.Contains("embedded host owns crash dumps", metadata.RootElement.GetProperty("limitations").ToString(),
                    "embedded limitations");
            }

            Verify.That(!File.Exists(Path.Combine(folders[0], "Minidump.dmp")), "embedded host wrote a dump");
            LaunchResult fatal = await Run(embedded, "--crash-off", "fatal");
            // 수집기가 없으면 Fatal은 예외를 내지 않고 바로 끝난다(D9). 호스트 filter도 거치지 않는다.
            Verify.Equal(unchecked((int)RocksFatal), fatal.ExitCode, "embedded fatal without crash reporting exit\n" + fatal.StdErr);
            Verify.That(!fatal.StdErr.Contains("LAUNCH HOST FILTER 0x", StringComparison.Ordinal), "Off fatal reached the host filter");
            LaunchResult reported = await Run(embedded, "--crash-dir", root, "fatal");
            Verify.Equal(HostHandledExitCode, reported.ExitCode, "embedded fatal exit\n" + reported.StdErr);
            Verify.Contains("LAUNCH HOST FILTER 0xE0524B46", reported.StdErr, "host filter did not receive the fatal exception");
            // 내장 여부는 진입 방식으로 정해진다. configure가 InProcess를 골라도 내장 보고 그대로이고(호스트 filter가 크래시를
            // 받는다), 종료 요청 처리기는 걸지 않는다(호스트의 Ctrl-C 처리를 지킨다). _Exit 요청도 무시한다(호스트가 끝낸다).
            using var other = new TemporaryDirectory();
            string otherRoot = other.FilePath("Crashes");
            LaunchResult inProcess = await Run(embedded, "--in-process", "--terminate", "--crash-dir", otherRoot, "crash");
            Verify.Equal(HostHandledExitCode, inProcess.ExitCode, "embedded --in-process crash exit\n" + inProcess.StdErr);
            Verify.Contains("LAUNCH HOST FILTER 0xC0000005", inProcess.StdErr, "embedded --in-process crash did not reach the host filter");
            string[] otherFolders = Directory.GetDirectories(otherRoot, "LaunchEmbeddedHost-*");
            Verify.Equal(1, otherFolders.Length, "embedded --in-process crash folders\n" + inProcess.StdErr);
            Verify.That(!File.Exists(Path.Combine(otherFolders[0], "Minidump.dmp")), "embedded --in-process wrote a dump");
            // 내장 진입은 정적 소멸 없이 끝내라는 요청을 무시하므로 범용 할당자를 파괴한다: 본문이 할당을 남기면 그 요청이 있어도
            // Fatal이다(단독 진입은 파괴하지 않고 본문의 종료 코드로 끝난다 — Native.Launch.default-allocator).
            LaunchResult embeddedLeak = await Run(embedded, "--crash-off", "allocator-leak-no-teardown");
            Verify.Contains("LAUNCH ALLOCATION LEFT", embeddedLeak.StdErr, "embedded leak probe body");
            Verify.Equal(unchecked((int)RocksFatal), embeddedLeak.ExitCode, "embedded no-teardown leak exit\n" + embeddedLeak.StdErr);
            Verify.Contains("BinnedAllocator destroyed with live allocations", embeddedLeak.StdErr, "embedded leak diagnostic");
            LaunchResult noTeardown = await Run(embedded, "no-teardown");
            Verify.Equal(TeardownExitCode, noTeardown.ExitCode, "embedded no-teardown exit\n" + noTeardown.StdErr);
            Verify.Contains(StaticTeardownLine, noTeardown.StdErr, "embedded no-teardown skipped static teardown");
            Verify.Contains("LAUNCH HOST FILTER INTACT", noTeardown.StdErr, "embedded no-teardown did not return to the host");
        });
    }
}
