using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈마다 명세를 두는 구성의 identity 명령 시험이다(Docs/Design/ReflectionFoundations.ko.md 1.6절): 명세 사이로 타입을
// 옮기는 ids move, 명세 집합의 ID 중복을 찾는 ids verify-set, 리비전마다 명세를 ID 기준으로 합쳐 보는 merge-check.
internal static class IdentityModuleCliTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.IdentityCliMoveKeepsIdsAndIsRerunnable", async () =>
        {
            using TemporaryDirectory temporary = new();
            string from = temporary.FilePath("Lower.rocksids.json");
            string to = temporary.FilePath("Upper.rocksids.json");
            Success(await Run(options, environment, "add", "--manifest", from, "--type", "demo::Actor", "--field", "demo::Actor::health",
                "--field", "demo::Actor::old", "--type", "demo::Other"));
            Success(await Run(options, environment, "retire", "--manifest", from, "--kind", "field", "--name", "demo::Actor::old"));
            StableIdManifest original = StableIdManifest.Load(from);
            byte[] beforeMove = File.ReadAllBytes(from);

            Success(await Run(options, environment, "move", "--from", from, "--to", to, "--type", "demo::Actor"));
            Verify.That(!File.ReadAllBytes(to).Contains((byte)'\r') && !File.ReadAllBytes(from).Contains((byte)'\r'), "manifests must use LF on every platform");
            StableIdManifest moved = StableIdManifest.Load(to);
            foreach (StableIdEntry entry in original.Entries.Where(entry => entry.Name.StartsWith("demo::Actor", StringComparison.Ordinal)))
            {
                Verify.That(moved.Entries.Any(value => value.Kind == entry.Kind && value.Name == entry.Name && value.Id == entry.Id && value.Retired == entry.Retired),
                    $"move lost or changed {entry.Kind} {entry.Name}");
            }

            StableIdManifest remaining = StableIdManifest.Load(from);
            Verify.That(remaining.Entries.Select(entry => entry.Name).SequenceEqual(["demo::Other"]), "move left entries in the source manifest");
            Success(await Run(options, environment, "verify-set", "--manifest", from, "--manifest", to));

            // 대상에 붙인 뒤 원본에서 떼기 전에 멈춘 상태: 두 명세에 같은 ID가 있어 verify-set이 실패하고, 같은 명령을
            // 다시 실행하면 대상은 그대로 두고 원본에서만 뗀다.
            File.WriteAllBytes(from, beforeMove);
            byte[] target = File.ReadAllBytes(to);
            ProcessResult interrupted = await Run(options, environment, "verify-set", "--manifest", from, "--manifest", to);
            Verify.Equal(1, interrupted.ExitCode, "verify-set accepted an interrupted move");
            Verify.Contains("error RMC2101", interrupted.StdErr, "shared ID code");
            Verify.Contains(original.Resolve("type", "demo::Actor"), interrupted.StdErr, "shared ID value");
            Success(await Run(options, environment, "move", "--from", from, "--to", to, "--type", "demo::Actor"));
            Verify.That(target.SequenceEqual(File.ReadAllBytes(to)), "rerun changed the target manifest");
            Success(await Run(options, environment, "verify-set", "--manifest", from, "--manifest", to));

            // 이미 옮긴 타입·같은 명세·타입 없는 호출은 명세를 바꾸지 않고 거부한다.
            byte[] source = File.ReadAllBytes(from);
            Error(await Run(options, environment, "move", "--from", from, "--to", to, "--type", "demo::Actor"), "does not exist");
            Error(await Run(options, environment, "move", "--from", from, "--to", from, "--type", "demo::Other"), "different manifests");
            Error(await Run(options, environment, "move", "--from", from, "--to", to), "--type");
            Verify.That(source.SequenceEqual(File.ReadAllBytes(from)) && target.SequenceEqual(File.ReadAllBytes(to)), "rejected move changed a manifest");
        });

        // 중첩 타입은 지정 순서와 무관하게 깊은 것부터 옮기고, 이름 변경 이력(별칭)도 그대로 옮긴다.
        suite.Add("Meta.IdentityCliMoveHandlesNestedTypesAndAliases", async () =>
        {
            using TemporaryDirectory temporary = new();
            string from = temporary.FilePath("Lower.rocksids.json");
            string to = temporary.FilePath("Upper.rocksids.json");
            Success(await Run(options, environment, "add", "--manifest", from, "--type", "demo::Outer", "--type", "demo::Outer::Inner",
                "--field", "demo::Outer::Inner::value"));
            Success(await Run(options, environment, "rename", "--manifest", from, "--kind", "type", "--old", "demo::Outer::Inner", "--new", "demo::Outer::Core"));
            StableIdManifest original = StableIdManifest.Load(from);
            Success(await Run(options, environment, "move", "--from", from, "--to", to, "--type", "demo::Outer", "--type", "demo::Outer::Core"));
            StableIdManifest moved = StableIdManifest.Load(to);
            Verify.Equal(0, StableIdManifest.Load(from).Entries.Count, "nested move left entries");
            StableIdEntry core = moved.Entries.Single(entry => entry.Kind == "type" && entry.Name == "demo::Outer::Core");
            Verify.Equal(original.Resolve("type", "demo::Outer::Core"), core.Id, "moved nested type ID");
            Verify.That(core.Aliases.Contains("demo::Outer::Inner"), "move dropped the rename history");
        });

        suite.Add("Meta.IdentityCliVerifySetRejectsSharedActiveAndRetiredIds", async () =>
        {
            using TemporaryDirectory temporary = new();
            string first = temporary.FilePath("First.rocksids.json");
            Success(await Run(options, environment, "add", "--manifest", first, "--type", "demo::Actor"));
            string id = StableIdManifest.Load(first).Resolve("type", "demo::Actor");
            string disjoint = temporary.FilePath("Disjoint.rocksids.json");
            Success(await Run(options, environment, "add", "--manifest", disjoint, "--type", "demo::Other"));
            Success(await Run(options, environment, "verify-set", "--manifest", first, "--manifest", disjoint, "--manifest", first));

            foreach (bool retired in new[] { false, true })
            {
                string copy = Manifest(temporary, $"Copy{retired}.rocksids.json", ("demo::Renamed", id, retired));
                ProcessResult rejected = await Run(options, environment, "verify-set", "--manifest", first, "--manifest", copy);
                Verify.Equal(1, rejected.ExitCode, $"verify-set accepted a shared ID (retired={retired})");
                Verify.Contains("error RMC2101", rejected.StdErr, "shared ID code");
                Verify.Contains(id, rejected.StdErr, "shared ID value");
                Verify.Contains("hint:", rejected.StdErr, "shared ID hint");
            }

            // 같은 이름을 다른 ID로 두 명세에 두는 것도 거부한다(한 이름은 한 명세에만 — 로컬 빌드와 CI 병합 검사가 같은 규칙).
            string sameName = temporary.FilePath("SameName.rocksids.json");
            Success(await Run(options, environment, "add", "--manifest", sameName, "--type", "demo::Actor"));
            ProcessResult name = await Run(options, environment, "verify-set", "--manifest", first, "--manifest", sameName);
            Verify.Equal(1, name.ExitCode, "verify-set accepted a name declared by two manifests");
            Verify.Contains("type name demo::Actor appears in both", name.StdErr, "shared name diagnostic");

            Error(await Run(options, environment, "verify-set"), "--manifest");
        });

        suite.Add("Meta.IdentityCliMergeCheckCombinesManifestsById", async () =>
        {
            using TemporaryDirectory temporary = new();
            string actor = Guid.NewGuid().ToString("N");
            string other = Guid.NewGuid().ToString("N");
            string baseLower = Manifest(temporary, "base/Lower.rocksids.json", ("demo::Actor", actor, false));
            // ours는 Actor를 Upper 명세로 옮기고, theirs는 Lower에 Other를 더했다. 합친 결과는 두 변경을 모두 담는다.
            string oursLower = Manifest(temporary, "ours/Lower.rocksids.json");
            string oursUpper = Manifest(temporary, "ours/Upper.rocksids.json", ("demo::Actor", actor, false));
            string theirsLower = Manifest(temporary, "theirs/Lower.rocksids.json", ("demo::Actor", actor, false), ("demo::Other", other, false));
            string mergedLower = Manifest(temporary, "merged/Lower.rocksids.json", ("demo::Other", other, false));
            string mergedUpper = Manifest(temporary, "merged/Upper.rocksids.json", ("demo::Actor", actor, false));
            ProcessResult accepted = await Run(options, environment, "merge-check", "--base", baseLower, "--ours", oursLower, "--ours", oursUpper,
                "--theirs", theirsLower, "--merged", mergedLower, "--merged", mergedUpper);
            Success(accepted);
            Verify.Contains("Stable identity merge: PASS", accepted.StdOut, "moved manifest merge");

            // 옮긴 쪽 명세를 합친 결과에서 빠뜨리면 ID가 사라진 것이다.
            ProcessResult lost = await Run(options, environment, "merge-check", "--base", baseLower, "--ours", oursLower, "--ours", oursUpper,
                "--theirs", theirsLower, "--merged", mergedLower);
            Verify.Equal(1, lost.ExitCode, "merge that lost a moved ID was accepted");
            Verify.Contains($"ID {actor}", lost.StdErr, "lost ID diagnostic");
            Verify.Contains("disappeared", lost.StdErr, "lost ID reason");

            // 한 측면의 두 명세에 같은 ID가 있으면(옮기지 않고 복사) 파일과 함께 보고하고 실패한다.
            string copied = Manifest(temporary, "merged/Copy.rocksids.json", ("demo::Copy", actor, false));
            ProcessResult duplicate = await Run(options, environment, "merge-check", "--base", baseLower, "--ours", oursLower, "--ours", oursUpper,
                "--theirs", theirsLower, "--merged", mergedLower, "--merged", mergedUpper, "--merged", copied);
            Verify.Equal(1, duplicate.ExitCode, "merge with a copied ID was accepted");
            Verify.Contains($"identity merge: merged: ID {actor}", duplicate.StdErr, "copied ID diagnostic names the side");
            Verify.Contains("Copy.rocksids.json", duplicate.StdErr, "copied ID diagnostic names the file");

            // 명세가 없던 리비전은 none으로 넘긴다. 각 측면은 적어도 한 번 지정해야 한다.
            Success(await Run(options, environment, "merge-check", "--base", "none", "--ours", oursUpper, "--theirs", "none", "--merged", oursUpper));
            Error(await Run(options, environment, "merge-check", "--base", baseLower, "--ours", oursUpper, "--theirs", theirsLower), "--merged");
        });
    }

    private static string Manifest(TemporaryDirectory temporary, string path, params (string Name, string Id, bool Retired)[] entries)
    {
        temporary.Write(path, JsonSerializer.Serialize(new
        {
            schema = 1,
            entries = entries.Select(entry => new { kind = "type", name = entry.Name, id = entry.Id, aliases = Array.Empty<string>(), retired = entry.Retired })
        }));
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
        Verify.Contains(message, result.StdErr, "identity command failure reason");
    }
}
