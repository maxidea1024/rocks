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

// config-dump: 엔진 호스트와 같은 설정 부트를 거친 값과 출처(엔진 층 Engine.json의 TracePresets 포함).
await Check("config dump", ["config-dump"], 0, "\"section\": \"TracePresets\"", "");
await Check("config dump engine file", ["config-dump"], 0, "\"tag\": \"Engine.json\"", "");
await Check("config dump usage", ["config-dump", "extra"], 2, "", "usage: config-dump");

// trace-report: 사용법·파일 오류(원본 오류 코드 포함)·trace가 아닌 입력·머리만 있는 trace(표준 출력과 파일 출력).
string traceDirectory = Path.Combine(Path.GetTempPath(), $"rocks-trace-report-{Environment.ProcessId}");
Directory.CreateDirectory(traceDirectory);
try
{
    string missing = Path.Combine(traceDirectory, "missing.rtrace");
    string junk = Path.Combine(traceDirectory, "junk.rtrace");
    string valid = Path.Combine(traceDirectory, "empty.rtrace");
    string report = Path.Combine(traceDirectory, "report.json");
    File.WriteAllBytes(junk, [1, 2, 3, 4]);
    byte[] header = new byte[96];
    "RKTR"u8.CopyTo(header);
    BitConverter.TryWriteBytes(header.AsSpan(4), (ushort)1);
    BitConverter.TryWriteBytes(header.AsSpan(6), (ushort)96);
    BitConverter.TryWriteBytes(header.AsSpan(16), 1000000UL);
    File.WriteAllBytes(valid, header);
    await Check("trace-report usage", ["trace-report"], 2, "", "usage: trace-report");
    await Check("trace-report missing input", ["trace-report", missing], 1, "", "native error");
    await Check("trace-report not a trace", ["trace-report", junk], 1, "", "is not a trace file");
    await Check("trace-report stdout", ["trace-report", valid], 0, "\"format\": \"rocks-trace-report\"", "");
    await Check("trace-report file", ["trace-report", valid, report], 0, "", "");
    bool written = File.Exists(report) &&
                   File.ReadAllText(report).Contains("\"tickFrequency\": 1000000", StringComparison.Ordinal);
    Console.WriteLine($"trace-report file content: {(written ? "PASS" : "FAIL")}");
    if (!written)
    {
        ++failures;
    }
}
finally
{
    Directory.Delete(traceDirectory, true);
}

// log keygen·seal·decode: 왕복, 끝 표시 없음(비정상 종료), 변조, 잘못된 키, 덮어쓰기 거부.
string logDirectory = Path.Combine(Path.GetTempPath(), $"rocks-log-commandlet-{Environment.ProcessId}");
Directory.CreateDirectory(logDirectory);
try
{
    string publicKey = Path.Combine(logDirectory, "log.pub");
    string privateKey = Path.Combine(logDirectory, "log.key");
    string plain = Path.Combine(logDirectory, "plain.log");
    string sealedLog = Path.Combine(logDirectory, "sealed.log");
    string truncated = Path.Combine(logDirectory, "truncated.log");
    string tampered = Path.Combine(logDirectory, "tampered.log");
    string text = string.Concat(Enumerable.Range(0, 300).Select(index => $"[Info][Game] line {index}\n"));
    File.WriteAllText(plain, text);
    await Check("log usage", ["log"], 2, "", "log keygen");
    await Check("log keygen", ["log", "keygen", publicKey, privateKey], 0, "log.file.recipient_key = ", "");
    await Check("log keygen refuses to overwrite", ["log", "keygen", publicKey, privateKey], 1, "", "cannot create");
    // 한쪽 경로만 이미 있으면 아무 키도 남기지 않는다(짝이 맞지 않는 키 쌍 금지).
    string lonePublic = Path.Combine(logDirectory, "lone.pub");
    string lonePrivate = Path.Combine(logDirectory, "lone.key");
    File.WriteAllBytes(lonePrivate, [1, 2, 3]);
    await Check("log keygen with an existing private path", ["log", "keygen", lonePublic, lonePrivate], 1, "", "cannot create");
    bool loneClean = !File.Exists(lonePublic) && File.ReadAllBytes(lonePrivate).Length == 3;
    Console.WriteLine($"log keygen leaves no half pair: {(loneClean ? "PASS" : "FAIL")}");
    if (!loneClean)
    {
        ++failures;
    }

    await Check("log seal", ["log", "seal", plain, sealedLog, "--public", publicKey], 0, "", "");
    await Check("log decode", ["log", "decode", sealedLog, "--key", privateKey], 0, "line 299\n", "");
    await Check("log decode json status", ["log", "decode", sealedLog, "--key", privateKey, "--json"], 0, "line 0\n",
        "\"endMarker\": true");
    byte[] sealedBytes = File.ReadAllBytes(sealedLog);
    // 마지막 기록(길이 4 + 빈 평문의 봉인 17)이 끝 표시다. 떼면 비정상 종료한 파일이다.
    File.WriteAllBytes(truncated, sealedBytes[..^21]);
    await Check("log decode without end marker", ["log", "decode", truncated, "--key", privateKey], 0, "line 299\n",
        "no end marker (abnormal termination)");
    byte[] changed = (byte[])sealedBytes.Clone();
    changed[1200] ^= 0x01;
    File.WriteAllBytes(tampered, changed);
    await Check("log decode tampered", ["log", "decode", tampered, "--key", privateKey], 1, "", "authentication failed");
    await Check("log decode wrong key file", ["log", "decode", sealedLog, "--key", publicKey], 1, "", "not a log private key");
    await Check("log decode not a log", ["log", "decode", plain, "--key", privateKey], 1, "", "not an encrypted Rocks log file");
    bool noPlaintext = !System.Text.Encoding.UTF8.GetString(sealedBytes).Contains("line 1", StringComparison.Ordinal);
    Console.WriteLine($"log seal hides plaintext: {(noPlaintext ? "PASS" : "FAIL")}");
    if (!noPlaintext)
    {
        ++failures;
    }
}
finally
{
    Directory.Delete(logDirectory, true);
}

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
