using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

// 성질 표가 막는 모든 칸의 거부 시험이다(Docs/Design/AssetSystem.ko.md 14.1절 "지원하는 모든 조합", ReflectionExtensions.ko.md 1.3절).
// ValueKindTable.Rows를 순회해 원소 자리(Array·정적 배열·Map 키·Map 값·Set·Optional)마다 표가 ×인 (종류, 자리) 칸을 원본 선언으로
// 만들고 MetaCompiler가 RMC 진단(RMC2xxx, C 배열 원소는 Clang 단계 RMC3000)으로 거부하는지 본다. 허용 칸의 실제 왕복은 런타임 시험 FixtureAsset_CombinationRoundTrip이,
// 허용 칸이 fixture에 빠짐없이 있는지는 FixtureAsset_CombinationsCoverPropertyTable이 본다. 필드·struct 멤버 자리는 Opaque 말고
// 모든 종류가 ○라 거부 칸이 없다. InstancedStruct 내용 열은 원본 선언이 아니라 실행 중 값이라(InstancedStruct<>::Emplace<S>의
// static_assert와 런타임 CheckInstancedStructContent가 막는다) 여기서 만들지 않는다. ObjectReference의 Array 원소·Map 값은
// Registry 컨테이너(ObjectRefArray·ObjectRefMap)로 ○이고 Array<ObjectRef>·OrderedMap<K, ObjectRef> 철자의 거부는 기존 시험
// (Meta.MapValuesHoldNoStrongReferences 등)이 본다.
internal static partial class MetaTests
{
    // 원소 자리 성질과 그 자리에 종류 철자 T를 놓는 선언이다.
    private static readonly (ValueKindProperty Property, string Name, Func<string, string> Declare)[] ElementPositions =
    [
        (ValueKindProperty.ArrayElement, "array", type => $"rocks::Array<{type}> cell;"),
        (ValueKindProperty.StaticArrayElement, "static array", type => type.EndsWith(']') ? $"rocks::int32 cell[2]{type[type.IndexOf('[')..]};" : $"{type} cell[2];"),
        (ValueKindProperty.MapKey, "map key", type => $"rocks::OrderedMap<{type}, rocks::int32> cell;"),
        (ValueKindProperty.MapValue, "map value", type => $"rocks::OrderedMap<rocks::int32, {type}> cell;"),
        (ValueKindProperty.SetElement, "set element", type => $"rocks::OrderedSet<{type}> cell;"),
        (ValueKindProperty.OptionalInner, "optional inner", type => $"rocks::Optional<{type}> cell;")
    ];

    // 종류 행 이름 → 원본 철자다(표에 있고 원소 자리에 거부 칸이 있을 수 있는 종류). Opaque는 철자가 없다. 정적 배열은 철자 끝의
    // [N]을 원소 자리 선언이 다룬다.
    private static readonly Dictionary<string, string> KindSpellings = new(StringComparer.Ordinal)
    {
        ["Bool"] = "bool",
        ["UInt8"] = "rocks::uint8",
        ["UInt16"] = "rocks::uint16",
        ["UInt32"] = "rocks::uint32",
        ["UInt64"] = "rocks::uint64",
        ["Int8"] = "rocks::int8",
        ["Int16"] = "rocks::int16",
        ["Int32"] = "rocks::int32",
        ["Int64"] = "rocks::int64",
        ["Float32"] = "float",
        ["Float64"] = "double",
        ["Guid"] = "rocks::Guid",
        ["Utf8String"] = "rocks::String",
        ["Name"] = "rocks::Name",
        ["AssetId"] = "rocks::AssetId",
        ["Digest256"] = "rocks::Sha256Digest",
        ["Enum"] = "Tone",
        ["ObjectReference"] = "rocks::ObjectRef<Target>",
        ["WeakObjectReference"] = "rocks::WeakObjectRef<Target>",
        ["ClassReference"] = "rocks::ClassRef<Target>",
        ["SoftObjectReference"] = "rocks::SoftObjectRef<Target>",
        ["Struct"] = "Point",
        ["Array"] = "rocks::Array<rocks::int32>",
        ["Optional"] = "rocks::Optional<rocks::int32>",
        ["Map"] = "rocks::OrderedMap<rocks::int32, rocks::int32>",
        ["Set"] = "rocks::OrderedSet<rocks::int32>",
        ["StaticArray"] = "rocks::int32[2]",
        ["InstancedStruct"] = "rocks::InstancedStruct<>",
        ["Text"] = "rocks::Text"
    };

    private const string CellPreamble =
        "#include <Meta/Annotations.h>\n#include <Core/Containers/Array.h>\n#include <Core/Containers/OrderedMap.h>\n" +
        "#include <Core/Containers/OrderedSet.h>\n#include <Core/Encoding/Sha256.h>\n#include <Core/Optional.h>\n#include <Core/Text/Name.h>\n" +
        "#include <Core/Text/String.h>\n#include <Localization/Text.h>\n#include <Object/AssetId.h>\n#include <Object/ClassRef.h>\n" +
        "#include <Object/InstancedStruct.h>\n#include <Object/Object.h>\n#include <Object/ObjectRefs.h>\n#include <Object/SoftObjectRef.h>\n" +
        "namespace probe {\nROCKS_ENUM()\nenum class Tone : rocks::uint8 { Low = 1, High = 2 };\n" +
        "ROCKS_CLASS()\nstruct Target : public rocks::Object {};\n" +
        "ROCKS_STRUCT()\nstruct Point { ROCKS_PROPERTY() rocks::int32 x = 0; };\n";

    // 표가 ×인 원소 칸 목록이다(종류 이름, 자리 이름, 원본).
    internal static IReadOnlyList<(string Kind, string Position, string Source)> RejectedElementCells()
    {
        List<(string, string, string)> cells = [];
        foreach (ValueKindRow row in ValueKindTable.Rows)
        {
            if (!KindSpellings.TryGetValue(row.Name, out string? spelling))
            {
                continue;
            }

            foreach ((ValueKindProperty property, string position, Func<string, string> declare) in ElementPositions)
            {
                if (ValueKindTable.Has(row.Name, property))
                {
                    continue;
                }

                // ObjectReference의 Array 원소·Map 값은 Registry 컨테이너로 ○이다(표 밖 규칙 — 위 주석).
                string source = CellPreamble + "ROCKS_CLASS()\nstruct Holder : public rocks::Object {\n    ROCKS_PROPERTY() " + declare(spelling) +
                    "\n};\n}\n";
                cells.Add((row.Name, position, source));
            }
        }

        return cells;
    }

    private static void RegisterRejectedCells(MetaContext context)
    {
        // 표의 종류 행마다 철자가 있다(새 종류는 KindSpellings에 더해야 이 시험이 그 행의 거부 칸을 만든다).
        context.Suite.Add("Meta.EveryRejectedValueKindCellHasASpelling", () =>
        {
            foreach (ValueKindRow row in ValueKindTable.Rows)
            {
                Verify.That(row.Name == "Opaque" || KindSpellings.ContainsKey(row.Name), "value kind without a probe spelling: " + row.Name);
            }
        });

        // 거부 칸마다 MetaCompiler가 위치·hint가 붙은 RMC 진단으로 거부하고 이전 출력을 바꾸지 않는다. 대부분은 철자 검사의
        // RMC2xxx이고, 정적 배열(C 배열)을 템플릿 원소로 쓰는 칸은 C++ 형식 자체가 컨테이너의 static_assert에 걸려 Clang
        // 단계 RMC3000이다(어느 쪽이든 생성 출력은 바뀌지 않는다).
        context.Suite.Add("Meta.EveryRejectedValueKindCellIsRejected", async () =>
        {
            IReadOnlyList<(string Kind, string Position, string Source)> cells = RejectedElementCells();
            Verify.That(cells.Count > 30, "rejected cells enumerated: " + cells.Count);
            using Probe probe = new(context.Options, context.Environment, context.Generator, context.Compiler, MetaCompilerArguments.Module);
            foreach ((string kind, string position, string source) in cells)
            {
                try
                {
                    await probe.RejectAsync(source, "error RMC", null!);
                }
                catch (Exception error)
                {
                    throw new InvalidOperationException($"{kind} as {position} was not rejected", error);
                }
            }
        });
    }
}
