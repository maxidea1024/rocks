using System.Diagnostics;
using System.Reflection.PortableExecutable;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
namespace Rocks.ToolTests;

/// <summary>
/// 크래시 수집기(Docs/Design/CrashHandlingAndLaunch.ko.md LC1a·LC1b·LC2)의 자식 probe 시험이다. 각 시험은 FoundationTests를
/// <c>--negative-control crash-&lt;mode&gt; &lt;crash directory&gt;</c>로 띄워 종료 코드, probe가 알린 스레드 id, 종료 콜백 줄을
/// 단언한다. Windows는 크래시 폴더의 CrashInfo.json과 Minidump.dmp를 C# 판독기(<see cref="MinidumpReader"/>)와 고정된 cdb
/// 명령(설계 17절)으로 확인한다. 자식마다 제한 시간을 두고, 넘기면 kill한 뒤 실패로 기록한다.
/// </summary>
internal static partial class CrashReportingTests
{
    // Platform/Fatal.h의 Rocks 종료 코드와 Windows 예외 코드다.
    private const uint RocksFatal = 0xE0524B46;

    private const uint AccessViolation = 0xC0000005;

    private const uint StackOverflow = 0xC00000FD;

    private const uint HeapCorruption = 0xC0000374;

    // TestSupport RaiseUserException의 사용자 예외 코드다(HostFailure.h).
    private const uint UserExceptionCode = 0xE0001234;

    // Platform/CrashReporting.h TerminationReason 값이다.
    private const int ReasonFatal = 0;

    private const int ReasonAssert = 1;

    private const int ReasonException = 2;

    private const int ReasonSignal = 3;

    private const int ReasonAbort = 4;

    private const int ReasonRuntimeCheck = 5;

    private const int ReasonSanitizer = 6;

    // POSIX는 신호로 끝난다(.NET ExitCode = 128 + 신호 번호). 접근 위반·stack overflow는 SIGSEGV 또는
    // SIGBUS(macOS 10, Linux·Android 7), Fatal·abort는 SIGABRT다.
    private const uint PosixSegmentationFault = 128 + 11;

    private const uint PosixBusError = 128 + 10;

    private const uint PosixBusErrorLinux = 128 + 7;

    private const uint PosixAbort = 128 + 6;

    // CrashReportingInternal.h의 덤프 사용자 stream 종류·형식 이름과 probe가 넣는 값이다(CrashProcessProbe.cpp).
    private const uint CrashRecordStreamType = 0x524B0001;

    private const uint CommentStreamA = 10;

    private const string CrashRecordStreamName = "RocksCrashRecord/1\n";

    private const string ProbeLogFile = "Saved/Logs/CrashProbe.log";

    private const string WorkerThreadName = "CrashProbeWorker";

    private static readonly TimeSpan ChildTimeout = TimeSpan.FromSeconds(90);

    private static readonly TimeSpan DebuggerTimeout = TimeSpan.FromSeconds(180);

    // 덤프 도우미는 부모가 끝난 뒤에도 덤프를 쓴다(종류별 마감까지). 시험은 도우미 종료를 기다린 뒤 파일을 본다.
    private static readonly TimeSpan HelperTimeout = TimeSpan.FromSeconds(120);

    // CrashProcessProbe.cpp의 고장 난 도우미 선택 환경 변수와 Windows 종료 코드다.
    private const string FaultyHelperVariable = "ROCKS_CRASH_PROBE_HELPER";

    private const uint FastFailExit = 0xC0000409;

    private const uint ControlCExit = 0xC000013A;

    private const uint KilledExit = 0xFFFFFFFF;

    [GeneratedRegex(@"crash probe (\w+) thread=(\d+)")]
    private static partial Regex ThreadLine();

    [GeneratedRegex(@"crash probe callback reason=(\d+) failing=(\d+) stopped=(\d+) first=(\d+) message=([^\r\n]*)")]
    private static partial Regex CallbackLine();

    [GeneratedRegex(@"crash probe helper pid=(\d+)")]
    private static partial Regex HelperLine();

    // CreateProcessW 생성 플래그다(job breakaway 전제 측정).
    private const uint CreateSuspended = 0x00000004;

    private const uint CreateBreakawayFromJob = 0x01000000;

    private const uint CreateNoWindow = 0x08000000;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public int Size;
        public IntPtr Reserved;
        public IntPtr Desktop;
        public IntPtr Title;
        public int X;
        public int Y;
        public int XSize;
        public int YSize;
        public int XCountChars;
        public int YCountChars;
        public int FillAttribute;
        public int Flags;
        public short ShowWindow;
        public short Reserved2Size;
        public IntPtr Reserved2;
        public IntPtr StandardInput;
        public IntPtr StandardOutput;
        public IntPtr StandardError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr Process;
        public IntPtr Thread;
        public int ProcessId;
        public int ThreadId;
    }

    [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcess(string applicationName, IntPtr commandLine, IntPtr processAttributes, IntPtr threadAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandles, uint creationFlags, IntPtr environment, string? currentDirectory,
        ref StartupInfo startupInfo, out ProcessInformation processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsProcessInJob(IntPtr process, IntPtr job, [MarshalAs(UnmanagedType.Bool)] out bool result);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcess(IntPtr process, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    // 이 시험 실행기 자리에서 CREATE_BREAKAWAY_FROM_JOB으로 만든 자식이 어느 job에도 없는가다(설계 17절 job 시험의 전제). 도우미와
    // 같은 호출을 직접 해 본다: executable을 CREATE_SUSPENDED로 띄워(실행되지 않는다) IsProcessInJob(child, null)을 보고 바로 끝낸다.
    // 바깥 job이 breakaway를 막으면 생성이 ERROR_ACCESS_DENIED로 실패한다. QueryInformationJobObject(null)는 바로 위 job만 보므로
    // 중첩 job에서 쓰지 않는다. 그 밖의 생성 실패는 전제를 정할 수 없으므로 예외다(시험 실패).
    private static bool BreakawayLeavesAllJobs(string executable)
    {
        const int AccessDenied = 5;
        const uint TerminateExitCode = 1;
        const uint WaitMilliseconds = 10000;
        var startup = new StartupInfo { Size = Marshal.SizeOf<StartupInfo>() };
        if (!CreateProcess(executable, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, false,
            CreateSuspended | CreateBreakawayFromJob | CreateNoWindow, IntPtr.Zero, null, ref startup, out ProcessInformation process))
        {
            int error = Marshal.GetLastWin32Error();
            if (error == AccessDenied)
            {
                return false;
            }

            throw new InvalidOperationException("breakaway probe process could not start: Win32 error " + error);
        }

        try
        {
            if (!IsProcessInJob(process.Process, IntPtr.Zero, out bool inJob))
            {
                throw new InvalidOperationException("IsProcessInJob failed: Win32 error " + Marshal.GetLastWin32Error());
            }

            return !inJob;
        }
        finally
        {
            _ = TerminateProcess(process.Process, TerminateExitCode);
            _ = WaitForSingleObject(process.Process, WaitMilliseconds);
            _ = CloseHandle(process.Thread);
            _ = CloseHandle(process.Process);
        }
    }

    private sealed record ProbeResult(uint ExitCode, int ProcessId, int HelperPid, bool HelperAliveAtExit, string StdErr,
        IReadOnlyDictionary<string, ulong> Threads, IReadOnlyList<Match> Callbacks);

    // 크래시 하나의 기대 값이다. Code는 Windows 종료·예외 코드, Frame은 cdb stack에 보여야 할 probe 함수 이름이다. Writer는 덤프를
    // 쓴 쪽(Helper·InProcess), HelperStatus는 메타데이터 helper.status(null이면 도우미를 쓰지 않음)다.
    private sealed record Expectation(string Mode, string Role, uint Code, int Reason, string Kind, string? Frame,
        string? ThreadName = null, string Writer = "Helper", string? HelperStatus = "Running");

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        string executable = Path.GetFullPath(options.Required("native-executable"));
        // probe는 시험 실행기 자신이고 Launch가 실행기 이름으로 수집기를 건다(크래시 폴더 접두·메타데이터 "program").
        string probeProgram = Path.GetFileNameWithoutExtension(executable);
        bool windows = OperatingSystem.IsWindows();
        int hardwareReason = windows ? ReasonException : ReasonSignal;

        // 도우미 pid 줄을 읽는 순간 Process 객체를 잡는다. 끝난 도우미의 pid가 재사용돼도 다른 프로세스를 보거나 kill하지 않는다.
        static Process? OpenHelper(int helperPid)
        {
            try
            {
                return Process.GetProcessById(helperPid);
            }
            catch (ArgumentException)
            {
                return null;
            }
        }

        // 도우미 프로세스가 끝날 때까지 기다린다(이미 끝났으면 바로 돌아온다).
        static async Task WaitForHelper(Process helper, string mode)
        {
            try
            {
                await helper.WaitForExitAsync().WaitAsync(HelperTimeout);
            }
            catch (TimeoutException)
            {
                helper.Kill();
                throw new InvalidOperationException(mode + " crash helper did not finish in time");
            }
        }

        // killAfterHelper면 도우미 pid 줄을 본 뒤 자식을 kill한다(harness kill 시험, 자식만이고 도우미는 kill하지 않는다).
        async Task<ProbeResult> Run(string mode, string? directory, IReadOnlyDictionary<string, string>? overrides = null,
            bool killAfterHelper = false)
        {
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

            foreach (var pair in overrides ?? new Dictionary<string, string>())
            {
                start.Environment[pair.Key] = pair.Value;
            }

            start.ArgumentList.Add("--negative-control");
            start.ArgumentList.Add("crash-" + mode);
            if (directory != null)
            {
                start.ArgumentList.Add(directory);
            }

            using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start crash probe");
            int helperPid = 0;
            Process? helperProcess = null;
            try
            {
                process.StandardInput.Close();
                var text = new StringBuilder();
                async Task ReadErrors()
                {
                    while (await process.StandardError.ReadLineAsync() is { } line)
                    {
                        text.Append(line).Append('\n');
                        Match helper = HelperLine().Match(line);
                        if (helper.Success)
                        {
                            helperPid = int.Parse(helper.Groups[1].Value);
                            helperProcess = helperPid != 0 ? OpenHelper(helperPid) : null;
                            if (killAfterHelper)
                            {
                                process.Kill(entireProcessTree: false);
                            }
                        }
                    }
                }

                await Task.WhenAll(ReadErrors(), process.WaitForExitAsync()).WaitAsync(ChildTimeout);
                string stderr = text.ToString();
                var threads = new Dictionary<string, ulong>(StringComparer.Ordinal);
                foreach (Match match in ThreadLine().Matches(stderr))
                {
                    threads[match.Groups[1].Value] = ulong.Parse(match.Groups[2].Value);
                }

                // 부모가 끝난 직후 도우미가 아직 도는가다(정상 종료는 도우미를 먼저 끝낸다).
                bool helperAlive = false;
                if (helperProcess != null)
                {
                    helperAlive = !helperProcess.HasExited;
                    await WaitForHelper(helperProcess, mode);
                }

                return new ProbeResult(unchecked((uint)process.ExitCode), process.Id, helperPid, helperAlive, stderr, threads,
                    CallbackLine().Matches(stderr).ToList());
            }
            finally
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                }

                helperProcess?.Dispose();
            }
        }

        void VerifyExit(ProbeResult result, uint windowsCode, string mode)
        {
            // 30–34는 probe 준비 실패·크래시 없음이다(CrashProcessProbe.cpp).
            Verify.That(result.ExitCode != 0 && (result.ExitCode < 30 || result.ExitCode > 34),
                mode + " did not crash as intended: 0x" + result.ExitCode.ToString("X8") + "\n" + result.StdErr);
            if (windows)
            {
                Verify.Equal(windowsCode, result.ExitCode, mode + " exit code\n" + result.StdErr);
            }
            else
            {
                // 겹친 스레드가 소유자보다 먼저 끝내면 다른 신호가 나온다.
                uint[] expected = windowsCode == RocksFatal
                    ? [PosixAbort]
                    : [PosixSegmentationFault, PosixBusError, PosixBusErrorLinux];
                Verify.That(expected.Contains(result.ExitCode),
                    mode + " POSIX exit signal: " + result.ExitCode + "\n" + result.StdErr);
            }
        }

        Match SingleCallback(ProbeResult result, string mode)
        {
            Verify.Equal(1, result.Callbacks.Count, mode + " callback lines\n" + result.StdErr);
            return result.Callbacks[0];
        }

        // 크래시 폴더 하나(<program>-<UTC>-<pid>)를 찾는다. reportLine이면 부모 stderr의 "Rocks crash report:" 줄도 본다(보고 없는
        // 종료는 도우미만 쓰므로 그 줄이 없다).
        string SingleCrashFolder(string root, ProbeResult result, string mode, bool reportLine = true)
        {
            Verify.That(Directory.Exists(root), mode + " crash root missing: " + root + "\n" + result.StdErr);
            string[] folders = Directory.GetDirectories(root);
            Verify.Equal(1, folders.Length, mode + " crash folders under " + root + "\n" + result.StdErr);
            string name = Path.GetFileName(folders[0]);
            Verify.That(name.StartsWith(probeProgram + "-", StringComparison.Ordinal) && name.EndsWith("-" + result.ProcessId, StringComparison.Ordinal),
                mode + " crash folder name: " + name);
            if (reportLine)
            {
                Verify.Contains("Rocks crash report: " + folders[0], result.StdErr, mode + " report line");
            }

            return folders[0];
        }

        // 실행 파일 PE의 CodeView PDB GUID다(덤프·메타데이터의 값과 같아야 디버거가 이 PDB를 받는다).
        string ExecutableGuid()
        {
            using var stream = File.OpenRead(executable);
            using var reader = new PEReader(stream);
            DebugDirectoryEntry entry = reader.ReadDebugDirectory().First(value => value.Type == DebugDirectoryEntryType.CodeView);
            return reader.ReadCodeViewDebugDirectoryData(entry).Guid.ToString("D").ToUpperInvariant();
        }

        // 고정 cdb 명령(설계 17절)으로 덤프를 열고 stack에 실패 함수가 있는지, private PDB로 해석되는지 본다.
        async Task VerifyDebugger(string dump, string frame, string mode)
        {
            string cdb = Environment.GetEnvironmentVariable("ROCKS_CDB")
                ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Windows Kits", "10", "Debuggers", "x64",
                    "cdb.exe");
            Verify.That(File.Exists(cdb), "cdb.exe is required for crash dump verification (Windows Kits 10 Debuggers): " + cdb);
            string bin = Path.GetDirectoryName(executable)!;
            string program = Path.GetFileNameWithoutExtension(executable);
            var start = new ProcessStartInfo(cdb)
            {
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                RedirectStandardInput = true,
                CreateNoWindow = true
            };
            foreach (string argument in new[] { "-z", dump, "-y", bin, "-i", bin, "-c",
                ".symopt+0x40; sxd av; sxd eh; .ecxr; kn 32; lm v m " + program + "; q" })
            {
                start.ArgumentList.Add(argument);
            }

            using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start cdb");
            try
            {
                process.StandardInput.Close();
                Task<string> output = process.StandardOutput.ReadToEndAsync();
                Task<string> errors = process.StandardError.ReadToEndAsync();
                await Task.WhenAll(output, errors, process.WaitForExitAsync()).WaitAsync(DebuggerTimeout);
                string text = output.Result + errors.Result;
                Verify.Contains(program + "!", text, mode + " cdb stack has no module frames");
                Verify.That(Regex.IsMatch(text, @"^[0-9a-f]{2} [0-9a-f`]+ [0-9a-f`]+\s+" + Regex.Escape(program) + "!.*" + Regex.Escape(frame),
                        RegexOptions.Multiline),
                    mode + " cdb stack has no " + frame + " frame:\n" + text);
                Verify.Contains("private pdb symbols", text, mode + " cdb did not load private symbols");
            }
            finally
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                }
            }
        }

        // CrashInfo.json과 덤프의 공통 단언이다. 메타데이터 문서를 돌려준다(호출자가 칸을 더 본다).
        async Task<JsonDocument> VerifyCrashFolder(string folder, ProbeResult result, Expectation expected, ulong failingThread,
            bool expectComplete = true)
        {
            string metadataPath = Path.Combine(folder, "CrashInfo.json");
            Verify.That(File.Exists(metadataPath), expected.Mode + " CrashInfo.json missing");
            Verify.That(!File.Exists(metadataPath + ".tmp"), expected.Mode + " temporary metadata left behind");
            var metadata = JsonDocument.Parse(File.ReadAllBytes(metadataPath));
            JsonElement root = metadata.RootElement;
            Verify.Equal("rocks.crash/1", root.GetProperty("format").GetString(), expected.Mode + " format");
            Verify.Equal(expectComplete, root.GetProperty("complete").GetBoolean(), expected.Mode + " complete: " + root);
            Verify.Equal(probeProgram, root.GetProperty("program").GetString(), expected.Mode + " program");
            Verify.Equal(Path.GetFileName(executable), root.GetProperty("executable").GetString(), expected.Mode + " executable");
            Verify.Equal(result.ProcessId, root.GetProperty("processId").GetInt32(), expected.Mode + " processId");
            Verify.Equal(32, root.GetProperty("crashId").GetString()!.Length, expected.Mode + " crashId");
            Verify.Equal(expected.Kind, root.GetProperty("kind").GetString(), expected.Mode + " kind");
            Verify.Equal("Running", root.GetProperty("phase").GetString(), expected.Mode + " phase");
            Verify.Equal("CrashProbe.Run", root.GetProperty("stage").GetString(), expected.Mode + " stage");
            Verify.Equal(failingThread, root.GetProperty("failingThreadId").GetUInt64(), expected.Mode + " failingThreadId");
            Verify.That(root.GetProperty("reporterThreadId").GetUInt64() != failingThread, expected.Mode + " reporter is the failing thread");
            Verify.Equal(expected.ThreadName, root.GetProperty("failingThreadName").GetString(), expected.Mode + " failingThreadName");
            Verify.Equal(ProbeLogFile, root.GetProperty("associatedFiles").GetProperty("logFile").GetString(), expected.Mode + " logFile");
            JsonElement build = root.GetProperty("build");
            // 빌드 식별은 Launch가 넘긴 엔진 버전·도구 빌드 ID다.
            Verify.That(Regex.IsMatch(build.GetProperty("engineVersion").GetString() ?? "", @"^\d+\.\d+\.\d+$"),
                expected.Mode + " build identity: " + build);
            if (windows)
            {
                Verify.Equal("Windows", build.GetProperty("platform").GetString(), expected.Mode + " build platform");
            }

            Verify.That(root.GetProperty("processStartUtc").GetString()!.EndsWith('Z'), expected.Mode + " processStartUtc");
            if (!windows)
            {
                Verify.Equal("Unsupported", root.GetProperty("dump").GetProperty("status").GetString(), expected.Mode + " POSIX dump status");
                return metadata;
            }

            Verify.Equal("0x" + expected.Code.ToString("X8"), root.GetProperty("exceptionCode").GetString(), expected.Mode + " exceptionCode");
            JsonElement dump = root.GetProperty("dump");
            Verify.Equal("Written", dump.GetProperty("status").GetString(), expected.Mode + " dump status: " + dump);
            Verify.Equal(expected.Writer, dump.GetProperty("writer").GetString(), expected.Mode + " dump writer");
            JsonElement helperInfo = root.GetProperty("helper");
            if (expected.HelperStatus == null)
            {
                Verify.That(helperInfo.ValueKind == JsonValueKind.Null, expected.Mode + " helper should be null: " + helperInfo);
            }
            else
            {
                Verify.Equal(expected.HelperStatus, helperInfo.GetProperty("status").GetString(), expected.Mode + " helper status: " + helperInfo);
            }

            string limitations = root.GetProperty("limitations").ToString();
            Verify.That(limitations.Contains("in-process", StringComparison.Ordinal) == (expected.Writer == "InProcess"),
                expected.Mode + " limitations: " + limitations);
            string dumpPath = Path.Combine(folder, "Minidump.dmp");
            MinidumpReader reader = MinidumpReader.Open(dumpPath);
            Verify.That(reader.HasException, expected.Mode + " exception stream missing");
            (uint threadId, uint code, _) = reader.Exception();
            Verify.Equal(expected.Code, code, expected.Mode + " dump exception code");
            Verify.Equal(failingThread, (ulong)threadId, expected.Mode + " dump exception thread");
            Verify.That(reader.ThreadIds().Contains(threadId), expected.Mode + " failing thread missing from the thread list");
            byte[]? record = reader.Stream(CrashRecordStreamType);
            Verify.That(record != null, expected.Mode + " RocksCrashRecord/1 user stream missing");
            string recordText = Encoding.UTF8.GetString(record!);
            Verify.That(recordText.StartsWith(CrashRecordStreamName, StringComparison.Ordinal), expected.Mode + " user stream name");
            using (var recordJson = JsonDocument.Parse(recordText[CrashRecordStreamName.Length..]))
            {
                Verify.Equal(root.GetProperty("crashId").GetString(), recordJson.RootElement.GetProperty("crashId").GetString(),
                    expected.Mode + " user stream crashId");
            }

            Verify.That(reader.Stream(CommentStreamA) != null, expected.Mode + " comment stream missing");
            (string guid, uint age, string pdbName) = reader.ModuleCodeView(Path.GetFileName(executable));
            JsonElement module = root.GetProperty("module");
            Verify.Equal(guid, module.GetProperty("pdbGuid").GetString(), expected.Mode + " dump module GUID vs metadata");
            Verify.Equal(ExecutableGuid(), guid, expected.Mode + " dump module GUID vs executable");
            Verify.Equal(age, module.GetProperty("pdbAge").GetUInt32(), expected.Mode + " pdbAge");
            Verify.Equal(Path.GetFileName(pdbName.Replace('\\', '/')), module.GetProperty("pdbName").GetString(), expected.Mode + " pdbName");
            if (expected.Frame != null)
            {
                await VerifyDebugger(dumpPath, expected.Frame, expected.Mode);
            }

            return metadata;
        }

        void AddCrash(Expectation expected, Func<ProbeResult, string, Task>? extra = null, IReadOnlyDictionary<string, string>? overrides = null)
        {
            suite.Add("Native.Crash." + expected.Mode, async () =>
            {
                using var directory = new TemporaryDirectory();
                string root = directory.FilePath("Crashes");
                ProbeResult result = await Run(expected.Mode, root, overrides);
                VerifyExit(result, expected.Code, expected.Mode);
                Verify.That(result.Threads.ContainsKey(expected.Role), expected.Mode + " did not announce the " + expected.Role + " thread\n" + result.StdErr);
                ulong failing = result.Threads[expected.Role];
                Match callback = SingleCallback(result, expected.Mode);
                Verify.Equal(expected.Reason, int.Parse(callback.Groups[1].Value), expected.Mode + " reason");
                Verify.Equal(failing, ulong.Parse(callback.Groups[2].Value), expected.Mode + " failing thread");
                string folder = SingleCrashFolder(root, result, expected.Mode);
                using JsonDocument metadata = await VerifyCrashFolder(folder, result, expected, failing);
                if (extra != null)
                {
                    await extra(result, folder);
                }
            });
        }

        // Fatal·assert·abort 계열이다(하드웨어 fault가 아니다). POSIX Asan 구성에서도 ASan에 넘기지 않고 Rocks가 보고하므로
        // 일반 기대값(SIGABRT, reason Assert·Fatal·Abort)으로 등록한다(DeferToSanitizer의 음성 측면, LC7 리뷰 7).
        Expectation[] fatalFamily =
        [
            new("assert", "main", RocksFatal, ReasonAssert, "Assert", "RunCrashProbe"),
            new("log-fatal", "main", RocksFatal, ReasonFatal, "Fatal", "RunLogFatal"),
            new("abort-main", "main", RocksFatal, ReasonAbort, "Abort", "AbortFromProbe"),
            new("abort-unmanaged-thread", "worker", RocksFatal, ReasonAbort, "Abort", "AbortFromProbe"),
        ];

        // Asan 구성(설계 13.2절): 덤프·도우미가 없다. 하드웨어 예외는 우리 filter가 넘겨 ASan이 보고하고, ASan 보고 뒤 death
        // callback이 공통 경로로 종료 콜백과 메타데이터(reason Sanitizer, 덤프 없음)를 남긴다. Fatal은 공통 경로 그대로다. 덤프·도우미를
        // 기대하는 크래시 시험은 Asan 구성에서 등록하지 않는다(POSIX는 덤프가 없으므로 Fatal 계열을 일반 기대값으로 등록한다).
        // death callback은 Windows와 POSIX 모두 건다(LC7). POSIX는 ASan이 처리기를 건 하드웨어 신호를 ASan에 먼저 넘기고, ASan 보고 뒤
        // death callback의 Fatal 경로가 abort로 끝낸다(종료 코드 128+SIGABRT). ASan이 치명으로 보지 않는 Windows 예외(사용자 예외 코드)는
        // 최종 filter에서 Rocks가 보고한다(종료 코드는 그 예외 코드).
        if (options.Asan)
        {
            uint fatalExit = windows ? RocksFatal : PosixAbort;
            var asanModes = new List<(string Mode, string Report, int Reason, uint ExitCode)>
            {
                ("asan-uaf", "AddressSanitizer: heap-use-after-free", ReasonSanitizer, fatalExit),
                ("main-access-violation", windows ? "AddressSanitizer: access-violation" : "AddressSanitizer: SEGV", ReasonSanitizer, fatalExit),
                ("main-stack-overflow", "AddressSanitizer: stack-overflow", ReasonSanitizer, fatalExit),
                ("platform-fatal", "crash probe: platform fatal", ReasonFatal, fatalExit),
            };
            if (windows)
            {
                asanModes.Add(("user-exception", "crash probe main thread=", ReasonException, UserExceptionCode));
            }

            foreach ((string mode, string report, int reason, uint exitCode) in asanModes)
            {
                suite.Add("Native.Crash.asan-policy-" + mode, async () =>
                {
                    using var directory = new TemporaryDirectory();
                    string root = directory.FilePath("Crashes");
                    ProbeResult result = await Run(mode, root);
                    Verify.Equal(exitCode, result.ExitCode, mode + " exit code\n" + result.StdErr);
                    Verify.Contains(report, result.StdErr, mode + " report text");
                    Verify.Equal(reason, int.Parse(SingleCallback(result, mode).Groups[1].Value), mode + " callback reason\n" + result.StdErr);
                    string folder = SingleCrashFolder(root, result, mode);
                    using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
                    Verify.Contains("sanitizer build", metadata.RootElement.GetProperty("limitations").ToString(), mode + " limitations");
                    Verify.That(!File.Exists(Path.Combine(folder, "Minidump.dmp")), mode + " wrote a dump under ASan");
                    Verify.Equal(JsonValueKind.Null, metadata.RootElement.GetProperty("helper").ValueKind, mode + " helper under ASan");
                    Verify.Equal(0, result.HelperPid, mode + " helper pid under ASan");
                });
            }

            if (!windows)
            {
                foreach (Expectation expected in fatalFamily)
                {
                    AddCrash(expected);
                }
            }

            return;
        }

        foreach (Expectation expected in new Expectation[] {
            new("main-access-violation", "main", AccessViolation, hardwareReason, windows ? "Exception" : "Signal", "FaultAccess"),
            new("main-stack-overflow", "main", StackOverflow, hardwareReason, windows ? "Exception" : "Signal", "OverflowStack"),
            new("thread-access-violation", "worker", AccessViolation, hardwareReason, windows ? "Exception" : "Signal", "FaultAccess",
                windows ? WorkerThreadName : null),
            new("thread-stack-overflow", "worker", StackOverflow, hardwareReason, windows ? "Exception" : "Signal", "OverflowStack",
                windows ? WorkerThreadName : null),
            new("unmanaged-thread-access-violation", "worker", AccessViolation, hardwareReason, windows ? "Exception" : "Signal",
                "FaultAccess"),
            new("platform-fatal", "main", RocksFatal, ReasonFatal, "Fatal", "FatalFromProbe") }.Concat(fatalFamily))
        {
            AddCrash(expected);
        }

        // 접근 위반 덤프는 실패 위치(읽기·쓰기)와 주소를 메타데이터에 남긴다.
        suite.Add("Native.Crash.fault-access-metadata", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            ProbeResult result = await Run("main-access-violation", root);
            VerifyExit(result, AccessViolation, "fault-access-metadata");
            string folder = SingleCrashFolder(root, result, "fault-access-metadata");
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
            if (windows)
            {
                JsonElement access = metadata.RootElement.GetProperty("faultAccess");
                Verify.Equal("Write", access.GetProperty("operation").GetString(), "fault operation");
                Verify.Equal("0x0000000000000000", access.GetProperty("address").GetString(), "fault address");
            }

            Verify.That(metadata.RootElement.GetProperty("fatal").ValueKind == JsonValueKind.Null, "hardware fault has no fatal record");
        });

        // Fatal 계열은 메시지·file·line·호출 위치를 따로 남긴다(덤프가 RaiseException만 가리키지 않게).
        suite.Add("Native.Crash.fatal-metadata", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            ProbeResult result = await Run("assert", root);
            VerifyExit(result, RocksFatal, "fatal-metadata");
            string folder = SingleCrashFolder(root, result, "fatal-metadata");
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
            JsonElement fatal = metadata.RootElement.GetProperty("fatal");
            Verify.Equal("Assert", fatal.GetProperty("kind").GetString(), "fatal kind");
            Verify.Contains("CrashProcessProbe.cpp", fatal.GetProperty("file").GetString()!, "fatal file");
            Verify.That(fatal.GetProperty("line").GetInt32() > 0, "fatal line");
            Verify.That(fatal.GetProperty("returnAddress").GetString() != "0x0000000000000000", "fatal return address");
        });

        if (windows)
        {
            foreach (Expectation expected in new Expectation[] {
                new("invalid-parameter", "main", RocksFatal, ReasonRuntimeCheck, "RuntimeCheck", "PassInvalidParameter"),
                new("purecall", "main", RocksFatal, ReasonRuntimeCheck, "RuntimeCheck", "CallPureVirtual"),
                // 힙 손상은 일반 filter에 오지 않는다. 선택 기능 VEH(captureHeapCorruption)가 받아 fail-fast 전에 보고한다.
                new("heap-corruption", "main", HeapCorruption, ReasonException, "Exception", "CorruptHeap") })
            {
                AddCrash(expected);
            }

            // Detailed는 기본 덤프에 더해 Minidump.detail.dmp를 쓴다.
            AddCrash(new("detailed-dump", "main", AccessViolation, ReasonException, "Exception", null), (result, folder) =>
            {
                string detail = Path.Combine(folder, "Minidump.detail.dmp");
                Verify.That(File.Exists(detail), "detailed dump file missing");
                MinidumpReader reader = MinidumpReader.Open(detail);
                Verify.Equal(AccessViolation, reader.Exception().Code, "detailed dump exception code");
                using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
                Verify.Equal("Written", metadata.RootElement.GetProperty("detailDump").GetProperty("status").GetString(), "detailDump status");
                return Task.CompletedTask;
            });
        }

        // 수집기를 설치하지 않은 Fatal은 비상 출력 뒤 바로 끝난다(콜백·크래시 폴더 없음).
        suite.Add("Native.Crash.off-fatal", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            ProbeResult result = await Run("off-fatal", root);
            VerifyExit(result, RocksFatal, "off-fatal");
            Verify.Equal(0, result.Callbacks.Count, "off-fatal callback lines");
            Verify.Contains("crash probe: fatal without crash reporting", result.StdErr, "off-fatal stderr");
            Verify.That(!Directory.Exists(root), "off-fatal created a crash folder");
        });

        // 스레드 A가 실패해 콜백이 gate에 들어온 뒤 스레드 B가 Fatal한다. 보고는 A 하나이고(콜백 한 번, 크래시 폴더 하나),
        // B는 멈춘 스레드 칸에 남으며, 프로세스는 A의 코드로 끝난다.
        suite.Add("Native.Crash.concurrent", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            ProbeResult result = await Run("concurrent", root);
            VerifyExit(result, AccessViolation, "concurrent");
            Match callback = SingleCallback(result, "concurrent");
            Verify.Equal(result.Threads["worker"], ulong.Parse(callback.Groups[2].Value), "concurrent failing thread");
            Verify.Equal("1", callback.Groups[3].Value, "concurrent stopped count");
            Verify.Equal(result.Threads["second"], ulong.Parse(callback.Groups[4].Value), "concurrent stopped thread");
            string folder = SingleCrashFolder(root, result, "concurrent");
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
            JsonElement concurrent = metadata.RootElement.GetProperty("concurrentCrashes");
            Verify.Equal(1, concurrent.GetProperty("count").GetInt32(), "concurrentCrashes.count");
            Verify.Equal(result.Threads["second"], concurrent.GetProperty("threads")[0].GetUInt64(), "concurrentCrashes.threads");
            if (windows)
            {
                Verify.Equal(result.Threads["worker"], (ulong)MinidumpReader.Open(Path.Combine(folder, "Minidump.dmp")).Exception().ThreadId,
                    "concurrent dump thread");
            }
        });

        // 출력 스레드가 크래시하면 로그 종료 콜백은 큐를 기다리지 않고 비상 문구만 남긴다.
        suite.Add("Native.Crash.writer-fault", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            ProbeResult result = await Run("writer-fault", root);
            VerifyExit(result, AccessViolation, "writer-fault");
            Match callback = SingleCallback(result, "writer-fault");
            Verify.Equal(result.Threads["worker"], ulong.Parse(callback.Groups[2].Value), "writer-fault failing thread");
            Verify.Contains("The log writer thread itself crashed; queued records are lost", result.StdErr, "writer-fault emergency text");
            _ = SingleCrashFolder(root, result, "writer-fault");
        });

        // 종료 콜백 안에서 다시 실패해도(크래시 처리 스레드의 실패) 남은 콜백만 포기하고 파일 단계를 이어 간다. 프로세스는 원래
        // 코드로 끝난다. abort는 SIGABRT 처리기가 반환하지 않으므로 종료 코드가 3이 아니다(abort 재진입).
        foreach ((string mode, uint code, string kind, int reason, string stderrText) in new[] {
            ("callback-fault", AccessViolation, windows ? "Exception" : "Signal", hardwareReason, "Rocks crash reporter thread failed"),
            ("callback-fatal", AccessViolation, windows ? "Exception" : "Signal", hardwareReason,
                "crash probe: fatal inside a termination callback"),
            ("callback-abort", RocksFatal, "Abort", ReasonAbort, "abort() was called") })
        {
            suite.Add("Native.Crash." + mode, async () =>
            {
                using var directory = new TemporaryDirectory();
                string root = directory.FilePath("Crashes");
                ProbeResult result = await Run(mode, root);
                VerifyExit(result, code, mode);
                Verify.That(result.ExitCode != 3, mode + " abort handler returned (exit code 3)");
                Verify.Equal(0, result.Callbacks.Count, mode + " callback lines");
                Verify.Contains(stderrText, result.StdErr, mode + " stderr");
                Verify.Contains("Rocks crash reporter thread failed; skipping the remaining termination callbacks", result.StdErr,
                    mode + " reporter failure");
                string folder = SingleCrashFolder(root, result, mode);
                var expected = new Expectation(mode, "main", code, reason, kind, null);
                using JsonDocument metadata = await VerifyCrashFolder(folder, result, expected, result.Threads["main"], expectComplete: false);
                JsonElement json = metadata.RootElement;
                Verify.That(json.GetProperty("reporterFailed").GetBoolean(), mode + " reporterFailed");
                Verify.Equal("ReporterFailed", json.GetProperty("terminationCallbacks").GetProperty("status").GetString(), mode + " callbacks status");
                Verify.Contains("reporterFailed", json.GetProperty("incompleteReasons").ToString(), mode + " incompleteReasons");
            });
        }

        // 출력 폴더를 만들 수 없으면(그 자리에 파일이 있다) 임시 폴더 아래 같은 이름으로 한 번 더 시도한다.
        suite.Add("Native.Crash.output-directory-fallback", async () =>
        {
            using var directory = new TemporaryDirectory();
            directory.Write("blocker", "not a directory");
            string temporary = directory.FilePath("temp");
            Directory.CreateDirectory(temporary);
            var overrides = new Dictionary<string, string> { ["TEMP"] = temporary, ["TMP"] = temporary, ["TMPDIR"] = temporary };
            ProbeResult result = await Run("main-access-violation", directory.FilePath("blocker"), overrides);
            VerifyExit(result, AccessViolation, "output-directory-fallback");
            _ = SingleCallback(result, "output-directory-fallback");
            string folder = SingleCrashFolder(Path.Combine(temporary, "Rocks", "Crashes"), result, "output-directory-fallback");
            var expected = new Expectation("output-directory-fallback", "main", AccessViolation, hardwareReason, windows ? "Exception" : "Signal",
                null);
            using JsonDocument metadata = await VerifyCrashFolder(folder, result, expected, result.Threads["main"]);
            Verify.Equal("Fallback", metadata.RootElement.GetProperty("reportDirectory").GetString(), "reportDirectory");
        });

        // 출력 폴더도 임시 폴더도 쓸 수 없으면 stderr에 경로와 오류를 한 줄씩 남기고, 종료 콜백은 그대로 돌고, 원래 코드로 끝난다.
        suite.Add("Native.Crash.output-directory-unavailable", async () =>
        {
            using var directory = new TemporaryDirectory();
            directory.Write("blocker", "not a directory");
            directory.Write("tempfile", "not a directory");
            string temporary = directory.FilePath("tempfile");
            var overrides = new Dictionary<string, string> { ["TEMP"] = temporary, ["TMP"] = temporary, ["TMPDIR"] = temporary };
            ProbeResult result = await Run("main-access-violation", directory.FilePath("blocker"), overrides);
            VerifyExit(result, AccessViolation, "output-directory-unavailable");
            _ = SingleCallback(result, "output-directory-unavailable");
            Verify.Contains("Rocks crash report directory unavailable; no crash files were written", result.StdErr, "directory failure line");
            Verify.Contains("  primary: " + directory.FilePath("blocker"), result.StdErr, "primary path line");
            Verify.Contains("  fallback: " + temporary, result.StdErr, "fallback path line");
            Verify.That(!result.StdErr.Contains("Rocks crash report: ", StringComparison.Ordinal), "no report line without files");
        });
        if (!windows)
        {
            return;
        }

        // 디버거 정책(설계 13.1절)을 cdb로 실제로 띄워 확인한다. probe는 cdb 아래에서 돈다(첫 번째 기회 접근 위반은 디버거가 넘긴다).
        string cdbPath = Environment.GetEnvironmentVariable("ROCKS_CDB")
            ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Windows Kits", "10", "Debuggers", "x64", "cdb.exe");

        async Task<(int ExitCode, string Output)> RunUnderDebugger(string mode, string directory, string commands, bool forceReport)
        {
            Verify.That(File.Exists(cdbPath), "cdb.exe is required for the debugger policy tests: " + cdbPath);
            string bin = Path.GetDirectoryName(executable)!;
            var start = new ProcessStartInfo(cdbPath)
            {
                WorkingDirectory = options.Root,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                RedirectStandardInput = true,
                CreateNoWindow = true
            };
            start.Environment.Clear();
            foreach (var pair in environment)
            {
                start.Environment[pair.Key] = pair.Value;
            }

            // 개발자 셸의 같은 변수가 기본 정책 시험을 바꾸지 않게 지운다.
            start.Environment.Remove("ROCKS_CRASH_REPORT_WITH_DEBUGGER");
            if (forceReport)
            {
                start.Environment["ROCKS_CRASH_REPORT_WITH_DEBUGGER"] = "1";
            }

            foreach (string argument in new[] { "-y", bin, "-i", bin, "-c", commands, executable, "--negative-control", "crash-" + mode, directory })
            {
                start.ArgumentList.Add(argument);
            }

            using var process = Process.Start(start) ?? throw new InvalidOperationException("Cannot start cdb");
            try
            {
                process.StandardInput.Close();
                Task<string> output = process.StandardOutput.ReadToEndAsync();
                Task<string> errors = process.StandardError.ReadToEndAsync();
                await Task.WhenAll(output, errors, process.WaitForExitAsync()).WaitAsync(DebuggerTimeout);
                return (process.ExitCode, output.Result + errors.Result);
            }
            finally
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    await process.WaitForExitAsync().WaitAsync(TimeSpan.FromSeconds(5));
                }
            }
        }

        // 기본: 디버거가 있으면 filter가 보고하지 않고 넘긴다. 디버거는 두 번째 기회로 원래 실패 지점(FaultAccess)에서 멈추고, 크래시
        // 폴더는 생기지 않는다(도우미가 없는 InProcess probe라 디버거가 끝낸 뒤의 무보고 종료 기록도 없다).
        suite.Add("Native.Crash.debugger-stops-at-fault", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            var (_, text) = await RunUnderDebugger("inprocess-access-violation", root,
                ".symopt+0x40; sxd -c2 \"kn 16; .echo ROCKS_SECOND_CHANCE; q\" av; g", forceReport: false);
            Verify.Contains("ROCKS_SECOND_CHANCE", text, "debugger did not stop at the second chance\n" + text);
            Verify.That(Regex.IsMatch(text, @"!.*FaultAccess", RegexOptions.Multiline), "debugger stack has no FaultAccess frame\n" + text);
            Verify.That(!text.Contains("crash probe callback", StringComparison.Ordinal), "crash reporting ran under the debugger\n" + text);
            Verify.That(!Directory.Exists(root) || Directory.GetDirectories(root).Length == 0, "crash folder written under the debugger");
        });

        // 강제 옵션(ROCKS_CRASH_REPORT_WITH_DEBUGGER=1): 디버거가 있어도 보고한다(도우미 덤프와 CrashInfo.json). 디버거는 종료만 본다.
        suite.Add("Native.Crash.debugger-forced-report", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            var (_, text) = await RunUnderDebugger("main-access-violation", root, "sxd av; g; q", forceReport: true);
            Verify.Contains("crash probe callback", text, "forced report did not run the callbacks\n" + text);
            Verify.That(Directory.Exists(root) && Directory.GetDirectories(root).Length == 1, "forced report crash folder missing\n" + text);
            string folder = Directory.GetDirectories(root)[0];
            // 덤프는 도우미가 부모가 끝난 뒤에도 쓴다. 도우미 pid 줄이 있어야 하고, 도우미가 끝나기를 늘 기다린다(이미 끝났으면
            // 열리지 않는다).
            Match helper = HelperLine().Match(text);
            Verify.That(helper.Success && int.Parse(helper.Groups[1].Value) != 0, "forced report helper pid line missing\n" + text);
            Process? helperProcess = OpenHelper(int.Parse(helper.Groups[1].Value));
            if (helperProcess != null)
            {
                await WaitForHelper(helperProcess, "debugger-forced-report");
            }

            Verify.That(File.Exists(Path.Combine(folder, "CrashInfo.json")), "forced report CrashInfo.json missing");
            Verify.That(File.Exists(Path.Combine(folder, "Minidump.dmp")), "forced report Minidump.dmp missing");
        });

        // Fatal은 디버거가 있으면 예외를 내지 않고 Fatal 프레임에서 중단점으로 멈춘다(호출자가 stack에 보인다).
        suite.Add("Native.Crash.debugger-fatal-break", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            var (_, text) = await RunUnderDebugger("inprocess-platform-fatal", root,
                ".symopt+0x40; sxe -c \"kn 16; .echo ROCKS_FATAL_BREAK; q\" bpe; g", forceReport: false);
            Verify.Contains("ROCKS_FATAL_BREAK", text, "debugger did not stop at the Fatal break\n" + text);
            Verify.That(Regex.IsMatch(text, @"!.*FatalFromProbe", RegexOptions.Multiline), "Fatal break stack has no FatalFromProbe frame\n" + text);
            Verify.That(!text.Contains("crash probe callback", StringComparison.Ordinal), "crash reporting ran under the debugger\n" + text);
        });

        // 사용자 예외 코드(RaiseException)도 같은 보고 경로다(종료 코드는 그 예외 코드).
        AddCrash(new("user-exception", "main", UserExceptionCode, ReasonException, "Exception", "RaiseUserException"));

        // 도우미를 쓰지 않는 설정은 크래시 처리 스레드가 프로세스 안에서 쓴다(결과에 in-process). 도우미를 쓰려다 실패한 경우는
        // 프로세스 안 fallback이고 helper.status에 사유가 남는다(설계 11.5절 표).
        AddCrash(new("inprocess-access-violation", "main", AccessViolation, ReasonException, "Exception", "FaultAccess",
            Writer: "InProcess", HelperStatus: null));
        AddCrash(new("helper-start-failure", "main", AccessViolation, ReasonException, "Exception", "FaultAccess",
            Writer: "InProcess", HelperStatus: "StartFailed"), (result, folder) =>
        {
            Verify.Equal(0, result.HelperPid, "helper-start-failure helper pid");
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
            // 없는 파일은 ERROR_FILE_NOT_FOUND, 없는 폴더 아래 경로는 ERROR_PATH_NOT_FOUND다.
            string? error = metadata.RootElement.GetProperty("helper").GetProperty("error").GetString();
            Verify.That(error is "0x00000002" or "0x00000003", "start error: " + error);
            return Task.CompletedTask;
        });
        foreach ((string mode, string fault, string status, string? exitCode) in new[] {
            ("helper-capture-timeout", "hang", "CaptureTimedOut", (string?)null),
            ("helper-exit-during-capture", "exit-on-request", "ExitedDuringCapture", "0x00000005"),
            ("helper-exited-early", "exit-now", "ExitedEarly", "0x00000006") })
        {
            AddCrash(new(mode, "main", AccessViolation, ReasonException, "Exception", "FaultAccess", Writer: "InProcess", HelperStatus: status),
                (result, folder) =>
                {
                    using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
                    JsonElement helper = metadata.RootElement.GetProperty("helper");
                    if (exitCode != null)
                    {
                        Verify.Equal(exitCode, helper.GetProperty("exitCode").GetString(), mode + " helper exit code");
                    }

                    return Task.CompletedTask;
                }, new Dictionary<string, string> { [FaultyHelperVariable] = fault });
        }

        // 소유 CAS의 반대 순서(부모가 먼저 거둠)를 시험 gate로 결정적으로 만든다. 실제 도우미가 캡처한 뒤 부모가 끝날 때까지
        // CAS를 미루므로, 부모는 캡처 마감을 넘겨 요청을 거두고 프로세스 안에서 쓴다. 도우미는 CAS에 져서 아무 파일도 쓰지
        // 않는다(덤프 하나, 쓴 쪽 InProcess, 폴더에 남는 파일은 둘뿐). 도우미가 이기는 순서는 main-access-violation이다.
        AddCrash(new("helper-gate-parent-first", "main", AccessViolation, ReasonException, "Exception", "FaultAccess", Writer: "InProcess",
            HelperStatus: "CaptureTimedOut"), (result, folder) =>
        {
            Verify.Equal("CrashInfo.json,Minidump.dmp", string.Join(',', Directory.GetFileSystemEntries(folder).Select(Path.GetFileName).Order()),
                "gate crash folder entries");
            Verify.That(result.HelperPid != 0, "gate helper pid");
            return Task.CompletedTask;
        }, new Dictionary<string, string> { [FaultyHelperVariable] = "gate-parent-first" });

        // job을 닫으면 그 안 프로세스가 모두 끝나는 harness(KILL_ON_JOB_CLOSE)다. 도우미가 job을 빠져나올 수 없으면 부모는 기본
        // 덤프와 최종 CrashInfo.json이 끝날 때까지 기다린 뒤 끝난다(그 뒤 job이 닫혀 도우미가 끝나도 덤프가 남는다).
        AddCrash(new("job-kill-on-close", "main", AccessViolation, ReasonException, "Exception", "FaultAccess"), (result, folder) =>
        {
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
            Verify.That(metadata.RootElement.GetProperty("helper").GetProperty("inJob").GetBoolean(),
                "job-kill-on-close helper must report that it stayed in the job: " + metadata.RootElement.GetProperty("helper"));
            return Task.CompletedTask;
        });

        // breakaway를 허락하는 KILL_ON_JOB_CLOSE job이다. VA clone은 job에 남을 수 있다(이 OS는 clone의 breakaway 플래그를 받지 않는다).
        // 부모는 clone이 job 안이면 기본 덤프가 끝날 때까지 기다린다(그 전에 job이 닫히면 덤프를 잃는다). 도우미가 빠져나가는지는 시험
        // 실행기가 바깥 job 안에 있는지에 달렸으므로 기대값을 측정한 전제에서 정한다(설계 17절): 이 자리에서 breakaway한 자식이 어느 job에도
        // 없으면 도우미도 job 밖이고, 바깥 job이 막으면 도우미는 job 안이다. 어느 쪽이든 덤프·CrashInfo.json은 온전해야 한다(공통 단언).
        AddCrash(new("job-kill-on-close-breakaway", "main", AccessViolation, ReasonException, "Exception", "FaultAccess"), (result, folder) =>
        {
            bool leaves = BreakawayLeavesAllJobs(executable);
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
            JsonElement helper = metadata.RootElement.GetProperty("helper");
            Verify.Equal(!leaves, helper.GetProperty("inJob").GetBoolean(),
                "job-kill-on-close-breakaway helper inJob (breakaway from this harness " + (leaves ? "leaves" : "does not leave") +
                " every job): " + helper);
            return Task.CompletedTask;
        });

        // 바깥 job이 breakaway를 막는 경우(CI 러너 조건)를 어디서나 결정적으로 만든다: probe가 breakaway를 막은 job에 먼저 들어간 뒤 breakaway
        // 허용 KILL_ON_JOB_CLOSE job을 만든다. 도우미는 job 안에 남고, 부모가 기본 덤프까지 기다려 덤프·CrashInfo.json이 온전하다.
        AddCrash(new("job-kill-on-close-breakaway-blocked", "main", AccessViolation, ReasonException, "Exception", "FaultAccess"),
            (result, folder) =>
        {
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
            Verify.That(metadata.RootElement.GetProperty("helper").GetProperty("inJob").GetBoolean(),
                "job-kill-on-close-breakaway-blocked helper must stay in the job: " + metadata.RootElement.GetProperty("helper"));
            return Task.CompletedTask;
        });

        // 부모가 도우미를 띄운 뒤에 job에 들어간 경우다(harness가 띄운 뒤 job에 넣음). 부모는 크래시 때 job 여부를 다시 읽어 기본
        // 덤프가 끝날 때까지 기다린다(그 전에 끝나면 job이 닫혀 VA clone과 덤프를 잃는다).
        AddCrash(new("job-after-helper", "main", AccessViolation, ReasonException, "Exception", "FaultAccess"));

        // 프로세스 heap을 망가뜨린 채 보고 경로에 들어와도(부모의 파일 API가 막힐 수 있다) 도우미가 snapshot에서 덤프를 쓴다.
        AddCrash(new("process-heap-corruption", "main", HeapCorruption, ReasonException, "Exception", "CorruptProcessHeap"));

        // Full 추가 덤프가 마감(1 ms)을 넘으면 취소되고 그 파일은 남지 않는다. 기본 덤프는 남는다.
        suite.Add("Native.Crash.full-dump-deadline", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            ProbeResult result = await Run("full-dump-deadline", root);
            VerifyExit(result, AccessViolation, "full-dump-deadline");
            string folder = SingleCrashFolder(root, result, "full-dump-deadline");
            var expected = new Expectation("full-dump-deadline", "main", AccessViolation, ReasonException, "Exception", null);
            using JsonDocument metadata = await VerifyCrashFolder(folder, result, expected, result.Threads["main"], expectComplete: false);
            JsonElement detail = metadata.RootElement.GetProperty("detailDump");
            Verify.Equal("Cancelled", detail.GetProperty("status").GetString(), "detailDump status: " + detail);
            Verify.Equal("Full", detail.GetProperty("detail").GetString(), "detailDump detail");
            Verify.That(!File.Exists(Path.Combine(folder, "Minidump.detail.dmp")), "cancelled detail dump left behind");
            Verify.Equal("CrashInfo.json,Minidump.dmp", string.Join(',', Directory.GetFiles(folder).Select(Path.GetFileName).Order()),
                "crash folder files");
            Verify.Contains("detailDumpNotWritten", metadata.RootElement.GetProperty("incompleteReasons").ToString(), "incompleteReasons");
        });

        // 크래시 폴더는 만들어졌지만 그 안에 파일을 만들 수 없으면(같은 이름의 폴더) 쓰는 쪽(도우미·프로세스 안)이 임시 폴더로 옮긴다.
        // dump-blocked는 덤프 파일만 막는다. 예비 CrashInfo.json을 primary에 쓴 뒤 옮기므로 primary의 예비본을 지워야 한다.
        foreach ((string mode, string writer, string? helperStatus) in new[] {
            ("files-blocked", "Helper", (string?)"Running"),
            ("files-blocked-inprocess", "InProcess", null),
            ("dump-blocked", "Helper", "Running"),
            ("dump-blocked-inprocess", "InProcess", null) })
        {
            suite.Add("Native.Crash." + mode, async () =>
            {
                using var directory = new TemporaryDirectory();
                string root = directory.FilePath("Crashes");
                string temporary = directory.FilePath("temp");
                Directory.CreateDirectory(temporary);
                var overrides = new Dictionary<string, string> { ["TEMP"] = temporary, ["TMP"] = temporary };
                ProbeResult result = await Run(mode, root, overrides);
                VerifyExit(result, AccessViolation, mode);
                string blocked = SingleCrashFolder(root, result, mode, reportLine: false);
                Verify.Equal(0, Directory.GetFiles(blocked).Length, mode + " files written into the blocked folder");
                string folder = SingleCrashFolder(Path.Combine(temporary, "Rocks", "Crashes"), result, mode);
                var expected = new Expectation(mode, "main", AccessViolation, ReasonException, "Exception", "FaultAccess", Writer: writer,
                    HelperStatus: helperStatus);
                using JsonDocument metadata = await VerifyCrashFolder(folder, result, expected, result.Threads["main"]);
                Verify.Equal("Fallback", metadata.RootElement.GetProperty("reportDirectory").GetString(), mode + " reportDirectory");
            });
        }

        // 처리기를 거치지 않는 fast-fail은 도우미만 안다. 종료 코드만 담은 UnreportedExit 기록이 남는다(덤프 없음).
        suite.Add("Native.Crash.fastfail", async () =>
        {
            using var directory = new TemporaryDirectory();
            string root = directory.FilePath("Crashes");
            ProbeResult result = await Run("fastfail", root);
            Verify.Equal(FastFailExit, result.ExitCode, "fastfail exit code\n" + result.StdErr);
            Verify.That(result.HelperPid != 0, "fastfail helper pid");
            Verify.Equal(0, result.Callbacks.Count, "fastfail callback lines");
            string folder = SingleCrashFolder(root, result, "fastfail", reportLine: false);
            using var metadata = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(folder, "CrashInfo.json")));
            JsonElement json = metadata.RootElement;
            Verify.Equal("UnreportedExit", json.GetProperty("kind").GetString(), "fastfail kind");
            Verify.Equal("0xC0000409", json.GetProperty("exitCode").GetString(), "fastfail exitCode");
            Verify.That(json.GetProperty("complete").GetBoolean(), "fastfail complete: " + json);
            Verify.That(json.GetProperty("failingThreadId").ValueKind == JsonValueKind.Null, "fastfail failingThreadId");
            Verify.Equal(result.ProcessId, json.GetProperty("processId").GetInt32(), "fastfail processId");
            Verify.That(!File.Exists(Path.Combine(folder, "Minidump.dmp")), "fastfail dump");
        });

        // 정상 종료(어떤 코드든)·Ctrl-C 종료 코드·harness kill은 기록하지 않는다. 정상 종료는 도우미에 Shutdown을 알려 도우미가
        // 부모보다 먼저 끝난다.
        foreach ((string mode, uint code, bool kill) in new[] {
            ("normal-exit", 0u, false),
            ("exit-7", 7u, false),
            ("control-c-exit", ControlCExit, false),
            ("wait-for-kill", KilledExit, true) })
        {
            suite.Add("Native.Crash.no-record-" + mode, async () =>
            {
                using var directory = new TemporaryDirectory();
                string root = directory.FilePath("Crashes");
                ProbeResult result = await Run(mode, root, killAfterHelper: kill);
                Verify.Equal(code, result.ExitCode, mode + " exit code\n" + result.StdErr);
                Verify.That(result.HelperPid != 0, mode + " helper pid");
                Verify.That(!Directory.Exists(root) || Directory.GetFileSystemEntries(root).Length == 0,
                    mode + " created a crash folder\n" + result.StdErr);
                if (mode == "normal-exit")
                {
                    Verify.That(!result.HelperAliveAtExit, "normal exit left the crash helper running");
                }
            });
        }
    }
}
