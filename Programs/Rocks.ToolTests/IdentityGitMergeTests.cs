using System.Collections;
using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 실제 merge commit에 대해 CI와 같은 PowerShell 진입점을 외부 프로세스로 실행한다.
// fixture는 임시 Git 저장소에만 쓰며 사용자 Git 설정, 현재 저장소, 외부 원격은 변경하지 않는다.
internal static class IdentityGitMergeTests
{
    private const string Original = "10000000000000000000000000000001";
    private const string Left = "10000000000000000000000000000002";
    private const string Right = "10000000000000000000000000000003";
    private const string Field = "20000000000000000000000000000001";

    public static void Register(TestSuite suite, Options options)
    {
        string script = Path.Combine(options.Root, "Build", "CheckIdentityMerges.ps1");
        string compiler = Path.GetFullPath(options.Optional("metacompiler") ?? typeof(StableIdManifest).Assembly.Location);
        suite.Add("IdentityGit.ValidRenameAndIndependentBranchAdditions", async () =>
        {
            using Fixture fixture = await Fixture.CreateAsync(script, compiler);
            object[] basis = [Type("sample::Old", Original), Property("sample::Old::health", Field)];
            object[] ours = [Type("sample::New", Original, "sample::Old"),
                Property("sample::New::health", Field, "sample::Old::health"), Type("sample::Left", Left)];
            object[] theirs = [.. basis, Type("sample::Right", Right)];
            object[] merge = [.. ours, Type("sample::Right", Right)];
            string common = await fixture.CommitAsync(basis);
            string first = await fixture.CommitAsync(ours, common);
            string second = await fixture.CommitAsync(theirs, common);
            await fixture.SetHeadAsync(await fixture.CommitAsync(merge, first, second));
            ProcessResult result = await fixture.CheckAsync();
            Verify.Equal(0, result.ExitCode, "valid Git merge: " + result.StdOut + result.StdErr);
            Verify.Contains("Stable identity history checked: 1 manifest(s).", result.StdOut, "manifest path was not discovered");
        });

        suite.Add("IdentityGit.ConcurrentSameNameDifferentIdsRejected", async () =>
        {
            using Fixture fixture = await Fixture.CreateAsync(script, compiler);
            string common = await fixture.CommitAsync([]);
            string first = await fixture.CommitAsync([Type("sample::Same", Left)], common);
            string second = await fixture.CommitAsync([Type("sample::Same", Right)], common);
            await fixture.SetHeadAsync(await fixture.CommitAsync([Type("sample::Same", Left)], first, second));
            ProcessResult result = await fixture.CheckAsync();
            Verify.That(result.ExitCode != 0, "conflicting IDs were accepted by CI script");
            Verify.Contains("name collision", result.StdOut + result.StdErr, "missing conflicting-name diagnosis");
        });

        suite.Add("IdentityGit.ResolvedMergeMustPreserveBothParents", async () =>
        {
            using Fixture fixture = await Fixture.CreateAsync(script, compiler);
            object[] basis = [Type("sample::Old", Original)];
            string common = await fixture.CommitAsync(basis);
            string first = await fixture.CommitAsync([.. basis, Type("sample::Left", Left)], common);
            string second = await fixture.CommitAsync([.. basis, Type("sample::Right", Right)], common);
            // 충돌 해결 중 왼쪽의 ID를 누락한 merge 결과를 명시적으로 만든다.
            await fixture.SetHeadAsync(await fixture.CommitAsync([.. basis, Type("sample::Right", Right)], first, second));
            ProcessResult result = await fixture.CheckAsync();
            Verify.That(result.ExitCode != 0, "merge losing an existing identity was accepted");
            Verify.Contains("disappeared", result.StdOut + result.StdErr, "missing lost-identity diagnosis");
        });

        suite.Add("IdentityGit.NonMergeAndOctopusAreRejected", async () =>
        {
            using Fixture fixture = await Fixture.CreateAsync(script, compiler);
            string common = await fixture.CommitAsync([]);
            string first = await fixture.CommitAsync([Type("sample::First", Original)], common);
            await fixture.SetHeadAsync(first);
            ProcessResult nonMerge = await fixture.CheckAsync();
            Verify.That(nonMerge.ExitCode != 0, "single-parent commit was accepted");
            Verify.Contains("two-parent PR merge commit", nonMerge.StdOut + nonMerge.StdErr, "missing parent-count diagnostic");
            string second = await fixture.CommitAsync([Type("sample::Second", Left)], common);
            string third = await fixture.CommitAsync([Type("sample::Third", Right)], common);
            await fixture.SetHeadAsync(await fixture.CommitAsync([], first, second, third));
            ProcessResult octopus = await fixture.CheckAsync();
            Verify.That(octopus.ExitCode != 0, "three-parent merge was accepted");
            Verify.Contains("two-parent PR merge commit", octopus.StdOut + octopus.StdErr, "missing octopus diagnostic");
        });
    }

    private static object Type(string name, string id, params string[] aliases) =>
        new { kind = "type", name, id, aliases, retired = false };

    private static object Property(string name, string id, params string[] aliases) =>
        new { kind = "field", name, id, aliases, retired = false };

    private sealed class Fixture : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();
        private readonly string script;
        private readonly string compiler;
        private readonly Dictionary<string, string?> environment;
        private const string Manifest = "Folder 한글/identity example.rocksids.json";
        private int counter;

        private Fixture(string scriptPath, string compilerPath)
        {
            script = Path.GetFullPath(scriptPath);
            compiler = compilerPath;
            environment = Environment.GetEnvironmentVariables().Cast<DictionaryEntry>()
                .Where(pair => pair.Key is string key && !key.StartsWith("GIT_", StringComparison.OrdinalIgnoreCase))
                .ToDictionary(pair => (string)pair.Key, pair => pair.Value?.ToString(), StringComparer.OrdinalIgnoreCase);
            environment["GIT_CONFIG_GLOBAL"] = temporary.FilePath("no-global-git-config");
            environment["GIT_CONFIG_NOSYSTEM"] = "1";
            environment["GIT_TERMINAL_PROMPT"] = "0";
        }

        public static async Task<Fixture> CreateAsync(string script, string compiler)
        {
            Fixture fixture = new(script, compiler);
            try
            {
                await fixture.GitAsync("init", "--quiet", "--initial-branch=identity-fixture");
                await fixture.GitAsync("config", "--local", "user.name", "Rocks Identity Test");
                await fixture.GitAsync("config", "--local", "user.email", "rocks-identity-test@example.invalid");
                await fixture.GitAsync("config", "--local", "commit.gpgSign", "false");
                return fixture;
            }
            catch
            {
                fixture.Dispose();
                throw;
            }
        }

        public async Task<string> CommitAsync(object[] entries, params string[] parents)
        {
            temporary.Write(Manifest, JsonSerializer.Serialize(new { schema = 1, entries }));
            await GitAsync("add", "--", Manifest);
            string tree = (await GitAsync("write-tree")).Trim();
            List<string> arguments = ["commit-tree", tree, "-m", "identity fixture " + ++counter];
            foreach (string parent in parents)
            {
                arguments.Add("-p");
                arguments.Add(parent);
            }

            return (await GitAsync(arguments.ToArray())).Trim();
        }

        public async Task SetHeadAsync(string commit) => await GitAsync("update-ref", "HEAD", commit);

        public Task<ProcessResult> CheckAsync() => ProcessRunner.RunAsync(new ProcessSpec("pwsh",
            ["-NoProfile", "-NonInteractive", "-File", script, "-MetaCompiler", compiler], temporary.Root,
            environment, Timeout: TimeSpan.FromSeconds(60)));

        private async Task<string> GitAsync(params string[] arguments)
        {
            ProcessResult result = await ProcessRunner.RunAsync(new ProcessSpec("git", arguments, temporary.Root,
                environment, Timeout: TimeSpan.FromSeconds(30)));
            if (result.ExitCode != 0)
            {
                throw new InvalidOperationException("Git fixture failed: " + result.StdErr);
            }

            return result.StdOut;
        }

        public void Dispose()
        {
            // Windows Git은 느슨한 object를 읽기 전용으로 만든다. 이 fixture 소유 .git 아래만
            // 속성을 해제하고, reparse point는 따라가지 않는다.
            string gitDirectory = Path.Combine(temporary.Root, ".git");
            if (Directory.Exists(gitDirectory))
            {
                var enumeration = new EnumerationOptions
                {
                    RecurseSubdirectories = true,
                    AttributesToSkip = FileAttributes.ReparsePoint,
                    IgnoreInaccessible = false
                };
                foreach (string file in Directory.EnumerateFiles(gitDirectory, "*", enumeration))
                {
                    FileAttributes attributes = File.GetAttributes(file);
                    if ((attributes & FileAttributes.ReadOnly) != 0)
                    {
                        File.SetAttributes(file, attributes & ~FileAttributes.ReadOnly);
                    }
                }
            }

            temporary.Dispose();
        }
    }
}
