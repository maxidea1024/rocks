using System.Text;
using Rocks.Build;
using Rocks.Build.Rules;

namespace Rocks.Tooling.Checks;

/// <summary>
/// 유니티 묶음 충돌 정책이다(Docs/Design/UnityBuildPch.ko.md 9.2절). 묶음 구성은 작업 파일 집합과 상한에 따라 바뀌므로 검사는 모듈
/// 단위다: 유니티를 켠 모듈에서 묶일 수 있는 어느 두 <c>.cpp</c>가 같은 번역 단위에 들어가도 안전해야 한다.
/// <list type="bullet">
/// <item><c>unity-file-local-name</c>: 두 <c>.cpp</c>가 같은 이름공간에서 같은 이름을 익명 namespace나 namespace 범위
/// <c>static</c>·<c>constexpr</c>·<c>const</c>로 선언한다(인자 타입만 다른 overload도 같은 이름이다 — 묶음 안에서 다른 파일의 함수가
/// 조용히 골라질 수 있다).</item>
/// <item><c>unity-file-scope-using</c>: 파일 범위(전역) <c>using namespace</c>가 <c>rocks</c>와 그 하위 namespace가 아니다.</item>
/// <item><c>unity-source-macro</c>: <c>.cpp</c>에서 <c>#define</c>한 매크로를 그 파일 끝까지 <c>#undef</c>하지 않는다.</item>
/// <item><c>unity-pragma-balance</c>: <c>#pragma warning(push)</c>·<c>clang diagnostic push</c>·<c>pack(push)</c>·<c>push_macro</c>를 같은
/// 파일에서 pop하지 않는다.</item>
/// </list>
/// 묶지 않는 소스(C 소스, <c>UnityExclude</c>, 첫 <c>#include</c> 앞에서 <c>#define</c>하는 파일)는 대상이 아니다. 검사는 토큰 단위
/// 근사다: 주석·문자열·전처리 지시문을 처리하지만 컴파일러 수준의 정확성은 없다(전처리 조건의 두 갈래를 모두 읽고, 매크로로 만든
/// 선언은 보지 못한다). 놓친 충돌은 드러나는 경우 컴파일 오류로 남는다.
/// </summary>
public static partial class PolicyChecks
{
    // 검사하는 대상 플랫폼과 아키텍처다(모듈 소스 목록이 플랫폼마다 다르다 — 같은 플랫폼에서 함께 컴파일되는 소스끼리만 본다).
    private static readonly (NativePlatform Platform, NativeArchitecture Architecture)[] UnityPlatforms =
    [
        (NativePlatform.Windows, NativeArchitecture.X64), (NativePlatform.MacOS, NativeArchitecture.Arm64),
        (NativePlatform.Android, NativeArchitecture.Arm64)
    ];

    /// <summary>
    /// 저장소 규칙을 평가해 유니티를 켠 모듈마다 검사한다. 사용 예:
    /// <c>PolicyChecks.UnityCollisions(root, ReleaseExclusion.DefaultRulesAssembly(root))</c>.
    /// </summary>
    /// <exception cref="FileNotFoundException">규칙 어셈블리가 없다(먼저 <c>dotnet build Programs/Rocks.Tools.csproj</c>).</exception>
    public static IReadOnlyList<CheckFailure> UnityCollisions(string root, string rulesAssembly, Func<NativeModuleInput, bool>? enabled = null)
    {
        root = Path.GetFullPath(root);
        if (!File.Exists(rulesAssembly))
        {
            throw new FileNotFoundException($"rules assembly {rulesAssembly} is missing; build Programs/Rocks.Tools.csproj first", rulesAssembly);
        }

        var failures = new List<CheckFailure>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach ((NativePlatform platform, NativeArchitecture architecture) in UnityPlatforms)
        {
            var context = new BuildContext(root, Path.Combine(root, "out", "policy-unity"), platform, architecture, NativeConfiguration.Debug);
            foreach (NativeModuleInput module in RuleLoader.LoadAll(rulesAssembly, context).Modules)
            {
                if (!(enabled ?? UnityRules.ModuleEnabled)(module))
                {
                    continue;
                }

                var excluded = module.UnityExclude.Select(path => Path.GetFullPath(path, root)).ToHashSet(StringComparer.OrdinalIgnoreCase);
                string[] sources = module.Sources.Select(path => Path.GetFullPath(path, root)).Where(path => !excluded.Contains(path)).ToArray();
                foreach (CheckFailure failure in UnityModuleCollisions(root, module.Name, sources))
                {
                    if (seen.Add(failure.ToString()))
                    {
                        failures.Add(failure);
                    }
                }
            }
        }

        return failures;
    }

    /// <summary>
    /// 모듈 하나의 소스(절대 경로)를 검사한다. C 소스와 첫 <c>#include</c> 앞에서 <c>#define</c>하는 파일은 묶이지 않으므로 건너뛴다.
    /// 규칙 평가와 떨어진 함수다(양성·음성 대조 시험이 직접 부른다).
    /// </summary>
    public static IReadOnlyList<CheckFailure> UnityModuleCollisions(string root, string module, IReadOnlyList<string> sources)
    {
        var failures = new List<CheckFailure>();
        var names = new Dictionary<string, (string Path, int Line)>(StringComparer.Ordinal);
        foreach (string source in sources.Order(StringComparer.Ordinal))
        {
            if (!UnitySourceExtensions.Contains(Path.GetExtension(source)) || !File.Exists(source))
            {
                continue;
            }

            string text = File.ReadAllText(source, StrictUtf8);
            if (UnityRules.DefinesBeforeFirstInclude(text))
            {
                continue;
            }

            string relative = Relative(root, source);
            List<UnityToken> tokens = UnityTokens(text);
            UnityDirectiveChecks(relative, tokens, failures);
            foreach ((string name, int line) in UnityFileLocalNames(relative, tokens, failures))
            {
                if (names.TryGetValue(name, out var first))
                {
                    if (first.Path != relative)
                    {
                        failures.Add(new(relative, line, "unity-file-local-name",
                            $"module {module}: file-local name '{name}' is also declared in {first.Path}:{first.Line}; in a unity batch the two " +
                            "declarations collide or silently join one overload set — rename one or move it into a namespace unique to its file"));
                    }
                }
                else
                {
                    names.Add(name, (relative, line));
                }
            }
        }

        return failures;
    }

    // 묶음 대상이 되는 C++ 소스 확장자다(Rocks.Build는 .c만 C로 본다).
    private static readonly HashSet<string> UnitySourceExtensions = new(StringComparer.OrdinalIgnoreCase) { ".cpp", ".cc", ".cxx", ".mm" };

    private enum UnityTokenKind
    {
        Identifier,
        Number,
        Literal,
        Punctuation,
        Directive
    }

    // 토큰 하나다. Directive는 전처리 지시문 한 줄 전체(줄 이음 포함)이고 Text는 '#' 뒤의 내용이다.
    private readonly record struct UnityToken(UnityTokenKind Kind, string Text, int Line);

    private static List<UnityToken> UnityTokens(string text)
    {
        var tokens = new List<UnityToken>();
        int line = 1;
        bool lineStart = true;
        int index = 0;
        while (index < text.Length)
        {
            char value = text[index];
            if (value == '\n')
            {
                ++line;
                lineStart = true;
                ++index;
            }
            else if (char.IsWhiteSpace(value))
            {
                ++index;
            }
            else if (value == '/' && index + 1 < text.Length && text[index + 1] == '/')
            {
                while (index < text.Length && text[index] != '\n')
                {
                    ++index;
                }
            }
            else if (value == '/' && index + 1 < text.Length && text[index + 1] == '*')
            {
                int end = text.IndexOf("*/", index + 2, StringComparison.Ordinal);
                end = end < 0 ? text.Length : end + 2;
                line += text.AsSpan(index, end - index).Count('\n');
                index = end;
            }
            else if (value == '#' && lineStart)
            {
                int start = index + 1;
                int startLine = line;
                var content = new StringBuilder();
                index = start;
                while (index < text.Length && text[index] != '\n')
                {
                    if (text[index] == '\\' && index + 1 < text.Length && (text[index + 1] == '\n' ||
                            (text[index + 1] == '\r' && index + 2 < text.Length && text[index + 2] == '\n')))
                    {
                        index += text[index + 1] == '\n' ? 2 : 3;
                        ++line;
                        content.Append(' ');
                        continue;
                    }

                    if (text[index] == '/' && index + 1 < text.Length && text[index + 1] == '/')
                    {
                        while (index < text.Length && text[index] != '\n')
                        {
                            ++index;
                        }

                        break;
                    }

                    if (text[index] == '/' && index + 1 < text.Length && text[index + 1] == '*')
                    {
                        int end = text.IndexOf("*/", index + 2, StringComparison.Ordinal);
                        end = end < 0 ? text.Length : end + 2;
                        line += text.AsSpan(index, end - index).Count('\n');
                        index = end;
                        content.Append(' ');
                        continue;
                    }

                    content.Append(text[index]);
                    ++index;
                }

                tokens.Add(new(UnityTokenKind.Directive, content.ToString().Trim(), startLine));
            }
            else if (value == 'R' && index + 1 < text.Length && text[index + 1] == '"')
            {
                int open = text.IndexOf('(', index + 2);
                string close = open < 0 ? "\"" : ")" + text[(index + 2)..open] + "\"";
                int end = open < 0 ? -1 : text.IndexOf(close, open + 1, StringComparison.Ordinal);
                end = end < 0 ? text.Length : end + close.Length;
                tokens.Add(new(UnityTokenKind.Literal, "\"\"", line));
                line += text.AsSpan(index, end - index).Count('\n');
                index = end;
                lineStart = false;
            }
            else if (value == '"' || (value == '\'' && (tokens.Count == 0 || tokens[^1].Kind != UnityTokenKind.Number || index == 0 ||
                         !char.IsAsciiLetterOrDigit(text[index - 1]))))
            {
                int end = index + 1;
                while (end < text.Length && text[end] != value && text[end] != '\n')
                {
                    end += text[end] == '\\' ? 2 : 1;
                }

                tokens.Add(new(UnityTokenKind.Literal, value == '"' ? "\"\"" : "''", line));
                index = Math.Min(end + 1, text.Length);
                lineStart = false;
            }
            else if (char.IsAsciiLetter(value) || value == '_')
            {
                int end = index;
                while (end < text.Length && (char.IsAsciiLetterOrDigit(text[end]) || text[end] == '_'))
                {
                    ++end;
                }

                tokens.Add(new(UnityTokenKind.Identifier, text[index..end], line));
                index = end;
                lineStart = false;
            }
            else if (char.IsAsciiDigit(value))
            {
                int end = index;
                while (end < text.Length && (char.IsAsciiLetterOrDigit(text[end]) || text[end] is '.' or '\'' or '_'))
                {
                    ++end;
                }

                tokens.Add(new(UnityTokenKind.Number, text[index..end], line));
                index = end;
                lineStart = false;
            }
            else
            {
                // '::'만 한 토큰으로 묶는다. 나머지 구두점은 한 글자씩이다('>>'도 '>' 둘 — 템플릿 인자 목록의 짝을 센다).
                bool scope = value == ':' && index + 1 < text.Length && text[index + 1] == ':';
                tokens.Add(new(UnityTokenKind.Punctuation, scope ? "::" : value.ToString(), line));
                index += scope ? 2 : 1;
                lineStart = false;
            }
        }

        return tokens;
    }

    // 매크로(#define 뒤 #undef)와 pragma push/pop 균형이다.
    private static void UnityDirectiveChecks(string relative, List<UnityToken> tokens, List<CheckFailure> failures)
    {
        var defined = new Dictionary<string, int>(StringComparer.Ordinal);
        var pushes = new Dictionary<string, Stack<int>>(StringComparer.Ordinal);
        foreach (UnityToken token in tokens.Where(token => token.Kind == UnityTokenKind.Directive))
        {
            string[] words = token.Text.Split([' ', '\t', '(', ')', ','], StringSplitOptions.RemoveEmptyEntries);
            if (words.Length >= 2 && words[0] == "define")
            {
                defined[words[1]] = token.Line;
            }
            else if (words.Length >= 2 && words[0] == "undef")
            {
                defined.Remove(words[1]);
            }
            else if (words.Length >= 2 && words[0] == "pragma")
            {
                // 종류: warning, pack, push_macro/pop_macro, clang·GCC diagnostic.
                (string? kind, bool? push) = words switch
                {
                    [_, "warning", "push", ..] => ("warning", true),
                    [_, "warning", "pop", ..] => ("warning", false),
                    [_, "pack", "push", ..] => ("pack", true),
                    [_, "pack", "pop", ..] => ("pack", false),
                    [_, "push_macro", ..] => ("push_macro", true),
                    [_, "pop_macro", ..] => ("push_macro", false),
                    [_, "clang" or "GCC", "diagnostic", "push", ..] => ("diagnostic", true),
                    [_, "clang" or "GCC", "diagnostic", "pop", ..] => ("diagnostic", false),
                    _ => ((string?)null, (bool?)null)
                };
                if (kind is null)
                {
                    continue;
                }

                if (!pushes.TryGetValue(kind, out Stack<int>? stack))
                {
                    pushes[kind] = stack = new Stack<int>();
                }

                if (push == true)
                {
                    stack.Push(token.Line);
                }
                else if (stack.Count == 0)
                {
                    failures.Add(new(relative, token.Line, "unity-pragma-balance",
                        $"#pragma {kind} pop without a push in this file; in a unity batch it pops the state of another source"));
                }
                else
                {
                    stack.Pop();
                }
            }
        }

        foreach ((string name, int line) in defined.OrderBy(pair => pair.Value))
        {
            failures.Add(new(relative, line, "unity-source-macro",
                $"macro {name} is defined in a .cpp and never #undef'ed; in a unity batch it leaks into the sources after this one — add #undef {name} " +
                "at the end of its use (or make it a constant or a function)"));
        }

        foreach ((string kind, Stack<int> stack) in pushes.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            foreach (int line in stack.Reverse())
            {
                failures.Add(new(relative, line, "unity-pragma-balance",
                    $"#pragma {kind} push is not popped in this file; in a unity batch the state leaks into the sources after this one"));
            }
        }
    }

    // 선언 지정자와 형식 키워드다(이름이 아니다).
    private static readonly HashSet<string> UnityNonNames = new(StringComparer.Ordinal)
    {
        "static", "constexpr", "constinit", "consteval", "const", "inline", "extern", "thread_local", "volatile", "mutable", "typename",
        "void", "bool", "char", "char8_t", "char16_t", "char32_t", "wchar_t", "short", "int", "long", "signed", "unsigned", "float", "double", "auto",
        "struct", "class", "union", "enum", "decltype", "alignas", "noexcept", "requires", "operator", "template", "typedef", "using", "namespace",
        "static_assert", "friend", "explicit", "virtual", "final", "override", "nullptr", "true", "false", "sizeof", "alignof", "return", "if",
        "__declspec", "__attribute__", "__cdecl", "__stdcall", "__vectorcall", "__forceinline"
    };

    // 괄호 묶음을 인자로 받는 지정자다(이름 앞 '('로 보지 않고 묶음째 건너뛴다).
    private static readonly HashSet<string> UnityParenthesizedSpecifiers = new(StringComparer.Ordinal)
    {
        "decltype", "alignas", "noexcept", "requires", "__declspec", "__attribute__", "static_assert"
    };

    // namespace 범위의 파일 지역 선언 이름(이름공간 경로 포함)과 줄이다. 전역 using namespace 위반도 여기서 낸다.
    private static List<(string Name, int Line)> UnityFileLocalNames(string relative, List<UnityToken> all, List<CheckFailure> failures)
    {
        List<UnityToken> tokens = all.Where(token => token.Kind != UnityTokenKind.Directive).ToList();
        var result = new List<(string Name, int Line)>();
        // 범위 스택: namespace면 이름(익명은 빈 문자열, extern "C" 블록은 null이 아닌 투명 표지 "\"C\""), 그 밖(클래스·함수 본문)은 null이다.
        var scopes = new List<string?>();
        const string Transparent = "\"";
        bool AtNamespaceScope() => scopes.All(scope => scope is not null);
        int index = 0;
        while (index < tokens.Count)
        {
            UnityToken token = tokens[index];
            if (!AtNamespaceScope())
            {
                if (token.Text == "{")
                {
                    scopes.Add(null);
                }
                else if (token.Text == "}" && scopes.Count != 0)
                {
                    scopes.RemoveAt(scopes.Count - 1);
                }

                ++index;
                continue;
            }

            if (token.Text == "}")
            {
                if (scopes.Count != 0)
                {
                    scopes.RemoveAt(scopes.Count - 1);
                }

                ++index;
                continue;
            }

            if (token.Text == ";")
            {
                ++index;
                continue;
            }

            // namespace [이름[::이름…]] { — 또는 inline namespace. namespace 별칭(namespace A = B;)은 선언으로 넘긴다.
            int cursor = index;
            if (tokens[cursor].Text == "inline" && cursor + 1 < tokens.Count && tokens[cursor + 1].Text == "namespace")
            {
                ++cursor;
            }

            if (tokens[cursor].Text == "namespace")
            {
                int name = cursor + 1;
                var parts = new List<string>();
                while (name < tokens.Count && (tokens[name].Kind == UnityTokenKind.Identifier || tokens[name].Text == "::"))
                {
                    if (tokens[name].Kind == UnityTokenKind.Identifier && tokens[name].Text != "inline")
                    {
                        parts.Add(tokens[name].Text);
                    }

                    ++name;
                }

                if (name < tokens.Count && tokens[name].Text == "{")
                {
                    scopes.Add(string.Join("::", parts));
                    index = name + 1;
                    continue;
                }
            }

            if (token.Text == "extern" && index + 2 < tokens.Count && tokens[index + 1].Kind == UnityTokenKind.Literal && tokens[index + 2].Text == "{")
            {
                scopes.Add(Transparent);
                index += 3;
                continue;
            }

            // 선언 하나를 모은다: 깊이 0의 ';'까지, 또는 함수 정의의 본문 '}'까지. head는 첫 본문 '{' 앞의 토큰이다.
            var head = new List<UnityToken>();
            int depth = 0;
            bool body = false;
            bool functionLike = false;
            int end = index;
            while (end < tokens.Count)
            {
                string text = tokens[end].Text;
                if (text is "(" or "[")
                {
                    ++depth;
                }
                else if (text is ")" or "]")
                {
                    --depth;
                }
                else if (text == "{" && depth <= 0)
                {
                    if (!body)
                    {
                        // 머리가 없는 중괄호 묶음(앞 선언에서 남은 조각)도 함수 본문처럼 그 묶음에서 끝낸다.
                        functionLike = head.Count == 0 || UnityLooksLikeFunction(head);
                    }

                    int braces = 0;
                    do
                    {
                        braces += tokens[end].Text == "{" ? 1 : tokens[end].Text == "}" ? -1 : 0;
                        ++end;
                    }
                    while (end < tokens.Count && braces != 0);
                    body = true;
                    // 함수 정의는 본문에서 끝난다(뒤에 ';'가 없다). 단, 생성자 초기화 목록의 중괄호 초기화(: a{1}, b{2} { … }) 뒤에는
                    // ','나 본문 '{'가 이어진다. 클래스·enum 정의와 변수의 중괄호 초기화는 ';'까지 간다.
                    if (end >= tokens.Count || tokens[end].Text == ";" || (functionLike && tokens[end].Text is not ("," or "{")))
                    {
                        break;
                    }

                    continue;
                }
                else if (text == ";" && depth <= 0)
                {
                    break;
                }

                if (!body)
                {
                    head.Add(tokens[end]);
                }

                ++end;
            }

            index = end < tokens.Count && tokens[end].Text == ";" ? end + 1 : end;
            if (head.Count == 0)
            {
                continue;
            }

            if (head[0].Text == "using" && head.Count >= 3 && head[1].Text == "namespace")
            {
                string target = string.Concat(head.Skip(2).Select(part => part.Text));
                string normalized = target.TrimStart(':');
                // 파일 범위(전역 — extern "C" 블록 안 포함)의 지시문만 본다. namespace 안의 지시문은 그 namespace에 머문다.
                bool fileScope = scopes.All(scope => scope == Transparent);
                if (fileScope && normalized != "rocks" && !normalized.StartsWith("rocks::", StringComparison.Ordinal))
                {
                    failures.Add(new(relative, head[0].Line, "unity-file-scope-using",
                        $"file-scope 'using namespace {target}' changes name lookup of the sources after this one in a unity batch; only rocks and " +
                        "its nested namespaces are allowed at file scope — move the directive into a function or a namespace"));
                }

                continue;
            }

            if (UnityDeclaredName(head) is not (string declared, bool variable, int line))
            {
                continue;
            }

            bool anonymous = scopes.Any(scope => scope is { Length: 0 });
            var specifiers = head.TakeWhile(part => part.Kind != UnityTokenKind.Punctuation || part.Text is "[" or "]" or "<" or ">" or "," or "::")
                .Select(part => part.Text).ToHashSet(StringComparer.Ordinal);
            bool local = anonymous || specifiers.Contains("static") || specifiers.Contains("constexpr") ||
                (variable && specifiers.Contains("const") && !specifiers.Contains("extern") && !specifiers.Contains("inline"));
            if (local)
            {
                string path = string.Join("::", scopes.Where(scope => scope is { Length: > 0 } && scope != Transparent));
                result.Add((path.Length == 0 ? declared : path + "::" + declared, line));
            }
        }

        return result;
    }

    // 선언 머리가 함수 정의처럼 보이는가(클래스·enum 정의나 변수 초기화가 아니다): 깊이 0의 '('가 '=' 앞에 있고 클래스 키로 시작하지 않는다.
    private static bool UnityLooksLikeFunction(List<UnityToken> head)
    {
        List<UnityToken> tokens = UnityWithoutTemplateHeads(head);
        if (tokens.Count == 0 || tokens.Any(token => token.Text is "struct" or "class" or "union" or "enum") && !tokens.Any(token => token.Text == "("))
        {
            return false;
        }

        foreach (UnityToken token in tokens)
        {
            if (token.Text == "=")
            {
                return false;
            }

            if (token.Text == "(")
            {
                return true;
            }
        }

        return false;
    }

    // 앞의 template <…> 머리와 [[…]] 특성을 뗀다.
    private static List<UnityToken> UnityWithoutTemplateHeads(List<UnityToken> head)
    {
        int index = 0;
        while (index < head.Count)
        {
            if (head[index].Text == "template" && index + 1 < head.Count && head[index + 1].Text == "<")
            {
                int depth = 0;
                ++index;
                do
                {
                    depth += head[index].Text == "<" ? 1 : head[index].Text == ">" ? -1 : 0;
                    ++index;
                }
                while (index < head.Count && depth != 0);
            }
            else if (head[index].Text == "[" && index + 1 < head.Count && head[index + 1].Text == "[")
            {
                int depth = 0;
                do
                {
                    depth += head[index].Text == "[" ? 1 : head[index].Text == "]" ? -1 : 0;
                    ++index;
                }
                while (index < head.Count && depth != 0);
            }
            else
            {
                break;
            }
        }

        return head.GetRange(index, head.Count - index);
    }

    // 선언 머리가 새로 선언하는 이름이다(없거나 알 수 없으면 null). Variable은 함수 괄호가 없는 선언(변수·형식)이다.
    private static (string Name, bool Variable, int Line)? UnityDeclaredName(List<UnityToken> head)
    {
        List<UnityToken> tokens = UnityWithoutTemplateHeads(head);
        // extern "C" 선언 하나: 링크 지정을 떼고 본다.
        if (tokens.Count > 2 && tokens[0].Text == "extern" && tokens[1].Kind == UnityTokenKind.Literal)
        {
            tokens = tokens.GetRange(2, tokens.Count - 2);
        }

        if (tokens.Count == 0 || tokens[0].Text is "static_assert" or "friend")
        {
            return null;
        }

        if (tokens[0].Text == "using")
        {
            // using 이름 = …; 만 새 이름이다(using A::B; 는 기존 이름을 끌어온다).
            return tokens.Count >= 3 && tokens[1].Kind == UnityTokenKind.Identifier && tokens[2].Text == "=" ? (tokens[1].Text, true, tokens[1].Line) : null;
        }

        if (tokens[0].Text == "namespace")
        {
            return null;
        }

        if (tokens[0].Text == "typedef")
        {
            UnityToken last = tokens.LastOrDefault(token => token.Kind == UnityTokenKind.Identifier && !UnityNonNames.Contains(token.Text));
            return last.Text is null ? null : (last.Text, true, last.Line);
        }

        // 클래스 키 뒤의 이름: 정의나 전방 선언이면 그 이름이 선언된다(특수화 Name<…>와 정교한 형식 지정자 뒤의 선언자는 아래로 넘긴다).
        int key = tokens.FindIndex(token => token.Text is "struct" or "class" or "union" or "enum");
        if (key >= 0 && tokens.Take(key).All(token => UnityNonNames.Contains(token.Text)))
        {
            int name = key + 1;
            if (name < tokens.Count && tokens[key].Text == "enum" && tokens[name].Text is "class" or "struct")
            {
                ++name;
            }

            while (name < tokens.Count && (UnityParenthesizedSpecifiers.Contains(tokens[name].Text) || tokens[name].Text == "["))
            {
                name = UnitySkipGroup(tokens, name);
            }

            if (name < tokens.Count && tokens[name].Kind == UnityTokenKind.Identifier && !UnityNonNames.Contains(tokens[name].Text))
            {
                string? next = name + 1 < tokens.Count ? tokens[name + 1].Text : null;
                if (next is null or ":" or "final")
                {
                    return (tokens[name].Text, true, tokens[name].Line);
                }

                if (next == "<")
                {
                    return null;
                }
            }
            else if (name >= tokens.Count)
            {
                return null;
            }
        }

        // 함수·변수: 깊이 0의 첫 '(' 바로 앞 식별자가 함수 이름이고, '('가 없으면 '=' · '[' · 끝 바로 앞 식별자가 변수 이름이다.
        int angle = 0;
        for (int index = 0; index < tokens.Count; ++index)
        {
            string text = tokens[index].Text;
            if (UnityParenthesizedSpecifiers.Contains(text) && index + 1 < tokens.Count && tokens[index + 1].Text == "(")
            {
                index = UnitySkipGroup(tokens, index + 1) - 1;
                continue;
            }

            if (text == "<" && index > 0 && tokens[index - 1].Kind == UnityTokenKind.Identifier)
            {
                ++angle;
            }
            else if (text == ">" && angle > 0)
            {
                --angle;
            }
            else if (angle == 0 && text is "(" or "=" or "[")
            {
                if (text == "[" && index + 1 < tokens.Count && tokens[index + 1].Text == "[")
                {
                    index = UnitySkipGroup(tokens, index) - 1;
                    continue;
                }

                return UnityNameBefore(tokens, index, variable: text != "(");
            }
        }

        return UnityNameBefore(tokens, tokens.Count, variable: true);
    }

    private static (string Name, bool Variable, int Line)? UnityNameBefore(List<UnityToken> tokens, int index, bool variable)
    {
        if (index == 0 || tokens[index - 1].Kind != UnityTokenKind.Identifier || UnityNonNames.Contains(tokens[index - 1].Text) ||
            (index >= 2 && tokens[index - 2].Text == "::"))
        {
            return null;
        }

        // 형식 이름 하나뿐인 선언(매크로 호출 등)은 선언자가 없다: 이름 앞에 형식·지정자가 하나는 있어야 한다.
        return index >= 2 ? (tokens[index - 1].Text, variable, tokens[index - 1].Line) : null;
    }

    // tokens[index]에서 시작하는 괄호·대괄호 묶음(또는 지정자 뒤의 묶음)의 다음 위치다.
    private static int UnitySkipGroup(List<UnityToken> tokens, int index)
    {
        while (index < tokens.Count && tokens[index].Text is not ("(" or "["))
        {
            ++index;
        }

        int depth = 0;
        while (index < tokens.Count)
        {
            depth += tokens[index].Text is "(" or "[" ? 1 : tokens[index].Text is ")" or "]" ? -1 : 0;
            ++index;
            if (depth == 0)
            {
                break;
            }
        }

        return index;
    }
}
