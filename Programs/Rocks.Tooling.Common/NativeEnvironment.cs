using System.Collections;
using System.Text;

namespace Rocks.Tooling.Common;

public static class NativeEnvironment
{
    public static async Task<Dictionary<string, string?>> CreateAsync(string? compiler = null,
        CancellationToken cancellationToken = default)
    {
        var environment = new Dictionary<string, string?>(OperatingSystem.IsWindows()
            ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        foreach (DictionaryEntry item in System.Environment.GetEnvironmentVariables())
            environment.Add((string)item.Key, (string?)item.Value);
        if (OperatingSystem.IsWindows() && !environment.ContainsKey("VCToolsInstallDir"))
        {
            var installer = Path.Combine(System.Environment.GetFolderPath(
                System.Environment.SpecialFolder.ProgramFilesX86), "Microsoft Visual Studio", "Installer", "vswhere.exe");
            var result = await ProcessRunner.RunAsync(new(installer,
                ["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"], Directory.GetCurrentDirectory(), environment), cancellationToken);
            var installation = result.StdOut.Trim();
            if (result.ExitCode != 0 || installation.Length == 0)
                throw new InvalidOperationException("Visual Studio C++ toolchain not found.");
            var script = Path.Combine(installation, "Common7", "Tools", "VsDevCmd.bat");
            if (script.IndexOfAny(['\"', '\r', '\n', '%']) >= 0)
                throw new InvalidOperationException("Unsupported Visual Studio installation path.");
            // cmd는 MS 제공 환경 진입점에만 사용한다. 사용자 compiler/source 인자는 shell을 통과하지 않는다.
            result = await ProcessRunner.RunAsync(new("cmd.exe",
                ["/d", "/u", "/c", "call", script, "-no_logo", "-arch=x64", "-host_arch=x64", ">nul", "&&", "set"],
                Directory.GetCurrentDirectory(), environment, StandardOutputEncoding: Encoding.Unicode, StandardErrorEncoding: Encoding.Unicode), cancellationToken);
            if (result.ExitCode != 0)
                throw new InvalidOperationException("Visual Studio environment initialization failed: " + result.StdErr);
            foreach (var line in result.StdOut.Split('\n'))
            {
                var separator = line.IndexOf('=');
                if (separator > 0)
                    environment[line[..separator]] = line[(separator + 1)..].TrimEnd('\r');
            }
        }
        if (compiler is not null && OperatingSystem.IsWindows())
        {
            var result = await ProcessRunner.RunAsync(new(compiler, ["/clang:-print-resource-dir"],
                Directory.GetCurrentDirectory(), environment), cancellationToken);
            if (result.ExitCode == 0)
            {
                var resource = result.StdOut.Trim();
                var directories = new[] { "lib/x86_64-unknown-windows-msvc", "lib/x86_64-pc-windows-msvc", "lib/windows" }
                    .Select(relative => Path.Combine(resource, relative)).Where(Directory.Exists);
                environment["PATH"] = string.Join(Path.PathSeparator, directories) + Path.PathSeparator + environment["PATH"];
            }
        }
        return environment;
    }

    public static string FindExecutable(string name, IReadOnlyDictionary<string, string?> environment)
    {
        if (Path.IsPathRooted(name) || name.Contains(Path.DirectorySeparatorChar) || name.Contains(Path.AltDirectorySeparatorChar))
        {
            if (File.Exists(name))
                return Path.GetFullPath(name);
            throw new FileNotFoundException("Executable not found.", name);
        }
        var path = environment.FirstOrDefault(pair => string.Equals(pair.Key, "PATH",
            OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal)).Value ?? "";
        var suffixes = OperatingSystem.IsWindows() && Path.GetExtension(name).Length == 0
            ? new[] { ".exe", ".cmd", ".bat", "" } : new[] { "" };
        foreach (var directory in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            foreach (var suffix in suffixes)
            {
                var candidate = Path.Combine(directory.Trim('"'), name + suffix);
                if (File.Exists(candidate))
                    return Path.GetFullPath(candidate);
            }
        throw new FileNotFoundException("Executable not found on PATH.", name);
    }
}
