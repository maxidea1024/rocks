using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

public static class FormatCheck
{
    public const string RequiredVersion = "21.1.6";

    /// <summary>고정 버전 clang-format으로 검증한다. apply=false이면 입력 소스를 변경하지 않는다.</summary>
    public static async Task CheckAsync(string root, string formatter, bool apply = false,
        IReadOnlyDictionary<string, string?>? environment = null, CancellationToken cancellationToken = default)
    {
        var version = await ProcessRunner.RunAsync(new(formatter, ["--version"], root, environment,
            Timeout: TimeSpan.FromSeconds(15)), cancellationToken);
        if (version.ExitCode != 0 || !System.Text.RegularExpressions.Regex.IsMatch(version.StdOut,
            @"version\s+21\.1\.6(?:\s|$)", System.Text.RegularExpressions.RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1)))
            throw new InvalidOperationException("Rocks requires clang-format " + RequiredVersion);
        IReadOnlyList<string> files = PolicyChecks.NativeSources(root);
        for (int offset = 0; offset < files.Count; offset += 40)
        {
            List<string> arguments = apply ? ["-i"] : ["--dry-run", "--Werror"];
            arguments.AddRange(files.Skip(offset).Take(40));
            var result = await ProcessRunner.RunAsync(new(formatter, arguments, root, environment,
                Timeout: TimeSpan.FromSeconds(60)), cancellationToken);
            if (result.ExitCode != 0)
                throw new InvalidOperationException("clang-format failed:\n" + result.StdOut + result.StdErr);
        }
    }
}
