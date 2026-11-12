using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    private const string DeterministicMathRoot = "Core/Source/Runtime/DeterministicMath";

    // Private 번역 단위가 include해도 되는 헤더다. PlatformMath·CRT 수학 헤더는 header inline 함수와 근사 명령(rsqrt 등)을
    // 끌어오므로 오브젝트의 외부 심볼 검사로는 보이지 않는다 — 그래서 소스에서 막는다.
    private static readonly HashSet<string> DeterministicMathPrivateIncludes = new(StringComparer.Ordinal)
    {
        "\"SrgbTables.generated.h\"", "<DeterministicMath/DeterministicMath.h>", "<Platform/Memory.h>", "<Base/Types.h>",
    };

    private static readonly Regex DeterministicMathInclude = Pattern("^\\s*#\\s*include\\s*([<\"][^>\"]+[>\"])");
    private static readonly Regex ExtendedPrecisionType = Pattern("\\blong\\s+double\\b");
    // 공개 헤더의 함수 본문(선언 뒤 '{'). 본문이 소비자 오브젝트에 inline되면 오브젝트 검사 범위 밖이 된다.
    private static readonly Regex FunctionBody = Pattern("\\)\\s*(?:const\\s*)?(?:noexcept\\s*)?\\{");

    // C runtime 실수 해석·scanf·locale·cvt 함수다(POSIX·MSVC·wide 변형 포함). 결과가 CRT 구현·locale에 따라 달라진다.
    private static readonly HashSet<string> CrtNumberFunctions = new(StringComparer.Ordinal)
    {
        "strtod", "strtof", "strtold", "strtod_l", "strtof_l", "strtold_l", "_strtod_l", "_strtof_l", "_strtold_l",
        "atof", "_atof_l", "_atodbl", "_atodbl_l", "_atoflt", "_atoflt_l", "_atoldbl",
        "wcstod", "wcstof", "wcstold", "wcstod_l", "_wcstod_l", "_wcstof_l", "_wcstold_l", "_wtof", "_wtof_l",
        "scanf", "scanf_s", "sscanf", "sscanf_s", "_sscanf_l", "_sscanf_s_l", "vsscanf", "vsscanf_s", "fscanf",
        "fscanf_s", "vfscanf", "vscanf", "swscanf", "swscanf_s", "vswscanf", "_snscanf", "_snscanf_s", "_snwscanf",
        "localeconv", "setlocale", "_wsetlocale", "_create_locale", "_wcreate_locale", "_free_locale", "newlocale",
        "uselocale", "duplocale", "freelocale", "_configthreadlocale",
        "gcvt", "ecvt", "fcvt", "_gcvt", "_ecvt", "_fcvt", "_gcvt_s", "_ecvt_s", "_fcvt_s",
    };

    // printf 계열: 같은 줄 문자열 리터럴에 실수 변환(%f·%e·%g·%a)이 있을 때만 위반이다(정수·문자열 서식은 결정적이다).
    private static readonly HashSet<string> PrintfFunctions = new(StringComparer.Ordinal)
    {
        "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf", "vsprintf", "vsnprintf", "_snprintf",
        "_snprintf_s", "sprintf_s", "vsprintf_s", "vsnprintf_s", "_vsnprintf", "swprintf", "vswprintf", "_sprintf_l",
        "_snprintf_l", "_vsnprintf_l",
    };

    private static readonly Regex FloatConversion = Pattern("%[-+ #0]*(?:\\*|[0-9]+)?(?:\\.(?:\\*|[0-9]+))?[hlLqjzt]*[aAeEfFgG]");
    private static readonly Regex Identifier = Pattern("[A-Za-z_][A-Za-z0-9_]*");

    /// <summary>
    /// 엔진·Core의 런타임과 개발 도구(쿠킹 산출물의 결정성, 시험 제외)는 C runtime 실수 해석·scanf·locale·cvt 함수와 실수
    /// 서식 printf를 쓰지 않는다(rule id crt-number-conversion, CoreDataFoundations.ko.md 3.2). 실수 해석·표기는 Core
    /// NumberText·StringBuilder(정수 연산만 쓰는 변환)를 쓴다. 주석·문자열 안의 이름과 멤버(.·->)·한정 이름(X::)은 대상이
    /// 아니다. 이름 자체를 보므로 함수 포인터(&amp;strtod)·여러 줄 호출도 잡는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> CrtNumberConversions(string root)
    {
        List<CheckFailure> failures = [];
        foreach (string path in NativeSources(root, ["Core/Source/Runtime", "Core/Source/Developer", "Engine/Source/Runtime",
                     "Engine/Source/Developer"]))
        {
            string relative = Relative(root, path);
            if (IsTestSource(relative))
                continue;
            var (code, literals) = StripCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            for (int index = 0; index < code.Length; ++index)
            {
                string line = code[index];
                foreach (Match match in Identifier.Matches(line))
                {
                    string name = match.Value;
                    bool crt = CrtNumberFunctions.Contains(name);
                    bool printf = PrintfFunctions.Contains(name) && literals[index].Any(literal => FloatConversion.IsMatch(literal));
                    if ((!crt && !printf) || IsMemberOrQualified(line, match.Index))
                        continue;
                    failures.Add(new(relative, index + 1, "crt-number-conversion", crt
                        ? $"C runtime number conversion or locale function '{name}' (use Core NumberText/StringBuilder)"
                        : $"floating-point printf format with '{name}' (use Core StringBuilder formatting)"));
                }
            }
        }
        return failures;
    }

    // 이름 앞이 멤버 접근(.·->)이거나 다른 이름으로 한정(X::name)되었는지 본다. 전역 한정(::name)은 CRT 이름이다.
    private static bool IsMemberOrQualified(string line, int start)
    {
        int index = start - 1;
        while (index >= 0 && char.IsWhiteSpace(line[index]))
            --index;
        if (index < 0)
            return false;
        if (line[index] == '.' || (line[index] == '>' && index > 0 && line[index - 1] == '-'))
            return true;
        if (line[index] == ':' && index > 0 && line[index - 1] == ':')
        {
            int before = index - 2;
            while (before >= 0 && char.IsWhiteSpace(line[before]))
                --before;
            return before >= 0 && (char.IsLetterOrDigit(line[before]) || line[before] == '_' || line[before] == '>');
        }
        return false;
    }

    // 주석을 공백으로, 문자열·문자 리터럴 내용을 비운 코드 줄과 줄마다의 문자열 리터럴 내용을 돌려준다(raw 문자열 포함).
    private static (string[] Code, List<string>[] Literals) StripCommentsAndLiterals(string text)
    {
        var codeLines = new List<string>();
        var literalLines = new List<List<string>>();
        var code = new System.Text.StringBuilder();
        var literals = new List<string>();
        var literal = new System.Text.StringBuilder();
        void EndLine()
        {
            codeLines.Add(code.ToString());
            literalLines.Add(literals);
            code.Clear();
            literals = [];
        }

        int i = 0;
        while (i < text.Length)
        {
            char c = text[i];
            if (c == '\n')
            {
                EndLine();
                ++i;
            }
            else if (c == '/' && i + 1 < text.Length && text[i + 1] == '/')
            {
                while (i < text.Length && text[i] != '\n')
                    ++i;
            }
            else if (c == '/' && i + 1 < text.Length && text[i + 1] == '*')
            {
                i += 2;
                while (i < text.Length && !(text[i] == '*' && i + 1 < text.Length && text[i + 1] == '/'))
                {
                    if (text[i] == '\n')
                        EndLine();
                    ++i;
                }
                i += 2;
                code.Append(' ');
            }
            else if (c == 'R' && i + 1 < text.Length && text[i + 1] == '"' && (i == 0 || !char.IsLetterOrDigit(text[i - 1])))
            {
                // R"delimiter(...)delimiter"
                int open = text.IndexOf('(', i + 2);
                string delimiter = open < 0 ? "" : text[(i + 2)..open];
                string close = ")" + delimiter + "\"";
                int end = open < 0 ? -1 : text.IndexOf(close, open + 1, StringComparison.Ordinal);
                if (end < 0)
                    end = text.Length - close.Length;
                literal.Clear();
                for (int k = open + 1; k < end; ++k)
                {
                    if (text[k] == '\n')
                    {
                        literals.Add(literal.ToString());
                        literal.Clear();
                        EndLine();
                    }
                    else
                        literal.Append(text[k]);
                }
                literals.Add(literal.ToString());
                code.Append("\"\"");
                i = end + close.Length;
            }
            else if (c == '"' || c == '\'')
            {
                char quote = c;
                literal.Clear();
                ++i;
                while (i < text.Length && text[i] != quote && text[i] != '\n')
                {
                    if (text[i] == '\\' && i + 1 < text.Length)
                    {
                        literal.Append(text[i]).Append(text[i + 1]);
                        i += 2;
                        continue;
                    }
                    literal.Append(text[i]);
                    ++i;
                }
                if (i < text.Length && text[i] == quote)
                    ++i;
                if (quote == '"')
                    literals.Add(literal.ToString());
                code.Append(quote).Append(quote);
            }
            else
            {
                code.Append(c);
                ++i;
            }
        }
        EndLine();
        return (codeLines.ToArray(), literalLines.ToArray());
    }

    /// <summary>
    /// 결정적 수학 모듈 소스 규칙(rule id deterministic-math-sources, CoreDataFoundations.ko.md 2.1·2.3):
    /// <list type="bullet">
    /// <item>Private은 허용 목록의 헤더만 include한다(생성 표, 자기 공개 헤더, Platform/Memory.h, Base/Types.h).</item>
    /// <item>공개 헤더는 Base/Types.h만 include하고 함수 본문을 두지 않는다(계산은 모두 검사되는 Private 오브젝트에).</item>
    /// <item>확장 정밀도 타입(long double)을 쓰지 않는다(x87·소프트웨어 128비트로 호스트마다 다르다).</item>
    /// </list>
    /// </summary>
    public static IReadOnlyList<CheckFailure> DeterministicMathSources(string root)
    {
        List<CheckFailure> failures = [];
        string moduleRoot = Path.Combine(Path.GetFullPath(root), DeterministicMathRoot);
        foreach (string folder in new[] { "Private", "Public" })
        {
            string directory = Path.Combine(moduleRoot, folder);
            if (!Directory.Exists(directory))
                continue;
            bool isPublic = folder == "Public";
            foreach (string path in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories)
                         .Where(file => file.EndsWith(".h", StringComparison.Ordinal) || file.EndsWith(".cpp", StringComparison.Ordinal))
                         .Order(StringComparer.Ordinal))
            {
                string relative = Relative(root, path);
                int line = 0;
                foreach (string text in File.ReadLines(path, StrictUtf8))
                {
                    ++line;
                    Match include = DeterministicMathInclude.Match(text);
                    if (include.Success && (isPublic ? include.Groups[1].Value != "<Base/Types.h>"
                                                     : !DeterministicMathPrivateIncludes.Contains(include.Groups[1].Value)))
                        failures.Add(new(relative, line, "deterministic-math-sources",
                            $"include {include.Groups[1].Value} is not allowed in deterministic math {folder} sources"));
                    if (ExtendedPrecisionType.IsMatch(text))
                        failures.Add(new(relative, line, "deterministic-math-sources",
                            "extended precision floating type is not deterministic across hosts"));
                    if (isPublic && FunctionBody.IsMatch(text))
                        failures.Add(new(relative, line, "deterministic-math-sources",
                            "deterministic math public headers must not define function bodies"));
                }
            }
        }
        return failures;
    }
}
