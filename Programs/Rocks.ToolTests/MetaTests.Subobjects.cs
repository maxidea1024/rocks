using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 선언 subobject(ROCKS_PROPERTY(Subobject·Root·AttachParent)·ROCKS_CLASS(SubobjectList·SubobjectRoot·SubobjectAttach)·
// InitSubobjectDefaults — Docs/Design/Prefab 10.2절)다: descriptor schema 10의 subobjects(펼친 표·소유 구조 지정자), 생성 schema·연산표, 생성
// 코드 컴파일, 거부 사례(RMC3001 지정자 조합, RMC3002 소유·타입·상속 규칙, RMC3003 hook 모양, RMC2100 field ↔ subobject kind 변경).
internal static partial class MetaTests
{
    private const string SubobjectPreamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n" +
        "#include <Core/Text/String.h>\n#include <Core/Result.h>\n#include <Object/ObjectRefArray.h>\nnamespace rocks {\nclass SubobjectDefaultsContext;\n}\nnamespace probe {\n" +
        "ROCKS_CLASS()\nclass Part : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
        "ROCKS_CLASS(SubobjectAttach = \"parent\")\nclass ScenePart : public Part {\nROCKS_GENERATED_BODY()\npublic:\n" +
        "ROCKS_PROPERTY() rocks::WeakObjectRef<ScenePart> parent;\n};\n";

    // 소유 클래스 Owner(소유 목록 parts, root 필드 root)의 원본이다. members는 Owner 안(protected 구역), extra는 Owner 뒤에 둔다.
    private static string SubobjectOwner(string members, string extra = "", string ownerSpecifiers = "SubobjectList = \"parts\", SubobjectRoot = \"root\"") =>
        SubobjectPreamble + $"ROCKS_CLASS({ownerSpecifiers})\nclass Owner : public rocks::Object {{\nROCKS_GENERATED_BODY()\npublic:\n" +
        "explicit Owner(rocks::Allocator& allocator) noexcept : parts(allocator) {}\nROCKS_PROPERTY() rocks::ObjectRefArray<Part> parts;\n" +
        "ROCKS_PROPERTY() rocks::ObjectRef<ScenePart> root;\nprotected:\n" + members + "\n};\n" + extra + "}\n";

    private static void RegisterSubobjects(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.SubobjectTable", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(SubobjectOwner(
                "ROCKS_PROPERTY(Subobject, AttachParent = \"body\") rocks::ObjectRef<ScenePart> arm;\n" +
                "ROCKS_PROPERTY(Subobject, Root) rocks::ObjectRef<ScenePart> body;\n" +
                "ROCKS_PROPERTY(Subobject) rocks::ObjectRef<Part> plain;\n" +
                "public:\nstatic rocks::Result<void> InitSubobjectDefaults(rocks::SubobjectDefaultsContext&) noexcept { return rocks::Result<void>::Success(); }",
                "ROCKS_CLASS()\nclass Sub : public Owner {\nROCKS_GENERATED_BODY()\npublic:\nusing Owner::Owner;\nprotected:\n" +
                "ROCKS_PROPERTY(Subobject, Name = \"Hand\") rocks::ObjectRef<ScenePart> hand_;\n};\n"));
            using (JsonDocument descriptor = JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor)))
            {
                Verify.Equal(10, descriptor.RootElement.GetProperty("schema").GetInt32(), "descriptor schema 10");
                JsonElement owner = DescriptorType(descriptor, "probe::Owner").GetProperty("subobjects");
                Verify.That(owner.GetProperty("list").ValueKind == JsonValueKind.String && owner.GetProperty("root").ValueKind == JsonValueKind.String &&
                    owner.GetProperty("attach").ValueKind == JsonValueKind.Null, "owner structure specifiers");
                string Entries(JsonElement subobjects) => string.Join(";", subobjects.GetProperty("entries").EnumerateArray().Select(entry =>
                    $"{entry.GetProperty("toolName").GetString()}:{entry.GetProperty("attachParent").GetInt32()}:{entry.GetProperty("root").GetBoolean()}"));
                // 자기 Root가 맨 앞, 나머지는 선언 순서다. arm은 body에, plain은 부착 지정자가 없는 타입이라 붙지 않는다.
                Verify.Equal("body:-1:True;arm:0:False;plain:-1:False", Entries(owner), "owner table");
                JsonElement sub = DescriptorType(descriptor, "probe::Sub").GetProperty("subobjects");
                Verify.Equal("body:-1:True;arm:0:False;plain:-1:False;Hand:0:False", Entries(sub), "derived table keeps the base prefix");
                Verify.Equal(owner.GetProperty("list").GetString(), sub.GetProperty("list").GetString(), "list specifier inherited");
                JsonElement scene = DescriptorType(descriptor, "probe::ScenePart").GetProperty("subobjects");
                Verify.That(scene.GetProperty("attach").ValueKind == JsonValueKind.String, "attach specifier");
            }

            string generated = Compact(probe.GeneratedText());
            Verify.That(generated.Contains("staticconstSubobjectSchemasubobjects[]", StringComparison.Ordinal), "schema table:\n" + generated);
            Verify.That(generated.Contains("static_cast<uint32>(SubobjectFlags::Root)", StringComparison.Ordinal), "root flag");
            Verify.That(generated.Contains("ops.initSubobjectDefaults=&::probe::Owner::InitSubobjectDefaults;", StringComparison.Ordinal), "owner hook");
            Verify.That(generated.Contains("Span<constRuntimeSubobjectOps>(subobjects,4)", StringComparison.Ordinal), "derived ops table");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated code compiles: " + compiled.StdOut + compiled.StdErr);
        });

        // 지정자 조합(RMC3001): Subobject 없는 Root·AttachParent, Root와 AttachParent 함께, 저장·통지 지정자와 함께, 잘못된 클래스 지정자 값.
        AddRejectGroup(context, "Meta.SubobjectSpecifiers", [
            SubobjectOwner("ROCKS_PROPERTY(Root) rocks::ObjectRef<ScenePart> body;"),
            SubobjectOwner("ROCKS_PROPERTY(AttachParent = \"x\") rocks::ObjectRef<ScenePart> body;"),
            SubobjectOwner("ROCKS_PROPERTY(Subobject, Root, AttachParent = \"x\") rocks::ObjectRef<ScenePart> body;"),
            SubobjectOwner("ROCKS_PROPERTY(Subobject, Transient) rocks::ObjectRef<ScenePart> body;"),
            SubobjectOwner("ROCKS_PROPERTY(Subobject, Notify) rocks::ObjectRef<ScenePart> body;"),
            SubobjectOwner("", ownerSpecifiers: "SubobjectList = \"not a name\"")],
            "RMC3001", "Subobject");
        // 소유·타입·상속 규칙(RMC3002).
        AddRejectGroup(context, "Meta.SubobjectRules", [
            // 소유 목록 지정자가 없다.
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::ObjectRef<ScenePart> body;", ownerSpecifiers: ""),
            // private 필드.
            SubobjectOwner("private:\nROCKS_PROPERTY(Subobject) rocks::ObjectRef<ScenePart> body;"),
            // ObjectRef가 아니다.
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::int32 body = 0;"),
            // 목록 원소 타입의 파생이 아니다.
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::ObjectRef<Owner> body;"),
            // Root 둘(상속 포함).
            SubobjectOwner("ROCKS_PROPERTY(Subobject, Root) rocks::ObjectRef<ScenePart> body;",
                "ROCKS_CLASS()\nclass Sub : public Owner {\nROCKS_GENERATED_BODY()\npublic:\nusing Owner::Owner;\nprotected:\n" +
                "ROCKS_PROPERTY(Subobject, Root) rocks::ObjectRef<ScenePart> hand;\n};\n"),
            // 없는 AttachParent.
            SubobjectOwner("ROCKS_PROPERTY(Subobject, AttachParent = \"missing\") rocks::ObjectRef<ScenePart> body;"),
            // 부착 지정자가 없는 타입을 AttachParent로.
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::ObjectRef<Part> plain;\nROCKS_PROPERTY(Subobject, AttachParent = \"plain\") rocks::ObjectRef<ScenePart> body;"),
            // 상속한 소유 목록 지정자를 다시 쓴다.
            SubobjectOwner("", "ROCKS_CLASS(SubobjectList = \"parts\")\nclass Sub : public Owner {\nROCKS_GENERATED_BODY()\npublic:\nusing Owner::Owner;\n};\n"),
            // 이름이 글자·숫자로 시작하지 않는다.
            SubobjectOwner("ROCKS_PROPERTY(Subobject, Name = \"_body\") rocks::ObjectRef<ScenePart> body;"),
            // 장치 예약어 이름.
            SubobjectOwner("ROCKS_PROPERTY(Subobject, Name = \"com0\") rocks::ObjectRef<ScenePart> body;"),
            // EditorOnly 타입.
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::ObjectRef<EdPart> body;").Replace("ROCKS_CLASS(SubobjectList",
                "ROCKS_CLASS(EditorOnly)\nclass EdPart : public Part {\nROCKS_GENERATED_BODY()\n};\nROCKS_CLASS(SubobjectList", StringComparison.Ordinal),
            // Abstract 타입.
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::ObjectRef<AbsPart> body;").Replace("ROCKS_CLASS(SubobjectList",
                "ROCKS_CLASS(Abstract)\nclass AbsPart : public Part {\nROCKS_GENERATED_BODY()\npublic:\nvirtual void Tick() noexcept = 0;\n};\nROCKS_CLASS(SubobjectList",
                StringComparison.Ordinal),
            // Root 타입이 root 필드 타입의 파생이 아니다.
            SubobjectOwner("ROCKS_PROPERTY(Subobject, Root) rocks::ObjectRef<Part> body;"),
            // SubobjectRoot 필드 자신을 Subobject로.
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::ObjectRef<ScenePart> body;",
                ownerSpecifiers: "SubobjectList = \"parts\", SubobjectRoot = \"body\""),
            // 부착 부모 타입이 부착 필드 타입(WeakObjectRef<ScenePart>)의 파생이 아니다.
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::ObjectRef<OtherPart> other;\nROCKS_PROPERTY(Subobject, AttachParent = \"other\") rocks::ObjectRef<ScenePart> body;")
                .Replace("ROCKS_CLASS(SubobjectList", "ROCKS_CLASS(SubobjectAttach = \"link\")\nclass OtherPart : public Part {\nROCKS_GENERATED_BODY()\npublic:\n" +
                    "ROCKS_PROPERTY() rocks::WeakObjectRef<OtherPart> link;\n};\nROCKS_CLASS(SubobjectList", StringComparison.Ordinal),
            // 소유 구조 지정자는 자기 필드만 가리킨다(base 필드 불가).
            SubobjectOwner("", "ROCKS_CLASS()\nclass ListBase : public rocks::Object {\nROCKS_GENERATED_BODY()\npublic:\n" +
                "explicit ListBase(rocks::Allocator& allocator) noexcept : items(allocator) {}\nROCKS_PROPERTY() rocks::ObjectRefArray<Part> items;\n};\n" +
                "ROCKS_CLASS(SubobjectList = \"items\")\nclass ListHolder : public ListBase {\nROCKS_GENERATED_BODY()\npublic:\nusing ListBase::ListBase;\n};\n"),
            // 소유 구조 지정자가 EditorOnly 필드를 가리킨다.
            SubobjectOwner(EditorDataOpen + "ROCKS_PROPERTY(EditorOnly) rocks::ObjectRef<ScenePart> edRoot;\n#endif",
                ownerSpecifiers: "SubobjectList = \"parts\", SubobjectRoot = \"edRoot\"")],
            "RMC3002", "ubobject");
        // hook 모양(RMC3003): 비정적, 매개변수 형식이 다름.
        AddRejectGroup(context, "Meta.SubobjectDefaultsHook", [
            SubobjectOwner("public:\nrocks::Result<void> InitSubobjectDefaults(rocks::SubobjectDefaultsContext&) noexcept { return rocks::Result<void>::Success(); }"),
            SubobjectOwner("public:\nstatic rocks::Result<void> InitSubobjectDefaults(rocks::int32) noexcept { return rocks::Result<void>::Success(); }")],
            "RMC3003", "InitSubobjectDefaults");
        // field로 등록된 이름을 Subobject로 바꾸면 ID를 조용히 새로 받지 않는다(RMC2100).
        AddRejectGroup(context, "Meta.SubobjectIdentityKind", [
            SubobjectOwner("ROCKS_PROPERTY(Subobject) rocks::ObjectRef<ScenePart> legacy;")],
            "RMC2100", "Subobject");
    }
}
