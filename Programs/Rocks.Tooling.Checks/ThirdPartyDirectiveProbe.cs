using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

/// <summary>
/// 경고 정책 "외부 원본 내부 진단 지시문" 규칙 (2)의 자동 확인이다(Docs/Design/Compression.ko.md 3.6). 외부 원본에 있는
/// 진단 지시문(<c>#pragma warning</c>·<c>clang/GCC diagnostic</c>, 경고 억제 define)이 실제로 가리는 경고가 없음을
/// 매 실행 증명한다.
/// <list type="number">
/// <item>compile DB에서 <c>Core/Source/ThirdParty/&lt;Lib&gt;/</c> 번역 단위를 찾는다(이번 구성·toolchain의 실제 인자).</item>
/// <item>라이브러리 폴더를 임시 위치로 복사하고 원본의 진단 지시문 줄을 빈 줄로 바꾼다(줄 번호 유지).</item>
/// <item>같은 인자로 문법 검사만 한다(출력·LTO 인자 제외). 진단이 하나라도 나오면 실패다.</item>
/// </list>
/// 지시문이 하나도 없는 라이브러리는 확인할 것이 없어 건너뛴다.
/// </summary>
public static class ThirdPartyDirectiveProbe
{
    private static readonly Regex DiagnosticDirective = new(
        "^\\s*#\\s*(?:/\\*.*?\\*/\\s*)*pragma\\s+(?:warning\\b|clang\\s+diagnostic\\b|GCC\\s+diagnostic\\b)" +
        "|^\\s*#\\s*define\\s+_(?:CRT|SCL)_\\w*NO_\\w*(?:WARNINGS|DEPRECATE)\\w*\\b",
        RegexOptions.CultureInvariant | RegexOptions.Compiled, TimeSpan.FromSeconds(1));

    /// <summary>
    /// 확인한 번역 단위 수를 돌려준다. 진단이 나오면 <see cref="InvalidOperationException"/>이다.
    /// </summary>
    public static async Task<int> VerifyAsync(string root, string compileCommands,
        IReadOnlyDictionary<string, string?> environment, CancellationToken cancellationToken = default)
    {
        root = Path.GetFullPath(root);
        // compile DB 경로와 비교하도록 플랫폼 구분자로 정규화한다.
        string thirdParty = Path.GetFullPath(Path.Combine(root, ThirdPartyManifest.ThirdPartyRoot)) + Path.DirectorySeparatorChar;
        using JsonDocument document = JsonDocument.Parse(File.ReadAllText(compileCommands, new UTF8Encoding(false, true)));
        var entries = document.RootElement.EnumerateArray()
            .Select(entry => (File: Path.GetFullPath(entry.GetProperty("file").GetString()!),
                Arguments: entry.GetProperty("arguments").EnumerateArray().Select(value => value.GetString()!).ToArray()))
            .Where(entry => entry.File.StartsWith(thirdParty, StringComparison.OrdinalIgnoreCase))
            .ToArray();

        string temporary = Path.Combine(Path.GetTempPath(), "rocks-thirdparty-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporary);
        int verified = 0;
        try
        {
            foreach (var library in entries.GroupBy(entry => entry.File[thirdParty.Length..].Split(Path.DirectorySeparatorChar, '/')[0],
                         StringComparer.OrdinalIgnoreCase))
            {
                string source = Path.Combine(thirdParty, library.Key);
                string copy = Path.Combine(temporary, library.Key);
                ThirdPartyManifest manifest = ThirdPartyManifest.Read(source);
                int stripped = CopyWithoutDiagnosticDirectives(source, copy, manifest.UpstreamDirectory);
                if (stripped == 0)
                {
                    continue;
                }

                List<string> failures = [];
                using var gate = new SemaphoreSlim(Math.Max(1, Environment.ProcessorCount / 2));
                await Task.WhenAll(library.Select(async entry =>
                {
                    await gate.WaitAsync(cancellationToken);
                    try
                    {
                        string[] arguments = SyntaxOnlyArguments(entry.Arguments, source, copy);
                        var result = await ProcessRunner.RunAsync(new(arguments[0], arguments[1..], temporary, environment,
                            Timeout: TimeSpan.FromMinutes(5)), cancellationToken);
                        string diagnostic = (result.StdOut + result.StdErr).Trim();
                        if (result.ExitCode != 0 || diagnostic.Contains("warning", StringComparison.OrdinalIgnoreCase) ||
                            diagnostic.Contains("error", StringComparison.OrdinalIgnoreCase))
                        {
                            lock (failures)
                            {
                                failures.Add($"{Path.GetFileName(entry.File)}: exit {result.ExitCode}\n{diagnostic}");
                            }
                        }
                    }
                    finally
                    {
                        gate.Release();
                    }
                }));
                if (failures.Count != 0)
                {
                    throw new InvalidOperationException(
                        $"{library.Key}: upstream diagnostic directives hide warnings (Compression.ko.md 3.6 rule 2):\n" +
                        string.Join("\n", failures));
                }

                verified += library.Count();
            }
        }
        finally
        {
            string actual = Path.GetFullPath(temporary);
            if (!Path.GetFileName(actual).StartsWith("rocks-thirdparty-", StringComparison.Ordinal) ||
                Path.GetDirectoryName(actual) != Path.TrimEndingDirectorySeparator(Path.GetFullPath(Path.GetTempPath())))
            {
                throw new InvalidOperationException("Refusing cleanup outside owned third-party fixture");
            }

            Directory.Delete(actual, recursive: true);
        }

        return verified;
    }

    // 라이브러리 폴더 전체를 복사하고, 원본 디렉터리 파일의 진단 지시문 논리 줄을 빈 줄로 바꾼다.
    private static int CopyWithoutDiagnosticDirectives(string source, string destination, string upstreamDirectory)
    {
        int stripped = 0;
        string upstream = Path.Combine(source, upstreamDirectory) + Path.DirectorySeparatorChar;
        foreach (string file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            string target = Path.Combine(destination, Path.GetRelativePath(source, file));
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            if (!file.StartsWith(upstream, StringComparison.OrdinalIgnoreCase))
            {
                File.Copy(file, target);
                continue;
            }

            string[] lines = Encoding.UTF8.GetString(File.ReadAllBytes(file)).Split('\n');
            for (int index = 0; index < lines.Length; ++index)
            {
                int start = index;
                string logical = lines[index];
                while (logical.EndsWith('\\') && index + 1 < lines.Length)
                {
                    logical = logical[..^1] + lines[++index];
                }

                if (!DiagnosticDirective.IsMatch(logical.TrimStart('﻿')))
                {
                    continue;
                }

                for (int blank = start; blank <= index; ++blank)
                {
                    lines[blank] = "";
                }

                ++stripped;
            }

            File.WriteAllBytes(target, Encoding.UTF8.GetBytes(string.Join('\n', lines)));
        }

        return stripped;
    }

    // compile DB 인자를 문법 검사 전용으로 바꾼다. 경고·정의·include·표준·구성 인자는 그대로 두고 출력·LTO·코드 생성 단계
    // 인자만 뺀다(남겨 두면 "argument unused" 진단이 -Werror로 실패한다).
    private static string[] SyntaxOnlyArguments(IReadOnlyList<string> arguments, string library, string copy)
    {
        string Rewrite(string argument) => argument
            .Replace(library, copy, StringComparison.OrdinalIgnoreCase)
            .Replace(library.Replace('\\', '/'), copy.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase);

        bool clangCl = arguments.Contains("/WX", StringComparer.Ordinal);
        List<string> result = [];
        for (int index = 0; index < arguments.Count; ++index)
        {
            string argument = arguments[index];
            if (argument is "-c" or "/c" or "-flto=thin" || argument.StartsWith("/Fo", StringComparison.Ordinal))
            {
                continue;
            }

            if (argument == "-o")
            {
                ++index;
                continue;
            }

            result.Add(Rewrite(argument));
        }

        result.Insert(1, clangCl ? "/Zs" : "-fsyntax-only");
        return result.ToArray();
    }
}
