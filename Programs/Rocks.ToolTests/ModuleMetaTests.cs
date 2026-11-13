using System.Text.Json;
using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// MetaCompiler 모듈 모드 시험이다(Docs/Design/ReflectionFoundations.ko.md 1.3–1.5절): 여러 헤더를 한 번에 생성하고,
// 의존 모듈 descriptor를 import해 base·참조를 풀고, private 헤더를 Private 생성 루트와 private 집계로 나눈다.
internal static class ModuleMetaTests
{
    private const string Parent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";

    private const string Value = "#pragma once\n#include <Lower/Parent.h>\n#include <Object/ObjectRefs.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Value : public Parent {\nROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n};\n}\n";

    private const string Leaf = "#pragma once\n#include <Upper/Value.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Leaf final : public Value {};\n}\n";

    private const string Detail = "#pragma once\n#include <Upper/Value.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Detail final : public Value {};\n}\n";

    // private 헤더만 있는 모듈의 타입이 import한 타입을 상속한다.
    private const string Hidden = "#pragma once\n#include <Lower/Parent.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Hidden final : public Parent {};\n}\n";

    // import한 Parent와 이름이 같지만 다른 파일의 선언(include하지 않은 재정의)이다.
    private const string FakeParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";

    private const string Impostor = "#pragma once\n#include <Upper/FakeParent.h>\nnamespace probe {\n" +
        "ROCKS_CLASS()\nstruct Impostor final : public Parent {};\n}\n";

    // B4b: struct를 가진 Lower다(Point, 참조 struct Link, struct 필드를 가진 Parent, enum Mode, NATIVE WorldOnly 소유자
    // 형질의 WorldBits와 그것을 담아 형질이 전파되는 Outer).
    private const string StructParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n#include <Object/ObjectRefs.h>\nnamespace probe {\n" +
        "struct Parent;\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off = 0 };\n" +
        "struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
        "ROCKS_STRUCT()\nstruct Point {\nROCKS_PROPERTY() rocks::uint16 x = 0;\n};\n" +
        "ROCKS_STRUCT()\nstruct Link {\nROCKS_PROPERTY() rocks::ObjectRef<Parent> target;\nROCKS_PROPERTY() Point at;\n};\n" +
        "ROCKS_STRUCT()\nstruct WorldBits {\nROCKS_NATIVE() Lease lease;\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        "ROCKS_STRUCT()\nstruct Outer {\nROCKS_PROPERTY() WorldBits inner;\n};\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() Point origin;\n};\n}\n";

    // B4c: CoreTypeBindings로 생성하는 Lower다(Vec3·Vec3d Binds 미러와 Parent).
    private const string BindsParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n#include <Core/Math/Vector3.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT(Binds = \"rocks::Vec3\")\nstruct Vec3Binding {\nROCKS_PROPERTY() float x = 0;\nROCKS_PROPERTY() float y = 0;\nROCKS_PROPERTY() float z = 0;\n};\n" +
        "ROCKS_STRUCT(Binds = \"rocks::Vec3d\")\nstruct Vec3dBinding {\nROCKS_PROPERTY() double x = 0;\nROCKS_PROPERTY() double y = 0;\nROCKS_PROPERTY() double z = 0;\n};\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";

    // Binds를 받는 유일한 모듈 이름이다(StructRules.BindingModule).
    private const string BindingsModule = "CoreTypeBindings";

    // 깊이 16의 struct 사슬(Chain1 ← … ← Chain16), 위조용 Top(UInt16 필드 하나), Parent를 가진 Lower다.
    private static readonly string ChainParent = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT()\nstruct Chain1 {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        string.Concat(Enumerable.Range(2, 15).Select(level => $"ROCKS_STRUCT()\nstruct Chain{level} {{\nROCKS_PROPERTY() Chain{level - 1} inner;\n}};\n")) +
        "ROCKS_STRUCT()\nstruct Top {\nROCKS_PROPERTY() rocks::uint16 x = 0;\n};\n" +
        "ROCKS_CLASS()\nstruct Parent : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n}\n";

    // Upper의 다른 헤더 struct다(import struct를 멤버로 담는다).
    private const string Shape = "#pragma once\n#include <Lower/Parent.h>\nnamespace probe {\n" +
        "ROCKS_STRUCT()\nstruct Shape {\nROCKS_PROPERTY() Point corner;\nROCKS_PROPERTY() Link link;\n};\n}\n";

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
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

        // B3b: 의존 모듈의 ROCKS_ENUM을 descriptor로 import해 필드 타입으로 쓴다. 생성 헤더는 그 enum의 생성 헤더를
        // include하고, 의존 모듈 집계가 enum을 먼저 등록한다.
        suite.Add("Meta.ImportedEnumsResolveAsFieldTypes", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("namespace probe {\n",
                "namespace probe {\nROCKS_ENUM(ToolPrefix = \"MODE_\")\nenum class Mode : rocks::uint16 { Off = 0, On ROCKS_ENUMERATOR(Name = \"Enabled\") = 3 };\n",
                StringComparison.Ordinal));
            layout.Manifest("Lower", ["probe::Mode"], []);
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() Mode mode = Mode::Off;\n", StringComparison.Ordinal));
            layout.Manifest("Upper", [], ["probe::Value::mode"]);
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string lowerAggregate = File.ReadAllText(layout.Generated("Lower", "Public/Lower/Lower.module.generated.h"));
            // 생성물은 clang-format을 거치므로 줄바꿈 위치에 기대지 않고 각 호출의 고유 부분을 찾는다.
            int enumRegistration = lowerAggregate.IndexOf("GeneratedType<::probe::Mode>::Schema()", StringComparison.Ordinal);
            int classRegistration = lowerAggregate.IndexOf("GeneratedType<::probe::Parent>::Schema()", StringComparison.Ordinal);
            Verify.That(enumRegistration >= 0 && classRegistration > enumRegistration, "aggregate registers enums first:\n" + lowerAggregate);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            JsonElement mode = Type(lowerDescriptor, "probe::Mode");
            Verify.Equal("Enabled", mode.GetProperty("values")[1].GetProperty("toolName").GetString(), "imported enumerator tool name");
            JsonElement field = Type(upperDescriptor, "probe::Value").GetProperty("fields").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == "mode");
            Verify.Equal("Enum", field.GetProperty("value").GetProperty("kind").GetString(), "imported enum field kind");
            Verify.Equal(mode.GetProperty("id").GetString(), field.GetProperty("value").GetProperty("type").GetString(), "imported enum target");
            string generated = File.ReadAllText(layout.Generated("Upper", "Public/Upper/Value.generated.h"));
            Verify.Contains("#include <Lower/Parent.generated.h>", generated, "imported enum generated header include");
        });

        // 의존 모듈 클래스의 enum 필드를 상속하면 생성 헤더가 그 enum의 생성 헤더를 include하고 native 키를 싣는다.
        suite.Add("Meta.InheritedImportedEnumFieldsGenerate", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("namespace probe {\n",
                "namespace probe {\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off, On };\n", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY() rocks::int32 value = 0;\n", "ROCKS_PROPERTY() rocks::int32 value = 0;\nROCKS_PROPERTY() Mode level = Mode::Off;\n", StringComparison.Ordinal));
            layout.Manifest("Lower", ["probe::Mode"], ["probe::Parent::level"]);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);
            string generated = File.ReadAllText(layout.Generated("Upper", "Public/Upper/Value.generated.h"));
            Verify.Contains("#include <Lower/Parent.generated.h>", generated, "inherited enum generated header include");
            Verify.Contains("NativeTypeKey<::probe::Mode>()", generated, "inherited enum field native key");
        });

        // descriptor의 enum 항목도 엄격하게 읽는다(런타임 RegisterEnum과 같은 규칙, 어기면 RMC1000).
        suite.Add("Meta.EnumDescriptorIsReadStrictly", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", Parent.Replace("namespace probe {\n",
                "namespace probe {\nROCKS_ENUM(ToolPrefix = \"MODE_\")\nenum class Mode : rocks::uint16 { Off = 0, On ROCKS_ENUMERATOR(Name = \"Enabled\") = 3 };\n",
                StringComparison.Ordinal));
            layout.Manifest("Lower", ["probe::Mode"], []);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            foreach ((string name, string from, string to) in new[]
            {
                ("underlying", "\"underlying\": \"UInt16\"", "\"underlying\": \"Int64\""),
                ("alias", "\"value\": 3", "\"value\": 0"),
                ("range", "\"value\": 3", "\"value\": 70000"),
                ("collision", "\"toolName\": \"Enabled\"", "\"toolName\": \"MODE_off\""),
                ("toolName", "\"toolName\": \"Enabled\"", "\"toolName\": \"has space\""),
                ("prefix", "\"toolPrefix\": \"MODE_\"", "\"toolPrefix\": \"MO-DE\""),
                ("extraKey", "\"toolPrefix\": \"MODE_\"", "\"toolPrefix\": \"MODE_\",\n      \"extra\": 1")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }
        });

        // 같은 모듈의 public 클래스는 private enum을 필드로 쓸 수 없다(public 집계는 private enum을 등록하지 않는다).
        suite.Add("Meta.PublicClassCannotUsePrivateEnum", async () =>
        {
            using Layout layout = new(options, environment);
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Write("Upper/Private/Upper/Secret.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
                "ROCKS_ENUM()\nenum class Secret : rocks::uint8 { A };\n}\n");
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("#include <Object/ObjectRefs.h>\n", "#include <Object/ObjectRefs.h>\n#include <Upper/Secret.h>\n", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n", "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() Secret secret = Secret::A;\n", StringComparison.Ordinal));
            layout.Manifest("Upper", ["probe::Secret"], ["probe::Value::secret"]);
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false),
                (layout.Source("Upper/Private/Upper/Secret.h"), "Upper/Secret.h", true)]), "RMC2006", "private enum");
        });

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

        // B4b: 의존 모듈의 ROCKS_STRUCT를 descriptor로 import해 클래스·struct 필드로 쓴다. Lower 집계는 enum → struct →
        // 클래스로 등록하고, Upper 생성 헤더는 import struct(상속 필드 포함)와 같은 모듈 다른 헤더 struct의 생성 헤더를
        // include하며, 참조 형질이 import struct에서 전파된다(Docs/Design/ReflectionFoundations.ko.md 4.6절).
        suite.Add("Meta.ImportedStructsResolveAsFieldTypes", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string lowerAggregate = Compact(File.ReadAllText(layout.Generated("Lower", "Public/Lower/Lower.module.generated.h")));
            int point = lowerAggregate.IndexOf("RegisterStructIfAbsent(::rocks::GeneratedType<::probe::Point>::Schema()", StringComparison.Ordinal);
            int link = lowerAggregate.IndexOf("RegisterStructIfAbsent(::rocks::GeneratedType<::probe::Link>::Schema()", StringComparison.Ordinal);
            int parent = lowerAggregate.IndexOf("RegisterIfAbsent(::rocks::GeneratedType<::probe::Parent>::Schema()", StringComparison.Ordinal);
            Verify.That(point >= 0 && link > point && parent > link, "aggregate registers member structs first, then classes:\n" + lowerAggregate);
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false),
                (layout.Source("Upper/Public/Upper/Shape.h"), "Upper/Shape.h", false)]);
            Verify.Equal(0, upper.ExitCode, "upper module: " + upper.StdErr);

            using JsonDocument lowerDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Lower")));
            using JsonDocument upperDescriptor = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            string pointId = Type(lowerDescriptor, "probe::Point").GetProperty("id").GetString()!;
            string linkId = Type(lowerDescriptor, "probe::Link").GetProperty("id").GetString()!;
            Verify.Equal("struct", Type(lowerDescriptor, "probe::Link").GetProperty("kind").GetString(), "struct descriptor kind");
            Verify.Equal("ContainsStrongReference", Type(lowerDescriptor, "probe::Link").GetProperty("flags")[0].GetString(), "reference struct flag");
            JsonElement Field(JsonDocument descriptor, string type, string name) => Type(descriptor, type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string TargetOf(JsonDocument descriptor, string type, string name) => Field(descriptor, type, name).GetProperty("value").GetProperty("type").GetString()!;
            Verify.Equal(pointId, TargetOf(upperDescriptor, "probe::Value", "at"), "own field of an imported struct");
            Verify.Equal(pointId, TargetOf(upperDescriptor, "probe::Value", "origin"), "inherited imported struct field");
            Verify.Equal(Type(upperDescriptor, "probe::Shape").GetProperty("id").GetString(), TargetOf(upperDescriptor, "probe::Value", "shape"), "same-module struct field");
            Verify.Equal(linkId, TargetOf(upperDescriptor, "probe::Shape", "link"), "struct member of an imported struct");
            Verify.Equal("ContainsStrongReference", Type(upperDescriptor, "probe::Shape").GetProperty("flags")[0].GetString(), "reference trait propagates from an imported struct");

            string value = Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Value.generated.h")));
            Verify.That(value.Contains("#include<Lower/Parent.generated.h>", StringComparison.Ordinal), "imported struct generated header include");
            Verify.That(value.Contains("#include<Upper/Shape.generated.h>", StringComparison.Ordinal), "same-module struct generated header include");
            Verify.That(value.Contains(Compact("GeneratedType<::probe::Point>::Trace(typed.*(&::probe::Parent::origin), visitor);"), StringComparison.Ordinal), "inherited struct field Trace");
            Verify.That(value.Contains(Compact("NativeTypeKey<::probe::Shape>()"), StringComparison.Ordinal), "same-module struct field native key");
            Verify.That(!value.Contains("structGeneratedType<::probe::Shape>", StringComparison.Ordinal), "struct specialization is defined once (in its own header)");
            string shape = Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Shape.generated.h")));
            Verify.That(shape.Contains("#include<Lower/Parent.generated.h>", StringComparison.Ordinal), "struct header includes imported member struct header");
            Verify.That(shape.Contains(Compact("GeneratedType<::probe::Link>::Trace(value.*(&::probe::Shape::link), visitor);"), StringComparison.Ordinal), "struct traces imported reference struct");
            string aggregate = Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")));
            int dependency = aggregate.IndexOf("::rocks::generated::Lower::RegisterModuleTypes", StringComparison.Ordinal);
            int own = aggregate.IndexOf("RegisterStructIfAbsent(::rocks::GeneratedType<::probe::Shape>::Schema()", StringComparison.Ordinal);
            int owner = aggregate.IndexOf("RegisterIfAbsent(::rocks::GeneratedType<::probe::Value>::Schema()", StringComparison.Ordinal);
            Verify.That(dependency >= 0 && own > dependency && owner > own, "upper aggregate registration order:\n" + aggregate);
        });

        // public 타입은 private struct를 필드·NATIVE 멤버로 쓸 수 없다(같은 모듈 — public 집계·생성 헤더가 private 것을 볼 수
        // 없다). 다른 모듈의 private struct도 쓸 수 없다.
        suite.Add("Meta.PublicTypeCannotUsePrivateStruct", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Write("Upper/Private/Upper/Secret.h", "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\nnamespace probe {\n" +
                "ROCKS_STRUCT()\nstruct Secret {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n}\n");
            layout.Manifest("Upper", ["probe::Secret"], ["probe::Secret::a", "probe::Value::secret", "probe::Shape::secret"]);
            foreach (string member in new[] { "ROCKS_PROPERTY() Secret secret;\n", "ROCKS_NATIVE() Secret secret;\n" })
            {
                layout.Write("Upper/Public/Upper/Value.h", Value.Replace("#include <Object/ObjectRefs.h>\n", "#include <Object/ObjectRefs.h>\n#include <Upper/Secret.h>\n", StringComparison.Ordinal)
                    .Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n", "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n" + member, StringComparison.Ordinal));
                await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false),
                    (layout.Source("Upper/Private/Upper/Secret.h"), "Upper/Secret.h", true)]), "RMC2006", "private struct");
            }

            // public struct도 같은 모듈의 private struct를 멤버로 쓸 수 없다.
            layout.Write("Upper/Public/Upper/Shape.h", "#pragma once\n#include <Upper/Secret.h>\nnamespace probe {\n" +
                "ROCKS_STRUCT()\nstruct Shape {\nROCKS_PROPERTY() Secret secret;\n};\n}\n");
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Shape.h"), "Upper/Shape.h", false),
                (layout.Source("Upper/Private/Upper/Secret.h"), "Upper/Secret.h", true)]), "RMC2006", "private struct");
            layout.Write("Upper/Public/Upper/Shape.h", Shape);

            Verify.Equal(0, (await layout.RunLowerAsync(privateParent: true)).ExitCode, "private lower module");
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Shape.h"), "Upper/Shape.h", false)]), "RMC2006", "private reflected type");
        });

        // descriptor의 struct 항목도 엄격하게 읽는다(키·flags·binds·필드 owner·대상 ID, 형질과 필드의 일치 — 어기면 RMC1000).
        suite.Add("Meta.StructDescriptorIsReadStrictly", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument parsed = JsonDocument.Parse(original);
            string pointId = Type(parsed, "probe::Point").GetProperty("id").GetString()!;
            foreach ((string name, string from, string to) in new[]
            {
                ("extraKey", "\"binds\": \"\"", "\"binds\": \"\",\n      \"base\": \"rocks::Object\""),
                ("binds", "\"binds\": \"\"", "\"binds\": \"rocks::Vector3<float\""),
                ("unknownFlag", "\"ContainsStrongReference\"", "\"Abstract\""),
                ("missingTrait", "\"ContainsStrongReference\"", ""),
                ("owner", "\"owner\": \"probe::Point\"", "\"owner\": \"probe::Link\""),
                ("emptyTarget", $"\"type\": \"{pointId}\"", "\"type\": \"\"")
            })
            {
                Verify.That(original.Contains(from, StringComparison.Ordinal), "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, original.Replace(from, to, StringComparison.Ordinal));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }

            // 대상 ID가 다른 종류의 타입을 가리키거나(Struct 필드 → enum, 참조 → struct) 멤버 struct의 WorldOnly 소유자 형질이
            // 전파되지 않은 descriptor도 망가진 것이다.
            string modeId = Type(parsed, "probe::Mode").GetProperty("id").GetString()!;
            foreach ((string name, string type, Action<System.Text.Json.Nodes.JsonObject> change) in new (string, string, Action<System.Text.Json.Nodes.JsonObject>)[]
            {
                ("structToEnum", "probe::Link", link => FieldNode(link, "at")["value"]!["type"] = modeId),
                ("referenceToStruct", "probe::Link", link => FieldNode(link, "target")["value"]!["type"] = pointId),
                ("missingWorldOnlyTrait", "probe::Outer", outer => outer["flags"] = new System.Text.Json.Nodes.JsonArray())
            })
            {
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, MutateType(original, type, change));
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC1000", "invalid descriptor");
            }
        });

        // import한 struct의 형질: Lower의 WorldBits(RequiresWorldOnlyOwner)를 필드로 둔 WorldOnly 아닌 Upper 클래스는 RMC2206이고,
        // descriptor가 public struct에서 private struct를 쓰거나 다른 모듈의 private struct를 쓰면 RMC2006이다.
        suite.Add("Meta.ImportedStructTraitsAndVisibilityAreEnforced", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            layout.Manifest("Upper", [], ["probe::Value::bits"]);
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() WorldBits bits;\n", StringComparison.Ordinal));
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)]), "RMC2206", "requires a WorldOnly owner");

            string original = File.ReadAllText(layout.Descriptor("Lower"));
            string hidden = layout.Generated("Mutated", "privatePoint.rocksmeta.json");
            Directory.CreateDirectory(Path.GetDirectoryName(hidden)!);
            File.WriteAllText(hidden, MutateType(original, "probe::Point", point => point["visibility"] = "private"));
            await Reject(layout.RunUpperAsync(imports: [hidden]), "RMC2006", "uses private struct");

            // 다른 모듈(Other)의 private 타입이 Lower의 private struct를 쓴다(손으로 만든 descriptor).
            Verify.Equal(0, (await layout.RunLowerAsync(privateParent: true)).ExitCode, "private lower module");
            string privateLower = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument parsed = JsonDocument.Parse(privateLower);
            string pointId = Type(parsed, "probe::Point").GetProperty("id").GetString()!;
            System.Text.Json.Nodes.JsonNode other = System.Text.Json.Nodes.JsonNode.Parse(privateLower)!;
            System.Text.Json.Nodes.JsonObject box = other["types"]!.AsArray().Select(node => node!.AsObject())
                .Single(node => (string?)node["name"] == "probe::Point").DeepClone().AsObject();
            box["name"] = "probe::OtherBox";
            box["id"] = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b01";
            box["header"] = "Other/Box.h";
            System.Text.Json.Nodes.JsonObject inner = FieldNode(box, "x");
            inner["owner"] = "probe::OtherBox";
            inner["name"] = "inner";
            inner["toolName"] = "inner";
            inner["id"] = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b02";
            inner["value"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "Struct", ["type"] = pointId };
            other["module"] = "Other";
            other["types"] = new System.Text.Json.Nodes.JsonArray(box);
            string otherPath = layout.Generated("Mutated", "Other.rocksmeta.json");
            File.WriteAllText(otherPath, other.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            await Reject(layout.RunUpperAsync(imports: [layout.Descriptor("Lower"), otherPath]), "RMC2006", "of module Lower");
        });

        // B4c: CoreTypeBindings descriptor의 Binds 미러는 실제 타입 철자(binds)로 다른 모듈의 필드 형식을 푼다. 생성 헤더는
        // 미러의 생성 헤더를 include하고 실제 타입의 native 키를 싣는다. descriptor의 binds는 철자 문법(RMC1000)과 모듈·중복·
        // 리플렉션 타입 결합 규칙(RMC2004)을 검사하고, 미러 타입을 필드 형식으로 쓰면 거부한다.
        suite.Add("Meta.ImportedBindsResolveRealTypes", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", BindsParent);
            layout.Manifest("Lower", ["probe::Vec3Binding", "probe::Vec3dBinding"],
                ["probe::Vec3Binding::x", "probe::Vec3Binding::y", "probe::Vec3Binding::z", "probe::Vec3dBinding::x", "probe::Vec3dBinding::y", "probe::Vec3dBinding::z"]);
            Directory.CreateDirectory(layout.Generated(BindingsModule, "Public"));
            Directory.CreateDirectory(layout.Generated(BindingsModule, "Private"));
            ProcessResult lower = await layout.RunLowerAsync(module: BindingsModule);
            Verify.Equal(0, lower.ExitCode, "bindings module: " + lower.StdErr);
            layout.Manifest("Upper", [], ["probe::Value::position", "probe::Value::mirror"]);
            string ValueWith(string member) => Value.Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n" + member, StringComparison.Ordinal);
            Task<ProcessResult> RunUpper(string descriptor) => layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)],
                imports: [descriptor], dependencyModule: BindingsModule);
            // 묶인 실제 타입을 NATIVE 멤버로 두면 생성 코드가 그 미러의 생성 헤더를 include하고 형질을 확인한다.
            layout.Write("Upper/Public/Upper/Value.h", ValueWith("ROCKS_PROPERTY() rocks::Vec3 position;\nROCKS_NATIVE() rocks::Vec3 cache;\n"));
            ProcessResult upper = await RunUpper(layout.Descriptor(BindingsModule));
            Verify.Equal(0, upper.ExitCode, "consumer module: " + upper.StdErr);

            using JsonDocument bindings = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor(BindingsModule)));
            using JsonDocument consumer = JsonDocument.Parse(File.ReadAllBytes(layout.Descriptor("Upper")));
            Verify.Equal("rocks::Vector3<float>", Type(bindings, "probe::Vec3Binding").GetProperty("binds").GetString(), "descriptor binds");
            Verify.Equal(Type(bindings, "probe::Vec3Binding").GetProperty("id").GetString(),
                FieldNodeOf(consumer, "probe::Value", "position").GetProperty("value").GetProperty("type").GetString(), "imported real type resolves to the mirror");
            string generated = Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Value.generated.h")));
            Verify.That(generated.Contains("#include<Lower/Parent.generated.h>", StringComparison.Ordinal), "mirror generated header include");
            Verify.That(generated.Contains(Compact("NativeTypeKey<::rocks::Vector3<float>>()"), StringComparison.Ordinal), "real type native key");
            Verify.That(generated.Contains(Compact("static_assert(!GeneratedType<::rocks::Vector3<float>>::ContainsStrongReference"), StringComparison.Ordinal),
                "NATIVE member of a bound real type is checked by its generated trait");
            Verify.That(Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Upper.module.generated.h")))
                .Contains("::rocks::generated::CoreTypeBindings::RegisterModuleTypes", StringComparison.Ordinal), "bindings module is registered first");

            string original = File.ReadAllText(layout.Descriptor(BindingsModule));
            string vec3Id = Type(bindings, "probe::Vec3Binding").GetProperty("id").GetString()!;
            foreach ((string name, string code, string reason, Func<string> mutate) in new (string, string, string, Func<string>)[]
            {
                ("bindsReflected", "RMC2004", "binds reflected type", () => MutateType(original, "probe::Vec3Binding", mirror => mirror["binds"] = "probe::Parent")),
                ("bindsGlobal", "RMC1000", "invalid descriptor", () => MutateType(original, "probe::Vec3Binding", mirror => mirror["binds"] = "::rocks::Vector3<float>")),
                ("bindsCycle", "RMC2302", "cycle", () => MutateType(original, "probe::Vec3Binding",
                    mirror => FieldNode(mirror, "x")["value"] = new System.Text.Json.Nodes.JsonObject { ["kind"] = "Struct", ["type"] = vec3Id })),
                ("bindsTwice", "RMC2004", "both bind", () => MutateType(original, "probe::Vec3dBinding", mirror => mirror["binds"] = "rocks::Vector3<float>")),
                ("bindsSpelling", "RMC1000", "invalid descriptor", () => MutateType(original, "probe::Vec3Binding", mirror => mirror["binds"] = "rocks::Vec3; int x")),
                ("bindsModule", "RMC2004", "only CoreTypeBindings", () => original.Replace($"\"module\": \"{BindingsModule}\"", "\"module\": \"Other\"", StringComparison.Ordinal))
            })
            {
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, mutate());
                await Reject(RunUpper(mutated), code, reason);
            }

            layout.Write("Upper/Public/Upper/Value.h", ValueWith("ROCKS_PROPERTY() Vec3Binding mirror;\n"));
            await Reject(RunUpper(layout.Descriptor(BindingsModule)), "RMC2000", "is a Binds mirror declaration");
        });

        // CoreTypeBindings를 import했는데 그 모듈이 묶지 않은 인스턴스(여기서는 Vec3만 묶고 Vec3d를 쓴다)를 쓰면 의존을 더하라는
        // 안내가 아니라 묶인 인스턴스를 알려 준다.
        suite.Add("Meta.UnboundCoreValueInstantiationHint", async () =>
        {
            using Layout layout = new(options, environment);
            int vec3d = BindsParent.IndexOf("ROCKS_STRUCT(Binds = \"rocks::Vec3d\")", StringComparison.Ordinal);
            int parent = BindsParent.IndexOf("ROCKS_CLASS()", StringComparison.Ordinal);
            layout.Write("Lower/Public/Lower/Parent.h", BindsParent[..vec3d] + BindsParent[parent..]);
            layout.Manifest("Lower", ["probe::Vec3Binding"], ["probe::Vec3Binding::x", "probe::Vec3Binding::y", "probe::Vec3Binding::z"]);
            Directory.CreateDirectory(layout.Generated(BindingsModule, "Public"));
            Directory.CreateDirectory(layout.Generated(BindingsModule, "Private"));
            ProcessResult lower = await layout.RunLowerAsync(module: BindingsModule);
            Verify.Equal(0, lower.ExitCode, "bindings module: " + lower.StdErr);
            layout.Manifest("Upper", [], ["probe::Value::precise"]);
            layout.Write("Upper/Public/Upper/Value.h", Value.Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() rocks::Vec3d precise;\n", StringComparison.Ordinal));
            ProcessResult upper = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Value.h"), "Upper/Value.h", false)],
                imports: [layout.Descriptor(BindingsModule)], dependencyModule: BindingsModule);
            Verify.That(upper.ExitCode != 0, "unbound Core value instantiation was accepted");
            Verify.Contains("RMC2000", upper.StdErr, "unsupported field type code");
            Verify.Contains("not bound by CoreTypeBindings", upper.StdErr, "unbound instantiation hint");
            Verify.Contains("rocks::Vector3<float>", upper.StdErr, "bound instantiation in the hint");
            Verify.That(!upper.StdErr.Contains("RequiredModules", StringComparison.Ordinal), "dependency hint is not shown when the descriptor is imported");
        });

        // 깊이는 import struct까지 이어서 센다: 위조한 import 사슬 17단(descriptor), 이 모듈 struct가 import한 깊이 16 사슬을
        // 담는 경우는 RMC2303이고 깊이 15 사슬을 담으면 깊이 16으로 받는다.
        suite.Add("Meta.ImportedStructDepthIsChecked", async () =>
        {
            using Layout layout = new(options, environment);
            layout.Write("Lower/Public/Lower/Parent.h", ChainParent);
            layout.Manifest("Lower", Enumerable.Range(1, 16).Select(level => $"probe::Chain{level}").Append("probe::Top").ToArray(),
                Enumerable.Range(2, 15).Select(level => $"probe::Chain{level}::inner").Concat(["probe::Chain1::value", "probe::Top::x"]).ToArray());
            ProcessResult lower = await layout.RunLowerAsync();
            Verify.Equal(0, lower.ExitCode, "lower module: " + lower.StdErr);
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument parsed = JsonDocument.Parse(original);
            string chain16 = Type(parsed, "probe::Chain16").GetProperty("id").GetString()!;
            string forged = layout.Generated("Mutated", "deep.rocksmeta.json");
            Directory.CreateDirectory(Path.GetDirectoryName(forged)!);
            File.WriteAllText(forged, original.Replace("\"kind\": \"UInt16\"", $"\"kind\": \"Struct\", \"type\": \"{chain16}\"", StringComparison.Ordinal));
            await Reject(layout.RunUpperAsync(imports: [forged]), "RMC2303", "nesting depth 17");

            layout.Manifest("Upper", ["probe::Deep"], ["probe::Deep::inner"]);
            string Deep(int level) => $"#pragma once\n#include <Lower/Parent.h>\nnamespace probe {{\nROCKS_STRUCT()\nstruct Deep {{\nROCKS_PROPERTY() Chain{level} inner;\n}};\n}}\n";
            layout.Write("Upper/Public/Upper/Deep.h", Deep(15));
            ProcessResult accepted = await layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Deep.h"), "Upper/Deep.h", false)]);
            Verify.Equal(0, accepted.ExitCode, "struct over an imported depth-15 chain: " + accepted.StdErr);
            Verify.That(Compact(File.ReadAllText(layout.Generated("Upper", "Public/Upper/Deep.generated.h"))).Contains("staticconstexpruint32Depth=16u;", StringComparison.Ordinal),
                "depth continues through imported structs");
            layout.Write("Upper/Public/Upper/Deep.h", Deep(16));
            await Reject(layout.RunUpperInputsAsync([(layout.Source("Upper/Public/Upper/Deep.h"), "Upper/Deep.h", false)]), "RMC2303", "nesting depth 17");
        });

        // struct 순환은 C++ 값 멤버로는 만들 수 없으므로(불완전 타입) 조작한 descriptor로 import 쪽 DFS를 시험한다(RMC2302).
        suite.Add("Meta.ImportedStructCycleIsRejected", async () =>
        {
            using Layout layout = new(options, environment);
            layout.WriteStructModules();
            Verify.Equal(0, (await layout.RunLowerAsync()).ExitCode, "lower module");
            string original = File.ReadAllText(layout.Descriptor("Lower"));
            using JsonDocument parsed = JsonDocument.Parse(original);
            string linkId = Type(parsed, "probe::Link").GetProperty("id").GetString()!;
            string pointId = Type(parsed, "probe::Point").GetProperty("id").GetString()!;
            // Point → Link → Point, 그리고 Point → Point(자기 자신).
            foreach ((string name, string target) in new[] { ("mutual", linkId), ("self", pointId) })
            {
                string mutatedText = original.Replace("\"kind\": \"UInt16\"", $"\"kind\": \"Struct\", \"type\": \"{target}\"", StringComparison.Ordinal);
                Verify.That(mutatedText != original, "descriptor fixture changed: " + name);
                string mutated = layout.Generated("Mutated", name + ".rocksmeta.json");
                Directory.CreateDirectory(Path.GetDirectoryName(mutated)!);
                File.WriteAllText(mutated, mutatedText);
                await Reject(layout.RunUpperAsync(imports: [mutated]), "RMC2302", "cycle");
            }
        });
    }

    // 공백을 모두 지운 텍스트다(clang-format 줄바꿈과 무관하게 생성 코드 조각을 찾는다).
    private static string Compact(string text) => System.Text.RegularExpressions.Regex.Replace(text, @"\s+", "",
        System.Text.RegularExpressions.RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));

    private static JsonElement Type(JsonDocument descriptor, string name) =>
        descriptor.RootElement.GetProperty("types").EnumerateArray().Single(type => type.GetProperty("name").GetString() == name);

    // descriptor의 name 타입 항목을 change로 바꾼 JSON이다(서식과 무관하게 위조 descriptor를 만든다).
    private static string MutateType(string descriptor, string name, Action<System.Text.Json.Nodes.JsonObject> change)
    {
        System.Text.Json.Nodes.JsonNode root = System.Text.Json.Nodes.JsonNode.Parse(descriptor)!;
        change(root["types"]!.AsArray().Select(node => node!.AsObject()).Single(node => (string?)node["name"] == name));
        return root.ToJsonString(new JsonSerializerOptions { WriteIndented = true });
    }

    private static System.Text.Json.Nodes.JsonObject FieldNode(System.Text.Json.Nodes.JsonObject type, string name) =>
        type["fields"]!.AsArray().Select(node => node!.AsObject()).Single(node => (string?)node["name"] == name);

    private static JsonElement FieldNodeOf(JsonDocument descriptor, string type, string name) =>
        Type(descriptor, type).GetProperty("fields").EnumerateArray().Single(field => field.GetProperty("name").GetString() == name);

    private static async Task Reject(Task<ProcessResult> run, string code, string reason)
    {
        ProcessResult result = await run;
        Verify.That(result.ExitCode != 0, $"accepted input that must fail with {code}");
        Verify.Contains(code, result.StdErr, "stable diagnostic code");
        Verify.That(result.StdErr.Contains(reason, StringComparison.OrdinalIgnoreCase), "diagnostic reason missing: " + reason + "\n" + result.StdErr);
    }

    // Lower(Parent)와 Upper(Value·Leaf public, Detail private) 두 모듈의 원본·명세·생성 루트다.
    private sealed class Layout : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();
        private readonly Options options;
        private readonly IReadOnlyDictionary<string, string?> environment;

        public Layout(Options options, IReadOnlyDictionary<string, string?> environment)
        {
            this.options = options;
            this.environment = environment;
            Write("Lower/Public/Lower/Parent.h", Parent);
            Write("Upper/Public/Upper/Value.h", Value);
            Write("Upper/Public/Upper/Leaf.h", Leaf);
            Write("Upper/Private/Upper/Detail.h", Detail);
            Write("Upper/Private/Upper/Hidden.h", Hidden);
            Write("Upper/Public/Upper/FakeParent.h", FakeParent);
            Write("Upper/Public/Upper/Impostor.h", Impostor);
            Manifest("Lower", ["probe::Parent"], ["probe::Parent::value"]);
            // Upper 명세는 재정의 시험이 식별자 단계를 지나 RMC2004에 닿도록 probe::Parent도 담는다.
            Manifest("Upper", ["probe::Value", "probe::Leaf", "probe::Detail", "probe::Hidden", "probe::Impostor", "probe::Parent"],
                ["probe::Value::next", "probe::Parent::value"]);
            foreach (string module in new[] { "Lower", "Upper" })
            {
                Directory.CreateDirectory(Generated(module, "Public"));
                Directory.CreateDirectory(Generated(module, "Private"));
            }
        }

        public string Generated(string module, string relative) => Path.Combine(temporary.Root, "out", module, relative);

        public string Descriptor(string module) => Path.Combine(temporary.Root, "out", module + ".rocksmeta.json");

        public void Write(string relative, string text) => temporary.Write(relative, text);

        // Lower를 struct 판(Point·Link·Parent.origin)으로, Upper Value를 struct 필드(Shape·Point)를 쓰는 판으로 바꾸고 명세를 더한다.
        public void WriteStructModules()
        {
            Write("Lower/Public/Lower/Parent.h", StructParent);
            Write("Upper/Public/Upper/Shape.h", Shape);
            Write("Upper/Public/Upper/Value.h", Value.Replace("#include <Object/ObjectRefs.h>\n", "#include <Object/ObjectRefs.h>\n#include <Upper/Shape.h>\n", StringComparison.Ordinal)
                .Replace("ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\n",
                    "ROCKS_PROPERTY() rocks::ObjectRef<Parent> next;\nROCKS_PROPERTY() Shape shape;\nROCKS_PROPERTY() Point at;\n", StringComparison.Ordinal));
            Manifest("Lower", ["probe::Point", "probe::Link", "probe::Mode", "probe::WorldBits", "probe::Outer"],
                ["probe::Point::x", "probe::Link::target", "probe::Link::at", "probe::Parent::origin", "probe::WorldBits::value", "probe::Outer::inner"]);
            Manifest("Upper", ["probe::Shape"], ["probe::Shape::corner", "probe::Shape::link", "probe::Value::shape", "probe::Value::at"]);
        }

        // module은 생성할 모듈 이름이다(원본·명세는 Lower 것을 쓴다). extra는 그대로 덧붙인다.
        public Task<ProcessResult> RunLowerAsync(bool privateParent = false, IReadOnlyList<(string Header, string Include)>? extraInputs = null,
            string? descriptor = null, string module = "Lower", IReadOnlyList<string>? extra = null)
        {
            List<string> arguments = ["--module", module, "--input", Source("Lower/Public/Lower/Parent.h"), "--include", "Lower/Parent.h"];
            foreach ((string header, string include) in extraInputs ?? [])
            {
                arguments.AddRange(["--input", Source("Lower/Public/" + header), "--include", include]);
            }

            if (privateParent)
            {
                arguments.AddRange(["--private", "Lower/Parent.h"]);
            }

            arguments.AddRange(extra ?? []);
            return RunAsync(module, arguments, [Source("Lower/Public")], [], [], descriptor, manifestModule: "Lower");
        }

        public Task<ProcessResult> RunUpperAsync(bool withLeaf = true, bool privateValue = false, IReadOnlyList<string>? imports = null,
            IReadOnlyList<string>? extra = null)
        {
            List<string> arguments = ["--module", "Upper", "--input", Source("Upper/Public/Upper/Value.h"), "--include", "Upper/Value.h"];
            if (withLeaf)
            {
                arguments.AddRange(["--input", Source("Upper/Public/Upper/Leaf.h"), "--include", "Upper/Leaf.h"]);
            }

            arguments.AddRange(["--input", Source("Upper/Private/Upper/Detail.h"), "--include", "Upper/Detail.h", "--private", "Upper/Detail.h"]);
            if (privateValue)
            {
                arguments.AddRange(["--private", "Upper/Value.h"]);
            }

            foreach (string import in imports ?? [Descriptor("Lower")])
            {
                arguments.AddRange(["--import", import]);
            }

            arguments.AddRange(extra ?? []);
            return RunAsync("Upper", arguments, [Source("Upper/Public")], [Source("Upper/Private")], [Source("Lower/Public"), Generated("Lower", "Public")]);
        }

        // 임의 입력 집합으로 Upper를 생성한다(Lower descriptor import).
        public Task<ProcessResult> RunUpperInputsAsync(IReadOnlyList<(string Path, string Include, bool Private)> inputs, IReadOnlyList<string>? imports = null,
            string dependencyModule = "Lower")
        {
            List<string> arguments = ["--module", "Upper"];
            foreach ((string path, string include, bool isPrivate) in inputs)
            {
                arguments.AddRange(["--input", path, "--include", include]);
                if (isPrivate)
                {
                    arguments.AddRange(["--private", include]);
                }
            }

            foreach (string import in imports ?? [Descriptor("Lower")])
            {
                arguments.AddRange(["--import", import]);
            }

            return RunAsync("Upper", arguments, [Source("Upper/Public")], [Source("Upper/Private")], [Source("Lower/Public"), Generated(dependencyModule, "Public")]);
        }

        public string Source(string relative) => temporary.FilePath(relative);

        public void Manifest(string module, string[] types, string[] fields)
        {
            StableIdManifest.Update(Source(module + "/" + module + ".rocksids.json"), true, manifest =>
            {
                foreach (string type in types)
                {
                    manifest.Register("type", type);
                }

                foreach (string field in fields)
                {
                    manifest.Register("field", field);
                }

                return 0;
            });
        }

        // include 검색 순서는 빌드(MetadataLayout.IncludeOrder)와 같다: 자기 public 원본·생성 Public, 자기 private 원본·생성
        // Private, 의존 모듈의 public 원본·생성 루트, Core.
        private async Task<ProcessResult> RunAsync(string module, List<string> moduleArguments, IReadOnlyList<string> ownPublic,
            IReadOnlyList<string> ownPrivate, IReadOnlyList<string> dependencies, string? descriptor = null, string? manifestModule = null)
        {
            string generator = Path.GetFullPath(options.Required("metacompiler"));
            string executable = generator;
            List<string> arguments = [];
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }

            arguments.AddRange(moduleArguments);
            arguments.AddRange(["--output-root", Path.Combine(temporary.Root, "out", module), "--descriptor", descriptor ?? Descriptor(module),
                "--compiler", options.Required("compiler"), "--identity-manifest", Source((manifestModule ?? module) + "/" + (manifestModule ?? module) + ".rocksids.json")]);
            IEnumerable<string> includes = ownPublic.Append(Generated(module, "Public"))
                .Concat(ownPrivate).Append(Generated(module, "Private"))
                .Concat(dependencies)
                .Concat(Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                    .Select(directory => Path.Combine(directory, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal));
            foreach (string include in includes)
            {
                arguments.AddRange(["--include-dir", include]);
            }

            foreach (string argument in options.ClangArguments)
            {
                arguments.Add("--clang-arg=" + argument);
            }

            return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(120)));
        }

        public void Dispose() => temporary.Dispose();
    }
}
