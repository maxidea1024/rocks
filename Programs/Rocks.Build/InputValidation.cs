using Rocks.Tooling.Common;

namespace Rocks.Build;

internal static class InputValidation
{
    public static void Validate(NativeBuildRequest request)
    {
        if (request.MaxParallelism < 1 || request.MaxParallelism > ProcessRunner.MaximumConcurrentProcesses)
        {
            throw new ArgumentOutOfRangeException(nameof(request.MaxParallelism),
                $"MaxParallelism must be between 1 and {ProcessRunner.MaximumConcurrentProcesses}.");
        }

        if (!Enum.IsDefined(request.Configuration))
        {
            throw new ArgumentException($"Configuration: unsupported value {request.Configuration}.");
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

        if (request.Toolchain.Platform == NativePlatform.Android && request.Configuration == NativeConfiguration.Asan)
        {
            throw new ArgumentException("Toolchain.Platform Android: the Asan configuration is not supported yet (use Debug, Development, or Release).");
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

            string basename = module.Name.Split('.')[0].ToUpperInvariant();
            if (new[] { "CON", "PRN", "AUX", "NUL" }.Contains(basename) ||
                (basename.Length == 4 && (basename.StartsWith("COM", StringComparison.Ordinal) || basename.StartsWith("LPT", StringComparison.Ordinal)) && basename[3] is >= '1' and <= '9'))
            {
                throw new ArgumentException($"Module {module.Name}: reserved filesystem name.");
            }

            foreach (string argument in module.AdditionalCompileArguments)
            {
                ValidateArgument(module.Name, "AdditionalCompileArguments", argument, true);
            }

            foreach (string argument in module.AdditionalLinkArguments)
            {
                ValidateArgument(module.Name, "AdditionalLinkArguments", argument, false);
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
                // 구성 매크로는 빌드 시스템만 정의한다(모듈이 바꾸면 구성 판정이 모듈마다 달라진다).
                if (name.StartsWith("ROCKS_BUILD_", StringComparison.Ordinal))
                {
                    throw new ArgumentException($"Module {module.Name}: define '{define}' is reserved for the build configuration.");
                }
            }
        }
    }

    private static void ValidateArgument(string module, string field, string argument, bool compile)
    {
        string lower = argument.ToLowerInvariant();
        bool invalid = argument.Length == 0 || argument.IndexOfAny(['\0', '\r', '\n']) >= 0 || argument.StartsWith('@') ||
            lower is "-o" or "-c" or "/c" or "/link" || lower.StartsWith("/fo", StringComparison.Ordinal) ||
            lower.StartsWith("/fe", StringComparison.Ordinal) || lower.StartsWith("/fd", StringComparison.Ordinal) ||
            lower.StartsWith("/out:", StringComparison.Ordinal) || lower.StartsWith("/pdb:", StringComparison.Ordinal) ||
            lower.StartsWith("/implib:", StringComparison.Ordinal);
        if (compile)
        {
            invalid |= lower is "-w" or "/w" or "/w0" or "/wx-" or "-fexceptions" or "-frtti" or "/gr" or "/gr+" ||
                lower.StartsWith("-wno-", StringComparison.Ordinal) || lower.StartsWith("/wd", StringComparison.Ordinal) ||
                lower.StartsWith("/clang:", StringComparison.Ordinal) || lower.StartsWith("/eh", StringComparison.Ordinal) ||
                lower.StartsWith("-include", StringComparison.Ordinal) || lower.StartsWith("/fi", StringComparison.Ordinal) ||
                lower.StartsWith("-isystem", StringComparison.Ordinal) || lower.StartsWith("/external:", StringComparison.Ordinal) ||
                argument.StartsWith("-I", StringComparison.Ordinal) || argument.StartsWith("/I", StringComparison.Ordinal) ||
                argument.StartsWith("-D", StringComparison.Ordinal) || argument.StartsWith("/D", StringComparison.Ordinal) ||
                lower is "-xclang" or "-xpreprocessor" ||
                // 번역 단위 언어와 표준은 빌드 도구가 확장자로 정한다(.c → C17, 그 밖 → C++23).
                lower == "-x" || (lower.StartsWith("-x", StringComparison.Ordinal) && !lower.StartsWith("-xclang", StringComparison.Ordinal) &&
                                  !lower.StartsWith("-xpreprocessor", StringComparison.Ordinal)) ||
                lower.StartsWith("--language", StringComparison.Ordinal) || lower.StartsWith("-std=", StringComparison.Ordinal) ||
                lower.StartsWith("/std:", StringComparison.Ordinal) || lower is "/tc" or "/tp" ||
                (argument.Length > 3 && (argument.StartsWith("/Tc", StringComparison.Ordinal) || argument.StartsWith("/Tp", StringComparison.Ordinal))) ||
                // 최적화 수준·LTO는 구성과 NativeModuleInput.Optimization이 정한다. 수준을 바꾸지 않는
                // /Oy-(frame pointer 유지)·/Oi(intrinsic) 등은 허용한다.
                OptimizationLevel.IsMatch(argument) || lower.StartsWith("-flto", StringComparison.Ordinal) ||
                lower.StartsWith("-fno-lto", StringComparison.Ordinal);
        }
        else
        {
            // link 단계에서 LTO를 끄거나 backend 최적화 수준을 바꾸는 인자도 구성이 정한다.
            invalid |= lower.StartsWith("-flto", StringComparison.Ordinal) || lower.StartsWith("-fno-lto", StringComparison.Ordinal) ||
                lower.StartsWith("/opt:lldlto", StringComparison.Ordinal) || lower.StartsWith("/lldlto", StringComparison.Ordinal) ||
                lower.StartsWith("-wl,--lto", StringComparison.Ordinal) || lower.StartsWith("-wl,--thinlto", StringComparison.Ordinal) ||
                lower.StartsWith("-wl,-lto", StringComparison.Ordinal) || lower.StartsWith("--lto", StringComparison.Ordinal);
        }

        if (invalid)
        {
            throw new ArgumentException($"Module {module}, {field}: argument '{argument}' overrides owned output/policy or hides inputs; use structured settings.");
        }
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
