using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 정의 헤더·모듈 PCH·작업 파일 분리(Docs/Design/UnityBuildPch.ko.md 7·8절, UB4)를 검사한다.
/// <list type="bullet">
/// <item>정의 헤더: 모듈 정의가 명령줄 대신 <c>&lt;out&gt;/Definitions/&lt;모듈&gt;.Definitions.h</c>로 가고 강제 include된다. 값이 바뀌면
/// 그 정의를 받는 번역 단위가 다시 컴파일된다.</item>
/// <item>모듈 PCH: 생성 action과 출력(PCH, clang-cl의 stub object, 의존 기록), 소비 번역 단위의 인자와 의존, 논리 명령(PCH 바이너리 대신
/// 강제 include), 같은 크기의 헤더 변경으로 PCH와 소비자가 다시 빌드됨, 번역 단위 수 하한, <c>--pch off</c> 변형, PCH를 쓰지 않는 번역
/// 단위(C 소스, include 앞 <c>#define</c>), 유니티 묶음과 함께 쓰기, 낡은 변형 디렉터리 정리.</item>
/// <item>작업 파일 분리: porcelain 상태 fixture, 작업 파일과 짝 헤더 규칙, 분리한 파일만 다시 컴파일됨, 도구 빌드 ID 불변, 실제 git.</item>
/// </list>
/// </summary>
internal static class PchTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        await DefinitionsHeaderAsync(request, root);
        pass("definitions header: module definitions leave the command line, are force-included and keyed by content");

        await ModulePchAsync(request, root);
        pass("module PCH: creation and use, logical commands, same-size header change, minimum translation units, --pch off, exclusions, unity, stale variants");

        await AdaptiveAsync(request, root);
        pass("adaptive unity: porcelain fixtures, working file and paired header separation, incremental scope, tool build ID, real git");

        await SharedPchAsync(request, root);
        pass("shared PCH: provider selection, shared and extra definitions, variants, shadow and extra-definition checks, timing report");

        TimingAggregation();
        pass("time trace aggregation: Source begin/end pairs and durations, member sources excluded, template instantiations");

        // NDK가 있으면 clang++(GNU 드라이버) 경로를 같은 fixture로 교차 빌드한다: PCH를 -x c++-header로 만들고 -include-pch로 쓴다(macOS도
        // 이 경로다). 실행 파일은 호스트에서 돌리지 않는다.
        NativeToolchain android;
        try
        {
            android = await NativeToolchains.DiscoverAndroidAsync("arm64", AndroidApiLevel);
        }
        catch (FileNotFoundException)
        {
            pass("PCH with the Android NDK clang++ skipped (NDK not found)");
            return;
        }

        NativeBuildRequest cross = request with { Toolchain = android };
        string crossRoot = Fixture(root, "android pch");
        await DefinitionsHeaderAsync(cross, crossRoot);
        await ModulePchAsync(cross, crossRoot);
        await SharedPchAsync(cross, crossRoot);
        pass("PCH with the Android NDK clang++ (GNU driver): definitions header, module PCH and shared PCH fixtures cross-built");
    }

    // --timing 집계(12절)의 trace 해석이다. clang은 Source(파일 하나의 파싱)를 시작·끝 쌍으로 연달아 쓰고, 길이를 직접 싣는 형태도 있다.
    // 실제 trace는 세밀도(500 µs)보다 짧은 항목을 빼므로 작은 fixture 빌드로는 헤더 목록을 보장하지 못한다 — 손으로 만든 trace로 본다.
    private static void TimingAggregation()
    {
        const string trace = """
            {"traceEvents":[
            {"pid":1,"tid":7,"ts":100,"ph":"b","cat":"Source","id":0,"name":"Source","args":{"detail":"C:\\inc\\a.h"}},
            {"pid":1,"tid":7,"ts":350,"ph":"e","cat":"Source","id":0,"name":"Source"},
            {"pid":1,"tid":7,"ts":50,"ph":"b","cat":"Source","id":0,"name":"Source","args":{"detail":"C:\\src\\member.cpp"}},
            {"pid":1,"tid":7,"ts":900,"ph":"e","cat":"Source","id":0,"name":"Source"},
            {"pid":1,"tid":7,"ts":1000,"ph":"b","cat":"Source","id":0,"name":"Source","args":{"detail":"C:\\inc\\a.h"}},
            {"pid":1,"tid":7,"ts":1040,"ph":"e","cat":"Source","id":0,"name":"Source"},
            {"pid":1,"tid":7,"ts":10,"ph":"X","dur":70,"name":"Source","args":{"detail":"/inc/b.h"}},
            {"pid":1,"tid":7,"ts":10,"ph":"X","dur":30,"name":"InstantiateClass","args":{"detail":"Box<int>"}},
            {"pid":1,"tid":7,"ts":60,"ph":"X","dur":12,"name":"InstantiateFunction","args":{"detail":"Box<int>"}},
            {"pid":1,"tid":7,"ts":0,"ph":"X","dur":5000,"name":"Total ExecuteCompiler","args":{"count":1}},
            {"pid":1,"tid":7,"ts":0,"ph":"X","dur":290,"name":"Total Source","args":{"count":3}},
            {"pid":1,"tid":7,"ts":0,"ph":"M","name":"process_name","args":{"name":"clang"}}
            ]}
            """;
        using System.Text.Json.JsonDocument document = System.Text.Json.JsonDocument.Parse(trace);
        var headers = new Dictionary<string, (long Total, int Count)>(StringComparer.Ordinal);
        var templates = new Dictionary<string, (long Total, int Count)>(StringComparer.Ordinal);
        long total = TimingReport.Accumulate(document.RootElement.GetProperty("traceEvents"), headers, templates);
        // a.h는 두 번(250 + 40), b.h는 길이 형태로 한 번, member.cpp(묶음 구성원)는 헤더 목록에 없다. 같은 템플릿의 클래스·함수 인스턴스화는 합친다.
        Check(total == 5000 && headers.Count == 2 && headers["C:\\inc\\a.h"] == (290, 2) && headers["/inc/b.h"] == (70, 1) &&
            templates.Count == 1 && templates["Box<int>"] == (42, 2),
            "time trace aggregation: total " + total + ", headers " + string.Join(", ", headers) + ", templates " + string.Join(", ", templates));
    }

    // 교차 빌드 fixture의 Android API 수준이다(엔진의 최소 지원 수준).
    private const int AndroidApiLevel = 26;

    private static string Fixture(string root, string name) => Directory.CreateDirectory(Path.Combine(root, name)).FullName;

    private static Func<string, string, string> Writer(string fixture) => (relative, content) =>
    {
        string path = Path.GetFullPath(Path.Combine(fixture, relative));
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, content);
        return path;
    };

    private static async Task<NativeBuildResult> Build(NativeBuildRequest request)
    {
        NativeBuildResult result = await new NativeBuildEngine().BuildAsync(request);
        Check(result.Succeeded, "fixture build failed: " + result.Failure + "\n" +
            string.Join("\n", result.Actions.Where(action => !action.Succeeded).Select(action => action.StdOut + action.StdErr)));
        return result;
    }

    // 실행 파일이 기대한 종료 코드로 끝나는가(교차 빌드한 Android 실행 파일은 호스트에서 돌리지 않는다).
    private static async Task<bool> Runs(NativeBuildRequest request, NativeBuildResult result, string module, int expected) =>
        request.Toolchain.Platform == NativePlatform.Android ||
        (await ProcessRunner.RunAsync(new(result.ModuleOutputs[module], [], request.RootDirectory, request.Toolchain.Environment))).ExitCode == expected;

    private static async Task DefinitionsHeaderAsync(NativeBuildRequest request, string root)
    {
        string fixture = Fixture(root, "definitions fixture");
        var write = Writer(fixture);
        string include = Path.GetDirectoryName(write("include/value.h", "#pragma once\nint Value() noexcept;\n"))!;
        string library = write("lib/value.cpp", "#include <value.h>\nint Value() noexcept { return DEF_PUBLIC + DEF_PRIVATE; }\n");
        string main = write("app/main.cpp", "#include <value.h>\nint main() { return Value() * 10 + DEF_PUBLIC; }\n");
        NativeBuildRequest With(string publicValue) => request with
        {
            RootDirectory = fixture,
            OutputDirectory = Path.Combine(fixture, "out"),
            Modules =
            [
                new() { Name = "DefLib", Sources = [library], PublicIncludeDirectories = [include], PublicDefines = ["DEF_PUBLIC=" + publicValue], PrivateDefines = ["DEF_PRIVATE"] },
                new() { Name = "DefApp", Kind = NativeModuleKind.Executable, Sources = [main], Dependencies = ["DefLib"] }
            ],
            Targets = ["DefApp"]
        };

        NativeBuildRequest three = With("3");
        NativePlan plan = ActionGraph.Create(three);
        string header = ModuleDefinitions.PathOf(three.OutputDirectory, "DefLib");
        string appHeader = ModuleDefinitions.PathOf(three.OutputDirectory, "DefApp");
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        NativeAction libraryCompile = plan.Actions.Single(action => action.Kind == "Compile" && action.Module == "DefLib");
        Check(plan.Actions.Where(action => action.Kind == "Compile").All(action => !action.Arguments.Any(argument => argument.Contains("DEF_", StringComparison.Ordinal))) &&
            (windows ? libraryCompile.Arguments.Contains("/FI" + header) : libraryCompile.Arguments.Contains(header)) &&
            libraryCompile.Inputs.Contains(header),
            "module definitions must move from the command line into a force-included header that is an input");
        Check(plan.GeneratedFiles.Single(file => file.Path == header).Content == "// Generated by Rocks.Build. Do not edit.\n#define DEF_PRIVATE 1\n#define DEF_PUBLIC 3\n" &&
            plan.GeneratedFiles.Single(file => file.Path == appHeader).Content == "// Generated by Rocks.Build. Do not edit.\n#define DEF_PUBLIC 3\n",
            "definitions header content: sorted #define lines, the dependent receives only the public definition");
        LogicalCompile logical = plan.LogicalCompiles.Single(compile => compile.Module == "DefLib");
        Check(logical.ForcedIncludes.SequenceEqual([header]) && (windows ? logical.Arguments.Contains("/FI" + header) : logical.Arguments.Contains(header)),
            "logical compile commands must force-include the definitions header");

        NativeBuildResult built = await Build(three);
        Check(File.ReadAllText(header).Contains("#define DEF_PUBLIC 3\n", StringComparison.Ordinal) && await Runs(three, built, "DefApp", 43),
            "the definitions header must reach the translation units");
        Check((await Build(three)).ExecutedActions == 0, "an unchanged build with definitions headers must execute zero actions");
        NativeBuildRequest four = With("4");
        NativeBuildResult changed = await Build(four);
        Check(changed.Actions.Count(action => action.Kind == "Compile" && action.Executed) == 2 && await Runs(four, changed, "DefApp", 54),
            "a changed definition must recompile every translation unit that receives it");
        try
        {
            ActionGraph.Create(three with
            {
                Modules = [three.Modules[0], three.Modules[1] with { PrivateDefines = ["DEF_PUBLIC=9"] }]
            });
            throw new InvalidOperationException("conflicting definitions were accepted");
        }
        catch (ArgumentException error) when (error.Message.Contains("conflicting propagated definition", StringComparison.Ordinal))
        {
        }
    }

    private static async Task ModulePchAsync(NativeBuildRequest request, string root)
    {
        string fixture = Fixture(root, "pch fixture");
        var write = Writer(fixture);
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        const string headerText = "#pragma once\n#include <stddef.h>\ninline int PchValue() noexcept { return 5; }\n";
        string pchHeader = write("lib/PchLib.h", headerText);
        string include = Path.GetDirectoryName(pchHeader)!;
        string[] sources = Enumerable.Range(0, 3).Select(index => write($"lib/part{index}.cpp",
            $"#include \"PchLib.h\"\nint Part{index}() noexcept {{ return PchValue() + {index}; }}\n")).ToArray();
        string defined = write("lib/defined.cpp", "#define PCH_FIXTURE_SETTING 1\n#include \"PchLib.h\"\nint Defined() noexcept { return PchValue(); }\n");
        string cSource = write("lib/plain.c", "int Plain(void) { return 1; }\n");
        string main = write("app/main.cpp", "int Part0() noexcept;\nint Part1() noexcept;\nint Part2() noexcept;\nint Defined() noexcept;\nextern \"C\" int Plain(void);\n" +
            "int main() { return Part0() + Part1() + Part2() + Defined() + Plain(); }\n");
        NativeModuleInput library = new()
        {
            Name = "PchLib",
            Sources = [.. sources, defined, cSource],
            PublicIncludeDirectories = [include],
            PrivateDefines = ["PCH_FIXTURE=1"],
            PrivatePchHeader = pchHeader
        };
        NativeModuleInput application = new() { Name = "PchApp", Kind = NativeModuleKind.Executable, Sources = [main], Dependencies = ["PchLib"] };
        NativeBuildRequest build = request with
        {
            RootDirectory = fixture,
            OutputDirectory = Path.Combine(fixture, "out"),
            Modules = [library, application],
            Targets = ["PchApp"]
        };

        NativePlan plan = ActionGraph.Create(build);
        NativeAction pch = plan.Actions.Single(action => action.Kind == "Pch");
        string definitions = ModuleDefinitions.PathOf(build.OutputDirectory, "PchLib");
        NativeAction[] compiles = plan.Actions.Where(action => action.Kind == "Compile" && action.Module == "PchLib").ToArray();
        NativeAction[] users = compiles.Where(action => action.Dependencies.Contains(pch.Id)).ToArray();
        string pchFile = pch.Outputs[0];
        Check(pch.Module == "PchLib" && pchFile.EndsWith(".pch", StringComparison.Ordinal) && pch.DependencyRecord is not null &&
            pch.Outputs.Count == (windows ? 3 : 2) && pch.Inputs.Contains(pchHeader) && pch.Inputs.Contains(definitions) &&
            Fingerprints.IsUnder(pchFile, Path.Combine(build.OutputDirectory, "Pch", "PchLib")) &&
            pch.Arguments.Contains("-fno-pch-timestamp") && (windows ? pch.Arguments.Contains("/Yc" + pchHeader) : pch.Arguments.Contains("c++-header")),
            "the PCH action must precompile the header after the definitions header and declare its outputs: " + string.Join(" ", pch.Arguments));
        Check(users.Length == 3 && users.All(action => sources.Contains(action.Source!) && action.Inputs.Contains(pchFile) &&
                (windows ? action.Arguments.Contains("/Yu" + pchHeader) && action.Arguments.Contains("/Fp" + pchFile)
                    : action.Arguments.Contains("-include-pch") && action.Arguments.Contains(pchFile))),
            "every C++ translation unit of the module must use the PCH and depend on its action");
        // PCH를 쓰지 않는 번역 단위: 첫 include 앞에서 #define하는 파일과 C 소스. 정의 헤더는 받는다(C 소스 포함).
        NativeAction definedCompile = compiles.Single(action => action.Source == defined);
        NativeAction cCompile = compiles.Single(action => action.Source == cSource);
        Check(!definedCompile.Dependencies.Contains(pch.Id) && !cCompile.Dependencies.Contains(pch.Id) &&
            definedCompile.Inputs.Contains(definitions) && cCompile.Inputs.Contains(definitions) &&
            !definedCompile.Arguments.Any(argument => argument.Contains(".pch", StringComparison.Ordinal)),
            "a source that defines macros before its first include and a C source must not use the PCH");
        // 논리 명령: PCH 바이너리 대신 PCH 앞부분과 PCH 헤더를 같은 순서로 강제 include한다.
        LogicalCompile logical = plan.LogicalCompiles.Single(compile => compile.Source == sources[0]);
        string[] expectedForced = windows ? ["/FI" + definitions, "/FI" + pchHeader] : ["-include", definitions, "-include", pchHeader];
        int at = logical.Arguments.ToList().IndexOf(expectedForced[0]);
        Check(at >= 0 && logical.Arguments.Skip(at).Take(expectedForced.Length).SequenceEqual(expectedForced) &&
            logical.ForcedIncludes.SequenceEqual([definitions, pchHeader]) &&
            !logical.Arguments.Any(argument => argument.StartsWith("/Yu", StringComparison.Ordinal) || argument.StartsWith("/Fp", StringComparison.Ordinal) ||
                argument == "-include-pch" || argument.EndsWith(".pch", StringComparison.Ordinal)),
            "logical compile commands must force-include the PCH prefix and header instead of using the PCH binary: " + string.Join(" ", logical.Arguments));

        NativeBuildResult cold = await Build(build);
        Check(cold.PchActions == 1 && pch.Outputs.All(File.Exists) && cold.Actions.Single(action => action.Kind == "Pch").Executed &&
            await Runs(build, cold, "PchApp", 24), "the PCH build must publish every PCH output and produce a working executable");
        Check((await Build(build)).ExecutedActions == 0, "an unchanged PCH build must execute zero actions");

        // 같은 크기의 헤더 변경: PCH action은 내용으로 다시 돌고(clang의 입력 검증에 기대지 않는다) 소비 번역 단위도 다시 컴파일된다.
        File.WriteAllText(pchHeader, headerText.Replace("return 5;", "return 6;", StringComparison.Ordinal));
        NativeBuildResult edited = await Build(build);
        Check(new FileInfo(pchHeader).Length == headerText.Length && edited.Actions.Single(action => action.Kind == "Pch").Executed &&
            edited.Actions.Count(action => action.Kind == "Compile" && action.Module == "PchLib" && action.Executed) == 4 &&
            await Runs(build, edited, "PchApp", 28),
            "a same-size PCH header change must rebuild the PCH and its consumers");
        File.WriteAllText(pchHeader, headerText);

        // 낡은 변형 디렉터리는 PCH를 켠 성공한 빌드가 지운다.
        string stale = Directory.CreateDirectory(Path.Combine(build.OutputDirectory, "Pch", "PchLib", "000000000000")).FullName;
        File.WriteAllText(Path.Combine(stale, "PchLib.pch"), "stale");
        await Build(build);
        Check(!Directory.Exists(stale) && File.Exists(pchFile), "a successful PCH build must delete stale PCH variant directories");

        // 번역 단위 수 하한, 모듈 설정, 요청 스위치.
        NativeBuildRequest With(NativeModuleInput replacement) => build with { Modules = [replacement, application] };
        Check(!ActionGraph.Create(With(library with { PrivatePchMinTus = 4 })).Actions.Any(action => action.Kind == "Pch"),
            "a module with fewer PCH users than PrivatePchMinTus must not build a PCH");
        Check(!ActionGraph.Create(With(library with { PchUsage = PchUsage.Off })).Actions.Any(action => action.Kind == "Pch") &&
            !ActionGraph.Create(With(library with { ThirdParty = true })).Actions.Any(action => action.Kind == "Pch"),
            "PchUsage.Off and third-party modules must not build a PCH");
        NativeBuildRequest withoutPch = build with { Pch = false };
        NativePlan plain = ActionGraph.Create(withoutPch);
        Check(!plain.Actions.Any(action => action.Kind == "Pch") &&
            plain.Actions.Where(action => action.Kind == "Compile").All(action => action.Outputs[0].Contains(Path.DirectorySeparatorChar + "u1p0" + Path.DirectorySeparatorChar)),
            "--pch off must not build a PCH and must use its own object variant directory");
        NativeBuildResult verified = await Build(withoutPch);
        Check(verified.PchActions == 0 && await Runs(withoutPch, verified, "PchApp", 24) && File.Exists(pchFile),
            "the verification variant must build without the PCH and keep the PCH variant of the other mode");

        // 유니티 묶음도 PCH를 쓴다.
        NativeBuildRequest unity = With(library with { Unity = UnityMode.On, UnityAnchorFiles = int.MaxValue, PrivatePchMinTus = 1 });
        NativePlan unityPlan = ActionGraph.Create(unity);
        NativeAction batch = unityPlan.Actions.Single(action => action.Members is not null);
        Check(batch.Members!.Count == 3 && batch.Dependencies.Contains(unityPlan.Actions.Single(action => action.Kind == "Pch").Id),
            "a unity batch must use the module PCH");
        NativeBuildResult unityBuilt = await Build(unity);
        Check(unityBuilt.UnityBatches == 1 && unityBuilt.PchActions == 1 && await Runs(unity, unityBuilt, "PchApp", 24), "unity with PCH must build and run");
    }

    private static async Task AdaptiveAsync(NativeBuildRequest request, string root)
    {
        // porcelain v1 -z: 상태 두 열 중 하나라도 공백이 아니면 작업 파일이다. 이름 변경·복사는 새 경로만이다.
        string porcelain = " M src/a.cpp\0?? new file.cpp\0A  added.cpp\0R  renamed.cpp\0old.cpp\0UU conflict.cpp\0T  type.cpp\0C  copy.cpp\0origin.cpp\0MM both.cpp\0";
        Check(SourceWorkingSet.ParsePorcelain(porcelain).SequenceEqual(
                ["src/a.cpp", "new file.cpp", "added.cpp", "renamed.cpp", "conflict.cpp", "type.cpp", "copy.cpp", "both.cpp"]) &&
            SourceWorkingSet.ParsePorcelain("").Count == 0, "porcelain status parsing");
        foreach (string malformed in new[] { "M\0", "XYZ\0", " Mmissing-space\0" })
        {
            try
            {
                SourceWorkingSet.ParsePorcelain(malformed);
            }
            catch (InvalidDataException)
            {
                continue;
            }

            throw new InvalidOperationException("a malformed git status entry was accepted: " + malformed);
        }

        string fixture = Fixture(root, "adaptive fixture");
        var write = Writer(fixture);
        string include = Directory.CreateDirectory(Path.Combine(fixture, "include")).FullName;
        string paired = write("include/member3.h", "#pragma once\nint Member3() noexcept;\n");
        string[] members = Enumerable.Range(0, 6).Select(index => write($"src/member{index}.cpp",
            (index == 3 ? "#include \"member3.h\"\n" : "") + $"int Member{index}() noexcept {{ return {index}; }}\n")).ToArray();
        string main = write("app/main.cpp", string.Concat(Enumerable.Range(0, 6).Select(index => $"int Member{index}() noexcept;\n")) +
            "int main() { return Member0() + Member1() + Member2() + Member3() + Member4() + Member5(); }\n");
        NativeModuleInput library = new()
        {
            Name = "AdaptLib",
            Sources = members,
            PublicIncludeDirectories = [include],
            Unity = UnityMode.On,
            UnityAnchorFiles = int.MaxValue
        };
        NativeBuildRequest build = request with
        {
            RootDirectory = fixture,
            OutputDirectory = Path.Combine(fixture, "out"),
            Modules = [library, new() { Name = "AdaptApp", Kind = NativeModuleKind.Executable, Sources = [main], Dependencies = ["AdaptLib"] }],
            Targets = ["AdaptApp"]
        };
        NativeBuildRequest Working(params string[] files) => build with { WorkingSet = files.ToHashSet(NativeToolchains.PathComparer) };
        string[] BatchMembers(NativePlan value) => value.Actions.Single(action => action.Members is not null).Members!.Order(StringComparer.Ordinal).ToArray();

        NativePlan whole = ActionGraph.Create(build);
        Check(BatchMembers(whole).SequenceEqual(members.Order(StringComparer.Ordinal)) && whole.AdaptiveSeparated == 0, "without working files every member is batched");
        // 자신이 작업 파일인 소스는 묶음에서 빠져 단독 번역 단위가 된다. 나머지 구성은 그대로다.
        NativePlan separated = ActionGraph.Create(Working(members[2]));
        Check(BatchMembers(separated).SequenceEqual(members.Where(member => member != members[2]).Order(StringComparer.Ordinal)) &&
            separated.AdaptiveSeparated == 1 && separated.Actions.Any(action => action.Members is null && action.Source == members[2]),
            "a working source must leave its batch and compile standalone");
        // 짝 헤더(첫 include)가 작업 파일이고 그 이름이 모듈 안에서 유일하면 그 소스도 분리한다. 같은 이름의 헤더가 둘이면 분리하지 않는다.
        Check(!BatchMembers(ActionGraph.Create(Working(paired))).Contains(members[3]), "a source whose paired header is a working file must be separated");
        string duplicate = write("src/nested/member3.h", "#pragma once\n");
        Check(BatchMembers(ActionGraph.Create(Working(paired))).Contains(members[3]), "a paired header name that is not unique in the module must not separate its source");
        File.Delete(duplicate);
        Directory.Delete(Path.GetDirectoryName(duplicate)!);
        // 유니티를 끄면 작업 파일 집합은 쓰이지 않는다.
        Check(ActionGraph.Create(Working(members[2]) with { Unity = false }).AdaptiveSeparated == 0, "--unity off ignores the working set");

        // 도구 빌드 ID manifest는 작업 파일 집합과 무관하다.
        NativeBuildRequest versioned = build with { EngineVersion = new EngineVersion(1, 2, 3) };
        string Manifest(NativeBuildRequest input) => BuildInfoGenerator.Manifest(input, ActionGraph.Create(input), "AdaptApp", new string('0', 64), new FileDigests());
        Check(Manifest(versioned) == Manifest(versioned with { WorkingSet = new HashSet<string>(NativeToolchains.PathComparer) { members[2] } }),
            "the tool build ID manifest must not depend on the working set");

        // 분리한 파일을 고치면 그 번역 단위만 다시 컴파일된다(묶음은 그대로다).
        NativeBuildRequest working = Working(members[2]);
        NativeBuildResult cold = await Build(working);
        Check(cold.AdaptiveSeparated == 1 && await Runs(working, cold, "AdaptApp", 15), "adaptive fixture build");
        File.WriteAllText(members[2], "int Member2() noexcept { return 20; }\n");
        NativeBuildResult edited = await Build(working);
        NativeActionResult[] recompiled = edited.Actions.Where(action => action.Kind == "Compile" && action.Executed).ToArray();
        Check(recompiled.Length == 1 && recompiled[0].Id == separated.Actions.Single(action => action.Source == members[2]).Id &&
            await Runs(working, edited, "AdaptApp", 33), "editing a separated working file must recompile only that translation unit");

        // 실제 git: 수정한 추적 파일과 미추적 파일이 작업 파일이고, 작업 사본이 아닌 디렉터리는 빈 집합이다(알림 한 번).
        string repository = Fixture(root, "adaptive git");
        var writeRepository = Writer(repository);
        string tracked = writeRepository("tracked.cpp", "int Tracked();\n");
        string clean = writeRepository("clean.cpp", "int Clean();\n");
        async Task Git(params string[] arguments)
        {
            ProcessResult result = await ProcessRunner.RunAsync(new("git", ["-C", repository, .. arguments], repository));
            Check(result.ExitCode == 0, "git " + string.Join(' ', arguments) + " failed: " + result.StdErr);
        }

        await Git("init", "--quiet");
        await Git("add", "tracked.cpp", "clean.cpp");
        await Git("-c", "user.name=Rocks Build Tests", "-c", "user.email=tests@example.invalid", "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture");
        File.AppendAllText(tracked, "// edited\n");
        string untracked = writeRepository("sub dir/untracked 파일.cpp", "int Untracked();\n");
        IReadOnlySet<string> found = await SourceWorkingSet.QueryAsync(repository, null, CancellationToken.None);
        Check(found.SetEquals([tracked, untracked]) && !found.Contains(clean), "git working files: " + string.Join(", ", found));
        // git은 object 파일을 읽기 전용으로 둔다. 시험 루트를 지울 수 있게 속성을 풀고 저장소를 지운다.
        foreach (string file in Directory.EnumerateFiles(repository, "*", SearchOption.AllDirectories))
        {
            File.SetAttributes(file, FileAttributes.Normal);
        }

        Directory.Delete(repository, true);
        var notices = new List<string>();
        string outside = Fixture(Path.GetPathRoot(root)!.Length == root.Length ? root : Path.GetTempPath(), "rocks-adaptive-outside-" + Guid.NewGuid().ToString("N"));
        try
        {
            IReadOnlySet<string> none = await SourceWorkingSet.QueryAsync(outside, null, CancellationToken.None, notices.Add);
            // 임시 디렉터리가 우연히 다른 작업 사본 안이면 이 대조는 건너뛴다.
            Check(notices.Count == 0 || (none.Count == 0 && notices.Count == 1), "a directory outside any working copy must give an empty set and one notice");
        }
        finally
        {
            Directory.Delete(outside, true);
        }
    }

    // 공유 PCH(8.4절)와 --timing(12절).
    private static async Task SharedPchAsync(NativeBuildRequest request, string root)
    {
        string fixture = Fixture(root, "shared pch fixture");
        var write = Writer(fixture);
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        const string baseHeaderText = "#pragma once\n#ifdef USER_FLAG\n#define BASE_FLAGGED 1\n#endif\n#define BASE_HAS_INDIRECT USER_INDIRECT\n" +
            "#if BASE_HAS_INDIRECT\n#define BASE_INDIRECT 1\n#endif\ninline int BaseValue() noexcept { return BASE_PUBLIC + 1; }\n";
        string baseInclude = Path.GetDirectoryName(Path.GetDirectoryName(write("base/include/shared/Base.h", baseHeaderText))!)!;
        write("base/include/shared/BasePch.h", "#pragma once\n#include <shared/Base.h>\n");
        string baseSource = write("base/src/base.cpp", "#include <shared/Base.h>\nint BaseSum() noexcept { return BaseValue(); }\n");
        string upperInclude = Path.GetDirectoryName(Path.GetDirectoryName(write("upper/include/upper/Upper.h",
            "#pragma once\n#include <shared/Base.h>\ninline int UpperValue() noexcept { return BaseValue() + 10; }\n"))!)!;
        write("upper/include/upper/UpperPch.h", "#pragma once\n#include <shared/BasePch.h>\n#include <upper/Upper.h>\n");
        string upperSource = write("upper/src/upper.cpp", "#include <upper/Upper.h>\nint UpperSum() noexcept { return UpperValue(); }\n");
        string userA = write("users/a.cpp", "#include <upper/Upper.h>\nint UserA() noexcept { return UpperValue() + 100; }\n");
        string userB = write("users/b.cpp", "#include <upper/Upper.h>\nint UserB() noexcept { return UpperValue() + USER_B_EXTRA; }\n");
        string main = write("app/main.cpp", "int BaseSum() noexcept;\nint UpperSum() noexcept;\nint UserA() noexcept;\nint UserB() noexcept;\n" +
            "int main() { return BaseSum() + UpperSum() + UserA() + UserB(); }\n");
        NativeModuleInput baseModule = new()
        {
            Name = "SharedBase",
            Sources = [baseSource],
            PublicIncludeDirectories = [baseInclude],
            PublicDefines = ["BASE_PUBLIC=1"],
            SharedPchHeader = "shared/BasePch.h"
        };
        NativeModuleInput upper = new()
        {
            Name = "SharedUpper",
            Sources = [upperSource],
            PublicIncludeDirectories = [upperInclude],
            Dependencies = ["SharedBase"],
            SharedPchHeader = "upper/UpperPch.h"
        };
        NativeModuleInput moduleA = new() { Name = "UserA", Sources = [userA], Dependencies = ["SharedUpper"] };
        NativeModuleInput moduleB = new() { Name = "UserB", Sources = [userB], Dependencies = ["SharedUpper"], PrivateDefines = ["USER_B_EXTRA=5"] };
        NativeModuleInput application = new() { Name = "SharedApp", Kind = NativeModuleKind.Executable, Sources = [main], Dependencies = ["UserA", "UserB"] };
        NativeBuildRequest With(NativeModuleInput a, NativeModuleInput b, string output = "out") => request with
        {
            RootDirectory = fixture,
            OutputDirectory = Path.Combine(fixture, output),
            Modules = [baseModule, upper, a, b, application],
            Targets = ["SharedApp"]
        };

        NativeBuildRequest build = With(moduleA, moduleB);
        NativePlan plan = ActionGraph.Create(build);
        NativeAction[] pchActions = plan.Actions.Where(action => action.Kind == "Pch").ToArray();
        NativeAction Compile(string module) => plan.Actions.Single(action => action.Kind == "Compile" && action.Module == module);
        // 제공 모듈 선택: 다른 제공 모듈을 가장 많이 포함하는 것(SharedUpper가 SharedBase를 포함한다). 제공 모듈 자신도 자기 공유 PCH를 쓴다.
        // 같은 인자·같은 앞부분의 소비 모듈은 PCH action 하나를 함께 쓴다.
        NativeAction upperPch = pchActions.Single(action => action.Module == "SharedUpper");
        NativeAction basePch = pchActions.Single(action => action.Module == "SharedBase");
        Check(pchActions.Length == 2 && Compile("SharedBase").Dependencies.Contains(basePch.Id) &&
            new[] { "SharedUpper", "UserA", "UserB", "SharedApp" }.All(module => Compile(module).Dependencies.Contains(upperPch.Id)),
            "shared PCH provider selection and sharing: " + string.Join(", ", pchActions.Select(action => action.Id)));
        // 공유 정의(제공 모듈의 public define과 전파분)가 PCH 앞부분이고, 소비 모듈의 나머지 정의는 PCH 뒤의 추가 정의 헤더다.
        string sharedDefinitions = ModuleDefinitions.SharedPathOf(build.OutputDirectory, "SharedUpper");
        string extraDefinitions = ModuleDefinitions.ExtraPathOf(build.OutputDirectory, "UserB");
        Check(plan.GeneratedFiles.Single(file => file.Path == sharedDefinitions).Content.EndsWith("#define BASE_PUBLIC 1\n", StringComparison.Ordinal) &&
            plan.GeneratedFiles.Single(file => file.Path == extraDefinitions).Content.EndsWith("#define USER_B_EXTRA 5\n", StringComparison.Ordinal) &&
            upperPch.Inputs.Contains(sharedDefinitions) && !plan.GeneratedFiles.Any(file => file.Path == ModuleDefinitions.ExtraPathOf(build.OutputDirectory, "UserA")),
            "shared and extra definitions headers");
        LogicalCompile logicalB = plan.LogicalCompiles.Single(compile => compile.Module == "UserB");
        Check(logicalB.ForcedIncludes.Count == 3 && logicalB.ForcedIncludes[0] == sharedDefinitions && logicalB.ForcedIncludes[2] == extraDefinitions &&
            logicalB.ForcedIncludes[1].EndsWith("UpperPch.h", StringComparison.Ordinal) &&
            Compile("UserB").Arguments.Contains(windows ? "/FI" + extraDefinitions : extraDefinitions),
            "forced include order must be [PCH prefix] -> [PCH header] -> [extra definitions]");

        NativeBuildResult built = await Build(build);
        // BaseValue = 2, UpperValue = 12: BaseSum 2 + UpperSum 12 + UserA 112 + UserB 17 = 143.
        Check(built.PchActions == 2 && await Runs(build, built, "SharedApp", 143), "shared PCH build and run");
        Check((await Build(build)).ExecutedActions == 0, "an unchanged shared PCH build must execute zero actions");

        // 경로가 아닌 인자가 다르면 다른 변형이다. 모듈 PCH가 있으면 그것을 쓰고, PrivateOnly는 공유 PCH를 쓰지 않는다.
        Check(ActionGraph.Create(With(moduleA with { Optimization = CodeOptimization.Always }, moduleB)).Actions.Count(action => action.Kind == "Pch") == 3,
            "a consumer with different non-path arguments must get its own PCH variant");
        Check(ActionGraph.Create(With(moduleA with { PchUsage = PchUsage.PrivateOnly }, moduleB)).Actions
                .Single(action => action.Kind == "Compile" && action.Module == "UserA").Dependencies.Count == 0,
            "PchUsage.PrivateOnly must not use a shared PCH");

        // 사용 검사: shadow — 소비 모듈의 앞선 include 디렉터리에 PCH closure 헤더와 같은 철자가 있다.
        string shadowInclude = Path.GetDirectoryName(Path.GetDirectoryName(write("users/private/upper/Upper.h", "#pragma once\ninline int UpperValue() noexcept { return 0; }\n"))!)!;
        NativeBuildResult shadowed = await new NativeBuildEngine().BuildAsync(With(moduleA, moduleB with { PrivateIncludeDirectories = [shadowInclude] }, "out-shadow"));
        Check(!shadowed.Succeeded && shadowed.Actions.Any(action => action.Module == "UserB" && !action.Succeeded &&
                action.StdErr.Contains("before the PCH's", StringComparison.Ordinal) && action.StdErr.Contains("upper/Upper.h", StringComparison.Ordinal)),
            "a header shadowing a PCH closure header must fail the consumer: " + shadowed.Failure);
        // 사용 검사: 추가 정의가 closure 헤더의 전처리 조건에 쓰인다(직접, 그리고 매크로 본문을 거친 간접 참조).
        foreach ((string define, string expected) in new[] { ("USER_FLAG=1", "definition USER_FLAG is used by a preprocessor condition"),
                     ("USER_INDIRECT=1", "definition USER_INDIRECT is used by a preprocessor condition") })
        {
            NativeBuildResult rejected = await new NativeBuildEngine().BuildAsync(
                With(moduleA, moduleB with { PrivateDefines = ["USER_B_EXTRA=5", define] }, "out-" + define.Split('=')[0]));
            Check(!rejected.Succeeded && rejected.Actions.Any(action => action.Module == "UserB" && !action.Succeeded &&
                    action.StdErr.Contains(expected, StringComparison.Ordinal) && action.StdErr.Contains("Base.h:", StringComparison.Ordinal) &&
                    (define.StartsWith("USER_FLAG", StringComparison.Ordinal) || action.StdErr.Contains("through macro BASE_HAS_INDIRECT", StringComparison.Ordinal))),
                $"an extra definition ({define}) used by a PCH closure header's preprocessor condition must fail the consumer: " + rejected.Failure);
        }

        // --timing: -ftime-trace 결과를 집계한다(번역 단위마다 한 항목, 헤더 목록).
        NativeBuildRequest timed = With(moduleA, moduleB, "out-timing") with { Timing = true, TimingLabel = "Probe" };
        await Build(timed);
        string report = Path.Combine(timed.OutputDirectory, "Timing", "Probe.timing.json");
        using System.Text.Json.JsonDocument timing = System.Text.Json.JsonDocument.Parse(File.ReadAllText(report));
        // 번역 단위 다섯과 PCH action 둘이다. PCH에 든 헤더의 파싱 시간은 PCH action의 trace에 있다.
        // 헤더·템플릿 목록은 clang의 기본 trace 세밀도(500 마이크로초)보다 오래 걸린 항목만 담으므로 이 작은 fixture에서는 비어 있을 수 있다.
        Check(timing.RootElement.GetProperty("translationUnits").GetArrayLength() == 7 &&
            timing.RootElement.GetProperty("translationUnits").EnumerateArray().All(item => item.GetProperty("microseconds").GetInt64() > 0) &&
            timing.RootElement.GetProperty("headers").ValueKind == System.Text.Json.JsonValueKind.Array &&
            timing.RootElement.GetProperty("templates").ValueKind == System.Text.Json.JsonValueKind.Array,
            "the timing report must list every translation unit and PCH action: " + File.ReadAllText(report));
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
