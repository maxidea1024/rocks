using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 모듈 모드 시험 중 모듈 생성·import·가시성·도구 이름과 모듈 descriptor 시험이다.
internal static partial class ModuleMetaTests
{
    // 모듈 생성·집계·import 오류·철자 충돌·포함 순서·상속 형질·타입 도구 이름·에셋 값 종류 왕복 시험이다.
    private static void RegisterModules(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        suite.Add("Meta.ModuleNameMayEqualAReflectedClassName", async () =>
        {
            // 집계 함수 이름공간 rocks::generated::Lower와 클래스 rocks::Lower가 서로 가리지 않는다(생성 코드는 이름을 ::rocks::로 한정한다 —
            // 검증 컴파일이 생성 헤더와 집계 헤더를 함께 컴파일한다). World 모듈과 rocks::World의 관계와 같다.
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", SameNameParent);
            layout.Manifest("Lower", ["rocks::Lower"], ["rocks::Lower::value"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "module named like its reflected class: " + lower.StdErr);
            string aggregate = File.ReadAllText(layout.Generated("Lower", "Public/Lower/Lower.module.generated.h"));
            Verify.Contains("namespace rocks::generated::Lower", aggregate, "aggregate namespace");
            Verify.Contains("GeneratedType<::rocks::Lower>", aggregate, "aggregate registers the same-named class");
            string generated = File.ReadAllText(layout.Generated("Lower", "Public/Lower/Parent.generated.h"));
            Verify.Contains("struct GeneratedType<::rocks::Lower>", generated, "generated specialization is fully qualified");
        });

        suite.Add("Meta.ModuleGeneratesHeadersAggregatesAndDescriptor", async () =>
        {
            using Layout layout = new(options, environment);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            Verify.That(File.Exists(layout.Generated("Lower", "Public/Lower/Parent.generated.h")), "lower generated header");
            string lowerAggregate = File.ReadAllText(layout.Generated("Lower", "Public/Lower/Lower.module.generated.h"));
            Verify.Contains("namespace rocks::generated::Lower", lowerAggregate, "lower aggregate namespace");
            Verify.Contains("GeneratedType<::probe::Parent>", lowerAggregate, "lower aggregate registers its type");
            Verify.That(!Directory.Exists(layout.Generated("Lower", "Private/Lower")), "public-only module emitted a private aggregate");
            Verify.That(!File.ReadAllBytes(layout.Descriptor("Lower")).Contains((byte)'\r'), "descriptor must use LF on every platform");

            ProcessResult upper = await layout.RunUpperAsync();
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            foreach (string relative in new[] { "Public/Upper/Value.generated.h", "Public/Upper/Leaf.generated.h", "Private/Upper/Detail.generated.h" })
            {
                Verify.That(File.Exists(layout.Generated("Upper", relative)), "missing generated header " + relative);
            }

            // public 집계는 import한 모듈을 먼저 등록하고, 이 모듈 타입을 base 먼저(헤더를 건너) 등록한다.
            string aggregate = File.ReadAllText(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h"));
            Verify.Contains("#include <Lower/Lower.module.generated.h>", aggregate, "dependency aggregate include");
            Verify.Contains("registration.EnterModule(&moduleKey)", aggregate, "aggregate registers each module once per registration");
            int dependency = aggregate.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int value = aggregate.IndexOf("GeneratedType<::probe::Value>::Schema()", StringComparison.Ordinal);
            int leaf = aggregate.IndexOf("GeneratedType<::probe::Leaf>::Schema()", StringComparison.Ordinal);
            Verify.That(dependency >= 0 && value > dependency && leaf > value, "aggregate registration order:\n" + aggregate);
            Verify.That(!aggregate.Contains("probe::Detail", StringComparison.Ordinal), "public aggregate registers a private type");
            string privateAggregate = File.ReadAllText(layout.Generated("Upper", "Private/Upper/Upper.private.module.generated.h"));
            Verify.Contains("#include <Upper/Upper.module.generated.h>", privateAggregate, "private aggregate includes public aggregate");
            Verify.Contains("RegisterPrivateModuleTypes", privateAggregate, "private aggregate function");
            Verify.Contains("GeneratedType<::probe::Detail>", privateAggregate, "private aggregate registers private type");

            // descriptor는 상속 필드를 펼치고, import한 타입을 가리키는 참조의 대상 ID를 담는다.
            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement parent = Type(lowerDescriptor, "probe::Parent");
            Verify.Equal("Lower/Parent.h", parent.GetProperty("header").GetString(), "descriptor header spelling");
            JsonElement valueType = Type(upperDescriptor, "probe::Value");
            Verify.Equal("probe::Parent", valueType.GetProperty("base").GetString(), "imported base");
            // 필드는 상속 포함 안정 ID 순이다(위치가 아니라 이름으로 찾는다).
            JsonElement[] fields = valueType.GetProperty("fields").EnumerateArray().ToArray();
            Verify.Equal(2, fields.Length, "inherited and own fields");
            JsonElement inherited = fields.Single(field => field.GetProperty("name").GetString() == "value");
            JsonElement next = fields.Single(field => field.GetProperty("name").GetString() == "next");
            Verify.Equal("probe::Parent", inherited.GetProperty("owner").GetString(), "inherited field owner");
            Verify.Equal(parent.GetProperty("fields")[0].GetProperty("id").GetString(), inherited.GetProperty("id").GetString(), "inherited field ID");
            Verify.Equal(parent.GetProperty("id").GetString(), next.GetProperty("value").GetProperty("type").GetString(), "imported reference target ID");
            Verify.Equal("private", Type(upperDescriptor, "probe::Detail").GetProperty("visibility").GetString(), "private visibility");

            // --check는 모든 생성물과 descriptor를 비교하고, Metadata에서 빠진 헤더의 낡은 생성물도 찾는다. 단독 실행은
            // 남은 생성물을 지우지 않는다(지우기는 빌드의 게시 트랜잭션이 맡는다).
            ProcessResult check = await layout.RunUpperAsync(extra: ["--check"]);
            Verify.Equal(0, check.ExitCode, "up-to-date check: " + check.StdErr);
            ProcessResult withoutLeaf = await layout.RunUpperAsync(withLeaf: false);
            Verify.Equal(0, withoutLeaf.ExitCode, "upper module without Leaf: " + withoutLeaf.StdErr);
            ProcessResult stale = await layout.RunUpperAsync(withLeaf: false, extra: ["--check"]);
            Verify.That(stale.ExitCode != 0, "check accepted the generated header of a removed input");
            Verify.Contains("RMC4000", stale.StdErr, "stale extra code");
            Verify.Contains("Leaf.generated.h", stale.StdErr, "stale extra path");
        });

        suite.Add("Meta.ModuleImportAndVisibilityErrors", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");

            // 의존 모듈의 리플렉션 선언은 그 모듈 descriptor를 import해야 쓸 수 있다.
            await Reject(layout.RunUpperAsync(imports: []), "RMC2003", "imported descriptor");

            // 같은 모듈을 두 번, 또는 같은 타입을 선언한 두 모듈을 import하면 거부한다.
            await Reject(layout.RunUpperAsync(imports: [layout.Descriptor("Lower"), layout.Descriptor("Lower")]), "RMC2004", "imported twice");
            string other = layout.Descriptor("Other");
            File.WriteAllText(other, File.ReadAllText(layout.Descriptor("Lower")).Replace("\"module\": \"Lower\"", "\"module\": \"Other\"", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(imports: [layout.Descriptor("Lower"), other]), "RMC2004", "two imported modules");

            // public 타입은 private 타입을 base로 둘 수 없고, 다른 모듈의 private 타입은 쓸 수 없다.
            await Reject(layout.RunUpperAsync(privateValue: true), "RMC2006", "derives from private type");
            Verify.Equal(0, (await layout.RunLowerAsync(privateParent: true)).ExitCode, "private lower module");
            await Reject(layout.RunUpperAsync(), "RMC2006", "private reflected type");
        });

        suite.Add("Meta.ModuleSpellingCollisionsAreRejected", async () =>
        {
            using Layout layout = new(options, environment);
            await Reject(layout.RunLowerAsync(extraInputs: [("Lower/Parent.h", "lower/parent.h")]), "RMC2005", "case-only");
            layout.Write("Lower/Public/Lower/Types.module.h", Parent);
            await Reject(layout.RunLowerAsync(extraInputs: [("Lower/Types.module.h", "Lower/Types.module.h")]), "RMC2005", "reserved");
            layout.Write("Lower/Public/Lower/Types.generated.h", Parent);
            await Reject(layout.RunLowerAsync(extraInputs: [("Lower/Types.generated.h", "Lower/Types.generated.h")]), "RMC2005", "reserved");

            // 출력 루트의 파일은 모두 생성 헤더다. descriptor·inventory를 그 안에 두면 --check가 잉여 파일로 보므로 거부한다.
            await Reject(layout.RunLowerAsync(descriptor: layout.Generated("Lower", "Lower.rocksmeta.json")), "RMC1000", "outside the output root");
            await Reject(layout.RunLowerAsync(extra: ["--identity-inventory", layout.Generated("Lower", "inventory.json")]), "RMC1000", "outside the output root");
        });

        // 집계 헤더는 이름을 ::rocks::로 한정하므로 rocks 타입과 같은 이름(Result)의 모듈도 그 모듈과 의존 모듈이 컴파일된다.
        suite.Add("Meta.ModuleNamedLikeRocksTypesCompiles", async () =>
        {
            using Layout layout = new(options, environment);
            Directory.CreateDirectory(layout.Generated("Result", "Public"));
            Directory.CreateDirectory(layout.Generated("Result", "Private"));
            ProcessResult lower = await layout.RunLowerAsync(module: "Result");
            Verify.Equal(0, lower.ExitCode, "module named Result: " + lower.StdErr);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)],
                imports: [layout.Descriptor("Result")], dependencyModule: "Result");
            Verify.Equal(0, upper.ExitCode, "module importing Result: " + upper.StdErr);
            Verify.Contains("::rocks::generated::Result::RegisterModuleTypes", File.ReadAllText(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")),
                "dependency call");
        });

        // 생성물 검증 컴파일은 소비자와 같은 include 순서를 쓴다. 작성자 디렉터리의 낡은 .generated.h가 생성 헤더보다 먼저
        // 풀리면 소비자도 그 파일을 보므로 검증이 실패해야 한다(검증 루트를 맨 앞에 두면 통과해 버린다).
        suite.Add("Meta.ValidationUsesTheConsumerIncludeOrder", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Write("Upper/Public/Upper/Value.generated.h", "#error stale generated header in the author directory\n");
            await Reject(layout.RunUpperAsync(), "RMC3000", "stale generated header");
        });

        // import한 base의 WorldOnly와 도구 이름은 descriptor로 전해져 파생 모듈에 상속된다(B2).
        suite.Add("Meta.ImportedWorldOnlyAndToolNamesAreInherited", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("ROCKS_CLASS()", "ROCKS_CLASS(WorldOnly)", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY()", "ROCKS_PROPERTY(Name=\"Amount\", EditorOnly)", StringComparison.Ordinal));
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            ProcessResult upper = await layout.RunUpperAsync();
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            using JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement value = Type(descriptor, "probe::Value");
            Verify.Equal("WorldOnly", value.GetProperty("flags")[0].GetString(), "WorldOnly inherited from an imported base");
            JsonElement inherited = value.GetProperty("fields").EnumerateArray().Single(field => field.GetProperty("name").GetString() == "value");
            Verify.Equal("Amount", inherited.GetProperty("toolName").GetString(), "inherited tool name");
            Verify.Equal("EditorOnly", inherited.GetProperty("flags")[0].GetString(), "inherited field flags round-trip");

            // 상속한 도구 이름과 대소문자만 다른 이름은 RMC2203이다.
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_PROPERTY()", "ROCKS_PROPERTY(Name=\"amount\")", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(), "RMC2203", "tool name");
        });

        // 리플렉션 B9d2: 타입 도구 이름(기본은 한정하지 않은 C++ 이름)이 descriptor toolName으로 전해지고, 이 모듈 타입의 이름은 import한
        // 타입의 이름과도 대소문자 무시로 달라야 한다(RMC2203 — 같은 Registry 등록의 AlreadyExists를 먼저 잡는다).
        suite.Add("Meta.TypeToolNamesAreUniqueAcrossImports", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            using (JsonDocument lower = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower"))))
            {
                Verify.Equal("Parent", Type(lower, "probe::Parent").GetProperty("toolName").GetString(), "default type tool name in the descriptor");
            }

            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_CLASS()", "ROCKS_CLASS(Name = \"PARENT\")", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(), "RMC2203", "of module Lower");
        });

        // 리플렉션 B9d2: import한 두 모듈의 타입 도구 이름이 대소문자 무시로 겹쳐도 RMC2203이다(그 둘을 함께 쓰는 프로그램의 등록이
        // AlreadyExists가 되기 전에 — 정규 이름 중복의 RMC2004와 같은 자리). Upper가 Lower(probe::Parent — 기본 이름 Parent)와
        // 도구 이름을 PARENT로 바꾼 Elements(probe::Plain)를 함께 import한다.
        suite.Add("Meta.TypeToolNamesAreUniqueAmongImportedModules", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteArrayModules();
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Elements")).ExitCode, "elements module");
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Lower")).ExitCode, "lower module");
            Verify.Equal(0, (await layout.RunArrayModuleAsync("Upper")).ExitCode, "upper module before the collision");
            string mutated = layout.Generated("Mutated", "Elements.rocksmeta.json");
            Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
            File.WriteAllText(mutated, MutateType(File.ReadAllText(layout.Descriptor("Elements")), "probe::Plain",
                plain => plain["toolName"] = "PARENT"));
            await Reject(layout.RunArrayModuleAsync("Upper", [layout.Descriptor("Lower"), mutated]), "RMC2203", "(module Lower)");
            // descriptor의 toolName도 형식을 검사한다(빌드 산출물 위조 — RMC1000).
            File.WriteAllText(mutated, MutateType(File.ReadAllText(layout.Descriptor("Elements")), "probe::Plain",
                plain => plain["toolName"] = "bad name"));
            await Reject(layout.RunArrayModuleAsync("Upper", [layout.Descriptor("Lower"), mutated]), "RMC1000", "toolName of probe::Plain");
        });

        // B3a: AssetId·Sha256Digest·SoftObjectRef 필드가 descriptor에 담기고, import한 base의 소프트 참조 기대 타입이 파생
        // 모듈로 전해진다(Docs/Design/ReflectionFoundations.ko.md 3.1–3.3절).
        suite.Add("Meta.AssetValueKindsRoundTripThroughImportedDescriptors", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "#include <Object/AssetId.h>\n#include <Object/SoftObjectRef.h>\n#include <Core/Encoding/Sha256.h>\nnamespace probe {\n" +
                "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\n" +
                "explicit Parent(rocks::Allocator& allocator) noexcept : icon(allocator), any(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() rocks::AssetId source;\nROCKS_PROPERTY() rocks::Sha256Digest hash;\n" +
                "ROCKS_PROPERTY(Required) rocks::SoftObjectRef<Parent> icon;\nROCKS_PROPERTY() rocks::SoftObjectRef<> any;\n};\n}\n");
            layout.Manifest("Lower", [], ["probe::Parent::source", "probe::Parent::hash", "probe::Parent::icon", "probe::Parent::any"]);
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("struct Value : public Parent {\n",
                "struct Value : public Parent {\nexplicit Value(rocks::Allocator& allocator) noexcept : Parent(allocator), back(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::SoftObjectRef<Parent> back;\n", StringComparison.Ordinal));
            layout.Manifest("Upper", [], ["probe::Value::back"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            string parentId = Type(lowerDescriptor, "probe::Parent").GetProperty("id").GetString()!;
            JsonElement Field(JsonDocument descriptor, string type, string name) => Type(descriptor, type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            Verify.Equal("AssetId", Field(lowerDescriptor, "probe::Parent", "source").GetProperty("value").GetProperty("kind").GetString(), "AssetId kind");
            Verify.That(!Field(lowerDescriptor, "probe::Parent", "source").GetProperty("value").TryGetProperty("type", out _), "AssetId has no target type");
            Verify.Equal("Digest256", Field(lowerDescriptor, "probe::Parent", "hash").GetProperty("value").GetProperty("kind").GetString(), "digest kind");
            JsonElement icon = Field(lowerDescriptor, "probe::Parent", "icon");
            Verify.Equal("SoftObjectReference", icon.GetProperty("value").GetProperty("kind").GetString(), "soft reference kind");
            Verify.Equal(parentId, icon.GetProperty("value").GetProperty("type").GetString(), "soft reference target");
            Verify.Equal("Required", icon.GetProperty("flags")[0].GetString(), "Required on a soft reference");
            Verify.Equal("", Field(lowerDescriptor, "probe::Parent", "any").GetProperty("value").GetProperty("type").GetString(), "Object soft reference target");
            Verify.Equal("", Field(upperDescriptor, "probe::Value", "any").GetProperty("value").GetProperty("type").GetString(), "inherited Object soft reference target");

            // 파생 모듈의 자기 소프트 참조 필드도 import한 타입을 기대 타입으로 푼다.
            JsonElement own = Field(upperDescriptor, "probe::Value", "back");
            Verify.Equal("SoftObjectReference", own.GetProperty("value").GetProperty("kind").GetString(), "own soft reference kind");
            Verify.Equal(parentId, own.GetProperty("value").GetProperty("type").GetString(), "own soft reference to an imported type");

            // import한 base의 필드는 파생 descriptor와 생성 스키마에 같은 값 형식으로 펼쳐진다.
            JsonElement inherited = Field(upperDescriptor, "probe::Value", "icon");
            Verify.Equal("SoftObjectReference", inherited.GetProperty("value").GetProperty("kind").GetString(), "inherited soft reference kind");
            Verify.Equal(parentId, inherited.GetProperty("value").GetProperty("type").GetString(), "inherited soft reference target");
            Verify.Equal("Required", inherited.GetProperty("flags")[0].GetString(), "inherited Required flag");
            string generated = File.ReadAllText(layout.Generated("Upper", "Public/Upper/Value.generated.h"));
            Verify.Contains("ValueKind::SoftObjectReference", generated, "generated soft reference schema");
            Verify.Contains("ValueKind::Digest256", generated, "generated digest schema");
        });

        // 리플렉션 B9e: Optional 필드(단일 값·enum·값 struct 내부, struct 안 Optional<String> 멤버)가 descriptor로 전해지고, import한
        // base의 Optional 필드가 파생 descriptor와 생성 스키마·함수표(Optional<struct>는 경로 thunk, 단일 값은 read·write)에 같은
        // 형식으로 펼쳐져 컴파일된다(descriptor 읽기의 element·maxElements 규칙).
        suite.Add("Meta.OptionalFieldsRoundTripThroughImportedDescriptors", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "#include <Core/Optional.h>\n#include <Core/Text/String.h>\nnamespace probe {\n" +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { First = 1, Second = 2 };\n" +
                "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::Optional<rocks::String> note;\n};\n" +
                "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\n" +
                "ROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() rocks::Optional<rocks::int32> count;\n" +
                "ROCKS_PROPERTY() rocks::Optional<Mode> mode;\nROCKS_PROPERTY() rocks::Optional<Leaf> leaf;\n};\n}\n");
            layout.Manifest("Lower", ["probe::Mode", "probe::Leaf"], ["probe::Leaf::note", "probe::Parent::count", "probe::Parent::mode", "probe::Parent::leaf"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement Field(JsonDocument descriptor, string type, string name) => Type(descriptor, type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string leafId = Type(lowerDescriptor, "probe::Leaf").GetProperty("id").GetString()!;
            JsonElement inherited = Field(upperDescriptor, "probe::Value", "leaf");
            Verify.Equal("Optional", inherited.GetProperty("value").GetProperty("kind").GetString(), "inherited optional kind");
            Verify.Equal("Struct", inherited.GetProperty("element").GetProperty("kind").GetString(), "inherited optional inner kind");
            Verify.Equal(leafId, inherited.GetProperty("element").GetProperty("type").GetString(), "inherited optional inner target");
            Verify.Equal(0, inherited.GetProperty("maxElements").GetInt32(), "inherited optional maxElements");
            Verify.Equal("Enum", Field(upperDescriptor, "probe::Value", "mode").GetProperty("element").GetProperty("kind").GetString(), "inherited optional enum");
            Verify.Equal("Utf8String", Field(lowerDescriptor, "probe::Leaf", "note").GetProperty("element").GetProperty("kind").GetString(), "optional struct member");
            string generated = Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Value.generated.h")));
            Verify.Contains("{ValueKind::Optional,Guid{}}", generated, "generated inherited optional schema");
            Verify.Contains("PathClassField<::probe::Value,::rocks::ValueKind::Struct>(cursor,&::probe::Parent::leaf)", generated,
                "inherited Optional<struct> field path thunk");
            Verify.Contains("ReadGeneratedField<::probe::Value>(registry,access,object,&::probe::Parent::count,out)", generated,
                "inherited Optional<scalar> field read thunk");
        });
    }

    // private 전용 모듈 등록·모듈 재선언·자기 import와 모듈 descriptor 엄격 읽기 시험이다(enum 시험 뒤에 등록한다).
    private static void RegisterModuleDescriptors(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        // private 헤더만 있는 모듈도 private 집계가 import한 모듈을 먼저 등록한다(base가 등록되어 있어야 한다).
        suite.Add("Meta.PrivateOnlyModuleRegistersImportedModules", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            ProcessResult hidden = await layout.RunUpperInputsAsync([(layout.Source("Upper/Private/Upper/Hidden.h"), "Upper/Hidden.h", true)]);
            Verify.Equal(0, hidden.ExitCode, "private-only module: " + hidden.StdErr);
            Verify.That(!File.Exists(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")), "private-only module emitted a public aggregate");
            string aggregate = File.ReadAllText(layout.Generated("Upper", "Private/Upper/Upper.private.module.generated.h"));
            Verify.Contains("#include <Lower/Lower.module.generated.h>", aggregate, "private-only aggregate includes the dependency aggregate");
            int dependency = aggregate.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int own = aggregate.IndexOf("GeneratedType<::probe::Hidden>::Schema()", StringComparison.Ordinal);
            Verify.That(dependency >= 0 && own > dependency, "private-only aggregate must register the imported base first:\n" + aggregate);
        });

        // 모듈은 자기 descriptor를 import할 수 없고, import한 타입을 다시 정의하거나 같은 이름을 다른 파일에서 선언할 수 없다.
        suite.Add("Meta.ModuleRedeclarationAndSelfImportAreRejected", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            Verify.Equal(0, (await layout.RunUpperAsync()).ExitCode, "upper module");
            await Reject(layout.RunUpperAsync(imports: [layout.Descriptor("Lower"), layout.Descriptor("Upper")]), "RMC2004", "imports its own descriptor");

            // 입력 밖 파일의 같은 이름 선언(같은 이름·다른 물리 파일)은 RMC2003이다.
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Impostor.h"), "Upper/Impostor.h", false)]), "RMC2003", "same name and header");

            // 이 모듈 입력 헤더가 import한 이름을 다시 정의하면 RMC2004다.
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/FakeParent.h"), "Upper/FakeParent.h", false)]), "RMC2004", "declared by this module and by imported module");
        });

        // descriptor는 빌드 산출물이다. 모르는 속성, 잘못된 모듈 이름·헤더 철자·이름·버전은 생성 C++에 들어가기 전에 거부한다.
        suite.Add("Meta.ModuleDescriptorIsReadStrictly", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            foreach ((string name, string from, string to) in new[]
            {
                ("value", "\"kind\": \"Int32\"", "\"kind\": \"Int32\",\n            \"extra\": 1"),
                ("module", "\"module\": \"Lower\"", "\"module\": \"Bad-Name\""),
                ("header", "\"header\": \"Lower/Parent.h\"", "\"header\": \"../Parent.h\""),
                ("version", "\"version\": 1", "\"version\": 4294967296"),
                ("name", "\"name\": \"probe::Parent\"", "\"name\": \"probe::Parent; int x\""),
                ("newline", "\"name\": \"probe::Parent\"", "\"name\": \"probe::Parent\\n\""),
                ("element", "\"element\": null", "\"element\": 1"),
                ("maxElements", "\"maxElements\": 0", "\"maxElements\": 3"),
                ("toolName", "\"toolName\": \"value\"", "\"toolName\": \"1bad\""),
                ("duplicate", "\"module\": \"Lower\"", "\"module\": \"Lower\",\n  \"module\": \"Lower\"")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }

            // 값 종류는 MetaCompiler가 만드는 종류만 받고, 대상 타입 키와 Required는 종류와 맞아야 한다(B3a).
            const string Int32Kind = "\"kind\": \"Int32\"";
            const string ValueField = "\"fieldKind\": \"Value\"";
            int fieldFlags = original.LastIndexOf("\"flags\": []", StringComparison.Ordinal);
            Verify.That(fieldFlags > original.IndexOf(ValueField, StringComparison.Ordinal), "descriptor fixture changed: field flags");
            foreach ((string name, string mutatedText) in new[]
            {
                ("enumKind", original.Replace(Int32Kind, "\"kind\": \"Enum\"", StringComparison.Ordinal)),
                ("opaqueKind", original.Replace(Int32Kind, "\"kind\": \"Opaque\"", StringComparison.Ordinal)),
                ("unknownKind", original.Replace(Int32Kind, "\"kind\": \"Bogus\"", StringComparison.Ordinal)),
                ("softWithoutType", original.Replace(Int32Kind, "\"kind\": \"SoftObjectReference\"", StringComparison.Ordinal)),
                ("assetWithType", original.Replace(Int32Kind, "\"kind\": \"AssetId\",\n            \"type\": \"\"", StringComparison.Ordinal)),
                ("softAsStrongEdge", original.Replace(Int32Kind, "\"kind\": \"SoftObjectReference\",\n            \"type\": \"\"", StringComparison.Ordinal)
                    .Replace(ValueField, "\"fieldKind\": \"ObjectReference\"", StringComparison.Ordinal)),
                ("requiredScalar", original[..fieldFlags] + "\"flags\": [\"Required\"]" + original[(fieldFlags + "\"flags\": []".Length)..]),
                ("enumWithoutTarget", original.Replace(Int32Kind, "\"kind\": \"Enum\",\n            \"type\": \"\"", StringComparison.Ordinal))
            })
            {
                Verify.That(mutatedText != original, "descriptor mutation had no effect: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, mutatedText);
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }
        });
    }
}
