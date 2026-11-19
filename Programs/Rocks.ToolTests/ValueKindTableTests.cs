using Rocks.MetaCompiler;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// MetaCompiler C# 값 종류 성질 표(ValueKindTable)와 런타임 표(Meta/TypeSchema.h ValueKindProperties)의 대조다. 근거는
// Docs/Design/ReflectionExtensions.ko.md 1.3절과 17절 B9a 항목이다. ValueKindTable.ParityHeader가 낸 대조 헤더를 실제 컴파일러로
// 문법 검사한다. 실제 표는 컴파일되고, 칸을 바꿔 넣은 표(C#만 허용하는 칸·런타임만 허용하는 칸·종류 수·열거자 값·성질 비트 값)는
// 그 칸을 가리키는 static_assert로 실패한다. C# 표가 성질 전체를 대조하는지, 원본 철자 해석이 만드는 종류가 표와 같은지도 본다.
internal static class ValueKindTableTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        string compiler = options.Required("compiler");
        suite.Add("Meta.ValueKindTableMatchesRuntime", async () =>
        {
            IReadOnlyList<ValueKindRow> rows = ValueKindTable.Rows;
            Verify.Equal(rows.Count, rows.Select(row => row.Name).Distinct(StringComparer.Ordinal).Count(), "value kind names are unique");
            Verify.That(rows.Select((row, index) => row.Value == (uint)index).All(same => same), "value kind values follow table order");

            // 대조 헤더가 C# 성질 전체를 서로 다른 런타임 질의로 덮는다(열이 빠지면 그 열은 대조 없이 통과한다).
            HashSet<ValueKindProperty> properties = [.. Enum.GetValues<ValueKindProperty>().Where(property => property != ValueKindProperty.None)];
            Verify.That(properties.SetEquals(ValueKindTable.Queries.Select(query => query.Property)), "Queries cover every value kind property");
            Verify.Equal(properties.Count, ValueKindTable.Queries.Count, "each property has one query");
            Verify.Equal(properties.Count, ValueKindTable.Queries.Select(query => query.Function).Distinct(StringComparer.Ordinal).Count(),
                "each query names a distinct runtime function");

            // Has는 성질 하나만 받는다(런타임 HasValueKindProperty와 같은 계약). 표에 없는 이름은 성질이 없다.
            Verify.That(!ValueKindTable.Has("NotAKind", ValueKindProperty.Field), "a kind outside the table has no property");
            Verify.That(ValueKindTable.Has("Int8", ValueKindProperty.MapKey) && ValueKindTable.Has("Int16", ValueKindProperty.ArrayElement),
                "B9a2 integer widths are keyed single values");
            Verify.That(ValueKindTable.Has("Name", ValueKindProperty.SetElement) && ValueKindTable.Has("Name", ValueKindProperty.WholeFieldValue) &&
                !ValueKindTable.Has("Name", ValueKindProperty.AcceptsRequired) && !ValueKindTable.Has("Name", ValueKindProperty.HasTargetType),
                "B9c2 Name is a keyed single value without Required or a target type");
            Verify.That(ValueKindTable.Has("WeakObjectReference", ValueKindProperty.ArrayElement) &&
                ValueKindTable.Has("WeakObjectReference", ValueKindProperty.MapValue) &&
                ValueKindTable.Has("WeakObjectReference", ValueKindProperty.HasTargetType) &&
                !ValueKindTable.Has("WeakObjectReference", ValueKindProperty.AcceptsRequired) &&
                !ValueKindTable.Has("WeakObjectReference", ValueKindProperty.MapKey) &&
                !ValueKindTable.Has("WeakObjectReference", ValueKindProperty.OptionalInner) &&
                !ValueKindTable.Has("WeakObjectReference", ValueKindProperty.RequiresTargetId),
                "B9d1 WeakObjectReference is a placed reference with an optional target type and no Required");
            Verify.That(ValueKindTable.Has("ClassReference", ValueKindProperty.ArrayElement) &&
                ValueKindTable.Has("ClassReference", ValueKindProperty.MapKey) &&
                ValueKindTable.Has("ClassReference", ValueKindProperty.SetElement) &&
                ValueKindTable.Has("ClassReference", ValueKindProperty.AcceptsRequired) &&
                ValueKindTable.Has("ClassReference", ValueKindProperty.RequiresTargetId) &&
                !ValueKindTable.Has("ClassReference", ValueKindProperty.OptionalInner) &&
                !ValueKindTable.Has("ClassReference", ValueKindProperty.InstancedStructContent),
                "B9d2 ClassReference is a keyed class id with a required base, Required and no Optional inner");
            Verify.That(ValueKindTable.Has("Optional", ValueKindProperty.Field) &&
                ValueKindTable.Has("Optional", ValueKindProperty.StructMember) &&
                ValueKindTable.Has("Optional", ValueKindProperty.Container) &&
                !ValueKindTable.Has("Optional", ValueKindProperty.ArrayElement) &&
                !ValueKindTable.Has("Optional", ValueKindProperty.OptionalInner) &&
                !ValueKindTable.Has("Optional", ValueKindProperty.AcceptsRequired) &&
                !ValueKindTable.Has("Optional", ValueKindProperty.WholeFieldValue),
                "B9e Optional is a field-only container whose aggregate shape depends on its inner kind");
            // Optional 필드의 Required 거부는 이 불변식에 기댄다(대상 종류 = 내부 종류 — 런타임 static_assert NoOptionalInnerAcceptsRequired와 같다).
            Verify.That(!ValueKindTable.Rows.Any(row => (row.Properties & ValueKindProperty.OptionalInner) != 0 && (row.Properties & ValueKindProperty.AcceptsRequired) != 0),
                "no Optional inner kind accepts Required");
            foreach (string container in new[] { "Map", "Set", "StaticArray" })
            {
                Verify.That(ValueKindTable.Has(container, ValueKindProperty.Field) &&
                    ValueKindTable.Has(container, ValueKindProperty.StructMember) &&
                    ValueKindTable.Has(container, ValueKindProperty.Container) &&
                    !ValueKindTable.Has(container, ValueKindProperty.ArrayElement) &&
                    !ValueKindTable.Has(container, ValueKindProperty.StaticArrayElement) &&
                    !ValueKindTable.Has(container, ValueKindProperty.MapKey) &&
                    !ValueKindTable.Has(container, ValueKindProperty.MapValue) &&
                    !ValueKindTable.Has(container, ValueKindProperty.SetElement) &&
                    !ValueKindTable.Has(container, ValueKindProperty.OptionalInner) &&
                    !ValueKindTable.Has(container, ValueKindProperty.WholeFieldValue),
                    $"B9f2/B9g {container} is a field-only aggregate container that does not nest");
            }

            Verify.That(ValueKindTable.Has("InstancedStruct", ValueKindProperty.Field) &&
                ValueKindTable.Has("InstancedStruct", ValueKindProperty.StructMember) &&
                ValueKindTable.Has("InstancedStruct", ValueKindProperty.ArrayElement) &&
                ValueKindTable.Has("InstancedStruct", ValueKindProperty.StaticArrayElement) &&
                ValueKindTable.Has("InstancedStruct", ValueKindProperty.MapValue) &&
                ValueKindTable.Has("InstancedStruct", ValueKindProperty.HasTargetType) &&
                !ValueKindTable.Has("InstancedStruct", ValueKindProperty.MapKey) &&
                !ValueKindTable.Has("InstancedStruct", ValueKindProperty.SetElement) &&
                !ValueKindTable.Has("InstancedStruct", ValueKindProperty.OptionalInner) &&
                !ValueKindTable.Has("InstancedStruct", ValueKindProperty.InstancedStructContent) &&
                !ValueKindTable.Has("InstancedStruct", ValueKindProperty.AcceptsRequired) &&
                !ValueKindTable.Has("InstancedStruct", ValueKindProperty.RequiresTargetId) &&
                !ValueKindTable.Has("InstancedStruct", ValueKindProperty.Container) &&
                !ValueKindTable.Has("InstancedStruct", ValueKindProperty.WholeFieldValue),
                "B9j InstancedStruct is a placed aggregate with an optional base and no Required");

            Verify.That(ValueKindTable.Has("SoftObjectReference", ValueKindProperty.MapKey), "single property query");
            Verify.Throws<ArgumentException>(() => ValueKindTable.Has("SoftObjectReference", ValueKindProperty.MapKey | ValueKindProperty.AcceptsRequired));
            Verify.Throws<ArgumentException>(() => ValueKindTable.Has("Bool", ValueKindProperty.None));

            string header = ValueKindTable.ParityHeader(rows);
            Verify.Contains("static_assert(::rocks::ValueKindCanBeMapKey(::rocks::ValueKind::Utf8String) == true", header,
                "parity header checks cells through the named runtime queries");
            Verify.Contains("static_assert(static_cast<::rocks::uint32>(::rocks::ValueKindProperty::MapKey) == 16u", header,
                "parity header checks property bit values");

            ProcessResult result = await CompileAsync(options, environment, compiler, header);
            Verify.Equal(0, result.ExitCode, "C# value kind table disagrees with the runtime table:\n" + result.StdOut + result.StdErr);
        });

        suite.Add("Meta.ValueKindTableMismatchFailsToCompile", async () =>
        {
            // C# 표를 바꿔 넣은 사례마다 컴파일이 실패하고 그 칸이 진단에 나와야 한다. 성질 비트 값은 C# enum을 바꿀 수 없으므로 헤더
            // 문장을 바꿔 넣는다.
            string real = ValueKindTable.ParityHeader(ValueKindTable.Rows);
            const string realBit = "::rocks::ValueKindProperty::MapKey) == 16u, \"ValueKindTable: ValueKindProperty::MapKey = 16\"";
            Verify.Contains(realBit, real, "bit assertion to replace");
            int count = ValueKindTable.Rows.Count;
            (string Name, string Header, string Diagnostic)[] cases =
            [
                ("C# allows, runtime rejects", Flip("Float32", ValueKindProperty.MapKey), "Float32 MapKey = true"),
                ("C# rejects, runtime allows", Flip("Bool", ValueKindProperty.MapKey), "Bool MapKey = false"),
                ("new kind cell", Flip("Int16", ValueKindProperty.ArrayElement), "Int16 ArrayElement = false"),
                ("Name kind cell", Flip("Name", ValueKindProperty.MapKey), "Name MapKey = false"),
                ("weak reference cell", Flip("WeakObjectReference", ValueKindProperty.AcceptsRequired), "WeakObjectReference AcceptsRequired = true"),
                ("class reference cell", Flip("ClassReference", ValueKindProperty.RequiresTargetId), "ClassReference RequiresTargetId = false"),
                ("optional cell", Flip("Optional", ValueKindProperty.ArrayElement), "Optional ArrayElement = true"),
                ("map cell", Flip("Map", ValueKindProperty.MapValue), "Map MapValue = true"),
                ("set cell", Flip("Set", ValueKindProperty.Container), "Set Container = false"),
                ("static array cell", Flip("StaticArray", ValueKindProperty.StaticArrayElement), "StaticArray StaticArrayElement = true"),
                ("instanced struct cell", Flip("InstancedStruct", ValueKindProperty.MapKey), "InstancedStruct MapKey = true"),
                ("text cell", Flip("Text", ValueKindProperty.OptionalInner), "Text OptionalInner = true"),
                ("container property", Flip("Array", ValueKindProperty.Container), "Array Container = false"),
                ("whole FieldValue property", Flip("Struct", ValueKindProperty.WholeFieldValue), "Struct WholeFieldValue = true"),
                // 런타임에 없는 종류를 C# 표에만 더한 경우다.
                ("extra C# kind", ValueKindTable.ParityHeader([.. ValueKindTable.Rows,
                    new("FutureKind", (uint)count, ValueKindProperty.Field)]),
                    $"C# table has {count + 1} value kinds"),
                ("missing C# kind", ValueKindTable.ParityHeader([.. ValueKindTable.Rows.Take(count - 1)]),
                    $"C# table has {count - 1} value kinds"),
                ("enumerator value", ValueKindTable.ParityHeader([.. ValueKindTable.Rows.Select(row => row.Name == "Guid" ? row with { Value = 11 } : row)]),
                    "Guid = 11"),
                ("property bit value", real.Replace(realBit,
                    "::rocks::ValueKindProperty::MapKey) == 32u, \"ValueKindTable: ValueKindProperty::MapKey = 32\"", StringComparison.Ordinal),
                    "ValueKindProperty::MapKey = 32")
            ];
            foreach ((string name, string header, string diagnostic) in cases)
            {
                ProcessResult result = await CompileAsync(options, environment, compiler, header);
                Verify.That(result.ExitCode != 0, name + ": mismatched value kind table compiled");
                Verify.Contains(diagnostic, result.StdOut + result.StdErr, name + ": mismatched cell diagnostic");
            }
        });

        suite.Add("Meta.ValueKindTableMatchesSourceResolution", () =>
        {
            // 원본 선언의 철자 해석이 만드는 단일 값 종류는 성질 표의 Array 원소 열과 같고, 모두 클래스 필드·struct 멤버가 될 수
            // 있다(SchemaValidator.Validate의 표 확인이 지금 늘 통과하는 근거 — 새 종류는 두 곳에 함께 더한다).
            HashSet<string> source = [.. SchemaValidator.SourceValueKinds];
            Verify.Equal(SchemaValidator.SourceValueKinds.Count, source.Count, "source value kinds are distinct");
            IEnumerable<string> elements = ValueKindTable.Rows.Select(row => row.Name)
                .Where(kind => ValueKindTable.Has(kind, ValueKindProperty.ArrayElement));
            Verify.That(source.SetEquals(elements), "source value kinds equal the table's array element kinds");
            foreach (string kind in source)
            {
                Verify.That(ValueKindTable.Has(kind, ValueKindProperty.Field) && ValueKindTable.Has(kind, ValueKindProperty.StructMember),
                    kind + " can be a class field and a struct member");
            }

            Verify.That(ValueKindTable.Has(ArrayRules.Array, ValueKindProperty.Field) && ValueKindTable.Has(ArrayRules.Array, ValueKindProperty.StructMember),
                "array fields can be class fields and struct members");
            Verify.That(ValueKindTable.Has(OptionalRules.Optional, ValueKindProperty.Field) &&
                ValueKindTable.Has(OptionalRules.Optional, ValueKindProperty.StructMember),
                "optional fields can be class fields and struct members");
            Verify.That(ValueKindTable.Has(MapRules.Map, ValueKindProperty.Field) && ValueKindTable.Has(MapRules.Set, ValueKindProperty.StructMember),
                "map and set fields can be class fields and struct members");
        });
    }

    // 실제 표에서 kind 행의 property 칸 하나를 뒤집은 표의 대조 헤더다.
    private static string Flip(string kind, ValueKindProperty property) =>
        ValueKindTable.ParityHeader([.. ValueKindTable.Rows.Select(row => row.Name == kind ? row with { Properties = row.Properties ^ property } : row)]);

    // 대조 헤더 header를 include하는 번역 단위를 소비자와 같은 경고 프로필로 문법 검사한다(include 루트는 Core 런타임 모듈의
    // Public 디렉터리다).
    private static async Task<ProcessResult> CompileAsync(Options options, IReadOnlyDictionary<string, string?> environment, string compiler,
        string header)
    {
        using TemporaryDirectory temporary = new();
        temporary.Write("ValueKindParity.h", header);
        temporary.Write("parity.cpp", "#include \"ValueKindParity.h\"\n");
        string source = temporary.FilePath("parity.cpp");
        bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
        List<string> arguments = clangCl
            ? ["/nologo", "/std:c++latest", "/W4", "/WX", "/Zs", source]
            : ["-x", "c++", "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only", source];
        foreach (string directory in Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
            .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal))
        {
            arguments.Add((clangCl ? "/I" : "-I") + directory);
        }

        arguments.AddRange(options.ClangArguments);
        return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment, Timeout: MetaTests.ProbeTimeout));
    }
}
