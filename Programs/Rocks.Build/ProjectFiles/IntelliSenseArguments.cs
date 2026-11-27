namespace Rocks.Build;

/// <summary>논리 compile 명령 인자 하나의 IntelliSense 분류다(Docs/Design/ProjectFiles.ko.md 8.2절).</summary>
public enum IntelliSenseArgumentKind
{
    /// <summary>include 디렉터리(<c>/I</c>). Value는 경로다.</summary>
    Include,

    /// <summary>정의(<c>/D</c>). Value는 <c>이름</c> 또는 <c>이름=값</c>이다.</summary>
    Define,

    /// <summary>강제 include(<c>/FI</c>). Value는 경로다.</summary>
    ForcedInclude,

    /// <summary>IntelliSense 옵션으로 옮긴다. Value는 Visual Studio 철자다.</summary>
    Option,

    /// <summary>번역 단위 언어(<c>/TP</c>·<c>/TC</c>). Value는 원래 철자다. 확장자와 다를 때만 옵션으로 남긴다.</summary>
    Language,

    /// <summary>컴파일할 소스 자신이다.</summary>
    Source,

    /// <summary>IntelliSense에 의미가 없어 버린다. Value는 원래 인자다.</summary>
    Dropped
}

/// <summary>분류 결과다.</summary>
public readonly record struct IntelliSenseArgument(IntelliSenseArgumentKind Kind, string Value);

/// <summary>
/// 논리 compile 명령(clang-cl 철자)의 인자를 Visual Studio IntelliSense 속성으로 분류한다(Docs/Design/ProjectFiles.ko.md 4·8.2절, PJD7).
/// 표에 없는 인자는 <see cref="ArgumentException"/>이다. 빌드에 새 인자가 생기면 IntelliSense에 의미가 있는지 정해 이 표에 넣는다 —
/// 조용히 버리면 빌드와 IntelliSense가 모르게 갈라진다. IntelliSense는 Windows 계획만 쓰므로(6.3절) clang-cl 철자만 받는다.
/// </summary>
public static class IntelliSenseArguments
{
    // 같은 철자로 IntelliSense 옵션에 넘긴다. 미리 정의 매크로(_DEBUG·_DLL·_CPPRTTI·__cpp_exceptions·__cplusplus·__SANITIZE_ADDRESS__)나
    // 원본 문자 집합을 바꾼다.
    private static readonly HashSet<string> PassThrough = new(StringComparer.Ordinal)
    {
        "/MDd", "/MD", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/utf-8", "/fsanitize=address"
    };

    // 철자를 바꿔 넘긴다. IntelliSense는 /clang: 접두를 모른다. clang-cl 17 이상의 /std:c++latest는 C++26이라 C++23은 미리 보기 철자다
    // (V11에서 받지 않으면 /std:c++latest로 바꾼다, 8.5절).
    private static readonly Dictionary<string, string> Translated = new(StringComparer.Ordinal)
    {
        ["/clang:-std=c++23"] = "/std:c++23preview",
        ["/clang:-std=c17"] = "/std:c17"
    };

    // 버린다: 진단·출력 형식, 코드 생성, clang 설정 파일 탐색. 최적화 인자가 만드는 미리 정의 매크로는 정책 ide-invisible-macro가 제품
    // 소스에서 막는다.
    private static readonly HashSet<string> DroppedExact = new(StringComparer.Ordinal)
    {
        "--no-default-config", "/nologo", "/c", "/W4", "/WX", "-Wunique-object-duplication", "-Wthread-safety", "/Z7", "/Od", "/O2",
        "/clang:-ffp-contract=off", "-fno-strict-aliasing", "-fno-strict-overflow", "-flto=thin", "-fprofile-instr-generate", "-fcoverage-mapping"
    };

    // 버리는 접두 철자다: __FILE__ 철자만 바꾸는 경로 매핑, object 출력.
    private static readonly string[] DroppedPrefixes = ["/clang:-fmacro-prefix-map=", "/Fo"];

    /// <summary>
    /// <paramref name="argument"/>를 분류한다. <paramref name="source"/>는 그 명령이 컴파일하는 소스(같은 철자면 <see cref="IntelliSenseArgumentKind.Source"/>)다.
    /// </summary>
    /// <exception cref="ArgumentException">표에 없는 인자.</exception>
    public static IntelliSenseArgument Classify(string argument, string source)
    {
        ArgumentNullException.ThrowIfNull(argument);
        if (string.Equals(argument, source, StringComparison.Ordinal))
        {
            return new(IntelliSenseArgumentKind.Source, argument);
        }

        if (argument.StartsWith("/FI", StringComparison.Ordinal) && argument.Length > 3)
        {
            return new(IntelliSenseArgumentKind.ForcedInclude, argument[3..]);
        }

        if (argument.StartsWith("/I", StringComparison.Ordinal) && argument.Length > 2)
        {
            return new(IntelliSenseArgumentKind.Include, argument[2..]);
        }

        if (argument.StartsWith("/D", StringComparison.Ordinal) && argument.Length > 2)
        {
            return new(IntelliSenseArgumentKind.Define, argument[2..]);
        }

        if (argument is "/TP" or "/TC")
        {
            return new(IntelliSenseArgumentKind.Language, argument);
        }

        if (PassThrough.Contains(argument))
        {
            return new(IntelliSenseArgumentKind.Option, argument);
        }

        if (Translated.TryGetValue(argument, out string? translated))
        {
            return new(IntelliSenseArgumentKind.Option, translated);
        }

        if (DroppedExact.Contains(argument) || DroppedPrefixes.Any(prefix => argument.StartsWith(prefix, StringComparison.Ordinal)))
        {
            return new(IntelliSenseArgumentKind.Dropped, argument);
        }

        throw new ArgumentException($"Unclassified compile argument for IntelliSense: '{argument}' (source {source}). " +
            "Add it to IntelliSenseArguments (Docs/Design/ProjectFiles.ko.md section 8.2).");
    }

    /// <summary>
    /// 분류표의 옮김 항목을 원래 철자로 되돌린다(왕복 검사용). 같은 철자로 넘기는 옵션은 그대로다.
    /// </summary>
    public static string OriginalOption(string option) =>
        Translated.FirstOrDefault(pair => pair.Value == option).Key ?? option;
}
