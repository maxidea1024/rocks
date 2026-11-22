using System.Text.RegularExpressions;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    /// <summary>NodeGraph 모듈 폴더(엔진 저장소 기준)다.</summary>
    private const string NodeGraphModuleRoot = "Engine/Source/Runtime/NodeGraph";

    /// <summary>
    /// NodeGraph 모듈 소스(Public·Private, 시험 제외)가 include할 수 있는 헤더 접두다. Object 층 헤더(Object/...)는 넣지 않는다 —
    /// 생성 codec이 쓰는 ObjectRuntime 헤더는 생성 헤더(&lt;NodeGraph/*.generated.h&gt;)가 include하므로 손으로 쓴 소스에는 필요 없다.
    /// </summary>
    public static readonly IReadOnlyList<string> NodeGraphIncludePrefixes = ["Base/", "Core/", "Meta/", "Serialization/", "NodeGraph/"];

    /// <summary>NodeGraph.Build.cs가 둘 수 있는 의존 모듈이다(Docs/Design/NodeGraph.ko.md 11.1절).</summary>
    public static readonly IReadOnlyList<string> NodeGraphAllowedDependencies = ["ObjectRuntime", "CoreTypeBindings"];

    // Object 층의 객체·lease 타입 이름이다(원칙 1 — 객체를 만들거나 쥐지 않고 TypeLease를 잡지 않는다).
    private static readonly Regex NodeGraphObjectToken = Pattern("\\b(InstancedStruct|TypeLease|ObjectHandle|ObjectRegistry|ObjectRef|RootRef)\\b");

    private static readonly Regex NodeGraphInclude = Pattern("^\\s*#\\s*include\\s*<([^>]+)>");

    private static readonly Regex NodeGraphDependencyList = Pattern("\\b(?:PublicDependencies|PrivateDependencies|DynamicModules)\\s*=>");

    /// <summary>
    /// NodeGraph 모듈의 경계다(rule id nodegraph-boundary, Docs/Design/NodeGraph.ko.md 원칙 1·GD17·GT14):
    /// 의존(PublicDependencies·PrivateDependencies·DynamicModules, 여러 줄 목록 포함)은 ObjectRuntime·CoreTypeBindings뿐이고
    /// (도메인·UI·장치 모듈 의존 금지), 손으로 쓴 소스(시험 제외)는
    /// <see cref="NodeGraphIncludePrefixes"/> 밖의 헤더를 include하지 않으며 InstancedStruct·TypeLease·ObjectHandle·
    /// ObjectRegistry·ObjectRef·RootRef를 쓰지 않는다. 주석·문자열 안은 보지 않는다. 모듈 폴더가 없으면 검사하지 않는다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> NodeGraphBoundary(string root)
    {
        List<CheckFailure> failures = [];
        if (!Directory.Exists(Path.Combine(root, NodeGraphModuleRoot)))
        {
            return failures;
        }

        string rules = Path.Combine(root, NodeGraphModuleRoot, "NodeGraph.Build.cs");
        if (File.Exists(rules))
        {
            // 의존 목록(PublicDependencies·PrivateDependencies·DynamicModules)은 여러 줄일 수 있다. 목록 시작 줄부터 닫는
            // ']'가 있는 줄까지의 문자열 리터럴을 모듈 이름으로 본다. 주석 안 목록은 지운 코드에 남지 않으므로 시작으로
            // 잡히지 않는다.
            var (code, literals) = StripCommentsAndLiterals(File.ReadAllText(rules, StrictUtf8));
            bool inList = false;
            for (int index = 0; index < code.Length; ++index)
            {
                string line = code[index];
                if (!inList)
                {
                    Match start = NodeGraphDependencyList.Match(line);
                    if (!start.Success)
                    {
                        continue;
                    }

                    inList = true;
                    line = line[start.Index..];
                }

                foreach (string module in literals[index])
                {
                    if (!NodeGraphAllowedDependencies.Contains(module, StringComparer.Ordinal))
                    {
                        failures.Add(new(Relative(root, rules), index + 1, "nodegraph-boundary",
                            $"NodeGraph must not depend on '{module}'; allowed dependencies are {string.Join(", ", NodeGraphAllowedDependencies)}"));
                    }
                }

                if (line.Contains(']', StringComparison.Ordinal))
                {
                    inList = false;
                }
            }
        }

        foreach (string path in Files(root, [NodeGraphModuleRoot]))
        {
            string relative = Relative(root, path);
            string extension = Path.GetExtension(path).ToLowerInvariant();
            if (extension is not (".h" or ".cpp") || relative.StartsWith(NodeGraphModuleRoot + "/Tests/", StringComparison.Ordinal))
            {
                continue;
            }

            var (code, _) = StripCommentsAndLiterals(File.ReadAllText(path, StrictUtf8));
            for (int index = 0; index < code.Length; ++index)
            {
                Match include = NodeGraphInclude.Match(code[index]);
                if (include.Success && !NodeGraphIncludePrefixes.Any(prefix => include.Groups[1].Value.StartsWith(prefix, StringComparison.Ordinal)))
                {
                    failures.Add(new(relative, index + 1, "nodegraph-boundary",
                        $"NodeGraph source includes <{include.Groups[1].Value}>; allowed prefixes are {string.Join(", ", NodeGraphIncludePrefixes)}"));
                }

                Match token = NodeGraphObjectToken.Match(code[index]);
                if (token.Success)
                {
                    failures.Add(new(relative, index + 1, "nodegraph-boundary",
                        $"NodeGraph uses '{token.Groups[1].Value}'; the graph model never creates or holds objects, handles or type leases"));
                }
            }
        }

        return failures;
    }
}
