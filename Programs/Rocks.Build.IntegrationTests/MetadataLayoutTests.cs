using Rocks.Build.Cli;
using Rocks.Build.Rules;

namespace Rocks.Build.IntegrationTests;

// MetadataLayout.Apply의 모듈 소유 메타데이터 검증 시험이다(Docs/Design/ReflectionFoundations.ko.md 1.1–1.2절). 실제 빌드
// 없이 임시 모듈 폴더로 가림(RMC1001)·이중 소유·생성 철자 충돌(RMC2005)·잘못된 선언·고아 명세(RMC2102)·타깃 범위를 본다.
internal static class MetadataLayoutTests
{
    private const string ActiveManifest = """{"schema":1,"entries":[{"kind":"type","name":"a::Thing","id":"a1000000000000000000000000000001","aliases":[],"retired":false}]}""";

    private const string RetiredManifest = """{"schema":1,"entries":[{"kind":"type","name":"a::Gone","id":"a1000000000000000000000000000002","aliases":[],"retired":true}]}""";

    public static void Run()
    {
        ValidModulesGetGeneratedIncludeDirectories();
        ShadowingHeaderIsRejected();
        HeaderOwnedByTwoModulesIsRejected();
        GeneratedSpellingCollisionAndReservedNamesAreRejected();
        InvalidDeclarationsAreRejected();
        OrphanManifestRules();
        OnlyTargetReachableModulesAreChecked();
        Console.WriteLine("PASS metadata layout: 7 groups");
    }

    private static void ValidModulesGetGeneratedIncludeDirectories()
    {
        using var fixture = new Fixture();
        fixture.Header("A/Public/Shared/Thing.h");
        fixture.Header("A/Private/Hidden.h");
        fixture.Manifest("A", ActiveManifest);
        // 의존 이름은 ActionGraph처럼 대소문자를 무시한다.
        var modules = new[] { fixture.Module("A", ["Shared/Thing.h", "Hidden.h"]), fixture.Module("B", [], dependencies: ["a"]) };
        MetadataPlan plan = MetadataLayout.Apply(fixture.Context, modules, ["B"]);
        MetadataModule a = plan.Generated.Single();
        Require(a.Name == "A" && a.Headers.Count == 2 && a.Headers.Single(header => header.Include == "Hidden.h").Private, "metadata module plan");
        NativeModuleInput updated = plan.Modules.Single(module => module.Name == "A");
        Require(updated.PublicIncludeDirectories.Contains(a.GeneratedPublic) && updated.PrivateIncludeDirectories.Contains(a.GeneratedPrivate),
            "generated include directories were not added");
        Require(Directory.Exists(a.GeneratedPublic) && a.Descriptor.EndsWith(Path.Combine("Metadata", "A.rocksmeta.json"), StringComparison.Ordinal),
            "generated directory or descriptor path");
        Expect("unknown module 'Missing'", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("C", [], dependencies: ["Missing"])], ["C"]));
    }

    private static void ShadowingHeaderIsRejected()
    {
        using var fixture = new Fixture();
        fixture.Header("A/Public/Shared/Thing.h");
        fixture.Header("B/Public/Shared/Thing.h");
        fixture.Manifest("A", ActiveManifest);
        // B는 A에 의존하지만 자기 public 디렉터리의 같은 철자 헤더가 먼저 풀린다 — 생성 헤더가 다른 원본을 include한다.
        var modules = new[] { fixture.Module("A", ["Shared/Thing.h"]), fixture.Module("B", [], dependencies: ["A"]) };
        Expect("RMC1001", () => MetadataLayout.Apply(fixture.Context, modules, ["B"]));
        File.Delete(fixture.Path("B/Public/Shared/Thing.h"));
        MetadataLayout.Apply(fixture.Context, modules, ["B"]);

        // 작성자 디렉터리의 낡은 생성 철자(.generated.h·집계 헤더)가 생성 루트보다 먼저 풀려도 거부한다.
        foreach (string stale in new[] { "A/Public/Shared/Thing.generated.h", "A/Public/A/A.module.generated.h" })
        {
            fixture.Header(stale);
            Expect("RMC1001", () => MetadataLayout.Apply(fixture.Context, modules, ["B"]));
            File.Delete(fixture.Path(stale));
        }

        MetadataLayout.Apply(fixture.Context, modules, ["B"]);
    }

    private static void HeaderOwnedByTwoModulesIsRejected()
    {
        using var fixture = new Fixture();
        fixture.Header("A/Public/Shared/Thing.h");
        fixture.Manifest("A", ActiveManifest);
        fixture.Manifest("B", ActiveManifest.Replace("a1000000000000000000000000000001", "b1000000000000000000000000000001", StringComparison.Ordinal));
        // B가 A의 public 디렉터리를 자기 include로 써서 같은 물리 헤더를 다시 선언한다.
        var modules = new[]
        {
            fixture.Module("A", ["Shared/Thing.h"]),
            fixture.Module("B", ["Shared/Thing.h"]) with { PublicIncludeDirectories = [fixture.Path("A/Public")] }
        };
        Expect("duplicate input ownership", () => MetadataLayout.Apply(fixture.Context, modules, ["A", "B"]));
        Expect("duplicate input ownership", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", ["Shared/Thing.h", "Shared/Thing.h"])], ["A"]));
    }

    private static void GeneratedSpellingCollisionAndReservedNamesAreRejected()
    {
        using var fixture = new Fixture();
        fixture.Header("A/Public/Foo/X.h");
        fixture.Header("B/Public/foo/x.h");
        fixture.Header("A/Public/Foo/Y.module.h");
        fixture.Header("A/Public/Foo/Z.generated.h");
        fixture.Manifest("A", ActiveManifest);
        fixture.Manifest("B", ActiveManifest.Replace("a1000000000000000000000000000001", "b1000000000000000000000000000001", StringComparison.Ordinal));
        Expect("RMC2005", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", ["Foo/X.h"]), fixture.Module("B", ["foo/x.h"])], ["A", "B"]));
        Expect("RMC2005", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", ["Foo/Y.module.h"])], ["A"]));
        Expect("RMC2005", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", ["Foo/Z.generated.h"])], ["A"]));
    }

    private static void InvalidDeclarationsAreRejected()
    {
        using var fixture = new Fixture();
        fixture.Header("A/Public/Shared/Thing.h");
        Expect("RMC2100", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", ["Shared/Thing.h"])], ["A"]));
        fixture.Manifest("A", ActiveManifest);
        Expect("requires ModuleDirectory", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", ["Shared/Thing.h"]) with { ModuleDirectory = null }], ["A"]));
        Expect("cannot be combined", () => MetadataLayout.Apply(fixture.Context,
            [fixture.Module("A", ["Shared/Thing.h"]) with { ImportedMetadata = new ImportedMetadata("x.rocksmeta.json", "x.rocksids.json") }], ["A"]));
        Expect("header not found", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", ["Shared/Missing.h"])], ["A"]));
        Expect("relative forward-slash", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", ["../Thing.h"])], ["A"]));
        Expect("ModuleDirectory must be inside", () => MetadataLayout.Apply(fixture.Context,
            [fixture.Module("A", ["Shared/Thing.h"]) with { ModuleDirectory = ".." }], ["A"]));
    }

    private static void OrphanManifestRules()
    {
        using var fixture = new Fixture();
        // Metadata가 없는 모듈의 명세는 활성 항목이 있으면 RMC2102, 은퇴 항목만 있으면 ID 예약으로 허용하고 집합 검증에 넣는다.
        fixture.Manifest("A", ActiveManifest);
        Expect("RMC2102", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A", [])], ["A"]));
        fixture.Manifest("A", RetiredManifest);
        MetadataPlan plan = MetadataLayout.Apply(fixture.Context, [fixture.Module("A", [])], ["A"]);
        Require(plan.Generated.Count == 0 && plan.ReservedManifests.SequenceEqual([fixture.Path("A/A.rocksids.json")]), "retired-only manifest reservation");
    }

    private static void OnlyTargetReachableModulesAreChecked()
    {
        using var fixture = new Fixture();
        fixture.Header("A/Public/Shared/Thing.h");
        fixture.Header("B/Public/Other.h");
        fixture.Manifest("A", ActiveManifest);
        // B는 명세가 없어 잘못된 메타데이터 모듈이지만 타깃(A)에서 도달하지 않으므로 검사하지도, 배치하지도 않는다.
        var modules = new[] { fixture.Module("A", ["Shared/Thing.h"]), fixture.Module("B", ["Other.h"]) };
        MetadataPlan plan = MetadataLayout.Apply(fixture.Context, modules, ["A"]);
        Require(plan.Generated.Select(module => module.Name).SequenceEqual(["A"]), "unreachable module was planned");
        Require(plan.Modules.Single(module => module.Name == "B").PublicIncludeDirectories.Count == 1, "unreachable module got generated includes");
        Expect("RMC2100", () => MetadataLayout.Apply(fixture.Context, modules, ["A", "B"]));
    }

    private static void Expect(string message, Action action)
    {
        try
        {
            action();
        }
        catch (ArgumentException error) when (error.Message.Contains(message, StringComparison.Ordinal))
        {
            return;
        }

        throw new InvalidOperationException("Expected metadata layout rejection: " + message);
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private sealed class Fixture : IDisposable
    {
        public Fixture()
        {
            Root = Directory.CreateTempSubdirectory("rocks-meta-layout-").FullName;
            Context = new BuildContext(Root, System.IO.Path.Combine(Root, "out"), NativePlatform.Windows, NativeArchitecture.X64, NativeConfiguration.Debug);
        }

        public string Root { get; }

        public BuildContext Context { get; }

        public string Path(string relative) => System.IO.Path.GetFullPath(System.IO.Path.Combine(Root, relative));

        public void Header(string relative)
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path(relative))!);
            File.WriteAllText(Path(relative), "#pragma once\n");
        }

        public void Manifest(string module, string text)
        {
            Directory.CreateDirectory(Path(module));
            File.WriteAllText(Path($"{module}/{module}.rocksids.json"), text);
        }

        public NativeModuleInput Module(string name, string[] metadata, string[]? dependencies = null) => new()
        {
            Name = name,
            PublicIncludeDirectories = [Path($"{name}/Public")],
            PrivateIncludeDirectories = [Path($"{name}/Private")],
            Dependencies = dependencies ?? [],
            Metadata = metadata,
            ModuleDirectory = name
        };

        public void Dispose() => Directory.Delete(Root, recursive: true);
    }
}
