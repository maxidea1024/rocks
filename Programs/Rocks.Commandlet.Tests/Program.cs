using Rocks.Tooling.Common;

if (args.Length != 2 || args[0] != "--executable" || !File.Exists(args[1]))
{
    Console.Error.WriteLine("Usage: Rocks.Commandlet.Tests --executable <path>");
    return 2;
}

string executable = Path.GetFullPath(args[1]);
string workingDirectory = Path.GetDirectoryName(executable)!;
int failures = 0;

async Task Check(string label, string[] arguments, int exit, string stdout, string stderr)
{
    ProcessResult result = await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory,
        Timeout: TimeSpan.FromSeconds(15)));
    bool passed = result.ExitCode == exit && result.StdOut.Contains(stdout, StringComparison.Ordinal) &&
                  result.StdErr.Contains(stderr, StringComparison.Ordinal) &&
                  (stdout.Length != 0 || result.StdOut.Length == 0) &&
                  (stderr.Length != 0 || result.StdErr.Length == 0);
    Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
    if (!passed)
    {
        ++failures;
        Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
    }
}

await Check("help", ["--help"], 0, "Usage:", "");
await Check("list", ["--list"], 0, "core-check\t", "");
await Check("core check", ["core-check"], 0, "Core allocation and release: PASS", "");
await Check("unknown", ["missing"], 2, "", "Unknown command");
await Check("no arguments", [], 2, "", "Usage:");
await Check("invalid help", ["--help", "extra"], 2, "", "accepts no arguments");
await Check("invalid check argument", ["core-check", "한글🎮"], 2, "", "accepts no arguments");
await Check("empty command", [""], 2, "", "Unknown command");
await Check("oversized argument", [new string('a', 4097)], 2, "", "argument");

if (OperatingSystem.IsWindows())
{
    // 무효 stderr 핸들을 child의 표준 오류로 직접 설정해 출력 실패를 재현한다.
    uint exitCode = WindowsInvalidStandardHandles.Run(executable, new string('a', 4097), workingDirectory);
    bool passed = exitCode == 1;
    Console.WriteLine($"invalid stderr on invalid argument: {(passed ? "PASS" : "FAIL")}");
    if (!passed)
    {
        ++failures;
        Console.Error.WriteLine($"  exit={exitCode}");
    }
}

if (OperatingSystem.IsMacOS())
{
    // 소비자가 먼저 pipe를 닫은 다음 도움말을 출력한다. SIGPIPE 대신 오류 코드가 나와야 한다.
    ProcessResult closedPipe = await ProcessRunner.RunAsync(new("/bin/bash",
        ["-c", "set -o pipefail; (sleep 1; exec \"$1\" --help) | (exit 0)", "rocks-pipe-test", executable],
        workingDirectory, Timeout: TimeSpan.FromSeconds(15)));
    bool passed = closedPipe.ExitCode == 1;
    Console.WriteLine($"closed pipe: {(passed ? "PASS" : "FAIL")}");
    if (!passed)
    {
        ++failures;
        Console.Error.WriteLine($"  exit={closedPipe.ExitCode}; stderr={closedPipe.StdErr}");
    }

    // POSIX argv는 byte열이므로 유효하지 않은 UTF-8도 프로세스에서 검증한다.
    ProcessResult invalidUtf8 = await ProcessRunner.RunAsync(new("/bin/sh",
        ["-c", "exec \"$1\" \"$(printf '\\377')\"", "rocks-argv-test", executable],
        workingDirectory, Timeout: TimeSpan.FromSeconds(15)));
    passed = invalidUtf8.ExitCode == 2 && invalidUtf8.StdErr.Contains("argument", StringComparison.Ordinal);
    Console.WriteLine($"invalid UTF-8 argument: {(passed ? "PASS" : "FAIL")}");
    if (!passed)
    {
        ++failures;
        Console.Error.WriteLine($"  exit={invalidUtf8.ExitCode}; stderr={invalidUtf8.StdErr}");
    }

    ProcessResult closedStderr = await ProcessRunner.RunAsync(new("/bin/sh",
        ["-c", "exec 2>&-; exec \"$1\" \"$2\"", "rocks-stderr-test", executable, new string('a', 4097)],
        workingDirectory, Timeout: TimeSpan.FromSeconds(15)));
    passed = closedStderr.ExitCode == 1;
    Console.WriteLine($"closed stderr on invalid argument: {(passed ? "PASS" : "FAIL")}");
    if (!passed)
    {
        ++failures;
        Console.Error.WriteLine($"  exit={closedStderr.ExitCode}");
    }
}

return failures == 0 ? 0 : 1;
