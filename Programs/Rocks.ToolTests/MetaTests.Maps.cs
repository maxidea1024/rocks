using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9f2 Map·Set PROPERTY 시험이다(Docs/Design/ReflectionExtensions.ko.md 7.2·15절).
internal static partial class MetaTests
{
    private const string MapPreamble = "#include <Core/Containers/Array.h>\n#include <Core/Containers/HashMap.h>\n#include <Core/Containers/OrderedMap.h>\n" +
        "#include <Core/Containers/OrderedSet.h>\n#include <Core/Optional.h>\n#include <Core/Text/Name.h>\n#include <Object/ClassRef.h>\n" +
        "#include <Object/SoftObjectRef.h>\n" + Preamble;

    // 필드 선언 하나를 담은 Map 시험 원본이다(Value 생성자가 allocator를 받는다 — 컨테이너 멤버 초기화는 시험이 보지 않는다).
    private static string MapDeclaration(string field) =>
        MapPreamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 x = 0;\n};\n" +
        "ROCKS_STRUCT()\nstruct Holder {\nROCKS_PROPERTY() rocks::ObjectRef<rocks::Object> target;\n};\n" +
        "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" + field + "\n};\n}\n";

    // Map·Set 클래스 필드와 struct 멤버가 descriptor(value {Map·Set}·element 값·원소 형식·key 키 형식·maxElements)와 생성 코드(스키마
    // {Map·Set, 0} + 값·원소 형식 + maxElements + 키 형식, 값·원소 종류의 경로 thunk — 리플렉션 B9f3, 키 enum native 키)에 닿고 생성 헤더가
    // 컴파일된다.
    private static void RegisterMaps(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.MapFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = MapPreamble +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { First = 1, Second = 2 };\n" +
                "ROCKS_STRUCT()\nstruct Leaf {\nexplicit Leaf(rocks::Allocator& allocator) noexcept : label(allocator), tags(allocator), named(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::String label;\nROCKS_PROPERTY() rocks::OrderedSet<rocks::Name> tags;\n" +
                "ROCKS_PROPERTY() rocks::OrderedMap<rocks::uint32, rocks::String> named;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : byMode(allocator), leaves(allocator), assets(allocator), kinds(allocator) {}\n" +
                "ROCKS_PROPERTY(MaxElements = 8) rocks::OrderedMap<Mode, rocks::String> byMode;\n" +
                "ROCKS_PROPERTY() rocks::OrderedMap<rocks::Name, Leaf> leaves;\n" +
                "ROCKS_PROPERTY(Required) rocks::OrderedSet<rocks::SoftObjectRef<>> assets;\n" +
                "ROCKS_PROPERTY() rocks::OrderedMap<rocks::ClassRef<Value>, rocks::uint32> kinds;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement Field(string type, string name) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement byMode = Field("probe::Value", "byMode");
            Verify.Equal("Map", byMode.GetProperty("value").GetProperty("kind").GetString(), "map field kind");
            Verify.That(!byMode.GetProperty("value").TryGetProperty("type", out _), "map value has no target type");
            Verify.Equal("Utf8String", byMode.GetProperty("element").GetProperty("kind").GetString(), "map value kind");
            Verify.Equal("Enum", byMode.GetProperty("key").GetProperty("kind").GetString(), "map key kind");
            Verify.Equal(Type("probe::Mode").GetProperty("id").GetString(), byMode.GetProperty("key").GetProperty("type").GetString(), "map key enum target");
            Verify.Equal(8, byMode.GetProperty("maxElements").GetInt32(), "map maxElements");
            System.Text.Json.JsonElement leaves = Field("probe::Value", "leaves");
            Verify.Equal("Struct", leaves.GetProperty("element").GetProperty("kind").GetString(), "map struct value");
            Verify.Equal("Name", leaves.GetProperty("key").GetProperty("kind").GetString(), "map name key");
            Verify.Equal(65536, leaves.GetProperty("maxElements").GetInt32(), "map default maxElements");
            System.Text.Json.JsonElement assets = Field("probe::Value", "assets");
            Verify.Equal("Set", assets.GetProperty("value").GetProperty("kind").GetString(), "set field kind");
            Verify.Equal("SoftObjectReference", assets.GetProperty("element").GetProperty("kind").GetString(), "set element kind");
            Verify.Equal(System.Text.Json.JsonValueKind.Null, assets.GetProperty("key").ValueKind, "set has no key");
            Verify.That(assets.GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Required"), "required set");
            Verify.Equal("ClassReference", Field("probe::Value", "kinds").GetProperty("key").GetProperty("kind").GetString(), "class key");
            Verify.Equal(Type("probe::Value").GetProperty("id").GetString(),
                Field("probe::Value", "kinds").GetProperty("key").GetProperty("type").GetString(), "class key base");
            Verify.Equal(System.Text.Json.JsonValueKind.Null, Field("probe::Leaf", "label").GetProperty("key").ValueKind, "non-map field key is null");
            Verify.Equal("Set", Field("probe::Leaf", "tags").GetProperty("value").GetProperty("kind").GetString(), "set struct member");
            string generated = Compact(probe.GeneratedText());
            Verify.Contains("{ValueKind::Map,Guid{}}", generated, "generated map schema");
            Verify.Contains("{ValueKind::Set,Guid{}}", generated, "generated set schema");
            Verify.Contains("NativeTypeKey<::probe::Mode>()}", generated, "map enum key native key");
            Verify.Contains("detail::StructFieldOps<::probe::Leaf,&::probe::Leaf::tags>(", generated, "set struct member ops");
            Verify.Contains("PathStructMember<::probe::Leaf,&::probe::Leaf::tags,ValueKind::Name>", generated, "set member path thunk targets the element kind");
            Verify.Contains("PathStructMember<::probe::Leaf,&::probe::Leaf::named,ValueKind::Utf8String>", generated, "map member path thunk targets the value kind");
            Verify.Contains("PathClassField<::probe::Value,::rocks::ValueKind::Struct>(cursor,&::probe::Value::leaves)", generated,
                "map class field path thunk targets the value kind");
            Verify.Contains("PathClassField<::probe::Value,::rocks::ValueKind::SoftObjectReference>(cursor,&::probe::Value::assets)", generated,
                "set class field path thunk targets the element kind");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated map header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // 키·원소 종류 오류는 RMC2305다: 실수·강한 참조·weak 참조·struct 키, 실수 Set 원소.
        AddRejectGroup(context, "Meta.MapKeyKindsAreRestricted", [
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<float, rocks::int32> byRatio;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::ObjectRef<Value>, rocks::int32> byObject;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::WeakObjectRef<Value>, rocks::int32> byWeak;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<Leaf, rocks::int32> byLeaf;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedSet<double> ratios;")],
            "RMC2305", "cannot be a");
        // 직접 중첩·Optional 위치는 RMC2305다(키·값·원소의 배열·Map·Set·Optional — 컨테이너 쪽 코드, 15절).
        AddRejectGroup(context, "Meta.MapNestingAndOptionalPositions", [
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, rocks::Array<rocks::int32>> lists;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, rocks::OrderedMap<rocks::int32, rocks::int32>> nested;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedSet<rocks::OrderedSet<rocks::int32>> sets;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, rocks::Optional<rocks::int32>> maybe;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedSet<rocks::Optional<rocks::int32>> maybeSet;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::Optional<rocks::int32>, rocks::int32> maybeKey;")],
            "RMC2305", "type '");
        // 강한 참조 값(ObjectRefMap — 리플렉션 B9f4)과 참조 struct 값은 RMC2305다.
        AddRejectGroup(context, "Meta.MapValuesHoldNoStrongReferences", [
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, rocks::ObjectRef<Value>> targets;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, Holder> holders;")],
            "RMC2305", "strong object reference");
        // 순서 없는 HashMap·HashSet PROPERTY는 저장할 수 없다(RMC2305).
        AddRejectGroup(context, "Meta.UnorderedContainersAreRejected", [
            MapDeclaration("ROCKS_PROPERTY() rocks::HashMap<rocks::int32, rocks::int32> unordered;")],
            "RMC2305", "unordered container");
        // 기본이 아닌 해시·같음 정책(MapRules.Split이 Clang 철자의 정책 인자를 본다)과 HashSet PROPERTY는 저장할 수 없다(RMC2305).
        AddRejectGroup(context, "Meta.MapPoliciesAndUnorderedSetsAreRejected", [
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, rocks::int32, rocks::Hash<rocks::int32>> custom;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::HashSet<rocks::int32> unorderedSet;").Replace("#include <Core/Containers/HashMap.h>\n",
                "#include <Core/Containers/HashMap.h>\n#include <Core/Containers/HashSet.h>\n", StringComparison.Ordinal)],
            "RMC2305", "cannot be reflected");
        // cv 키·값과 지원하지 않는 값 형식은 RMC2305다.
        AddRejectGroup(context, "Meta.MapCvKeysAndValuesAreRejected", [
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<const rocks::int32, rocks::int32> constKey;"),
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, const rocks::int32> constValue;")],
            "RMC2305", "is cv-qualified");
        AddRejectGroup(context, "Meta.MapUnsupportedValueTypesAreRejected", [
            MapDeclaration("ROCKS_PROPERTY() rocks::OrderedMap<rocks::int32, rocks::int32*> pointers;")],
            "RMC2305", "unsupported map value type");
        // 원소 위치의 Map·Set은 그 컨테이너의 코드다: Array 원소는 RMC2304, Optional 내부는 RMC2306이다.
        AddRejectGroup(context, "Meta.MapElementPositionsUseContainerCode", [
            MapDeclaration("ROCKS_PROPERTY() rocks::Array<rocks::OrderedSet<rocks::int32>> sets;")],
            "RMC2304", "is a map or set");
        AddRejectGroup(context, "Meta.MapInsideOptionalUsesOptionalCode", [
            MapDeclaration("ROCKS_PROPERTY() rocks::Optional<rocks::OrderedMap<rocks::int32, rocks::int32>> maybeMap;")],
            "RMC2306", "is a map or set");
        // Required는 값이 Soft·ClassRef인 Map과 그 원소의 Set에만 쓴다(키에는 Required가 없다 — RMC2202).
        AddRejectGroup(context, "Meta.MapRequiredFollowsTheValueKind", [
            MapDeclaration("ROCKS_PROPERTY(Required) rocks::OrderedMap<rocks::SoftObjectRef<>, rocks::int32> byAsset;"),
            MapDeclaration("ROCKS_PROPERTY(Required) rocks::OrderedSet<rocks::Name> tags;")],
            "RMC2202", "Required applies only");
    }
}
