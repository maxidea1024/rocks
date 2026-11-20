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
    // 짝 없는 UTF-16 surrogate는 Launch가 명령줄을 캡처하지 못한다. Commandlet은 사용법 오류 2와 Launch 문구다.
    await Check("invalid UTF-16 argument", ["bad\uD800"], 2, "", "command line arguments could not be captured (InvalidArgument)");
}

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

    // log view: 폴더는 시작 시각·번호 순, JSON lines는 모든 필드, 기본 레이아웃 텍스트는 시각·레벨·
    // 카테고리로 거른다. 봉인 파일은 --key로 읽는다.
    string viewDirectory = Path.Combine(logDirectory, "view");
    Directory.CreateDirectory(viewDirectory);
    const string JsonWarning = "{\"t\":\"2026-11-18T00:00:01.000Z\",\"lvl\":\"Warning\",\"cat\":\"Net\"," +
        "\"event\":\"00000000000000ab\",\"msg\":\"slow request\",\"corr\":{\"request\":55}}";
    const string JsonInfo = "{\"t\":\"2026-11-18T00:00:02.000Z\",\"lvl\":\"Info\",\"cat\":\"Net\"," +
        "\"event\":\"00000000000000cd\",\"msg\":\"ok\"}";
    const string TextInfo = "[2026-11-18T01:02:03.004Z][Info][Engine] engine started";
    const string TextWarning = "[2026-11-18T01:02:04.000Z][Warning][Graphics.D3D12] device slow";
    const string TextError = "[2026-11-18T01:02:05.000Z][Error][Graphics] device lost";
    File.WriteAllText(Path.Combine(viewDirectory, "Host-20261118T010203004Z-2.log"), TextError + "\n");
    File.WriteAllText(Path.Combine(viewDirectory, "Host-20261118T010203004Z.log"), TextInfo + "\n" + TextWarning + "\n");
    File.WriteAllText(Path.Combine(viewDirectory, "Host-20261118T000000000Z.log"), JsonWarning + "\n" + JsonInfo + "\n");
    File.WriteAllText(Path.Combine(viewDirectory, "notes.txt"), "not a log\n");
    async Task CheckView(string label, string[] filters, params string[] lines)
    {
        string[] arguments = ["log", "view", viewDirectory, .. filters];
        ProcessResult result = await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory,
            Timeout: TimeSpan.FromSeconds(15)));
        string expected = string.Concat(lines.Select(line => line + "\n"));
        bool passed = result.ExitCode == 0 && result.StdOut == expected && result.StdErr.Length == 0;
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    await CheckView("log view folder order", [], JsonWarning, JsonInfo, TextInfo, TextWarning, TextError);
    await CheckView("log view level", ["--level", "warning"], JsonWarning, TextWarning, TextError);
    await CheckView("log view category", ["--category", "graphics"], TextWarning, TextError);
    await CheckView("log view correlation", ["--correlation", "request=55"], JsonWarning);
    await CheckView("log view event", ["--event", "00000000000000AB"], JsonWarning);
    await CheckView("log view since and until", ["--since", "2026-11-18T01:02:04", "--until", "2026-11-18T01:02:04.999Z"],
        TextWarning);
    await CheckView("log view grep", ["--grep", "device"], TextWarning, TextError);
    await Check("log view sealed", ["log", "view", sealedLog, "--key", privateKey, "--grep", "line 29"], 0,
        "] line 29\n[Info][Game] line 290\n", "");
    await Check("log view bad level", ["log", "view", viewDirectory, "--level", "loud"], 2, "", "log view");

    // 압축(.log.zst)·봉인+압축 읽기, 잘린 frame은 앞 frame까지 보여 주고 경고, 키 없는 봉인 파일은
    // 건너뛴다(섞인 폴더).
    string packDirectory = Path.Combine(logDirectory, "pack");
    Directory.CreateDirectory(packDirectory);
    string packSource = Path.Combine(logDirectory, "pack-source.txt");
    File.WriteAllText(packSource, string.Concat(Enumerable.Range(0, 400).Select(index =>
        $"[2026-11-18T02:00:00.000Z][{(index % 2 == 0 ? "Info" : "Warning")}][Pack] packed {index}\n")));
    string packed = Path.Combine(packDirectory, "Host-20261118T020000000Z.log.zst");
    await Check("log pack", ["log", "pack", packSource, packed], 0, "", "");
    await Check("log view compressed", ["log", "view", packed, "--grep", "packed 399"], 0, "packed 399\n", "");
    await Check("log view compressed level", ["log", "view", packed, "--level", "error"], 0, "", "");
    byte[] packedBytes = File.ReadAllBytes(packed);
    string truncatedPack = Path.Combine(logDirectory, "truncated.log.zst");
    File.WriteAllBytes(truncatedPack, packedBytes[..^10]);
    await Check("log view truncated frame", ["log", "view", truncatedPack, "--grep", "packed 0\n"], 0, "",
        "truncated or damaged");
    ProcessResult truncatedView = await ProcessRunner.RunAsync(new(executable, ["log", "view", truncatedPack],
        workingDirectory, Timeout: TimeSpan.FromSeconds(15)));
    bool keptFrames = truncatedView.StdOut.Contains("packed 0\n", StringComparison.Ordinal) &&
                      !truncatedView.StdOut.Contains("packed 399", StringComparison.Ordinal);
    Console.WriteLine($"log view truncated keeps earlier frames: {(keptFrames ? "PASS" : "FAIL")}");
    if (!keptFrames)
    {
        ++failures;
    }

    string sealedPacked = Path.Combine(packDirectory, "Host-20261118T030000000Z.log");
    await Check("log seal compress", ["log", "seal", packSource, sealedPacked, "--public", publicKey, "--compress"], 0, "", "");
    await Check("log view sealed compressed", ["log", "view", sealedPacked, "--key", privateKey, "--grep", "packed 398"], 0,
        "packed 398\n", "");
    await Check("log decode sealed compressed", ["log", "decode", sealedPacked, "--key", privateKey], 0, "packed 397\n", "");
    await Check("log view mixed folder without key", ["log", "view", packDirectory, "--grep", "packed 5\n"], 0, "",
        "needs --key");
}
finally
{
    Directory.Delete(logDirectory, true);
}

// content-sign keygen·sign·verify(ContentSigning.ko.md 7절 5·7): 왕복, 덮어쓰기 거부, 틀린 비밀번호, 매니페스트가 아닌 입력,
// 다른 scope·높은 최소 버전·변조 봉투의 거부, 사용법 오류. 입력 매니페스트는 빈 쿠킹 매니페스트(AssetSystem 10절 형식)를 직접 쓴다.
string signDirectory = Path.Combine(Path.GetTempPath(), $"rocks-content-sign-{Environment.ProcessId}");
Directory.CreateDirectory(signDirectory);
try
{
    string keyBlob = Path.Combine(signDirectory, "content.key");
    string publicLine = Path.Combine(signDirectory, "content.pub");
    string password = Path.Combine(signDirectory, "password.txt");
    string wrongPassword = Path.Combine(signDirectory, "wrong.txt");
    string emptyPassword = Path.Combine(signDirectory, "empty.txt");
    string manifest = Path.Combine(signDirectory, "CookedManifest.rman");
    string envelope = Path.Combine(signDirectory, "CookedManifest.rsig");
    string tampered = Path.Combine(signDirectory, "tampered.rsig");
    string notManifest = Path.Combine(signDirectory, "junk.rman");
    File.WriteAllText(password, "correct horse battery\r\n");
    File.WriteAllText(wrongPassword, "wrong\n");
    File.WriteAllText(emptyPassword, "\n");
    File.WriteAllBytes(manifest, EmptyCookedManifest("Windows"));
    File.WriteAllBytes(notManifest, [1, 2, 3, 4]);
    string[] scope = ["--product", "rocks.game", "--channel", "live"];
    await Check("content-sign usage", ["content-sign"], 2, "", "content-sign keygen");
    await Check("content-sign help", ["content-sign", "--help"], 0, "content-sign verify", "");
    await Check("content-sign keygen without password", ["content-sign", "keygen", keyBlob, publicLine], 2, "",
        "usage:");
    await Check("content-sign keygen empty password",
        ["content-sign", "keygen", keyBlob, publicLine, "--password-file", emptyPassword], 1, "", "is empty");
    await Check("content-sign keygen", ["content-sign", "keygen", keyBlob, publicLine, "--password-file", password], 0,
        "Keep the key file out of the repository", "");
    string line = File.ReadAllText(publicLine);
    bool lineShape = line.Length == 98 && line[32] == ' ' && line[97] == '\n';
    Console.WriteLine($"content-sign public line: {(lineShape ? "PASS" : "FAIL")}");
    if (!lineShape)
    {
        ++failures;
        Console.Error.WriteLine($"  line={line}");
    }

    await Check("content-sign keygen refuses to overwrite",
        ["content-sign", "keygen", keyBlob, publicLine, "--password-file", password], 1, "", "cannot create");
    await Check("content-sign sign", ["content-sign", "sign", manifest, envelope, "--key", keyBlob,
        "--password-file", password, "--version", "9", .. scope], 0, "content version 9", "");
    await Check("content-sign verify", ["content-sign", "verify", envelope, "--trust", publicLine, .. scope,
        "--minimum-version", "9"], 0, "content-sign verify: OK", "");
    await Check("content-sign verify other channel", ["content-sign", "verify", envelope, "--trust", publicLine,
        "--product", "rocks.game", "--channel", "beta"], 1, "", "rejected at Scope");
    await Check("content-sign verify higher minimum", ["content-sign", "verify", envelope, "--trust", publicLine,
        .. scope, "--minimum-version", "10"], 1, "", "rejected at ContentVersion");
    byte[] bytes = File.ReadAllBytes(envelope);
    bytes[^1] ^= 1;
    File.WriteAllBytes(tampered, bytes);
    await Check("content-sign verify tampered", ["content-sign", "verify", tampered, "--trust", publicLine, .. scope],
        1, "", "rejected at Signature");
    await Check("content-sign sign replaces", ["content-sign", "sign", manifest, envelope, "--key", keyBlob,
        "--password-file", password, "--version", "10", .. scope], 0, "content version 10", "");
    await Check("content-sign verify replaced", ["content-sign", "verify", envelope, "--trust", publicLine, .. scope,
        "--minimum-version", "10"], 0, "content version 10", "");
    bool noTemporary = !File.Exists(envelope + ".tmp");
    Console.WriteLine($"content-sign leaves no temporary file: {(noTemporary ? "PASS" : "FAIL")}");
    if (!noTemporary)
    {
        ++failures;
    }

    await Check("content-sign sign wrong password", ["content-sign", "sign", manifest, envelope, "--key", keyBlob,
        "--password-file", wrongPassword, "--version", "9", .. scope], 1, "", "wrong password");
    await Check("content-sign sign not a manifest", ["content-sign", "sign", notManifest, envelope, "--key", keyBlob,
        "--password-file", password, "--version", "9", .. scope], 1, "", "is not a cooked manifest");
    await Check("content-sign sign bad version", ["content-sign", "sign", manifest, envelope, "--key", keyBlob,
        "--password-file", password, "--version", "nine", .. scope], 2, "", "usage:");
    await Check("content-sign sign bad product", ["content-sign", "sign", manifest, envelope, "--key", keyBlob,
        "--password-file", password, "--version", "9", "--product", "rocks game", "--channel", "live"], 2, "",
        "usage:");
    await Check("content-sign sign duplicate option", ["content-sign", "sign", manifest, envelope, "--key", keyBlob,
        "--key", keyBlob, "--password-file", password, "--version", "9", .. scope], 2, "", "usage:");
    await Check("content-sign sign unknown option", ["content-sign", "sign", manifest, envelope, "--trust", keyBlob,
        "--password-file", password, "--version", "9", .. scope], 2, "", "usage:");
    await Check("content-sign verify not a key line", ["content-sign", "verify", envelope, "--trust", keyBlob,
        .. scope], 1, "", "is not a content public key line");
    await Check("content-sign unknown subcommand", ["content-sign", "seal"], 2, "", "usage:");
}
finally
{
    Directory.Delete(signDirectory, true);
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

// 항목이 없는 쿠킹 매니페스트 byte다(AssetSystem 10절 형식 v1): magic, 형식 버전, 항목 수, 프로파일 id(LE64 길이 + byte),
// 프로파일·쿠킹 설정 digest, 쿠커 버전, 쿠킹한 엔진 버전 셋, 앞 모든 byte의 CRC32.
static byte[] EmptyCookedManifest(string profile)
{
    using MemoryStream stream = new();
    using (BinaryWriter writer = new(stream, System.Text.Encoding.UTF8, true))
    {
        writer.Write("RMAN"u8);
        writer.Write(1u);
        writer.Write(0u);
        byte[] profileBytes = System.Text.Encoding.UTF8.GetBytes(profile);
        writer.Write((ulong)profileBytes.Length);
        writer.Write(profileBytes);
        writer.Write(new byte[64]);
        writer.Write(1u);
        writer.Write(0u);
        writer.Write(1u);
        writer.Write(0u);
    }

    byte[] body = stream.ToArray();
    byte[] file = new byte[body.Length + 4];
    body.CopyTo(file, 0);
    BitConverter.TryWriteBytes(file.AsSpan(body.Length), Crc32(body));
    return file;
}

// CRC-32(IEEE 802.3, 반사 다항식 0xEDB88320)다.
static uint Crc32(byte[] bytes)
{
    uint crc = 0xFFFFFFFFu;
    foreach (byte value in bytes)
    {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }

    return ~crc;
}
