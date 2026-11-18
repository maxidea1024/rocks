using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // CVar 핸들 정의(CVar<T>·BootCVar<T>·CVarGroup·ConsoleCommand 이름 = ...)다. 템플릿 인자는 한 겹 중첩까지 받는다
    // (CVar<Array<int32>>는 지원 타입이 아니므로 그보다 깊을 일이 없다). 이름은 한정 이름(Owner::Name)일 수 있다.
    private static readonly Regex CVarHandleDefinition = Pattern(
        "\\b(?:(?:CVar|BootCVar)\\s*<(?:[^<>;{}()]|<[^<>;{}()]*>)*>|CVarGroup|ConsoleCommand)\\s+" +
        "((?:[A-Za-z_]\\w*\\s*::\\s*)*[A-Za-z_]\\w*)\\s*=(?!=)");

    private static readonly Regex RegisterCall = Pattern("\\bRegister\\s*\\(");

    private static readonly Regex NamespaceHeader = Pattern("(?:^|[\\s;])(?:inline\\s+)?namespace\\b[^()]*$|\\bextern\\s*$");

    private static readonly Regex ClassHeader = Pattern("\\b(?:class|struct|union)\\s+[A-Za-z_]\\w*[^(){};=]*$");

    private static readonly Regex DefineDirective = Pattern("^\\s*#\\s*define\\b");

    private const string CVarModuleDirectory = "Core/Source/Runtime/Config/";

    private enum BraceScope
    {
        Namespace,
        Class,
        Other
    }

    /// <summary>
    /// CVar 핸들 등록 누락과 정의 위치를 검사한다(rule id cvar-registration, Docs/Design/CVarSystem.ko.md 5절 VD1).
    /// <list type="bullet">
    /// <item>시험이 아닌 엔진 소스(Core·Engine·Programs)의 namespace 범위·클래스 범위 핸들 정의(<c>CVar&lt;T&gt;</c>·
    /// <c>BootCVar&lt;T&gt;</c>·<c>CVarGroup</c>·<c>ConsoleCommand</c> <c>이름 = ...</c>) 이름이 같은 모듈(가장 가까운
    /// <c>*.Build.cs</c>가 있는 디렉터리)의 어느 <c>Register(...)</c> 인자에 나와야 한다. 한정 이름은 마지막 마디로 맞춘다.</item>
    /// <item>함수 본문 안의 핸들 정의는 거부한다(핸들은 정적 저장 기간이고 모듈 등록 함수가 올린다 — 함수 안 지역 핸들은
    /// Register가 실행 중에도 거부한다).</item>
    /// <item>CVar 구현 소스(<c>Core/Source/Runtime/Config/</c>의 <c>CVar*</c> 파일)는 매크로를 정의하지 않는다(rule id
    /// cvar-macro — CVar 시스템은 새 매크로가 없다, 2·19절).</item>
    /// </list>
    /// 주석·문자열 안의 철자는 보지 않는다. 시험 소스는 등록하지 않는 핸들·지역 핸들을 일부러 쓰므로 대상이 아니다. 전처리·이름
    /// 해석을 하지 않는 lexical 검사이므로 같은 모듈의 다른 문맥에서 이름이 같은 인자도 등록으로 센다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> CVarRegistrations(string root)
    {
        List<CheckFailure> failures = [];
        Dictionary<string, HashSet<string>> registered = new(StringComparer.Ordinal);
        List<(string Module, string Relative, int Line, string Name)> definitions = [];
        Dictionary<string, string> modules = new(StringComparer.Ordinal);
        foreach (string path in NativeSources(root, RuntimeRoots))
        {
            string relative = Relative(root, path);
            string original = File.ReadAllText(path, StrictUtf8);
            if (relative.StartsWith(CVarModuleDirectory, StringComparison.Ordinal) &&
                Path.GetFileName(relative).StartsWith("CVar", StringComparison.Ordinal))
            {
                string[] lines = WithoutComments(original).Split('\n');
                for (int index = 0; index < lines.Length; ++index)
                {
                    if (DefineDirective.IsMatch(lines[index]))
                    {
                        failures.Add(new(relative, index + 1, "cvar-macro", "CVar sources define no macros"));
                    }
                }
            }

            if (IsTestSource(relative))
            {
                continue;
            }

            string code = WithoutCommentsAndLiterals(original);
            string module = ModuleOf(root, path, modules);
            if (!registered.TryGetValue(module, out HashSet<string>? names))
            {
                names = new HashSet<string>(StringComparer.Ordinal);
                registered.Add(module, names);
            }

            foreach (Match call in RegisterCall.Matches(code))
            {
                // MatchingParenthesis(PolicyChecks.CompileTimeMessages)는 짝 ')' 다음 위치를 돌려준다.
                int end = MatchingParenthesis(code, call.Index + call.Length - 1);
                foreach (Match name in Identifier.Matches(code[(call.Index + call.Length)..end]))
                {
                    names.Add(name.Value);
                }
            }

            BraceScope[] scopes = ScopesOf(code);
            foreach (Match match in CVarHandleDefinition.Matches(code))
            {
                int line = 1 + code.AsSpan(0, match.Index).Count('\n');
                if (scopes[match.Index] == BraceScope.Other)
                {
                    failures.Add(new(relative, line, "cvar-registration",
                        "CVar handle defined inside a function body; define it at namespace scope (or as a static member) and pass it to ConfigContext::Register"));
                    continue;
                }

                string qualified = match.Groups[1].Value;
                string name = qualified[(qualified.LastIndexOf(':') + 1)..].Trim();
                definitions.Add((module, relative, line, name));
            }
        }

        foreach (var definition in definitions)
        {
            if (!registered[definition.Module].Contains(definition.Name))
            {
                failures.Add(new(definition.Relative, definition.Line, "cvar-registration",
                    $"CVar handle '{definition.Name}' is not passed to ConfigContext::Register in its module"));
            }
        }

        return failures;
    }

    // 가장 가까운 *.Build.cs가 있는 디렉터리(모듈)다. 없으면 파일의 디렉터리다.
    private static string ModuleOf(string root, string path, Dictionary<string, string> cache)
    {
        string fullRoot = Path.GetFullPath(root);
        string? directory = Path.GetDirectoryName(Path.GetFullPath(path));
        string start = directory ?? fullRoot;
        List<string> visited = [];
        while (directory is not null && directory.Length >= fullRoot.Length)
        {
            if (cache.TryGetValue(directory, out string? known))
            {
                start = known;
                break;
            }

            visited.Add(directory);
            if (Directory.EnumerateFiles(directory, "*.Build.cs").Any())
            {
                start = directory;
                break;
            }

            directory = Path.GetDirectoryName(directory);
        }

        foreach (string seen in visited)
        {
            cache[seen] = start;
        }

        return start;
    }

    // 위치마다 가장 안쪽 중괄호 범위의 종류다(namespace·extern 블록과 그 바깥은 Namespace, 클래스 본문 — 그 안의 모든 바깥도
    // namespace·클래스 — 은 Class, 그 밖 함수 본문·초기화 목록·람다는 Other). 중괄호 앞 문장 머리(직전 ';'·'{'·'}' 뒤)로 판정한다.
    private static BraceScope[] ScopesOf(string code)
    {
        BraceScope[] scopes = new BraceScope[code.Length + 1];
        List<BraceScope> stack = [];
        BraceScope current = BraceScope.Namespace;
        int headerStart = 0;
        for (int index = 0; index < code.Length; ++index)
        {
            scopes[index] = current;
            char c = code[index];
            if (c == '{')
            {
                string header = code[headerStart..index];
                BraceScope opened = current == BraceScope.Other ? BraceScope.Other
                    : NamespaceHeader.IsMatch(header) && current == BraceScope.Namespace ? BraceScope.Namespace
                    : ClassHeader.IsMatch(header) ? BraceScope.Class
                    : BraceScope.Other;
                stack.Add(current);
                current = opened;
                headerStart = index + 1;
            }
            else if (c == '}')
            {
                if (stack.Count != 0)
                {
                    current = stack[^1];
                    stack.RemoveAt(stack.Count - 1);
                }

                headerStart = index + 1;
            }
            else if (c == ';')
            {
                headerStart = index + 1;
            }
        }

        scopes[code.Length] = current;
        return scopes;
    }
}
