using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

// 지정자 축의 거부 칸 시험이다(Docs/Design/AssetSystem.ko.md 14.1절 "지원하는 모든 조합", ReflectionExtensions.ko.md 1.3절).
// 성질 표(ValueKindTable.Rows)와 철자 표(KindSpellings)에서 칸을 기계적으로 만들어 MetaCompiler가 칸마다 정한 진단 코드 하나로
// 거부하는지 본다(RMC2100·RMC1xxx·엉뚱한 Clang 오류로 거부되면 실패):
// - Required: AcceptsRequired가 ×인 모든 종류를 클래스 필드와 표가 ○인 원소 자리(Array·정적 배열·Map 값·Set) 모두에 — RMC2202.
//   Optional 필드의 Required는 Optional 행의 필드 칸이다(Required를 받는 참조 종류는 Optional 내부가 될 수 없다). Map 키만 Required를 받는 종류인 Map(SoftObjectRef 키, int32 값) — RMC2202
//   (Required는 Map 값에 걸린다 — 키에는 Required가 없다).
// - MaxElements: Container가 ×인 모든 종류 — RMC2202.
// - bitfield(폭 1): 부호 없는 정수(UInt8·16·32·64)가 아닌 종류. C++가 bitfield를 허락하는 정수 계열(bool·부호 있는 정수·enum)은
//   철자 검사의 RMC2207, 그 밖은 C++ 자체가 거부하는 Clang 단계 RMC3000(사유 "bit-field")이다.
// 허용 쪽 대조(Required를 받는 종류·컨테이너의 MaxElements·부호 없는 정수 bitfield)는 기존 시험(Meta.FieldFlags·Meta.Bitfields·
// Meta.Arrays 등)과 런타임 조합 fixture가 본다. 표에 Required·Container가 ×인 행이 새로 생기면 그 칸이 여기서 만들어지고, 철자가
// 없으면 Meta.EveryRejectedValueKindCellHasASpelling이 실패한다.
internal static partial class MetaTests
{
    // C++가 bitfield를 허락하는 정수 계열 종류다(나머지 종류의 bitfield 선언은 C++ 오류다).
    private static readonly HashSet<string> IntegralKinds = new(StringComparer.Ordinal)
    {
        "Bool", "UInt8", "UInt16", "UInt32", "UInt64", "Int8", "Int16", "Int32", "Int64", "Enum"
    };

    // ROCKS_PROPERTY bitfield가 받는 종류다(부호 없는 정수 — 리플렉션 B9m).
    private static readonly HashSet<string> BitfieldKinds = new(StringComparer.Ordinal) { "UInt8", "UInt16", "UInt32", "UInt64" };

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

            if (!ValueKindTable.Has(row.Name, ValueKindProperty.AcceptsRequired))
            {
                Member field = FieldMember(spelling);
                cells.Add(($"{row.Name} Required field", "RMC2202", HolderSource([field with { Specifiers = "Required" }])));
                foreach ((ValueKindProperty property, string position, _, Func<string, string, Member> declare) in ElementPositions)
                {
                    // 원소 자리가 ×인 칸은 철자 검사가 먼저 거부한다(거부 칸 시험). Map 키의 Required는 아래 따로, Optional은 필드 칸이 본다.
                    if (!ValueKindTable.Has(row.Name, property) || property is ValueKindProperty.MapKey or ValueKindProperty.OptionalInner)
                    {
                        continue;
                    }

                    cells.Add(($"{row.Name} Required {position}", "RMC2202",
                        HolderSource([declare(spelling, "cell") with { Specifiers = "Required" }])));
                }
            }

            if (!ValueKindTable.Has(row.Name, ValueKindProperty.Container))
            {
                cells.Add(($"{row.Name} MaxElements", "RMC2202", HolderSource([FieldMember(spelling) with { Specifiers = "MaxElements = 4" }])));
            }

            if (!BitfieldKinds.Contains(row.Name) && !spelling.EndsWith(']'))
            {
                string code = IntegralKinds.Contains(row.Name) ? "RMC2207" : "RMC3000";
                cells.Add(($"{row.Name} bitfield", code, HolderSource([new Member($"{spelling} cell : 1;", "cell{}")])));
            }
        }

        // Required를 받는 종류(SoftObjectRef)가 Map 키에만 있고 값은 받지 않는 종류다 — Required는 값에 걸린다.
        cells.Add(("Map key Required", "RMC2202",
            HolderSource([Single("rocks::OrderedMap<rocks::SoftObjectRef<Target>, rocks::int32> cell;", "cell") with { Specifiers = "Required" }])));
        return cells;
    }

    private static void RegisterRejectedSpecifiers(MetaContext context)
    {
        // 칸마다 기대 진단 하나로 거부하고 이전 출력을 바꾸지 않는다(코드 집합이 기대 코드 하나와 같아야 한다). Clang 단계 칸은 사유가
        // bitfield 형식 오류여야 한다.
        context.Suite.Add("Meta.EveryRejectedSpecifierCellIsRejected", async () =>
        {
            IReadOnlyList<(string Name, string Code, string Source)> cells = RejectedSpecifierCells();
            Verify.That(cells.Count > 40, "rejected specifier cells enumerated: " + cells.Count);
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
    }
}
