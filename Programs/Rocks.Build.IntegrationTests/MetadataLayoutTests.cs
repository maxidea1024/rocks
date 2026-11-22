using Rocks.Build.Cli;
using Rocks.Build.Rules;

namespace Rocks.Build.IntegrationTests;

// MetadataLayout.Apply의 모듈 소유 메타데이터 검증 시험이다(Docs/Design/ReflectionFoundations.ko.md 1.1–1.2·1.10절). 실제 빌드
// 없이 임시 모듈 폴더로 리플렉션 헤더 탐색(표지·범위·순서)·가림(RMC1001)·이중 소유·생성 철자 충돌(RMC2005)·잘못된 선언·고아 명세
// (RMC2102)·타깃 범위를 본다.
internal static class MetadataLayoutTests
{
    private const string ActiveManifest = """{"schema":1,"entries":[{"kind":"type","name":"a::Thing","id":"a1000000000000000000000000000001","aliases":[],"retired":false}]}""";

    private const string RetiredManifest = """{"schema":1,"entries":[{"kind":"type","name":"a::Gone","id":"a1000000000000000000000000000002","aliases":[],"retired":true}]}""";

    public static void Run()
    {
        DiscoveryFindsMarkedHeaders();
        ValidModulesGetGeneratedIncludeDirectories();
        ShadowingHeaderIsRejected();
        HeaderOwnedByTwoModulesIsRejected();
        GeneratedSpellingCollisionAndReservedNamesAreRejected();
        InvalidDeclarationsAreRejected();
        OrphanManifestRules();
        OnlyTargetReachableModulesAreChecked();
        IdePlanLayoutCreatesNoDirectories();
        Console.WriteLine("PASS metadata layout: 9 groups");
    }

    // IDE 계획(Docs/Design/ProjectFiles.ko.md 8.1절)은 같은 배치를 얻되 생성 include 디렉터리를 만들지 않는다.
    private static void IdePlanLayoutCreatesNoDirectories()
    {
        using var fixture = new Fixture();
        fixture.Reflected("A/Public/Shared/Thing.h");
        fixture.Manifest("A", ActiveManifest);
        MetadataPlan plan = MetadataLayout.Apply(fixture.Context, [fixture.Module("A")], ["A"], createDirectories: false);
        MetadataModule a = plan.Generated.Single();
        Require(!Directory.Exists(a.GeneratedPublic) && !Directory.Exists(fixture.Path("out")), "IDE layout created output directories");
        Require(plan.Modules.Single().PublicIncludeDirectories.Contains(a.GeneratedPublic), "IDE layout keeps the generated include directory");
    }

    // 표지 헤더만 찾는다(Docs/Design/ReflectionFoundations.ko.md 1.10절): 줄 첫머리 ROCKS_CLASS·ROCKS_STRUCT·ROCKS_ENUM(공백·탭 뒤 포함, 괄호
    // 앞 공백 허용, 자릿수 구분자 뒤 줄 포함), 하위 폴더 철자, 철자 순서. 주석(// 끝 역슬래시 이음 포함)·#define(이어진 줄 포함)·문자열
    // (역슬래시 이음·raw string 포함)·줄 중간의 철자, 표지 없는 헤더, .h가 아닌 파일(.cpp·대문자 .H), 모듈 폴더 밖 include 디렉터리(서드파티)는
    // 아니다. 모듈 폴더 안의 include 디렉터리 밖 표지 헤더(include가 아닌 Private/·모듈 루트·다른 폴더)는 거부하고(RMC2003), 모듈 시험
    // Tests/와 모듈 폴더 안 다른 모듈의 폴더는 훑지 않는다. ModuleDirectory가 없는 모듈의 표지 헤더는 RMC2100이다.
    private static void DiscoveryFindsMarkedHeaders()
    {
        using var fixture = new Fixture();
        fixture.Manifest("A", ActiveManifest);
        fixture.Write("A/Public/Zeta.h", "#pragma once\nROCKS_CLASS()\nstruct Zeta {};\n");
        fixture.Write("A/Public/Deep/Inner/Struct.h", "#pragma once\n\tROCKS_STRUCT ()\nstruct S {};\n");
        fixture.Write("A/Public/Alpha.h", "#pragma once\n  ROCKS_ENUM()\nenum class E { A };\n");
        fixture.Write("A/Private/Beta.h", "#pragma once\nROCKS_CLASS()\nstruct P {};\n");
        fixture.Write("A/Public/Digits.h", "#pragma once\nconstexpr int Big = 1'000'000;\nROCKS_CLASS()\nstruct D {};\n");
        foreach ((string file, string text) in new[]
        {
            ("A/Public/Plain.h", "#pragma once\nstruct Plain {};\n"),
            ("A/Public/LineComment.h", "#pragma once\n// ROCKS_CLASS()\nstruct C {};\n"),
            ("A/Public/BlockComment.h", "#pragma once\n/*\nROCKS_CLASS()\n*/\nstruct C {};\n"),
            ("A/Public/Define.h", "#pragma once\n#define MARK ROCKS_CLASS()\n"),
            ("A/Public/Continued.h", "#pragma once\n#define MARK \\\nROCKS_CLASS()\n"),
            ("A/Public/CommentContinued.h", "#pragma once\n// note \\\nROCKS_CLASS()\n"),
            ("A/Public/Raw.h", "#pragma once\nconst char* text = R\"x(\nROCKS_CLASS()\n)x\";\n"),
            ("A/Public/Upper.H", "#pragma once\nROCKS_CLASS()\nstruct U {};\n"),
            ("A/Public/Literal.h", "#pragma once\nconst char* text = \"\\\nROCKS_CLASS()\";\n"),
            ("A/Public/Middle.h", "#pragma once\nint x; ROCKS_CLASS()\n"),
            ("A/Public/Similar.h", "#pragma once\nROCKS_CLASSIC()\n"),
            ("A/Public/Source.cpp", "ROCKS_CLASS()\nstruct Cpp {};\n"),
            ("A/Tests/TestTypes.h", "#pragma once\nROCKS_CLASS()\nstruct T {};\n"),
            ("ThirdParty/Lib/Public/Lib.h", "#pragma once\nROCKS_CLASS()\nstruct L {};\n")
        })
        {
            fixture.Write(file, text);
        }

        NativeModuleInput module = fixture.Module("A") with
        {
            PublicIncludeDirectories = [fixture.Path("A/Public"), fixture.Path("ThirdParty/Lib/Public")]
        };
        MetadataModule found = MetadataLayout.Apply(fixture.Context, [module], ["A"]).Generated.Single();
        Require(found.Headers.Select(header => (header.Include, header.Private)).SequenceEqual(
            [("Alpha.h", false), ("Beta.h", true), ("Deep/Inner/Struct.h", false), ("Digits.h", false), ("Zeta.h", false)]),
            "discovered headers: " + string.Join(", ", found.Headers.Select(header => header.Include + (header.Private ? " (private)" : ""))));

        // include 디렉터리가 아닌 모듈 소스 폴더 Private/의 표지 헤더는 거부한다(표지 없는 헤더는 상관없다).
        NativeModuleInput sourceOnly = module with { PrivateIncludeDirectories = [] };
        Expect("RMC2003", () => MetadataLayout.Apply(fixture.Context, [sourceOnly], ["A"]));
        File.WriteAllText(fixture.Path("A/Private/Beta.h"), "#pragma once\nstruct P {};\n");
        Require(MetadataLayout.Apply(fixture.Context, [sourceOnly], ["A"]).Generated.Single().Headers.Count == 4,
            "an unmarked Private/ header was rejected or scanned");

        // 모듈 폴더의 다른 자리(모듈 루트·Internal/)에 둔 표지 헤더도 빌드가 찾지 않으므로 거부한다.
        foreach (string loose in new[] { "A/Loose.h", "A/Internal/Hidden.h" })
        {
            fixture.Reflected(loose);
            Expect("RMC2003", () => MetadataLayout.Apply(fixture.Context, [sourceOnly], ["A"]));
            File.Delete(fixture.Path(loose));
        }

        // 모듈 폴더 안의 다른 모듈 폴더는 그 모듈 몫이다(A가 거부하지 않는다).
        fixture.Reflected("A/Nested/Public/Nested.h");
        NativeModuleInput nested = fixture.Module("Nested") with { ModuleDirectory = "A/Nested", PublicIncludeDirectories = [fixture.Path("A/Nested/Public")] };
        Require(MetadataLayout.Apply(fixture.Context, [sourceOnly, nested], ["A"]).Generated.Single().Headers.Count == 4,
            "a nested module's header was claimed by the enclosing module");
        Directory.Delete(fixture.Path("A/Nested"), recursive: true);

        // ModuleDirectory가 없는 모듈의 표지 헤더는 명세 위치를 정할 수 없어 거부한다.
        Expect("RMC2100", () => MetadataLayout.Apply(fixture.Context, [module with { ModuleDirectory = null }], ["A"]));
        // 그러나 그 모듈의 include 디렉터리가 다른 모듈 폴더를 감싸면(시험 실행 파일이 상위 폴더를 include로 둔다) 그 안의 헤더는 그 모듈
        // 몫이라 거부하지 않는다(다른 모듈 폴더 안의 include 디렉터리를 직접 두어도 같다, 서드파티 폴더도 훑지 않는다).
        NativeModuleInput runner = new() { Name = "Runner", PrivateIncludeDirectories = [fixture.Root, fixture.Path("A/Public")], PublicDependencies = ["A"] };
        Require(MetadataLayout.Apply(fixture.Context, [sourceOnly, runner], ["Runner"]).Generated.Select(found => found.Name).SequenceEqual(["A"]),
            "a module without ModuleDirectory claimed headers of the modules under its include directory");

        // 엔진 계획처럼 modules를 타깃에서 닿는 모듈로 줄여 넘겨도(A가 빠짐) 전체 규칙 모듈(allModules)로 A의 폴더를 알아본다.
        NativeModuleInput alone = runner with { PublicDependencies = [] };
        Require(MetadataLayout.Apply(fixture.Context, [alone], ["Runner"], allModules: [sourceOnly, alone]).Generated.Count == 0,
            "a module without ModuleDirectory claimed headers of a module outside the reachable set");
        Expect("RMC2100", () => MetadataLayout.Apply(fixture.Context, [alone], ["Runner"]));
    }

    private static void ValidModulesGetGeneratedIncludeDirectories()
    {
        using var fixture = new Fixture();
        fixture.Reflected("A/Public/Shared/Thing.h");
        fixture.Reflected("A/Private/Hidden.h");
        fixture.Manifest("A", ActiveManifest);
        // 의존 이름은 ActionGraph처럼 대소문자를 무시한다.
        var modules = new[] { fixture.Module("A"), fixture.Module("B", dependencies: ["a"]) };
        MetadataPlan plan = MetadataLayout.Apply(fixture.Context, modules, ["B"]);
        MetadataModule a = plan.Generated.Single();
        Require(a.Name == "A" && a.Headers.Count == 2 && a.Headers.Single(header => header.Include == "Hidden.h").Private, "metadata module plan");
        Require(plan.Modules.Single(module => module.Name == "A").Metadata.SequenceEqual(["Hidden.h", "Shared/Thing.h"]),
            "the discovered headers were not recorded in spelling order");
        NativeModuleInput updated = plan.Modules.Single(module => module.Name == "A");
        Require(updated.PublicIncludeDirectories.Contains(a.GeneratedPublic) && updated.PrivateIncludeDirectories.Contains(a.GeneratedPrivate),
            "generated include directories were not added");
        Require(Directory.Exists(a.GeneratedPublic) && a.Descriptor.EndsWith(Path.Combine("Metadata", "A.rocksmeta.json"), StringComparison.Ordinal),
            "generated directory or descriptor path");
        // 생성 소스는 <out>/Generated/<모듈>/Source 아래다: 리플렉션 헤더마다 타입별 생성 소스(public 헤더는 Public/<철자>.generated.cpp,
        // private 헤더는 Private/…)와 모듈 생성 소스 <모듈>.module.generated.cpp. 손으로 쓴 Sources가 아니라 리플렉션 생성 소스 목록에
        // 든다(진입점의 자기 타입 표지를 정한다). Source는 include 루트가 아니다. Metadata가 없는 모듈은 생성 소스가 없다.
        string moduleSource = fixture.Path("out/Generated/A/Source/A.module.generated.cpp");
        string[] typeSources = [fixture.Path("out/Generated/A/Source/Private/Hidden.generated.cpp"), fixture.Path("out/Generated/A/Source/Public/Shared/Thing.generated.cpp")];
        Require(a.GeneratedSource == fixture.Path("out/Generated/A/Source") && a.ModuleSource == moduleSource &&
            a.Headers.Select(a.TypeSource).SequenceEqual(typeSources) &&
            updated.ReflectionSources.SequenceEqual([.. typeSources, moduleSource]) && updated.Sources.Count == 0,
            "the generated sources were not added to the reflection source list: " + string.Join(", ", updated.ReflectionSources));
        // 생성 소스의 컴파일이 실패하면 붙일 RMC3000 안내문을 모듈에 싣는다(재실행 옵션 이름 포함).
        Require(updated.ReflectionSourceFailureNote.Contains("RMC3000", StringComparison.Ordinal) &&
            updated.ReflectionSourceFailureNote.Contains("module 'A'", StringComparison.Ordinal) &&
            updated.ReflectionSourceFailureNote.Contains("--" + MetadataLayout.VerifySourcesOption + " on", StringComparison.Ordinal) &&
            plan.Modules.Single(module => module.Name == "B").ReflectionSourceFailureNote.Length == 0,
            "the reflection source failure note was not set: " + updated.ReflectionSourceFailureNote);
        Require(!updated.PublicIncludeDirectories.Concat(updated.PrivateIncludeDirectories).Contains(a.GeneratedSource),
            "the generated source directory must not be an include root");
        Require(plan.Modules.Single(module => module.Name == "B").ReflectionSources.Count == 0, "a module without Metadata got a reflection source");
        // 모듈 표 제공 모듈에 닿지 않으면 진입점 모듈이 아니므로 진입점 TU가 없다(빌드가 ROCKS_MODULE_NAME을 정의하지 않는다).
        Require(updated.GeneratedSources.Count == 0, "a metadata module outside the module table got an entry point source");
        // 모듈 표 제공 모듈에 닿고 손으로 쓴 소스가 없는 메타데이터 모듈은 진입점 TU를 받는다(Docs/Design/ModuleLoader.ko.md 3.10절).
        // 손으로 쓴 소스가 있는 메타데이터 모듈은 자기 소스에 진입점을 둔다.
        NativeModuleInput provider = new() { Name = "Core", ProvidesModuleTable = true };
        NativeModuleInput withSources = fixture.Module("A", dependencies: ["Core"]) with { Sources = [fixture.Path("A/a.cpp")] };
        MetadataPlan entryPlan = MetadataLayout.Apply(fixture.Context, [fixture.Module("A", dependencies: ["Core"]), provider], ["A"]);
        NativeGeneratedSource entry = entryPlan.Modules.Single(module => module.Name == "A").GeneratedSources.Single();
        Require(entry.FileName == "A" + MetadataLayout.ModuleEntrySuffix &&
            entry.Content.Contains("ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, A)", StringComparison.Ordinal),
            "a metadata module without hand-written sources did not get its entry point source: " + entry.FileName);
        Require(MetadataLayout.Apply(fixture.Context, [withSources, provider], ["A"]).Modules.Single(module => module.Name == "A").GeneratedSources.Count == 0,
            "a metadata module with hand-written sources got a generated entry point");
        Expect("unknown module 'Missing'", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("C", dependencies: ["Missing"])], ["C"]));
    }

    private static void ShadowingHeaderIsRejected()
    {
        using var fixture = new Fixture();
        fixture.Reflected("A/Public/Shared/Thing.h");
        fixture.Header("B/Public/Shared/Thing.h");
        fixture.Manifest("A", ActiveManifest);
        // B는 A에 의존하지만 자기 public 디렉터리의 같은 철자 헤더(표지 없음)가 먼저 풀린다 — 생성 헤더가 다른 원본을 include한다.
        var modules = new[] { fixture.Module("A"), fixture.Module("B", dependencies: ["A"]) };
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
        fixture.Reflected("A/Public/Shared/Thing.h");
        fixture.Manifest("A", ActiveManifest);
        fixture.Manifest("B", ActiveManifest.Replace("a1000000000000000000000000000001", "b1000000000000000000000000000001", StringComparison.Ordinal));
        // B의 모듈 폴더가 A를 감싸고 A의 public 디렉터리를 자기 include로 써서 같은 물리 헤더를 다시 찾는다.
        var modules = new[]
        {
            fixture.Module("A"),
            fixture.Module("B") with { PublicIncludeDirectories = [fixture.Path("A/Public")], ModuleDirectory = "." }
        };
        Directory.CreateDirectory(fixture.Path("B"));
        File.Copy(fixture.Path("B/B.rocksids.json"), fixture.Path("B.rocksids.json"));
        Expect("duplicate input ownership", () => MetadataLayout.Apply(fixture.Context, modules, ["A", "B"]));
    }

    private static void GeneratedSpellingCollisionAndReservedNamesAreRejected()
    {
        using var fixture = new Fixture();
        fixture.Reflected("A/Public/Foo/X.h");
        fixture.Reflected("B/Public/foo/x.h");
        fixture.Manifest("A", ActiveManifest);
        fixture.Manifest("B", ActiveManifest.Replace("a1000000000000000000000000000001", "b1000000000000000000000000000001", StringComparison.Ordinal));
        Expect("RMC2005", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A"), fixture.Module("B")], ["A", "B"]));
        // 생성 헤더 접미사(.module.h·.generated.h)를 쓴 표지 헤더도 거부한다.
        foreach (string reserved in new[] { "A/Public/Foo/Y.module.h", "A/Public/Foo/Z.generated.h" })
        {
            fixture.Reflected(reserved);
            Expect("RMC2005", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A")], ["A"]));
            File.Delete(fixture.Path(reserved));
        }
    }

    private static void InvalidDeclarationsAreRejected()
    {
        using var fixture = new Fixture();
        fixture.Reflected("A/Public/Shared/Thing.h");
        Expect("RMC2100", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A")], ["A"]));
        fixture.Manifest("A", ActiveManifest);
        // 모듈 규칙이 목록을 쓰면 거부한다(빌드가 찾는다).
        Expect("module rules do not declare Metadata", () => MetadataLayout.Apply(fixture.Context,
            [fixture.Module("A") with { Metadata = ["Shared/Thing.h"] }], ["A"]));
        // ModuleDirectory가 없는 모듈의 표지 헤더는 명세 위치를 정할 수 없어 거부하고(RMC2100), 표지 헤더가 없으면 그대로 둔다. SDK
        // 모듈(ImportedMetadata)은 찾지 않는다.
        Expect("RMC2100", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A") with { ModuleDirectory = null }], ["A"]));
        Require(MetadataLayout.Apply(fixture.Context, [fixture.Module("Plain") with { ModuleDirectory = null }], ["Plain"]).Generated.Count == 0,
            "a module without ModuleDirectory and markup was planned");
        Require(MetadataLayout.Apply(fixture.Context,
            [fixture.Module("A") with { ImportedMetadata = new ImportedMetadata("x.rocksmeta.json", "x.rocksids.json") }], ["A"]).Generated.Count == 0,
            "an SDK module was scanned");
        Expect("ModuleDirectory must be inside", () => MetadataLayout.Apply(fixture.Context,
            [fixture.Module("A") with { ModuleDirectory = ".." }], ["A"]));
    }

    private static void OrphanManifestRules()
    {
        using var fixture = new Fixture();
        // 리플렉션 헤더가 없는 모듈의 명세는 활성 항목이 있으면 RMC2102, 은퇴 항목만 있으면 ID 예약으로 허용하고 집합 검증에 넣는다.
        fixture.Manifest("A", ActiveManifest);
        Expect("RMC2102", () => MetadataLayout.Apply(fixture.Context, [fixture.Module("A")], ["A"]));
        fixture.Manifest("A", RetiredManifest);
        MetadataPlan plan = MetadataLayout.Apply(fixture.Context, [fixture.Module("A")], ["A"]);
        Require(plan.Generated.Count == 0 && plan.ReservedManifests.SequenceEqual([fixture.Path("A/A.rocksids.json")]), "retired-only manifest reservation");
    }

    private static void OnlyTargetReachableModulesAreChecked()
    {
        using var fixture = new Fixture();
        fixture.Reflected("A/Public/Shared/Thing.h");
        fixture.Reflected("B/Public/Other.h");
        fixture.Manifest("A", ActiveManifest);
        // B는 명세가 없어 잘못된 메타데이터 모듈이지만 타깃(A)에서 도달하지 않으므로 검사하지도, 배치하지도 않는다.
        var modules = new[] { fixture.Module("A"), fixture.Module("B") };
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

        // 표지 없는 헤더다(리플렉션 헤더가 아니다).
        public void Header(string relative) => Write(relative, "#pragma once\n");

        // 리플렉션 표지가 있는 헤더다(빌드가 찾는다).
        public void Reflected(string relative) => Write(relative, "#pragma once\nROCKS_CLASS()\nstruct Thing {};\n");

        public void Write(string relative, string text)
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path(relative))!);
            File.WriteAllText(Path(relative), text);
        }

        public void Manifest(string module, string text)
        {
            Directory.CreateDirectory(Path(module));
            File.WriteAllText(Path($"{module}/{module}.rocksids.json"), text);
        }

        public NativeModuleInput Module(string name, string[]? dependencies = null) => new()
        {
            Name = name,
            PublicIncludeDirectories = [Path($"{name}/Public")],
            PrivateIncludeDirectories = [Path($"{name}/Private")],
            PublicDependencies = dependencies ?? [],
            ModuleDirectory = name
        };

        public void Dispose() => Directory.Delete(Root, recursive: true);
    }
}
