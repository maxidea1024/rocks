using System.Text.RegularExpressions;
using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

// 성질 표가 막는 모든 칸의 거부 시험이다(Docs/Design/AssetSystem.ko.md 14.1절 "지원하는 모든 조합", ReflectionExtensions.ko.md 1.3절).
// ValueKindTable.Rows를 순회해 원소 자리(Array·정적 배열·Map 키·Map 값·Set·Optional)마다 표가 ×인 (종류, 자리) 칸을 원본 선언으로
// 만들고 MetaCompiler가 칸마다 정해진 RMC 진단 하나로 거부하는지 본다(ExpectedRejection). 대부분은 철자 검사의 RMC2xxx이고, C 배열을
// 템플릿 원소로 쓰는 칸만 Clang 단계 RMC3000이다. 칸이 다른 이유(명세에 없는 이름의 RMC2100, 환경·입력 오류의 RMC1xxx, 엉뚱한 Clang
// 오류)로 거부되어 시험이 아무것도 확인하지 않은 채 통과하지 않도록, 거부 진단의 코드 집합이 기대 코드 하나와 같아야 한다. 대조 시험
// Meta.EveryValueKindSpellingIsAcceptedWhereAllowed는 같은 preamble·철자·필드 이름으로 표가 ○인 자리(클래스 필드와 허용 원소 자리
// 모두)를 생성에 성공시킨다 — preamble·명세·철자가 그 자체로 유효하다는 증거다. 허용 칸의 실제 왕복은 런타임 시험
// FixtureAsset_CombinationRoundTrip이, 허용 칸이 fixture에 빠짐없이 있는지는 FixtureAsset_CombinationsCoverPropertyTable이 본다.
// 필드·struct 멤버 자리는 Opaque 말고 모든 종류가 ○라 거부 칸이 없다. InstancedStruct 내용 열은 원본 선언이 아니라 실행 중 값이라
// (InstancedStruct<>::Emplace<S>의 static_assert와 런타임 CheckInstancedStructContent가 막는다) 여기서 만들지 않는다.
// ObjectReference의 Array 원소·Map 값은 Registry 컨테이너(ObjectRefArray·ObjectRefMap)로 ○이고 Array<ObjectRef>·
// OrderedMap<K, ObjectRef> 철자의 거부는 기존 시험(Meta.MapValuesHoldNoStrongReferences 등)이 본다.
internal static partial class MetaTests
{
    // 멤버 선언 하나와 Holder 생성자의 그 멤버 초기화식, ROCKS_PROPERTY 지정자다.
    private readonly record struct Member(string Declaration, string Initializer, string Specifiers = "");

    // 단일 값(정적 배열 아님) 멤버다. 초기화는 probe::Make가 Allocator를 받는 형식이면 allocator로, 아니면 기본값으로 만든다.
    private static Member Single(string declaration, string field) => new(declaration, $"{field}(probe::Make<decltype({field})>(allocator))");

    // 원소 자리 성질, 이름, 대조 시험의 필드 이름, 그 자리에 종류 철자 T를 놓는 멤버(필드 이름을 받는다)다.
    private static readonly (ValueKindProperty Property, string Name, string Field, Func<string, string, Member> Declare)[] ElementPositions =
    [
        (ValueKindProperty.ArrayElement, "array", "array", (type, field) => Single($"rocks::Array<{type}> {field};", field)),
        (ValueKindProperty.StaticArrayElement, "static array", "fixed", (type, field) => type.EndsWith(']')
            ? new($"rocks::int32 {field}[2]{type[type.IndexOf('[')..]};", field + "{}")
            : new($"{type} {field}[2];", $"{field}{{probe::Make<{type}>(allocator), probe::Make<{type}>(allocator)}}")),
        (ValueKindProperty.MapKey, "map key", "mapKey", (type, field) => Single($"rocks::OrderedMap<{type}, rocks::int32> {field};", field)),
        (ValueKindProperty.MapValue, "map value", "mapValue", (type, field) => Single($"rocks::OrderedMap<rocks::int32, {type}> {field};", field)),
        (ValueKindProperty.SetElement, "set element", "set", (type, field) => Single($"rocks::OrderedSet<{type}> {field};", field)),
        (ValueKindProperty.OptionalInner, "optional inner", "maybe", (type, field) => Single($"rocks::Optional<{type}> {field};", field))
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

    // 표가 ○인 원소 자리 가운데 Registry 컨테이너가 받는 칸(표 밖 규칙)의 대조 멤버다.
    private static readonly Dictionary<(string Kind, ValueKindProperty Property), Func<string, Member>> AllowedOverrides = new()
    {
        [("ObjectReference", ValueKindProperty.ArrayElement)] = field => Single($"rocks::ObjectRefArray<Target> {field};", field),
        [("ObjectReference", ValueKindProperty.MapValue)] = field => Single($"rocks::ObjectRefMap<rocks::int32, Target> {field};", field)
    };

    // probe 명세(MetaProbeIdentity)에 등록한 타입·필드 이름만 쓴다: probe::Tone·Target·Point·Holder와 Point::x, Holder의
    // cell(거부 칸·대조 시험의 클래스 필드 자리)과 원소 자리 필드(ElementPositions.Field). probe::Make는 Holder 생성자가 멤버를
    // 만드는 도우미다(Allocator를 받는 형식은 allocator로) — 생성자가 없으면 컨테이너 멤버 때문에 기본 생성자가 지워져 모든 칸이
    // 같은 RMC3000으로 거부된다.
    private const string CellPreamble =
        "#include <Meta/Annotations.h>\n#include <Core/Containers/Array.h>\n#include <Core/Containers/OrderedMap.h>\n" +
        "#include <Core/Containers/OrderedSet.h>\n#include <Core/Encoding/Sha256.h>\n#include <Core/Optional.h>\n#include <Core/Text/Name.h>\n" +
        "#include <Core/Text/String.h>\n#include <Localization/Text.h>\n#include <Object/AssetId.h>\n#include <Object/ClassRef.h>\n" +
        "#include <Object/InstancedStruct.h>\n#include <Object/Object.h>\n#include <Object/ObjectRefArray.h>\n#include <Object/ObjectRefMap.h>\n" +
        "#include <Object/ObjectRefs.h>\n#include <Object/SoftObjectRef.h>\n" +
        "namespace probe {\ntemplate <class T>\nT Make(rocks::Allocator& allocator) noexcept {\n" +
        "    if constexpr (requires { T(allocator); }) {\n        return T(allocator);\n    } else {\n        return T();\n    }\n}\n" +
        "ROCKS_ENUM()\nenum class Tone : rocks::uint8 { Low = 1, High = 2 };\n" +
        "ROCKS_CLASS()\nstruct Target : public rocks::Object {};\n" +
        "ROCKS_STRUCT()\nstruct Point { ROCKS_PROPERTY() rocks::int32 x = 0; };\n";

    private static string HolderSource(IReadOnlyList<Member> members) =>
        CellPreamble + "ROCKS_CLASS()\nstruct Holder : public rocks::Object {\n" +
        "    explicit Holder([[maybe_unused]] rocks::Allocator& allocator) noexcept : " + string.Join(", ", members.Select(member => member.Initializer)) + " {}\n" +
        string.Concat(members.Select(member => $"    ROCKS_PROPERTY({member.Specifiers}) " + member.Declaration + "\n")) + "};\n}\n";

    // 클래스 필드 자리의 멤버다(정적 배열 철자는 int32 cell[2]).
    private static Member FieldMember(string spelling) => spelling.EndsWith(']')
        ? new($"rocks::int32 cell{spelling[spelling.IndexOf('[')..]};", "cell{}")
        : Single($"{spelling} cell;", "cell");

    // 칸의 기대 거부 진단이다. C 배열(정적 배열 철자)을 Array·OrderedMap·OrderedSet·Optional의 템플릿 인자로 쓰면 C++ 형식 자체가
    // 컨테이너의 static_assert에 걸려 Clang 단계 RMC3000이다. 나머지는 철자 검사가 자리별 코드로 거부한다: Array 원소 RMC2304,
    // Map 키·값·Set 원소 RMC2305, Optional 내부 RMC2306, 정적 배열 원소 RMC2307(int32 x[2][2]처럼 C++로 유효한 다차원 배열 포함).
    internal static string ExpectedRejection(string kind, ValueKindProperty property)
    {
        if (kind == "StaticArray" && property != ValueKindProperty.StaticArrayElement)
        {
            return "RMC3000";
        }

        return property switch
        {
            ValueKindProperty.ArrayElement => "RMC2304",
            ValueKindProperty.MapKey or ValueKindProperty.MapValue or ValueKindProperty.SetElement => "RMC2305",
            ValueKindProperty.OptionalInner => "RMC2306",
            ValueKindProperty.StaticArrayElement => "RMC2307",
            _ => throw new ArgumentException("not an element position: " + property, nameof(property))
        };
    }

    // 표가 ×인 원소 칸 목록이다(종류 이름, 자리 성질, 자리 이름, 원본).
    internal static IReadOnlyList<(string Kind, ValueKindProperty Property, string Position, string Source)> RejectedElementCells()
    {
        List<(string, ValueKindProperty, string, string)> cells = [];
        foreach (ValueKindRow row in ValueKindTable.Rows)
        {
            if (!KindSpellings.TryGetValue(row.Name, out string? spelling))
            {
                continue;
            }

            foreach ((ValueKindProperty property, string position, _, Func<string, string, Member> declare) in ElementPositions)
            {
                if (ValueKindTable.Has(row.Name, property))
                {
                    continue;
                }

                cells.Add((row.Name, property, position, HolderSource([declare(spelling, "cell")])));
            }
        }

        return cells;
    }

    // 종류마다 표가 ○인 자리(클래스 필드 + 허용 원소 자리)를 모두 담은 대조 원본이다(종류 이름, 원본).
    internal static IReadOnlyList<(string Kind, string Source)> AllowedSpellingSources()
    {
        List<(string, string)> sources = [];
        foreach (ValueKindRow row in ValueKindTable.Rows)
        {
            if (!KindSpellings.TryGetValue(row.Name, out string? spelling))
            {
                continue;
            }

            List<Member> members = [FieldMember(spelling)];
            foreach ((ValueKindProperty property, _, string field, Func<string, string, Member> declare) in ElementPositions)
            {
                if (!ValueKindTable.Has(row.Name, property))
                {
                    continue;
                }

                members.Add(AllowedOverrides.TryGetValue((row.Name, property), out Func<string, Member>? spelled)
                    ? spelled(field) : declare(spelling, field));
            }

            sources.Add((row.Name, HolderSource(members)));
        }

        return sources;
    }

    // C 배열 원소를 받은 컨테이너의 static_assert 진단 문구다(Clang 단계 칸의 거부 사유).
    private const string CArrayElementReason = "static assertion failed";

    // stderr의 "error RMCnnnn" 코드 집합이다.
    private static SortedSet<string> ErrorCodes(string stderr) =>
        new(Regex.Matches(stderr, @"error (RMC\d{4})", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1)).Select(match => match.Groups[1].Value),
            StringComparer.Ordinal);

    private static void RegisterRejectedCells(MetaContext context)
    {
        // 표의 종류 행마다 철자가 있다(새 종류는 KindSpellings에 더해야 이 시험이 그 행의 거부 칸을 만든다).
        context.Suite.Add("Meta.EveryRejectedValueKindCellHasASpelling", () =>
        {
            foreach (ValueKindRow row in ValueKindTable.Rows)
            {
                Verify.That(row.Name == "Opaque" || KindSpellings.ContainsKey(row.Name), "value kind without a probe spelling: " + row.Name);
            }

            // 표가 쓰는 성질 비트는 모두 원소 자리(ElementPositions), 거부 칸이 없는 자리(클래스 필드·struct 멤버·InstancedStruct
            // 내용), 자리가 아닌 성질 중 하나다. 표에 새 자리 열이 생기면 여기서 실패한다 — ElementPositions에 그 자리를 더하라는 신호다.
            ValueKindProperty known = ValueKindProperty.Field | ValueKindProperty.StructMember | ValueKindProperty.InstancedStructContent |
                ValueKindProperty.AcceptsRequired | ValueKindProperty.HasTargetType | ValueKindProperty.RequiresTargetId |
                ValueKindProperty.Container | ValueKindProperty.WholeFieldValue;
            foreach ((ValueKindProperty property, _, _, _) in ElementPositions)
            {
                Verify.That((known & property) == 0, "element position listed twice: " + property);
                known |= property;
            }

            foreach (ValueKindRow row in ValueKindTable.Rows)
            {
                Verify.That((row.Properties & ~known) == 0, $"value kind {row.Name} uses an unclassified property: {row.Properties & ~known}");
            }
        });

        // 대조: 같은 preamble·명세·철자로 표가 ○인 자리를 모두 선언하면 생성에 성공한다(거부 시험의 거부가 칸 때문임을 보장).
        context.Suite.Add("Meta.EveryValueKindSpellingIsAcceptedWhereAllowed", async () =>
        {
            IReadOnlyList<(string Kind, string Source)> sources = AllowedSpellingSources();
            Verify.Equal(KindSpellings.Count, sources.Count, "every spelling has a control source");
            using Probe probe = new(context.Options, context.Environment, context.Generator, context.Compiler, MetaCompilerArguments.Module);
            foreach ((string kind, string source) in sources)
            {
                try
                {
                    await probe.GenerateAsync(source);
                }
                catch (Exception error)
                {
                    throw new InvalidOperationException($"{kind} was not accepted where the table allows it", error);
                }
            }
        });

        // 거부 칸마다 MetaCompiler가 위치·hint가 붙은 기대 RMC 진단 하나로 거부하고 이전 출력을 바꾸지 않는다. 진단 코드 집합이 기대
        // 코드 하나와 같아야 하므로 명세 누락(RMC2100)·입력 오류(RMC1xxx)·엉뚱한 Clang 오류(철자 검사 칸의 RMC3000)는 실패다.
        context.Suite.Add("Meta.EveryRejectedValueKindCellIsRejected", async () =>
        {
            IReadOnlyList<(string Kind, ValueKindProperty Property, string Position, string Source)> cells = RejectedElementCells();
            Verify.That(cells.Count > 30, "rejected cells enumerated: " + cells.Count);
            Verify.That(cells.Any(cell => ExpectedRejection(cell.Kind, cell.Property) == "RMC3000") &&
                cells.Any(cell => ExpectedRejection(cell.Kind, cell.Property) != "RMC3000"), "both rejection stages are exercised");
            using Probe probe = new(context.Options, context.Environment, context.Generator, context.Compiler, MetaCompilerArguments.Module);
            List<string> mismatches = [];
            foreach ((string kind, ValueKindProperty property, string position, string source) in cells)
            {
                string expected = ExpectedRejection(kind, property);
                try
                {
                    await probe.RejectAsync(source, "error " + expected, null!);
                }
                catch (Exception error)
                {
                    mismatches.Add($"{kind} as {position}: not rejected with {expected}: {error.Message}\n{probe.LastStdErr}");
                    continue;
                }

                SortedSet<string> codes = ErrorCodes(probe.LastStdErr);
                if (codes.Count != 1 || codes.Min != expected)
                {
                    mismatches.Add($"{kind} as {position}: expected only {expected}, got {string.Join(",", codes)}\n{probe.LastStdErr}");
                }
                else if (expected == "RMC3000" && !probe.LastStdErr.Contains(CArrayElementReason, StringComparison.Ordinal))
                {
                    // Clang 단계 칸은 컨테이너가 C 배열 원소를 거부한 static_assert여야 한다(다른 Clang 오류가 아니라).
                    mismatches.Add($"{kind} as {position}: RMC3000 without the container static_assert\n{probe.LastStdErr}");
                }
            }

            Verify.That(mismatches.Count == 0, string.Join("\n", mismatches));
        });
    }
}
