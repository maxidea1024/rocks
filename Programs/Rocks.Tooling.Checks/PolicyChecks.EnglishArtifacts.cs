using System.Text;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 실행 중에 사용자·개발자에게 보이는 문자열(로그·추적·진단·오류·도움말)이 들어가는 소스의 루트다. 주석은 한국어로 둘 수 있고
    // 주석 밖의 한글만 금지한다. Build/는 모듈·타깃 규칙 기반 클래스(Build/Rules/*.cs)가 빌드 도구 문구를 낸다.
    private static readonly string[] EnglishMessageRoots =
        ["Core/Source/Runtime", "Core/Source/Developer", "Engine/Source/Runtime", "Engine/Source/Developer", "Engine/Plugins", "Programs", "Build"];

    // 빌드 설정과 생성물처럼 파일 전체(주석 포함)를 영어로 두는 저장소 루트의 파일이다. 빌드 도구·편집기·CI가 읽는 설정이다.
    private static readonly string[] EnglishRootFiles =
        ["Rocks.proj", ".editorconfig", ".gitattributes", ".gitignore", ".clang-format", "global.json", "Directory.Build.rsp"];

    // 파일 전체를 영어로 두는 디렉터리다. Templates/는 새 게임 프로젝트로 복사되는 결과물이고, Programs/Scaffold/Templates/는
    // scaffold가 그대로 써 넣는 생성 원본이며, <루트>/Config/는 엔진이 읽는 설정 파일이다.
    private static readonly string[] EnglishWholeDirectories = ["Templates", "Programs/Scaffold/Templates", "Engine/Config", "Core/Config"];

    // 이름만으로 빌드 설정임을 아는 파일이다(모듈·타깃 규칙, MSBuild 프로젝트·속성, 솔루션).
    private static readonly string[] EnglishBuildFileSuffixes = [".Build.cs", ".Target.cs", ".csproj", ".props", ".targets", ".slnx", ".proj"];

    // 예외(명시 목록). 영어 규칙을 적용하지 않는 것과 그 이유다.
    // - 문서(Docs/·*.md·*.svg·*.pdf): 설계·사용 문서는 한국어로 쓴다.
    // - 외부 원본(ThirdParty.json의 upstreamDirectory): 업스트림 byte를 그대로 보존한다(예: UnicodeData/ucd-*/NormalizationTest.txt의 한글).
    // - 시험 소스(모듈 Tests/·Programs/FoundationTests/·C# 시험 프로젝트 Programs/Rocks.*Tests/): 유니코드 처리를 확인하려고
    //   한글 경로·문자열을 일부러 쓴다. 문자열 규칙에만 해당하고, 시험 실행 파일의 Build.cs·Target.cs와 시험용 생성 파일은 영어다.
    // - 시험 fixture 모듈(*TestFixtures): DisplayName 같은 메타데이터에 한글을 넣어 리플렉션의 UTF-8 처리를 시험한다. 문자열
    //   규칙에만 해당하고, fixture 모듈의 Build.cs는 영어다.
    private static readonly string[] EnglishDocumentExtensions = [".md", ".svg", ".pdf"];

    // 빌드 출력·로컬 상태 디렉터리다(.gitignore). Build/ 아래를 훑을 때 건너뛴다.
    private static readonly string[] EnglishIgnoredDirectories = ["Build/Artifacts/", "Build/Cache/"];

    private static bool IsEnglishDocument(string relative) =>
        relative.StartsWith("Docs/", StringComparison.Ordinal) ||
        EnglishDocumentExtensions.Any(extension => relative.EndsWith(extension, StringComparison.OrdinalIgnoreCase));

    // C# 시험 프로젝트(Programs/Rocks.ToolTests·Rocks.Build.Tests·Rocks.Build.IntegrationTests·Rocks.Commandlet.Tests 등)다.
    private static bool IsCSharpTestProject(string relative)
    {
        string[] parts = relative.Split('/');
        return parts.Length > 2 && parts[0] == "Programs" && parts[1].StartsWith("Rocks.", StringComparison.Ordinal) &&
               parts[1].EndsWith("Tests", StringComparison.Ordinal);
    }

    // 시험 fixture 모듈(<루트>/Source/Developer/<이름>TestFixtures/) 안의 파일인가.
    private static bool IsTestFixtureSource(string relative)
    {
        string[] parts = relative.Split('/');
        return parts.Length > 4 && parts[1] == "Source" && parts[2] == "Developer" &&
               parts[3].EndsWith("TestFixtures", StringComparison.Ordinal);
    }

    // 생성 파일(*.generated.h 등)인가. 생성기가 내는 주석까지 영어여야 한다.
    private static bool IsGeneratedFile(string relative) =>
        Path.GetFileName(relative).Contains(".generated.", StringComparison.Ordinal);

    /// <summary>
    /// 시험이 아닌 런타임·개발 도구·프로그램 소스의 주석 밖 한글을 금지한다(rule id source-literal-english,
    /// Docs/CodingStyle.md). 로그·추적·진단·오류·도움말처럼 실행 중에 보이는 문자열은 영어로 쓴다. 대상은 C++(.h·.c·.cpp·.mm)과
    /// C#(.cs) 소스이고 주석(//·/* */·///)은 보지 않으므로 손으로 쓴 주석은 한국어로 둔다. 시험 소스·시험 fixture 모듈·외부 원본은
    /// 제외하고, 파일 전체를 보는 <see cref="EnglishBuildFiles"/> 대상(빌드 규칙·생성 파일)은 그쪽에서 보고한다.
    /// </summary>
    /// <param name="root">저장소 루트.</param>
    /// <returns>위반마다 한 항목(한글이 있는 줄마다 하나).</returns>
    public static IReadOnlyList<CheckFailure> EnglishSourceLiterals(string root)
    {
        List<CheckFailure> failures = [];
        IReadOnlyList<string> upstream = UpstreamDirectories(root);
        foreach (string path in Files(root, EnglishMessageRoots))
        {
            string relative = Relative(root, path);
            string extension = Path.GetExtension(path).ToLowerInvariant();
            bool native = extension is ".h" or ".c" or ".cpp" or ".mm";
            if ((!native && extension != ".cs") || IsTestSource(relative) || IsCSharpTestProject(relative) ||
                IsTestFixtureSource(relative) || IsEnglishBuildFile(relative) || IsEnglishIgnored(relative) ||
                upstream.Any(directory => relative.StartsWith(directory, StringComparison.Ordinal)))
            {
                continue;
            }

            string text = File.ReadAllText(path, StrictUtf8);
            string code = native ? WithoutComments(text) : WithoutCSharpComments(text);
            AddHangulLines(relative, code, "source-literal-english",
                "text outside comments must be English (runtime, tool and diagnostic strings; comments may stay Korean)", failures);
        }

        return failures;
    }

    /// <summary>
    /// 빌드 설정과 생성물은 주석까지 파일 전체를 영어로 쓴다(rule id build-file-english, Docs/CodingStyle.md). 대상은 다음이다.
    /// <list type="bullet">
    /// <item>저장소 루트의 Rocks.proj·.editorconfig·.gitattributes·.gitignore·.clang-format·global.json·Directory.Build.rsp</item>
    /// <item>.github/workflows/ 아래 파일</item>
    /// <item>모든 <c>*.Build.cs</c>·<c>*.Target.cs</c>·<c>*.csproj</c>·<c>*.props</c>·<c>*.targets</c>·<c>*.slnx</c>·<c>*.proj</c></item>
    /// <item>Build/ 아래 스크립트·설정(ps1·sh·cmake 등, 규칙 기반 클래스 *.cs와 빌드 출력 디렉터리는 제외)</item>
    /// <item>ThirdParty 라이브러리 폴더에서 Rocks가 쓴 파일(ThirdParty.json·Build.cs·도구 빌드 스크립트·patch·unity 파일 등,
    /// upstreamDirectory는 제외)</item>
    /// <item>Templates/·Programs/Scaffold/Templates/·Engine/Config/·Core/Config/ 아래 파일</item>
    /// <item>생성 파일(<c>*.generated.*</c>, 시험 디렉터리 안의 것 포함)</item>
    /// </list>
    /// 문서(Docs/·*.md·*.svg·*.pdf)는 어디서나 제외한다.
    /// </summary>
    /// <param name="root">저장소 루트.</param>
    /// <returns>위반마다 한 항목(한글이 있는 줄마다 하나).</returns>
    public static IReadOnlyList<CheckFailure> EnglishBuildFiles(string root)
    {
        root = Path.GetFullPath(root);
        SortedSet<string> targets = new(StringComparer.Ordinal);
        foreach (string file in EnglishRootFiles.Where(file => File.Exists(Path.Combine(root, file))))
        {
            targets.Add(file);
        }

        IReadOnlyList<string> upstream = UpstreamDirectories(root);
        foreach (string path in Files(root, [".github/workflows", "Build", .. OwnedSourceRoots]))
        {
            string relative = Relative(root, path);
            if (IsEnglishBuildFile(relative) && !IsEnglishIgnored(relative) &&
                !upstream.Any(directory => relative.StartsWith(directory, StringComparison.Ordinal)))
            {
                targets.Add(relative);
            }
        }

        foreach (string path in Files(root, ThirdPartyManifest.ThirdPartyRoots))
        {
            string relative = Relative(root, path);
            if (!upstream.Any(directory => relative.StartsWith(directory, StringComparison.Ordinal)) && !IsEnglishDocument(relative))
            {
                targets.Add(relative);
            }
        }

        List<CheckFailure> failures = [];
        foreach (string relative in targets)
        {
            byte[] bytes = File.ReadAllBytes(Path.Combine(root, relative));
            if (Array.IndexOf(bytes, (byte)0) >= 0)
            {
                continue;
            }

            AddHangulLines(relative, Encoding.UTF8.GetString(bytes), "build-file-english",
                "build configuration and generated files must be English, including comments", failures);
        }

        return failures;
    }

    // 파일 전체 영어 규칙(EnglishBuildFiles)의 대상인가. 문서는 제외한다. ThirdParty 폴더는 upstreamDirectory를 알아야 하므로
    // EnglishBuildFiles가 따로 고른다.
    private static bool IsEnglishBuildFile(string relative)
    {
        if (IsEnglishDocument(relative))
        {
            return false;
        }

        return relative.StartsWith(".github/workflows/", StringComparison.Ordinal) ||
               EnglishBuildFileSuffixes.Any(suffix => relative.EndsWith(suffix, StringComparison.Ordinal)) ||
               (relative.StartsWith("Build/", StringComparison.Ordinal) && !relative.EndsWith(".cs", StringComparison.Ordinal)) ||
               EnglishWholeDirectories.Any(directory => relative.StartsWith(directory + "/", StringComparison.Ordinal)) ||
               IsGeneratedFile(relative);
    }

    private static bool IsEnglishIgnored(string relative) =>
        EnglishIgnoredDirectories.Any(directory => relative.StartsWith(directory, StringComparison.Ordinal));

    private static void AddHangulLines(string relative, string text, string rule, string message, List<CheckFailure> failures)
    {
        int line = 1;
        int reported = 0;
        for (int index = 0; index < text.Length; ++index)
        {
            char current = text[index];
            if (current == '\n')
            {
                ++line;
            }
            else if (reported != line && IsHangul(current))
            {
                failures.Add(new(relative, line, rule, message));
                reported = line;
            }
        }
    }

    // 한글 자모(U+1100–U+11FF)·호환 자모(U+3130–U+318F)·완성형 음절(U+AC00–U+D7A3)이다(CompileTimeMessages의 Hangul과 같은 범위).
    private static bool IsHangul(char value) =>
        value is (>= (char)0x1100 and <= (char)0x11FF) or (>= (char)0x3130 and <= (char)0x318F) or (>= (char)0xAC00 and <= (char)0xD7A3);

    // C# 소스에서 주석(//·/* */, 문서 주석 포함)만 공백으로 바꾼 텍스트다(길이·줄 번호는 원문과 같다). 문자열 안의 // 나 /* 를 주석으로
    // 보지 않도록 일반·축자(@"", "" escape)·원시("""…""", 따옴표 수가 같은 닫는 표지) 문자열과 문자 리터럴을 건너뛴다. 보간 문자열의
    // 구멍 안 문자열은 바깥 문자열을 끝냈다가 다시 여는 것으로 읽히지만 주석 경계는 같다. lexical 검사용 근사다.
    internal static string WithoutCSharpComments(string text)
    {
        StringBuilder result = new(text.Length);
        int index = 0;
        void Blank(int end)
        {
            for (; index < end && index < text.Length; ++index)
            {
                result.Append(text[index] == '\n' ? '\n' : ' ');
            }
        }

        void Keep(int end)
        {
            for (; index < end && index < text.Length; ++index)
            {
                result.Append(text[index]);
            }
        }

        while (index < text.Length)
        {
            char c = text[index];
            char next = index + 1 < text.Length ? text[index + 1] : '\0';
            if (c == '/' && next == '/')
            {
                int end = text.IndexOf('\n', index);
                Blank(end < 0 ? text.Length : end);
            }
            else if (c == '/' && next == '*')
            {
                int end = text.IndexOf("*/", index + 2, StringComparison.Ordinal);
                Blank(end < 0 ? text.Length : end + 2);
            }
            else if (c == '"' && next == '"' && index + 2 < text.Length && text[index + 2] == '"')
            {
                int quotes = 0;
                while (index + quotes < text.Length && text[index + quotes] == '"')
                {
                    ++quotes;
                }

                int end = text.IndexOf(new string('"', quotes), index + quotes, StringComparison.Ordinal);
                Keep(end < 0 ? text.Length : end + quotes);
            }
            else if ((c == '@' && next == '"') ||
                     (((c == '@' && next == '$') || (c == '$' && next == '@')) && index + 2 < text.Length && text[index + 2] == '"'))
            {
                int end = text.IndexOf('"', index) + 1;
                while (end < text.Length && !(text[end] == '"' && (end + 1 >= text.Length || text[end + 1] != '"')))
                {
                    end += text[end] == '"' ? 2 : 1;
                }

                Keep(Math.Min(end + 1, text.Length));
            }
            else if (c == '"' || (c == '\'' && (index == 0 || !char.IsAsciiLetterOrDigit(text[index - 1]))))
            {
                int end = index + 1;
                while (end < text.Length && text[end] != c && text[end] != '\n')
                {
                    end += text[end] == '\\' ? 2 : 1;
                }

                Keep(Math.Min(end + 1, text.Length));
            }
            else
            {
                result.Append(c);
                ++index;
            }
        }

        return result.ToString();
    }
}
