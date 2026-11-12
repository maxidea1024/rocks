using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

/// <summary>
/// 결정적 수학 모듈의 기계어 오브젝트 검사다(Docs/Design/CoreDataFoundations.ko.md 2.3). 소스 정책
/// (<see cref="PolicyChecks.DeterministicMathSources"/>)으로는 보이지 않는 것을 이번 구성·toolchain의 실제 오브젝트에서
/// 확인한다.
/// <list type="number">
/// <item>외부 심볼 허용 목록: CRT 수학 함수(sin·exp·pow·floor…)나 다른 모듈 함수를 부르지 않고 코드 생성 지원
/// 심볼(stack protector·stack probe·<c>_fltused</c>·mem*·bzero, Asan이면 sanitizer 계측과 그 등록용
/// <c>__cxa_atexit</c>·<c>__dso_handle</c>)만 참조한다. 컴파일러가 연산을 libcall로 바꾼 경우도 잡힌다.</item>
/// <item>금지 명령: 융합 곱셈-덧셈(fmadd·vfmadd·fmla …, 반올림이 한 번 줄어 호스트마다 결과가 달라진다), CPU마다 결과가
/// 다른 근사 명령(x86 rcp·rsqrt, AArch64 frecpe·frsqrte·frecps·frsqrts·frecpx), x86의 x87 명령(확장 정밀도).</item>
/// </list>
/// Release의 ThinLTO 오브젝트(LLVM bitcode)는 compiler 옆 clang으로 -O2 기계어를 만들어 같은 검사를 한다(backend가
/// libcall·명령을 고르기 전이라 bitcode 자체로는 볼 수 없다).
/// </summary>
public static class DeterministicMathObjectCheck
{
    /// <summary>검사 대상 번역 단위가 있는 저장소 상대 폴더다.</summary>
    public const string SourceDirectory = "Core/Source/Runtime/DeterministicMath/Private";

    // 코드 생성 지원 심볼이다. 수학 결과에 관여하지 않는다. Mach-O 이름 앞의 '_' 하나는 IsAllowedSymbol이 벗긴다.
    private static readonly HashSet<string> AllowedSymbols = new(StringComparer.Ordinal)
    {
        "_fltused",                                          // MSVC ABI: 부동소수점 사용 표시
        "__security_check_cookie", "__security_cookie",      // /GS stack cookie
        "__GSHandlerCheck",
        "__chkstk", "__chkstk_darwin",                       // 큰 stack frame probe
        "__stack_chk_fail", "__stack_chk_guard",             // stack protector
        "memcpy", "memmove", "memset", "bzero", "__bzero",   // 컴파일러가 만드는 byte 복사·채우기(Darwin은 bzero)
    };

    // sanitizer 계측(Asan 구성)이다. Darwin은 module dtor를 __cxa_atexit로 등록한다.
    private static readonly string[] SanitizerPrefixes = ["__asan_", "__sanitizer_", "__ubsan_"];
    private static readonly HashSet<string> SanitizerSymbols = new(StringComparer.Ordinal) { "__cxa_atexit", "__dso_handle" };

    private static readonly RegexOptions Options = RegexOptions.CultureInvariant | RegexOptions.IgnoreCase | RegexOptions.Compiled;

    // 융합 곱셈-덧셈: x86 FMA3/FMA4(vfmadd132sd·vfnmsub231ps·vfmaddsubps…), AArch64 스칼라(fmadd·fmsub·fnmadd·fnmsub)·
    // 벡터(fmla·fmls, Apple 문법 fmla.2d)·SVE(fmad·fmsb·fnmad·fnmsb·fnmla·fnmls).
    private static readonly Regex FusedMnemonic = new(
        "^(?:v?fn?m(?:add|sub)[a-z0-9]*|fn?ml[as](?:\\.[a-z0-9]+)?|fn?mad|fn?msb)$", Options, TimeSpan.FromSeconds(1));

    // AArch64 근사·보조 명령(구현마다 결과가 다를 수 있다).
    private static readonly Regex ArmApproximation = new("^f(?:recpe|rsqrte|recps|rsqrts|recpx)(?:\\.[a-z0-9]+)?$", Options,
        TimeSpan.FromSeconds(1));

    // x86 근사 명령(SSE rcp·rsqrt, AVX-512 rcp14·rsqrt14·rcp28·rsqrt28)과 x87 명령(f로 시작하는 mnemonic, %st 피연산자).
    private static readonly Regex X86Forbidden = new(
        "^(?:v?r(?:cp|sqrt)(?:ss|ps|sd|pd|14[a-z]*|28[a-z]*)|f[a-z0-9]+)$|%st", Options, TimeSpan.FromSeconds(1));

    /// <summary>검사 결과다. <see cref="Verified"/>는 확인한 오브젝트 수(bitcode는 기계어로 만든 뒤),
    /// <see cref="Bitcode"/>는 그중 LLVM bitcode였던 수, <see cref="Failures"/>는 위반 설명이다.</summary>
    public sealed record Result(int Verified, int Bitcode, IReadOnlyList<string> Failures);

    /// <summary>
    /// compile DB(<paramref name="compileCommands"/>)에서 <see cref="SourceDirectory"/>의 번역 단위 오브젝트를 찾아 두 검사를
    /// 한다. 도구는 그 항목 compiler 옆의 <c>llvm-nm</c>·<c>llvm-objdump</c>(·bitcode면 <c>clang</c>)를 쓰고, 없으면 PATH,
    /// macOS에서는 Xcode의 <c>nm</c>·<c>objdump</c>(LLVM 기반)를 쓴다. <paramref name="sanitizer"/>는 Asan 구성이다.
    /// 도구가 없거나 실패하면 예외다.
    /// </summary>
    public static async Task<Result> VerifyAsync(string root, string compileCommands,
        IReadOnlyDictionary<string, string?> environment, bool sanitizer = false, CancellationToken cancellationToken = default)
    {
        string source = Path.GetFullPath(Path.Combine(root, SourceDirectory)) + Path.DirectorySeparatorChar;
        var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(compileCommands, new UTF8Encoding(false, true)));
        var entries = new List<(string Output, string Compiler, string[] Arguments)>();
        foreach (JsonElement entry in document.RootElement.EnumerateArray())
        {
            string directory = entry.TryGetProperty("directory", out JsonElement value) ? value.GetString()! : root;
            if (!Path.GetFullPath(entry.GetProperty("file").GetString()!, Path.GetFullPath(directory)).StartsWith(source, comparison))
                continue;
            string[] arguments = entry.GetProperty("arguments").EnumerateArray().Select(item => item.GetString()!).ToArray();
            entries.Add((Path.GetFullPath(entry.GetProperty("output").GetString()!, Path.GetFullPath(directory)),
                NativeEnvironment.FindExecutable(arguments[0], environment), arguments));
        }

        List<string> failures = [];
        int verified = 0;
        int bitcode = 0;
        string temporary = Path.Combine(Path.GetTempPath(), "rocks-objects-" + Guid.NewGuid().ToString("N"));
        try
        {
            foreach (var (objectPath, compiler, arguments) in entries)
            {
                string name = Path.GetFileName(objectPath);
                byte[] header = new byte[4];
                int read;
                using (FileStream stream = File.OpenRead(objectPath))
                    read = stream.ReadAtLeast(header, header.Length, throwOnEndOfStream: false);
                if (read < header.Length)
                {
                    failures.Add($"{name}: object file is shorter than an object header ({objectPath})");
                    continue;
                }

                string machine = objectPath;
                if (IsBitcode(header))
                {
                    ++bitcode;
                    Directory.CreateDirectory(temporary);
                    machine = Path.Combine(temporary, verified + ".o");
                    List<string> codegen = ["-O2", "-ffp-contract=off", "-c", "-x", "ir", objectPath, "-o", machine];
                    string? target = arguments.FirstOrDefault(argument => argument.StartsWith("--target=", StringComparison.Ordinal));
                    if (target is not null)
                        codegen.Insert(0, target);
                    await RunToolAsync(FindTool(compiler, "clang", null, environment), codegen.ToArray(), root, environment,
                        cancellationToken);
                }

                var symbols = await RunToolAsync(FindTool(compiler, "llvm-nm", "/usr/bin/nm", environment), ["-u", "-j", machine], root,
                    environment, cancellationToken);
                foreach (string symbol in symbols.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
                    if (!IsAllowedSymbol(symbol, sanitizer))
                        failures.Add($"{name}: external symbol '{symbol}' is not in the deterministic math allow-list");
                var disassembly = await RunToolAsync(FindTool(compiler, "llvm-objdump", "/usr/bin/objdump", environment),
                    ["-d", "--no-show-raw-insn", machine], root, environment, cancellationToken);
                foreach (string instruction in ForbiddenInstructions(disassembly))
                    failures.Add($"{name}: forbidden instruction '{instruction}'");
                ++verified;
            }
        }
        finally
        {
            if (Directory.Exists(temporary))
                Directory.Delete(temporary, recursive: true);
        }
        return new Result(verified, bitcode, failures);
    }

    /// <summary>외부 심볼이 허용 목록에 있는지 본다. Mach-O가 C 이름 앞에 붙이는 '_' 하나를 벗긴 이름도 본다.
    /// <paramref name="sanitizer"/>면 sanitizer 계측 심볼도 허용한다.</summary>
    public static bool IsAllowedSymbol(string symbol, bool sanitizer = false)
    {
        bool Allowed(string name) => AllowedSymbols.Contains(name) ||
            (sanitizer && (SanitizerSymbols.Contains(name) || SanitizerPrefixes.Any(prefix => name.StartsWith(prefix, StringComparison.Ordinal))));
        return Allowed(symbol) || (symbol.StartsWith('_') && Allowed(symbol[1..]));
    }

    /// <summary>
    /// disassembly 출력에서 금지 명령 줄을 찾는다. 아키텍처는 출력의 "file format" 줄로 정한다(x86-64 또는 aarch64/arm64).
    /// 명령 이름은 공백·쉼표로 나눈 토큰으로 비교한다(기호 이름 &lt;…&gt;은 토큰이 되지 않는다).
    /// </summary>
    public static IReadOnlyList<string> ForbiddenInstructions(string disassembly)
    {
        bool x86 = false;
        bool arm = false;
        var found = new List<string>();
        foreach (string raw in disassembly.Split('\n'))
        {
            string line = raw.Trim();
            int format = line.IndexOf("file format", StringComparison.OrdinalIgnoreCase);
            if (format >= 0)
            {
                string kind = line[format..].ToLowerInvariant();
                x86 = kind.Contains("x86-64") || kind.Contains("x86_64") || kind.Contains("i386");
                arm = kind.Contains("aarch64") || kind.Contains("arm64");
                continue;
            }
            // 명령 줄은 "주소: mnemonic 피연산자" 형태다.
            int colon = line.IndexOf(':');
            if (colon <= 0 || !line[..colon].All(Uri.IsHexDigit))
                continue;
            string[] tokens = line[(colon + 1)..].Split([' ', '\t', ','], StringSplitOptions.RemoveEmptyEntries);
            bool forbidden = tokens.Any(token => FusedMnemonic.IsMatch(token) || (arm && ArmApproximation.IsMatch(token)) ||
                                                 (x86 && X86Forbidden.IsMatch(token)));
            if (forbidden)
                found.Add(line);
        }
        return found;
    }

    /// <summary>LLVM bitcode(raw 'BC' 0xC0DE 또는 wrapper 0x0B17C0DE) 오브젝트인지 앞 4 byte로 본다.</summary>
    public static bool IsBitcode(ReadOnlySpan<byte> header) =>
        header.Length >= 4 && ((header[0] == 0x42 && header[1] == 0x43 && header[2] == 0xC0 && header[3] == 0xDE) ||
                               (header[0] == 0xDE && header[1] == 0xC0 && header[2] == 0x17 && header[3] == 0x0B));

    private static string FindTool(string compiler, string llvmName, string? macFallback, IReadOnlyDictionary<string, string?> environment)
    {
        string executable = llvmName + (OperatingSystem.IsWindows() ? ".exe" : "");
        string beside = Path.Combine(Path.GetDirectoryName(compiler)!, executable);
        if (File.Exists(beside))
            return beside;
        try
        {
            return NativeEnvironment.FindExecutable(llvmName, environment);
        }
        catch (Exception error) when (error is FileNotFoundException or InvalidOperationException)
        {
            if (OperatingSystem.IsMacOS() && macFallback is not null && File.Exists(macFallback))
                return macFallback;
            throw new FileNotFoundException($"{llvmName} not found beside the compiler or on PATH; install it with the toolchain.", beside);
        }
    }

    private static async Task<string> RunToolAsync(string tool, string[] arguments, string root,
        IReadOnlyDictionary<string, string?> environment, CancellationToken cancellationToken)
    {
        var result = await ProcessRunner.RunAsync(new(tool, arguments, root, environment, Timeout: TimeSpan.FromMinutes(2)),
            cancellationToken);
        if (result.ExitCode != 0)
            throw new InvalidOperationException($"{Path.GetFileName(tool)} failed with exit {result.ExitCode}: {result.StdErr.Trim()}");
        return result.StdOut.Replace("\r", "", StringComparison.Ordinal);
    }
}
