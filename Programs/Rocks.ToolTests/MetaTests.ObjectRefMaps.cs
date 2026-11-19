using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9f4 ObjectRefMap PROPERTY 시험이다(Docs/Design/ReflectionExtensions.ko.md 7.4·15절).
internal static partial class MetaTests
{
    private const string ObjectRefMapPreamble = "#include <Core/Containers/Array.h>\n#include <Core/Containers/OrderedMap.h>\n#include <Core/Optional.h>\n" +
        "#include <Core/Text/Name.h>\n#include <Object/ObjectRefArray.h>\n#include <Object/ObjectRefMap.h>\n#include <Object/StructRefArray.h>\n" + Preamble;

    // 필드 선언 하나를 담은 ObjectRefMap 시험 원본이다(Value 생성자가 allocator를 받는다 — 컨테이너 멤버 초기화는 시험이 보지 않는다).
    private static string ObjectRefMapDeclaration(string members) =>
        ObjectRefMapPreamble + "ROCKS_CLASS()\nstruct Target : public rocks::Object {};\n" +
        "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& allocator) noexcept;\n" + members + "\n};\n}\n";

    private static void RegisterObjectRefMaps(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        // ObjectRefMap 클래스 필드와 인라인 struct 멤버가 descriptor(value {Map}·element {ObjectReference, 대상}·key 키 형식·
        // fieldKind ObjectReference·Required)와 생성 코드(FieldKind::ObjectReference 스키마, 생성 Trace의 TraceMapField, 값 종류
        // ObjectReference의 경로 thunk, struct의 ContainsReferenceArray 형질)에 닿고 생성 헤더가 컴파일된다.
        suite.Add("Meta.ObjectRefMapFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = ObjectRefMapPreamble +
                "ROCKS_ENUM()\nenum class Slot : rocks::uint8 { Head = 1, Hand = 2 };\n" +
                "ROCKS_CLASS()\nstruct Target : public rocks::Object {};\n" +
                "ROCKS_STRUCT()\nstruct Bundle {\nexplicit Bundle(rocks::Allocator& allocator) noexcept : byName(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::ObjectRefMap<rocks::Name, Target> byName;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : bySlot(allocator), anything(allocator), required(allocator), bundle(allocator) {}\n" +
                "ROCKS_PROPERTY(MaxElements = 4) rocks::ObjectRefMap<Slot, Target> bySlot;\n" +
                "ROCKS_PROPERTY() rocks::ObjectRefMap<rocks::String> anything;\n" +
                "ROCKS_PROPERTY(Required) rocks::ObjectRefMap<rocks::uint32, Target> required;\n" +
                "ROCKS_PROPERTY() Bundle bundle;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement Field(string type, string name) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement bySlot = Field("probe::Value", "bySlot");
            Verify.Equal("ObjectReference", bySlot.GetProperty("fieldKind").GetString(), "object reference map field kind");
            Verify.Equal("Map", bySlot.GetProperty("value").GetProperty("kind").GetString(), "object reference map value kind");
            Verify.Equal("ObjectReference", bySlot.GetProperty("element").GetProperty("kind").GetString(), "object reference map value");
            Verify.Equal(Type("probe::Target").GetProperty("id").GetString(), bySlot.GetProperty("element").GetProperty("type").GetString(),
                "object reference map value target");
            Verify.Equal("Enum", bySlot.GetProperty("key").GetProperty("kind").GetString(), "object reference map key kind");
            Verify.Equal(4, bySlot.GetProperty("maxElements").GetInt32(), "object reference map maxElements");
            System.Text.Json.JsonElement anything = Field("probe::Value", "anything");
            Verify.Equal("Utf8String", anything.GetProperty("key").GetProperty("kind").GetString(), "string key");
            Verify.Equal("", anything.GetProperty("element").GetProperty("type").GetString(), "default target is Object");
            Verify.That(Field("probe::Value", "required").GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Required"),
                "required object reference map");
            Verify.Equal("ObjectReference", Field("probe::Bundle", "byName").GetProperty("fieldKind").GetString(), "struct member object reference map");
            Verify.That(Type("probe::Bundle").GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "ContainsReferenceArray"),
                "struct holding an ObjectRefMap is a registry container holder");
            string generated = Compact(probe.GeneratedText());
            Verify.Contains("FieldKind::ObjectReference", generated, "generated field kind");
            Verify.Contains("detail::TraceMapField(value.*(&::probe::Value::bySlot),visitor);", generated, "generated class trace visits the map");
            Verify.Contains("detail::TraceMapField(value.*(&::probe::Bundle::byName),visitor);", generated, "generated struct trace visits the map");
            Verify.Contains("PathClassField<::probe::Value,::rocks::ValueKind::ObjectReference>(cursor,&::probe::Value::bySlot)", generated,
                "object reference map path thunk targets the value kind");
            Verify.Contains("PathStructMember<::probe::Bundle,&::probe::Bundle::byName,ValueKind::ObjectReference>", generated, "struct member path thunk");
            Verify.Contains("staticconstexprboolContainsReferenceArray=true;", generated, "generated registry container trait");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated ObjectRefMap header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // 키 종류 오류는 Map과 같은 RMC2305다(실수·강한 참조·struct 키, cv·컨테이너 키).
        AddRejectGroup(context, "Meta.ObjectRefMapKeyKindsAreRestricted", [
            ObjectRefMapDeclaration("ROCKS_PROPERTY() rocks::ObjectRefMap<float, Target> byRatio;"),
            ObjectRefMapDeclaration("ROCKS_PROPERTY() rocks::ObjectRefMap<rocks::ObjectRef<Target>, Target> byObject;")],
            "RMC2305", "cannot be a");
        AddRejectGroup(context, "Meta.ObjectRefMapContainerKeysAreRejected", [
            ObjectRefMapDeclaration("ROCKS_PROPERTY() rocks::ObjectRefMap<rocks::Optional<rocks::int32>, Target> maybeKey;"),
            ObjectRefMapDeclaration("ROCKS_PROPERTY() rocks::ObjectRefMap<const rocks::int32, Target> constKey;")],
            "RMC2305", "type '");
        // 값 대상이 클래스가 아니면 ObjectRefArray<T>와 같은 RMC2003이다.
        AddRejectGroup(context, "Meta.ObjectRefMapTargetMustBeAClass", [
            ObjectRefMapDeclaration("ROCKS_PROPERTY() rocks::ObjectRefMap<rocks::int32, rocks::String> notClass;")],
            "RMC2003", "");
        // 원소·내부 위치의 ObjectRefMap은 그 컨테이너의 코드다: Array 원소는 RMC2304, Optional 내부는 RMC2306, Map 값은 RMC2305다.
        // ObjectRefMap은 이동할 수 없어 컨테이너의 nothrow 이동 static_assert로 Clang이 먼저 실패하고, 그 실패를 멤버 위치의 진단으로
        // 바꾼다(강한 참조 템플릿 — ClangAstParser.StrongReferenceTemplates).
        AddRejectGroup(context, "Meta.ObjectRefMapElementPositionsUseContainerCode", [
            ObjectRefMapDeclaration("ROCKS_PROPERTY() rocks::Array<rocks::ObjectRefMap<rocks::int32, Target>> maps;")],
            "RMC2304", "strong object reference");
        AddRejectGroup(context, "Meta.ObjectRefMapInsideOptionalUsesOptionalCode", [
            ObjectRefMapDeclaration("ROCKS_PROPERTY() rocks::Optional<rocks::ObjectRefMap<rocks::int32, Target>> maybeMap;")],
            "RMC2306", "strong object reference");
        AddRejectGroup(context, "Meta.ObjectRefMapInsideMapIsRejected", [
            ObjectRefMapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, rocks::ObjectRefMap<rocks::int32, Target>> nestedRefs;")],
            "RMC2305", "strong object reference");
        // StructRefArray 원소 struct는 ObjectRefMap을 담을 수 없다(원소 블록이 owner 저장소 밖 — RF 결정 15, RMC2304).
        AddRejectGroup(context, "Meta.ObjectRefMapCannotLiveInStructRefArrayElements", [
            ObjectRefMapPreamble + "ROCKS_CLASS()\nstruct Target : public rocks::Object {};\n" +
            "ROCKS_STRUCT()\nstruct Slot {\nexplicit Slot(rocks::Allocator& allocator) noexcept : byKey(allocator) {}\n" +
            "ROCKS_PROPERTY() rocks::ObjectRef<Target> material;\nROCKS_PROPERTY() rocks::ObjectRefMap<rocks::int32, Target> byKey;\n};\n" +
            "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& allocator) noexcept;\n" +
            "ROCKS_PROPERTY() rocks::StructRefArray<Slot> slots;\n};\n}\n"],
            "RMC2304", "ObjectRefMap members");
        // NATIVE 멤버는 ObjectRefMap을 담을 수 없다(추적되지 않는다 — RMC2205).
        AddRejectGroup(context, "Meta.NativeObjectRefMapIsRejected", [
            ObjectRefMapDeclaration("ROCKS_NATIVE() rocks::ObjectRefMap<rocks::int32, Target> cache;")],
            "RMC2205", "strong object reference");
    }
}
