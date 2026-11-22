using Rocks.Tooling.Common;

namespace Rocks.Build;

internal static class InputValidation
{
    // RootDirectory·OutputDirectory·SdkDirectory는 절대 경로여야 한다.
    public static void RequireAbsoluteRoots(NativeBuildRequest request)
    {
        foreach ((string name, string? path) in new[] { ("RootDirectory", request.RootDirectory), ("OutputDirectory", request.OutputDirectory),
                     ("SdkDirectory", request.SdkDirectory) })
        {
            if (path is not null && (path.IndexOf('\0') >= 0 || !Path.IsPathFullyQualified(path)))
            {
                throw new ArgumentException($"{name} must be an absolute path: {path}");
            }
        }
    }

    public static void Validate(NativeBuildRequest request)
    {
        RequireAbsoluteRoots(request);
        ValidateLinkType(request);
        if (request.MaxParallelism < 1 || request.MaxParallelism > ProcessRunner.MaximumConcurrentProcesses)
        {
            throw new ArgumentOutOfRangeException(nameof(request.MaxParallelism),
                $"MaxParallelism must be between 1 and {ProcessRunner.MaximumConcurrentProcesses}.");
        }

        if (!Enum.IsDefined(request.Configuration))
        {
            throw new ArgumentException($"Configuration: unsupported value {request.Configuration}.");
        }

        if (request.LogCompiledMinimum is LogLevel logMinimum && !Enum.IsDefined(logMinimum))
        {
            throw new ArgumentException($"LogCompiledMinimum: unsupported value {(int)logMinimum}.");
        }
        // enum 멤버가 추가돼도 실제 타깃 지원을 검토하기 전에는 통과시키지 않는다.
        bool hostMatches = request.Toolchain.Platform switch
        {
            NativePlatform.Windows => OperatingSystem.IsWindows(),
            NativePlatform.MacOS => OperatingSystem.IsMacOS(),
            // NDK는 Windows·macOS host를 지원한다.
            NativePlatform.Android => OperatingSystem.IsWindows() || OperatingSystem.IsMacOS(),
            _ => throw new ArgumentException($"Toolchain.Platform: unsupported value {request.Toolchain.Platform}.")
        };
        if (!hostMatches)
        {
            throw new PlatformNotSupportedException($"Toolchain.Platform {request.Toolchain.Platform}: this native host is not supported for that target.");
        }

        if (request.Toolchain.Platform == NativePlatform.Android && request.Configuration is NativeConfiguration.Asan or NativeConfiguration.Coverage)
        {
            throw new ArgumentException($"Toolchain.Platform Android: the {request.Configuration} configuration is not supported yet (use Debug, Development, or Release).");
        }
        // Release는 ThinLTO bitcode object를 만든다. MSVC lib.exe·link.exe는 bitcode를 읽지 못하고, 다른
        // 버전의 LLVM 도구는 새 bitcode를 읽지 못할 수 있으므로 compiler와 같은 폴더의 llvm-lib·lld-link를
        // 요구한다(clang-cl -fuse-ld=lld도 그 폴더의 lld-link를 쓴다).
        if (request.Toolchain.Platform == NativePlatform.Windows && request.Configuration == NativeConfiguration.Release)
        {
            string compilerDirectory = Path.GetDirectoryName(request.Toolchain.CompilerPath) ?? "";
            bool Beside(string? path, string name) => path != null && IsTool(path, name) &&
                NativeToolchains.PathComparer.Equals(Path.GetDirectoryName(path), compilerDirectory);
            if (!Beside(request.Toolchain.ArchiverPath, "llvm-lib") || !Beside(request.Toolchain.LinkerPath, "lld-link"))
            {
                throw new ArgumentException("Configuration Release: ThinLTO requires llvm-lib and lld-link in the compiler's directory " +
                    $"{compilerDirectory} (archiver: {request.Toolchain.ArchiverPath}, linker: {request.Toolchain.LinkerPath ?? "<none>"}).");
            }
        }

        if (NativeToolchains.PathComparer.Equals(request.RootDirectory, request.OutputDirectory) ||
            Fingerprints.IsUnder(request.RootDirectory, request.OutputDirectory))
        {
            throw new ArgumentException($"OutputDirectory must not be the source root or its ancestor: {request.OutputDirectory}");
        }

        foreach (var pair in new[] { ("CompilerPath", request.Toolchain.CompilerPath), ("ArchiverPath", request.Toolchain.ArchiverPath) })
        {
            if (!Path.IsPathFullyQualified(pair.Item2) || !File.Exists(pair.Item2))
            {
                throw new ArgumentException($"Toolchain.{pair.Item1}: expected an existing absolute executable path: {pair.Item2}");
            }
        }

        if (request.Toolchain.LinkerPath is string linker && (!Path.IsPathFullyQualified(linker) || !File.Exists(linker)))
        {
            throw new ArgumentException($"Toolchain.LinkerPath: expected an existing absolute path: {linker}");
        }

        if (string.IsNullOrWhiteSpace(request.Toolchain.Identity))
        {
            throw new ArgumentException("Toolchain.Identity must not be empty.");
        }

        // 빈 인자는 GNU 규칙 response file로 옮길 수 없다(LLVM이 빈 토큰을 버린다). 모듈 인자처럼 빌드 전에 거부한다.
        foreach ((string name, IReadOnlyList<string> arguments) in new[] { ("CompileArguments", request.Toolchain.CompileArguments),
                     ("LinkArguments", request.Toolchain.LinkArguments) })
        {
            if (arguments.Any(argument => string.IsNullOrEmpty(argument) || argument.Contains('\0')))
            {
                throw new ArgumentException($"Toolchain.{name}: arguments must be non-empty and must not contain NUL characters.");
            }
        }

        ValidateModuleNameCase(request.Modules);
        foreach (var module in request.Modules)
        {
            if (!Enum.IsDefined(module.Kind))
            {
                throw new ArgumentException($"Module {module.Name}: unsupported Kind {module.Kind}.");
            }

            if (!Enum.IsDefined(module.Optimization))
            {
                throw new ArgumentException($"Module {module.Name}: unsupported Optimization {module.Optimization}.");
            }

            if (!Enum.IsDefined(module.WindowsSubsystem))
            {
                throw new ArgumentException($"Module {module.Name}: unsupported WindowsSubsystem {module.WindowsSubsystem}.");
            }

            if (module.WindowsSubsystem != WindowsSubsystem.Console && module.Kind != NativeModuleKind.Executable)
            {
                throw new ArgumentException($"Module {module.Name}: WindowsSubsystem applies only to executables.");
            }

            string basename = module.Name.Split('.')[0].ToUpperInvariant();
            if (new[] { "CON", "PRN", "AUX", "NUL" }.Contains(basename) ||
                (basename.Length == 4 && (basename.StartsWith("COM", StringComparison.Ordinal) || basename.StartsWith("LPT", StringComparison.Ordinal)) && basename[3] is >= '1' and <= '9'))
            {
                throw new ArgumentException($"Module {module.Name}: reserved filesystem name.");
            }

            // C++ 모듈 이름은 모듈 API 매크로(ROCKS_<MODULE>_API)와 진입점 기호를 만들므로 C 식별자여야 한다(Docs/Design/BuildTargets.ko.md
            // 3.8절 BD27). 서드파티 모듈은 매크로를 받지 않는다.
            if (!module.ThirdParty && !ModuleTables.IsIdentifier(module.Name))
            {
                throw new ArgumentException($"Module {module.Name}: a C++ module must have a C identifier name (letters, digits and '_', not " +
                    "starting with a digit) — it names ROCKS_<MODULE>_API.");
            }

            bool posix = request.Toolchain.Platform != NativePlatform.Windows;
            GeneratedSources.Validate(module);
            ValidateReflectionSources(module);
            foreach (string argument in module.AdditionalCompileArguments)
            {
                ValidateArgument(module.Name, "AdditionalCompileArguments", argument, true, posix);
            }

            foreach (string argument in module.AdditionalLinkArguments)
            {
                ValidateArgument(module.Name, "AdditionalLinkArguments", argument, false, posix);
            }

            foreach (string library in module.SystemLibraries)
            {
                ValidateLinkName(module.Name, "SystemLibraries", library);
            }

            foreach (string framework in module.Frameworks)
            {
                ValidateLinkName(module.Name, "Frameworks", framework);
            }

            if (module.Frameworks.Count != 0 && request.Toolchain.Platform != NativePlatform.MacOS)
            {
                throw new ArgumentException($"Module {module.Name}: Frameworks are Apple-only, but the toolchain targets " +
                    $"{request.Toolchain.Platform}. Declare them only when BuildContext.Platform is MacOS.");
            }

            foreach (string define in module.PublicDefines.Concat(module.PrivateDefines))
            {
                string name = define.Split('=')[0];
                if (name.Length == 0 || (!char.IsAsciiLetter(name[0]) && name[0] != '_') ||
                    name.Any(value => !char.IsAsciiLetterOrDigit(value) && value != '_') || define.IndexOfAny(['\0', '\r', '\n']) >= 0)
                {
                    throw new ArgumentException($"Module {module.Name}: invalid define '{define}'.");
                }
                // 구성·대상 매크로는 빌드 시스템과 Base/Compiler.h만 정의한다(모듈이 바꾸면 구성·대상 판정이 모듈마다 달라지고,
                // MetaCompiler가 프로필과 모듈 정의를 함께 받아 중복 오류를 낸다). 함수 단위 target 매크로(ROCKS_TARGET_*)도
                // Compiler.h만 정의한다(정책 검사 target-intrinsics가 계열별로 가둔다).
                if (name.StartsWith("ROCKS_BUILD_", StringComparison.Ordinal) || name.StartsWith("ROCKS_PLATFORM_", StringComparison.Ordinal) ||
                    name.StartsWith("ROCKS_ARCH_", StringComparison.Ordinal) || name.StartsWith("ROCKS_TARGET_", StringComparison.Ordinal))
                {
                    throw new ArgumentException($"Module {module.Name}: define '{define}' is reserved for the build configuration and target.");
                }
                // 모듈 이름·자기 타입 표지 매크로는 빌드가 진입점 모듈마다 정의한다(ModuleTables). 규칙이 정의하면 진입점 매크로의 이름
                // 대조가 무의미해지거나 진입 기술이 없는 타입 등록 함수를 가리킨다.
                if (name == ModuleTables.ModuleNameMacro || name == ModuleTables.ModuleTypesMacro || name == ModuleTables.SharedLibraryMacro)
                {
                    throw new ArgumentException($"Module {module.Name}: define '{define}' is reserved; the build defines " +
                        $"{name} for every module that implements a module entry point.");
                }
                // 링크 방식·모듈 API 매크로는 빌드가 타깃·모듈마다 정의한다(Docs/Design/BuildTargets.ko.md 3.8절 BD27).
                if (name == LinkTypes.MonolithicMacroName || LinkTypes.IsApiMacro(name))
                {
                    throw new ArgumentException($"Module {module.Name}: define '{define}' is reserved; the build defines {LinkTypes.MonolithicMacroName} " +
                        "for the target and ROCKS_<MODULE>_API for every module.");
                }
                // 로그 컴파일 하한은 타깃 전체에 하나다(LogCompiledMinimums). 모듈이 바꾸면 Log.h inline 본문이 TU마다 달라져 ODR 위반이다.
                if (name == LogCompiledMinimums.MacroName)
                {
                    throw new ArgumentException($"Module {module.Name}: define '{define}' is reserved for the target " +
                        "(set TargetRules.LogCompiledMinimum instead; a per-module value violates the ODR).");
                }
                // 경로를 담은 define 값은 호스트마다 달라 도구 빌드 ID와 바이너리 신원을 호스트에 묶는다. 외부 파일은 선언 입력으로 둔다.
                int equals = define.IndexOf('=');
                if (equals >= 0 && HasAbsolutePath(define[(equals + 1)..], request.Toolchain.Platform != NativePlatform.Windows))
                {
                    throw new ArgumentException($"Module {module.Name}: define '{define}' contains an absolute path; " +
                        "declare external files as inputs (include directories, AdditionalInputs, PrebuiltLibraries) instead.");
                }
            }
        }
    }

    // 링크 방식(Docs/Design/BuildTargets.ko.md 3.8절 BD17). Modular는 Android(UE도 모바일에서 거부)와 Asan·Coverage·Release 구성에서 쓰지
    // 않는다(Editor는 Release로 빌드하지 않는다 — BD20).
    private static void ValidateLinkType(NativeBuildRequest request)
    {
        if (!Enum.IsDefined(request.LinkType))
        {
            throw new ArgumentException($"Invalid link type: {request.LinkType}.");
        }

        if (request.LinkType != NativeLinkType.Modular)
        {
            return;
        }

        if (request.Toolchain.Platform == NativePlatform.Android)
        {
            throw new ArgumentException("Modular linking is not supported on Android; use LinkType Monolithic.");
        }

        if (!LinkTypes.SupportsConfiguration(request.LinkType, request.Configuration))
        {
            throw new ArgumentException($"Modular linking supports only the Debug and Development configurations, not {request.Configuration}.");
        }

        if (request.TargetName is null && request.Targets.Count != 1)
        {
            throw new ArgumentException("Modular linking needs a target name for shared library file names (NativeBuildRequest.TargetName).");
        }

        // 빌드 정보 TU와 manifest는 타깃 하나 몫이다: 모듈 표·빌드 정보 제공 모듈에 닿는 대상 실행 파일은 하나 이하이고, 있으면 엔진 버전이
        // 있어야 한다(manifest·빌드 정보가 싣는다 — 링크가 모두 끝난 뒤 실패하지 않게).
        var byName = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in request.Modules)
        {
            byName.TryAdd(module.Name, module);
        }

        // 모듈 표 제공 모듈에 닿는 Modular 실행 파일은 호스트 모듈이 되고 실행 파일은 Launch의 Modular 진입만 부른다(Docs/Design/ModuleLoader.ko.md
        // 3.14절 ML4b). Launch에 닿지 않으면 모듈을 시작할 진입이 없으므로 거부한다.
        foreach (NativeModuleInput executable in byName.Values.Where(module => module.Kind == NativeModuleKind.Executable)
            .OrderBy(module => module.Name, StringComparer.Ordinal))
        {
            HashSet<string> closure = LinkClosure.Reachable([executable.Name], byName);
            if (closure.Any(name => byName.TryGetValue(name, out NativeModuleInput? value) && value.ProvidesModuleTable) &&
                !closure.Any(name => byName.TryGetValue(name, out NativeModuleInput? value) && value.ProvidesLaunch))
            {
                throw new ArgumentException($"Module {executable.Name}: a Modular executable that reaches the module table provider must reach " +
                    "the launch module, whose Modular entry loads the module manifest (Docs/Design/ModuleLoader.ko.md section 3.14).");
            }
        }

        if (byName.Values.Any(module => module.ProvidesModuleTable || module.ProvidesBuildInfo))
        {
            IEnumerable<string> roots = request.Targets.Count == 0 ? byName.Keys : request.Targets;
            HashSet<string> reachable = LinkClosure.Reachable(roots, byName);
            string[] executables = byName.Values.Where(module => module.Kind == NativeModuleKind.Executable && reachable.Contains(module.Name) &&
                    LinkClosure.Reachable([module.Name], byName).Any(name => byName[name].ProvidesModuleTable || byName[name].ProvidesBuildInfo))
                .Select(module => module.Name).Order(StringComparer.Ordinal).ToArray();
            if (executables.Length > 1)
            {
                throw new ArgumentException("Modular linking builds the module manifest and build information for one executable, but the " +
                    $"targets reach {string.Join(", ", executables)}; build one target at a time.");
            }

            if (executables.Length == 1 && request.EngineVersion is not { IsEmpty: false })
            {
                throw new ArgumentException($"Modular linking of {executables[0]} needs NativeBuildRequest.EngineVersion for its module manifest.");
            }
        }

        // 켠 플러그인 모듈의 공유 라이브러리 위치(플러그인 Binaries manifest)는 ML4다(Docs/Design/BuildTargets.ko.md 3.9절 BD41).
        if (request.Modules.FirstOrDefault(module => module.PluginModules.Count != 0) is { } host)
        {
            throw new ArgumentException($"Module {host.Name}: Modular linking of a target that enables plugins is not supported until the plugin " +
                "binary manifest (ModuleLoader ML4); use Monolithic.");
        }

        // 미리 빌드한 라이브러리만 있는 모듈(설치 SDK 모듈)은 아직 공유 라이브러리로 나르지 않는다.
        if (request.Modules.FirstOrDefault(module => !module.ThirdParty && module.PrebuiltLibraries.Count != 0) is { } prebuilt)
        {
            throw new ArgumentException($"Module {prebuilt.Name}: Modular linking of prebuilt (installed SDK) modules is not supported yet; use Monolithic.");
        }
    }

    // 리플렉션 생성 소스 목록(NativeModuleInput.ReflectionSources)은 MetaCompiler가 낸 C++ 번역 단위의 경로다. 빈 경로·NUL·.cpp가 아닌
    // 확장자를 거부한다(C로 컴파일되는 .c나 header가 들어오지 않게).
    private static void ValidateReflectionSources(NativeModuleInput module)
    {
        foreach (string source in module.ReflectionSources)
        {
            if (string.IsNullOrWhiteSpace(source) || source.Contains('\0') || !source.EndsWith(".cpp", StringComparison.Ordinal))
            {
                throw new ArgumentException($"Module {module.Name}, ReflectionSources: '{source}' must be the path of a generated .cpp translation unit.");
            }
        }
    }

    // 모듈 이름은 실행 중 대소문자를 구분해 비교한다(Docs/Design/ModuleLoader.ko.md 3.4절 MD17). 대소문자만 다른 두 모듈은 Windows 파일 이름과
    // 진입점 기호가 충돌하므로 거부한다. 의존 목록의 철자도 모듈 이름과 정확히 같아야 한다(빌드가 대소문자를 무시해 이어 준 의존이 모듈 표에서
    // 끊기지 않게). 정확히 같은 이름의 중복과 모르는 의존은 액션 그래프가 알린다.
    private static void ValidateModuleNameCase(IReadOnlyList<NativeModuleInput> modules)
    {
        var byFolded = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in modules)
        {
            if (byFolded.TryGetValue(module.Name, out string? existing))
            {
                if (!string.Equals(existing, module.Name, StringComparison.Ordinal))
                {
                    throw new ArgumentException($"Module names '{existing}' and '{module.Name}' differ only in case; module names must be unique " +
                        "ignoring case.");
                }

                continue;
            }

            byFolded.Add(module.Name, module.Name);
        }

        foreach (NativeModuleInput module in modules)
        {
            foreach (string dependency in module.AllDependencies)
            {
                if (byFolded.TryGetValue(dependency, out string? actual) && !string.Equals(actual, dependency, StringComparison.Ordinal))
                {
                    throw new ArgumentException($"Module {module.Name}, dependencies: '{dependency}' differs in case from module '{actual}'.");
                }
            }

            // 한 모듈을 두 목록에 함께 적으면 공개 범위가 모호하다(Docs/Design/BuildTargets.ko.md 3.5절 BD11).
            foreach (string dependency in module.PublicDependencies)
            {
                if (module.PrivateDependencies.Contains(dependency, StringComparer.OrdinalIgnoreCase))
                {
                    throw new ArgumentException($"Module {module.Name}: '{dependency}' is in both PublicDependencies and PrivateDependencies.");
                }
            }
        }

        ValidateDynamicModules(modules, byFolded);
        ValidatePluginModules(modules, byFolded);
        ValidateGlobalStateConsumers(modules);
    }

    // 프로세스 전역 상태가 있는 서드파티(NativeModuleInput.HasGlobalState — Docs/Design/BuildTargets.ko.md 3.8절 BD29·BS8c)는 직접 의존하는
    // 모듈이 하나여야 한다. Modular에서 서드파티는 직접 소비 모듈의 공유 라이브러리마다 링크되므로 둘이면 상태가 두 벌이 된다(Monolithic에서도
    // 같은 규칙으로 막아 Editor 빌드에서만 드러나지 않게 한다). 소비 모듈 이름을 모두 알린다. 표지는 서드파티 모듈만 가질 수 있다(Rocks 모듈의
    // 상태는 소유 모듈 .cpp에 두고 API로 노출한다).
    private static void ValidateGlobalStateConsumers(IReadOnlyList<NativeModuleInput> modules)
    {
        foreach (NativeModuleInput library in modules.Where(module => module.HasGlobalState))
        {
            if (!library.ThirdParty)
            {
                throw new ArgumentException($"Module {library.Name}: HasGlobalState is only for a third-party module (ThirdParty.json globalState).");
            }

            string[] consumers = modules.Where(module => module.AllDependencies.Contains(library.Name, StringComparer.OrdinalIgnoreCase))
                .Select(module => module.Name).Order(StringComparer.Ordinal).ToArray();
            if (consumers.Length > 1)
            {
                throw new ArgumentException($"Module {library.Name} has process-wide state (ThirdParty.json globalState), so only one module may " +
                    $"depend on it directly; found {string.Join(", ", consumers)}. Route the others through the owning module's API.");
            }
        }
    }

    // 실행 파일에 링크할 켠 플러그인 모듈 목록(NativeModuleInput.PluginModules — Docs/Design/BuildTargets.ko.md 3.9절 BD36)이다. 빌드가
    // 주 실행 파일에만 채운다. 표에 있는 라이브러리 모듈을 정확한 철자로 한 번씩 가리켜야 한다.
    private static void ValidatePluginModules(IReadOnlyList<NativeModuleInput> modules, Dictionary<string, string> byFolded)
    {
        var kinds = new Dictionary<string, NativeModuleKind>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in modules)
        {
            kinds.TryAdd(module.Name, module.Kind);
        }

        foreach (NativeModuleInput module in modules.Where(module => module.PluginModules.Count != 0))
        {
            if (module.Kind != NativeModuleKind.Executable)
            {
                throw new ArgumentException($"Module {module.Name}, PluginModules: only an executable links plugin modules.");
            }

            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string plugin in module.PluginModules)
            {
                if (!byFolded.TryGetValue(plugin, out string? actual) || !string.Equals(actual, plugin, StringComparison.Ordinal))
                {
                    throw new ArgumentException($"Module {module.Name}, PluginModules: unknown module {plugin}.");
                }

                if (!seen.Add(plugin) || kinds[plugin] == NativeModuleKind.Executable)
                {
                    throw new ArgumentException($"Module {module.Name}, PluginModules: {plugin} is listed twice or is an executable.");
                }
            }
        }
    }

    // 실행 중 적재 모듈 목록(NativeModuleInput.DynamicModules — Docs/Design/ModuleLoader.ko.md 3.8절 MD11)이다. 표에 있는 라이브러리 모듈을
    // 정확한 철자로 한 번씩 가리켜야 한다. 링크 의존과 겹치면 거부한다(UE도 오류 — UEBuildModuleCPP.cs ValidateModule): 같은 모듈을
    // 링크하면서 실행 중에 적재한다는 뜻이 모순이다. 순환은 검사하지 않는다(동적 관계는 참조 사슬이 아니다).
    private static void ValidateDynamicModules(IReadOnlyList<NativeModuleInput> modules, Dictionary<string, string> byFolded)
    {
        var byName = new Dictionary<string, NativeModuleInput>(StringComparer.OrdinalIgnoreCase);
        foreach (NativeModuleInput module in modules)
        {
            byName.TryAdd(module.Name, module);
        }

        foreach (NativeModuleInput module in modules)
        {
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string dynamic in module.DynamicModules)
            {
                if (!byFolded.TryGetValue(dynamic, out string? actual))
                {
                    throw new ArgumentException($"Module {module.Name}, DynamicModules: unknown module {dynamic}.");
                }

                if (!string.Equals(actual, dynamic, StringComparison.Ordinal))
                {
                    throw new ArgumentException($"Module {module.Name}, DynamicModules: '{dynamic}' differs in case from module '{actual}'.");
                }

                if (string.Equals(dynamic, module.Name, StringComparison.OrdinalIgnoreCase))
                {
                    throw new ArgumentException($"Module {module.Name}, DynamicModules: a module cannot load itself at run time.");
                }

                if (!seen.Add(dynamic))
                {
                    throw new ArgumentException($"Module {module.Name}, DynamicModules: '{dynamic}' is listed twice.");
                }

                if (module.AllDependencies.Contains(dynamic, StringComparer.OrdinalIgnoreCase))
                {
                    throw new ArgumentException($"Module {module.Name}: '{dynamic}' is both a dependency and in DynamicModules; a module that is " +
                        "linked as a dependency cannot also be loaded at run time.");
                }

                if (byName[dynamic].Kind == NativeModuleKind.Executable)
                {
                    throw new ArgumentException($"Module {module.Name}, DynamicModules: '{dynamic}' is an executable.");
                }
            }
        }
    }

    // CL 철자 표다(Docs/Design/ReflectionFoundations.ko.md 8.1절). clang-cl은 CL 옵션을 '/'와 '-' 두 접두로 받고 대소문자를
    // 구분한다. 그래서 '/'·'-' 두 접두 모두 대소문자를 구분해 대조한다 — 소문자로 바꿔 비교하면 clang-cl이 받는 GCC형
    // -fdiagnostics-color·-fdebug-compilation-dir=.가 /Fd로 오인된다. Windows(clang-cl) 대상에만 적용한다(POSIX clang의
    // -Fdeps/...는 framework 검색 경로다).
    // 출력을 바꾸는 CL 옵션(compile·link 공통): 정확히 같은 철자와 접두.
    private static readonly string[] OwnedClExact = ["c", "link"];

    private static readonly string[] OwnedClPrefixes = ["Fo", "Fe", "Fd", "Fa", "Fi", "Fp"];

    // compile 정책을 바꾸는 CL 옵션: 경고 끄기·낮추기, RTTI·예외, 강제 include, 외부 헤더, 언어 표준·언어 전환, /clang: 우회.
    private static readonly string[] PolicyClExact = ["w", "W0", "W1", "W2", "W3", "WX-", "GR", "GR+", "TC", "TP"];

    private static readonly string[] PolicyClPrefixes = ["wd", "EH", "FI", "external:", "std:", "clang:"];

    // ISA·FP 하드웨어 선택(-m*)의 허용 목록이다. 지금은 비어 있다 — ISA·FP 하드웨어 선택이 FP 결과를 바꾼다.
    private static readonly string[] MachineAllowlist = [];

    // 경로를 붙여 받는 옵션 접두다. 뒤의 값이 절대 경로면 거부한다(대소문자 무시 — link.exe 옵션은 대소문자를 구분하지 않는다).
    private static readonly string[] PathOptionPrefixes =
    [
        "-L", "-F", "-B", "-I", "-iquote", "-isystem", "-idirafter", "-iprefix", "-iwithprefix", "-iwithprefixbefore",
        "-iwithsysroot", "-isysroot", "--sysroot=", "--sysroot", "-rpath", "-fmacro-prefix-map=", "-ffile-prefix-map=",
        "-fdebug-prefix-map=", "-fprofile-instr-use=", "-fprofile-use=", "/LIBPATH:", "-LIBPATH:", "/I", "/FI", "-include"
    ];

    // sanitizer 계측·runtime은 구성(Asan)과 NativeModuleInput.SanitizerInstrumentation이 정한다. 모듈 인자로 켜거나 끄면
    // 같은 실행 파일 안에서 계측과 runtime이 어긋난다. lower는 소문자로 바꾼 인자다.
    private static bool IsSanitizerSetting(string lower) =>
        lower.StartsWith("-fsanitize", StringComparison.Ordinal) || lower.StartsWith("-fno-sanitize", StringComparison.Ordinal) ||
        lower.StartsWith("/fsanitize", StringComparison.Ordinal);

    private static bool MatchesCl(string argument, string[] exact, string[] prefixes)
    {
        if (argument.Length < 2 || argument[0] is not ('/' or '-'))
        {
            return false;
        }

        string body = argument[1..];
        return exact.Contains(body, StringComparer.Ordinal) || prefixes.Any(prefix => body.StartsWith(prefix, StringComparison.Ordinal));
    }

    // -Wl, 조각에서 앞의 '-'·'/'를 모두 뗀 뒤 비교하는 링커 우회 철자다(LLVM 내부 옵션·LTO plugin·LTO 설정 — -Wl,--mllvm=·
    // -Wl,--plugin-opt=·-Wl,/opt:lldlto= 등).
    private static readonly string[] LinkerBypassPrefixes = ["mllvm", "plugin-opt", "lto", "thinlto", "opt:lldlto", "lldlto"];

    private static void ValidateArgument(string module, string field, string argument, bool compile, bool posix)
    {
        bool windows = !posix;
        string lower = argument.ToLowerInvariant();
        // clang-cl의 /clang:·-clang: 안의 인자도 GCC형 규칙으로 본다(link 인자에서도 우회 경로가 되지 않게).
        string inner = windows && (argument.StartsWith("/clang:", StringComparison.Ordinal) || argument.StartsWith("-clang:", StringComparison.Ordinal))
            ? argument["/clang:".Length..] : argument;
        string innerLower = inner.ToLowerInvariant();
        bool invalid = argument.Length == 0 || argument.IndexOfAny(['\0', '\r', '\n']) >= 0 || argument.StartsWith('@') ||
            lower is "-o" or "-c" ||
            (windows && (MatchesCl(argument, OwnedClExact, OwnedClPrefixes) ||
                lower.StartsWith("/out:", StringComparison.Ordinal) || lower.StartsWith("/pdb:", StringComparison.Ordinal) ||
                lower.StartsWith("/implib:", StringComparison.Ordinal) || lower.StartsWith("-out:", StringComparison.Ordinal) ||
                lower.StartsWith("-pdb:", StringComparison.Ordinal) || lower.StartsWith("-implib:", StringComparison.Ordinal) ||
                argument.StartsWith("/mllvm:", StringComparison.Ordinal))) ||
            // FP 모델(결정 7)과 그 우회 경로: LLVM 내부 옵션, 링크 줄의 fast-math(crtfastmath를 붙인다), 링커 직접 인자, -Ofast.
            // ISA·FP 하드웨어 선택(-m*)의 허용 목록 판정은 컴파일러 인자에만 한다(링커 철자 -machine: 등을 오인하지 않게).
            FloatingPointArguments.IsModelArgument(argument) ||
            (compile && inner.StartsWith("-m", StringComparison.Ordinal) && !MachineAllowlist.Contains(inner, StringComparer.Ordinal)) ||
            inner.StartsWith("-mllvm", StringComparison.Ordinal) ||
            (innerLower.StartsWith("-wl,", StringComparison.Ordinal) && inner.Split(',').Skip(1)
                .Select(part => part.TrimStart('-', '/'))
                .Any(part => LinkerBypassPrefixes.Any(prefix => part.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)))) ||
            inner.StartsWith("-Xlinker", StringComparison.Ordinal) || inner == "-Ofast" ||
            // cc1·전처리기 직접 인자, config 파일, 링커 선택, 대상·ABI 선택은 빌드가 정한다(owned).
            innerLower.StartsWith("-xclang", StringComparison.Ordinal) || innerLower.StartsWith("-xpreprocessor", StringComparison.Ordinal) ||
            innerLower.StartsWith("--config", StringComparison.Ordinal) ||
            innerLower.StartsWith("-fuse-ld", StringComparison.Ordinal) || innerLower.StartsWith("--ld-path", StringComparison.Ordinal) ||
            innerLower.StartsWith("--target", StringComparison.Ordinal) || inner == "-target" || inner == "-arch" ||
            inner.StartsWith("-fms-compatibility-version", StringComparison.Ordinal) || inner.StartsWith("-fmsc-version", StringComparison.Ordinal);
        if (compile)
        {
            invalid |= lower is "-w" or "-fexceptions" or "-frtti" || (windows && MatchesCl(argument, PolicyClExact, PolicyClPrefixes)) ||
                lower.StartsWith("-wno-", StringComparison.Ordinal) ||
                lower.StartsWith("-include", StringComparison.Ordinal) || lower.StartsWith("-isystem", StringComparison.Ordinal) ||
                argument.StartsWith("-I", StringComparison.Ordinal) || argument.StartsWith("-D", StringComparison.Ordinal) ||
                (windows && (argument.StartsWith("/I", StringComparison.Ordinal) || argument.StartsWith("/D", StringComparison.Ordinal))) ||
                // 번역 단위 언어와 표준은 빌드 도구가 확장자로 정한다(.c → C17, 그 밖 → C++23).
                lower.StartsWith("-x", StringComparison.Ordinal) ||
                lower.StartsWith("--language", StringComparison.Ordinal) || lower.StartsWith("-std=", StringComparison.Ordinal) ||
                (windows && argument.Length > 3 && argument[0] is ('/' or '-') &&
                    (argument[1..].StartsWith("Tc", StringComparison.Ordinal) || argument[1..].StartsWith("Tp", StringComparison.Ordinal))) ||
                // 최적화 수준·LTO는 구성과 NativeModuleInput.Optimization이 정한다. 수준을 바꾸지 않는
                // /Oy-(frame pointer 유지)·/Oi(intrinsic) 등은 허용한다.
                OptimizationLevel.IsMatch(argument) || lower.StartsWith("-flto", StringComparison.Ordinal) ||
                lower.StartsWith("-fno-lto", StringComparison.Ordinal) || IsSanitizerSetting(lower);
        }
        else
        {
            // link 단계에서 LTO를 끄거나 backend 최적화 수준을 바꾸는 인자도 구성이 정한다. sanitizer runtime도 구성이 고른다.
            invalid |= IsSanitizerSetting(lower) ||
                lower.StartsWith("-flto", StringComparison.Ordinal) || lower.StartsWith("-fno-lto", StringComparison.Ordinal) ||
                lower.StartsWith("/opt:lldlto", StringComparison.Ordinal) || lower.StartsWith("/lldlto", StringComparison.Ordinal) ||
                lower.StartsWith("-wl,--lto", StringComparison.Ordinal) || lower.StartsWith("-wl,--thinlto", StringComparison.Ordinal) ||
                lower.StartsWith("-wl,-lto", StringComparison.Ordinal) || lower.StartsWith("--lto", StringComparison.Ordinal);
        }

        if (invalid)
        {
            throw new ArgumentException($"Module {module}, {field}: argument '{argument}' overrides owned output/policy or hides inputs; use structured settings.");
        }

        if (HasAbsolutePath(argument, posix))
        {
            throw new ArgumentException($"Module {module}, {field}: argument '{argument}' contains an absolute path; declare external files " +
                "as inputs (include directories, AdditionalInputs, PrebuiltLibraries) instead.");
        }
    }

    // 값 안의 절대 경로를 찾는다. Windows 드라이브(C:\·C:/)·UNC(\\server)는 어느 대상에서나, POSIX 대상은 '/'로 시작하는 조각도
    // 절대 경로다. 인자 전체, 경로 옵션 접두를 뗀 나머지, = , ; " ' 공백 괄호로 나눈 조각(과 그 조각의 접두를 뗀 나머지)을 본다.
    internal static bool HasAbsolutePath(string text, bool posix)
    {
        bool Absolute(string value) =>
            (value.Length >= 3 && char.IsAsciiLetter(value[0]) && value[1] == ':' && value[2] is ('\\' or '/')) ||
            value.StartsWith(@"\\", StringComparison.Ordinal) || (posix && value.StartsWith('/'));
        bool Check(string value) => Absolute(value) || PathOptionPrefixes.Any(prefix =>
            value.Length > prefix.Length && value.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) && Absolute(value[prefix.Length..]));
        return Check(text) || text.Split(['=', ',', ';', '"', '\'', ' ', '(', ')'], StringSplitOptions.RemoveEmptyEntries).Any(Check);
    }

    // 시스템 라이브러리·framework 이름은 toolchain이 플랫폼 형식으로 바꾸므로 이름만 받는다. 경로·플래그·확장자는
    // 플랫폼 중립성을 깨고 링크 인자 정책을 우회하므로 거부한다.
    private static void ValidateLinkName(string module, string field, string name)
    {
        bool valid = name.Length is > 0 and <= 128 && (char.IsAsciiLetterOrDigit(name[0]) || name[0] == '_') &&
            name.All(value => char.IsAsciiLetterOrDigit(value) || value is '_' or '.' or '+' or '-');
        string lower = name.ToLowerInvariant();
        bool extension = lower.EndsWith(".lib", StringComparison.Ordinal) || lower.EndsWith(".a", StringComparison.Ordinal) ||
            lower.EndsWith(".so", StringComparison.Ordinal) || lower.EndsWith(".dylib", StringComparison.Ordinal) ||
            lower.EndsWith(".framework", StringComparison.Ordinal) || lower.EndsWith(".tbd", StringComparison.Ordinal);
        if (!valid || extension)
        {
            throw new ArgumentException($"Module {module}, {field}: '{name}' must be a bare library or framework name " +
                "(no path, flag prefix, or file extension).");
        }
    }

    // 최적화 수준 인자다(대소문자 구분). clang: -O, -O0~3, -Os, -Oz, -Og, -Ofast, --optimize[=N].
    // clang-cl: /O1, /O2, /Od, /Og, /Os, /Ot, /Ox, /Ob0~3.
    private static readonly System.Text.RegularExpressions.Regex OptimizationLevel = new(
        "^(?:-O(?:[0-9]|s|z|g|fast)?|--optimize(?:=.*)?|/O(?:[12dgstx]|b[0-3]))$",
        System.Text.RegularExpressions.RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));

    private static bool IsTool(string path, string name) =>
        Path.GetFileNameWithoutExtension(path).Equals(name, StringComparison.OrdinalIgnoreCase);
}
