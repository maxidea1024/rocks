using System.Text;
using System.Text.RegularExpressions;

namespace Rocks.Build;

/// <summary>모듈에서 찾은 리플렉션 헤더 하나다. Path는 절대 경로, Include는 그 헤더를 찾은 include 디렉터리 기준 철자(<c>/</c> 구분)다.</summary>
public sealed record ReflectionHeader(string Path, string Include, bool Private);

/// <summary>
/// 모듈의 리플렉션 헤더를 표지로 찾는다(Docs/Design/ReflectionFoundations.ko.md 1.10절 — UE UBT가 모듈 폴더의 헤더에서 줄 첫머리
/// <c>UCLASS</c>·<c>USTRUCT</c>·<c>UENUM</c>을 찾는 것과 같다). 모듈 규칙은 헤더 목록을 적지 않는다. 빌드(<c>MetadataLayout</c>)와 정책
/// 검사(<c>PolicyChecks.ReflectionAnnotations</c>)가 같은 판정을 쓴다.
/// <list type="bullet">
/// <item>표지: 주석(<c>//</c>·<c>/* */</c>)과 문자열·문자 리터럴(raw string 포함)의 내용, 전처리 지시문 줄(역슬래시로 이어진 줄 포함)을 지운
/// 줄이 공백 뒤 <c>ROCKS_CLASS(</c>·<c>ROCKS_STRUCT(</c>·<c>ROCKS_ENUM(</c>로 시작한다. 주석 속 예·매크로 정의·줄 중간의 철자는 표지가
/// 아니다. 어휘 판정이라 전처리 조건(<c>#if 0</c> 등) 안의 표지도 표지다.</item>
/// <item>범위: 모듈 폴더 안에 있는 public·private include 디렉터리의 <c>.h</c>를 재귀로 훑는다. 모듈 폴더 밖 include 디렉터리(서드파티)는
/// 훑지 않는다. 같은 파일을 두 디렉터리에서 찾으면 먼저 찾은 쪽(public)을 쓴다.</item>
/// <item>순서: 철자의 서수 정렬(같은 철자면 public 먼저)이라 같은 입력이면 같은 목록이다.</item>
/// </list>
/// 사용 예:
/// <code>
/// var found = ReflectionHeaderScan.Discover(moduleDirectory, module.PublicIncludeDirectories, module.PrivateIncludeDirectories);
/// bool reflected = ReflectionHeaderScan.HasMarkup(File.ReadAllText(path));
/// </code>
/// </summary>
public static class ReflectionHeaderScan
{
    // 줄 첫머리 리플렉션 선언 표지다(주석·리터럴·전처리 줄을 지운 텍스트의 한 줄).
    private static readonly Regex Markup = new(@"^[ \t]*ROCKS_(?:CLASS|STRUCT|ENUM)[ \t]*\(", RegexOptions.CultureInvariant | RegexOptions.Multiline,
        TimeSpan.FromSeconds(10));

    // 리플렉션 헤더의 확장자다(대소문자를 구분한다 — 플랫폼마다 같은 결과).
    private const string HeaderExtension = ".h";

    // 모듈 폴더를 훑을 때 들어가지 않는 폴더 이름이다: 모듈 시험(모듈에 컴파일되지 않는다), 빌드 출력·저장·버전 관리, 외부 라이브러리.
    private static readonly HashSet<string> SkippedFolders = new(["Tests", "out", "Binaries", "Intermediate", "Saved", ".git", "ThirdParty"],
        StringComparer.OrdinalIgnoreCase);

    private static readonly StringComparer PathComparer =
        OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    private static readonly StringComparison PathComparison =
        OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

    /// <summary>text(헤더 원문)에 리플렉션 선언 표지가 있는가다. 주석·리터럴·전처리 지시문 줄은 보지 않는다.</summary>
    public static bool HasMarkup(string text) => Markup.IsMatch(WithoutPreprocessorLines(WithoutCommentsAndLiterals(text)));

    /// <summary>
    /// moduleDirectory(절대 경로) 안의 include 디렉터리에서 리플렉션 헤더를 찾는다. 반환 목록은 철자 순서다. 없는 디렉터리는 건너뛴다.
    /// </summary>
    public static IReadOnlyList<ReflectionHeader> Discover(string moduleDirectory, IReadOnlyList<string> publicIncludeDirectories,
        IReadOnlyList<string> privateIncludeDirectories)
    {
        string module = Path.GetFullPath(moduleDirectory);
        var seen = new HashSet<string>(PathComparer);
        var found = new List<ReflectionHeader>();
        foreach ((IReadOnlyList<string> directories, bool isPrivate) in new[] { (publicIncludeDirectories, false), (privateIncludeDirectories, true) })
        {
            foreach (string directory in directories.Select(Path.GetFullPath))
            {
                if (!IsInside(directory, module) || !Directory.Exists(directory))
                {
                    continue;
                }

                foreach (string full in Headers(directory, []))
                {
                    if (!seen.Add(full) || !HasMarkup(File.ReadAllText(full)))
                    {
                        continue;
                    }

                    found.Add(new(full, Path.GetRelativePath(directory, full).Replace('\\', '/'), isPrivate));
                }
            }
        }

        return [.. found.OrderBy(header => header.Include, StringComparer.Ordinal).ThenBy(header => header.Private)];
    }

    /// <summary>
    /// moduleDirectory 아래의 표지 헤더 가운데 include 디렉터리(scanned) 밖에 있는 것이다. 그 헤더는 생성 헤더가 원본을 포함할 철자가 없고
    /// 빌드가 찾지도 않으므로(조용히 빠진다) 빌드가 거부한다(1.10절). 모듈 시험·빌드 출력·외부 라이브러리 폴더(<see cref="SkippedFolders"/>)와
    /// excluded(모듈 폴더 안의 다른 모듈 폴더)에는 들어가지 않는다.
    /// </summary>
    public static IReadOnlyList<string> MarkedOutside(string moduleDirectory, IEnumerable<string> scanned, IEnumerable<string> excluded)
    {
        string root = Path.GetFullPath(moduleDirectory);
        if (!Directory.Exists(root))
        {
            return [];
        }

        string[] skipped = [.. scanned.Concat(excluded).Select(Path.GetFullPath)];
        // 시작 폴더 자체가 제외 폴더 안이면(다른 모듈 폴더 안의 include 디렉터리) 훑을 것이 없다.
        if (skipped.Any(skip => IsInside(root, skip)))
        {
            return [];
        }

        return [.. Headers(root, skipped).Where(path => HasMarkup(File.ReadAllText(path))).Order(StringComparer.Ordinal)];
    }

    // directory 아래의 .h 파일이다(절대 경로). skipped 디렉터리, SkippedFolders 이름의 폴더, 디렉터리 링크에는 들어가지 않는다.
    private static IEnumerable<string> Headers(string directory, string[] skipped)
    {
        var pending = new Stack<string>([directory]);
        while (pending.Count != 0)
        {
            string current = pending.Pop();
            foreach (string file in Directory.EnumerateFiles(current))
            {
                if (file.EndsWith(HeaderExtension, StringComparison.Ordinal))
                {
                    yield return Path.GetFullPath(file);
                }
            }

            foreach (string child in Directory.EnumerateDirectories(current))
            {
                string full = Path.GetFullPath(child);
                // 디렉터리 링크(symlink·junction)에는 들어가지 않는다 — 같은 헤더를 두 경로로 찾거나 순환하지 않게.
                bool link = (File.GetAttributes(full) & FileAttributes.ReparsePoint) != 0;
                if (!link && !SkippedFolders.Contains(Path.GetFileName(full)) && !skipped.Any(skip => IsInside(full, skip)))
                {
                    pending.Push(full);
                }
            }
        }
    }

    // path가 directory 자신이거나 그 아래인가다.
    private static bool IsInside(string path, string directory)
    {
        string prefix = directory.EndsWith(Path.DirectorySeparatorChar) ? directory : directory + Path.DirectorySeparatorChar;
        return path.Equals(directory, PathComparison) || path.StartsWith(prefix, PathComparison);
    }

    // 전처리 지시문 줄(공백 뒤 #으로 시작하는 줄과 역슬래시로 이어진 다음 줄들)을 공백으로 바꾼다. 줄바꿈은 남긴다. 주석·리터럴을 지운
    // 텍스트에 쓴다(여러 줄 #define의 이어진 줄이 표지로 시작해도 표지가 아니다).
    private static string WithoutPreprocessorLines(string text)
    {
        string[] lines = text.Split('\n');
        bool continued = false;
        for (int index = 0; index < lines.Length; ++index)
        {
            string line = lines[index];
            bool directive = continued || line.TrimStart(' ', '\t').StartsWith('#');
            continued = directive && line.TrimEnd('\r', ' ', '\t').EndsWith('\\');
            if (directive)
            {
                lines[index] = new string(' ', line.Length);
            }
        }

        return string.Join('\n', lines);
    }

    // 주석과 문자열·문자 리터럴의 내용을 공백으로 바꾼 텍스트다(줄바꿈과 따옴표는 남긴다). 리터럴 안의 주석 기호는 주석이 아니고, 리터럴 안
    // 역슬래시 줄 이음 뒤의 철자와 raw string(R"구분자( … )구분자")의 여러 줄 내용도 지운다. // 주석 끝의 역슬래시는 다음 줄도 주석으로
    // 잇는다. 숫자 뒤의 '는 자릿수 구분자(1'000)라 문자 리터럴이 아니다.
    private static string WithoutCommentsAndLiterals(string text)
    {
        var output = new StringBuilder(text.Length);
        int index = 0;
        while (index < text.Length)
        {
            char current = text[index];
            char next = index + 1 < text.Length ? text[index + 1] : '\0';
            if (current == '/' && next == '/')
            {
                // 줄 끝의 역슬래시는 주석을 다음 줄로 잇는다.
                while (index < text.Length)
                {
                    if (text[index] == '\n')
                    {
                        int end = index - 1;
                        while (end >= 0 && text[end] == '\r')
                        {
                            --end;
                        }

                        if (end < 0 || text[end] != '\\')
                        {
                            break;
                        }

                        output.Append('\n');
                        ++index;
                        continue;
                    }

                    output.Append(' ');
                    ++index;
                }
            }
            else if (current == '/' && next == '*')
            {
                output.Append("  ");
                index += 2;
                while (index < text.Length && !(text[index] == '*' && index + 1 < text.Length && text[index + 1] == '/'))
                {
                    output.Append(Blank(text[index]));
                    ++index;
                }

                if (index < text.Length)
                {
                    output.Append("  ");
                    index += 2;
                }
            }
            else if (current == 'R' && next == '"' && (index == 0 || !IsIdentifierChar(text[index - 1]) || IsRawPrefix(text, index)))
            {
                // raw string: R"구분자( … )구분자". 구분자를 읽고 닫는 )구분자"까지 지운다.
                int open = text.IndexOf('(', index + 2);
                if (open < 0)
                {
                    output.Append(current);
                    ++index;
                    continue;
                }

                string close = ")" + text[(index + 2)..open] + "\"";
                int end = text.IndexOf(close, open + 1, StringComparison.Ordinal);
                int stop = end < 0 ? text.Length : end + close.Length;
                output.Append("R\"");
                for (int blank = index + 2; blank < stop; ++blank)
                {
                    output.Append(Blank(text[blank]));
                }

                index = stop;
            }
            else if (current == '"' || (current == '\'' && (index == 0 || !char.IsAsciiDigit(text[index - 1]))))
            {
                output.Append(current);
                ++index;
                while (index < text.Length && text[index] != current && text[index] != '\n')
                {
                    if (text[index] == '\\' && index + 1 < text.Length)
                    {
                        output.Append(' ');
                        ++index;
                    }

                    output.Append(Blank(text[index]));
                    ++index;
                }

                if (index < text.Length && text[index] == current)
                {
                    output.Append(current);
                    ++index;
                }
            }
            else
            {
                output.Append(current);
                ++index;
            }
        }

        return output.ToString();
    }

    // 지운 자리의 문자다(줄바꿈은 남겨 줄 번호를 지킨다).
    private static char Blank(char value) => value == '\n' ? '\n' : ' ';

    private static bool IsIdentifierChar(char value) => char.IsAsciiLetterOrDigit(value) || value == '_';

    // R 앞이 raw string 접두(u8R·uR·UR·LR)인가다.
    private static bool IsRawPrefix(string text, int index)
    {
        int start = index;
        while (start > 0 && IsIdentifierChar(text[start - 1]))
        {
            --start;
        }

        string prefix = text[start..index];
        return prefix is "u8" or "u" or "U" or "L";
    }
}
