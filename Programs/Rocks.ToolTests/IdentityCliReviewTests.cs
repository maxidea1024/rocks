using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 실제 메타컴파일러 프로세스의 종료 코드·진단과 manifest 원자성을 함께 검사한다.
internal static class IdentityCliReviewTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.IdentityCliAddRenameRetireAndFailures", async () =>
        {
            using TemporaryDirectory temporary = new();
            string manifest = temporary.FilePath("identities.rocksids.json");
            var missingOwner = await Run(options, environment, "add", "--manifest", manifest, "--field", "demo::Missing::value");
            Error(missingOwner, "owner");
            Verify.That(!File.Exists(manifest), "failed add published a partial manifest");
            Success(await Run(options, environment, "add", "--manifest", manifest, "--type", "demo::Actor",
                "--field", "demo::Actor::health"));
            StableIdManifest before = StableIdManifest.Load(manifest);
            string type = before.Resolve("type", "demo::Actor");
            string field = before.Resolve("field", "demo::Actor::health");
            Verify.That(Guid.TryParseExact(type, "N", out Guid typeGuid) && typeGuid != Guid.Empty &&
                Guid.TryParseExact(field, "N", out Guid fieldGuid) && fieldGuid != Guid.Empty && type != field,
                "add did not issue distinct nonzero IDs");
            byte[] bytes = File.ReadAllBytes(manifest);
            Error(await Run(options, environment, "add", "--manifest", manifest, "--type", "demo::Actor"), "Duplicate");
            Verify.That(bytes.SequenceEqual(File.ReadAllBytes(manifest)), "duplicate add changed existing manifest");
            Error(await Run(options, environment, "rename", "--manifest", manifest, "--kind", "type", "--old", "demo::Missing", "--new", "demo::Actor"), "does not exist");
            Verify.That(bytes.SequenceEqual(File.ReadAllBytes(manifest)), "failed rename changed manifest");
            Success(await Run(options, environment, "rename", "--manifest", manifest, "--kind", "type", "--old", "demo::Actor", "--new", "demo::Player"));
            Success(await Run(options, environment, "rename", "--manifest", manifest, "--kind", "field", "--old", "demo::Player::health", "--new", "demo::Player::hitPoints"));
            StableIdManifest renamed = StableIdManifest.Load(manifest);
            Verify.Equal(type, renamed.Resolve("type", "demo::Player"), "type rename changed ID");
            Verify.Equal(field, renamed.Resolve("field", "demo::Player::hitPoints"), "field rename changed ID");
            Verify.That(renamed.Entries.Single(e => e.Id == field).Aliases.Contains("demo::Actor::health"), "field history missing");
            Success(await Run(options, environment, "retire", "--manifest", manifest, "--kind", "type", "--name", "demo::Player"));
            StableIdManifest retired = StableIdManifest.Load(manifest);
            Verify.That(retired.Entries.Where(e => e.Id == type || e.Id == field).All(e => e.Retired), "type retirement did not retire child field");
            bytes = File.ReadAllBytes(manifest);
            Error(await Run(options, environment, "add", "--manifest", manifest, "--type", "demo::Actor"), "Duplicate");
            Verify.That(bytes.SequenceEqual(File.ReadAllBytes(manifest)), "retired name reuse changed manifest");
            Error(await Run(options, environment, "add", "--manifest", temporary.FilePath("wrong.json"), "--type", "demo::X"), ".rocksids.json");
            Error(await Run(options, environment, "add", "--manifest", manifest, "--type", "demo::X", "--unexpected", "x"), "Unknown ids option");
        });

        suite.Add("Meta.IdentityCliMultipleAddsAreAtomic", async () =>
        {
            using TemporaryDirectory temporary = new();
            string manifest = temporary.FilePath("identities.rocksids.json");
            Success(await Run(options, environment, "add", "--manifest", manifest, "--type", "demo::Actor"));
            byte[] before = File.ReadAllBytes(manifest);
            Error(await Run(options, environment, "add", "--manifest", manifest, "--type", "demo::New",
                "--field", "missing::Owner::value"), "owner");
            Verify.That(before.SequenceEqual(File.ReadAllBytes(manifest)), "failed batch added an earlier valid type");
            string fresh = temporary.FilePath("fresh.rocksids.json");
            Error(await Run(options, environment, "add", "--manifest", fresh, "--type", "demo::Repeated",
                "--type", "demo::Repeated"), "Duplicate");
            Verify.That(!File.Exists(fresh), "failed duplicate batch published partial new manifest");
            Verify.That(!File.Exists(fresh + ".lock") && !File.Exists(manifest + ".lock"), "failed batch left a lock");
        });

        suite.Add("Meta.IdentityCliDamagedManifestNeverPublishes", async () =>
        {
            using TemporaryDirectory temporary = new();
            string manifest = temporary.FilePath("damaged.rocksids.json");
            foreach (string content in new[] { "{", "{\"schema\":2,\"entries\":[]}",
                "{\"schema\":1,\"entries\":[{\"kind\":\"type\",\"name\":\"demo::Broken\"}]}",
                "{\"schema\":1,\"entries\":[{\"kind\":\"type\",\"name\":\"demo::Broken\",\"id\":\"not-a-guid\",\"aliases\":[],\"retired\":false}]}" })
            {
                File.WriteAllText(manifest, content);
                byte[] before = File.ReadAllBytes(manifest);
                ProcessResult rejected = await Run(options, environment, "add", "--manifest", manifest, "--type", "demo::New");
                Verify.Equal(1, rejected.ExitCode, "damaged manifest exit");
                Verify.Contains("error RMC1000", rejected.StdErr, "damaged manifest error");
                Verify.Contains("hint:", rejected.StdErr, "damaged manifest hint");
                Verify.That(before.SequenceEqual(File.ReadAllBytes(manifest)), "damaged manifest was modified");
                Verify.That(!File.Exists(manifest + ".lock"), "invalid manifest left a lock");
            }
        });

        suite.Add("Meta.IdentityCliMergeCheckExitAndDiagnostics", async () =>
        {
            using TemporaryDirectory temporary = new();
            string common = Manifest(temporary, "base.json");
            string ours = temporary.FilePath("ours.rocksids.json");
            string theirs = temporary.FilePath("theirs.rocksids.json");
            Success(await Run(options, environment, "add", "--manifest", ours, "--type", "demo::New"));
            Success(await Run(options, environment, "add", "--manifest", theirs, "--type", "demo::New"));
            string ourId = StableIdManifest.Load(ours).Resolve("type", "demo::New");
            string theirId = StableIdManifest.Load(theirs).Resolve("type", "demo::New");
            Verify.That(ourId != theirId, "independent branch allocations unexpectedly matched");
            ProcessResult reject = await Run(options, environment, "merge-check", "--base", common, "--ours", ours, "--theirs", theirs, "--merged", ours);
            Verify.Equal(1, reject.ExitCode, "unsafe merge must fail");
            Verify.Contains("identity merge:", reject.StdErr, "merge failure diagnostic");
            Verify.Contains("name collision", reject.StdErr, "same name collision diagnostic");
            string disjoint = temporary.FilePath("theirs-disjoint.rocksids.json");
            Success(await Run(options, environment, "add", "--manifest", disjoint, "--type", "demo::Other"));
            string otherId = StableIdManifest.Load(disjoint).Resolve("type", "demo::Other");
            string correct = Manifest(temporary, "merged-correct.json", ("demo::New", ourId), ("demo::Other", otherId));
            ProcessResult accepted = await Run(options, environment, "merge-check", "--base", common, "--ours", ours, "--theirs", disjoint, "--merged", correct);
            Success(accepted);
            Verify.Contains("Stable identity merge: PASS", accepted.StdOut, "merge success diagnostic");
        });
    }

    private static string Manifest(TemporaryDirectory temporary, string path, params (string Name, string Id)[] entries)
    {
        temporary.Write(path, JsonSerializer.Serialize(new { schema = 1, entries = entries.Select(e => new { kind = "type", name = e.Name, id = e.Id, aliases = Array.Empty<string>(), retired = false }) }));
        return temporary.FilePath(path);
    }

    private static async Task<ProcessResult> Run(Options options, IReadOnlyDictionary<string, string?> environment, params string[] arguments)
    {
        string generator = Path.GetFullPath(options.Required("metacompiler"));
        string executable = generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) ? options.Optional("dotnet") ?? "dotnet" : generator;
        List<string> command = [];
        if (executable != generator)
        {
            command.Add(generator);
        }

        command.Add("ids");
        command.AddRange(arguments);
        return await ProcessRunner.RunAsync(new(executable, command, options.Root, environment, Timeout: TimeSpan.FromSeconds(30)));
    }

    private static void Success(ProcessResult result) => Verify.Equal(0, result.ExitCode, "identity command: " + result.StdErr);

    private static void Error(ProcessResult result, string message)
    {
        Verify.Equal(1, result.ExitCode, "identity command error exit: " + result.StdErr);
        Verify.Contains("error RMC1000", result.StdErr, "identity command error code");
        Verify.Contains("hint:", result.StdErr, "identity command hint");
        Verify.Contains(message, result.StdErr, "identity command failure reason");
    }
}
