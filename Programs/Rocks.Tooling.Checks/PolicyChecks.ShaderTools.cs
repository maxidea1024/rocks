using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

// 셰이더 도구 참조 정책이다(Docs/Design/ShaderSystem.ko.md 9절 정책 검사). 도구 표 참조와 binary 로드를 따로 본다.
public static partial class PolicyChecks
{
    // 도구 표 모듈(Developer/ShaderToolTable)을 참조해도 되는 곳이다: 표 모듈 자신, 메인 프로세스 컴파일 관리자, 워커와 그 안의
    // 형식 모듈, 워커 자리 모듈(ShaderToolLoader), 시험 runner(EngineTests가 표·로더 시험을 링크한다), SH0 도구 스파이크
    // (Programs/ShaderToolSpike, 워커가 생기기 전에 도구를 직접 여는 측정 프로그램).
    private static readonly string[] ShaderToolTableUsers =
    [
        "Engine/Source/Developer/ShaderToolTable/", "Engine/Source/Developer/ShaderCompiler/", "Engine/Source/Developer/ShaderToolLoader/",
        "Programs/ShaderCompileWorker/", "Programs/FoundationTests/", "Programs/ShaderToolSpike/"
    ];

    // 도구 binary를 열어도 되는 곳이다: 워커와 그 안의 형식 모듈, 워커 자리 모듈, 시험 runner, SH0 도구 스파이크. 메인
    // 프로세스(ShaderCompiler)는 도구 표만 쓰고 binary를 열지 않는다.
    private static readonly string[] ShaderToolLoadUsers =
    [
        "Engine/Source/Developer/ShaderToolLoader/", "Programs/ShaderCompileWorker/", "Programs/FoundationTests/", "Programs/ShaderToolSpike/"
    ];

    // DynamicLibrary::Open(절대 경로 열기)을 불러도 되는 곳이다: 도구 로드 허용 위치와 API 구현·시험(PlatformBase·TestSupport), 모듈
    // 로더의 Modular manifest 적재(Docs/Design/ModuleLoader.ko.md 3.13절 — 엔진 모듈 공유 라이브러리를 manifest 폴더의 절대 경로로 연다).
    // 그 밖(Runtime 모듈)은 OS 공급 라이브러리를 이름으로 여는 OpenSystem만 쓴다(Vulkan 로더·XInput).
    private static readonly string[] DynamicLibraryOpenUsers =
    [
        .. ShaderToolLoadUsers, "Core/Source/Runtime/PlatformBase/", "Core/Source/Developer/TestSupport/",
        "Core/Source/Runtime/ModuleLoader/Private/ModuleManifest.cpp"
    ];

    private const string ShaderFormatPrefix = "Engine/Source/Developer/ShaderFormat";

    private static readonly Regex ShaderToolTableInclude = Pattern("#\\s*include\\s*[<\"]ShaderToolTable/");

    private static readonly Regex ShaderToolLoaderInclude = Pattern("#\\s*include\\s*[<\"]ShaderToolLoader/");

    private static readonly Regex DynamicLibraryUse = Pattern("\\bDynamicLibrary\\b");

    // OpenSystem이 아닌 Open 호출이다(DynamicLibrary::Open(absolutePath)). DynamicLibrary를 쓰는 파일에서만 본다.
    private static readonly Regex AbsoluteOpenCall = Pattern("\\bOpen\\s*\\(");

    private static bool UnderAny(string relative, IEnumerable<string> prefixes) =>
        prefixes.Any(prefix => relative.StartsWith(prefix, StringComparison.Ordinal));

    private static bool IsShaderFormatModule(string relative) =>
        relative.StartsWith(ShaderFormatPrefix, StringComparison.Ordinal) && relative.IndexOf('/', ShaderFormatPrefix.Length) > ShaderFormatPrefix.Length;

    private static bool MayUseShaderToolTable(string relative) => UnderAny(relative, ShaderToolTableUsers) || IsShaderFormatModule(relative);

    private static bool MayLoadShaderTools(string relative) => UnderAny(relative, ShaderToolLoadUsers) || IsShaderFormatModule(relative);

    private static bool MayOpenDynamicLibraryPath(string relative) =>
        UnderAny(relative, DynamicLibraryOpenUsers) || IsShaderFormatModule(relative);

    /// <summary>
    /// 셰이더 도구 참조 정책을 저장소의 binary manifest(Engine 루트 <c>kind: binary</c>)로 검사한다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> ShaderToolReferences(string root)
    {
        List<string> libraries = [];
        List<string> headers = [];
        foreach (string library in ThirdPartyManifest.LibraryDirectories(root))
        {
            ThirdPartyManifest manifest;
            try
            {
                manifest = ThirdPartyManifest.Read(library);
            }
            catch (FormatException)
            {
                continue; // manifest 오류는 ThirdParty 검사가 보고한다.
            }

            if (manifest.Kind != ThirdPartyKind.Binary)
            {
                continue;
            }

            libraries.Add(manifest.Name);
            foreach (string header in manifest.AdapterHeaders)
            {
                // include 철자는 include 디렉터리 기준 경로다(예: dxc-1.9.2609/include + dxc/dxcapi.h).
                string? directory = manifest.IncludeDirectories.FirstOrDefault(include => header.StartsWith(include + "/", StringComparison.Ordinal));
                headers.Add(directory is null ? header : header[(directory.Length + 1)..]);
            }
        }

        return ShaderToolReferences(root, libraries, headers);
    }

    /// <summary>
    /// 셰이더 도구 참조 정책이다(Docs/Design/ShaderSystem.ko.md 9절). 도구 표 참조와 binary 로드를 따로 본다.
    /// <list type="bullet">
    /// <item><c>shader-tool-table</c>: 도구 표 모듈(<c>"ShaderToolTable"</c> 의존, <c>&lt;ShaderToolTable/...&gt;</c> include)은
    /// Developer/ShaderCompiler·워커(Programs/ShaderCompileWorker)·Developer/ShaderFormat*·워커 자리 모듈 ShaderToolLoader·시험 runner만
    /// 참조한다. Runtime 모듈의 참조는 거부한다.</item>
    /// <item><c>shader-tool-binary-load</c>: binary를 여는 코드는 워커·ShaderFormat*·ShaderToolLoader·시험 runner에만 둔다. 다음을 거부한다:
    /// 그 밖의 규칙이 ShaderToolLoader나 binary 라이브러리(<paramref name="binaryLibraries"/>, 예: DirectXShaderCompiler)를 이름으로
    /// 참조, 그 밖의 소스가 <c>&lt;ShaderToolLoader/...&gt;</c>나 binary 공개 헤더(<paramref name="binaryHeaders"/>, 예: dxc/dxcapi.h)를
    /// include, DynamicLibrary를 쓰는 소스의 절대 경로 <c>Open(</c> 호출(PlatformBase·TestSupport·모듈 로더 manifest 적재 제외, OS 라이브러리는 OpenSystem).</item>
    /// </list>
    /// 주석은 보지 않는다. 문자열 리터럴은 보므로(의존 목록) 규칙의 설명 문자열에 모듈 이름을 따옴표로 쓰지 않는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> ShaderToolReferences(string root, IReadOnlyList<string> binaryLibraries,
        IReadOnlyList<string> binaryHeaders)
    {
        List<CheckFailure> failures = [];
        Regex binaryInclude = binaryHeaders.Count == 0
            ? Pattern("(?!)")
            : Pattern("#\\s*include\\s*[<\"](?:" + string.Join('|', binaryHeaders.Select(Regex.Escape)) + ")[>\"]");
        foreach (string path in Files(root, OwnedSourceRoots).Where(path => path.EndsWith(".Build.cs", StringComparison.Ordinal)))
        {
            string relative = Relative(root, path);
            string text = WithoutComments(File.ReadAllText(path, StrictUtf8));
            if (!MayUseShaderToolTable(relative) && text.Contains("\"ShaderToolTable\"", StringComparison.Ordinal))
            {
                failures.Add(new(relative, LineOf(text, "\"ShaderToolTable\""), "shader-tool-table",
                    "only Developer/ShaderCompiler, the shader compile worker and Developer/ShaderFormat* modules may depend on ShaderToolTable"));
            }

            if (MayLoadShaderTools(relative))
            {
                continue;
            }

            foreach (string name in binaryLibraries.Prepend("ShaderToolLoader"))
            {
                string literal = "\"" + name + "\"";
                if (text.Contains(literal, StringComparison.Ordinal))
                {
                    failures.Add(new(relative, LineOf(text, literal), "shader-tool-binary-load",
                        $"only the shader compile worker and Developer/ShaderFormat* modules may reference {name} (tool binaries)"));
                }
            }
        }

        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string relative = Relative(root, path);
            string text = WithoutComments(File.ReadAllText(path, StrictUtf8));
            if (!MayUseShaderToolTable(relative))
            {
                AddMatches(relative, text, ShaderToolTableInclude, "shader-tool-table",
                    "only Developer/ShaderCompiler, the shader compile worker and Developer/ShaderFormat* modules may include the shader tool table", failures);
            }

            if (!MayLoadShaderTools(relative))
            {
                AddMatches(relative, text, ShaderToolLoaderInclude, "shader-tool-binary-load",
                    "only the shader compile worker and Developer/ShaderFormat* modules may open shader tool binaries", failures);
                AddMatches(relative, text, binaryInclude, "shader-tool-binary-load",
                    "only the shader compile worker and Developer/ShaderFormat* modules may include tool binary headers", failures);
            }

            string code = WithoutCommentsAndLiterals(text);
            if (!MayOpenDynamicLibraryPath(relative) && DynamicLibraryUse.IsMatch(code))
            {
                AddMatches(relative, code, AbsoluteOpenCall, "shader-tool-binary-load",
                    "DynamicLibrary::Open(absolute path) is reserved for the shader compile worker; open OS libraries with OpenSystem", failures);
            }
        }

        return failures;
    }

    private static void AddMatches(string relative, string text, Regex pattern, string rule, string message, List<CheckFailure> failures)
    {
        foreach (Match match in pattern.Matches(text))
        {
            failures.Add(new(relative, 1 + text.AsSpan(0, match.Index).Count('\n'), rule, message));
        }
    }

    private static int LineOf(string text, string value) => 1 + text.AsSpan(0, text.IndexOf(value, StringComparison.Ordinal)).Count('\n');
}
