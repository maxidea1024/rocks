using Rocks.Tooling.Common;

if (args.Length != 2 || args[0] != "--executable" || !File.Exists(args[1]))
{
    Console.Error.WriteLine("Usage: Rocks.Commandlet.Tests --executable <path>");
    return 2;
}

string executable = Path.GetFullPath(args[1]);
string workingDirectory = Path.GetDirectoryName(executable)!;
int failures = 0;

async Task Check(string label, string[] arguments, int exit, string stdout, string stderr, string? directory = null)
{
    ProcessResult result = await ProcessRunner.RunAsync(new(executable, arguments, directory ?? workingDirectory,
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
    // 독립 참조 구현(Ed25519phReference)이 golden 봉투를 다시 만든다 — C++ ContentSignature_GoldenEnvelope의 기대 byte와 같다.
    byte[] goldenSeed = Enumerable.Range(1, 32).Select(value => (byte)value).ToArray();
    byte[] goldenKeyId = Enumerable.Range(0x40, 16).Select(value => (byte)value).ToArray();
    byte[] goldenScope = System.Security.Cryptography.SHA256.HashData("rocks.test\0main"u8.ToArray());
    byte[] goldenEnvelope = Ed25519phReference.Envelope(goldenSeed, goldenKeyId, 7, goldenScope,
        "cooked-manifest-golden-bytes-v1"u8.ToArray());
    bool goldenSame = Convert.ToHexStringLower(Ed25519phReference.PublicKey(goldenSeed)) ==
                      "79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664" &&
                      Convert.ToHexStringLower(goldenEnvelope) == GoldenEnvelopeHex &&
                      Ed25519phReference.VerifyEnvelope(Ed25519phReference.PublicKey(goldenSeed), goldenEnvelope);
    Console.WriteLine($"content-sign golden envelope (independent Ed25519ph): {(goldenSame ? "PASS" : "FAIL")}");
    if (!goldenSame)
    {
        ++failures;
    }

    // 참조 구현 자체를 RFC 8032 7.3 Ed25519ph 벡터("abc")로 확인한다.
    byte[] rfcSeed = Convert.FromHexString("833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42");
    bool rfcSame = Convert.ToHexStringLower(Ed25519phReference.PublicKey(rfcSeed)) ==
                   "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf" &&
                   Convert.ToHexStringLower(Ed25519phReference.Sign(rfcSeed, "abc"u8.ToArray())) ==
                   "98a70222f0b8121aa9d30f813d683f809e462b469c7ff87639499bb94e6dae41" +
                   "31f85042463c2a355a2003d062adf5aaa10b8c61e636062aaad11c2a26083406";
    Console.WriteLine($"content-sign reference RFC 8032 Ed25519ph vector: {(rfcSame ? "PASS" : "FAIL")}");
    if (!rfcSame)
    {
        ++failures;
    }

    // 신뢰 키로 서명했지만 안의 매니페스트가 깨진 봉투는 verify가 매니페스트 판정에서 거부한다(봉투는 통과).
    string badManifestEnvelope = Path.Combine(signDirectory, "bad-manifest.rsig");
    string goldenTrust = Path.Combine(signDirectory, "golden.pub");
    File.WriteAllBytes(badManifestEnvelope,
        Ed25519phReference.Envelope(goldenSeed, goldenKeyId, 7, goldenScope, [1, 2, 3, 4]));
    File.WriteAllText(goldenTrust, Convert.ToHexStringLower(goldenKeyId) + " " +
                                   Convert.ToHexStringLower(Ed25519phReference.PublicKey(goldenSeed)) + "\n");
    await Check("content-sign verify signed bad manifest", ["content-sign", "verify", badManifestEnvelope, "--trust",
        goldenTrust, "--product", "rocks.test", "--channel", "main"], 1, "", "holds a bad cooked manifest");

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
    // content-sign이 쓴 서명을 독립 참조 구현으로 확인하고, 헤더의 keyId·버전·scope·길이 칸을 대조한다.
    byte[] signedBytes = File.ReadAllBytes(envelope);
    byte[] publicKeyBytes = Convert.FromHexString(line[33..97]);
    byte[] expectedScope = System.Security.Cryptography.SHA256.HashData("rocks.game\0live"u8.ToArray());
    bool independent = Ed25519phReference.VerifyEnvelope(publicKeyBytes, signedBytes) &&
                       Convert.ToHexStringLower(signedBytes.AsSpan(8, 16)) == line[..32] &&
                       BitConverter.ToUInt64(signedBytes, 24) == 9 &&
                       signedBytes.AsSpan(32, 32).SequenceEqual(expectedScope) &&
                       BitConverter.ToUInt64(signedBytes, 64) == (ulong)(signedBytes.Length - 136) &&
                       signedBytes.AsSpan(136).SequenceEqual(File.ReadAllBytes(manifest));
    Console.WriteLine($"content-sign envelope verifies independently: {(independent ? "PASS" : "FAIL")}");
    if (!independent)
    {
        ++failures;
    }

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
    bool noTemporary = Directory.GetFiles(signDirectory, "*.tmp").Length == 0;
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
    await Check("content-sign keygen missing positional", ["content-sign", "keygen", keyBlob, "--password-file", password],
        2, "", "usage:");
    await Check("content-sign keygen missing password file",
        ["content-sign", "keygen", Path.Combine(signDirectory, "k2.key"), Path.Combine(signDirectory, "k2.pub"),
         "--password-file", Path.Combine(signDirectory, "missing.txt")], 1, "", "cannot open");
    // 공개 파일 자리만 이미 있으면 방금 만든 키 파일을 지우고 실패한다(짝이 맞지 않는 파일을 남기지 않는다).
    string loneKey = Path.Combine(signDirectory, "lone.key");
    await Check("content-sign keygen with an existing public path",
        ["content-sign", "keygen", loneKey, publicLine, "--password-file", password], 1, "", "cannot create");
    bool loneClean = !File.Exists(loneKey);
    Console.WriteLine($"content-sign keygen leaves no half pair: {(loneClean ? "PASS" : "FAIL")}");
    if (!loneClean)
    {
        ++failures;
    }

    await Check("content-sign sign missing key option", ["content-sign", "sign", manifest, envelope,
        "--password-file", password, "--version", "9", .. scope], 2, "", "usage:");
    await Check("content-sign sign missing manifest", ["content-sign", "sign", Path.Combine(signDirectory, "none.rman"),
        envelope, "--key", keyBlob, "--password-file", password, "--version", "9", .. scope], 1, "", "cannot open");
    // 다른 종류의 키 파일(log 개인키 — KeyBlob이 아니다)은 열지 않는다.
    string logPublic = Path.Combine(signDirectory, "log.pub");
    string logPrivate = Path.Combine(signDirectory, "log.key");
    await Check("content-sign log keygen", ["log", "keygen", logPublic, logPrivate], 0, "log.file.recipient_key", "");
    await Check("content-sign sign other key kind", ["content-sign", "sign", manifest, envelope, "--key", logPrivate,
        "--password-file", password, "--version", "9", .. scope], 1, "", "not a content signing key");
    // 출력 디렉터리가 없으면 임시 파일을 만들 수 없고, 출력 자리가 디렉터리면 교체가 실패한다(임시 파일은 지운다).
    await Check("content-sign sign into a missing directory", ["content-sign", "sign", manifest,
        Path.Combine(signDirectory, "missing", "out.rsig"), "--key", keyBlob, "--password-file", password,
        "--version", "9", .. scope], 1, "", "cannot create");
    string directoryTarget = Path.Combine(signDirectory, "target.rsig");
    Directory.CreateDirectory(directoryTarget);
    await Check("content-sign sign onto a directory", ["content-sign", "sign", manifest, directoryTarget, "--key",
        keyBlob, "--password-file", password, "--version", "9", .. scope], 1, "", "cannot replace");
    bool cleaned = Directory.GetFiles(signDirectory, "*.tmp").Length == 0;
    Console.WriteLine($"content-sign failed replace removes the temporary file: {(cleaned ? "PASS" : "FAIL")}");
    if (!cleaned)
    {
        ++failures;
    }

    await Check("content-sign verify missing product", ["content-sign", "verify", envelope, "--trust", publicLine,
        "--channel", "live"], 2, "", "usage:");
    await Check("content-sign verify bad minimum", ["content-sign", "verify", envelope, "--trust", publicLine, .. scope,
        "--minimum-version", "-1"], 2, "", "usage:");
    await Check("content-sign verify missing envelope", ["content-sign", "verify",
        Path.Combine(signDirectory, "none.rsig"), "--trust", publicLine, .. scope], 1, "", "cannot open");
    await Check("content-sign verify missing trust file", ["content-sign", "verify", envelope, "--trust",
        Path.Combine(signDirectory, "none.pub"), .. scope], 1, "", "cannot open");
    // 사용법 오류: 위치 인자 부족, 필수 선택지 없음, 빈 값, 모르는 이름, 도움말 뒤 인자.
    await Check("content-sign keygen no positional", ["content-sign", "keygen"], 2, "", "usage:");
    await Check("content-sign sign one positional", ["content-sign", "sign", manifest], 2, "", "usage:");
    await Check("content-sign sign missing password option", ["content-sign", "sign", manifest, envelope, "--key",
        keyBlob, "--version", "9", .. scope], 2, "", "usage:");
    await Check("content-sign sign missing version", ["content-sign", "sign", manifest, envelope, "--key", keyBlob,
        "--password-file", password, .. scope], 2, "", "usage:");
    await Check("content-sign sign empty value", ["content-sign", "sign", manifest, envelope, "--key", keyBlob,
        "--password-file", password, "--version", "", .. scope], 2, "", "usage:");
    await Check("content-sign sign bogus option", ["content-sign", "sign", manifest, envelope, "--bogus", "1",
        "--key", keyBlob, "--password-file", password, "--version", "9", .. scope], 2, "", "usage:");
    await Check("content-sign verify no positional", ["content-sign", "verify"], 2, "", "usage:");
    await Check("content-sign verify missing trust", ["content-sign", "verify", envelope, .. scope], 2, "", "usage:");
    await Check("content-sign help with extra", ["content-sign", "--help", "extra"], 2, "", "usage:");
    await Check("content-sign sign missing key file", ["content-sign", "sign", manifest, envelope, "--key",
        Path.Combine(signDirectory, "none.key"), "--password-file", password, "--version", "9", .. scope], 1, "",
        "cannot open");
    await Check("content-sign sign missing password file", ["content-sign", "sign", manifest, envelope, "--key",
        keyBlob, "--password-file", Path.Combine(signDirectory, "none.txt"), "--version", "9", .. scope], 1, "",
        "cannot open");
    await Check("content-sign verify not an envelope", ["content-sign", "verify", manifest, "--trust", publicLine,
        .. scope], 1, "", "rejected at File");
}
finally
{
    Directory.Delete(signDirectory, true);
}

// asset-list·asset-index-verify(AssetSystem.ko.md 13절, 7.1절 13 — A2b): 패키지는 C++ 시험이 고정한 fixture 배열
// (AssetDatabase/Tests/AssetIndexCommandletFixtures.h — Hero 2300·Base 2301·같은 id의 Derived 대표·엔진 99.0.0 저장 Newer
// 2310)을 읽어 쓴다. 목록·--under·프로젝트 찾기(위로)·기술자 없는 --project·진단 줄(누락 의존·형식 불일치·같은 id·이름
// 규칙·깨진 패키지·새 엔진·IO 실패)·빈 프로젝트·Content가 파일·사용법 오류·출력 실패.
Dictionary<string, byte[]> fixtures = ReadPackageFixtures(executable);
string indexDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-index-{Environment.ProcessId}");
try
{
    string project = Path.Combine(indexDirectory, "Demo");
    string problems = Path.Combine(indexDirectory, "Problems");
    string empty = Path.Combine(indexDirectory, "Empty");
    string fileContent = Path.Combine(indexDirectory, "FileContent");
    string noDescriptor = Path.Combine(indexDirectory, "NoDescriptor");
    string outside = Path.Combine(indexDirectory, "Outside");
    foreach (string directory in new[] { project, problems, empty, fileContent })
    {
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, $"{Path.GetFileName(directory)}.rocksproject"), "{}\n");
    }

    Directory.CreateDirectory(noDescriptor);
    Directory.CreateDirectory(outside);
    void WriteContent(string root, string relative, byte[] bytes)
    {
        string path = Path.Combine(root, "Content", relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, bytes);
    }

    WriteContent(project, "Characters/Hero.rasset", fixtures["CommandletHeroPackage"]);
    WriteContent(project, "Items/Base.rasset", fixtures["CommandletBasePackage"]);
    WriteContent(project, "Items/Notes.txt", [1, 2, 3]);
    WriteContent(problems, "Hero.rasset", fixtures["CommandletHeroPackage"]);
    WriteContent(problems, "Copy/Hero2.rasset", fixtures["CommandletHeroPackage"]);
    WriteContent(problems, "Base.rasset", fixtures["CommandletBaseAsDerivedPackage"]);
    WriteContent(problems, "Newer.rasset", fixtures["CommandletNewerPackage"]);
    WriteContent(problems, "Bad#Name.rasset", fixtures["CommandletBasePackage"]);
    WriteContent(problems, "Junk.rasset", [1, 2, 3, 4]);
    File.WriteAllBytes(Path.Combine(fileContent, "Content"), [1]);
    const string heroId = "5e570000-a55e-7000-0000-0000000008fc";
    const string baseId = "5e570000-a55e-7000-0000-0000000008fd";
    const string missingId = "5e570000-a55e-7000-0000-000000000905";

    // 목록: 경로 키 순 한 줄씩(논리 경로·AssetId·대표 타입 id), 정확한 줄 수와 순서.
    ProcessResult listed = await ProcessRunner.RunAsync(new(executable, ["asset-list", $"--project={project}"],
        workingDirectory, Timeout: TimeSpan.FromSeconds(15)));
    string[] lines = listed.StdOut.Split('\n', StringSplitOptions.RemoveEmptyEntries);
    bool listShape = listed.ExitCode == 0 && listed.StdErr.Length == 0 && lines.Length == 2 &&
                     lines[0].StartsWith($"/Game/Characters/Hero {heroId} ", StringComparison.Ordinal) &&
                     lines[1].StartsWith($"/Game/Items/Base {baseId} ", StringComparison.Ordinal) &&
                     lines.All(line => line.Split(' ').Length == 3 && line.Split(' ')[2].Length == 36);
    Console.WriteLine($"asset-list lines: {(listShape ? "PASS" : "FAIL")}");
    if (!listShape)
    {
        ++failures;
        Console.Error.WriteLine($"  exit={listed.ExitCode}; stdout={listed.StdOut}; stderr={listed.StdErr}");
    }

    await Check("asset-list under", ["asset-list", "--under=/Game/Items", $"--project={project}"], 0,
        $"/Game/Items/Base {baseId}", "");
    await Check("asset-list under nothing", ["asset-list", "--under=/game/nothing", $"--project={project}"], 0, "", "");
    await Check("asset-list invalid under", ["asset-list", "--under=Game", $"--project={project}"], 1, "",
        "asset-list: InvalidPath: 'Game' is not a logical directory");
    // 프로젝트 찾기: 하위 디렉터리에서 위로, 기술자가 없으면 실패, --project 디렉터리에 기술자가 없으면 실패.
    await Check("asset-list finds the project upward", ["asset-list"], 0, $"/Game/Characters/Hero {heroId}", "",
        Path.Combine(project, "Content", "Items"));
    await Check("asset-list without a project", ["asset-list"], 1, "", "no project found", outside);
    await Check("asset-list project without descriptor", ["asset-list", $"--project={noDescriptor}"], 1, "",
        "no '*.rocksproject' in");
    // 기술자는 그 디렉터리에 있어야 한다(위에서 찾은 프로젝트의 하위 디렉터리는 프로젝트가 아니다).
    await Check("asset-list project below a project", ["asset-list", $"--project={Path.Combine(project, "Content")}"], 1,
        "", "no '*.rocksproject' in");
    if (OperatingSystem.IsWindows())
    {
        // 드라이브 상대 경로는 프로젝트 루트로 쓸 수 없다.
        await Check("asset-list unusable project", ["asset-list", "--project=C:Projects"], 1, "",
            "is not a usable project directory (InvalidArgument)");
    }

    await Check("asset-list empty project", ["asset-list", $"--project={empty}"], 0, "", "");
    await Check("asset-list content is a file", ["asset-list", $"--project={fileContent}"], 1, "", "cannot scan");
    await Check("asset-list help", ["asset-list", "--help"], 0, "usage: asset-list", "");
    await Check("asset-list positional", ["asset-list", "extra"], 2, "", "usage: asset-list");
    await Check("asset-list twice", ["asset-list", $"--project={project}", $"--project={project}"], 2, "",
        "usage: asset-list");
    await Check("asset-list empty value", ["asset-list", "--project="], 2, "", "usage: asset-list");
    await Check("asset-list unknown option", ["asset-list", "--type=Texture2D"], 2, "", "usage: asset-list");
    // 13절 공통 옵션 --wait=<초>는 받고(읽기 명령은 잠금이 없다) 10진 숫자가 아니면 사용법 오류다.
    await Check("asset-list wait", ["asset-list", "--wait=5", $"--project={project}"], 0, "/Game/Items/Base", "");
    await Check("asset-list invalid wait", ["asset-list", "--wait=5s", $"--project={project}"], 2, "",
        "usage: asset-list");

    // 검증: 진단 줄과 요약, 진단이 있으면 1.
    await Check("asset-index-verify missing dependency", ["asset-index-verify", $"--project={project}"], 1,
        "asset-index-verify: 2 packages, 1 diagnostics",
        $"MissingDependency: Content/Characters/Hero.rasset: dependency {missingId} is not in the index\n");
    await Check("asset-index-verify wait", ["asset-index-verify", "--wait=0", $"--project={empty}"], 0,
        "asset-index-verify: 0 packages, 0 diagnostics", "");
    await Check("asset-index-verify empty", ["asset-index-verify", $"--project={empty}"], 0,
        "asset-index-verify: 0 packages, 0 diagnostics", "");
    await Check("asset-index-verify content is a file", ["asset-index-verify", $"--project={fileContent}"], 1, "",
        "asset-index-verify: cannot scan");
    await Check("asset-index-verify without a project", ["asset-index-verify"], 1, "", "no project found", outside);
    await Check("asset-index-verify help", ["asset-index-verify", "--help"], 0, "usage: asset-index-verify", "");
    await Check("asset-index-verify under is not an option", ["asset-index-verify", "--under=/Game"], 2, "",
        "usage: asset-index-verify");
    // 줄 순서까지 본다: 검색 진단(파일 이름 → 코드 순) 다음에 의존 대조 진단(경로 키 순). 명령은 타입을 등록하지
    // 않아 형식 대조를 하지 않으므로, 같은 id에 파생 대표(DerivedGameAsset)를 둔 Base는 진단이 없다(로드는 파생을
    // 받는다 — 5.7절 14). NewerVersion 줄은 위치(detail) 앞까지만 본다.
    string[] expectedProblems =
    [
        "InvalidPath: Content/Bad#Name.rasset: invalid logical path (InvalidComponent)",
        $"DuplicateAssetId: Content/Copy/Hero2.rasset: asset id {heroId} is also in 'Content/Hero.rasset'",
        $"DuplicateAssetId: Content/Hero.rasset: asset id {heroId} is also in 'Content/Copy/Hero2.rasset'",
        "CorruptPackage: Content/Junk.rasset: package check failed (section 0, detail 0, InvalidArgument)",
        "NewerVersion: Content/Newer.rasset: package check failed (section 1, detail ",
        $"MissingDependency: Content/Copy/Hero2.rasset: dependency {missingId} is not in the index",
        $"MissingDependency: Content/Hero.rasset: dependency {missingId} is not in the index",
    ];
    ProcessResult verified = await ProcessRunner.RunAsync(new(executable, ["asset-index-verify", $"--project={problems}"],
        workingDirectory, Timeout: TimeSpan.FromSeconds(15)));
    string[] problemLines = verified.StdErr.Split('\n', StringSplitOptions.RemoveEmptyEntries);
    bool problemsShape = verified.ExitCode == 1 &&
                         verified.StdOut == "asset-index-verify: 3 packages, 7 diagnostics\n" &&
                         verified.StdErr.EndsWith('\n') && problemLines.Length == expectedProblems.Length &&
                         problemLines.Zip(expectedProblems).All(pair => pair.Second.EndsWith(' ')
                             ? pair.First.StartsWith(pair.Second, StringComparison.Ordinal)
                             : pair.First == pair.Second);
    Console.WriteLine($"asset-index-verify problems: {(problemsShape ? "PASS" : "FAIL")}");
    if (!problemsShape)
    {
        ++failures;
        Console.Error.WriteLine($"  exit={verified.ExitCode}; stdout={verified.StdOut}; stderr={verified.StdErr}");
    }

    if (OperatingSystem.IsWindows())
    {
        // 다른 프로세스가 공유 없이 연 파일은 열기 실패다: 코드 자리에 원본 오류 이름(진단 None — 이름은 플랫폼 파일
        // 오류 대응을 따른다)이 온다.
        string locked = Path.Combine(project, "Content", "Locked.rasset");
        File.WriteAllBytes(locked, fixtures["CommandletBasePackage"]);
        using (new FileStream(locked, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
        {
            await Check("asset-index-verify unreadable file", ["asset-index-verify", $"--project={project}"], 1,
                "asset-index-verify: 2 packages, 2 diagnostics",
                ": Content/Locked.rasset: cannot read the package file\n");
        }

        File.Delete(locked);
        // 표준 출력·오류가 무효 핸들이면 목록 줄·진단 줄·요약 쓰기가 실패한다(실행 오류 1).
        string[] invalidOutputs =
        [
            $"asset-list \"--project={project}\"",
            $"asset-index-verify \"--project={project}\"",
            $"asset-index-verify \"--project={empty}\"",
        ];
        foreach (string arguments in invalidOutputs)
        {
            uint code = WindowsInvalidStandardHandles.Run(executable, arguments, workingDirectory);
            bool failed = code == 1;
            Console.WriteLine($"invalid output handles ({arguments.Split(' ')[0]}): {(failed ? "PASS" : "FAIL")}");
            if (!failed)
            {
                ++failures;
                Console.Error.WriteLine($"  exit={code}");
            }
        }
    }
}
finally
{
    Directory.Delete(indexDirectory, true);
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

// AssetDatabase/Tests/AssetIndexCommandletFixtures.h의 배열(이름 → byte)을 읽는다. 저장소 루트는 실행 파일 위쪽에서
// Rocks.proj를 가진 첫 폴더다.
static Dictionary<string, byte[]> ReadPackageFixtures(string executable)
{
    DirectoryInfo? root = new FileInfo(executable).Directory;
    while (root is not null && !File.Exists(Path.Combine(root.FullName, "Rocks.proj")))
    {
        root = root.Parent;
    }

    if (root is null)
    {
        throw new InvalidOperationException("The repository root (Rocks.proj) is not above the executable.");
    }

    string header = File.ReadAllText(Path.Combine(root.FullName, "Engine", "Source", "Developer", "AssetDatabase", "Tests",
        "AssetIndexCommandletFixtures.h"));
    Dictionary<string, byte[]> arrays = [];
    foreach (System.Text.RegularExpressions.Match match in System.Text.RegularExpressions.Regex.Matches(header,
                 @"inline constexpr uint8 (\w+)\[\] = \{([^}]*)\};"))
    {
        arrays[match.Groups[1].Value] = System.Text.RegularExpressions.Regex.Matches(match.Groups[2].Value, "0x([0-9a-f]{2})")
            .Select(value => Convert.ToByte(value.Groups[1].Value, 16)).ToArray();
    }

    return arrays;
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

// golden 봉투 hex다(seed 1..32, keyId 0x40..0x4f, 버전 7, scope("rocks.test", "main"), 매니페스트 "cooked-manifest-golden-
// bytes-v1" — C++ ContentSignatureTests.cpp의 GoldenEnvelopeHex와 같다).
partial class Program
{
    private const string GoldenEnvelopeHex =
        "5253494701020000404142434445464748494a4b4c4d4e4f0700000000000000" +
        "90247830ed4611caa2c6bbd614ba47aadb1a18adf4272a86df95503548e1116a" +
        "1f000000000000009ac8d7fc0045177e58f04bd19bcb4eeddef81c7481abdba6" +
        "c98b2884f1f8b4f5c6906590e651aaf9fa8c0db558cfaaa938279f4c3365b6a9" +
        "3e59b49b4f5d130f636f6f6b65642d6d616e69666573742d676f6c64656e2d62" +
        "797465732d7631";
}
