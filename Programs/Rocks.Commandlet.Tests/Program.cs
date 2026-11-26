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

// asset-list·asset-index-verify(AssetSystem 13절, 7.1절 13 — A2b): 패키지는 C++ 시험이 고정한 fixture 배열
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
    // class-defaults-dump(A5c 대조 2): 등록된 구체 클래스마다 한 줄, 사용법.
    await Check("class-defaults-dump", ["class-defaults-dump"], 0, " present ", "");
    await Check("class-defaults-dump help", ["class-defaults-dump", "--help"], 0, "usage: class-defaults-dump", "");
    await Check("class-defaults-dump extra", ["class-defaults-dump", "extra"], 2, "", "usage: class-defaults-dump");
    await Check("asset-list positional", ["asset-list", "extra"], 2, "", "usage: asset-list");
    await Check("asset-list twice", ["asset-list", $"--project={project}", $"--project={project}"], 2, "",
        "usage: asset-list");
    await Check("asset-list empty value", ["asset-list", "--project="], 2, "", "usage: asset-list");
    // A2c3b부터 --type은 옵션이다: 등록하지 않은 타입 이름은 실행 실패다.
    await Check("asset-list unknown type", ["asset-list", "--type=NoSuchAssetType", $"--project={project}"], 1, "",
        "asset-list: NotFound: NoSuchAssetType: unknown type");
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
                ": Content/Locked.rasset: cannot read the file or directory\n");
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

// asset-new·asset-set·asset-info·asset-list --type(AssetSystem 13절, 7.1절 15 — A2c3b): 실제 에셋 타입 Skeleton·
// AnimationClip(Animation 모듈 — Commandlet이 시작해 타입을 등록한다)으로 만들기·대입(Hard·Soft 참조·float·bool·enum·
// struct 멤버·struct 원소 배열·Name)·출력(줄·JSON·--all·AssetId 대상)·줄 출력 → asset-set 왕복·목록 옵션(--erase·
// --unset)·대입 오류 모아 보고·타입·경로·대상 거부·--type(파생 포함)·사용법 오류·잠금 Busy·열기 경고와 --force·
// 프로젝트 찾기·출력 실패.
string editDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-edit-{Environment.ProcessId}");
try
{
    string project = Path.Combine(editDirectory, "Edit");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Edit.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";

    async Task<ProcessResult> RunEdit(string directory, params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, directory, Timeout: TimeSpan.FromSeconds(30)));

    void Report(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    // exit·표준 출력 조각 모두·표준 오류 조각 모두를 본다(조각이 없으면 그 출력은 비어야 한다).
    async Task<ProcessResult> CheckAll(string label, string[] arguments, int exit, string[] stdout, string[] stderr)
    {
        ProcessResult result = await RunEdit(workingDirectory, arguments);
        bool passed = result.ExitCode == exit &&
                      stdout.All(part => result.StdOut.Contains(part, StringComparison.Ordinal)) &&
                      stderr.All(part => result.StdErr.Contains(part, StringComparison.Ordinal)) &&
                      (stdout.Length != 0 || result.StdOut.Length == 0) &&
                      (stderr.Length != 0 || result.StdErr.Length == 0);
        Report(label, passed, result);
        return result;
    }

    // 만들기: Skeleton(대입 없음)과 AnimationClip(대입 여럿). 표준 출력은 '<논리 경로> <AssetId>' 한 줄이다.
    ProcessResult hero = await CheckAll("asset-new skeleton", ["asset-new", "Skeleton", "/Game/Characters/Hero", projectOption],
        0, ["/Game/Characters/Hero "], []);
    string heroId = hero.StdOut.Trim().Split(' ').Last();
    Report("asset-new output shape", hero.StdOut.Length == "/Game/Characters/Hero ".Length + 36 + 1 &&
        hero.StdOut.EndsWith('\n') && File.Exists(Path.Combine(project, "Content", "Characters", "Hero.rasset")), hero);
    ProcessResult run = await CheckAll("asset-new clip with assignments",
        ["asset-new", "AnimationClip", "/Game/Anims/Run", "skeleton=/Game/Characters/Hero", "rateScale=1.5", "loop=on",
         "interpolation=Step", "rootMotion.enable=true", "events[0]={}", "events[0].name=Foot", projectOption],
        0, ["/Game/Anims/Run "], []);
    string runId = run.StdOut.Trim().Split(' ').Last();

    // 출력: Edit 줄(참조는 논리 경로), AssetId 대상은 같은 출력, JSON, --all.
    string[] runLines = ["skeleton=/Game/Characters/Hero\n", "rateScale=1.5\n", "loop=true\n", "interpolation=Step\n",
        "rootMotion.enable=true\n", "events=\n", "events[0]={}\n", "events[0].name=Foot\n"];
    ProcessResult info = await CheckAll("asset-info lines", ["asset-info", "/Game/Anims/Run", projectOption], 0, runLines, []);
    ProcessResult byId = await RunEdit(workingDirectory, "asset-info", runId, projectOption);
    Report("asset-info by AssetId", byId.ExitCode == 0 && byId.StdOut == info.StdOut, byId);
    await CheckAll("asset-info json", ["asset-info", "/Game/Anims/Run", "--json", projectOption], 0,
        ["{\n", "\"rateScale\": 1.5", "\"skeleton\": \"/Game/Characters/Hero\"", "}\n"], []);
    await CheckAll("asset-info all", ["asset-info", "/Game/Anims/Run", "--all", projectOption], 0, runLines, []);
    await CheckAll("asset-info json all", ["asset-info", "--json", "--all", "/Game/Anims/Run", projectOption], 0,
        ["\"loop\": true"], []);

    // 왕복: asset-info 줄을 그대로 asset-set에 넣으면 받고 출력이 같다.
    string[] lines = info.StdOut.Split('\n', StringSplitOptions.RemoveEmptyEntries);
    ProcessResult roundTrip = await RunEdit(workingDirectory, [.. new[] { "asset-set", "/Game/Anims/Run" }, .. lines, projectOption]);
    ProcessResult again = await RunEdit(workingDirectory, "asset-info", "/Game/Anims/Run", projectOption);
    Report("asset-info lines round trip through asset-set", roundTrip.ExitCode == 0 && roundTrip.StdOut.Length == 0 &&
        again.StdOut == info.StdOut, roundTrip);

    // asset-set: AssetId 대상, Soft 참조 배열, --erase 목록 옵션.
    await CheckAll("asset-set by AssetId", ["asset-set", runId, "rateScale=2", projectOption], 0, [], []);
    await CheckAll("asset-set result", ["asset-info", "/Game/Anims/Run", projectOption], 0, ["rateScale=2\n"], []);
    await CheckAll("asset-set by path", ["asset-set", "/Game/Anims/Run", "rateScale=2.5", projectOption], 0, [], []);
    await CheckAll("asset-set by path result", ["asset-info", runId, projectOption], 0, ["rateScale=2.5\n"], []);
    await CheckAll("asset-set soft array", ["asset-set", "/Game/Characters/Hero",
        "compatibleSkeletons=/Game/Characters/Hero", "useRetargetModesFromCompatibleSkeleton=on", projectOption], 0, [], []);
    await CheckAll("asset-set soft array result", ["asset-info", heroId, projectOption], 0,
        ["compatibleSkeletons=/Game/Characters/Hero\n", "useRetargetModesFromCompatibleSkeleton=true\n"], []);
    await CheckAll("asset-set erase", ["asset-set", "/Game/Anims/Run", "--erase=events[0]", projectOption], 0, [], []);
    ProcessResult erased = await RunEdit(workingDirectory, "asset-info", "/Game/Anims/Run", projectOption);
    Report("asset-set erase result", erased.StdOut.Contains("events=\n", StringComparison.Ordinal) &&
        !erased.StdOut.Contains("events[0]", StringComparison.Ordinal), erased);

    // 경로 key 따옴표 안의 쉼표·escape는 목록 옵션을 나누지 않는다(Name 키 Map 값 struct).
    await CheckAll("asset-set quoted keys", ["asset-set", "/Game/Anims/Run", "targetFrameRate.overrides[\"Hi,There\"]={}",
        "targetFrameRate.overrides[\"a\\\"b\"]={}", projectOption], 0, [], []);
    await CheckAll("asset-set quoted keys result", ["asset-info", "/Game/Anims/Run", projectOption], 0,
        ["targetFrameRate.overrides[\"Hi,There\"]={}\n", "targetFrameRate.overrides[\"a\\\"b\"]={}\n"], []);
    await CheckAll("asset-set erase quoted list", ["asset-set", "/Game/Anims/Run",
        "--erase=targetFrameRate.overrides[\"Hi,There\"],targetFrameRate.overrides[\"a\\\"b\"]", projectOption], 0, [], []);
    ProcessResult quotedErased = await RunEdit(workingDirectory, "asset-info", "/Game/Anims/Run", projectOption);
    Report("asset-set erase quoted list result", quotedErased.ExitCode == 0 &&
        quotedErased.StdOut.Contains("targetFrameRate.overrides=\n", StringComparison.Ordinal) &&
        !quotedErased.StdOut.Contains("targetFrameRate.overrides[", StringComparison.Ordinal), quotedErased);

    // --reset: 클래스 기본값으로 되돌린다(A5b2 — rateScale 기본 1). 빈 경로는 사용법 오류, asset-new는
    // --reset을 받지 않는다(사용법 오류).
    await CheckAll("asset-set reset", ["asset-set", "/Game/Anims/Run", "--reset=rateScale", projectOption], 0, [], []);
    await CheckAll("asset-set reset result", ["asset-info", "/Game/Anims/Run", projectOption], 0, ["rateScale=1\n"], []);
    await CheckAll("asset-set reset empty", ["asset-set", "/Game/Anims/Run", "--reset=", projectOption], 2, [],
        ["usage: asset-set"]);
    await CheckAll("asset-new reset", ["asset-new", "AnimationClip", "/Game/Anims/Reset", "--reset=rateScale",
        projectOption], 2, [], ["usage: asset-new"]);

    // 대입 오류는 모두 모아 보고하고 아무것도 바꾸지 않는다(파일 byte 그대로).
    string runFile = Path.Combine(project, "Content", "Anims", "Run.rasset");
    byte[] before = File.ReadAllBytes(runFile);
    await CheckAll("asset-set errors", ["asset-set", "/Game/Anims/Run", "nope=1", "rateScale=abc",
        "skeleton=/Game/Nope", "--unset=loop", projectOption], 1, [],
        ["asset-set: UnknownProperty: nope: ", "asset-set: InvalidValue: rateScale: ", "asset-set: Unresolved: skeleton: ",
         "asset-set: InvalidValue: loop: "]);
    Report("asset-set errors leave the file", File.ReadAllBytes(runFile).SequenceEqual(before), new(0, "", ""));
    await CheckAll("asset-set soft unresolved", ["asset-set", "/Game/Characters/Hero", "compatibleSkeletons=/Game/Nope",
        projectOption], 1, [], ["asset-set: Unresolved: compatibleSkeletons: "]);

    // 텍스처 타입 모듈(A5d — AssetTypeModules): Texture2D를 만들고 값·숨김 값 입력을 본다.
    await CheckAll("asset-new texture", ["asset-new", "Texture2D", "/Game/Tex/Plain", "lodGroup=UI",
        "compressionSettings=Normalmap", projectOption], 0, ["/Game/Tex/Plain "], []);
    await CheckAll("asset-new texture result", ["asset-info", "/Game/Tex/Plain", projectOption], 0,
        ["lodGroup=UI\n", "compressionSettings=Normalmap\n"], []);
    await CheckAll("asset-set texture hidden value", ["asset-set", "/Game/Tex/Plain", "compressionSettings=LQ", projectOption],
        1, [], ["asset-set: InvalidSetting: compressionSettings: "]);

    // asset-new 거부: 타입(모름·추상·에셋 아님·struct)·경로(있음·규칙)·대입 오류(만들지 않음).
    await CheckAll("asset-new unknown type", ["asset-new", "NoSuchType", "/Game/X", projectOption], 1, [],
        ["asset-new: NotFound: NoSuchType: unknown type"]);
    await CheckAll("asset-new abstract type", ["asset-new", "AnimationAsset", "/Game/X", projectOption], 1, [],
        ["asset-new: TypeMismatch: AnimationAsset: not a concrete asset class"]);
    await CheckAll("asset-new not an asset", ["asset-new", "Package", "/Game/X", projectOption], 1, [],
        ["asset-new: TypeMismatch: Package: not a concrete asset class"]);
    await CheckAll("asset-new struct type", ["asset-new", "BoneSocket", "/Game/X", projectOption], 1, [],
        ["asset-new: TypeMismatch: BoneSocket: not a concrete asset class"]);
    await CheckAll("asset-new existing path", ["asset-new", "Skeleton", "/game/characters/HERO", projectOption], 1, [],
        ["asset-new: DuplicatePath: /game/characters/HERO: AlreadyExists"]);
    await CheckAll("asset-new invalid path", ["asset-new", "Skeleton", "/Game//X", projectOption], 1, [],
        ["asset-new: InvalidPath: /Game//X: InvalidArgument"]);
    await CheckAll("asset-new assignment errors", ["asset-new", "AnimationClip", "/Game/Anims/Bad", "nope=1",
        "skeleton=/Game/Anims/Bad", projectOption], 1, [],
        ["asset-new: UnknownProperty: nope: ", "asset-new: Unresolved: skeleton: "]);
    Report("asset-new assignment errors create nothing",
        !File.Exists(Path.Combine(project, "Content", "Anims", "Bad.rasset")), new(0, "", ""));

    // asset-info 거부.
    await CheckAll("asset-info missing", ["asset-info", "/Game/Nope", projectOption], 1, [],
        ["asset-info: NotFound: /Game/Nope: failed\n"]);
    await CheckAll("asset-info missing id", ["asset-info", "5e570000-a55e-7000-0000-000000000000", projectOption], 1, [],
        ["asset-info: NotFound: 5e570000-a55e-7000-0000-000000000000: failed\n"]);
    await CheckAll("asset-info invalid target", ["asset-info", "Hero", projectOption], 1, [],
        ["asset-info: InvalidPath: Hero: InvalidArgument"]);

    // asset-list --type: 그 클래스와 파생(추상 기반 포함), --under와 함께, 거부.
    ProcessResult skeletons = await CheckAll("asset-list type", ["asset-list", "--type=Skeleton", projectOption], 0,
        [$"/Game/Characters/Hero {heroId} "], []);
    Report("asset-list type lines", skeletons.StdOut.Split('\n', StringSplitOptions.RemoveEmptyEntries).Length == 1, skeletons);
    await CheckAll("asset-list derived type", ["asset-list", "--type=AnimationAsset", projectOption], 0,
        [$"/Game/Anims/Run {runId} "], []);
    ProcessResult assets = await CheckAll("asset-list base type", ["asset-list", "--type=Asset", projectOption], 0,
        ["/Game/Anims/Run ", "/Game/Characters/Hero "], []);
    Report("asset-list base type order", assets.StdOut.IndexOf("/Game/Anims/Run", StringComparison.Ordinal) <
        assets.StdOut.IndexOf("/Game/Characters/Hero", StringComparison.Ordinal), assets);
    await CheckAll("asset-list type under", ["asset-list", "--type=Asset", "--under=/Game/Anims", projectOption], 0,
        ["/Game/Anims/Run "], []);
    await CheckAll("asset-list type under nothing", ["asset-list", "--type=Skeleton", "--under=/Game/Anims", projectOption],
        0, [], []);
    await CheckAll("asset-list type invalid under", ["asset-list", "--type=Skeleton", "--under=Game", projectOption], 1, [],
        ["asset-list: InvalidPath: 'Game' is not a logical directory"]);
    await CheckAll("asset-list not a class", ["asset-list", "--type=BoneSocket", projectOption], 1, [],
        ["asset-list: TypeMismatch: BoneSocket: not a class"]);
    await CheckAll("asset-list type twice", ["asset-list", "--type=Skeleton", "--type=Asset", projectOption], 2, [],
        ["usage: asset-list"]);

    // 사용법 오류(2)와 도움말(0).
    string[][] usageErrors =
    [
        ["asset-new", "Skeleton", projectOption],
        ["asset-new", "Skeleton", "/Game/Y", "positional", projectOption],
        ["asset-new", "Skeleton", "/Game/Y", "--force", projectOption],
        ["asset-new", "Skeleton", "/Game/Y", "--unset=", projectOption],
        ["asset-new", "Skeleton", "/Game/Y", "--unset=a,,b", projectOption],
        ["asset-new", "Skeleton", "/Game/Y", "--erase=a,", projectOption],
        ["asset-new", "", "/Game/Y", projectOption],
        ["asset-set", "/Game/Anims/Run", projectOption],
        ["asset-set", "/Game/Anims/Run", "loop=on", "--json", projectOption],
        ["asset-set", "/Game/Anims/Run", "loop=on", "--force", "--force", projectOption],
        ["asset-set", "/Game/Anims/Run", "loop=on", projectOption, projectOption],
        ["asset-set", "/Game/Anims/Run", "loop=on", "--wait=86401", projectOption],
        ["asset-set", "/Game/Anims/Run", "loop=on", "--wait=5s", projectOption],
        ["asset-set", "/Game/Anims/Run", "loop=on", "--wait=", projectOption],
        ["asset-info", projectOption],
        ["asset-info", "/Game/Anims/Run", "extra", projectOption],
        ["asset-info", "/Game/Anims/Run", "--force", projectOption],
        ["asset-info", "/Game/Anims/Run", "--json", "--json", projectOption],
        ["asset-info", "/Game/Anims/Run", "--unset=loop", projectOption],
        ["asset-info", "/Game/Anims/Run", "--help"],
    ];
    foreach (string[] arguments in usageErrors)
    {
        await CheckAll($"usage error: {string.Join(' ', arguments.Where(argument => argument != projectOption))}", arguments,
            2, [], [$"usage: {arguments[0]}"]);
    }

    foreach (string command in new[] { "asset-new", "asset-set", "asset-info" })
    {
        await CheckAll($"{command} help", [command, "--help"], 0, [$"usage: {command}"], []);
    }

    // 잠금: 다른 쪽이 잡고 있으면 쓰기 명령은 기다린 뒤 실패하고 읽기 명령은 잠금 없이 된다(Windows — LockFileEx 0..1).
    if (OperatingSystem.IsWindows())
    {
        string lockPath = Path.Combine(project, "Intermediate", "AssetDatabase.lock");
        Directory.CreateDirectory(Path.GetDirectoryName(lockPath)!);
        using (FileStream held = new(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite))
        {
            held.Lock(0, 1);
            await CheckAll("asset-set locked", ["asset-set", "/Game/Anims/Run", "loop=off", projectOption], 1, [],
                ["asset-set: Busy: ", ": the project is locked by another process"]);
            await CheckAll("asset-new locked with wait", ["asset-new", "Skeleton", "/Game/Locked", "--wait=1", projectOption],
                1, [], ["asset-new: Busy: ", ": the project is locked by another process"]);
            await CheckAll("asset-info without lock", ["asset-info", "/Game/Anims/Run", projectOption], 0, ["loop=true\n"], []);
            held.Unlock(0, 1);
        }

        // 지울 수 없는 고아 임시 파일(삭제 공유 없이 열림)은 저장을 막지 않고 경고 OrphanNotDeleted다.
        string orphan = Path.Combine(project, "Content", "Anims", "Run.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp");
        File.WriteAllBytes(orphan, [1, 2, 3]);
        using (new FileStream(orphan, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        {
            await CheckAll("asset-set orphan not deleted", ["asset-set", "/Game/Anims/Run", "loop=on", projectOption], 0, [],
                [": warning OrphanNotDeleted (related "]);
        }

        await CheckAll("asset-set orphan deleted", ["asset-set", "/Game/Anims/Run", "loop=on", projectOption], 0, [], []);
        Report("orphan removed", !File.Exists(orphan), new(0, "", ""));
    }

    // 프로젝트 찾기: --project 없이 프로젝트 안에서 위로 찾는다.
    ProcessResult found = await RunEdit(Path.Combine(project, "Content"), "asset-info", "/Game/Anims/Run");
    Report("asset-info finds the project upward", found.ExitCode == 0 &&
        found.StdOut.Contains("rateScale=1\n", StringComparison.Ordinal) && found.StdErr.Length == 0, found);
    await CheckAll("asset-info without a project", ["asset-info", "/Game/Anims/Run"], 1, [], ["no project found"]);
    await CheckAll("asset-list type without a project", ["asset-list", "--type=Skeleton"], 1, [], ["no project found"]);

    // Content가 파일이면 색인을 만들 수 없고(cannot scan), Intermediate가 파일이면 잠금을 잡을 수 없다.
    string brokenContent = Path.Combine(editDirectory, "BrokenContent");
    string brokenLock = Path.Combine(editDirectory, "BrokenLock");
    foreach (string directory in new[] { brokenContent, brokenLock })
    {
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, $"{Path.GetFileName(directory)}.rocksproject"), "{}\n");
    }

    File.WriteAllBytes(Path.Combine(brokenContent, "Content"), [1]);
    File.WriteAllBytes(Path.Combine(brokenLock, "Intermediate"), [1]);
    await CheckAll("asset-info content is a file", ["asset-info", "/Game/X", $"--project={brokenContent}"], 1, [],
        ["asset-info: ", ": cannot scan the content directory"]);
    await CheckAll("asset-list type content is a file", ["asset-list", "--type=Asset", $"--project={brokenContent}"], 1, [],
        ["asset-list: ", ": cannot scan the content directory"]);
    await CheckAll("asset-new cannot lock", ["asset-new", "Skeleton", "/Game/X", $"--project={brokenLock}"], 1, [],
        ["asset-new: ", ": cannot lock the project"]);

    // 열기 경고: 의존 패키지(Hero)가 없으면 열기는 경고와 함께 되고, 저장은 --force 없이 MissingDependency, --force면
    // null로 저장하고 경고다.
    File.Delete(Path.Combine(project, "Content", "Characters", "Hero.rasset"));
    await CheckAll("asset-info excluded dependency", ["asset-info", "/Game/Anims/Run", projectOption], 0,
        ["skeleton=none\n"], [$": {heroId}: warning ExcludedDependency (related "]);
    await CheckAll("asset-set missing dependency", ["asset-set", "/Game/Anims/Run", "loop=off", projectOption], 1, [],
        [$"asset-set: MissingDependency: /Game/Anims/Run: InvalidArgument (related {heroId})\n"]);
    await CheckAll("asset-set force", ["asset-set", "/Game/Anims/Run", "loop=off", "--force", projectOption], 0, [],
        [": warning DroppedUnresolvedImport (related "]);
    await CheckAll("asset-set force result", ["asset-info", "/Game/Anims/Run", projectOption], 0,
        ["loop=false\n", "skeleton=none\n"], []);
    // 만들기의 참조 해석이 연 패키지(Hero가 없는 Run을 다시 가리킴 — Run은 이제 Hero 의존이 없다)와 경고.
    File.Delete(Path.Combine(project, "Content", "Anims", "Run.rasset"));
    await CheckAll("asset-new clip without skeleton", ["asset-new", "AnimationClip", "/Game/Anims/Base",
        "skeleton=/Game/Characters/Hero", projectOption], 1, [], ["asset-new: Unresolved: skeleton: "]);

    if (OperatingSystem.IsWindows())
    {
        // 표준 출력이 무효 핸들이면 출력 쓰기가 실패한다(실행 오류 1).
        // 그 전에 경고가 나는 장면을 다시 만든다: Hero(대상) 없이 Hero를 가리키는 Clip.
        await CheckAll("asset-new skeleton again", ["asset-new", "Skeleton", "/Game/Characters/Hero", projectOption], 0,
            ["/Game/Characters/Hero "], []);
        await CheckAll("asset-new clip again", ["asset-new", "AnimationClip", "/Game/Anims/Run",
            "skeleton=/Game/Characters/Hero", projectOption], 0, ["/Game/Anims/Run "], []);
        File.Delete(Path.Combine(project, "Content", "Characters", "Hero.rasset"));
        await CheckAll("asset-new with open warnings", ["asset-new", "AnimationClip", "/Game/Anims/Walk",
            "additive.basePoseClip=/Game/Anims/Run", projectOption], 0, ["/Game/Anims/Walk "],
            [": warning ExcludedDependency (related "]);
        string[] invalidOutputs =
        [
            $"asset-info /Game/Anims/Run \"--project={project}\"",
            $"asset-info /Game/Anims/Walk \"--project={project}\"",
            $"asset-list --type=Asset \"--project={project}\"",
            $"asset-new Skeleton /Game/Invalid/Output \"--project={project}\"",
            $"asset-new AnimationClip /Game/Invalid/Warned additive.basePoseClip=/Game/Anims/Run \"--project={project}\"",
            $"asset-set /Game/Invalid/Output nope=1 \"--project={project}\"",
            $"asset-set /Game/Anims/Run loop=on --force \"--project={project}\"",
        ];
        foreach (string arguments in invalidOutputs)
        {
            uint code = WindowsInvalidStandardHandles.Run(executable, arguments, workingDirectory);
            bool failed = code == 1;
            Console.WriteLine($"invalid output handles ({arguments.Split(' ')[0]} edit): {(failed ? "PASS" : "FAIL")}");
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
    Directory.Delete(editDirectory, true);
}

// asset-index-rebuild·색인 캐시(AssetSystem 7.1절 16 — A2d1): 재구축은 잠금을 잡고 캐시 없이 검색해 고아 임시
// 파일(Content 저장 임시·Intermediate 캐시 임시)을 지우고 캐시를 쓴다(진단이 있어도 0). 쓰기 명령은 끝에 캐시를 쓰고 쓰지
// 못하면 경고 CacheNotWritten(성공 0)이다. 손상된 캐시는 없는 것과 같다(목록·점검 결과 그대로).
string cacheDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-cache-{Environment.ProcessId}");
try
{
    string project = Path.Combine(cacheDirectory, "Cached");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Cached.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";
    string cacheFile = Path.Combine(project, "Intermediate", "AssetIndex.cache");

    async Task<ProcessResult> RunCache(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportCache(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult made = await RunCache("asset-new", "Skeleton", "/Game/Cached/Hero", projectOption);
    ReportCache("asset-new writes the index cache", made.ExitCode == 0 && made.StdErr.Length == 0 && File.Exists(cacheFile), made);
    string heroId = made.StdOut.Trim().Split(' ').Last();
    ProcessResult listed = await RunCache("asset-list", projectOption);
    ReportCache("asset-list with the cache", listed.ExitCode == 0 &&
        listed.StdOut.StartsWith($"/Game/Cached/Hero {heroId} ", StringComparison.Ordinal), listed);

    // 고아 임시 파일과 재구축.
    string contentOrphan = Path.Combine(project, "Content", "Cached", "Hero.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp");
    string cacheOrphan = Path.Combine(project, "Intermediate", "AssetIndex.cache.0123abcd-0123-4567-89ab-0123456789ab.tmp");
    string keptName = Path.Combine(project, "Content", "Cached", "Hero.txt.0123abcd-0123-4567-89ab-0123456789ab.tmp");
    File.WriteAllBytes(contentOrphan, [1]);
    File.WriteAllBytes(cacheOrphan, [1]);
    File.WriteAllBytes(keptName, [1]);
    File.Delete(cacheFile);
    ProcessResult rebuilt = await RunCache("asset-index-rebuild", projectOption);
    ReportCache("asset-index-rebuild", rebuilt.ExitCode == 0 && rebuilt.StdErr.Length == 0 &&
        rebuilt.StdOut == "asset-index-rebuild: 1 packages, 0 diagnostics, 2 orphans removed\n" &&
        File.Exists(cacheFile) && !File.Exists(contentOrphan) && !File.Exists(cacheOrphan) && File.Exists(keptName), rebuilt);
    File.Delete(keptName);

    // 진단이 있어도 0(점검은 verify).
    File.WriteAllBytes(Path.Combine(project, "Content", "Junk.rasset"), [1, 2, 3]);
    ProcessResult withDiagnostics = await RunCache("asset-index-rebuild", "--wait=0", projectOption);
    ReportCache("asset-index-rebuild with diagnostics", withDiagnostics.ExitCode == 0 &&
        withDiagnostics.StdOut == "asset-index-rebuild: 1 packages, 1 diagnostics, 0 orphans removed\n" &&
        withDiagnostics.StdErr.Contains(": Content/Junk.rasset: ", StringComparison.Ordinal), withDiagnostics);
    File.Delete(Path.Combine(project, "Content", "Junk.rasset"));

    // 손상된 캐시는 없는 것과 같다(목록·점검·편집 그대로, 편집이 다시 쓴다).
    File.WriteAllBytes(cacheFile, [0x52, 0x4b, 0x41, 0x49, 1, 2, 3]);
    ProcessResult corruptList = await RunCache("asset-list", projectOption);
    ReportCache("asset-list ignores a corrupt cache", corruptList.ExitCode == 0 &&
        corruptList.StdOut.StartsWith($"/Game/Cached/Hero {heroId} ", StringComparison.Ordinal) &&
        corruptList.StdErr.Length == 0, corruptList);
    ProcessResult corruptVerify = await RunCache("asset-index-verify", projectOption);
    ReportCache("asset-index-verify ignores the cache", corruptVerify.ExitCode == 0 &&
        corruptVerify.StdOut == "asset-index-verify: 1 packages, 0 diagnostics\n", corruptVerify);
    ProcessResult corruptInfo = await RunCache("asset-info", "/Game/Cached/Hero", projectOption);
    ReportCache("asset-info ignores a corrupt cache", corruptInfo.ExitCode == 0 && corruptInfo.StdErr.Length == 0, corruptInfo);
    ProcessResult rewritten = await RunCache("asset-set", "/Game/Cached/Hero", "useRetargetModesFromCompatibleSkeleton=on", projectOption);
    ReportCache("asset-set rewrites the cache", rewritten.ExitCode == 0 && rewritten.StdErr.Length == 0 &&
        new FileInfo(cacheFile).Length > 7, rewritten);

    // 캐시를 쓰지 못하면(캐시 자리가 디렉터리) 쓰기 명령은 경고와 함께 성공한다. 재구축은 실패한다.
    File.Delete(cacheFile);
    Directory.CreateDirectory(cacheFile);
    File.WriteAllBytes(Path.Combine(cacheFile, "keep"), [1]);
    ProcessResult notWritten = await RunCache("asset-set", "/Game/Cached/Hero", "useRetargetModesFromCompatibleSkeleton=off", projectOption);
    ReportCache("asset-set cache not written", notWritten.ExitCode == 0 &&
        notWritten.StdErr.Contains(": warning CacheNotWritten", StringComparison.Ordinal), notWritten);
    ProcessResult newNotWritten = await RunCache("asset-new", "Skeleton", "/Game/Cached/Other", projectOption);
    ReportCache("asset-new cache not written", newNotWritten.ExitCode == 0 &&
        newNotWritten.StdOut.StartsWith("/Game/Cached/Other ", StringComparison.Ordinal) &&
        newNotWritten.StdErr.Contains(": warning CacheNotWritten", StringComparison.Ordinal), newNotWritten);
    ProcessResult rebuildFails = await RunCache("asset-index-rebuild", projectOption);
    ReportCache("asset-index-rebuild cache not written", rebuildFails.ExitCode == 1 &&
        rebuildFails.StdErr.Contains(": cannot write the index cache", StringComparison.Ordinal), rebuildFails);
    Directory.Delete(cacheFile, true);

    // 지울 수 없는 고아(Windows — 삭제 공유 없이 연 파일)는 경고이고 재구축은 성공한다. 잠금 Busy는 실패다.
    if (OperatingSystem.IsWindows())
    {
        File.WriteAllBytes(contentOrphan, [1]);
        using (new FileStream(contentOrphan, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        {
            ProcessResult held = await RunCache("asset-index-rebuild", projectOption);
            ReportCache("asset-index-rebuild orphan not deleted", held.ExitCode == 0 &&
                held.StdErr.Contains(": Content/Cached/Hero.rasset.0123abcd-0123-4567-89ab-0123456789ab.tmp: warning OrphanNotDeleted",
                    StringComparison.Ordinal) && held.StdOut.EndsWith(", 0 orphans removed\n", StringComparison.Ordinal), held);
        }

        File.Delete(contentOrphan);
        string lockPath = Path.Combine(project, "Intermediate", "AssetDatabase.lock");
        using (FileStream locked = new(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite))
        {
            locked.Lock(0, 1);
            ProcessResult busy = await RunCache("asset-index-rebuild", projectOption);
            ReportCache("asset-index-rebuild locked", busy.ExitCode == 1 &&
                busy.StdErr.Contains("asset-index-rebuild: Busy: ", StringComparison.Ordinal), busy);
            locked.Unlock(0, 1);
        }

        uint code = WindowsInvalidStandardHandles.Run(executable, $"asset-index-rebuild \"--project={project}\"", workingDirectory);
        Console.WriteLine($"invalid output handles (asset-index-rebuild): {(code == 1 ? "PASS" : "FAIL")}");
        if (code != 1)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={code}");
        }
    }

    // 사용법·환경.
    string[][] rebuildUsage =
    [
        ["asset-index-rebuild", "extra", projectOption],
        ["asset-index-rebuild", "--under=/Game", projectOption],
        ["asset-index-rebuild", "--wait=86401", projectOption],
        ["asset-index-rebuild", "--wait=5s", projectOption],
        ["asset-index-rebuild", projectOption, projectOption],
    ];
    foreach (string[] arguments in rebuildUsage)
    {
        ProcessResult usage = await RunCache(arguments);
        ReportCache($"asset-index-rebuild usage: {string.Join(' ', arguments.Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-index-rebuild", StringComparison.Ordinal), usage);
    }

    ProcessResult help = await RunCache("asset-index-rebuild", "--help");
    ReportCache("asset-index-rebuild help", help.ExitCode == 0 && help.StdOut.StartsWith("usage: asset-index-rebuild", StringComparison.Ordinal), help);
    ProcessResult noProject = await RunCache("asset-index-rebuild");
    ReportCache("asset-index-rebuild without a project", noProject.ExitCode == 1 &&
        noProject.StdErr.Contains("no project found", StringComparison.Ordinal), noProject);
    string brokenContent = Path.Combine(cacheDirectory, "BrokenContent");
    string brokenLock = Path.Combine(cacheDirectory, "BrokenLock");
    foreach (string directory in new[] { brokenContent, brokenLock })
    {
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, $"{Path.GetFileName(directory)}.rocksproject"), "{}\n");
    }

    File.WriteAllBytes(Path.Combine(brokenContent, "Content"), [1]);
    File.WriteAllBytes(Path.Combine(brokenLock, "Intermediate"), [1]);
    ProcessResult scanFails = await RunCache("asset-index-rebuild", $"--project={brokenContent}");
    ReportCache("asset-index-rebuild content is a file", scanFails.ExitCode == 1 &&
        scanFails.StdErr.Contains(": cannot scan the content directory", StringComparison.Ordinal), scanFails);
    ProcessResult lockFails = await RunCache("asset-index-rebuild", $"--project={brokenLock}");
    ReportCache("asset-index-rebuild cannot lock", lockFails.ExitCode == 1 &&
        lockFails.StdErr.Contains(": cannot lock the project", StringComparison.Ordinal), lockFails);
}
finally
{
    Directory.Delete(cacheDirectory, true);
}

// asset-alias-*·별칭 해석(AssetSystem 4.3절, 7.1절 17 — A2e1): 목록(만료·shadowed·missing·지금 경로), 열기가 옛
// 경로를 별칭으로 풀고 경고 AliasResolved, 별칭 경로에 새 패키지는 DuplicatePath, prune은 Config 텍스트에 쓰인 만료 항목을
// 남기고 나머지 만료 항목을 지운다, remove, 깨진 표의 진단 줄, 사용법·환경 오류, 재구축이 별칭 표 임시 파일을 지운다.
string aliasDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-alias-{Environment.ProcessId}");
try
{
    string project = Path.Combine(aliasDirectory, "Aliased");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Aliased.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";
    string configDirectory = Path.Combine(project, "Config");
    string aliasFile = Path.Combine(configDirectory, "AssetAliases.json");

    async Task<ProcessResult> RunAlias(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportAlias(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    void ProjectResultCheck(ProcessResult result, string label, Func<ProcessResult, bool> check) =>
        ReportAlias(label, check(result), result);

    ProcessResult made = await RunAlias("asset-new", "Skeleton", "/Game/Al/Hero", projectOption);
    ReportAlias("alias: asset-new", made.ExitCode == 0, made);
    string heroId = made.StdOut.Trim().Split(' ').Last();
    const string missingId = "5e570000-a55e-7000-0000-00000000dead";

    // 별칭 표: 만료(2000년)·만료 아님(2100년)·지금 경로와 같은 경로(shadowed)·없는 대상.
    string Alias(string path, string asset, string created, int days) =>
        $"{{\"path\": \"{path}\", \"asset\": \"{asset}\", \"created\": \"{created}\", \"days\": {days}}}";
    void WriteAliases(params string[] entries)
    {
        Directory.CreateDirectory(configDirectory);
        File.WriteAllText(aliasFile, "{\"version\": 1, \"aliases\": [" + string.Join(", ", entries) + "]}\n");
    }

    WriteAliases(
        Alias("/Game/Al/OldHero", heroId, "2000-01-01T00:00:00Z", 1),
        Alias("/Game/Al/Hero", heroId, "2100-01-01T00:00:00Z", 90),
        Alias("/Game/Al/Gone", missingId, "2100-01-01T00:00:00Z", 90),
        Alias("/Game/Al/Named", heroId, "2000-01-01T00:00:00Z", 1));
    ProcessResult listed = await RunAlias("asset-alias-list", projectOption);
    string expectedList =
        $"/Game/Al/Gone {missingId} 2100-01-01T00:00:00Z 2100-04-01T00:00:00Z -> missing\n" +
        $"/Game/Al/Hero {heroId} 2100-01-01T00:00:00Z 2100-04-01T00:00:00Z shadowed -> /Game/Al/Hero\n" +
        $"/Game/Al/Named {heroId} 2000-01-01T00:00:00Z 2000-01-02T00:00:00Z expired -> /Game/Al/Hero\n" +
        $"/Game/Al/OldHero {heroId} 2000-01-01T00:00:00Z 2000-01-02T00:00:00Z expired -> /Game/Al/Hero\n";
    ReportAlias("asset-alias-list", listed.ExitCode == 0 && listed.StdOut == expectedList && listed.StdErr.Length == 0, listed);

    // 열기가 옛 경로를 푼다(경고), 별칭 경로에 새 패키지는 점유.
    ProcessResult info = await RunAlias("asset-info", "/Game/Al/OldHero", projectOption);
    ReportAlias("asset-info through an alias", info.ExitCode == 0 &&
        info.StdErr.Contains($"asset-info: AliasResolved: /Game/Al/OldHero: warning resolved through an alias to {heroId}",
            StringComparison.Ordinal), info);
    ProcessResult occupied = await RunAlias("asset-new", "Skeleton", "/Game/Al/OldHero", projectOption);
    ReportAlias("asset-new on an alias path", occupied.ExitCode == 1 &&
        occupied.StdErr.Contains("DuplicatePath", StringComparison.Ordinal), occupied);

    // prune: Named는 Config 텍스트에 쓰여 남고 OldHero는 지운다(만료 아닌 둘은 남는다).
    File.WriteAllText(Path.Combine(configDirectory, "Game.ini"), "[Startup]\nMap=\"/Game/Al/Named\"\n");
    ProcessResult pruned = await RunAlias("asset-alias-prune", projectOption);
    ReportAlias("asset-alias-prune", pruned.ExitCode == 0 &&
        pruned.StdOut == "asset-alias-prune: 1 removed, 1 in use, 2 kept\n" &&
        pruned.StdErr.Contains("asset-alias-prune: AliasInUse: Config/Game.ini:2: warning /Game/Al/Named is still named",
            StringComparison.Ordinal) &&
        !File.ReadAllText(aliasFile).Contains("/Game/Al/OldHero", StringComparison.Ordinal), pruned);

    // remove: 있으면 지운다, 없으면 NotFound 1, 경로 규칙 위반 1.
    ProcessResult removed = await RunAlias("asset-alias-remove", "/Game/Al/Named", projectOption);
    ReportAlias("asset-alias-remove", removed.ExitCode == 0 &&
        removed.StdOut == "asset-alias-remove: removed /Game/Al/Named\n" &&
        !File.ReadAllText(aliasFile).Contains("/Game/Al/Named", StringComparison.Ordinal), removed);
    ProcessResult notThere = await RunAlias("asset-alias-remove", "/Game/Al/Named", projectOption);
    ReportAlias("asset-alias-remove missing", notThere.ExitCode == 1 &&
        notThere.StdErr.Contains("NotFound: /Game/Al/Named: no alias", StringComparison.Ordinal), notThere);
    ProcessResult badPath = await RunAlias("asset-alias-remove", "Game/Al/Named", projectOption);
    ReportAlias("asset-alias-remove invalid path", badPath.ExitCode == 1 &&
        badPath.StdErr.Contains("InvalidPath: Game/Al/Named", StringComparison.Ordinal), badPath);

    // 깨진 표: JSON 문법(줄·열)과 칸 위반(항목 번호) — 별칭 명령은 1, 색인에 있는 경로 열기는 그대로.
    File.WriteAllText(aliasFile, "{\n  \"version\": 1,\n  \"aliases\": [,]\n}\n");
    ProcessResult syntax = await RunAlias("asset-alias-list", projectOption);
    ReportAlias("asset-alias-list corrupt JSON", syntax.ExitCode == 1 &&
        syntax.StdErr.Contains("CorruptAliasTable: Config/AssetAliases.json: JSON syntax at line 3 column", StringComparison.Ordinal), syntax);
    WriteAliases(Alias("/Game/Al/X", heroId, "2000-01-01T00:00:00Z", 0));
    ProcessResult field = await RunAlias("asset-alias-prune", projectOption);
    ReportAlias("asset-alias-prune corrupt field", field.ExitCode == 1 &&
        field.StdErr.Contains("CorruptAliasTable: Config/AssetAliases.json: invalid days (entry 0)", StringComparison.Ordinal), field);
    File.WriteAllText(aliasFile, "{\"version\": 2, \"aliases\": []}");
    ProcessResult header = await RunAlias("asset-alias-remove", "/Game/Al/X", projectOption);
    ReportAlias("asset-alias-remove corrupt header", header.ExitCode == 1 &&
        header.StdErr.Contains("CorruptAliasTable: Config/AssetAliases.json: invalid version", StringComparison.Ordinal), header);
    ProcessResult infoDirect = await RunAlias("asset-info", "/Game/Al/Hero", projectOption);
    ReportAlias("asset-info ignores a corrupt alias table", infoDirect.ExitCode == 0, infoDirect);
    File.Delete(aliasFile);

    // 쓰기 실패: 별칭 표 자리가 디렉터리.
    WriteAliases(Alias("/Game/Al/Y", heroId, "2000-01-01T00:00:00Z", 1));
    ProcessResult beforeDirectory = await RunAlias("asset-alias-list", projectOption);
    File.Delete(aliasFile);
    Directory.CreateDirectory(aliasFile);
    ProcessResult unreadable = await RunAlias("asset-alias-list", projectOption);
    ReportAlias("asset-alias-list alias table is a directory", beforeDirectory.ExitCode == 0 && unreadable.ExitCode == 1 &&
        unreadable.StdErr.Contains(": Config/AssetAliases.json: cannot read the alias table", StringComparison.Ordinal), unreadable);
    Directory.Delete(aliasFile);

    // 재구축이 Config의 별칭 표 임시 파일을 지운다.
    string aliasOrphan = Path.Combine(configDirectory, "AssetAliases.json.0123abcd-0123-4567-89ab-0123456789ab.tmp");
    File.WriteAllBytes(aliasOrphan, [1]);
    ProcessResult rebuilt = await RunAlias("asset-index-rebuild", projectOption);
    ReportAlias("asset-index-rebuild removes alias temporary files", rebuilt.ExitCode == 0 &&
        rebuilt.StdOut.EndsWith(", 1 orphans removed\n", StringComparison.Ordinal) && !File.Exists(aliasOrphan), rebuilt);

    // 잠금 Busy(Windows), 사용법, 도움말, 프로젝트 없음.
    if (OperatingSystem.IsWindows())
    {
        string lockPath = Path.Combine(project, "Intermediate", "AssetDatabase.lock");
        using FileStream locked = new(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        locked.Lock(0, 1);
        foreach (string[] arguments in new[] { new[] { "asset-alias-remove", "/Game/Al/Y" }, new[] { "asset-alias-prune" } })
        {
            ProcessResult busy = await RunAlias([.. arguments, projectOption]);
            ReportAlias($"{arguments[0]} locked", busy.ExitCode == 1 &&
                busy.StdErr.Contains($"{arguments[0]}: Busy: ", StringComparison.Ordinal), busy);
        }

        locked.Unlock(0, 1);
    }

    // prune 검색이 상한(파일 하나 64 MiB)에 닿으면 아무것도 지우지 않고 경고한다.
    WriteAliases(Alias("/Game/Al/Y", heroId, "2000-01-01T00:00:00Z", 1));
    string hugeFile = Path.Combine(configDirectory, "Huge.bin");
    using (FileStream huge = new(hugeFile, FileMode.Create, FileAccess.Write))
    {
        huge.SetLength((64L << 20) + 1);
    }

    ProjectResultCheck(await RunAlias("asset-alias-prune", projectOption), "asset-alias-prune truncated", result =>
        result.ExitCode == 0 && result.StdOut == "asset-alias-prune: 0 removed, 0 in use, 1 kept\n" &&
        result.StdErr.Contains("AliasSearchTruncated: ", StringComparison.Ordinal) &&
        File.ReadAllText(aliasFile).Contains("/Game/Al/Y", StringComparison.Ordinal));
    File.Delete(hugeFile);

    if (OperatingSystem.IsWindows())
    {
        // 별칭 표를 삭제 공유 없이 열어 두면 교체가 실패한다(remove — 표 그대로).
        using (new FileStream(aliasFile, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            ProjectResultCheck(await RunAlias("asset-alias-remove", "/Game/Al/Y", projectOption), "asset-alias-remove cannot write",
                result => result.ExitCode == 1 &&
                    result.StdErr.Contains(": Config/AssetAliases.json: cannot write the alias table", StringComparison.Ordinal));
        }

        // 검색할 파일을 공유 없이 열어 두면 읽기가 실패한다(prune — 표 그대로).
        string lockedText = Path.Combine(configDirectory, "Locked.ini");
        File.WriteAllText(lockedText, "x\n");
        using (new FileStream(lockedText, FileMode.Open, FileAccess.Read, FileShare.None))
        {
            ProjectResultCheck(await RunAlias("asset-alias-prune", projectOption), "asset-alias-prune cannot search",
                result => result.ExitCode == 1 &&
                    result.StdErr.Contains(": cannot search or write (AssetAliases.json)", StringComparison.Ordinal) &&
                    File.ReadAllText(aliasFile).Contains("/Game/Al/Y", StringComparison.Ordinal));
        }

        File.Delete(lockedText);

        // 무효 출력 핸들: 목록 줄·prune 경고 줄을 쓰지 못하면 1이다.
        File.WriteAllText(Path.Combine(configDirectory, "Uses.ini"), "/Game/Al/Y\n");
        foreach (string command in new[] { "asset-alias-list", "asset-alias-prune" })
        {
            uint code = WindowsInvalidStandardHandles.Run(executable, $"{command} \"--project={project}\"", workingDirectory);
            Console.WriteLine($"invalid output handles ({command}): {(code == 1 ? "PASS" : "FAIL")}");
            if (code != 1)
            {
                ++failures;
                Console.Error.WriteLine($"  exit={code}");
            }
        }

        File.Delete(Path.Combine(configDirectory, "Uses.ini"));

        // 검색 상한 경고 줄을 쓰지 못해도 1이다.
        using (FileStream huge = new(hugeFile, FileMode.Create, FileAccess.Write))
        {
            huge.SetLength((64L << 20) + 1);
        }

        uint truncatedCode = WindowsInvalidStandardHandles.Run(executable, $"asset-alias-prune \"--project={project}\"", workingDirectory);
        Console.WriteLine($"invalid output handles (asset-alias-prune truncated): {(truncatedCode == 1 ? "PASS" : "FAIL")}");
        if (truncatedCode != 1)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={truncatedCode}");
        }

        File.Delete(hugeFile);
    }

    // 환경: Content가 파일이면 목록의 검색이, Intermediate가 파일이면 잠금이 실패한다.
    string aliasBrokenContent = Path.Combine(aliasDirectory, "AliasBrokenContent");
    string aliasBrokenLock = Path.Combine(aliasDirectory, "AliasBrokenLock");
    foreach (string directory in new[] { aliasBrokenContent, aliasBrokenLock })
    {
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, $"{Path.GetFileName(directory)}.rocksproject"), "{}\n");
    }

    File.WriteAllBytes(Path.Combine(aliasBrokenContent, "Content"), [1]);
    File.WriteAllBytes(Path.Combine(aliasBrokenLock, "Intermediate"), [1]);
    ProjectResultCheck(await RunAlias("asset-alias-list", $"--project={aliasBrokenContent}"), "asset-alias-list content is a file",
        result => result.ExitCode == 1 && result.StdErr.Contains(": cannot scan the content directory", StringComparison.Ordinal));
    foreach (string[] arguments in new[] { new[] { "asset-alias-remove", "/Game/A" }, new[] { "asset-alias-prune" } })
    {
        ProjectResultCheck(await RunAlias([.. arguments, $"--project={aliasBrokenLock}"]), $"{arguments[0]} cannot lock",
            result => result.ExitCode == 1 && result.StdErr.Contains(": cannot lock the project", StringComparison.Ordinal));
    }

    string[][] aliasUsage =
    [
        ["asset-alias-list", "extra", projectOption],
        ["asset-alias-list", "--under=/Game", projectOption],
        ["asset-alias-list", "--wait=5s", projectOption],
        ["asset-alias-remove", projectOption],
        ["asset-alias-remove", "/Game/A", "/Game/B", projectOption],
        ["asset-alias-remove", "/Game/A", projectOption, projectOption],
        ["asset-alias-prune", "extra", projectOption],
        ["asset-alias-prune", "--wait=", projectOption],
    ];
    foreach (string[] arguments in aliasUsage)
    {
        ProcessResult usage = await RunAlias(arguments);
        ReportAlias($"{arguments[0]} usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith($"usage: {arguments[0]}", StringComparison.Ordinal), usage);
    }

    foreach (string name in new[] { "asset-alias-list", "asset-alias-remove", "asset-alias-prune" })
    {
        ProcessResult help = await RunAlias(name, "--help");
        ReportAlias($"{name} help", help.ExitCode == 0 && help.StdOut.StartsWith($"usage: {name}", StringComparison.Ordinal), help);
        string[] noProjectArguments = name == "asset-alias-remove" ? [name, "/Game/A"] : [name];
        ProcessResult noProject = await RunAlias(noProjectArguments);
        ReportAlias($"{name} without a project", noProject.ExitCode == 1 &&
            noProject.StdErr.Contains("no project found", StringComparison.Ordinal), noProject);
    }
}
finally
{
    Directory.Delete(aliasDirectory, true);
}

// asset-move(AssetSystem 4.4절, 7.1절 17 — A2e2): 파일·디렉터리 이동(요약 줄·디스크·옛 경로의 별칭 해석), 대소문자만
// 바꾸기(별칭 없음), 거부(대상 점유 — 색인 항목·다른 에셋 별칭, 없는 원본, 대상이 원본 아래), 사용법·도움말·프로젝트 없음·잠금 Busy.
string moveDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-move-{Environment.ProcessId}");
try
{
    string project = Path.Combine(moveDirectory, "Moving");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Moving.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";
    string content = Path.Combine(project, "Content");

    async Task<ProcessResult> RunMove(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportMove(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult hero = await RunMove("asset-new", "Skeleton", "/Game/Mv/Hero", projectOption);
    ProcessResult leaf = await RunMove("asset-new", "Skeleton", "/Game/Mv/Sub/Leaf", projectOption);
    ProcessResult single = await RunMove("asset-new", "Skeleton", "/Game/Single", projectOption);
    ReportMove("move: asset-new", hero.ExitCode == 0 && leaf.ExitCode == 0 && single.ExitCode == 0, hero);
    string heroId = hero.StdOut.Trim().Split(' ').Last();

    // 파일 하나(대상 부모 디렉터리를 만든다).
    ProcessResult file = await RunMove("asset-move", "/Game/Single", "/Game/Elsewhere/Single2", projectOption);
    ReportMove("asset-move file", file.ExitCode == 0 &&
        file.StdOut == "asset-move: 1 packages moved, 1 aliases added\n" && file.StdErr.Length == 0 &&
        File.Exists(Path.Combine(content, "Elsewhere", "Single2.rasset")) &&
        !File.Exists(Path.Combine(content, "Single.rasset")), file);

    // 디렉터리(하위 디렉터리째 한 번의 OS 이동).
    ProcessResult directory = await RunMove("asset-move", "/Game/Mv", "/Game/Moved", projectOption);
    ReportMove("asset-move directory", directory.ExitCode == 0 &&
        directory.StdOut == "asset-move: 2 packages moved, 2 aliases added\n" && directory.StdErr.Length == 0 &&
        File.Exists(Path.Combine(content, "Moved", "Hero.rasset")) &&
        File.Exists(Path.Combine(content, "Moved", "Sub", "Leaf.rasset")) &&
        !Directory.Exists(Path.Combine(content, "Mv")), directory);

    // 옛 경로는 별칭으로 열린다(경고), 새 경로는 그대로 열린다.
    ProcessResult oldPath = await RunMove("asset-info", "/Game/Mv/Hero", projectOption);
    ReportMove("asset-info through a move alias", oldPath.ExitCode == 0 &&
        oldPath.StdErr.Contains($"asset-info: AliasResolved: /Game/Mv/Hero: warning resolved through an alias to {heroId}",
            StringComparison.Ordinal), oldPath);
    ProcessResult newPath = await RunMove("asset-info", "/Game/Moved/Hero", projectOption);
    ReportMove("asset-info at the new path", newPath.ExitCode == 0 && newPath.StdErr.Length == 0, newPath);

    // 대소문자만 바꾸기(별칭 없음).
    ProcessResult caseOnly = await RunMove("asset-move", "/Game/Moved", "/Game/moved", projectOption);
    ReportMove("asset-move case only", caseOnly.ExitCode == 0 &&
        caseOnly.StdOut == "asset-move: 2 packages moved, 0 aliases added\n" &&
        new DirectoryInfo(content).GetDirectories().Any(entry => entry.Name == "moved"), caseOnly);

    // 거부: 대상 점유(색인 항목·다른 에셋의 별칭), 없는 원본, 대상이 원본 아래.
    ProcessResult occupied = await RunMove("asset-move", "/Game/Elsewhere/Single2", "/Game/moved/Hero", projectOption);
    ReportMove("asset-move onto a package", occupied.ExitCode == 1 &&
        occupied.StdErr.Contains($"asset-move: DuplicatePath: /Game/Elsewhere/Single2: AlreadyExists (related {heroId})",
            StringComparison.Ordinal) && File.Exists(Path.Combine(content, "Elsewhere", "Single2.rasset")), occupied);
    ProcessResult onAlias = await RunMove("asset-move", "/Game/Elsewhere/Single2", "/Game/Mv/Hero", projectOption);
    ReportMove("asset-move onto another asset's alias", onAlias.ExitCode == 1 &&
        onAlias.StdErr.Contains($"asset-move: DuplicatePath: /Game/Elsewhere/Single2: AlreadyExists (related {heroId})",
            StringComparison.Ordinal), onAlias);
    ProcessResult missing = await RunMove("asset-move", "/Game/Nowhere", "/Game/X", projectOption);
    ReportMove("asset-move missing source", missing.ExitCode == 1 &&
        missing.StdErr.Contains("asset-move: NotFound: /Game/Nowhere: failed", StringComparison.Ordinal), missing);
    ProcessResult inside = await RunMove("asset-move", "/Game/moved", "/Game/moved/Inner", projectOption);
    ReportMove("asset-move into itself", inside.ExitCode == 1 &&
        inside.StdErr.Contains("asset-move: InvalidArgument: /Game/moved: failed", StringComparison.Ordinal), inside);

    // 잠금 Busy(Windows), 사용법, 도움말, 프로젝트 없음.
    if (OperatingSystem.IsWindows())
    {
        string lockPath = Path.Combine(project, "Intermediate", "AssetDatabase.lock");
        using FileStream locked = new(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        locked.Lock(0, 1);
        ProcessResult busy = await RunMove("asset-move", "/Game/moved", "/Game/Other", projectOption);
        ReportMove("asset-move locked", busy.ExitCode == 1 &&
            busy.StdErr.Contains("asset-move: Busy: ", StringComparison.Ordinal), busy);
        locked.Unlock(0, 1);
    }

    string[][] moveUsage =
    [
        ["asset-move", projectOption],
        ["asset-move", "/Game/A", projectOption],
        ["asset-move", "/Game/A", "/Game/B", "/Game/C", projectOption],
        ["asset-move", "/Game/A", "/Game/B", "--wait=", projectOption],
        ["asset-move", "/Game/A", "/Game/B", "--json", projectOption],
    ];
    foreach (string[] arguments in moveUsage)
    {
        ProcessResult usage = await RunMove(arguments);
        ReportMove($"asset-move usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-move", StringComparison.Ordinal), usage);
    }

    ProcessResult moveHelp = await RunMove("asset-move", "--help");
    ReportMove("asset-move help", moveHelp.ExitCode == 0 &&
        moveHelp.StdOut.StartsWith("usage: asset-move", StringComparison.Ordinal), moveHelp);
    ProcessResult moveNoProject = await RunMove("asset-move", "/Game/A", "/Game/B");
    ReportMove("asset-move without a project", moveNoProject.ExitCode == 1 &&
        moveNoProject.StdErr.Contains("no project found", StringComparison.Ordinal), moveNoProject);
}
finally
{
    Directory.Delete(moveDirectory, true);
}

// asset-delete(AssetSystem 7.1절 17 — A2e3a): 지우기(요약 줄·파일·지운 별칭 경고 줄), 디스크 참조자 거부(참조자 줄·
// 진단 줄)와 --force(경고 줄 — 참조자는 MissingDependency), id로 고르기, 없음·별칭 경로 따르지 않음·경로 규칙, 잠금 Busy,
// 사용법·도움말·프로젝트 없음.
string deleteDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-delete-{Environment.ProcessId}");
try
{
    string project = Path.Combine(deleteDirectory, "Deleting");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Deleting.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";
    string content = Path.Combine(project, "Content");

    async Task<ProcessResult> RunDelete(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportDelete(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult hero = await RunDelete("asset-new", "Skeleton", "/Game/Dl/Hero", projectOption);
    ProcessResult run = await RunDelete("asset-new", "AnimationClip", "/Game/Dl/Run", "skeleton=/Game/Dl/Hero", projectOption);
    ProcessResult leaf = await RunDelete("asset-new", "Skeleton", "/Game/Dl/Leaf", projectOption);
    ProcessResult moved = await RunDelete("asset-move", "/Game/Dl/Leaf", "/Game/Dl/Leaf2", projectOption);
    ReportDelete("delete: setup", hero.ExitCode == 0 && run.ExitCode == 0 && leaf.ExitCode == 0 && moved.ExitCode == 0, run);
    string heroId = hero.StdOut.Trim().Split(' ').Last();
    string runId = run.StdOut.Trim().Split(' ').Last();
    string leafId = leaf.StdOut.Trim().Split(' ').Last();

    // 지우기와 그 에셋을 가리키던 별칭.
    ProcessResult deleted = await RunDelete("asset-delete", "/Game/Dl/Leaf2", projectOption);
    ReportDelete("asset-delete", deleted.ExitCode == 0 &&
        deleted.StdOut == $"asset-delete: deleted /Game/Dl/Leaf2 {leafId}\n" &&
        deleted.StdErr.Contains($"asset-delete: AliasRemoved: /Game/Dl/Leaf: warning removed the alias to {leafId}",
            StringComparison.Ordinal) &&
        !File.Exists(Path.Combine(content, "Dl", "Leaf2.rasset")), deleted);
    // 별칭이 지워졌으므로 옛 경로는 없다.
    ProcessResult oldInfo = await RunDelete("asset-info", "/Game/Dl/Leaf", projectOption);
    ReportDelete("asset-info after delete", oldInfo.ExitCode == 1 &&
        oldInfo.StdErr.Contains("asset-info: NotFound: /Game/Dl/Leaf", StringComparison.Ordinal), oldInfo);

    // 디스크 참조자: 거부(참조자 줄·진단 줄), --force는 경고 줄과 함께 지운다.
    ProcessResult refused = await RunDelete("asset-delete", "/Game/Dl/Hero", projectOption);
    ReportDelete("asset-delete referenced", refused.ExitCode == 1 &&
        refused.StdErr.Contains($"asset-delete: ReferencedBy: /Game/Dl/Run: {runId} references /Game/Dl/Hero",
            StringComparison.Ordinal) &&
        refused.StdErr.Contains($"asset-delete: ReferencedBy: /Game/Dl/Hero: InvalidArgument (related {runId})",
            StringComparison.Ordinal) &&
        refused.StdOut.Length == 0 && File.Exists(Path.Combine(content, "Dl", "Hero.rasset")), refused);
    ProcessResult forced = await RunDelete("asset-delete", "/Game/Dl/Hero", "--force", projectOption);
    ReportDelete("asset-delete --force", forced.ExitCode == 0 &&
        forced.StdErr.Contains($"asset-delete: ReferencedBy: /Game/Dl/Run: warning {runId} references /Game/Dl/Hero",
            StringComparison.Ordinal) &&
        forced.StdOut == $"asset-delete: deleted /Game/Dl/Hero {heroId}\n" &&
        !File.Exists(Path.Combine(content, "Dl", "Hero.rasset")), forced);
    ProcessResult verified = await RunDelete("asset-index-verify", projectOption);
    ReportDelete("asset-index-verify after a forced delete", verified.StdErr.Contains("MissingDependency", StringComparison.Ordinal),
        verified);

    // id로 고르기, 없음·별칭 경로·경로 규칙.
    ProcessResult byId = await RunDelete("asset-delete", runId, projectOption);
    ReportDelete("asset-delete by id", byId.ExitCode == 0 &&
        byId.StdOut == $"asset-delete: deleted /Game/Dl/Run {runId}\n", byId);
    ProcessResult missing = await RunDelete("asset-delete", "/Game/Nowhere", projectOption);
    ReportDelete("asset-delete missing", missing.ExitCode == 1 &&
        missing.StdErr.Contains("asset-delete: NotFound: /Game/Nowhere: failed", StringComparison.Ordinal), missing);
    ProcessResult invalid = await RunDelete("asset-delete", "Game/Bad", projectOption);
    ReportDelete("asset-delete invalid path", invalid.ExitCode == 1 &&
        invalid.StdErr.Contains("asset-delete: InvalidPath: Game/Bad", StringComparison.Ordinal), invalid);

    if (OperatingSystem.IsWindows())
    {
        string lockPath = Path.Combine(project, "Intermediate", "AssetDatabase.lock");
        using FileStream locked = new(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        locked.Lock(0, 1);
        ProcessResult busy = await RunDelete("asset-delete", "/Game/Dl/Run", projectOption);
        ReportDelete("asset-delete locked", busy.ExitCode == 1 &&
            busy.StdErr.Contains("asset-delete: Busy: ", StringComparison.Ordinal), busy);
        locked.Unlock(0, 1);
    }

    string[][] deleteUsage =
    [
        ["asset-delete", projectOption],
        ["asset-delete", "/Game/A", "/Game/B", projectOption],
        ["asset-delete", "/Game/A", "--wait=", projectOption],
        ["asset-delete", "/Game/A", "--json", projectOption],
    ];
    foreach (string[] arguments in deleteUsage)
    {
        ProcessResult usage = await RunDelete(arguments);
        ReportDelete($"asset-delete usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-delete", StringComparison.Ordinal), usage);
    }

    ProcessResult deleteHelp = await RunDelete("asset-delete", "--help");
    ReportDelete("asset-delete help", deleteHelp.ExitCode == 0 &&
        deleteHelp.StdOut.StartsWith("usage: asset-delete", StringComparison.Ordinal), deleteHelp);
    ProcessResult deleteNoProject = await RunDelete("asset-delete", "/Game/A");
    ReportDelete("asset-delete without a project", deleteNoProject.ExitCode == 1 &&
        deleteNoProject.StdErr.Contains("no project found", StringComparison.Ordinal), deleteNoProject);
}
finally
{
    Directory.Delete(deleteDirectory, true);
}

// asset-duplicate(AssetSystem 7.1절 17 — A2e3b): 경로·id로 고른 원본의 사본(새 id·새 파일·열림), 대상 점유(패키지·다른 에셋 별칭),
// 없는 원본·대상 경로 규칙, 잠금 Busy, 사용법·도움말·프로젝트 없음.
string duplicateDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-duplicate-{Environment.ProcessId}");
try
{
    string project = Path.Combine(duplicateDirectory, "Duplicating");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Duplicating.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";
    string content = Path.Combine(project, "Content");

    async Task<ProcessResult> RunDuplicate(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportDuplicate(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult hero = await RunDuplicate("asset-new", "Skeleton", "/Game/Dp/Hero", projectOption);
    ProcessResult run = await RunDuplicate("asset-new", "AnimationClip", "/Game/Dp/Run", "skeleton=/Game/Dp/Hero", "rateScale=2",
        projectOption);
    ReportDuplicate("duplicate: setup", hero.ExitCode == 0 && run.ExitCode == 0, run);
    string heroId = hero.StdOut.Trim().Split(' ').Last();
    string runId = run.StdOut.Trim().Split(' ').Last();

    // 경로로 고른 원본: 새 id·새 파일, 사본을 열면 값과 참조가 같다.
    ProcessResult copied = await RunDuplicate("asset-duplicate", "/Game/Dp/Run", "/Game/Dp/Run2", projectOption);
    string copyId = copied.StdOut.Trim().Split(' ').Last();
    ReportDuplicate("asset-duplicate", copied.ExitCode == 0 && copied.StdOut.StartsWith("/Game/Dp/Run2 ", StringComparison.Ordinal) &&
        copyId != runId && copyId.Length == 36 && copied.StdErr.Length == 0 &&
        File.Exists(Path.Combine(content, "Dp", "Run2.rasset")) && File.Exists(Path.Combine(content, "Dp", "Run.rasset")), copied);
    ProcessResult copyInfo = await RunDuplicate("asset-info", "/Game/Dp/Run2", projectOption);
    ReportDuplicate("asset-info of a copy", copyInfo.ExitCode == 0 &&
        copyInfo.StdOut.Contains("skeleton=/Game/Dp/Hero\n", StringComparison.Ordinal) &&
        copyInfo.StdOut.Contains("rateScale=2\n", StringComparison.Ordinal), copyInfo);

    // id로 고른 원본.
    ProcessResult byId = await RunDuplicate("asset-duplicate", heroId, "/Game/Dp/Hero2", projectOption);
    ReportDuplicate("asset-duplicate by id", byId.ExitCode == 0 && byId.StdOut.StartsWith("/Game/Dp/Hero2 ", StringComparison.Ordinal) &&
        !byId.StdOut.Contains(heroId, StringComparison.Ordinal), byId);

    // 거부: 대상이 패키지·다른 에셋의 별칭, 없는 원본, 대상 경로 규칙.
    ProcessResult occupied = await RunDuplicate("asset-duplicate", "/Game/Dp/Run", "/Game/Dp/Hero", projectOption);
    ReportDuplicate("asset-duplicate onto a package", occupied.ExitCode == 1 &&
        occupied.StdErr.Contains("asset-duplicate: DuplicatePath: /Game/Dp/Run: AlreadyExists", StringComparison.Ordinal), occupied);
    ProcessResult moved = await RunDuplicate("asset-move", "/Game/Dp/Hero2", "/Game/Dp/Hero3", projectOption);
    string hero2Id = byId.StdOut.Trim().Split(' ').Last();
    ProcessResult onAlias = await RunDuplicate("asset-duplicate", "/Game/Dp/Run", "/Game/Dp/Hero2", projectOption);
    ReportDuplicate("asset-duplicate onto an alias", moved.ExitCode == 0 && onAlias.ExitCode == 1 &&
        onAlias.StdErr.Contains($"asset-duplicate: DuplicatePath: /Game/Dp/Run: AlreadyExists (related {hero2Id})",
            StringComparison.Ordinal), onAlias);
    ProcessResult missing = await RunDuplicate("asset-duplicate", "/Game/Nowhere", "/Game/X", projectOption);
    ReportDuplicate("asset-duplicate missing source", missing.ExitCode == 1 &&
        missing.StdErr.Contains("asset-duplicate: NotFound: /Game/Nowhere", StringComparison.Ordinal), missing);
    ProcessResult invalid = await RunDuplicate("asset-duplicate", "/Game/Dp/Run", "Game/X", projectOption);
    ReportDuplicate("asset-duplicate invalid target", invalid.ExitCode == 1 &&
        invalid.StdErr.Contains("asset-duplicate: InvalidPath: /Game/Dp/Run", StringComparison.Ordinal), invalid);

    if (OperatingSystem.IsWindows())
    {
        string lockPath = Path.Combine(project, "Intermediate", "AssetDatabase.lock");
        using FileStream locked = new(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        locked.Lock(0, 1);
        ProcessResult busy = await RunDuplicate("asset-duplicate", "/Game/Dp/Run", "/Game/Dp/Run9", projectOption);
        ReportDuplicate("asset-duplicate locked", busy.ExitCode == 1 &&
            busy.StdErr.Contains("asset-duplicate: Busy: ", StringComparison.Ordinal), busy);
        locked.Unlock(0, 1);
    }

    string[][] duplicateUsage =
    [
        ["asset-duplicate", projectOption],
        ["asset-duplicate", "/Game/A", projectOption],
        ["asset-duplicate", "/Game/A", "/Game/B", "/Game/C", projectOption],
        ["asset-duplicate", "/Game/A", "/Game/B", "--force", projectOption],
    ];
    foreach (string[] arguments in duplicateUsage)
    {
        ProcessResult usage = await RunDuplicate(arguments);
        ReportDuplicate($"asset-duplicate usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-duplicate", StringComparison.Ordinal), usage);
    }

    ProcessResult duplicateHelp = await RunDuplicate("asset-duplicate", "--help");
    ReportDuplicate("asset-duplicate help", duplicateHelp.ExitCode == 0 &&
        duplicateHelp.StdOut.StartsWith("usage: asset-duplicate", StringComparison.Ordinal), duplicateHelp);
    ProcessResult duplicateNoProject = await RunDuplicate("asset-duplicate", "/Game/A", "/Game/B");
    ReportDuplicate("asset-duplicate without a project", duplicateNoProject.ExitCode == 1 &&
        duplicateNoProject.StdErr.Contains("no project found", StringComparison.Ordinal), duplicateNoProject);
}
finally
{
    Directory.Delete(duplicateDirectory, true);
}

// asset-fix-duplicate-id(AssetSystem 7.1절 17 — A2e4b): 밖에서 복사한 Skeleton 사본을 새 id로(출력 두 줄 — 옛 id 참조는 남은
// 원본으로), verify의 같은 id 진단이 사라지고 원본을 참조하는 AnimationClip은 그대로, 같은 id가 아님·없음·경로 규칙 거부, 잠금 Busy,
// 사용법·도움말·프로젝트 없음. OrphanedOutput 경고는 명령이 임포트 기록을 만들 수 없어(A2f 전) C++ 시험이 맡는다.
string fixDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-fix-id-{Environment.ProcessId}");
try
{
    string project = Path.Combine(fixDirectory, "Fixing");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Fixing.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";
    string content = Path.Combine(project, "Content");

    async Task<ProcessResult> RunFix(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportFix(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult hero = await RunFix("asset-new", "Skeleton", "/Game/Fx/Hero", projectOption);
    ProcessResult run = await RunFix("asset-new", "AnimationClip", "/Game/Fx/Run", "skeleton=/Game/Fx/Hero", projectOption);
    ReportFix("fix-id: setup", hero.ExitCode == 0 && run.ExitCode == 0, run);
    string heroId = hero.StdOut.Trim().Split(' ').Last();
    File.Copy(Path.Combine(content, "Fx", "Hero.rasset"), Path.Combine(content, "Fx", "Hero2.rasset"));
    ProcessResult twin = await RunFix("asset-index-verify", projectOption);
    ReportFix("fix-id: duplicated id before", twin.ExitCode == 1 &&
        twin.StdErr.Contains("DuplicateAssetId", StringComparison.Ordinal), twin);

    ProcessResult fixedId = await RunFix("asset-fix-duplicate-id", "/Game/Fx/Hero2", projectOption);
    string[] lines = fixedId.StdOut.Split('\n', StringSplitOptions.RemoveEmptyEntries);
    string newId = lines.Length == 2 ? lines[0].Split(' ').Last() : "";
    ReportFix("asset-fix-duplicate-id", fixedId.ExitCode == 0 && fixedId.StdErr.Length == 0 && lines.Length == 2 &&
        lines[0] == $"asset-fix-duplicate-id: /Game/Fx/Hero2 {heroId} -> {newId}" && newId.Length == 36 && newId != heroId &&
        lines[1] == $"asset-fix-duplicate-id: references to {heroId} resolve to /Game/Fx/Hero", fixedId);
    ProcessResult clean = await RunFix("asset-index-verify", projectOption);
    ReportFix("fix-id: verify is clean after", clean.ExitCode == 0 && clean.StdErr.Length == 0, clean);
    ProcessResult runInfo = await RunFix("asset-info", "/Game/Fx/Run", projectOption);
    ReportFix("fix-id: the clip still references the original", runInfo.ExitCode == 0 &&
        runInfo.StdOut.Contains("skeleton=/Game/Fx/Hero\n", StringComparison.Ordinal), runInfo);
    ProcessResult copyInfo = await RunFix("asset-info", "/Game/Fx/Hero2", projectOption);
    ProcessResult fixedList = await RunFix("asset-list", "--under=/Game/Fx", projectOption);
    ReportFix("fix-id: the copy opens and lists with the new id", copyInfo.ExitCode == 0 && fixedList.ExitCode == 0 &&
        fixedList.StdOut.Contains($"/Game/Fx/Hero {heroId}", StringComparison.Ordinal) &&
        fixedList.StdOut.Contains($"/Game/Fx/Hero2 {newId}", StringComparison.Ordinal), fixedList);

    // 같은 id가 셋이면 하나를 고쳐도 남은 둘은 아직 같은 id다(still shared by — 경로 키 순), 하나를 더 고치면 원본으로 풀린다.
    File.Copy(Path.Combine(content, "Fx", "Hero.rasset"), Path.Combine(content, "Fx", "Hero3.rasset"));
    File.Copy(Path.Combine(content, "Fx", "Hero.rasset"), Path.Combine(content, "Fx", "Hero4.rasset"));
    ProcessResult shared = await RunFix("asset-fix-duplicate-id", "/Game/Fx/Hero3", projectOption);
    string[] sharedLines = shared.StdOut.Split('\n', StringSplitOptions.RemoveEmptyEntries);
    ReportFix("asset-fix-duplicate-id with two remaining", shared.ExitCode == 0 && sharedLines.Length == 3 &&
        sharedLines[1] == $"asset-fix-duplicate-id: {heroId} is still shared by /Game/Fx/Hero" &&
        sharedLines[2] == $"asset-fix-duplicate-id: {heroId} is still shared by /Game/Fx/Hero4", shared);
    ProcessResult last = await RunFix("asset-fix-duplicate-id", "/Game/Fx/Hero4", projectOption);
    ReportFix("asset-fix-duplicate-id the last copy", last.ExitCode == 0 &&
        last.StdOut.EndsWith($"asset-fix-duplicate-id: references to {heroId} resolve to /Game/Fx/Hero\n", StringComparison.Ordinal),
        last);

    ProcessResult notDuplicate = await RunFix("asset-fix-duplicate-id", "/Game/Fx/Hero", projectOption);
    ReportFix("asset-fix-duplicate-id not a duplicate", notDuplicate.ExitCode == 1 &&
        notDuplicate.StdErr.Contains("asset-fix-duplicate-id: InvalidArgument: /Game/Fx/Hero", StringComparison.Ordinal), notDuplicate);
    ProcessResult missing = await RunFix("asset-fix-duplicate-id", "/Game/Nowhere", projectOption);
    ReportFix("asset-fix-duplicate-id missing", missing.ExitCode == 1 &&
        missing.StdErr.Contains("asset-fix-duplicate-id: NotFound: /Game/Nowhere", StringComparison.Ordinal), missing);
    ProcessResult invalid = await RunFix("asset-fix-duplicate-id", "Game/X", projectOption);
    ReportFix("asset-fix-duplicate-id invalid path", invalid.ExitCode == 1 &&
        invalid.StdErr.Contains("asset-fix-duplicate-id: InvalidPath: Game/X", StringComparison.Ordinal), invalid);

    if (OperatingSystem.IsWindows())
    {
        string lockPath = Path.Combine(project, "Intermediate", "AssetDatabase.lock");
        using FileStream locked = new(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        locked.Lock(0, 1);
        ProcessResult busy = await RunFix("asset-fix-duplicate-id", "/Game/Fx/Hero2", projectOption);
        ReportFix("asset-fix-duplicate-id locked", busy.ExitCode == 1 &&
            busy.StdErr.Contains("asset-fix-duplicate-id: Busy: ", StringComparison.Ordinal), busy);
        locked.Unlock(0, 1);
    }

    string[][] fixUsage =
    [
        ["asset-fix-duplicate-id", projectOption],
        ["asset-fix-duplicate-id", "/Game/A", "/Game/B", projectOption],
        ["asset-fix-duplicate-id", "/Game/A", "--force", projectOption],
    ];
    foreach (string[] arguments in fixUsage)
    {
        ProcessResult usage = await RunFix(arguments);
        ReportFix($"asset-fix-duplicate-id usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-fix-duplicate-id", StringComparison.Ordinal), usage);
    }

    ProcessResult fixHelp = await RunFix("asset-fix-duplicate-id", "--help");
    ReportFix("asset-fix-duplicate-id help", fixHelp.ExitCode == 0 &&
        fixHelp.StdOut.StartsWith("usage: asset-fix-duplicate-id", StringComparison.Ordinal), fixHelp);
    ProcessResult fixNoProject = await RunFix("asset-fix-duplicate-id", "/Game/A");
    ReportFix("asset-fix-duplicate-id without a project", fixNoProject.ExitCode == 1 &&
        fixNoProject.StdErr.Contains("no project found", StringComparison.Ordinal), fixNoProject);
}
finally
{
    Directory.Delete(fixDirectory, true);
}

// asset-import(AssetSystem 7.1절 18 — A2f1b): 제품 실행 파일은 실제 임포터만 등록하고 이 단계에는 아직 없다(A5 텍스처) — 임포터 없음
// UnsupportedSourceFormat, 사용법·도움말·프로젝트 없음. 성공·출력·경고 줄은 EngineTests가 시험 임포터로 명령 함수를 부른다.
string importDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-import-{Environment.ProcessId}");
try
{
    string project = Path.Combine(importDirectory, "Importing");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Importing.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";
    string source = Path.Combine(importDirectory, "Rock.png");
    File.WriteAllBytes(source, [1, 2, 3]);

    async Task<ProcessResult> RunImport(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportImport(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult noImporter = await RunImport("asset-import", "/Game/Rock", source, projectOption);
    ReportImport("asset-import without an importer", noImporter.ExitCode == 1 &&
        noImporter.StdErr.Contains("asset-import: UnsupportedSourceFormat: /Game/Rock", StringComparison.Ordinal) &&
        !File.Exists(Path.Combine(project, "Content", "Rock.rasset")), noImporter);
    ProcessResult named = await RunImport("asset-import", "/Game/Rock", source, "--importer=Texture", projectOption);
    ReportImport("asset-import unknown importer name", named.ExitCode == 1 &&
        named.StdErr.Contains("asset-import: UnsupportedSourceFormat: /Game/Rock", StringComparison.Ordinal), named);

    string[][] importUsage =
    [
        ["asset-import", projectOption],
        ["asset-import", "/Game/A", projectOption],
        ["asset-import", "/Game/A", "a.png", "b.png", projectOption],
        ["asset-import", "/Game/A", "a.png", "--source.px=b.png", projectOption],
        ["asset-import", "/Game/A", "a.png", "--cube=x", projectOption],
        ["asset-import", "/Game/A", "a.png", "--force", projectOption],
    ];
    foreach (string[] arguments in importUsage)
    {
        ProcessResult usage = await RunImport(arguments);
        ReportImport($"asset-import usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-import", StringComparison.Ordinal), usage);
    }

    ProcessResult importHelp = await RunImport("asset-import", "--help");
    ReportImport("asset-import help", importHelp.ExitCode == 0 &&
        importHelp.StdOut.StartsWith("usage: asset-import", StringComparison.Ordinal), importHelp);
    ProcessResult importNoProject = await RunImport("asset-import", "/Game/A", source);
    ReportImport("asset-import without a project", importNoProject.ExitCode == 1 &&
        importNoProject.StdErr.Contains("no project found", StringComparison.Ordinal), importNoProject);

    // asset-reimport(A2f2a): 기록 없는 에셋은 InvalidArgument, 사용법·도움말·프로젝트 없음.
    ProcessResult skeleton = await RunImport("asset-new", "Skeleton", "/Game/Plain", projectOption);
    ProcessResult notImported = await RunImport("asset-reimport", "/Game/Plain", projectOption);
    ReportImport("asset-reimport without an import record", skeleton.ExitCode == 0 && notImported.ExitCode == 1 &&
        notImported.StdErr.Contains("asset-reimport: InvalidArgument: /Game/Plain", StringComparison.Ordinal), notImported);
    string[][] reimportUsage =
    [
        ["asset-reimport", projectOption],
        ["asset-reimport", "/Game/A", "a.png", projectOption],
        ["asset-reimport", "/Game/A", "--importer=x", projectOption],
        ["asset-reimport", "/Game/A", "--source=a", "--source.px=b", projectOption],
    ];
    foreach (string[] arguments in reimportUsage)
    {
        ProcessResult usage = await RunImport(arguments);
        ReportImport($"asset-reimport usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-reimport", StringComparison.Ordinal), usage);
    }

    ProcessResult reimportHelp = await RunImport("asset-reimport", "--help");
    ReportImport("asset-reimport help", reimportHelp.ExitCode == 0 &&
        reimportHelp.StdOut.StartsWith("usage: asset-reimport", StringComparison.Ordinal), reimportHelp);
    ProcessResult reimportNoProject = await RunImport("asset-reimport", "/Game/A");
    ReportImport("asset-reimport without a project", reimportNoProject.ExitCode == 1 &&
        reimportNoProject.StdErr.Contains("no project found", StringComparison.Ordinal), reimportNoProject);

    // asset-info --import(A2f2b): 기록 없는 에셋은 InvalidArgument(패키지를 열지 않는다), 없는 경로 NotFound,
    // --all과 함께·두 번은 사용법 오류, 도움말에 --import.
    ProcessResult statusPlain = await RunImport("asset-info", "/Game/Plain", "--import", projectOption);
    ReportImport("asset-info import without an import record", statusPlain.ExitCode == 1 &&
        statusPlain.StdErr.Contains("asset-info: InvalidArgument: /Game/Plain", StringComparison.Ordinal), statusPlain);
    ProcessResult statusMissing = await RunImport("asset-info", "/Game/Nope", "--import", "--json", projectOption);
    ReportImport("asset-info import missing", statusMissing.ExitCode == 1 &&
        statusMissing.StdErr.Contains("asset-info: NotFound: /Game/Nope", StringComparison.Ordinal), statusMissing);
    string[][] statusUsage =
    [
        ["asset-info", "/Game/Plain", "--import", "--all", projectOption],
        ["asset-info", "/Game/Plain", "--import", "--import", projectOption],
    ];
    foreach (string[] arguments in statusUsage)
    {
        ProcessResult usage = await RunImport(arguments);
        ReportImport($"asset-info usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-info", StringComparison.Ordinal), usage);
    }

    ProcessResult infoHelp = await RunImport("asset-info", "--help");
    ReportImport("asset-info help mentions import", infoHelp.ExitCode == 0 &&
        infoHelp.StdOut.Contains("--import", StringComparison.Ordinal), infoHelp);
}
finally
{
    Directory.Delete(importDirectory, true);
}

// asset-cook(AssetSystem 13절, 10.2절 "A4e 구현 세부" — A4e1): 제품 실행 파일이 쿠커 모듈을 시작해 설정·보고서 타입을
// 등록하고 엔진 프로파일(실행 파일 위쪽 Engine/Config)로 쿠킹한다. 전체 쿠킹 게시·두 번째는 전부 재사용·--json·사용법·도움말. 진단 줄·
// 설정 거부·너겟 설정·verify는 EngineTests가 명령 함수를 프로세스 안에서 부른다. 셰이더 산출물(SH2d 셰이더 라이브러리 단계)은
// 아래 asset-cook --engine 시험이 보므로, 여기서는 실행 파일을 임시 배치에 복사하고 그 위쪽 Engine/Config의 Windows 프로필을
// 셰이더 없는 프로필(graphicsBackends·shaderFormats 비움)로 둔다 — 프로젝트 에셋 쿠킹 시험이 셰이더 컴파일(시간·DXC 호스트)과
// 저장소 Engine/Saved에 기대지 않게 한다.
string cookDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-cook-{Environment.ProcessId}");
try
{
    string cookExecutable = ShaderlessEngineLayout(executable, Path.Combine(cookDirectory, "Engine-layout"));
    string project = Path.Combine(cookDirectory, "Cooking");
    Directory.CreateDirectory(Path.Combine(project, "Config"));
    File.WriteAllText(Path.Combine(project, "Cooking.rocksproject"), "{}\n");
    File.WriteAllText(Path.Combine(project, "Config", "CookSettings.json"), "{\"alwaysCook\": [\"/Game/\"]}\n");
    string projectOption = $"--project={project}";

    async Task<ProcessResult> RunCook(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(cookExecutable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(60)));

    void ReportCook(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult made = await RunCook("asset-new", "Skeleton", "/Game/Hero", projectOption);
    ReportCook("asset-cook setup", made.ExitCode == 0, made);
    ProcessResult first = await RunCook("asset-cook", "--profile=Windows", projectOption);
    ReportCook("asset-cook full", first.ExitCode == 0 &&
        first.StdOut.StartsWith("asset-cook: Windows published targets 1 cooked 1 reused 0 bytes ", StringComparison.Ordinal) &&
        first.StdErr.Length == 0 && File.Exists(Path.Combine(project, "Saved", "Cooked", "Windows", "CookedManifest.rman")) &&
        File.Exists(Path.Combine(project, "Saved", "Cooked", "Windows", "CookReport.json")), first);
    ProcessResult second = await RunCook("asset-cook", "--profile=Windows", projectOption);
    ReportCook("asset-cook reuses", second.ExitCode == 0 &&
        second.StdOut == "asset-cook: Windows published targets 1 cooked 0 reused 1 bytes 0\n", second);
    ProcessResult json = await RunCook("asset-cook", "--profile=Windows", "--json", projectOption);
    ReportCook("asset-cook json", json.ExitCode == 0 && json.StdOut.StartsWith('{') &&
        json.StdOut.Contains("\"published\": true", StringComparison.Ordinal), json);
    ProcessResult missing = await RunCook("asset-cook", "--profile=Windows", "/Game/Nope", projectOption);
    ReportCook("asset-cook invalid path", missing.ExitCode == 1 &&
        missing.StdErr.Contains("asset-cook: InvalidPath: /Game/Nope: error", StringComparison.Ordinal), missing);
    ProcessResult usage = await RunCook("asset-cook", projectOption);
    ReportCook("asset-cook usage", usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-cook", StringComparison.Ordinal), usage);
    ProcessResult help = await RunCook("asset-cook", "--help");
    ReportCook("asset-cook help", help.ExitCode == 0 && help.StdOut.StartsWith("usage: asset-cook", StringComparison.Ordinal), help);

    // 제품 쿠킹 처리기 연결(MeshAnimationAssets 13.1절 2): 빈 StaticMesh·SkeletalMesh·AnimationClip은 각 처리기가 거부한다
    // (메시 둘은 쓸 LOD가 없어 InvalidSource, 클립은 스켈레톤이 없어 MissingDependency). 기본 처리기였다면 모두 성공한다.
    ProcessResult rock = await RunCook("asset-new", "StaticMesh", "/Game/Rock", projectOption);
    ProcessResult body = await RunCook("asset-new", "SkeletalMesh", "/Game/Body", projectOption);
    ProcessResult walk = await RunCook("asset-new", "AnimationClip", "/Game/Walk", projectOption);
    ReportCook("asset-new mesh and clip types", rock.ExitCode == 0 && body.ExitCode == 0 && walk.ExitCode == 0, walk);
    ProcessResult wired = await RunCook("asset-cook", "--profile=Windows", projectOption);
    ReportCook("asset-cook mesh and clip processors", wired.ExitCode == 1 &&
        wired.StdErr.Contains("InvalidSource: /Game/Rock: error", StringComparison.Ordinal) &&
        wired.StdErr.Contains("InvalidSource: /Game/Body: error", StringComparison.Ordinal) &&
        wired.StdErr.Contains("MissingDependency: /Game/Walk: error", StringComparison.Ordinal), wired);
}
finally
{
    Directory.Delete(cookDirectory, true);
}

// asset-cook --engine(ShaderSystem 16.1절 SSD44–SSD46, AssetSystem 10.3.1절 A4f4): 제품 실행 파일이 ShaderCompiler 모듈을 시작해
// 셰이더 라이브러리 단계를 등록하고, 단계가 엔진 묶음(RenderBase 엔진 셰이더 타입)을 시작해 Windows 프로필의 형식마다 Engine 라이브러리를
// 엔진 쿠킹 루트에 게시한다. 두 번째 쿠킹은 같은 cookKey라 라이브러리를 재사용한다. 저장소 Engine/Saved를 건드리지 않게 실행 파일·워커와
// Engine/Config·Engine/Shaders를 임시 배치에 복사한다(엔진 너겟 root도 그 배치다). DXIL·DXBC를 만드는 호스트(Windows)에서만 돈다.
if (OperatingSystem.IsWindows())
{
    string engineCookDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-cook-engine-{Environment.ProcessId}");
    try
    {
        string engineCookExecutable = EngineCookLayout(executable, engineCookDirectory);
        string shaders = Path.Combine(engineCookDirectory, "Engine", "Saved", "Cooked", "Windows", "Shaders");

        async Task<ProcessResult> RunEngineCook(params string[] arguments) =>
            await ProcessRunner.RunAsync(new(engineCookExecutable, arguments, Path.GetDirectoryName(engineCookExecutable)!,
                Timeout: TimeSpan.FromSeconds(600)));

        void ReportEngineCook(string label, bool passed, ProcessResult result)
        {
            Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
            if (!passed)
            {
                ++failures;
                Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
            }
        }

        string[] Libraries() => Directory.Exists(shaders)
            ? [.. Directory.EnumerateFiles(shaders, "*.rshl").Select(Path.GetFileName).OfType<string>().Order(StringComparer.Ordinal)]
            : [];

        bool HasLibrary(string[] files, string format) => files.Any(file => file.StartsWith($"Engine-{format}-", StringComparison.Ordinal));

        ProcessResult first = await RunEngineCook("asset-cook", "--engine", "--profile=Windows");
        string[] published = Libraries();
        ReportEngineCook("asset-cook --engine publishes the engine shader libraries", first.ExitCode == 0 &&
            first.StdOut.StartsWith("asset-cook: Windows published targets 0 ", StringComparison.Ordinal) && published.Length == 3 &&
            HasLibrary(published, "D3D12_SM6") && HasLibrary(published, "D3D11_SM5") && HasLibrary(published, "VULKAN_SM5") &&
            File.Exists(Path.Combine(engineCookDirectory, "Engine", "Saved", "Cooked", "Windows", "CookedManifest.rman")), first);
        ProcessResult second = await RunEngineCook("asset-cook", "--engine", "--profile=Windows", "--json");
        int reused = second.StdOut.Split("\"reused\": true", StringSplitOptions.None).Length - 1;
        ReportEngineCook("asset-cook --engine reuses unchanged libraries", second.ExitCode == 0 && reused == 3 &&
            second.StdOut.Contains("\"kind\": \"ShaderLibrary\"", StringComparison.Ordinal) && Libraries().SequenceEqual(published), second);
    }
    finally
    {
        if (Directory.Exists(engineCookDirectory))
        {
            Directory.Delete(engineCookDirectory, true);
        }
    }
}

// asset-import-unreal(UnrealExchange.ko.md 10.5절): 제품 실행 파일에는 아직 종류별 임포터가 없다(UX2b부터). 저장소 fixture 묶음의
// dry run(텍스처 하나 계획), 실행(임포터 없음 — 건너뜀 경고, 종료 0), 없는 --only 값, 사용법·도움말. 결과 줄 전체와 시험 임포터
// 실행은 EngineTests가 명령 함수를 프로세스 안에서 부른다.
string unrealDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-import-unreal-{Environment.ProcessId}");
try
{
    string project = Path.Combine(unrealDirectory, "Unreal");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Unreal.rocksproject"), "{}\n");
    string projectOption = $"--project={project}";
    string bundle = Path.Combine(FindRepositoryRoot(executable).FullName, "Engine", "Plugins", "Importers", "UnrealExchange", "Tests",
        "Bundles", "Basic.rxbundle");
    const string textureOnly = "--only=/Game/Textures/T_Rock.T_Rock";

    async Task<ProcessResult> RunUnreal(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportUnreal(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult dryRun = await RunUnreal("asset-import-unreal", bundle, textureOnly, "--dry-run", projectOption);
    ReportUnreal("asset-import-unreal dry run", dryRun.ExitCode == 0 &&
        dryRun.StdOut.Contains("asset-import-unreal: plan-create /Game/Textures/T_Rock.T_Rock /Game/Unreal/Game/Textures/T_Rock\n",
            StringComparison.Ordinal) &&
        dryRun.StdOut.Contains("asset-import-unreal: 1 to create, 0 to reimport, 0 failed, 0 skipped", StringComparison.Ordinal), dryRun);
    ProcessResult noImporter = await RunUnreal("asset-import-unreal", bundle, textureOnly, projectOption);
    ReportUnreal("asset-import-unreal without a kind importer", noImporter.ExitCode == 0 &&
        noImporter.StdOut.Contains("asset-import-unreal: skipped /Game/Textures/T_Rock.T_Rock no-importer", StringComparison.Ordinal) &&
        noImporter.StdErr.Contains("warning NoKindImporter Texture2D", StringComparison.Ordinal), noImporter);
    ProcessResult notFound = await RunUnreal("asset-import-unreal", bundle, "--only=/Game/Nope.Nope", "--dry-run", projectOption);
    ReportUnreal("asset-import-unreal only not in the bundle", notFound.ExitCode == 1 &&
        notFound.StdErr.Contains("asset-import-unreal: NotFound: /Game/Nope.Nope: not in the bundle", StringComparison.Ordinal), notFound);
    string[][] unrealUsage =
    [
        ["asset-import-unreal", projectOption],
        ["asset-import-unreal", bundle, "--mesh-normals=flat", projectOption],
        ["asset-import-unreal", bundle, "--dry-run", "--dry-run", projectOption],
    ];
    foreach (string[] arguments in unrealUsage)
    {
        ProcessResult usage = await RunUnreal(arguments);
        ReportUnreal($"asset-import-unreal usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption && argument != bundle))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith("usage: asset-import-unreal", StringComparison.Ordinal), usage);
    }

    ProcessResult unrealHelp = await RunUnreal("asset-import-unreal", "--help");
    ReportUnreal("asset-import-unreal help", unrealHelp.ExitCode == 0 &&
        unrealHelp.StdOut.StartsWith("usage: asset-import-unreal", StringComparison.Ordinal), unrealHelp);
}
finally
{
    Directory.Delete(unrealDirectory, true);
}

// asset-orphans-*·verify 경고(AssetSystem 7.1절 17 — A2e4a): 고아 출력 fixture(Orphan 2311 — owner 2312 없음, HeroOrphan
// 2313 — 기록 없는 Hero가 owner)로 목록 줄(경로 키 순, owner 경로·missing), 경고만인 verify는 종료 0, 고아 지우기·고아 아닌 거부,
// 잠금 Busy, 사용법·도움말·프로젝트 없음.
string orphanDirectory = Path.Combine(Path.GetTempPath(), $"rocks-asset-orphans-{Environment.ProcessId}");
try
{
    void WriteOrphanContent(string root, string relative, byte[] bytes)
    {
        string path = Path.Combine(root, "Content", relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, bytes);
    }

    const string orphanId = "5e570000-a55e-7000-0000-000000000907";
    const string heroOrphanId = "5e570000-a55e-7000-0000-000000000909";
    const string missingOwnerId = "5e570000-a55e-7000-0000-000000000908";
    string project = Path.Combine(orphanDirectory, "Orphans");
    Directory.CreateDirectory(project);
    File.WriteAllText(Path.Combine(project, "Orphans.rocksproject"), "{}\n");
    WriteOrphanContent(project, "Characters/Hero.rasset", fixtures["CommandletHeroPackage"]);
    WriteOrphanContent(project, "Items/Base.rasset", fixtures["CommandletBasePackage"]);
    WriteOrphanContent(project, "Outputs/Orphan.rasset", fixtures["CommandletOrphanPackage"]);
    WriteOrphanContent(project, "Outputs/HeroOrphan.rasset", fixtures["CommandletHeroOrphanPackage"]);
    string projectOption = $"--project={project}";
    string warningsOnly = Path.Combine(orphanDirectory, "WarningsOnly");
    Directory.CreateDirectory(warningsOnly);
    File.WriteAllText(Path.Combine(warningsOnly, "WarningsOnly.rocksproject"), "{}\n");
    WriteOrphanContent(warningsOnly, "Outputs/Orphan.rasset", fixtures["CommandletOrphanPackage"]);

    async Task<ProcessResult> RunOrphans(params string[] arguments) =>
        await ProcessRunner.RunAsync(new(executable, arguments, workingDirectory, Timeout: TimeSpan.FromSeconds(30)));

    void ReportOrphans(string label, bool passed, ProcessResult result)
    {
        Console.WriteLine($"{label}: {(passed ? "PASS" : "FAIL")}");
        if (!passed)
        {
            ++failures;
            Console.Error.WriteLine($"  exit={result.ExitCode}; stdout={result.StdOut}; stderr={result.StdErr}");
        }
    }

    ProcessResult listed = await RunOrphans("asset-orphans-list", projectOption);
    ReportOrphans("asset-orphans-list", listed.ExitCode == 0 && listed.StdErr.Length == 0 &&
        listed.StdOut == $"/Game/Outputs/HeroOrphan {heroOrphanId} owner /Game/Characters/Hero\n" +
                         $"/Game/Outputs/Orphan {orphanId} owner missing\n", listed);

    // 경고만 있는 verify는 종료 0이다(진단 줄은 쓴다).
    ProcessResult warned = await RunOrphans("asset-index-verify", $"--project={warningsOnly}");
    ReportOrphans("asset-index-verify warnings only", warned.ExitCode == 0 &&
        warned.StdErr.Contains($"OrphanedOutput: Content/Outputs/Orphan.rasset: warning the owning import {missingOwnerId} is not in the index",
            StringComparison.Ordinal) &&
        warned.StdOut == "asset-index-verify: 1 packages, 1 diagnostics\n", warned);
    ProcessResult mixed = await RunOrphans("asset-index-verify", projectOption);
    ReportOrphans("asset-index-verify errors and warnings", mixed.ExitCode == 1 &&
        mixed.StdErr.Contains("OrphanedOutput: Content/Outputs/HeroOrphan.rasset: warning the owning import", StringComparison.Ordinal) &&
        mixed.StdErr.Contains("has no import record of its own", StringComparison.Ordinal) &&
        mixed.StdErr.Contains("MissingDependency", StringComparison.Ordinal), mixed);

    // 지우기: 고아가 아니면 거부, 고아는 지운다.
    ProcessResult notOrphan = await RunOrphans("asset-orphans-delete", "/Game/Characters/Hero", projectOption);
    ReportOrphans("asset-orphans-delete not an orphan", notOrphan.ExitCode == 1 &&
        notOrphan.StdErr.Contains("asset-orphans-delete: InvalidArgument: /Game/Characters/Hero: failed", StringComparison.Ordinal) &&
        File.Exists(Path.Combine(project, "Content", "Characters", "Hero.rasset")), notOrphan);
    ProcessResult deleted = await RunOrphans("asset-orphans-delete", "/Game/Outputs/Orphan", projectOption);
    ReportOrphans("asset-orphans-delete", deleted.ExitCode == 0 &&
        deleted.StdOut == $"asset-orphans-delete: deleted /Game/Outputs/Orphan {orphanId}\n" &&
        !File.Exists(Path.Combine(project, "Content", "Outputs", "Orphan.rasset")), deleted);
    ProcessResult after = await RunOrphans("asset-orphans-list", projectOption);
    ReportOrphans("asset-orphans-list after a delete", after.ExitCode == 0 &&
        after.StdOut == $"/Game/Outputs/HeroOrphan {heroOrphanId} owner /Game/Characters/Hero\n", after);
    // 같은 id owner가 둘이면 owner는 AssetId로 쓴다(Hero 사본).
    WriteOrphanContent(project, "Characters/HeroCopy.rasset", fixtures["CommandletHeroPackage"]);
    ProcessResult twin = await RunOrphans("asset-orphans-list", projectOption);
    ReportOrphans("asset-orphans-list with a duplicated owner id", twin.ExitCode == 0 &&
        twin.StdOut == $"/Game/Outputs/HeroOrphan {heroOrphanId} owner 5e570000-a55e-7000-0000-0000000008fc\n", twin);
    File.Delete(Path.Combine(project, "Content", "Characters", "HeroCopy.rasset"));

    // Content가 파일이면 검색 실패다.
    string brokenContent = Path.Combine(orphanDirectory, "BrokenContent");
    Directory.CreateDirectory(brokenContent);
    File.WriteAllText(Path.Combine(brokenContent, "BrokenContent.rocksproject"), "{}\n");
    File.WriteAllBytes(Path.Combine(brokenContent, "Content"), [1]);
    ProcessResult unscanned = await RunOrphans("asset-orphans-list", $"--project={brokenContent}");
    ReportOrphans("asset-orphans-list content is a file", unscanned.ExitCode == 1 &&
        unscanned.StdErr.Contains("asset-orphans-list: cannot scan", StringComparison.Ordinal), unscanned);

    ProcessResult missing = await RunOrphans("asset-orphans-delete", "/Game/Outputs/Orphan", projectOption);
    ReportOrphans("asset-orphans-delete missing", missing.ExitCode == 1 &&
        missing.StdErr.Contains("asset-orphans-delete: NotFound: /Game/Outputs/Orphan: failed", StringComparison.Ordinal), missing);

    if (OperatingSystem.IsWindows())
    {
        string lockPath = Path.Combine(project, "Intermediate", "AssetDatabase.lock");
        Directory.CreateDirectory(Path.GetDirectoryName(lockPath)!);
        using FileStream locked = new(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.ReadWrite);
        locked.Lock(0, 1);
        ProcessResult busy = await RunOrphans("asset-orphans-delete", "/Game/Outputs/HeroOrphan", projectOption);
        ReportOrphans("asset-orphans-delete locked", busy.ExitCode == 1 &&
            busy.StdErr.Contains("asset-orphans-delete: Busy: ", StringComparison.Ordinal), busy);
        // 목록은 잠금을 잡지 않는다.
        ProcessResult unlockedList = await RunOrphans("asset-orphans-list", projectOption);
        ReportOrphans("asset-orphans-list while locked", unlockedList.ExitCode == 0, unlockedList);
        locked.Unlock(0, 1);
    }

    string[][] orphanUsage =
    [
        ["asset-orphans-delete", projectOption],
        ["asset-orphans-delete", "/Game/A", "/Game/B", projectOption],
        ["asset-orphans-delete", "/Game/A", "--force", projectOption],
        ["asset-orphans-list", "extra", projectOption],
        ["asset-orphans-list", "--under=/Game", projectOption],
    ];
    foreach (string[] arguments in orphanUsage)
    {
        ProcessResult usage = await RunOrphans(arguments);
        ReportOrphans($"{arguments[0]} usage: {string.Join(' ', arguments.Skip(1).Where(argument => argument != projectOption))}",
            usage.ExitCode == 2 && usage.StdErr.StartsWith($"usage: {arguments[0]}", StringComparison.Ordinal), usage);
    }

    foreach (string name in new[] { "asset-orphans-list", "asset-orphans-delete" })
    {
        ProcessResult help = await RunOrphans(name, "--help");
        ReportOrphans($"{name} help", help.ExitCode == 0 && help.StdOut.StartsWith($"usage: {name}", StringComparison.Ordinal), help);
        string[] noProjectArguments = name == "asset-orphans-delete" ? [name, "/Game/A"] : [name];
        ProcessResult noProject = await RunOrphans(noProjectArguments);
        ReportOrphans($"{name} without a project", noProject.ExitCode == 1 &&
            noProject.StdErr.Contains("no project found", StringComparison.Ordinal), noProject);
    }
}
finally
{
    Directory.Delete(orphanDirectory, true);
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

// 실행 파일을 root/bin에 복사하고, 실행 파일 위쪽의 엔진 Config(저장소 Engine/Config)를 root/Engine/Config로 복사한 뒤 Windows
// 프로필을 셰이더 없는 프로필로 바꾼다. 복사한 실행 파일 경로를 돌려준다.
static string ShaderlessEngineLayout(string executable, string root)
{
    string? directory = Path.GetDirectoryName(Path.GetFullPath(executable));
    string? engineConfig = null;
    while (directory is not null)
    {
        string candidate = Path.Combine(directory, "Engine", "Config");
        if (Directory.Exists(candidate))
        {
            engineConfig = candidate;
            break;
        }

        directory = Path.GetDirectoryName(directory);
    }

    if (engineConfig is null)
    {
        throw new InvalidOperationException("Engine/Config not found above the executable");
    }

    string targetConfig = Path.Combine(root, "Engine", "Config");
    foreach (string file in Directory.EnumerateFiles(engineConfig, "*", SearchOption.AllDirectories))
    {
        string destination = Path.Combine(targetConfig, Path.GetRelativePath(engineConfig, file));
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        File.Copy(file, destination, true);
    }

    string profile = Path.Combine(targetConfig, "TargetProfiles", "Windows.json");
    var json = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(profile))!.AsObject();
    json["graphicsBackends"] = new System.Text.Json.Nodes.JsonArray();
    json["shaderFormats"] = new System.Text.Json.Nodes.JsonObject();
    File.WriteAllText(profile, json.ToJsonString());
    string bin = Path.Combine(root, "bin");
    Directory.CreateDirectory(bin);
    string copied = Path.Combine(bin, Path.GetFileName(executable));
    File.Copy(executable, copied, true);
    return copied;
}

// 항목이 없는 쿠킹 매니페스트 byte다(AssetSystem 10.3절 4항 형식 v2): magic, 형식 버전, 항목 수, 프로파일 id(LE64 길이 +
// byte), 프로파일·쿠킹 설정 digest, 쿠커 버전, 쿠킹한 엔진 버전 셋, backend 수(0 — 셰이더 없는 프로필), artifact 수(0),
// 앞 모든 byte의 CRC32.
static byte[] EmptyCookedManifest(string profile)
{
    using MemoryStream stream = new();
    using (BinaryWriter writer = new(stream, System.Text.Encoding.UTF8, true))
    {
        writer.Write("RMAN"u8);
        writer.Write(2u);
        writer.Write(0u);
        byte[] profileBytes = System.Text.Encoding.UTF8.GetBytes(profile);
        writer.Write((ulong)profileBytes.Length);
        writer.Write(profileBytes);
        writer.Write(new byte[64]);
        writer.Write(1u);
        writer.Write(0u);
        writer.Write(1u);
        writer.Write(0u);
        writer.Write(0u);
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
    DirectoryInfo root = FindRepositoryRoot(executable);
    string header = File.ReadAllText(Path.Combine(root.FullName, "Engine", "source", "Developer", "AssetDatabase", "Tests",
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

// 실행 파일 위쪽에서 Rocks.proj를 가진 첫 폴더(저장소 루트)다.
static DirectoryInfo FindRepositoryRoot(string executable)
{
    DirectoryInfo? root = new FileInfo(executable).Directory;
    while (root is not null && !File.Exists(Path.Combine(root.FullName, "Rocks.proj")))
    {
        root = root.Parent;
    }

    return root ?? throw new InvalidOperationException("The repository root (Rocks.proj) is not above the executable.");
}

// 엔진 전용 쿠킹 배치다: 저장소 Engine/Config·Engine/Shaders를 root/Engine 아래로, 실행 파일과 그 옆 ShaderCompileWorker를 root/bin으로
// 복사해 복사한 실행 파일 경로를 돌려준다(FindEngineDirectory가 실행 파일 위쪽의 Engine/Config로 root/Engine을 찾는다).
static string EngineCookLayout(string executable, string root)
{
    DirectoryInfo repository = FindRepositoryRoot(executable);
    foreach (string folder in new[] { "Config", "Shaders" })
    {
        string source = Path.Combine(repository.FullName, "Engine", folder);
        foreach (string file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            string destination = Path.Combine(root, "Engine", folder, Path.GetRelativePath(source, file));
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(file, destination, true);
        }
    }

    string bin = Path.Combine(root, "bin");
    Directory.CreateDirectory(bin);
    string directory = Path.GetDirectoryName(Path.GetFullPath(executable))!;
    foreach (string name in new[] { "ShaderCompileWorker.exe", "ShaderCompileWorker" })
    {
        string worker = Path.Combine(directory, name);
        if (File.Exists(worker))
        {
            File.Copy(worker, Path.Combine(bin, name), true);
        }
    }

    string copied = Path.Combine(bin, Path.GetFileName(executable));
    File.Copy(executable, copied, true);
    return copied;
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
