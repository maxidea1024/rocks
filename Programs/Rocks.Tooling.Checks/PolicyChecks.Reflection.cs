using System.Text;
using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 리플렉션 필드 annotation(ROCKS_PROPERTY·ROCKS_NATIVE)과 native 타입 매크로(ROCKS_NATIVE_TYPE)다. ROCKS_NATIVE 뒤의 \b는 단어
    // 문자(_TYPE)를 받지 않으므로 ROCKS_NATIVE_TYPE과 겹치지 않는다.
    private static readonly Regex ReflectionAnnotation = Pattern("\\bROCKS_(?<name>PROPERTY|NATIVE_TYPE|NATIVE)\\b");

    // 모듈 규칙(*.Build.cs)에서 Metadata라는 이름이다. ImportedMetadata 같은 다른 이름은 앞뒤의 \b가 거른다.
    private static readonly Regex MetadataName = Pattern("\\bMetadata\\b");

    // 리터럴 목록의 괄호 안(주석을 지운 텍스트)이다: 문자열 리터럴만 쉼표로 나열하고 끝 쉼표를 허용한다.
    private static readonly Regex LiteralList = Pattern(
        "^\\s*(?:\"(?:[^\"\\\\\\n]|\\\\.)*\"\\s*(?:,\\s*\"(?:[^\"\\\\\\n]|\\\\.)*\"\\s*)*,?\\s*)?$");

    private static readonly Regex StringLiteral = Pattern("\"((?:[^\"\\\\\\n]|\\\\.)*)\"");

    /// <summary>
    /// MetaCompiler가 보지 않는 리플렉션 필드 annotation을 거부한다(Docs/Design/ReflectionFoundations.ko.md 9.1절 B8a).
    /// ROCKS_PROPERTY·ROCKS_NATIVE는 Clang attribute일 뿐이므로 MetaCompiler가 생성 코드를 내지 않으면 컴파일은 되지만 GC가
    /// 추적하지도 저장하지도 않는 필드로 조용히 남는다(GC 구멍). 엔진 저장소(템플릿 포함)의 소유 native 소스(.h·.c·.cpp·.mm —
    /// 시험·Developer 포함)와 모듈 규칙에서 다음을 찾는다.
    /// <list type="bullet">
    /// <item>reflection-annotation-native-type: ROCKS_NATIVE_TYPE을 직접 담은 클래스 본문 안의 annotation(중첩 클래스의 멤버는 그
    /// 중첩 클래스로 판정한다). native 클래스는 MetaCompiler 대상이 아니다 — 리플렉션 필드가 필요하면 ROCKS_CLASS로 Metadata
    /// 헤더에 선언하고, 아니면 annotation을 지우고 강한 간선은 손 Trace로 방문한다. Metadata 헤더 안이면 MetaCompiler도
    /// 거부하므로 이 규칙의 실익은 .cpp 안 native 클래스에 구체적인 메시지를 주는 데 있다.</item>
    /// <item>reflection-annotation-unlisted: 어느 모듈 규칙의 Metadata 목록에도 없는 파일의 annotation. 목록은 *.Build.cs의
    /// `Metadata =&gt; [...]`·`Metadata = [...]`·`Metadata { get; } = [...]` 문자열 리터럴이고(목록 안 주석은 항목이 아니다),
    /// 파일은 그 규칙 파일이 있는 모듈 폴더 아래에 있으면서 경로가 "/철자"로 끝나면 목록에 있다고 본다(include 디렉터리 해석이
    /// 아니라 lexical 근사다).</item>
    /// <item>reflection-metadata-not-literal: 리터럴 목록이 아닌 Metadata 정의(식별자·`new[] { }`·도우미 호출·spread·보간
    /// 문자열·접근자 본문). 이 검사가 목록을 읽을 수 없으므로 규칙 파일 위치에 보고하고, 그 모듈 폴더의 파일에는 unlisted를
    /// 보고하지 않는다.</item>
    /// </list>
    /// 주석·문자열과 전처리 지시문 줄(매크로 정의)은 보지 않는다. 한 위치에는 native-type을 먼저 보고한다. 알려진 lexical 한계:
    /// 매크로로 감싼 annotation(정의 줄은 보지 않고 사용 위치에는 이름이 없다), u8·L 접두 문자 리터럴 안의 중괄호, 줄 끝 역슬래시
    /// 앞에 블록 주석이 있는 #define 이어짐 줄, #if로 갈린 두 클래스 머리(중괄호 짝이 어긋난다), .hpp·.inl 같은 다른 확장자는
    /// 판정이 틀리거나 검사하지 않는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> ReflectionAnnotations(string root)
    {
        List<CheckFailure> failures = [];
        MetadataListing metadata = ReadMetadataListing(root);
        failures.AddRange(metadata.Failures);
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            string code = WithoutPreprocessorLines(WithoutCommentsAndLiterals(File.ReadAllText(path, StrictUtf8)));
            MatchCollection matches = ReflectionAnnotation.Matches(code);
            if (matches.Count == 0)
            {
                continue;
            }

            string relative = Relative(root, path);
            int[] scopes = InnermostScopes(code, matches);
            HashSet<int> nativeTypeScopes = [];
            for (int index = 0; index < matches.Count; ++index)
            {
                if (matches[index].Groups["name"].Value == "NATIVE_TYPE" && scopes[index] >= 0)
                {
                    nativeTypeScopes.Add(scopes[index]);
                }
            }

            bool listed = metadata.Unknown.Any(directory => relative.StartsWith(directory + "/", StringComparison.Ordinal)) ||
                metadata.Entries.Any(entry => relative.StartsWith(entry.Directory + "/", StringComparison.Ordinal) &&
                    relative.EndsWith("/" + entry.Spelling, StringComparison.Ordinal));
            for (int index = 0; index < matches.Count; ++index)
            {
                string name = matches[index].Groups["name"].Value;
                if (name == "NATIVE_TYPE")
                {
                    continue;
                }

                int line = 1 + code.AsSpan(0, matches[index].Index).Count('\n');
                if (nativeTypeScopes.Contains(scopes[index]))
                {
                    failures.Add(new(relative, line, "reflection-annotation-native-type",
                        $"ROCKS_{name} inside a ROCKS_NATIVE_TYPE class is never generated, so the field is neither traced by GC nor saved; " +
                        "declare the class with ROCKS_CLASS in a Metadata header, or drop the annotation and visit strong edges in a hand Trace"));
                    continue;
                }

                if (!listed)
                {
                    failures.Add(new(relative, line, "reflection-annotation-unlisted",
                        $"ROCKS_{name} in a file that no module lists in Metadata is never seen by MetaCompiler, so the field is neither " +
                        "traced by GC nor saved; add the header to its module's Metadata (reflected types belong in headers)"));
                }
            }
        }

        return failures;
    }

    // 모듈 생성 헤더(<모듈>/<모듈>.module.generated.h·<모듈>/<모듈>.private.module.generated.h)의 include 지시문이다. 주석을 지운 한
    // 줄에 맞춘다(문자열 리터럴 안의 철자는 줄 머리의 # 가 아니라 맞지 않는다).
    private static readonly Regex ModuleGeneratedHeaderInclude = Pattern(
        "^[ \\t]*#[ \\t]*include[ \\t]*[<\"][^<>\"]*\\.module\\.generated\\.h[>\"]");

    /// <summary>
    /// 손으로 쓴 헤더가 모듈 생성 헤더(<c>*.module.generated.h</c>)를 include하면 거부한다(Docs/Design/ReflectionGeneratedSources.ko.md 8절
    /// GSD6, 규칙 <c>module-generated-header-include</c>). 모듈 생성 헤더는 타입 등록 함수의 선언만 담는다. 등록은 번역 단위(.cpp)가
    /// 하는 일이므로 헤더가 끌어올 이유가 없고, 헤더가 include하면 그 헤더를 쓰는 모든 번역 단위가 등록 함수 선언에 묶인다. 헤더가
    /// <c>GeneratedType&lt;T&gt;</c>를 써야 하면 그 타입의 생성 헤더(<c>&lt;모듈/타입.generated.h&gt;</c>)를 include하고, 등록 함수는
    /// 모듈 생성 헤더를 include한 .cpp에서 부른다.
    /// <list type="bullet">
    /// <item>대상: 엔진 저장소(템플릿 포함)의 소유 <c>.h</c> 전부 — 모듈의 Public·Private, 시험(Tests), 프로그램, 템플릿.</item>
    /// <item>제외: 이름에 <c>.generated.</c>가 든 파일(생성물), 외부 라이브러리 원본, <c>.cpp</c>·<c>.c</c>·<c>.mm</c>.</item>
    /// <item>주석 안의 예시(사용 예의 #include 줄)와 문자열 리터럴 안의 철자는 보지 않는다.</item>
    /// </list>
    /// 알려진 lexical 한계: 매크로로 만든 include 철자, <c>.hpp</c>·<c>.inl</c> 같은 다른 헤더 확장자(소유 소스는 쓰지 않는다)는 검사하지
    /// 않는다. 다른 헤더를 거친 간접 include는 그 헤더에서 보고한다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> ModuleGeneratedHeaderIncludes(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, OwnedSourceRoots))
        {
            if (!Path.GetExtension(path).Equals(".h", StringComparison.OrdinalIgnoreCase) ||
                Path.GetFileName(path).Contains(".generated.", StringComparison.Ordinal))
            {
                continue;
            }

            string relative = Relative(root, path);
            int line = 0;
            foreach (string text in WithoutComments(File.ReadAllText(path, StrictUtf8)).Split('\n'))
            {
                ++line;
                if (ModuleGeneratedHeaderInclude.IsMatch(text))
                {
                    failures.Add(new(relative, line, "module-generated-header-include",
                        "a hand-written header must not include a module generated header (*.module.generated.h); include the generated " +
                        "header of each type it uses (<Module/Type.generated.h>) and call the registration function from a .cpp"));
                }
            }
        }

        return failures;
    }

    // 모듈 규칙들의 Metadata 목록이다: (저장소 상대 모듈 폴더, 철자) 항목, 리터럴 목록이 아니라 읽지 못한 모듈 폴더, 그 위치의
    // 실패.
    private sealed record MetadataListing(List<(string Directory, string Spelling)> Entries, HashSet<string> Unknown, List<CheckFailure> Failures);

    private static MetadataListing ReadMetadataListing(string root)
    {
        MetadataListing listing = new([], new(StringComparer.Ordinal), []);
        foreach (string path in Files(root, OwnedSourceRoots).Where(path => path.EndsWith(".Build.cs", StringComparison.Ordinal)))
        {
            string text = File.ReadAllText(path, StrictUtf8);
            // code는 주석·문자열을 지운 텍스트(구조 판정), commentless는 주석만 지운 텍스트(리터럴 읽기)다. 둘 다 원문과 길이가 같다.
            string code = WithoutCommentsAndLiterals(text);
            string commentless = WithoutComments(text);
            string relative = Relative(root, path);
            int slash = relative.LastIndexOf('/');
            string directory = slash < 0 ? "" : relative[..slash];
            foreach (Match match in MetadataName.Matches(code))
            {
                int value = MetadataValueStart(code, match);
                if (value == NotDefinition)
                {
                    continue;
                }

                List<string>? spellings = value < 0 ? null : LiteralListAt(code, commentless, value);
                if (spellings is null)
                {
                    listing.Unknown.Add(directory);
                    int line = 1 + code.AsSpan(0, match.Index).Count('\n');
                    listing.Failures.Add(new(relative, line, "reflection-metadata-not-literal",
                        "Metadata must be a literal list of header spellings (Metadata => [\"Module/Type.h\"]) so policy checks can find " +
                        "the reflected headers; identifiers, new[] { }, helpers, spreads and interpolated strings are not read"));
                    continue;
                }

                listing.Entries.AddRange(spellings.Select(spelling => (directory, spelling)));
            }
        }

        return listing;
    }

    private const int NotDefinition = int.MinValue;

    // Metadata 이름 뒤가 정의(`=>`·`=`·접근자 블록 뒤 `=`)이면 값이 시작하는 위치, 접근자 본문이라 값을 읽을 수 없으면 -1, 정의가
    // 아니면(멤버 접근 `x.Metadata`·인자·형식 이름 등) NotDefinition이다.
    private static int MetadataValueStart(string code, Match match)
    {
        int before = match.Index - 1;
        while (before >= 0 && char.IsWhiteSpace(code[before]))
        {
            --before;
        }

        if (before >= 0 && code[before] == '.')
        {
            return NotDefinition;
        }

        int index = SkipSpace(code, match.Index + match.Length);
        if (index < code.Length && code[index] == '{')
        {
            // 프로퍼티 접근자 블록이다. `{ get; } = [...]`·`{ get; init; } = [...]`만 초기화 값으로 읽는다.
            int close = code.IndexOf('}', index);
            if (close < 0)
            {
                return -1;
            }

            string accessors = code[(index + 1)..close];
            int next = SkipSpace(code, close + 1);
            bool autoProperty = accessors.Split(';', StringSplitOptions.TrimEntries).All(part => part is "" or "get" or "init" or "set");
            return autoProperty && next < code.Length && code[next] == '=' && (next + 1 >= code.Length || code[next + 1] != '=')
                ? SkipSpace(code, next + 1)
                : -1;
        }

        if (index + 1 < code.Length && code[index] == '=' && code[index + 1] == '>')
        {
            return SkipSpace(code, index + 2);
        }

        if (index < code.Length && code[index] == '=' && (index + 1 >= code.Length || code[index + 1] != '='))
        {
            return SkipSpace(code, index + 1);
        }

        return NotDefinition;
    }

    // value 위치의 값이 문자열 리터럴만 담은 `[...]`이고 식이 거기서 끝나면(`;`·`,`·`}`) 그 철자들, 아니면 null이다.
    private static List<string>? LiteralListAt(string code, string commentless, int value)
    {
        if (value >= code.Length || code[value] != '[')
        {
            return null;
        }

        int close = code.IndexOf(']', value + 1);
        if (close < 0 || code.IndexOf('[', value + 1, close - value - 1) >= 0)
        {
            return null;
        }

        int after = SkipSpace(code, close + 1);
        if (after >= code.Length || code[after] is not (';' or ',' or '}'))
        {
            return null;
        }

        string content = commentless[(value + 1)..close];
        if (!LiteralList.IsMatch(content))
        {
            return null;
        }

        return StringLiteral.Matches(content).Select(literal => literal.Groups[1].Value.Replace('\\', '/')).ToList();
    }

    private static int SkipSpace(string code, int index)
    {
        while (index < code.Length && char.IsWhiteSpace(code[index]))
        {
            ++index;
        }

        return index;
    }

    // 각 match 위치를 담은 가장 안쪽 중괄호 범위의 번호다(파일 범위는 -1). 주석·문자열을 지운 텍스트에서 센다.
    private static int[] InnermostScopes(string code, MatchCollection matches)
    {
        int[] scopes = new int[matches.Count];
        Stack<int> open = new();
        int next = 0;
        int position = 0;
        for (int index = 0; index < matches.Count; ++index)
        {
            for (; position < matches[index].Index; ++position)
            {
                if (code[position] == '{')
                {
                    open.Push(next++);
                }
                else if (code[position] == '}' && open.Count != 0)
                {
                    open.Pop();
                }
            }

            scopes[index] = open.Count == 0 ? -1 : open.Peek();
        }

        return scopes;
    }

    // 전처리 지시문 줄(# 로 시작하는 줄과 역슬래시로 이어진 줄)을 공백으로 바꾼다. 줄바꿈은 남겨 줄 번호를 지킨다. 주석·문자열을
    // 이미 지운 텍스트에 쓴다(매크로 정의 안의 이름을 사용 위치로 보지 않는다).
    private static string WithoutPreprocessorLines(string code)
    {
        StringBuilder result = new(code.Length);
        bool directive = false;
        int start = 0;
        while (start < code.Length)
        {
            int end = code.IndexOf('\n', start);
            int stop = end < 0 ? code.Length : end;
            string line = code[start..stop];
            directive = directive || line.TrimStart().StartsWith('#');
            result.Append(directive ? new string(' ', line.Length) : line);
            directive = directive && line.TrimEnd('\r').EndsWith('\\');
            if (end >= 0)
            {
                result.Append('\n');
            }

            start = stop + 1;
        }

        return result.ToString();
    }
}
