using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

// 지정자 축의 거부 칸과 허용 대조 시험이다(Docs/Design/AssetSystem 14.1절 "지원하는 모든 조합", ReflectionExtensions.ko.md 1.3절).
// 성질 표(ValueKindTable.Rows)와 철자 표(KindSpellings)에서 칸을 기계적으로 만든다. 지정자 자리는 클래스 필드(probe::Holder::cell)와 struct
// 멤버(probe::Cell::cell) 둘이고, 같은 원본 생성기(SpecifierHolders)로 거부 칸과 허용 대조를 만든다.
// 거부 칸(Meta.EveryRejectedSpecifierCellIsRejected) — MetaCompiler가 칸마다 정한 진단 코드 하나로 거부해야 한다(RMC2100·RMC1xxx·엉뚱한
// Clang 오류로 거부되면 실패):
// - Required: AcceptsRequired가 ×인 모든 종류를 클래스 필드·struct 멤버와 표가 ○인 원소 자리(Array·정적 배열·Map 값·Set) 모두에 — RMC2202.
//   Optional 필드의 Required는 Optional 행의 필드 칸이다(Required를 받는 참조 종류는 Optional 내부가 될 수 없다). Map 키만 Required를 받는
//   종류인 Map(SoftObjectRef 키, int32 값) — RMC2202(Required는 Map 값에 걸린다 — 키에는 Required가 없다).
// - MaxElements: 가변 길이 컨테이너(Array·Map·Set — MaxElementsKinds)가 아닌 모든 종류(클래스 필드·struct 멤버) — RMC2202. Container가
//   ○여도 길이가 고정이거나 원소가 하나뿐인 Optional·정적 배열은 거부한다(MetaCompiler 진단 "MaxElements applies only to array, map
//   and set fields").
// - bitfield(폭 1, 클래스 필드·struct 멤버): 부호 없는 정수(UInt8·16·32·64)가 아닌 종류. C++가 bitfield를 허락하는 정수 계열(bool·부호
//   있는 정수·enum)은 철자 검사의 RMC2207, 그 밖은 C++ 자체가 거부하는 Clang 단계 RMC3000(사유 "bit-field")이다.
// 허용 대조(Meta.EverySpecifierIsAcceptedWhereAllowed) — 같은 생성기로 표가 ○인 칸을 만들면 생성에 성공해야 한다(거부 칸의 거부가 그
// 칸 때문임을 보장한다): AcceptsRequired가 ○인 종류의 Required(클래스 필드·struct 멤버와 표가 ○인 원소 자리 — Map 키·Optional 내부
// 제외), 가변 길이 컨테이너의 MaxElements, 부호 없는 정수 bitfield. 표에 Required가 바뀐 행이 생기면 두 시험의 칸이 함께
// 바뀌고, 철자가 없으면 Meta.EveryRejectedValueKindCellHasASpelling이 실패한다.
internal static partial class MetaTests
{
    // C++가 bitfield를 허락하는 정수 계열 종류다(나머지 종류의 bitfield 선언은 C++ 오류다).
    private static readonly HashSet<string> IntegralKinds = new(StringComparer.Ordinal)
    {
        "Bool", "UInt8", "UInt16", "UInt32", "UInt64", "Int8", "Int16", "Int32", "Int64", "Enum"
    };

    // MaxElements를 받는 종류다(가변 길이 컨테이너 — 리플렉션 필드 상한).
    private static readonly HashSet<string> MaxElementsKinds = new(StringComparer.Ordinal) { "Array", "Map", "Set" };

    // ROCKS_PROPERTY bitfield가 받는 종류다(부호 없는 정수 — 리플렉션 B9m).
    private static readonly HashSet<string> BitfieldKinds = new(StringComparer.Ordinal) { "UInt8", "UInt16", "UInt32", "UInt64" };

    // struct 멤버 자리의 원본이다(probe::Cell — 클래스 Holder와 같은 멤버·초기화식을 ROCKS_STRUCT에 둔다).
    private static string CellStructSource(IReadOnlyList<Member> members) =>
        CellPreamble + "ROCKS_STRUCT()\nstruct Cell {\n" +
        "    explicit Cell([[maybe_unused]] rocks::Allocator& allocator) noexcept : " + string.Join(", ", members.Select(member => member.Initializer)) + " {}\n" +
        string.Concat(members.Select(member => $"    ROCKS_PROPERTY({member.Specifiers}) " + member.Declaration + "\n")) + "};\n}\n";

    // 지정자 자리(이름, 원본 생성기)다. 거부 칸과 허용 대조가 같은 생성기를 쓴다.
    private static readonly (string Name, Func<IReadOnlyList<Member>, string> Source)[] SpecifierHolders =
    [
        ("class field", HolderSource),
        ("struct member", CellStructSource)
    ];

    // 폭 1 bitfield 멤버다.
    private static Member BitfieldMember(string spelling) => new($"{spelling} cell : 1;", "cell{}");

    // 지정자 거부 칸 목록이다(이름, 기대 코드, 원본).
    internal static IReadOnlyList<(string Name, string Code, string Source)> RejectedSpecifierCells()
    {
        List<(string, string, string)> cells = [];
        foreach (ValueKindRow row in ValueKindTable.Rows)
        {
            if (!KindSpellings.TryGetValue(row.Name, out string? spelling))
            {
                continue;
            }

            foreach ((string holder, Func<IReadOnlyList<Member>, string> source) in SpecifierHolders)
            {
                if (!ValueKindTable.Has(row.Name, ValueKindProperty.AcceptsRequired))
                {
                    cells.Add(($"{row.Name} Required {holder}", "RMC2202", source([FieldMember(spelling) with { Specifiers = "Required" }])));
                }

                if (!MaxElementsKinds.Contains(row.Name))
                {
                    cells.Add(($"{row.Name} MaxElements {holder}", "RMC2202", source([FieldMember(spelling) with { Specifiers = "MaxElements = 4" }])));
                }

                if (!BitfieldKinds.Contains(row.Name) && !spelling.EndsWith(']'))
                {
                    string code = IntegralKinds.Contains(row.Name) ? "RMC2207" : "RMC3000";
                    cells.Add(($"{row.Name} bitfield {holder}", code, source([BitfieldMember(spelling)])));
                }
            }

            if (ValueKindTable.Has(row.Name, ValueKindProperty.AcceptsRequired))
            {
                continue;
            }

            foreach ((ValueKindProperty property, string position, _, Func<string, string, Member> declare) in ElementPositions)
            {
                // 원소 자리가 ×인 칸은 철자 검사가 먼저 거부한다(거부 칸 시험). Map 키의 Required는 아래 따로, Optional은 필드 칸이 본다.
                if (!ValueKindTable.Has(row.Name, property) || property is ValueKindProperty.MapKey or ValueKindProperty.OptionalInner)
                {
                    continue;
                }

                cells.Add(($"{row.Name} Required {position}", "RMC2202", HolderSource([declare(spelling, "cell") with { Specifiers = "Required" }])));
            }
        }

        // Required를 받는 종류(SoftObjectRef)가 Map 키에만 있고 값은 받지 않는 종류다 — Required는 값에 걸린다.
        foreach ((string holder, Func<IReadOnlyList<Member>, string> source) in SpecifierHolders)
        {
            cells.Add(($"Map key Required {holder}", "RMC2202",
                source([Single("rocks::OrderedMap<rocks::SoftObjectRef<Target>, rocks::int32> cell;", "cell") with { Specifiers = "Required" }])));
        }

        return cells;
    }

    // 지정자 허용 대조 목록이다(이름, 원본). 칸 하나에 원본 하나다(어느 칸이 거부됐는지 바로 보이게).
    internal static IReadOnlyList<(string Name, string Source)> AllowedSpecifierCells()
    {
        List<(string, string)> cells = [];
        foreach (ValueKindRow row in ValueKindTable.Rows)
        {
            if (!KindSpellings.TryGetValue(row.Name, out string? spelling))
            {
                continue;
            }

            foreach ((string holder, Func<IReadOnlyList<Member>, string> source) in SpecifierHolders)
            {
                if (ValueKindTable.Has(row.Name, ValueKindProperty.AcceptsRequired))
                {
                    cells.Add(($"{row.Name} Required {holder}", source([FieldMember(spelling) with { Specifiers = "Required" }])));
                }

                if (MaxElementsKinds.Contains(row.Name))
                {
                    cells.Add(($"{row.Name} MaxElements {holder}", source([FieldMember(spelling) with { Specifiers = "MaxElements = 4" }])));
                }

                if (BitfieldKinds.Contains(row.Name))
                {
                    cells.Add(($"{row.Name} bitfield {holder}", source([BitfieldMember(spelling)])));
                }
            }

            if (!ValueKindTable.Has(row.Name, ValueKindProperty.AcceptsRequired))
            {
                continue;
            }

            foreach ((ValueKindProperty property, string position, string field, Func<string, string, Member> declare) in ElementPositions)
            {
                if (!ValueKindTable.Has(row.Name, property) || property is ValueKindProperty.MapKey or ValueKindProperty.OptionalInner)
                {
                    continue;
                }

                Member member = AllowedOverrides.TryGetValue((row.Name, property), out Func<string, Member>? spelled)
                    ? spelled(field) : declare(spelling, field);
                cells.Add(($"{row.Name} Required {position}", HolderSource([member with { Specifiers = "Required" }])));
            }
        }

        return cells;
    }

    private static void RegisterRejectedSpecifiers(MetaContext context)
    {
        // 칸마다 기대 진단 하나로 거부하고 이전 출력을 바꾸지 않는다(코드 집합이 기대 코드 하나와 같아야 한다). Clang 단계 칸은 사유가
        // bitfield 형식 오류여야 한다.
        context.Suite.Add("Meta.EveryRejectedSpecifierCellIsRejected", async () =>
        {
            IReadOnlyList<(string Name, string Code, string Source)> cells = RejectedSpecifierCells();
            Verify.That(cells.Count > 80, "rejected specifier cells enumerated: " + cells.Count);
            Verify.That(cells.Any(cell => cell.Name.EndsWith("struct member", StringComparison.Ordinal)), "struct member cells are enumerated");
            using Probe probe = new(context.Options, context.Environment, context.Generator, context.Compiler, MetaCompilerArguments.Module);
            List<string> mismatches = [];
            foreach ((string name, string code, string source) in cells)
            {
                try
                {
                    await probe.RejectAsync(source, "error " + code, null!);
                }
                catch (Exception error)
                {
                    mismatches.Add($"{name}: not rejected with {code}: {error.Message}\n{probe.LastStdErr}");
                    continue;
                }

                SortedSet<string> codes = ErrorCodes(probe.LastStdErr);
                if (codes.Count != 1 || codes.Min != code)
                {
                    mismatches.Add($"{name}: expected only {code}, got {string.Join(",", codes)}\n{probe.LastStdErr}");
                }
                else if (code == "RMC3000" && !probe.LastStdErr.Contains("bit-field", StringComparison.Ordinal))
                {
                    mismatches.Add($"{name}: RMC3000 without a bit-field diagnostic\n{probe.LastStdErr}");
                }
            }

            Verify.That(mismatches.Count == 0, string.Join("\n", mismatches));
        });

        // 대조: 같은 생성기로 표가 ○인 지정자 칸을 만들면 생성에 성공한다.
        context.Suite.Add("Meta.EverySpecifierIsAcceptedWhereAllowed", async () =>
        {
            IReadOnlyList<(string Name, string Source)> cells = AllowedSpecifierCells();
            Verify.That(cells.Any(cell => cell.Name.Contains("Required", StringComparison.Ordinal)) &&
                cells.Any(cell => cell.Name.Contains("MaxElements", StringComparison.Ordinal)) &&
                cells.Any(cell => cell.Name.Contains("bitfield", StringComparison.Ordinal)), "every specifier has allowed cells: " + cells.Count);
            using Probe probe = new(context.Options, context.Environment, context.Generator, context.Compiler, MetaCompilerArguments.Module);
            List<string> rejected = [];
            foreach ((string name, string source) in cells)
            {
                try
                {
                    await probe.GenerateAsync(source);
                }
                catch (Exception error)
                {
                    rejected.Add($"{name}: not accepted: {error.Message}\n{probe.LastStdErr}");
                }
            }

            Verify.That(rejected.Count == 0, string.Join("\n", rejected));
        });
    }
}
