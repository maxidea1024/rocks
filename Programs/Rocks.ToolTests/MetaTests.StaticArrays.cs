using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 리플렉션 B9g 정적 배열 PROPERTY 시험이다(Docs/Design/ReflectionExtensions.ko.md 8·15절).
internal static partial class MetaTests
{
    private const string StaticArrayPreamble = "#include <Core/Containers/Array.h>\n#include <Core/Containers/OrderedSet.h>\n#include <Core/Optional.h>\n" +
        "#include <Core/Text/Name.h>\n#include <Object/ClassRef.h>\n#include <Object/ObjectRefArray.h>\n#include <Object/SoftObjectRef.h>\n" + Preamble;

    // 필드 선언 하나를 담은 정적 배열 시험 원본이다(Bundle은 Registry 컨테이너를 담은 struct — 원소로 쓰면 RMC2307).
    private static string StaticArrayDeclaration(string field, string constructor = "") =>
        StaticArrayPreamble + "ROCKS_STRUCT()\nstruct Bundle {\nexplicit Bundle(rocks::Allocator& allocator) noexcept : refs(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::ObjectRefArray<rocks::Object> refs;\n};\n" +
        "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" + constructor + field + "\n};\n}\n";

    // 정적 배열 클래스 필드와 struct 멤버가 descriptor(value {StaticArray}·element 원소 형식·maxElements = N·ObjectRef 원소의 fieldKind)와
    // 생성 코드(스키마 {StaticArray, 0} + 원소 형식 + N, 원소 종류의 경로 thunk, TraceArrayField)에 닿고 생성 헤더가 컴파일된다. 원소
    // 종류는 B9g가 정적 배열 원소로 여는 Name·WeakObjectRef·ClassRef·Int8·Int16을 포함한다.
    private static void RegisterStaticArrays(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.StaticArrayFieldsReachDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = StaticArrayPreamble +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { First = 1, Second = 2 };\n" +
                "struct Value;\n" +
                "ROCKS_STRUCT()\nstruct Slot {\nROCKS_PROPERTY() rocks::ObjectRef<Value> target;\nROCKS_PROPERTY() rocks::uint16 codes[3] = {};\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "ROCKS_PROPERTY() rocks::int32 values[4] = {};\n" +
                "ROCKS_PROPERTY(Required) rocks::ObjectRef<Value> refs[2];\n" +
                "ROCKS_PROPERTY() Mode modes[2] = {Mode::First, Mode::First};\n" +
                "ROCKS_PROPERTY() Slot slots[2];\n" +
                "ROCKS_PROPERTY() rocks::Name names[2];\n" +
                "ROCKS_PROPERTY() rocks::WeakObjectRef<Value> watchers[2];\n" +
                "ROCKS_PROPERTY() rocks::ClassRef<Value> kinds[2];\n" +
                "ROCKS_PROPERTY() rocks::int8 smalls[2] = {};\n" +
                "ROCKS_PROPERTY() rocks::int16 shorts[2] = {};\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement Field(string type, string name) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement values = Field("probe::Value", "values");
            Verify.Equal("StaticArray", values.GetProperty("value").GetProperty("kind").GetString(), "static array field kind");
            Verify.That(!values.GetProperty("value").TryGetProperty("type", out _), "static array value has no target type");
            Verify.Equal("Int32", values.GetProperty("element").GetProperty("kind").GetString(), "static array element kind");
            Verify.Equal(4, values.GetProperty("maxElements").GetInt32(), "static array maxElements is N");
            Verify.Equal("Value", values.GetProperty("fieldKind").GetString(), "value element field kind");
            System.Text.Json.JsonElement refs = Field("probe::Value", "refs");
            Verify.Equal("ObjectReference", refs.GetProperty("fieldKind").GetString(), "object reference element field kind");
            Verify.Equal(Type("probe::Value").GetProperty("id").GetString(), refs.GetProperty("element").GetProperty("type").GetString(), "reference element target");
            Verify.That(refs.GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Required"), "required static array");
            Verify.Equal(Type("probe::Mode").GetProperty("id").GetString(), Field("probe::Value", "modes").GetProperty("element").GetProperty("type").GetString(),
                "enum element target");
            Verify.Equal(3, Field("probe::Slot", "codes").GetProperty("maxElements").GetInt32(), "struct member static array");
            Verify.That(Type("probe::Slot").GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "ContainsStrongReference"), "reference struct");
            foreach ((string name, string kind) in new[] { ("names", "Name"), ("watchers", "WeakObjectReference"), ("kinds", "ClassReference"), ("smalls", "Int8"), ("shorts", "Int16") })
            {
                Verify.Equal(kind, Field("probe::Value", name).GetProperty("element").GetProperty("kind").GetString(), name + " element kind");
            }

            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.Contains("{ValueKind::StaticArray,Guid{}}", generated, "generated static array schema");
            Verify.Contains("{ValueKind::Int32,Guid{}},4u}", generated, "generated element and N");
            Verify.Contains("detail::TraceArrayField(value.*(&::probe::Value::refs),visitor);", generated, "class trace visits reference elements");
            Verify.Contains("detail::TraceArrayField(value.*(&::probe::Value::slots),visitor);", generated, "class trace visits reference struct elements");
            Verify.Contains("PathClassField<::probe::Value,::rocks::ValueKind::ObjectReference>(cursor,&::probe::Value::refs)", generated,
                "static array path thunk targets the element kind");
            Verify.Contains("PathStructMember<::probe::Slot,&::probe::Slot::codes,ValueKind::UInt16>", generated, "struct member static array path thunk");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated static array header compiles: " + compiled.StdOut + compiled.StdErr);
        });
        // C 배열 ROCKS_NATIVE 멤버(`Leaf cache[2][3]`)의 원소 struct도 생성 코드가 형질로 한 번 더 확인한다(StaticArrayProbe가 모든 차원을 벗긴
        // 원소를 준다): 생성 헤더의 Leaf 형질 ContainsStrongReference를 참으로 바꾸면 컴파일이 실패한다(AST 1차 검사 RMC2205를 지난 뒤의 2차 확인).
        suite.Add("Meta.NativeStaticArrayElementStructTraitIsChecked", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(StaticArrayPreamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 x = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_NATIVE() Leaf cache[2][3];\n};\n}\n");
            string original = File.ReadAllText(probe.Output);
            Verify.Contains(Compact("static_assert(!GeneratedType<::probe::Leaf>::ContainsStrongReference"), Compact(original),
                "native array element struct trait check");
            ProcessResult control = await probe.CompileGeneratedAsync();
            Verify.Equal(0, control.ExitCode, "generated header compiles: " + control.StdOut + control.StdErr);
            const string from = "static constexpr bool ContainsStrongReference = false;";
            int at = original.IndexOf(from, original.IndexOf("struct GeneratedType<::probe::Leaf>", StringComparison.Ordinal), StringComparison.Ordinal);
            Verify.That(at >= 0, "generated Leaf trait missing");
            File.WriteAllText(probe.Output, original[..at] + "static constexpr bool ContainsStrongReference = true;" + original[(at + from.Length)..]);
            ProcessResult rejected = await probe.CompileGeneratedAsync();
            Verify.That(rejected.ExitCode != 0, "tampered native array element trait compiled");
            Verify.Contains("holds reference struct", rejected.StdOut + rejected.StdErr, "native array element trait static_assert diagnostic");
        });
        // 다차원·N 범위 밖·원소 위치의 Optional·컨테이너·cv·지원하지 않는 원소·Registry 컨테이너를 담은 원소 struct는 RMC2307이다(Optional
        // 원소는 B9e에서 옮긴 시험).
        AddRejectGroup(context, "Meta.StaticArrayShapesAreRestricted", [
            StaticArrayDeclaration("ROCKS_PROPERTY() rocks::int32 grid[2][3] = {};"),
            StaticArrayDeclaration("ROCKS_PROPERTY() rocks::uint8 huge[65536] = {};"),
            StaticArrayDeclaration("ROCKS_PROPERTY() rocks::Optional<rocks::int32> maybe[2];"),
            StaticArrayDeclaration("ROCKS_PROPERTY() rocks::Array<rocks::int32> lists[2];"),
            StaticArrayDeclaration("ROCKS_PROPERTY() rocks::OrderedSet<rocks::int32> sets[2];"),
            StaticArrayDeclaration("ROCKS_PROPERTY() const rocks::int32 fixed[2] = {};"),
            StaticArrayDeclaration("ROCKS_PROPERTY() rocks::int32* pointers[2] = {};"),
            StaticArrayDeclaration("ROCKS_PROPERTY() Bundle bundles[1];",
                "explicit Value(rocks::Allocator& allocator) noexcept : bundles{Bundle(allocator)} {}\n")],
            "RMC2307", "static array");
        // MaxElements는 정적 배열에 쓸 수 없고(RMC2202 — 원소 수는 N) Required는 참조·Soft·ClassRef 원소만 받는다(weak·값 원소는 RMC2202).
        AddRejectGroup(context, "Meta.StaticArraySpecifiersFollowElementKind", [
            StaticArrayDeclaration("ROCKS_PROPERTY(MaxElements = 2) rocks::int32 values[2] = {};"),
            StaticArrayDeclaration("ROCKS_PROPERTY(Required) rocks::int32 values[2] = {};"),
            StaticArrayDeclaration("ROCKS_PROPERTY(Required) rocks::WeakObjectRef<Value> watchers[2];")],
            "RMC2202", "applies only to");
    }
}
