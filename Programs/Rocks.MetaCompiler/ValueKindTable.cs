using System.Globalization;
using System.Numerics;
using System.Text;

namespace Rocks.MetaCompiler;

// 값 종류의 성질이다(Docs/Design/ReflectionExtensions.ko.md 1.3절 종류 성질 표). 비트와 뜻은 런타임
// rocks::ValueKindProperty(Meta/TypeSchema.h)와 같다(대조 헤더가 비트 값도 대조한다). 위치 성질은 그 자리에 그 종류를 둘 수
// 있는지이고, ObjectReference의 Registry 컨테이너(ObjectRefArray·ObjectRefMap)와 참조 struct 제한은 표 밖의 규칙이다.
// Required·대상 타입 성질은 필드의 대상 종류(FieldModel.TargetKind — 배열이면 원소 종류)로 판정한다.
[Flags]
internal enum ValueKindProperty : uint
{
    None = 0,

    // 클래스 필드
    Field = 1u << 0,

    // 리플렉션 struct 멤버
    StructMember = 1u << 1,

    // Array 계열(Array·ObjectRefArray·StructRefArray) 원소
    ArrayElement = 1u << 2,

    // 정적 배열(T x[N]) 원소
    StaticArrayElement = 1u << 3,

    // OrderedMap 키
    MapKey = 1u << 4,

    // OrderedMap 값
    MapValue = 1u << 5,

    // OrderedSet 원소
    SetElement = 1u << 6,

    // Optional<T>의 T
    OptionalInner = 1u << 7,

    // InstancedStruct가 담는 값
    InstancedStructContent = 1u << 8,

    // Required 지정자(그 값 또는 원소마다 non-null)
    AcceptsRequired = 1u << 9,

    // ValueType::type이 대상 타입 ID다(descriptor value·element의 type 키)
    HasTargetType = 1u << 10,

    // 대상 ID가 비어 있으면 안 된다(참조의 빈 ID는 Object다)
    RequiresTargetId = 1u << 11,

    // 원소 형식(FieldSchema::element)을 갖는 컨테이너다
    Container = 1u << 12,

    // FieldValue로 통째 읽고 쓴다(아니면 aggregate)
    WholeFieldValue = 1u << 13
}

// C# 표의 한 행이다: 런타임 ValueKind 열거자 이름과 값, 성질이다.
internal sealed record ValueKindRow(string Name, uint Value, ValueKindProperty Properties);

// MetaCompiler의 값 종류 성질 표다(Docs/Design/ReflectionExtensions.ko.md 1.3절). 런타임 표(Meta/TypeSchema.h
// ValueKindProperties)의 C# 사본이다. 쓰는 곳: 원본 선언의 종류 자리 확인(SchemaValidator.Validate — 철자 해석이 정한 값 종류가
// 클래스 필드·struct 멤버·Array 원소 자리에 허용되는지), Required·대상 타입·대상 ID 판정(FieldModel·SchemaValidator·
// CompilerDriver·CppEmitter), descriptor 읽기의 종류 검사(ModuleDescriptor). C++ 철자 → 값 종류 대응은 표가 아니라
// SchemaValidator의 철자 해석이 맡는다(해석이 만드는 종류 집합과 표의 Array 원소 열이 같은지는 ToolTests가 본다). 런타임 표와의
// 일치는 ParityHeader가 낸 대조 헤더를 ToolTests가 Clang으로 컴파일해 칸마다 양방향으로 확인한다(생성 헤더마다 비용이 들지
// 않는다).
internal static class ValueKindTable
{
    // 단일 값의 자리 넷(클래스 필드·struct 멤버·Array 원소·정적 배열 원소)이다.
    private const ValueKindProperty Placed =
        ValueKindProperty.Field | ValueKindProperty.StructMember | ValueKindProperty.ArrayElement | ValueKindProperty.StaticArrayElement;

    // Map 키·Set 원소·Optional 내부까지 되는 단일 값이다.
    private const ValueKindProperty Keyed = Placed | ValueKindProperty.MapKey | ValueKindProperty.MapValue | ValueKindProperty.SetElement |
        ValueKindProperty.OptionalInner | ValueKindProperty.WholeFieldValue;

    // 런타임 ValueKind 열거자 순서(값 = 위치)다. 새 종류는 런타임과 같이 끝에 붙인다.
    public static IReadOnlyList<ValueKindRow> Rows { get; } =
    [
        new("Opaque", 0, ValueKindProperty.None),
        new("Bool", 1, Keyed),
        new("UInt8", 2, Keyed),
        new("UInt16", 3, Keyed),
        new("UInt32", 4, Keyed),
        new("UInt64", 5, Keyed),
        new("Int32", 6, Keyed),
        new("Int64", 7, Keyed),
        new("Float32", 8, Placed | ValueKindProperty.MapValue | ValueKindProperty.OptionalInner | ValueKindProperty.WholeFieldValue),
        new("Float64", 9, Placed | ValueKindProperty.MapValue | ValueKindProperty.OptionalInner | ValueKindProperty.WholeFieldValue),
        new("Guid", 10, Keyed),
        new("Utf8String", 11, Keyed),
        new("ObjectReference", 12, Placed | ValueKindProperty.MapValue | ValueKindProperty.WholeFieldValue | ValueKindProperty.AcceptsRequired |
            ValueKindProperty.HasTargetType),
        new("Enum", 13, Keyed | ValueKindProperty.HasTargetType | ValueKindProperty.RequiresTargetId),
        new("Struct", 14, Placed | ValueKindProperty.MapValue | ValueKindProperty.OptionalInner | ValueKindProperty.InstancedStructContent |
            ValueKindProperty.HasTargetType | ValueKindProperty.RequiresTargetId),
        new("Array", 15, ValueKindProperty.Field | ValueKindProperty.StructMember | ValueKindProperty.Container),
        new("AssetId", 16, Keyed),
        new("SoftObjectReference", 17, Placed | ValueKindProperty.MapKey | ValueKindProperty.MapValue | ValueKindProperty.SetElement |
            ValueKindProperty.WholeFieldValue | ValueKindProperty.AcceptsRequired | ValueKindProperty.HasTargetType),
        new("Digest256", 18, Keyed),
        new("Int8", 19, Keyed),
        new("Int16", 20, Keyed),
        new("Name", 21, Keyed),
        new("WeakObjectReference", 22, Placed | ValueKindProperty.MapValue | ValueKindProperty.WholeFieldValue | ValueKindProperty.HasTargetType)
    ];

    // 성질과 같은 뜻의 런타임 질의 함수(rocks:: 이름공간)다. 대조 헤더가 칸마다 이 함수를 부른다.
    public static IReadOnlyList<(ValueKindProperty Property, string Function)> Queries { get; } =
    [
        (ValueKindProperty.Field, "ValueKindCanBeField"),
        (ValueKindProperty.StructMember, "ValueKindCanBeStructMember"),
        (ValueKindProperty.ArrayElement, "ValueKindCanBeArrayElement"),
        (ValueKindProperty.StaticArrayElement, "ValueKindCanBeStaticArrayElement"),
        (ValueKindProperty.MapKey, "ValueKindCanBeMapKey"),
        (ValueKindProperty.MapValue, "ValueKindCanBeMapValue"),
        (ValueKindProperty.SetElement, "ValueKindCanBeSetElement"),
        (ValueKindProperty.OptionalInner, "ValueKindCanBeOptionalInner"),
        (ValueKindProperty.InstancedStructContent, "ValueKindCanBeInstancedStructContent"),
        (ValueKindProperty.AcceptsRequired, "ValueKindAcceptsRequired"),
        (ValueKindProperty.HasTargetType, "ValueKindHasTargetType"),
        (ValueKindProperty.RequiresTargetId, "ValueKindRequiresTargetId"),
        (ValueKindProperty.Container, "ValueKindIsContainer"),
        (ValueKindProperty.WholeFieldValue, "ValueKindIsWholeFieldValue")
    ];

    private static readonly Dictionary<string, ValueKindProperty> ByName =
        Rows.ToDictionary(row => row.Name, row => row.Properties, StringComparer.Ordinal);

    // kind(ValueKind 이름)가 성질 property 하나를 가졌는지다. 표에 없는 이름은 어느 성질도 없다(descriptor의 모르는 종류는 거부).
    // property는 성질 하나여야 한다(None이나 여러 비트의 OR면 ArgumentException — 런타임 HasValueKindProperty와 같은 계약).
    public static bool Has(string kind, ValueKindProperty property)
    {
        if (BitOperations.PopCount((uint)property) != 1)
        {
            throw new ArgumentException($"expected exactly one value kind property, got {property}", nameof(property));
        }

        return ByName.TryGetValue(kind, out ValueKindProperty properties) && (properties & property) != 0;
    }

    // rows(보통 Rows — 시험은 칸을 바꾼 표를 넣는다)의 대조 헤더 텍스트다. 성질마다 런타임 ValueKindProperty 비트 값이 C# 값과
    // 같고, 런타임 ValueKindCount가 행 수와 같고, 행마다 열거자 값이 같으며, 모든 성질 칸에서 런타임 질의 결과가 C# 칸과 같아야
    // 컴파일된다(static_assert — C#만 허용하는 칸과 런타임만 허용하는 칸을 모두 잡는다). 실패 메시지에 종류·성질 이름이 들어간다.
    // 출력은 결정적이고 줄바꿈은 LF다.
    public static string ParityHeader(IReadOnlyList<ValueKindRow> rows)
    {
        StringBuilder text = new();
        text.Append("// 생성 파일: Rocks.MetaCompiler ValueKindTable이 C# 값 종류 성질 표로 만든 런타임 표 대조 헤더다\n");
        text.Append("// (Docs/Design/ReflectionExtensions.ko.md 1.3절). 고치지 말고 C#·런타임 표를 함께 고친다.\n");
        text.Append("#pragma once\n\n#include <Meta/TypeSchema.h>\n\n");
        foreach ((ValueKindProperty property, _) in Queries)
        {
            text.Append(CultureInfo.InvariantCulture,
                $"static_assert(static_cast<::rocks::uint32>(::rocks::ValueKindProperty::{property}) == {(uint)property}u, \"ValueKindTable: ValueKindProperty::{property} = {(uint)property}\");\n");
        }

        text.Append(CultureInfo.InvariantCulture,
            $"static_assert(::rocks::ValueKindCount == {rows.Count}u, \"ValueKindTable: C# table has {rows.Count} value kinds\");\n");
        foreach (ValueKindRow row in rows)
        {
            text.Append(CultureInfo.InvariantCulture,
                $"static_assert(static_cast<::rocks::uint32>(::rocks::ValueKind::{row.Name}) == {row.Value}u, \"ValueKindTable: {row.Name} = {row.Value}\");\n");
            foreach ((ValueKindProperty property, string function) in Queries)
            {
                string expected = (row.Properties & property) == property ? "true" : "false";
                text.Append(CultureInfo.InvariantCulture,
                    $"static_assert(::rocks::{function}(::rocks::ValueKind::{row.Name}) == {expected}, \"ValueKindTable: {row.Name} {property} = {expected}\");\n");
            }
        }

        return text.ToString();
    }
}
