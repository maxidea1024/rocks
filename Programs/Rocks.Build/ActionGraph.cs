namespace Rocks.Build;

// Inputs는 내용이 key에 들어가는 파일(번역 단위의 소스, archive·link의 object·라이브러리)이고, HeaderDirectories는 헤더 이름 목록이 key에
// 들어가는 디렉터리(include 디렉터리와 소스 디렉터리)다. DependencyRecord는 이 action의 의존 기록 파일이다(compile action — Outputs의
// 하나이고, 컴파일러가 연 헤더의 내용은 key가 아니라 이 기록으로 검증한다). Arguments의 {DEPFILE}은 실행 때 임시 depfile 경로가 된다.
internal sealed record NativeAction(string Id, string Module, string Kind, string Tool,
    IReadOnlyList<string> Arguments, IReadOnlyList<string> Outputs, IReadOnlyList<string> Inputs,
    IReadOnlyList<string> HeaderDirectories, IReadOnlyList<string> DeclaredInputs,
    IReadOnlyList<string> Dependencies, IReadOnlyList<string> Identity, string? Source = null, string? FailureNote = null,
    string? DependencyRecord = null)
{
    // 실행 때 컴파일러가 depfile을 쓸 경로로 바뀌는 인자 자리 표시다(Executor — action 임시 디렉터리 안이라 key에 들지 않는다).
    public const string DepfilePlaceholder = "{DEPFILE}";

    /// <summary>
    /// 선언 입력 key다(Docs/Design/UnityBuildPch.ko.md 4.1절 2단 key의 첫 단). 이전 실행 결과에 의존하지 않는다: 툴체인 key, action 인자,
    /// TU 구성(Identity), 입력 파일 내용, 선언 추가 입력 내용, include·소스 디렉터리의 헤더 이름 목록을 담는다. 헤더 내용은 담지 않는다 —
    /// 컴파일러가 실제로 연 헤더는 의존 기록(<see cref="DependencyRecord"/>)이 검증한다(IncrementalStore.IsCurrent). FailureNote는 표시
    /// 전용이라 넣지 않는다.
    /// </summary>
    public string Key(string toolchain, BuildInputs inputs)
    {
        var tokens = new List<string> { toolchain, Id, Tool };
        foreach (string input in Inputs)
        {
            if (!File.Exists(input))
            {
                throw new FileNotFoundException("Declared build input does not exist.", input);
            }

            tokens.Add(input);
            tokens.Add(inputs.Content(input));
        }

        tokens.Add("header-directories");
        foreach (string directory in HeaderDirectories.Distinct(NativeToolchains.PathComparer))
        {
            tokens.Add(directory);
            tokens.Add(inputs.Listing(directory));
        }

        tokens.Add("declared-inputs");
        foreach (string declared in DeclaredInputs.Distinct(NativeToolchains.PathComparer))
        {
            tokens.Add(declared);
            tokens.Add(inputs.Declared(declared));
        }

        tokens.Add("identity");
        tokens.AddRange(Identity);
        tokens.Add("arguments");
        tokens.AddRange(Arguments);
        return Fingerprints.Strings(tokens);
    }

    /// <summary>메모 없이 계산한 선언 입력 key다(action 하나만 보는 시험용 — 빌드는 <see cref="Key(string, BuildInputs)"/>를 쓴다).</summary>
    public string Key(string toolchain, string outputRoot, string root) => Key(toolchain, new BuildInputs(root, outputRoot, null));

    public string[] ResolveArguments(IReadOnlyList<string> outputs, string? depfile = null) =>
        Resolve(Arguments, outputs, depfile);

    internal static string[] Resolve(IReadOnlyList<string> arguments, IReadOnlyList<string> outputs, string? depfile = null)
    {
        return arguments.Select(argument =>
        {
            for (int index = 0; index < outputs.Count; ++index)
            {
                argument = argument.Replace($"{{OUTPUT{index}}}", outputs[index], StringComparison.Ordinal);
            }

            return depfile is null ? argument : argument.Replace(DepfilePlaceholder, depfile, StringComparison.Ordinal);
        }).ToArray();
    }
}

/// <summary>
/// 원본 소스 하나의 논리 compile 명령이다(Docs/Design/UnityBuildPch.ko.md 8.6절). compile_commands.json과 BuildInfo manifest는 물리
/// action이 아니라 이것으로 만든다 — depfile 인자 같은 실행 전용 인자가 없고, 소스 하나에 명령 하나다. Arguments의 {OUTPUT0}은
/// <see cref="Object"/>다. Action은 이 소스를 실제로 컴파일하는 물리 action의 Id다.
/// </summary>
internal sealed record LogicalCompile(string Module, string Source, string Tool, IReadOnlyList<string> Arguments, string Object, string Action);

// Closures: 계획에 든 모듈마다 도달 집합(자기 자신, 그다음 의존 순서)이다. BuildInfo가 실행 파일의 액션을 고를 때 쓴다.
// LogicalCompiles: 원본 소스마다의 논리 compile 명령이다(계획의 compile action 순서).
internal sealed record NativePlan(IReadOnlyList<NativeAction> Actions,
    IReadOnlyDictionary<string, string> ModuleOutputs, IReadOnlyDictionary<string, IReadOnlyList<string>> Closures,
    IReadOnlyList<LogicalCompile> LogicalCompiles);

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
        string[] targetArguments = TargetCompileArguments(request, root).ToArray();
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
        var logicalCompiles = new List<LogicalCompile>();
        var outputs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var terminalActions = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var closures = new Dictionary<string, IReadOnlyList<string>>(StringComparer.OrdinalIgnoreCase);
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        // 생성 TU 디렉터리는 header 디렉터리 목록에 넣지 않는다 — 공용 디렉터리의 header 확장자 파일 하나가 모든 실행 파일 TU를
        // 다시 컴파일하게 만들지 않는다(생성 TU의 key는 cpp 내용이다).
        string buildInfo = Path.TrimEndingDirectorySeparator(BuildInfoGenerator.DirectoryOf(output));
        // 진입점 모듈(모듈 표에 드는 C++ 모듈)은 소스에 ROCKS_MODULE_NAME=<이름>을 받는다(Docs/Design/ModuleLoader.ko.md 3.2절). 진입점 매크로가
        // 이 값과 매크로 인자를 static_assert로 대조한다. 모듈 표 제공 모듈이 없는 빌드(시험 fixture 등)에서는 아무 모듈도 받지 않는다.
        HashSet<string> entryModules = ModuleTables.EntryModules(modules);
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
            closures[module.Name] = [module.Name, .. dependencies.Select(value => value.Name)];
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

            if (entryModules.Contains(module.Name))
            {
                defines = [.. defines, ModuleTables.ModuleNameMacro + "=" + module.Name];
            }

            // 리플렉션 생성 소스는 모듈 소스와 같은 인자로 컴파일해 같은 archive·실행 파일에 넣는다(목록만 따로다 —
            // NativeModuleInput.ReflectionSources).
            string[] reflectionSources = module.ReflectionSources.Select(Absolute).ToArray();
            var reflectionSet = new HashSet<string>(reflectionSources, NativeToolchains.PathComparer);
            string? reflectionNote = module.ReflectionSourceFailureNote.Length == 0 ? null : module.ReflectionSourceFailureNote;
            string[] sources = module.Sources.Select(Absolute).Concat(reflectionSources).Order(StringComparer.Ordinal).ToArray();
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

            // 리플렉션 생성 소스의 디렉터리(<out>/Generated/<M>/Source)도 header 디렉터리가 아니다 — 생성 소스만 있고 include 루트가
            // 아니다(Docs/Design/ReflectionGeneratedSources.ko.md 6.1절). 생성 소스의 key는 cpp 내용과 include 디렉터리다.
            string[] headers = includes.Concat(module.Sources.Select(Absolute).Select(value => Path.GetDirectoryName(value)!))
                .Where(value => !NativeToolchains.PathComparer.Equals(Path.TrimEndingDirectorySeparator(value), buildInfo))
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
                // 계측을 끈 모듈은 sanitizer 인자만 빠진다(프레임 포인터 등 나머지 Asan 프로필은 같다).
                arguments.AddRange(NativeCompileProfile.ModuleArguments(request.Toolchain, request.Configuration, module, language));
                // ThinLTO는 코드 생성 옵션이라 AST 프로필(NativeCompileProfile)에 넣지 않는다. 모듈이 끄면(LinkTimeOptimization)
                // Release에서도 일반 object로 남는다.
                if (module.LinkTimeOptimization && UsesThinLto(request.Configuration, module.Optimization))
                {
                    arguments.Add("-flto=thin");
                }

                // clang source-based coverage 계측(Docs/Design/AssetSystem.ko.md 14.1절 — 줄·분기 coverage를 llvm-cov로 잰다)도
                // 코드 생성 옵션이라 AST 프로필에 넣지 않는다(MetaCompiler는 프로필 밖 옵션을 거부한다).
                if (request.Configuration == NativeConfiguration.Coverage)
                {
                    arguments.AddRange(["-fprofile-instr-generate", "-fcoverage-mapping"]);
                }

                // 타깃 단위 인자: 로그 컴파일 하한과 소스 경로 접두 매핑은 모든 모듈·번역 단위에 같다(모듈 인자로 바꾸지 못한다).
                arguments.AddRange(targetArguments);
                arguments.AddRange(defines.Select(value => (windows ? "/D" : "-D") + value));
                foreach (string include in includes)
                {
                    arguments.Add((windows ? "/I" : "-I") + include);
                }
                // macOS·Android의 compiler는 clang++ driver라서 C 소스에는 언어를 명시한다(없으면 C++로 처리하며 -Werror에서 경고가
                // 오류가 된다).
                string[] translationUnit = windows ? ["/c", language == NativeLanguage.C ? "/TC" : "/TP", source, "/Fo{OUTPUT0}"]
                    : language == NativeLanguage.C ? ["-c", "-x", "c", source, "-o", "{OUTPUT0}"]
                    : ["-c", source, "-o", "{OUTPUT0}"];
                // 물리 action은 depfile을 쓰게 한다(의존 기록의 원천 — Docs/Design/UnityBuildPch.ko.md 4.1절). 논리 명령(compile DB·
                // BuildInfo)에는 넣지 않는다. clang-cl은 -MD·-MF를 /clang:로만 받는다(실측).
                string[] depfile = windows ? ["/clang:-MD", "/clang:-MF" + NativeAction.DepfilePlaceholder]
                    : ["-MD", "-MF", NativeAction.DepfilePlaceholder];
                string recordPath = objectPath + DependencyRecord.Extension;
                // identity는 이 번역 단위의 구성(단독 TU면 소스 하나)이다. 모듈의 전체 소스 목록은 넣지 않는다 — 소스 하나를 더하거나
                // 지워도 다른 번역 단위는 다시 컴파일하지 않는다(4.2절. 소스 옆 header가 생기거나 사라지는 변화는 소스 디렉터리의 헤더
                // 이름 목록이 잡는다).
                actions.Add(new(id, module.Name, "Compile", request.Toolchain.CompilerPath, [.. arguments, .. depfile, .. translationUnit],
                    [objectPath, recordPath], [source], headers, extraInputs, [],
                    [request.ContextFingerprint, request.Configuration.ToString(), module.Optimization.ToString(), source], source,
                    FailureNote: reflectionSet.Contains(source) ? reflectionNote : null, DependencyRecord: recordPath));
                logicalCompiles.Add(new(module.Name, source, request.Toolchain.CompilerPath, [.. arguments, .. translationUnit], objectPath, id));
                objectPaths.Add(objectPath);
                compileIds.Add(id);
            }
            // Base/MetaSchema 같은 header-only 모듈은 설정만 전파하고 가짜 빈 archive를 만들지 않는다. 리플렉션 생성 소스만 있는
            // 모듈은 그 object로 archive를 만든다.
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

                    if (request.Configuration == NativeConfiguration.Coverage)
                    {
                        // driver가 고르는 triple 디렉터리(x86_64-pc-windows-msvc)와 배포판의 디렉터리 이름(Swift 툴체인은
                        // x86_64-unknown-windows-msvc)이 달라 driver의 자동 링크를 믿지 않고 compiler의 resource 디렉터리에서
                        // profile 런타임을 찾아 넘긴다(Asan import·thunk와 같은 방식).
                        string profileLibrary = CoverageRuntimeLibrary(request.Toolchain.CompilerPath, module.Name);
                        finalArguments.Add(profileLibrary);
                        finalInputs.Add(profileLibrary);
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
            foreach (string input in module.Sources.Concat(module.ReflectionSources).Concat(module.AdditionalInputs)
                .Concat(module.PrebuiltLibraries).Select(Absolute))
            {
                if (generated.Contains(input))
                {
                    throw new ArgumentException($"Module {module.Name}: output overlaps declared input {input}.");
                }
            }
        }

        return new(actions, outputs, closures, logicalCompiles);
    }

    /// <summary>
    /// Windows Coverage 링크에 넣을 clang profile 런타임(<c>clang_rt.profile*.lib</c>, x86_64)을 compiler의 resource 디렉터리
    /// (<c>&lt;bin&gt;/../lib/clang/&lt;version&gt;/lib/&lt;triple&gt;</c>)에서 찾는다. 하나가 아니면 FileNotFoundException이다.
    /// </summary>
    internal static string CoverageRuntimeLibrary(string compilerPath, string module)
    {
        string resource = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(compilerPath)!, "..", "lib", "clang"));
        string[] candidates = Directory.Exists(resource)
            ? Directory.EnumerateFiles(resource, "clang_rt.profile*.lib", SearchOption.AllDirectories)
                .Where(path => path.Contains("x86_64", StringComparison.OrdinalIgnoreCase) && !path.Contains("i686", StringComparison.OrdinalIgnoreCase))
                .Order(StringComparer.Ordinal).ToArray()
            : [];
        if (candidates.Length != 1)
        {
            throw new FileNotFoundException($"Module {module}: expected exactly one x86_64 clang_rt.profile library under {resource}, found {candidates.Length}.");
        }

        return candidates[0];
    }

    /// <summary>구성과 모듈 재정의로 최적화 여부를 정한다. Debug·Asan·Coverage는 끄고 Development·Release는 켠다.</summary>
    internal static bool Optimizes(NativeConfiguration configuration, CodeOptimization optimization) => optimization switch
    {
        CodeOptimization.Always => true,
        CodeOptimization.Never => false,
        CodeOptimization.Default => configuration is NativeConfiguration.Development or NativeConfiguration.Release,
        _ => throw new ArgumentException($"Invalid code optimization: {optimization}")
    };

    /// <summary>
    /// 타깃의 모든 compile action이 같은 값으로 받는 인자다(Docs/Design/LoggingSystem.ko.md 6.4절).
    /// <list type="bullet">
    /// <item><c>ROCKS_LOG_COMPILED_MINIMUM=n</c>: 로그 컴파일 하한(<see cref="LogCompiledMinimums"/>). 타깃 재정의가 없으면 구성 기본값.</item>
    /// <item><c>-fmacro-prefix-map=&lt;루트&gt;&lt;구분자&gt;=</c>: <c>__FILE__</c>·<c>__builtin_FILE()</c>을 루트 기준 상대 경로로 남긴다
    /// (로그 호출 위치·assert 위치가 빌드 장소를 담지 않는다). 루트는 소스 루트와, 설치 SDK 게임 빌드면 SDK 루트다. 디버그 정보 경로는
    /// 바꾸지 않으므로 <c>-ffile-prefix-map</c>이 아니다. clang-cl은 이 옵션을 직접 받지 않아 <c>/clang:</c>로 넘긴다. clang은
    /// Windows에서 접두를 구분자·대소문자 무관하게 맞춘다(clang-cl 21.1.6로 확인).</item>
    /// </list>
    /// </summary>
    internal static IEnumerable<string> TargetCompileArguments(NativeBuildRequest request, string root)
    {
        bool windows = request.Toolchain.Platform == NativePlatform.Windows;
        yield return (windows ? "/D" : "-D") + LogCompiledMinimums.Macro(request.Configuration, request.LogCompiledMinimum);
        var prefixes = new List<string> { root };
        if (request.SdkDirectory is not null)
        {
            prefixes.Add(Path.GetFullPath(request.SdkDirectory));
        }

        // 경로는 host 철자다(Windows host의 Android 교차 빌드도 '\'). 대상이 아니라 host 구분자를 붙인다.
        char separator = Path.DirectorySeparatorChar;
        foreach (string prefix in prefixes.Distinct(NativeToolchains.PathComparer))
        {
            string directory = Path.TrimEndingDirectorySeparator(prefix);
            // 드라이브·파일 시스템 루트(C:\, /)는 이미 구분자로 끝난다.
            string mapped = directory.EndsWith(separator) ? directory : directory + separator;
            yield return (windows ? "/clang:" : "") + "-fmacro-prefix-map=" + mapped + "=";
        }
    }

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
            yield return "/D" + NativeConfigurations.PlatformMacro(NativePlatform.Windows);
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
            }

            yield return "-D" + NativeConfigurations.PlatformMacro(toolchain.Platform);

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
            // Coverage의 clang_rt.profile은 위에서 찾은 파일을 넘긴다(driver의 triple 경로 추정을 피한다).
        }
        else if (request.Configuration == NativeConfiguration.Coverage)
        {
            yield return "-fprofile-instr-generate";
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
