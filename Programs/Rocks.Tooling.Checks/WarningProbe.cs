using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

public static class WarningProbe
{
    /// <summary>매번 새 TU/object를 사용하여 이전 산출물이 경고 음성 대조를 만족하지 못하게 한다.</summary>
    public static async Task VerifyAsync(string compiler, string workingDirectory,
        IReadOnlyDictionary<string, string?> environment, CancellationToken cancellationToken = default)
    {
        string temporary = Path.Combine(Path.GetTempPath(), "rocks-warning-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        try
        {
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            foreach (bool warning in new[] { false, true })
            {
                string source = Path.Combine(temporary, warning ? "warning.cpp" : "clean.cpp");
                string output = Path.Combine(temporary, warning ? "warning.o" : "clean.o");
                File.WriteAllText(source, warning ? "int RocksWarningProbe() noexcept { int unusedWarningProbe = 7; return 0; }\n" :
                    "int RocksWarningProbe() noexcept { return 0; }\n");
                List<string> arguments = clangCl ? ["/nologo", "/std:c++latest", "/W4", "/WX", "/c", source, "/Fo" + output] :
                    ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-c", source, "-o", output];
                var result = await ProcessRunner.RunAsync(new(compiler, arguments, workingDirectory, environment,
                    Timeout: TimeSpan.FromSeconds(60)), cancellationToken);
                string diagnostic = result.StdOut + result.StdErr;
                if (!warning && (result.ExitCode != 0 || !File.Exists(output)))
                {
                    throw new InvalidOperationException("Clean native compiler control failed:\n" + diagnostic);
                }

                if (warning && (result.ExitCode == 0 || !diagnostic.Contains("unusedWarningProbe", StringComparison.Ordinal) ||
                    !(diagnostic.Contains("-Werror", StringComparison.Ordinal) || diagnostic.Contains("C2220", StringComparison.Ordinal))))
                {
                    throw new InvalidOperationException("Compiler did not reject the required warning:\n" + diagnostic);
                }
            }
        }
        finally
        {
            string actual = Path.GetFullPath(temporary);
            if (!Path.GetFileName(actual).StartsWith("rocks-warning-", StringComparison.Ordinal) ||
                Path.GetDirectoryName(actual) != Path.TrimEndingDirectorySeparator(Path.GetFullPath(Path.GetTempPath())))
            {
                throw new InvalidOperationException("Refusing cleanup outside owned warning fixture");
            }

            Directory.Delete(actual, recursive: true);
        }
    }
}
