using System.Security.Cryptography;
using System.Text;

namespace Rocks.Build;

/// <summary>
/// 툴체인 지문이다(Docs/Design/ReflectionFoundations.ko.md 8·8.1절). 같은 지문이면 같은 컴파일러·링커 바이너리, 같은 대상,
/// 같은 FP 모델로 빌드된 것이다. 지문은 에셋 너겟 키에만 들어가고 쿠킹 byte에는 들어가지 않는다. 빌드 구성(Debug·Release)과
/// 호스트 경로·환경은 넣지 않으므로 같은 툴체인의 모든 구성이 같은 지문을 갖는다.
/// <para>
/// 사용 예:
/// <code>
/// NativeToolchain toolchain = await NativeToolchains.DiscoverAsync();
/// string text = ToolchainFingerprint.CanonicalText(toolchain);   // 사람이 읽는 원문(LF)
/// byte[] digest = ToolchainFingerprint.Compute(toolchain);        // SHA-256(UTF-8(text)), 32 byte
/// </code>
/// 둘 다 프로세스를 실행하지 않는 순수 함수다. 신원 필드(CompilerVersion·TargetTriple·CompilerDigest·LinkerDigest)가 비어 있으면
/// (수동으로 만든 툴체인) ArgumentException, 구성·최적화·언어 조합 사이에 FP 모델 인자가 다르면 InvalidOperationException이다.
/// </para>
/// </summary>
public static class ToolchainFingerprint
{
    /// <summary>
    /// 정규 텍스트다(LF, 마지막 줄도 LF, 줄 끝 공백 없음, 줄 순서 고정):
    /// <code>
    /// rocks-toolchain-fingerprint 1
    /// compiler: clang version 21.1.6
    /// compiler-sha256: &lt;64 hex&gt;
    /// linker-sha256: &lt;64 hex&gt;
    /// target: x86_64-pc-windows-msvc
    /// fp: /clang:-ffp-contract=off
    /// </code>
    /// target은 MSVC 환경 버전 접미를 지운 TargetTriple, fp는 NativeCompileProfile.Arguments의 FP 모델 인자(원래 순서, 공백으로
    /// 이음)다.
    /// </summary>
    public static string CanonicalText(NativeToolchain toolchain) =>
        CanonicalText(toolchain, (configuration, optimization, language) =>
            NativeCompileProfile.Arguments(toolchain, configuration, optimization, language));

    /// <summary>SHA-256(UTF-8(<see cref="CanonicalText(NativeToolchain)"/>))다(32 byte).</summary>
    public static byte[] Compute(NativeToolchain toolchain) =>
        SHA256.HashData(Encoding.UTF8.GetBytes(CanonicalText(toolchain)));

    // 프로필 공급자를 받는 내부 형태다. 시험이 조합마다 다른 프로필로 InvalidOperationException을 확인한다.
    internal static string CanonicalText(NativeToolchain toolchain,
        Func<NativeConfiguration, CodeOptimization, NativeLanguage, IReadOnlyList<string>> profile)
    {
        ArgumentNullException.ThrowIfNull(toolchain);
        RequireIdentity(toolchain);
        string[]? model = null;
        string? first = null;
        foreach (NativeConfiguration configuration in Enum.GetValues<NativeConfiguration>())
        {
            foreach (CodeOptimization optimization in Enum.GetValues<CodeOptimization>())
            {
                foreach (NativeLanguage language in Enum.GetValues<NativeLanguage>())
                {
                    string[] current = profile(configuration, optimization, language).Where(FloatingPointArguments.IsModelArgument).ToArray();
                    string name = $"{configuration}/{optimization}/{language}";
                    if (model is null)
                    {
                        model = current;
                        first = name;
                    }
                    else if (!model.SequenceEqual(current, StringComparer.Ordinal))
                    {
                        // 결정 7: FP 모델은 구성·최적화·언어에 따라 달라지지 않는다(지문에 구성을 넣지 않는 전제).
                        throw new InvalidOperationException($"Toolchain fingerprint: floating-point model arguments differ between {first} " +
                            $"({string.Join(' ', model)}) and {name} ({string.Join(' ', current)}).");
                    }
                }
            }
        }

        var text = new StringBuilder();
        text.Append("rocks-toolchain-fingerprint 1\n");
        text.Append("compiler: ").Append(toolchain.CompilerVersion).Append('\n');
        text.Append("compiler-sha256: ").Append(toolchain.CompilerDigest).Append('\n');
        text.Append("linker-sha256: ").Append(toolchain.LinkerDigest).Append('\n');
        text.Append("target: ").Append(PortableTarget(toolchain.TargetTriple)).Append('\n');
        text.Append("fp:").Append(model!.Length == 0 ? "" : " " + string.Join(' ', model)).Append('\n');
        return text.ToString();
    }

    // 신원 필드가 discovery 형태인지 확인한다. 빈 지문을 조용히 만들지 않는다.
    internal static void RequireIdentity(NativeToolchain toolchain)
    {
        var missing = new List<string>();
        if (string.IsNullOrWhiteSpace(toolchain.CompilerVersion))
        {
            missing.Add(nameof(NativeToolchain.CompilerVersion));
        }

        if (string.IsNullOrWhiteSpace(toolchain.TargetTriple))
        {
            missing.Add(nameof(NativeToolchain.TargetTriple));
        }

        if (!IsDigest(toolchain.CompilerDigest))
        {
            missing.Add(nameof(NativeToolchain.CompilerDigest));
        }

        if (!IsDigest(toolchain.LinkerDigest))
        {
            missing.Add(nameof(NativeToolchain.LinkerDigest));
        }

        if (missing.Count != 0)
        {
            throw new ArgumentException("Toolchain identity is incomplete for BuildInfo (use NativeToolchains discovery or fill " +
                string.Join(", ", missing) + "; digests are 64 lowercase hex characters).");
        }

        if (toolchain.CompilerVersion.IndexOfAny(['\r', '\n']) >= 0 || toolchain.TargetTriple.IndexOfAny(['\r', '\n']) >= 0)
        {
            throw new ArgumentException("Toolchain identity must be single-line text (CompilerVersion, TargetTriple).");
        }
    }

    /// <summary>
    /// 지문용 대상이다. Windows MSVC 환경 버전 접미(<c>msvc19.29.30159</c>의 <c>19.29.30159</c> — 설치된 VC 도구 버전, 호스트 쪽)를
    /// 지운다. macOS 배포 대상·Android API 수준은 compile 인자가 정한 대상이므로 남긴다.
    /// </summary>
    internal static string PortableTarget(string triple)
    {
        int separator = triple.LastIndexOf('-');
        string environment = triple[(separator + 1)..];
        if (environment.Length > "msvc".Length && environment.StartsWith("msvc", StringComparison.Ordinal) &&
            environment["msvc".Length..].All(value => char.IsAsciiDigit(value) || value == '.'))
        {
            return triple[..(separator + 1)] + "msvc";
        }

        return triple;
    }

    private static bool IsDigest(string value) =>
        value.Length == 64 && value.All(character => char.IsAsciiDigit(character) || character is >= 'a' and <= 'f');
}
