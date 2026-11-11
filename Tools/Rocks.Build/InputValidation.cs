using Rocks.Tooling.Common;

namespace Rocks.Build;

internal static class InputValidation
{
    public static void Validate(NativeBuildRequest request)
    {
        if (request.MaxParallelism < 1 || request.MaxParallelism > ProcessRunner.MaximumConcurrentProcesses)
            throw new ArgumentOutOfRangeException(nameof(request.MaxParallelism),
                $"MaxParallelism must be between 1 and {ProcessRunner.MaximumConcurrentProcesses}.");
        if (!Enum.IsDefined(request.Configuration)) throw new ArgumentException($"Configuration: unsupported value {request.Configuration}.");
        // enum 멤버가 추가돼도 실제 타깃 지원을 검토하기 전에는 통과시키지 않는다.
        bool hostMatches = request.Toolchain.Platform switch
        {
            NativePlatform.Windows => OperatingSystem.IsWindows(),
            NativePlatform.MacOS => OperatingSystem.IsMacOS(),
            _ => throw new ArgumentException($"Toolchain.Platform: unsupported value {request.Toolchain.Platform}.")
        };
        if (!hostMatches)
            throw new PlatformNotSupportedException($"Toolchain.Platform {request.Toolchain.Platform}: this native host is not supported for that target.");
        if (NativeToolchains.PathComparer.Equals(request.RootDirectory, request.OutputDirectory) ||
            Fingerprints.IsUnder(request.RootDirectory, request.OutputDirectory))
            throw new ArgumentException($"OutputDirectory must not be the source root or its ancestor: {request.OutputDirectory}");
        foreach (var pair in new[] { ("CompilerPath", request.Toolchain.CompilerPath), ("ArchiverPath", request.Toolchain.ArchiverPath) })
            if (!Path.IsPathFullyQualified(pair.Item2) || !File.Exists(pair.Item2))
                throw new ArgumentException($"Toolchain.{pair.Item1}: expected an existing absolute executable path: {pair.Item2}");
        if (request.Toolchain.LinkerPath is string linker && (!Path.IsPathFullyQualified(linker) || !File.Exists(linker)))
            throw new ArgumentException($"Toolchain.LinkerPath: expected an existing absolute path: {linker}");
        if (string.IsNullOrWhiteSpace(request.Toolchain.Identity)) throw new ArgumentException("Toolchain.Identity must not be empty.");
        foreach (var module in request.Modules)
        {
            if (!Enum.IsDefined(module.Kind)) throw new ArgumentException($"Module {module.Name}: unsupported Kind {module.Kind}.");
            string basename = module.Name.Split('.')[0].ToUpperInvariant();
            if (new[] { "CON", "PRN", "AUX", "NUL" }.Contains(basename) ||
                (basename.Length == 4 && (basename.StartsWith("COM", StringComparison.Ordinal) || basename.StartsWith("LPT", StringComparison.Ordinal)) && basename[3] is >= '1' and <= '9'))
                throw new ArgumentException($"Module {module.Name}: reserved filesystem name.");
            foreach (string argument in module.AdditionalCompileArguments)
                ValidateArgument(module.Name, "AdditionalCompileArguments", argument, true);
            foreach (string argument in module.AdditionalLinkArguments)
                ValidateArgument(module.Name, "AdditionalLinkArguments", argument, false);
            foreach (string define in module.PublicDefines.Concat(module.PrivateDefines))
            {
                string name = define.Split('=')[0];
                if (name.Length == 0 || (!char.IsAsciiLetter(name[0]) && name[0] != '_') ||
                    name.Any(value => !char.IsAsciiLetterOrDigit(value) && value != '_') || define.IndexOfAny(['\0', '\r', '\n']) >= 0)
                    throw new ArgumentException($"Module {module.Name}: invalid define '{define}'.");
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
                lower is "-xclang" or "-xpreprocessor";
        }
        if (invalid) throw new ArgumentException($"Module {module}, {field}: argument '{argument}' overrides owned output/policy or hides inputs; use structured settings.");
    }
}
