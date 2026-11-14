using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

/// <summary>
/// C# 서식 검사(<see cref="FormatCheck.CheckCSharpAsync"/>) 자체의 시험이다. 저장소의 실제 .editorconfig·global.json을 복사한
/// 임시 저장소에서 규칙마다 일부러 어긴 시료가 그 규칙 ID·파일·행으로 실패하는지, 지키는 시료는 통과하고 바뀌지 않는지,
/// apply(--write)가 고치는지, 어느 프로젝트에도 속하지 않는 .cs를 알리는지 확인한다. dotnet format을 실제로 실행하므로(복원과
/// 작업 영역 적재) 시험마다 몇 초씩 걸린다.
/// </summary>
internal static class FormatCheckTests
{
    private const string SampleDirectory = "Programs/Sample";

    private const string SampleProject = """
        <Project Sdk="Microsoft.NET.Sdk">
          <PropertyGroup>
            <TargetFramework>net10.0</TargetFramework>
            <Nullable>enable</Nullable>
            <ImplicitUsings>enable</ImplicitUsings>
          </PropertyGroup>
        </Project>

        """;

    private const string Clean = """
        namespace Sample;

        public sealed class Clean
        {
            private readonly int seed;

            public Clean(int seed)
            {
                this.seed = seed;
            }

            public int Value { get; init; }

            public int Run(int value)
            {
                if (value > seed)
                {
                    return 1;
                }
                else if (value < 0)
                {
                    return -1;
                }

                switch (value)
                {
                    case 0:
                        return 0;
                    default:
                        break;
                }

                Clean copy = new(seed)
                {
                    Value = value
                };
                return copy.Value;
            }
        }

        """;

    // 규칙마다 하나씩 어긴 시료다. Line이 0이면 행은 보지 않는다(IDE2001은 공백 서식을 먼저 고친 문서에서 위치를 잡는다).
    private static readonly (string File, string Rule, int Line, string Text)[] Violations =
    [
        ("Braces.cs", "IDE0011", 7, """
            namespace Sample;

            public static class Braces
            {
                public static int Run(int value)
                {
                    if (value > 0)
                        return 1;

                    return 0;
                }
            }

            """),
        ("BlankLines.cs", "IDE2000", 8, """
            namespace Sample;

            public static class BlankLines
            {
                public static int Run(int value)
                {
                    int result = value;


                    return result;
                }
            }

            """),
        ("Embedded.cs", "IDE2001", 0, """
            namespace Sample;

            public static class Embedded
            {
                public static int Run(int value)
                {
                    if (value > 0) { return 1; }

                    return 0;
                }
            }

            """),
        ("ConsecutiveBraces.cs", "IDE2002", 14, """
            namespace Sample;

            public static class ConsecutiveBraces
            {
                public static int Run(int value)
                {
                    if (value > 0)
                    {
                        if (value > 1)
                        {
                            return 2;
                        }

                    }

                    return 0;
                }
            }

            """),
        ("AfterBlock.cs", "IDE2003", 10, """
            namespace Sample;

            public static class AfterBlock
            {
                public static int Run(int value)
                {
                    if (value > 0)
                    {
                        value = 1;
                    }
                    return value;
                }
            }

            """),
        ("Allman.cs", "WHITESPACE", 3, """
            namespace Sample;

            public static class Allman {
                public static int Run(int value)
                {
                    return value;
                }
            }

            """),
        ("Statements.cs", "WHITESPACE", 7, """
            namespace Sample;

            public static class Statements
            {
                public static int Run(int value)
                {
                    int first = value; int second = first;
                    return second;
                }
            }

            """)
    ];

    public static void Register(TestSuite suite, Options options)
    {
        string dotnet = options.Optional("dotnet") ?? "dotnet";
        suite.Add("Format.CSharpRuleViolationsFailWithLocationAndWriteFixesThem", async () =>
        {
            using TemporaryDirectory temporary = new();
            WriteWorkspace(temporary, options.Root, $"../../{SampleDirectory}/Sample.csproj");
            temporary.Write($"{SampleDirectory}/Sample.csproj", SampleProject);
            temporary.Write($"{SampleDirectory}/Clean.cs", Clean);
            foreach (var violation in Violations)
            {
                temporary.Write($"{SampleDirectory}/{violation.File}", violation.Text);
            }

            Dictionary<string, byte[]> before = Sources(temporary);
            IReadOnlyList<CheckFailure> failures = await FormatCheck.CheckCSharpAsync(temporary.Root, dotnet);
            string report = string.Join('\n', failures);
            foreach (var violation in Violations)
            {
                string path = $"{SampleDirectory}/{violation.File}";
                Verify.That(failures.Any(failure => failure.Path == path && failure.Rule == violation.Rule &&
                    (violation.Line == 0 ? failure.Line > 0 : failure.Line == violation.Line)),
                    $"{violation.Rule} violation in {violation.File} (line {violation.Line}) was not reported:\n{report}");
            }

            Verify.That(failures.All(failure => failure.Path != $"{SampleDirectory}/Clean.cs"), "compliant sample was reported:\n" + report);
            Verify.That(failures.All(failure => failure.Rule is not ("csharp-project" or "dotnet-format")),
                "sample workspace was not checked cleanly:\n" + report);
            Verify.That(SameSources(before, Sources(temporary)), "verification changed sources");

            // apply는 자동 수정 뒤 다시 검증한다. 시료의 위반은 모두 dotnet format이 고칠 수 있다.
            failures = await FormatCheck.CheckCSharpAsync(temporary.Root, dotnet, apply: true);
            Verify.That(failures.Count == 0, "violations left after apply:\n" + string.Join('\n', failures));
            Dictionary<string, byte[]> after = Sources(temporary);
            Verify.That(before[$"{SampleDirectory}/Clean.cs"].AsSpan().SequenceEqual(after[$"{SampleDirectory}/Clean.cs"]),
                "apply changed the compliant sample");
            foreach (var violation in Violations)
            {
                string path = $"{SampleDirectory}/{violation.File}";
                Verify.That(!before[path].AsSpan().SequenceEqual(after[path]), $"apply left {violation.File} unchanged");
            }

            Verify.Contains("if (value > 0)\n        {\n            return 1;\n        }\n",
                File.ReadAllText(temporary.FilePath($"{SampleDirectory}/Braces.cs")), "IDE0011 fix did not add braces");
        });
        suite.Add("Format.CSharpSourcesOutsideWorkspaceProjectsFail", async () =>
        {
            using TemporaryDirectory temporary = new();
            WriteWorkspace(temporary, options.Root, $"../../{SampleDirectory}/Sample.csproj");
            temporary.Write($"{SampleDirectory}/Sample.csproj", SampleProject);
            temporary.Write($"{SampleDirectory}/Clean.cs", Clean);
            NoFailures(await FormatCheck.CheckCSharpProjectsAsync(temporary.Root, dotnet));

            // 빌드 출력(bin·obj)과 외부 라이브러리 원본의 .cs는 저장소 소스가 아니다. 원본 옆 Rocks 규칙(<Lib>.Build.cs)과 게임
            // 템플릿 규칙은 저장소 소스이므로, 어느 프로젝트에도 넣지 않으면 알려야 한다.
            temporary.Write($"{SampleDirectory}/obj/Generated.cs", "// generated\n");
            temporary.Write($"{SampleDirectory}/bin/Copied.cs", "// copied\n");
            temporary.Write($"{ThirdPartyPolicyTests.Upstream}/contrib/Upstream.cs", "// upstream\n");
            ThirdPartyPolicyTests.WriteFixture(temporary);
            temporary.Write("Templates/Game/Sample/Game.Build.cs", "// template rule\n");
            IReadOnlyList<CheckFailure> failures = await FormatCheck.CheckCSharpProjectsAsync(temporary.Root, dotnet);
            string[] expected = [$"{ThirdPartyPolicyTests.Library}/Fake.Build.cs", "Templates/Game/Sample/Game.Build.cs"];
            Verify.That(failures.Select(failure => failure.Path).SequenceEqual(expected) &&
                failures.All(failure => failure.Rule == "csharp-project" && failure.Message.Contains(FormatCheck.CSharpWorkspace, StringComparison.Ordinal)),
                "sources outside workspace projects were not reported exactly:\n" + string.Join('\n', failures));

            // 규칙 프로젝트처럼 glob으로 모으는 프로젝트를 작업 영역에 넣으면 통과한다.
            temporary.Write("Build/Format/Rules.csproj", """
                <Project Sdk="Microsoft.NET.Sdk">
                  <PropertyGroup>
                    <TargetFramework>net10.0</TargetFramework>
                    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
                  </PropertyGroup>
                  <ItemGroup>
                    <Compile Include="../../Core/Source/**/*.Build.cs;../../Templates/**/*.Build.cs" />
                  </ItemGroup>
                </Project>

                """);
            WriteWorkspace(temporary, options.Root, $"../../{SampleDirectory}/Sample.csproj", "Rules.csproj");
            NoFailures(await FormatCheck.CheckCSharpProjectsAsync(temporary.Root, dotnet));
        });
        suite.Add("Format.RepositoryCSharpSourcesBelongToWorkspace", async () =>
            NoFailures(await FormatCheck.CheckCSharpProjectsAsync(options.Root, dotnet)));
    }

    // 임시 저장소에 저장소의 서식 규칙(.editorconfig)과 SDK 고정(global.json)을 복사하고, 주어진 프로젝트(작업 영역 기준 상대
    // 경로)로 서식 검사 작업 영역을 쓴다. 빈 Directory.Build.props·targets는 임시 디렉터리 위쪽의 MSBuild 설정을 막는다.
    private static void WriteWorkspace(TemporaryDirectory temporary, string repository, params string[] projects)
    {
        File.Copy(Path.Combine(repository, ".editorconfig"), temporary.FilePath(".editorconfig"), overwrite: true);
        File.Copy(Path.Combine(repository, "global.json"), temporary.FilePath("global.json"), overwrite: true);
        temporary.Write("Directory.Build.props", "<Project />\n");
        temporary.Write("Directory.Build.targets", "<Project />\n");
        string entries = string.Concat(projects.Select(project => $"  <Project Path=\"{project}\" />\n"));
        temporary.Write(FormatCheck.CSharpWorkspace, $"<Solution>\n{entries}</Solution>\n");
    }

    private static Dictionary<string, byte[]> Sources(TemporaryDirectory temporary) =>
        Directory.EnumerateFiles(temporary.FilePath(SampleDirectory), "*.cs")
            .ToDictionary(path => $"{SampleDirectory}/{Path.GetFileName(path)}", File.ReadAllBytes, StringComparer.Ordinal);

    private static bool SameSources(Dictionary<string, byte[]> left, Dictionary<string, byte[]> right) =>
        left.Count == right.Count && left.All(pair => right.TryGetValue(pair.Key, out byte[]? bytes) && pair.Value.AsSpan().SequenceEqual(bytes));

    private static void NoFailures(IReadOnlyList<CheckFailure> failures) => Verify.That(failures.Count == 0, string.Join('\n', failures));
}
