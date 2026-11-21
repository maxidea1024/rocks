using Rocks.Build;

/// <summary>
/// IDE 계획 모드와 IntelliSense 모델(Docs/Design/ProjectFiles.ko.md 6.2·8절, PJ0)을 검사한다.
/// <list type="bullet">
/// <item>인자 분류표: 표의 모든 철자, 소스, 언어 표지, 모르는 인자 거부.</item>
/// <item>IDE 계획: 없는 생성물(리플렉션 생성 소스·출력 아래 include 디렉터리·prebuilt)을 요구하지 않고 출력에 아무것도 쓰지 않는다.
/// 손으로 쓴 소스의 논리 명령은 빌드 계획과 같다(object 경로 제외). 실행 파일 출력 경로를 싣는다.</item>
/// <item>모델: 정의 디렉터리 기준 프로젝트, 경로 기준 파일 소유와 동률 규칙, (파일, 구성) 원천 명령, 묶음 접기, 정의 헤더 사본, 왕복,
/// 엔진 프로젝트 구성 충돌 거부.</item>
/// </list>
/// </summary>
internal static class IdePlanTests
{
    public static void Run(NativeBuildRequest request, string root, Action<string> pass)
    {
        Classification();
        pass("IntelliSense argument table: pass-through, translated, dropped, language, source, unknown rejected");

        IdePlanAndModel(request, root);
        pass("IDE plan mode: no generated inputs required, nothing written, logical commands equal the build plan; IntelliSense model ownership, bundles, copies, round trip");
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
            try
            {
                IntelliSenseArguments.Classify(unknown, source);
                throw new InvalidOperationException("Unknown argument was classified: " + unknown);
            }
            catch (ArgumentException error)
            {
                Check(error.Message.Contains(unknown, StringComparison.Ordinal), "unknown argument message names it: " + unknown);
            }
        }
    }

    private static void IdePlanAndModel(NativeBuildRequest request, string root)
    {
        string fixture = Path.Combine(root, "ide-plan");
        string engine = Directory.CreateDirectory(Path.Combine(fixture, "Engine", "Lib")).FullName;
        string programs = Directory.CreateDirectory(Path.Combine(fixture, "Programs", "Tool")).FullName;
        string shared = Directory.CreateDirectory(Path.Combine(fixture, "Programs", "Tool", "Shared")).FullName;
        File.WriteAllText(Path.Combine(engine, "Lib.h"), "#pragma once\nint Lib() noexcept;\n");
        File.WriteAllText(Path.Combine(engine, "Lib.cpp"), "#include \"Lib.h\"\nint Lib() noexcept { return LIB_VALUE; }\n");
        File.WriteAllText(Path.Combine(engine, "Raw.c"), "int Raw(void) { return 1; }\n");
        File.WriteAllText(Path.Combine(programs, "Main.cpp"), "#include \"Lib.h\"\nint main() { return Lib() == 7 ? 0 : 1; }\n");
        File.WriteAllText(Path.Combine(shared, "Both.cpp"), "int Both() noexcept { return 2; }\n");
        string output = Path.Combine(fixture, "out", "debug");
        string generatedInclude = Path.Combine(output, "Generated", "Lib", "Public");
        string generatedSource = Path.Combine(output, "Generated", "Lib", "Source", "Lib.module.generated.cpp");
        NativeModuleInput[] modules =
        [
            new()
            {
                Name = "Lib", Sources = [Path.Combine(engine, "Lib.cpp"), Path.Combine(engine, "Raw.c")], PublicIncludeDirectories = [engine, generatedInclude],
                PublicDefines = ["LIB_VALUE=7"], ReflectionSources = [generatedSource]
            },
            new() { Name = "Shared", Sources = [Path.Combine(shared, "Both.cpp")] },
            new() { Name = "SharedCopy", Sources = [Path.Combine(shared, "Both.cpp")], PrivateDefines = ["COPY=1"] },
            new()
            {
                Name = "Tool", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(programs, "Main.cpp")],
                PublicDependencies = ["Lib", "Shared"], PrebuiltLibraries = [Path.Combine(fixture, "missing", "prebuilt.lib")]
            },
            new()
            {
                Name = "Other", Kind = NativeModuleKind.Executable, Sources = [Path.Combine(programs, "Main.cpp")],
                PublicDependencies = ["Lib", "SharedCopy"]
            }
        ];
        NativeBuildRequest Request(string target, string configurationOutput, NativeConfiguration configuration) => request with
        {
            RootDirectory = fixture,
            OutputDirectory = configurationOutput,
            Modules = modules,
            Targets = [target],
            Configuration = configuration
        };

        // 1. IDE 계획은 없는 생성물을 요구하지 않고 출력에 아무것도 쓰지 않는다. 빌드 계획은 같은 요청을 거부한다.
        IdePlan tool = IdePlanner.Plan(Request("Tool", output, NativeConfiguration.Debug));
        Check(!Directory.Exists(Path.Combine(fixture, "out")), "IDE plan wrote into the output directory");
        Check(tool.Compiles.All(compile => !compile.Source.Contains("generated", StringComparison.Ordinal)), "IDE plan kept a generated source");
        Check(tool.ModuleOutputs["Tool"].EndsWith(OperatingSystem.IsWindows() ? "Tool.exe" : "Tool", StringComparison.Ordinal), "executable output path");
        Check(tool.GeneratedFiles.Keys.Any(path => path.EndsWith("Lib.Definitions.h", StringComparison.Ordinal)), "definitions header content");
        try
        {
            ActionGraph.Create(Request("Tool", output, NativeConfiguration.Debug));
            throw new InvalidOperationException("Build plan accepted a missing generated source");
        }
        catch (FileNotFoundException)
        {
        }

        // 2. 손으로 쓴 소스의 논리 명령은 빌드 계획과 같다(생성물을 뺀 같은 요청, 유니티·작업 파일 분리와 무관).
        NativeModuleInput[] handWritten = modules.Select(module => module with
        {
            ReflectionSources = [],
            PrebuiltLibraries = [],
            PublicIncludeDirectories = module.PublicIncludeDirectories.Where(directory => directory != generatedInclude).ToArray()
        }).ToArray();
        NativePlan build = ActionGraph.Create(request with
        {
            RootDirectory = fixture,
            OutputDirectory = output,
            Modules = handWritten,
            Targets = ["Tool"],
            Unity = true,
            Adaptive = false
        });
        IdePlan reference = IdePlanner.Plan(request with { RootDirectory = fixture, OutputDirectory = output, Modules = handWritten, Targets = ["Tool"] });
        static string[] WithoutObject(IEnumerable<string> arguments) =>
            [.. arguments.Where(argument => !argument.StartsWith("/Fo", StringComparison.Ordinal) && !argument.StartsWith("-o", StringComparison.Ordinal))];
        foreach (IdeCompile compile in reference.Compiles)
        {
            LogicalCompile logical = build.LogicalCompiles.Single(value => value.Module == compile.Module && value.Source == compile.Source);
            Check(WithoutObject(compile.Arguments).SequenceEqual(WithoutObject(NativeAction.Resolve(logical.Arguments, [logical.Object]))),
                "IDE logical command differs from the build plan for " + compile.Source);
        }

        if (request.Toolchain.Platform != NativePlatform.Windows)
        {
            // 분류표는 clang-cl 철자만 받는다(IntelliSense는 Windows 계획만 쓴다, 6.3절).
            return;
        }

        // 3. 모델: Lib는 엔진 루트, Tool·Other는 같은 정의 디렉터리. Both.cpp는 두 모듈(Shared·SharedCopy)이 컴파일한다.
        var plans = new List<IdeTargetPlan>();
        foreach (NativeConfiguration configuration in new[] { NativeConfiguration.Debug, NativeConfiguration.Development })
        {
            foreach (string target in new[] { "Tool", "Other" })
            {
                // 생성물 경로는 1번의 출력 하나에 묶여 있으므로 손으로 쓴 모듈만 쓴다(타깃·구성마다 출력이 다르다).
                plans.Add(new(target, programs, "Program", configuration.ToString(), IdePlanner.Plan(request with
                {
                    RootDirectory = fixture,
                    OutputDirectory = Path.Combine(fixture, "out", target + "-" + configuration),
                    Modules = handWritten,
                    Targets = [target],
                    Configuration = configuration
                })));
            }
        }

        string projectFiles = Path.Combine(fixture, "out", "ProjectFiles");
        IntelliSenseModel model = IntelliSenseModel.Create(plans, fixture, projectFiles, [Path.Combine(fixture, "Engine")], "Development");
        Check(!Directory.Exists(Path.Combine(fixture, "out")), "model wrote files");
        IdeProject rocks = model.Projects.Single(project => project.Name == IntelliSenseModel.EngineProjectName);
        IdeProject toolProject = model.Projects.Single(project => project.Name == "Tool");
        IdeProject otherProject = model.Projects.Single(project => project.Name == "Other");
        Check(rocks.Configurations.Count == 0 && rocks.Sources.Select(file => Path.GetFileName(file.Path)).SequenceEqual(["Lib.cpp", "Raw.c"]),
            "engine root files belong to the engine project");
        Check(rocks.Headers.Select(Path.GetFileName).SequenceEqual(["Lib.h"]), "headers from module include directories, generated directories excluded");
        // 같은 정의 디렉터리의 두 타깃: 기준 구성에서 그 파일을 컴파일하는 ordinal 첫 타깃(Other)이 갖는다. Both.cpp는 Other만 컴파일한다.
        Check(otherProject.Sources.Select(file => Path.GetFileName(file.Path)).SequenceEqual(["Main.cpp", "Both.cpp"]) && toolProject.Sources.Count == 0,
            "tie between targets in one definition directory: Other " + string.Join(", ", otherProject.Sources.Select(file => file.Path)) +
            "; Tool " + string.Join(", ", toolProject.Sources.Select(file => file.Path)));
        Check(otherProject.Configurations.Select(value => value.Key).SequenceEqual(["Other|Debug", "Other|Development"]), "project configurations");
        IdeBundle both = otherProject.Bundles[otherProject.Sources.Single(file => file.Path.EndsWith("Both.cpp", StringComparison.Ordinal)).Bundle!.Value];
        Check(both.Values["Other|Debug"]!.ForcedIncludes.Count == 1 &&
            model.DefinitionCopies[Path.GetFileName(both.Values["Other|Debug"]!.ForcedIncludes[0])].Contains("#define COPY 1", StringComparison.Ordinal),
            "Both.cpp takes the SharedCopy command of its own target");
        Check(both.Values["Other|Debug"]!.Defines.Contains("ROCKS_BUILD_DEBUG=1") && both.Values["Other|Development"]!.Defines.Contains("ROCKS_BUILD_DEVELOPMENT=1"),
            "values differ per configuration");
        Check(otherProject.Bundles.Count == 2 && otherProject.DefaultBundle is not null, "one bundle per distinct value tuple");
        Check(model.DefinitionCopies.Keys.All(name => name.Length == 16 + 2) &&
            model.DefinitionCopies.Values.All(content => !content.Contains(fixture, StringComparison.OrdinalIgnoreCase)), "definition copies are content named and path free");
        // Lib.cpp는 엔진 프로젝트(구성 없음)에 있으므로 IntelliSense 값이 없다. Raw.c는 /TC가 확장자와 같아 옵션에 남지 않는다.
        IdeCompile raw = plans[0].Plan.Compiles.Single(compile => compile.Source.EndsWith("Raw.c", StringComparison.Ordinal));
        IntelliSenseValues rawValues = IntelliSenseModel.Values(raw, plans[0].Plan, (_, content) => Path.Combine(projectFiles, "Definitions", IntelliSenseModel.CopyName(content)));
        Check(!rawValues.Options.Contains("/TC") && rawValues.Options.Contains("/std:c17"), "C source language and standard");
        foreach (IdeTargetPlan plan in plans)
        {
            foreach (IdeCompile compile in plan.Plan.Compiles)
            {
                IntelliSenseValues values = IntelliSenseModel.Values(compile, plan.Plan,
                    (_, content) => Path.Combine(projectFiles, "Definitions", IntelliSenseModel.CopyName(content)));
                Check(IntelliSenseModel.RoundTrip(compile, plan.Plan, values, projectFiles) is null, "round trip " + compile.Source);
            }
        }

        // 4. 엔진 루트에 같은 종류 타깃 둘이면 구성 이름이 겹친다.
        try
        {
            IntelliSenseModel.Create([plans[0] with { DefinitionDirectory = engine }, plans[1] with { DefinitionDirectory = engine }], fixture, projectFiles,
                [Path.Combine(fixture, "Engine")], "Development");
            throw new InvalidOperationException("Engine project configuration collision accepted");
        }
        catch (ArgumentException error)
        {
            Check(error.Message.Contains("share the configuration names", StringComparison.Ordinal), "collision message");
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
