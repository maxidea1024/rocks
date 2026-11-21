using System.Text.Json;
using Rocks.Build;

/// <summary>
/// IDE 계획 모드와 IntelliSense 모델(Docs/Design/ProjectFiles.ko.md 6.2·8절, PJ0)을 검사한다.
/// <list type="bullet">
/// <item>인자 분류표: 표의 모든 철자, 소스, 언어 표지, 모르는 인자 거부.</item>
/// <item>IDE 계획: 없는 생성물을 요구하지 않고 출력에 아무것도 쓰지 않는다. BuildAsync는 IDE 모드를 거부한다.</item>
/// <item>같음: 생성 소스·생성 include·유니티·모듈 PCH·공유 PCH를 켠 plan-only 빌드의 compile DB와 IDE 계획의 논리 명령이 같다(16절 V1).</item>
/// <item>모델: 정의 디렉터리·정의 파일 이름 동률 규칙(타깃 3개), 정의 디렉터리 밖 파일, (파일, 구성) 원천과 묶음 값의 대조(Verify),
/// 정의 헤더 사본, 계획 순서와 무관한 결과, 엔진 프로젝트 구성 충돌.</item>
/// </list>
/// </summary>
internal static class IdePlanTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        Classification();
        pass("IntelliSense argument table: pass-through, translated, dropped, language, source, unknown rejected");

        MissingGeneratedInputs(request, root);
        await RejectedByBuildAsync(request, root);
        pass("IDE plan mode: no generated inputs required, nothing written, BuildAsync rejects it");

        await EqualsBuildPlanAsync(request, root);
        pass("IDE plan equals the plan-only compile database with generated sources, unity, module PCH and shared PCH");

        if (request.Toolchain.Platform == NativePlatform.Windows)
        {
            // 분류표는 clang-cl 철자만 받는다(IntelliSense는 Windows 계획만 쓴다, 6.3절).
            Model(request, root);
            pass("IntelliSense model: ownership ties and names, files outside definition directories, folded values verified, copies, order independence");
        }
    }

    private static void Classification()
    {
        const string source = @"C:\r\a.cpp";
        foreach (string argument in new[] { "/MDd", "/MD", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/utf-8", "/fsanitize=address" })
        {
            Check(IntelliSenseArguments.Classify(argument, source) == new IntelliSenseArgument(IntelliSenseArgumentKind.Option, argument),
                "pass-through option " + argument);
        }

        Check(IntelliSenseArguments.Classify("/clang:-std=c++23", source).Value == "/std:c++23preview", "C++23 spelling");
        Check(IntelliSenseArguments.Classify("/clang:-std=c17", source).Value == "/std:c17", "C17 spelling");
        Check(IntelliSenseArguments.OriginalOption("/std:c++23preview") == "/clang:-std=c++23", "translated option reverses");
        foreach (string argument in new[] { "--no-default-config", "/nologo", "/c", "/W4", "/WX", "-Wunique-object-duplication", "/Z7", "/Od", "/O2",
            "/clang:-ffp-contract=off", "-fno-strict-aliasing", "-fno-strict-overflow", "-flto=thin", "-fprofile-instr-generate",
            "-fcoverage-mapping", @"/clang:-fmacro-prefix-map=C:\r\=", @"/FoC:\r\out\a.obj" })
        {
            Check(IntelliSenseArguments.Classify(argument, source).Kind == IntelliSenseArgumentKind.Dropped, "dropped " + argument);
        }

        Check(IntelliSenseArguments.Classify(@"/IC:\r\inc", source) == new IntelliSenseArgument(IntelliSenseArgumentKind.Include, @"C:\r\inc"), "include");
        Check(IntelliSenseArguments.Classify("/DX=1", source) == new IntelliSenseArgument(IntelliSenseArgumentKind.Define, "X=1"), "define");
        Check(IntelliSenseArguments.Classify(@"/FIC:\r\d.h", source) == new IntelliSenseArgument(IntelliSenseArgumentKind.ForcedInclude, @"C:\r\d.h"),
            "forced include");
        Check(IntelliSenseArguments.Classify("/TP", source).Kind == IntelliSenseArgumentKind.Language, "language");
        Check(IntelliSenseArguments.Classify(source, source).Kind == IntelliSenseArgumentKind.Source, "source");
        foreach (string unknown in new[] { "/Ox", "-ffast-math", "/clang:-std=c++26", "/I", "/D" })
        {
            Throws<ArgumentException>(() => IntelliSenseArguments.Classify(unknown, source), unknown, "unknown argument " + unknown);
        }
    }

    // 생성물(리플렉션 생성 소스·생성 include·prebuilt)이 없어도 IDE 계획은 서고 출력에 아무것도 쓰지 않는다. 빌드 계획은 거부한다.
    private static void MissingGeneratedInputs(NativeBuildRequest request, string root)
    {
        string fixture = Path.Combine(root, "ide-plan-missing");
        string engine = Directory.CreateDirectory(Path.Combine(fixture, "Engine", "Lib")).FullName;
        File.WriteAllText(Path.Combine(engine, "Lib.cpp"), "int Lib() noexcept { return 7; }\n");
        File.WriteAllText(Path.Combine(engine, "Main.cpp"), "int Lib() noexcept;\nint main() { return Lib() == 7 ? 0 : 1; }\n");
        string output = Path.Combine(fixture, "out", "debug");
        NativeBuildRequest ide = request with
        {
            RootDirectory = fixture,
            OutputDirectory = output,
            Targets = ["Tool"],
            Modules =
            [
                new()
                {
                    Name = "Lib", Sources = [Path.Combine(engine, "Lib.cpp")], PublicIncludeDirectories = [Path.Combine(output, "Generated", "Lib", "Public")],
                    ReflectionSources = [Path.Combine(output, "Generated", "Lib", "Source", "Lib.module.generated.cpp")]
                },
                new()
                {
                    Name = "Tool", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(engine, "Main.cpp")], PublicDependencies = ["Lib"],
                    PrebuiltLibraries = [Path.Combine(fixture, "missing", "prebuilt.lib")]
                }
            ]
        };
        IdePlan plan = IdePlanner.Plan(ide);
        Check(!Directory.Exists(Path.Combine(fixture, "out")), "IDE plan wrote into the output directory");
        Check(plan.Compiles.Select(compile => Path.GetFileName(compile.Source)).Order(StringComparer.Ordinal).SequenceEqual(["Lib.cpp", "Main.cpp"]),
            "IDE plan lists only hand-written sources");
        Check(plan.ModuleOutputs["Tool"].EndsWith(OperatingSystem.IsWindows() ? "Tool.exe" : "Tool", StringComparison.Ordinal), "executable output path");
        Throws<FileNotFoundException>(() => ActionGraph.Create(ide), "Source does not exist", "build plan with a missing generated source");
    }

    private static async Task RejectedByBuildAsync(NativeBuildRequest request, string root)
    {
        try
        {
            await new NativeBuildEngine().BuildAsync(request with { OutputDirectory = Path.Combine(root, "ide-plan-rejected"), PlanMode = NativePlanMode.Ide });
            throw new InvalidOperationException("BuildAsync accepted an IDE plan request");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("IdePlanner", StringComparison.Ordinal), "BuildAsync rejection message");
        }
    }

    // 16절 V1의 "일반 모드와 같음": 생성 소스·생성 include가 실제로 있고 유니티·모듈 PCH·공유 PCH를 켠 plan-only 빌드의 compile DB와 IDE
    // 계획의 논리 명령(손으로 쓴 소스)이 object 경로만 빼고 같다. 모듈 PCH 하한은 원본 소스 수로 세므로 유니티 묶음 하나가 되어도 PCH가 있다.
    private static async Task EqualsBuildPlanAsync(NativeBuildRequest request, string root)
    {
        string fixture = Path.Combine(root, "ide-plan-equal");
        string Write(string relative, string text)
        {
            string path = Path.Combine(fixture, relative);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, text);
            return path;
        }

        string output = Path.Combine(fixture, "out", "debug");
        string baseInclude = Path.GetDirectoryName(Path.GetDirectoryName(Write("base/include/shared/Base.h", "#pragma once\ninline int BaseValue() noexcept { return BASE_ONE; }\n"))!)!;
        Write("base/include/shared/BasePch.h", "#pragma once\n#include <shared/Base.h>\n");
        string baseSource = Write("base/src/base.cpp", "#include <shared/Base.h>\nint BaseSum() noexcept { return BaseValue(); }\n");
        string libDirectory = Path.GetDirectoryName(Write("lib/Lib.h", "#pragma once\ninline int LibValue() noexcept { return 3; }\n"))!;
        Write("lib/LibPch.h", "#pragma once\n#include \"Lib.h\"\n");
        string[] libSources = [.. new[] { "a", "b", "c", "d" }.Select(name => Write($"lib/{name}.cpp", $"#include \"Lib.h\"\nint Lib_{name}() noexcept {{ return LibValue(); }}\n"))];
        string generatedInclude = Path.GetDirectoryName(Path.Combine(output, "Generated", "Lib", "Public", "Lib.generated.h"))!;
        Write(Path.Combine(output, "Generated", "Lib", "Public", "Lib.generated.h"), "#pragma once\n");
        string generatedSource = Write(Path.Combine(output, "Generated", "Lib", "Source", "Lib.module.generated.cpp"), "int LibGenerated() noexcept { return 1; }\n");
        string user = Write("user/user.cpp", "#include <shared/Base.h>\nint User() noexcept { return BaseValue(); }\n");
        string main = Write("app/main.cpp", "int BaseSum() noexcept;\nint User() noexcept;\nint main() { return BaseSum() + User(); }\n");
        NativeModuleInput[] modules =
        [
            new() { Name = "Base", Sources = [baseSource], PublicIncludeDirectories = [baseInclude], PublicDefines = ["BASE_ONE=1"], SharedPchHeader = "shared/BasePch.h" },
            new()
            {
                Name = "Lib", Sources = libSources, PublicIncludeDirectories = [libDirectory, generatedInclude], ReflectionSources = [generatedSource],
                Unity = UnityMode.On, UnityAnchorFiles = int.MaxValue, PrivatePchHeader = Path.Combine(libDirectory, "LibPch.h"), PrivatePchMinTus = 3
            },
            new() { Name = "User", Sources = [user], PublicDependencies = ["Base"] },
            new() { Name = "App", Kind = NativeModuleKind.Executable, Sources = [main], PublicDependencies = ["Lib", "User"] }
        ];
        NativeBuildRequest planned = request with { RootDirectory = fixture, OutputDirectory = output, Modules = modules, Targets = ["App"] };
        NativeBuildResult result = await new NativeBuildEngine().BuildAsync(planned with { PlanOnly = true, Unity = true, Pch = true, Adaptive = false });
        Check(result.Succeeded && result.CompilationDatabasePath is not null, "plan-only build");
        IdePlan ide = IdePlanner.Plan(planned);
        static string WithoutObject(IEnumerable<string> arguments) => string.Join('\u001f', arguments.Where(argument =>
            !argument.StartsWith("/Fo", StringComparison.Ordinal) && argument != "-o" && !argument.EndsWith(".obj", StringComparison.Ordinal) &&
            !argument.EndsWith(".o", StringComparison.Ordinal)));
        using JsonDocument database = JsonDocument.Parse(File.ReadAllBytes(result.CompilationDatabasePath!));
        string[] built = [.. database.RootElement.EnumerateArray()
            .Where(entry => !Path.GetFullPath(entry.GetProperty("file").GetString()!).StartsWith(output, StringComparison.OrdinalIgnoreCase))
            .Select(entry => Path.GetFullPath(entry.GetProperty("file").GetString()!) + "|" +
                WithoutObject(entry.GetProperty("arguments").EnumerateArray().Skip(1).Select(value => value.GetString()!)))
            .Order(StringComparer.Ordinal)];
        string[] planned2 = [.. ide.Compiles.Select(compile => Path.GetFullPath(compile.Source) + "|" + WithoutObject(compile.Arguments)).Order(StringComparer.Ordinal)];
        Check(built.Length == 7 && built.SequenceEqual(planned2, StringComparer.Ordinal), "IDE logical commands differ from the plan-only compile database");
        string pch = Path.Combine(libDirectory, "LibPch.h");
        Check(ide.Compiles.Where(compile => compile.Module == "Lib").All(compile => compile.PchHeader == pch), "module PCH survives one unity batch");
        Check(ide.Compiles.Single(compile => compile.Module == "User").PchHeader == Path.Combine(baseInclude, "shared", "BasePch.h"), "shared PCH header");
        Check(ide.Compiles.All(compile => !compile.Source.StartsWith(output, StringComparison.OrdinalIgnoreCase)), "generated source excluded");
    }

    private static void Model(NativeBuildRequest request, string root)
    {
        string fixture = Path.Combine(root, "ide-plan-model");
        string engine = Directory.CreateDirectory(Path.Combine(fixture, "Engine", "Lib")).FullName;
        string tests = Directory.CreateDirectory(Path.Combine(fixture, "Programs", "Tests")).FullName;
        string outside = Directory.CreateDirectory(Path.Combine(fixture, "Programs", "Helpers")).FullName;
        File.WriteAllText(Path.Combine(engine, "Lib.h"), "#pragma once\nint Lib() noexcept;\n");
        File.WriteAllText(Path.Combine(engine, "Lib.cpp"), "#include \"Lib.h\"\nint Lib() noexcept { return LIB_VALUE; }\n");
        File.WriteAllText(Path.Combine(engine, "Raw.c"), "int Raw(void) { return 1; }\n");
        File.WriteAllText(Path.Combine(tests, "Main.cpp"), "int main() { return 0; }\n");
        File.WriteAllText(Path.Combine(tests, "Both.cpp"), "int Both() noexcept { return 2; }\n");
        File.WriteAllText(Path.Combine(outside, "Helper.h"), "#pragma once\n");
        File.WriteAllText(Path.Combine(outside, "Helper.cpp"), "int Helper() noexcept { return 3; }\n");
        NativeModuleInput[] modules =
        [
            new() { Name = "Lib", Sources = [Path.Combine(engine, "Lib.cpp"), Path.Combine(engine, "Raw.c")], PublicIncludeDirectories = [engine], PublicDefines = ["LIB_VALUE=7"] },
            new() { Name = "Helpers", Sources = [Path.Combine(outside, "Helper.cpp")], PublicIncludeDirectories = [outside] },
            new() { Name = "Shared", Sources = [Path.Combine(tests, "Both.cpp")] },
            new() { Name = "SharedCopy", Sources = [Path.Combine(tests, "Both.cpp")], PrivateDefines = ["COPY=1"] },
            new() { Name = "Alpha", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(tests, "Main.cpp")], PublicDependencies = ["Lib", "Helpers"] },
            new() { Name = "Beta", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(tests, "Main.cpp")], PublicDependencies = ["Lib", "Shared"] },
            new() { Name = "Gamma", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(tests, "Main.cpp")], PublicDependencies = ["Lib", "SharedCopy"] }
        ];

        // 한 정의 디렉터리(Programs/Tests)에 타깃 셋. 정의 파일은 Gamma.Target.cs라는 가정이다(이름이 같은 Gamma가 동률을 먼저 가져간다).
        List<IdeTargetPlan> Plans(string? definitionName)
        {
            var plans = new List<IdeTargetPlan>();
            foreach (NativeConfiguration configuration in new[] { NativeConfiguration.Debug, NativeConfiguration.Development })
            {
                foreach (string target in new[] { "Alpha", "Beta", "Gamma" })
                {
                    plans.Add(new(target, tests, "Program", configuration.ToString(), IdePlanner.Plan(request with
                    {
                        RootDirectory = fixture,
                        OutputDirectory = Path.Combine(fixture, "out", target + "-" + configuration),
                        Modules = modules,
                        Targets = [target],
                        Configuration = configuration
                    }), definitionName));
                }
            }

            return plans;
        }

        string projectFiles = Path.Combine(fixture, "out", "ProjectFiles");
        string[] engineRoots = [Path.Combine(fixture, "Engine")];
        List<IdeTargetPlan> named = Plans("Gamma");
        IntelliSenseModel model = IntelliSenseModel.Create(named, fixture, projectFiles, engineRoots, "Development");
        Check(!Directory.Exists(Path.Combine(fixture, "out")), "model wrote files");
        string[] Names(IdeProject project) => [.. project.Sources.Select(file => Path.GetFileName(file.Path))];
        IdeProject Project(IntelliSenseModel value, string name) => value.Projects.Single(project => project.Name == name);
        IdeProject rocks = Project(model, IntelliSenseModel.EngineProjectName);
        Check(rocks.Configurations.Count == 0 && Names(rocks).SequenceEqual(["Lib.cpp", "Raw.c"]), "engine root files belong to the engine project");
        Check(rocks.Headers.Select(Path.GetFileName).SequenceEqual(["Lib.h"]), "engine headers");
        // (1) 정의 파일 이름과 같은 타깃: Gamma가 Main.cpp·Both.cpp를 모두 갖는다(Both.cpp는 Beta·Gamma가 컴파일하고 Alpha는 아니다).
        Check(Names(Project(model, "Gamma")).SequenceEqual(["Both.cpp", "Main.cpp"]) && Names(Project(model, "Alpha")).SequenceEqual(["Helper.cpp"]) &&
            Project(model, "Beta").Sources.Count == 0, "definition file name wins the tie: " +
            string.Join(" | ", model.Projects.Select(project => project.Name + "=" + string.Join(",", Names(project)))));
        IntelliSenseModel unnamed = IntelliSenseModel.Create(Plans(null), fixture, projectFiles, engineRoots, "Development");
        // (2) 이름이 같은 타깃이 없으면 기준 구성에서 컴파일하는 타깃 ordinal: Main.cpp는 Alpha, Both.cpp는 Beta(Alpha는 컴파일하지 않는다).
        Check(Names(Project(unnamed, "Alpha")).Contains("Main.cpp") && Names(Project(unnamed, "Beta")).SequenceEqual(["Both.cpp"]) &&
            Project(unnamed, "Gamma").Sources.Count == 0, "base configuration compile then ordinal");
        foreach (IntelliSenseModel value in new[] { model, unnamed })
        {
            IdeProject helperOwner = value.Projects.Single(project => project.Sources.Any(file => file.Path.EndsWith("Helper.cpp", StringComparison.Ordinal)));
            Check(helperOwner.Name == "Alpha" && helperOwner.Headers.Select(Path.GetFileName).SequenceEqual(["Helper.h"]),
                "outside a definition directory: the Program target compiling the file, headers follow the sources");
            Check(value.Notices.Count(notice => notice.Contains("Helpers", StringComparison.Ordinal)) == 2, "notices per directory: " + string.Join(" | ", value.Notices));
        }

        // Both.cpp는 Gamma 프로젝트에서 SharedCopy 명령을 고른다(자기 타깃의 명령이 먼저다). 값은 구성마다 다르다.
        IdeProject gamma = Project(model, "Gamma");
        IdeSourceFile both = gamma.Sources.Single(file => file.Path.EndsWith("Both.cpp", StringComparison.Ordinal));
        IdeBundle bothBundle = gamma.Bundles[both.Bundle!.Value];
        Check(both.Choices["Gamma|Debug"]!.Compile.Module == "SharedCopy" &&
            model.DefinitionCopies[Path.GetFileName(bothBundle.Values["Gamma|Debug"]!.ForcedIncludes[0])].Contains("#define COPY 1", StringComparison.Ordinal),
            "own target's command first");
        Check(bothBundle.Values["Gamma|Debug"]!.Defines.Contains("ROCKS_BUILD_DEBUG=1") &&
            bothBundle.Values["Gamma|Development"]!.Defines.Contains("ROCKS_BUILD_DEVELOPMENT=1"), "values differ per configuration");
        Check(gamma.Bundles.Count == 2 && gamma.DefaultBundle is not null, "one bundle per distinct value tuple");
        Check(model.DefinitionCopies.Keys.All(name => name.Length == 16 + 2) &&
            model.DefinitionCopies.Values.All(content => !content.Contains(fixture, StringComparison.OrdinalIgnoreCase)), "definition copies are content named and path free");

        // 모델 검사: 접힌 값이 고른 명령과 왕복한다. 묶음 값을 하나 바꾸면 잡는다.
        Check(IntelliSenseModel.Verify(model, named, projectFiles).Count == 0, "model verification: " + string.Join(" | ", IntelliSenseModel.Verify(model, named, projectFiles)));
        IntelliSenseValues original = bothBundle.Values["Gamma|Debug"]!;
        var tampered = new Dictionary<string, IntelliSenseValues?>(bothBundle.Values) { ["Gamma|Debug"] = original with { Defines = [.. original.Defines.Skip(1)] } };
        IdeProject broken = gamma with { Bundles = [.. gamma.Bundles.Select(bundle => bundle.Number == bothBundle.Number ? bundle with { Values = tampered } : bundle)] };
        IntelliSenseModel brokenModel = model with { Projects = [.. model.Projects.Select(project => project.Name == "Gamma" ? broken : project)] };
        Check(IntelliSenseModel.Verify(brokenModel, named, projectFiles).Count == 1, "verification catches a folded value that differs from its command");
        var hintless = new Dictionary<string, IntelliSenseValues?>(bothBundle.Values) { ["Gamma|Debug"] = original with { Options = [.. original.Options, "/TP"] } };
        brokenModel = model with
        {
            Projects = [.. model.Projects.Select(project => project.Name == "Gamma"
                ? gamma with { Bundles = [.. gamma.Bundles.Select(bundle => bundle.Number == bothBundle.Number ? bundle with { Values = hintless } : bundle)] }
                : project)]
        };
        Check(IntelliSenseModel.Verify(brokenModel, named, projectFiles).Count == 1, "verification catches a wrong language option");

        // 계획 순서를 섞어도 같은 모델이다.
        string Serialize(IntelliSenseModel value) => string.Join("\n", value.Projects.Select(project => project.Name + ":" +
            string.Join(",", project.Configurations.Select(configuration => configuration.Key)) + ":" +
            string.Join(",", project.Sources.Select(file => file.Path + "#" + file.Bundle)) + ":" +
            string.Join(",", project.Bundles.Select(bundle => string.Join(";", bundle.Values.Select(pair => pair.Key + "=" + pair.Value?.Canonical()))))));
        List<IdeTargetPlan> reversed = [.. named];
        reversed.Reverse();
        Check(Serialize(IntelliSenseModel.Create(reversed, fixture, projectFiles, engineRoots, "Development")) == Serialize(model), "plan order changes the model");

        // Raw.c: /TC는 확장자와 같아 옵션에 남지 않는다.
        IdeCompile raw = named[0].Plan.Compiles.Single(compile => compile.Source.EndsWith("Raw.c", StringComparison.Ordinal));
        IntelliSenseValues rawValues = IntelliSenseModel.Values(raw, named[0].Plan, (_, content) => Path.Combine(projectFiles, "Definitions", IntelliSenseModel.CopyName(content)));
        Check(!rawValues.Options.Contains("/TC") && rawValues.Options.Contains("/std:c17"), "C source language and standard");
        // 정의 헤더도 PCH 헤더도 아닌 강제 include는 분류 오류다.
        IdeCompile stray = raw with { Arguments = [.. raw.Arguments, @"/FIC:\stray.h"] };
        Throws<ArgumentException>(() => IntelliSenseModel.Values(stray, named[0].Plan, (_, _) => "x"), "neither a definitions header", "stray forced include");

        // 엔진 루트에 같은 종류 타깃 둘이면 구성 이름이 겹친다.
        Throws<ArgumentException>(() => IntelliSenseModel.Create([named[0] with { DefinitionDirectory = engine }, named[1] with { DefinitionDirectory = engine }],
            fixture, projectFiles, engineRoots, "Development"), "share the configuration names", "engine project configuration collision");
    }

    private static void Throws<T>(Action action, string fragment, string what) where T : Exception
    {
        try
        {
            action();
        }
        catch (T error)
        {
            Check(error.Message.Contains(fragment, StringComparison.Ordinal), what + ": unexpected message " + error.Message);
            return;
        }

        throw new InvalidOperationException(what + ": no exception");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
