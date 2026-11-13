namespace Rocks.Build;

internal sealed record NativeAction(string Id, string Module, string Kind, string Tool,
    IReadOnlyList<string> Arguments, IReadOnlyList<string> Outputs, IReadOnlyList<string> Inputs,
    IReadOnlyList<string> HeaderDirectories, IReadOnlyList<string> DeclaredInputs,
    IReadOnlyList<string> Dependencies, IReadOnlyList<string> Identity, string? Source = null)
{
    public string Key(string toolchain, string outputRoot, string root) => Fingerprints.Strings(new[]
    {
        toolchain, Id, Tool, Fingerprints.Paths(Inputs), Fingerprints.Paths(HeaderDirectories, true, outputRoot, root),
        Fingerprints.Paths(DeclaredInputs)
    }.Concat(Identity).Concat(Arguments));

    public string[] ResolveArguments(IReadOnlyList<string> outputs)
    {
        return Arguments.Select(argument =>
        {
            for (int index = 0; index < outputs.Count; ++index)
            {
                argument = argument.Replace($"{{OUTPUT{index}}}", outputs[index], StringComparison.Ordinal);
            }

            return argument;
        }).ToArray();
    }
}

internal sealed record NativePlan(IReadOnlyList<NativeAction> Actions,
    IReadOnlyDictionary<string, string> ModuleOutputs);

internal static class ActionGraph
{
    public static NativePlan Create(NativeBuildRequest request)
    {
        if (request.MaxParallelism < 1)
        {
            throw new ArgumentException("MaxParallelism must be positive.");
        }

        string root = Path.GetFullPath(request.RootDirectory);
        string output = Path.GetFullPath(request.OutputDirectory, root);
        var modules = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (var module in request.Modules)
        {
            if (module.Name.Length == 0 || module.Name is "." or ".." ||
                module.Name.Any(value => !char.IsAsciiLetterOrDigit(value) && value is not '_' and not '-' and not '.') ||
                !modules.TryAdd(module.Name, module))
            {
                throw new ArgumentException($"Invalid or duplicate module name: {module.Name}");
            }

            if (module.Kind == NativeModuleKind.Executable && module.Sources.Count == 0)
            {
                throw new ArgumentException($"Executable requires a source: {module.Name}");
            }
        }

        var ordered = new List<NativeModuleInput>();
        var marks = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        void Visit(string name)
        {
            if (!modules.TryGetValue(name, out var module))
            {
                throw new ArgumentException($"Unknown module: {name}");
            }

            if (marks.TryGetValue(name, out int mark))
            {
                if (mark == 1)
                {
                    throw new ArgumentException($"Module dependency cycle at {name}");
                }

                return;
            }

            marks[name] = 1;
            foreach (string dependency in module.Dependencies)
            {
                if (!modules.ContainsKey(dependency))
                {
                    throw new ArgumentException($"Module {module.Name}, Dependencies: unknown module {dependency}.");
                }

                if (modules.TryGetValue(dependency, out var candidate) && candidate.Kind == NativeModuleKind.Executable)
                {
                    throw new ArgumentException("A module cannot depend on an executable.");
                }

                Visit(dependency);
            }

            marks[name] = 2;
            ordered.Add(module);
        }

        IEnumerable<string> targets = request.Targets.Count == 0 ? modules.Keys.Order(StringComparer.Ordinal) : request.Targets;
        foreach (string target in targets)
        {
            Visit(target);
        }

        string Absolute(string path) => Path.GetFullPath(path, root);
        var actions = new List<NativeAction>();
        var outputs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var terminalActions = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        foreach (var module in ordered)
        {
            var reachable = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var dependencyOrder = new List<NativeModuleInput>();
            void Reach(string name)
            {
                if (!reachable.Add(name))
                {
                    return;
                }

                foreach (string dependency in modules[name].Dependencies)
                {
                    Reach(dependency);
                }

                if (!string.Equals(name, module.Name, StringComparison.OrdinalIgnoreCase))
                {
                    dependencyOrder.Add(modules[name]);
                }
            }

            Reach(module.Name);
            var dependencies = dependencyOrder.ToArray();
            string[] includes = module.PublicIncludeDirectories.Concat(module.PrivateIncludeDirectories)
                .Concat(dependencies.SelectMany(value => value.PublicIncludeDirectories)).Select(Absolute)
                .Distinct(NativeToolchains.PathComparer).ToArray();
            string[] defines = module.PublicDefines.Concat(module.PrivateDefines)
                .Concat(dependencies.SelectMany(value => value.PublicDefines)).Distinct(StringComparer.Ordinal).ToArray();
            foreach (var define in defines.GroupBy(value => value.Split('=')[0], StringComparer.Ordinal))
            {
                if (define.Count() != 1)
                {
                    throw new ArgumentException($"Module {module.Name}: conflicting propagated definition {define.Key} ({string.Join(", ", define)}).");
                }
            }

            string[] sources = module.Sources.Select(Absolute).Order(StringComparer.Ordinal).ToArray();
            if (sources.Distinct(NativeToolchains.PathComparer).Count() != sources.Length)
            {
                throw new ArgumentException($"Duplicate source in {module.Name}");
            }

            foreach (string source in sources)
            {
                if (!File.Exists(source))
                {
                    throw new FileNotFoundException("Source does not exist.", source);
                }
            }

            foreach (string include in includes)
            {
                if (!Directory.Exists(include))
                {
                    throw new DirectoryNotFoundException(include);
                }
            }

            string[] headers = includes.Concat(sources.Select(value => Path.GetDirectoryName(value)!))
                .Distinct(NativeToolchains.PathComparer).ToArray();
            string[] extraInputs = module.AdditionalInputs.Concat(dependencies.SelectMany(value => value.AdditionalInputs))
                .Select(Absolute).Distinct(NativeToolchains.PathComparer).ToArray();
            var objectPaths = new List<string>();
            var compileIds = new List<string>();
            foreach (string source in sources)
            {
                string identity = Fingerprints.Strings([source])[..20];
                string objectPath = Path.Combine(output, "obj", module.Name, identity + (windows ? ".obj" : ".o"));
                string id = module.Name + ":compile:" + identity;
                NativeLanguage language = LanguageOf(source);
                var arguments = new List<string>();
                arguments.AddRange(module.AdditionalCompileArguments);
                arguments.AddRange(NativeCompileProfile.Arguments(request.Toolchain, request.Configuration, module.Optimization, language));
                // ThinLTO는 코드 생성 옵션이라 AST 프로필(NativeCompileProfile)에 넣지 않는다.
                if (UsesThinLto(request.Configuration, module.Optimization))
                {
                    arguments.Add("-flto=thin");
                }

                arguments.AddRange(defines.Select(value => (windows ? "/D" : "-D") + value));
                foreach (string include in includes)
                {
                    arguments.Add((windows ? "/I" : "-I") + include);
                }
                // C++ 번역 단위의 인자는 C 지원 이전과 byte 단위로 같아야 한다(action key가 인자를 포함하므로
                // 바뀌면 모든 증분 빌드가 무효화된다). macOS·Android의 compiler는 clang++ driver라서 C 소스에는
                // 언어를 명시한다(없으면 C++로 처리하며 -Werror에서 경고가 오류가 된다).
                if (windows)
                {
                    arguments.AddRange(["/c", language == NativeLanguage.C ? "/TC" : "/TP", source, "/Fo{OUTPUT0}"]);
                }
                else if (language == NativeLanguage.C)
                {
                    arguments.AddRange(["-c", "-x", "c", source, "-o", "{OUTPUT0}"]);
                }
                else
                {
                    arguments.AddRange(["-c", source, "-o", "{OUTPUT0}"]);
                }

                actions.Add(new(id, module.Name, "Compile", request.Toolchain.CompilerPath, arguments,
                    [objectPath], [source], headers, extraInputs, [],
                    new[] { request.ContextFingerprint, request.Configuration.ToString(), module.Optimization.ToString() }.Concat(sources).ToArray(), source));
                objectPaths.Add(objectPath);
                compileIds.Add(id);
            }
            // Base/MetaSchema 같은 header-only 모듈은 설정만 전파하고 가짜 빈 archive를 만들지 않는다.
            if (sources.Length == 0)
            {
                continue;
            }

            string finalPath = Path.Combine(output, module.Kind == NativeModuleKind.StaticLibrary ? "lib" : "bin",
                module.Name + (module.Kind == NativeModuleKind.StaticLibrary ? (windows ? ".lib" : ".a") : (windows ? ".exe" : "")));
            string terminalId = module.Name + (module.Kind == NativeModuleKind.StaticLibrary ? ":archive" : ":link");
            var finalArguments = new List<string>();
            var finalOutputs = new List<string> { finalPath };
            var finalInputs = new List<string>(objectPaths);
            var predecessors = new List<string>(compileIds);
            string tool;
            if (module.Kind == NativeModuleKind.StaticLibrary)
            {
                tool = request.Toolchain.ArchiverPath;
                finalArguments.AddRange(windows ? ["/nologo", "/out:{OUTPUT0}"] : ["rcs", "{OUTPUT0}"]);
                finalArguments.AddRange(objectPaths);
            }
            else
            {
                tool = request.Toolchain.CompilerPath;
                finalArguments.AddRange(request.Toolchain.LinkArguments);
                finalArguments.AddRange(LinkDriverFlags(request));
                finalArguments.AddRange(objectPaths);
                // 정적 라이브러리는 의존하는 쪽을 먼저 둔다. diamond에서도 각 archive는 한 번만 쓴다.
                foreach (var dependency in dependencies.Reverse())
                {
                    if (outputs.TryGetValue(dependency.Name, out string? library))
                    {
                        finalArguments.Add(library);
                        finalInputs.Add(library);
                        predecessors.Add(terminalActions[dependency.Name]);
                    }
                }

                foreach (string library in new[] { module }.Concat(dependencies.Reverse())
                    .SelectMany(value => value.PrebuiltLibraries).Select(Absolute).Distinct(NativeToolchains.PathComparer))
                {
                    if (!File.Exists(library))
                    {
                        throw new FileNotFoundException("Prebuilt library does not exist.", library);
                    }

                    finalArguments.Add(library);
                    finalInputs.Add(library);
                }

                finalArguments.AddRange(new[] { module }.Concat(dependencies.Reverse())
                    .SelectMany(value => value.AdditionalLinkArguments));
                // 시스템 라이브러리·framework는 이 모듈과 모든 의존 모듈의 선언을 모아 한 번씩 넣는다.
                finalArguments.AddRange(new[] { module }.Concat(dependencies.Reverse())
                    .SelectMany(value => value.SystemLibraries).Distinct(StringComparer.Ordinal)
                    .Select(name => windows ? name + ".lib" : "-l" + name));
                foreach (string framework in new[] { module }.Concat(dependencies.Reverse())
                    .SelectMany(value => value.Frameworks).Distinct(StringComparer.Ordinal))
                {
                    finalArguments.AddRange(["-framework", framework]);
                }

                if (windows)
                {
                    string? asanThunk = null;
                    if (request.Configuration == NativeConfiguration.Asan)
                    {
                        if (request.Toolchain.RuntimeFiles.Count != 1)
                        {
                            throw new ArgumentException($"Module {module.Name}: Windows ASan requires exactly one x64 runtime DLL.");
                        }

                        string directory = Path.GetDirectoryName(request.Toolchain.RuntimeFiles[0])!;
                        string asanLibrary = Path.ChangeExtension(request.Toolchain.RuntimeFiles[0], ".lib");
                        asanThunk = Directory.EnumerateFiles(directory, "clang_rt.asan_dynamic_runtime_thunk*.lib").SingleOrDefault();
                        if (!File.Exists(asanLibrary) || asanThunk == null)
                        {
                            throw new FileNotFoundException($"Module {module.Name}: matching ASan import/thunk libraries are missing in {directory}.");
                        }

                        finalArguments.Add(asanLibrary);
                        finalInputs.AddRange([asanLibrary, asanThunk]);
                    }

                    finalArguments.AddRange(["/Fe{OUTPUT0}", "/link", "/INCREMENTAL:NO", "/SUBSYSTEM:CONSOLE", "/MANIFEST:EMBED"]);
                    if (asanThunk != null)
                    {
                        finalArguments.AddRange(["/INCLUDE:__asan_seh_interceptor", "/WHOLEARCHIVE:" + asanThunk]);
                    }
                    // 모든 구성이 PDB를 남긴다. Release PDB는 배포 파일과 분리해 크래시 분석용으로 보관한다.
                    finalOutputs.Add(Path.ChangeExtension(finalPath, ".pdb"));
                    finalArguments.AddRange(["/DEBUG:FULL", "/PDB:{OUTPUT1}"]);
                    finalArguments.AddRange(new[] { "kernel32", "user32", "gdi32", "winspool", "shell32", "ole32", "oleaut32", "uuid", "comdlg32", "advapi32" }
                        .Select(value => "/DEFAULTLIB:" + value + ".lib"));
                }
                else
                {
                    finalArguments.AddRange(["-o", "{OUTPUT0}"]);
                }
            }

            actions.Add(new(terminalId, module.Name, module.Kind == NativeModuleKind.StaticLibrary ? "Archive" : "Link",
                tool, finalArguments, finalOutputs, finalInputs, [], extraInputs, predecessors,
                [request.ContextFingerprint, request.Configuration.ToString()]));
            outputs[module.Name] = finalPath;
            terminalActions[module.Name] = terminalId;
        }

        if (request.Configuration == NativeConfiguration.Asan && actions.Any(value => value.Kind == "Link"))
        {
            foreach (string runtime in request.Toolchain.RuntimeFiles.Distinct(NativeToolchains.PathComparer))
            {
                string destination = Path.Combine(output, "bin", Path.GetFileName(runtime));
                actions.Add(new("runtime:" + Path.GetFileName(runtime), "Runtime", "Copy", "", [],
                    [destination], [runtime], [], [], [], [request.ContextFingerprint]));
            }
        }

        if (actions.SelectMany(value => value.Outputs).Distinct(NativeToolchains.PathComparer).Count() != actions.Sum(value => value.Outputs.Count))
        {
            throw new ArgumentException("More than one action produces the same output.");
        }

        var generated = actions.SelectMany(value => value.Outputs).ToHashSet(NativeToolchains.PathComparer);
        foreach (var module in ordered)
        {
            foreach (string input in module.Sources.Concat(module.AdditionalInputs).Concat(module.PrebuiltLibraries).Select(Absolute))
            {
                if (generated.Contains(input))
                {
                    throw new ArgumentException($"Module {module.Name}: output overlaps declared input {input}.");
                }
            }
        }

        return new(actions, outputs);
    }

    /// <summary>구성과 모듈 재정의로 최적화 여부를 정한다. Debug·Asan은 끄고 Development·Release는 켠다.</summary>
    internal static bool Optimizes(NativeConfiguration configuration, CodeOptimization optimization) => optimization switch
    {
        CodeOptimization.Always => true,
        CodeOptimization.Never => false,
        CodeOptimization.Default => configuration is NativeConfiguration.Development or NativeConfiguration.Release,
        _ => throw new ArgumentException($"Invalid code optimization: {optimization}")
    };

    /// <summary>Release의 최적화된 모듈만 ThinLTO bitcode로 만든다. 최적화를 끈 모듈은 일반 object로 남는다.</summary>
    internal static bool UsesThinLto(NativeConfiguration configuration, CodeOptimization optimization) =>
        configuration == NativeConfiguration.Release && Optimizes(configuration, optimization);

    /// <summary>확장자가 정확히 소문자 <c>.c</c>인 소스만 C다. 대문자 <c>.C</c>는 관례상 C++이므로 C로 보지 않는다.</summary>
    internal static NativeLanguage LanguageOf(string source) =>
        Path.GetExtension(source).Equals(".c", StringComparison.Ordinal) ? NativeLanguage.C : NativeLanguage.Cpp;

    internal static IEnumerable<string> CompileFlags(NativeToolchain toolchain, NativeConfiguration configuration,
        CodeOptimization optimization = CodeOptimization.Default, NativeLanguage language = NativeLanguage.Cpp)
    {
        if (!Enum.IsDefined(language))
        {
            throw new ArgumentException($"Invalid native language: {language}");
        }

        bool c = language == NativeLanguage.C;
        bool release = configuration == NativeConfiguration.Release;
        bool asan = configuration == NativeConfiguration.Asan;
        bool optimize = Optimizes(configuration, optimization);
        // 구성 매크로는 정확히 하나만 1로 정의한다(Base/Compiler.h가 확인한다). Asan은 Debug 계열이다.
        string buildMacro = NativeConfigurations.BuildMacro(configuration);
        if (toolchain.Platform == NativePlatform.Windows)
        {
            yield return "/nologo";
            yield return "/utf-8";
            yield return "/W4";
            yield return "/WX";
            if (c)
            {
                // C17. RTTI·예외·__cplusplus 보고는 C++ 전용이다. 경고 수준과 오류 처리는 C++와 같다.
                yield return "/clang:-std=c17";
            }
            else
            {
                yield return "/GR-";
                yield return "/EHs-c-";
                yield return "/Zc:__cplusplus";
                yield return "/clang:-std=c++23";
            }
            // 부동소수점 축약(FMA) 금지: ARM64처럼 FMA가 항상 있는 CPU에서만 a*b+c가 fused되어
            // 플랫폼별 결과·판정(예: 행렬 역행렬 성공 여부)이 달라지는 것을 막는다.
            yield return "/clang:-ffp-contract=off";
            yield return "/DWIN32";
            yield return "/D_WINDOWS";
            yield return "/DNOMINMAX";
            yield return "/DWIN32_LEAN_AND_MEAN";
            yield return "/DROCKS_PLATFORM_WINDOWS=1";
            yield return "/D" + buildMacro;
            // 디버그 CRT는 Debug에서만 쓴다. 모듈 재정의는 CRT를 바꾸지 않는다(링크 시 CRT 불일치 방지).
            yield return configuration == NativeConfiguration.Debug ? "/MDd" : "/MD";
            yield return optimize ? "/O2" : "/Od";
            // 디버그 정보는 모든 구성에서 만든다(UE Shipping처럼 배포본 크래시 덤프를 심볼화한다).
            if (release)
            {
                yield return "/DNDEBUG";
            }

            yield return "/Z7";
            if (asan)
            {
                yield return "/fsanitize=address";
            }
        }
        else
        {
            string[] languageFlags = c
                ? ["-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-ffp-contract=off"]
                : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-rtti", "-fno-exceptions", "-ffp-contract=off"];
            foreach (string flag in languageFlags)
            {
                yield return flag;
            }

            if (toolchain.Platform == NativePlatform.Android)
            {
                // Android 실행 파일·공유 라이브러리는 PIE/PIC가 필수다.
                yield return "-fPIC";
                yield return "-DROCKS_PLATFORM_ANDROID=1";
            }
            else
            {
                yield return "-DROCKS_PLATFORM_MAC=1";
            }

            yield return "-D" + buildMacro;
            yield return optimize ? "-O2" : "-O0";
            if (release)
            {
                yield return "-DNDEBUG";
            }

            yield return "-g";
            if (asan)
            {
                yield return "-fsanitize=address";
                yield return "-fno-omit-frame-pointer";
            }
        }
    }

    private static IEnumerable<string> LinkDriverFlags(NativeBuildRequest request)
    {
        if (request.Toolchain.Platform == NativePlatform.Windows)
        {
            yield return "/nologo";
            yield return "/WX";
            yield return request.Configuration == NativeConfiguration.Debug ? "/MDd" : "/MD";
            if (request.Toolchain.LinkerPath != null && Path.GetFileNameWithoutExtension(request.Toolchain.LinkerPath).Equals("lld-link", StringComparison.OrdinalIgnoreCase))
            {
                yield return "-fuse-ld=lld";
            }
            // Windows ASan runtime 위치는 배포판별로 다르므로 위에서 검증한 import/thunk를 넘긴다.
            // /fsanitize=address는 compile에만 사용해 driver의 잘못된 triple 경로 추정을 피한다.
        }
        else if (request.Configuration == NativeConfiguration.Asan)
        {
            yield return "-fsanitize=address";
        }
        // POSIX link driver는 bitcode object를 받으려면 LTO를 알아야 한다(ld64는 libLTO, lld는 내장).
        // Windows lld-link는 bitcode object를 자동으로 ThinLTO 처리한다.
        else if (request.Configuration == NativeConfiguration.Release)
        {
            yield return "-flto=thin";
        }
    }
}
