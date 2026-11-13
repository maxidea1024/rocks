using System.Text;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;
using Rocks.Build;
using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

internal static class MetaTests
{
    private const string Preamble = "#include <Meta/Annotations.h>\n#include <Object/ObjectRefs.h>\n#include <Object/Object.h>\n#include <Core/Text/String.h>\nnamespace probe {\n";

    private static string Declaration(string? field = null,
        string bases = "public rocks::Object", string extra = "") =>
        Preamble + $"ROCKS_CLASS()\nstruct Value : {bases} {{\n" +
        (field ?? "ROCKS_PROPERTY() rocks::int32 count = 0;") + "\n" + extra + "\n};\n}\n";

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        string compiler = options.Required("compiler");
        string generator = Path.GetFullPath(options.Required("metacompiler"));
        if (!File.Exists(generator))
        {
            throw new FileNotFoundException("MetaCompiler executable missing", generator);
        }

        Dictionary<string, string?> actualEnvironment = new(environment, OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        if (options.Optional("formatter") is string formatter)
        {
            actualEnvironment["ROCKS_CLANG_FORMAT"] = formatter;
        }

        GeneratedBodyReviewTests.Register(suite, options, actualEnvironment);
        TypeConvenienceCompilerTests.Register(suite, options, actualEnvironment);
        IdentityCompilerTests.Register(suite, options, actualEnvironment);
        IdentityCliReviewTests.Register(suite, options, actualEnvironment);
        ModuleMetaTests.Register(suite, options, actualEnvironment);
        IdentityModuleCliTests.Register(suite, options, actualEnvironment);

        suite.Add("Meta.FixtureDeterminismTimestampAndStaleCheck", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(fixture: true);
            byte[] expected = File.ReadAllBytes(probe.Output);
            Verify.That(!expected.Contains((byte)'\r'), "generated output must use LF");
            Verify.Contains("rocks::reflection_fixture::Node", Encoding.UTF8.GetString(expected), "fixture generated type");
            File.SetLastWriteTimeUtc(probe.Output, DateTime.UnixEpoch.AddSeconds(1_000_000_000));
            DateTime timestamp = File.GetLastWriteTimeUtc(probe.Output);
            await probe.GenerateAsync(fixture: true);
            Verify.That(File.ReadAllBytes(probe.Output).AsSpan().SequenceEqual(expected), "generator output changed for identical source");
            Verify.Equal(timestamp, File.GetLastWriteTimeUtc(probe.Output), "unchanged output was rewritten");
            await probe.GenerateAsync(fixture: true, extra: ["--check"]);
            Verify.Equal(timestamp, File.GetLastWriteTimeUtc(probe.Output), "check changed output timestamp");
            File.WriteAllText(probe.Output, "stale\n");
            await probe.GenerateAsync(fixture: true, extra: ["--check"], expectedCode: "RMC4000", reason: "stale");
            Verify.Equal("stale\n", File.ReadAllText(probe.Output), "stale check overwrote output");
        });
        suite.Add("Meta.GeneratedHeaderDirectoryControlsIncludeBinding", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.OutputDirectoryBindingAsync();
        });
        suite.Add("Meta.SymlinkedParentsAndShadowedHeaders", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.SymlinkLocationsAsync();
        });
        suite.Add("Meta.ClangSourceNotesDoNotMisidentifyNames", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.SourceNotesAsync();
        });
        suite.Add("Meta.IncludedHeaderDiagnosticUsesOriginalFileAndLine", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.NestedLocationAsync();
        });
        AddRejectGroup("Meta.MissingFieldAnnotation", [Declaration("rocks::int32 count = 0;")], "RMC2000", "field");
        AddRejectGroup("Meta.PointerAndCvFields", new[] { "rocks::int32* count = nullptr", "const rocks::int32 count = 0" }
            .Select(field => Declaration($"ROCKS_PROPERTY() {field};")), "RMC2000", "type");
        AddRejectGroup("Meta.PrivateWithoutGeneratedBody", [Declaration("private: ROCKS_PROPERTY() rocks::int32 count = 0;",
            extra: "public: rocks::int32 Read() const noexcept { return count; }")], "RMC2000", "GENERATED_BODY");
        AddRejectGroup("Meta.Bitfields", [Declaration("ROCKS_PROPERTY() unsigned int count : 1;")], "RMC2000", "bitfield");
        AddRejectGroup("Meta.StaticAnnotatedField", [Declaration("ROCKS_PROPERTY() static inline rocks::int32 count = 0;")], "RMC200", "nonstatic");
        AddRejectGroup("Meta.ManualTraceConflict", [Declaration(extra: "void Trace() noexcept {}")], "RMC2000", "Trace");
        AddRejectGroup("Meta.VirtualAndMultipleInheritance", [Declaration(bases: "public virtual rocks::Object"),
            Declaration(bases: "public rocks::Object, public Other").Replace("namespace probe {", "namespace probe { struct Other {};", StringComparison.Ordinal)], "RMC2000", "nonvirtual");
        AddRejectGroup("Meta.UnreflectedBaseAndReference", [
            Declaration(bases: "public Hidden").Replace("namespace probe {", "namespace probe { struct Hidden : rocks::Object {};", StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::ObjectRef<Hidden> next;")
                .Replace("namespace probe {", "namespace probe { struct Hidden : rocks::Object {};", StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY() rocks::SoftObjectRef<Hidden> next;", extra: "explicit Value(rocks::Allocator& allocator) noexcept : next(allocator) {}")
                .Replace("namespace probe {", "#include <Object/SoftObjectRef.h>\nnamespace probe { struct Hidden : rocks::Object {};", StringComparison.Ordinal)],
            "RMC2003", "reflected");
        AddRejectGroup("Meta.ClangSyntaxFailurePreservesOutput", [Declaration("this is not valid cxx;")], "RMC3000", "Clang");
        suite.Add("Meta.DiagnosticsAndResponseOverridesRejected", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            var arguments = new List<string> { "-Wno-error", "-w", "@hidden.rsp", "-I", "-Ihidden", "-iquote", "-iquotehidden", "-isystem", "-isystemhidden", "-idirafter", "-idirafterhidden", "-include", "-includehidden", "-imacros", "-imacroshidden", "-F", "-Fhidden", "-iframeworkhidden", "-Wp,-Ihidden", "-ivfsoverlay=hidden", "--config=hidden" };
            arguments.AddRange(["-iwithprefixbefore", "-iwithprefixbeforehidden", "-iwithprefix", "-iwithprefixhidden", "-iprefix", "-iprefixhidden", "-iwithsysroot", "-iwithsysroothidden", "-stdlib++-isystem", "-stdlib++-isystemhidden", "--gcc-toolchain", "--gcc-toolchain=hidden", "-vfsoverlay", "-vfsoverlay=hidden", "-working-directory", "-working-directory=hidden", "-D", "-D=invalid", "-D_CRT_SECURE_NO_WARNINGS", "--target=hidden"]);
            if (OperatingSystem.IsWindows())
            {
                arguments.AddRange(["/clang:-Wno-error", "/W0", "/I", "/Ihidden", "/FI", "/FIhidden", "/external:Ihidden", "/clang:-Ihidden"]);
            }

            foreach (string argument in arguments)
            {
                await probe.GenerateAsync(Declaration(), extra: ["--clang-arg=" + argument], expectedCode: "RMC1000", reason: "override");
                Verify.That(!File.Exists(probe.Output), "rejected CLI wrote output");
            }
        });
        if (OperatingSystem.IsMacOS())
        {
            suite.Add("Meta.MacAbsolutePathsAreNotWindowsSwitches", async () =>
            {
                using Probe probe = new(options, actualEnvironment, generator, compiler);
                // 경로가 없으므로 SDK 진단으로 거부한다. /I·/FI Windows 옵션으로 오인하지 않는다.
                foreach (string path in new[] { "/Install/RocksMissingSdk", "/Files/RocksMissingSdk" })
                {
                    await probe.GenerateAsync(Declaration(), extra: ["--clang-arg=-isysroot", "--clang-arg=" + path],
                        expectedCode: "RMC1000", reason: "SDK");
                }
            });
        }

        suite.Add("Meta.ConflictingOrDuplicateCompilerOptionsAreRejected", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            var cases = new List<string[]>
            {
                new[] { "-DROCKS_CONFLICT=1", "-DROCKS_CONFLICT=2" },
                new[] { "-DROCKS_DUPLICATE=1", "-DROCKS_DUPLICATE=1" },
                new[] { "-DROCKS_SAME", "-DROCKS_SAME=1" }
            };
            if (OperatingSystem.IsWindows())
            {
                cases.AddRange([["/MD", "/MDd"], ["/O2", "/Od"], ["/MD", "/MD"],
                    ["/MDd", "/fsanitize=address"], ["/DROCKS_MIXED=1", "-DROCKS_MIXED=2"]]);
            }
            else
            {
                cases.AddRange([["-O0", "-O2"], ["-Wall", "-Wall"],
                    ["-mmacosx-version-min=13.0", "-mmacosx-version-min=14.0"]]);
            }

            foreach (string[] conflict in cases)
            {
                await probe.GenerateAsync(Declaration(), extra: conflict.Select(value => "--clang-arg=" + value).ToArray(),
                    expectedCode: "RMC1000", reason: "conflict");
                Verify.That(!File.Exists(probe.Output), "conflicting options published metadata");
            }
        });
        suite.Add("Meta.AllNativeCompileProfilesAndExplicitDefinesAreAccepted", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            NativeToolchain toolchain = await NativeToolchains.DiscoverAsync(compiler);
            // 모든 구성 × 모든 모듈 최적화 재정의 조합이 AST 허용 목록을 통과해야 한다.
            foreach (NativeConfiguration configuration in Enum.GetValues<NativeConfiguration>())
            {
                foreach (CodeOptimization optimization in Enum.GetValues<CodeOptimization>())
                {
                    string[] arguments = NativeCompileProfile.Arguments(toolchain, configuration, optimization)
                        .Select(argument => "--clang-arg=" + argument)
                        .Append("--clang-arg=-DROCKS_META_PROFILE_PROBE=1").ToArray();
                    await probe.GenerateAsync(Declaration(), extra: arguments);
                }
            }
        });
        suite.Add("Meta.PrefixClassSupportsNestedScopesCommentsAndConditionalBlock", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string marker = "ROCKS_CLASS()";
            string valid = Preamble + "struct Outer { public:\n#if 1\n" + marker +
                "\n// #if 0 is only a comment\n/* #include <ignored> */\n" +
                "class Inner : public rocks::Object { public: Inner() noexcept = default; };\n#endif\n};\n}\n";
            await probe.GenerateAsync(valid);
            Verify.Contains("probe::Outer::Inner", File.ReadAllText(probe.Output), "nested prefix CLASS");
        });
        suite.Add("Meta.PrefixClassNeverBindsAcrossTokensOrPreprocessing", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string marker = "ROCKS_CLASS()";
            string definition = "struct Value : public rocks::Object {};\n";
            string[] invalid =
            [
                marker + "\nint intervening;\n" + definition,
                marker + ";\n" + definition,
                marker + "\n" + marker + "\n" + definition,
                marker + "\nstruct [[clang::annotate(\"rocks\", \"rocks.type\")]] Value : public rocks::Object {};\n",
                marker + "\n#if 0\nstruct Intended : public rocks::Object {};\n#endif\n" + definition,
                marker + "\n#include <Meta/Annotations.h>\n" + definition,
                marker + "\ntemplate <class T> struct Value : public rocks::Object {};\n",
                marker + "\nstruct Value;\n",
                marker + "\nnamespace other {}\n" + definition,
                "#define CLASS_WRAP(...) ROCKS_CLASS(__VA_ARGS__)\n" +
                    "CLASS_WRAP()\n" + definition,
                "#define CLASS_BODY struct Value : public rocks::Object {}\n" + marker + "\nCLASS_BODY;\n",
                "void Test() {\n" + marker + "\n" + definition + "}\n",
                "struct ROCKS_CLASS() Value : public rocks::Object {};\n",
                marker + "\n#line 100 \"nonexistent_virtual_source.hpp\"\n" + definition
            ];
            foreach (string body in invalid)
            {
                await probe.RejectAsync(Preamble + body + "}\n", "RMC", "ROCKS_CLASS");
            }
        });
        suite.Add("Meta.PrefixClassCannotBindAcrossIncludeFiles", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.PrefixDifferentFileAsync();
        });
        suite.Add("Meta.PrivateGeneratedBodyAndUnicodeSpecifiers", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "ROCKS_CLASS()\nclass Value : public rocks::Object {\n" +
                "ROCKS_GENERATED_BODY()\n" + "ROCKS_PROPERTY(ReadOnly, Category=\"능력\", DisplayName=\"체력🎮\") rocks::int32 count = 0;\n" +
                "public: rocks::int32 Read() const noexcept { return count; }\n};\n}\n";
            await probe.GenerateAsync(source);
            string generated = File.ReadAllText(probe.Output);
            Verify.Contains("\"체력🎮\"", generated, "generated display name remains readable UTF-8");
            Verify.Contains("ErrorCode::Unsupported", generated, "ReadOnly public setter policy");
            Verify.Contains("WriteGeneratedStagedField", generated, "ReadOnly staged initialization remains available");
        });
        suite.Add("Meta.AbstractClassHasExplicitMetadata", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "ROCKS_CLASS(Abstract)\nclass Value : public rocks::Object { public:\n" +
                "ROCKS_PROPERTY() rocks::int32 count=0; virtual void Tick() noexcept=0;\n};\n}\n";
            await probe.GenerateAsync(source);
            Verify.Contains("MakeRuntimeTypeOps<::probe::Value, true>", File.ReadAllText(probe.Output), "abstract generated runtime ops");
        });
        // B2: CLASS·PROPERTY 지정자가 descriptor와 생성 스키마에 닿는다(Docs/Design/ReflectionFoundations.ko.md 2절).
        suite.Add("Meta.ClassAndPropertySpecifiersReachTheSchema", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble.Replace("namespace probe {", "#include <Object/SoftObjectRef.h>\nnamespace probe {", StringComparison.Ordinal) +
                "struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
                "struct Cache { rocks::ObjectRef<rocks::Object> ref; };\n" +
                "struct Holder { rocks::ObjectRef<rocks::Object> ref; Lease lease; };\n" +
                "ROCKS_CLASS(WorldOnly, Version=2147483647)\nstruct Parent : public rocks::Object {};\n" +
                "ROCKS_CLASS(Version=3)\nstruct Value : public Parent {\n" +
                "ROCKS_PROPERTY(EditorOnly, Name=\"Hits\") rocks::int32 count = 0;\n" +
                "ROCKS_PROPERTY(Required) rocks::ObjectRef<Value> next;\n" +
                "ROCKS_PROPERTY(Name=\"other\") rocks::int32 other = 0;\n" +
                "ROCKS_NATIVE() rocks::int32 cache = 0;\n" +
                "ROCKS_NATIVE() rocks::WeakRef<Value> weak;\n" +
                "ROCKS_NATIVE() int* raw = nullptr;\n" +
                "ROCKS_NATIVE() Cache* borrowed = nullptr;\n" +
                "ROCKS_NATIVE() Lease lease;\n" +
                // 소프트 참조는 대상을 담지 않으므로 인자(Holder)의 강한 참조를 따라가지 않는다(RMC2205 아님).
                "ROCKS_NATIVE() rocks::SoftObjectRef<Holder> soft;\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : soft(allocator) {}\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement value = Type("probe::Value");
            Verify.Equal(3, value.GetProperty("version").GetInt32(), "Version specifier");
            Verify.Equal("WorldOnly", value.GetProperty("flags")[0].GetString(), "WorldOnly is inherited from the base");
            Verify.Equal("WorldOnly", Type("probe::Parent").GetProperty("flags")[0].GetString(), "WorldOnly specifier");
            Verify.Equal(int.MaxValue, Type("probe::Parent").GetProperty("version").GetInt32(), "largest Version value");
            System.Text.Json.JsonElement Field(string name) => value.GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            Verify.Equal("EditorOnly", Field("count").GetProperty("flags")[0].GetString(), "EditorOnly flag");
            Verify.Equal("Hits", Field("count").GetProperty("toolName").GetString(), "Name specifier");
            Verify.Equal("Required", Field("next").GetProperty("flags")[0].GetString(), "Required flag");
            Verify.Equal("other", Field("other").GetProperty("toolName").GetString(), "Name equal to the field name is the default");
            Verify.Equal(3, value.GetProperty("fields").GetArrayLength(), "NATIVE members are not schema fields");
            string generated = File.ReadAllText(probe.Output);
            Verify.Contains("IsNativeStrongReference", generated, "NATIVE strong reference static_assert");
            Verify.Contains("::probe::Value::lease", generated, "NATIVE member static_assert target");
            Verify.That(!generated.Contains("RequiresWorldOnlyOwner<", StringComparison.Ordinal), "WorldOnly owner needs no RMC2206 static_assert");
        });
        // B3b: ROCKS_ENUM이 descriptor와 생성 스키마·검증기·집계 등록 순서에 닿는다(Docs/Design/ReflectionFoundations.ko.md 3.4절).
        suite.Add("Meta.EnumSchemaReachesDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "ROCKS_ENUM(ToolPrefix = \"MODE_\")\nenum class Mode : rocks::uint8 {\n" +
                "Off = 0,\nEight ROCKS_ENUMERATOR(Name = \"8Bit\", DisplayName = \"여덟\") = 8,\nNext,\nSecret ROCKS_ENUMERATOR(Hidden) = 200,\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "ROCKS_ENUM(Flags)\nenum class Nested : rocks::uint32 { None = 0, A = 1, B = 0x80000000u };\n" +
                "ROCKS_PROPERTY() Mode mode = Mode::Off;\nROCKS_PROPERTY() Nested nested = Nested::None;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement mode = Type("probe::Mode");
            Verify.Equal("enum", mode.GetProperty("kind").GetString(), "enum descriptor kind");
            Verify.Equal("UInt8", mode.GetProperty("underlying").GetString(), "underlying");
            Verify.Equal("MODE_", mode.GetProperty("toolPrefix").GetString(), "tool prefix");
            System.Text.Json.JsonElement[] values = mode.GetProperty("values").EnumerateArray().ToArray();
            Verify.Equal(4, values.Length, "enumerator count");
            Verify.Equal("8Bit", values[1].GetProperty("toolName").GetString(), "enumerator tool name");
            Verify.Equal(9, values[2].GetProperty("value").GetInt32(), "implicit value is previous + 1");
            Verify.Equal("Hidden", values[3].GetProperty("flags")[0].GetString(), "hidden enumerator");
            Verify.Equal("Flags", Type("probe::Value::Nested").GetProperty("flags")[0].GetString(), "nested Flags enum");
            System.Text.Json.JsonElement field = Type("probe::Value").GetProperty("fields").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == "mode");
            Verify.Equal("Enum", field.GetProperty("value").GetProperty("kind").GetString(), "enum field kind");
            Verify.Equal(mode.GetProperty("id").GetString(), field.GetProperty("value").GetProperty("type").GetString(), "enum field target");
            string generated = File.ReadAllText(probe.Output);
            Verify.Contains("struct GeneratedType<::probe::Mode>", generated, "enum specialization");
            Verify.Contains("static constexpr bool IsDeclared", generated, "constexpr validator");
            Verify.Contains("NativeTypeKey<::probe::Mode>()", generated, "enum field native key");
            Verify.That(generated.IndexOf("GeneratedType<::probe::Mode>", StringComparison.Ordinal) <
                generated.IndexOf("GeneratedType<::probe::Value>", StringComparison.Ordinal), "enums are emitted before classes");
        });
        AddRejectGroup("Meta.EnumUnderlyingAndValuesAreRestricted", new[]
            {
                "ROCKS_ENUM()\nenum class Mode { A };",
                "ROCKS_ENUM()\nenum class Mode : long long { A };",
                "ROCKS_ENUM()\nenum class Mode : signed char { A };",
                "ROCKS_ENUM()\nenum class Mode : short { A };",
                "ROCKS_ENUM()\nenum Mode : rocks::uint8 { A };",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A = 1, B = 1 };",
                "ROCKS_ENUM(Flags)\nenum class Mode : rocks::int32 { A = -1 };",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 {};",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8;",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint16 { " + string.Join(", ", Enumerable.Range(0, 4097).Select(index => "E" + index)) + " };"
            }.Select(declaration => Preamble + declaration + "\n" + "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"), "RMC2401", "probe::Mode");
        AddRejectGroup("Meta.EnumToolNamesAreValidAndUnique", new[]
            {
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A ROCKS_ENUMERATOR(Name = \"b\"), B };",
                "ROCKS_ENUM(ToolPrefix = \"M_\")\nenum class Mode : rocks::uint8 { A, M_A };",
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A ROCKS_ENUMERATOR(Name = \"has space\") };",
                "ROCKS_ENUM(ToolPrefix = \"bad-\")\nenum class Mode : rocks::uint8 { A };"
            }.Select(declaration => Preamble + declaration + "\n}\n"), "RMC2203", "probe::Mode");
        AddRejectGroup("Meta.EnumMarkerMustPrecedeAnEnum", [Preamble + "ROCKS_ENUM()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "immediately followed by a named enum");
        AddRejectGroup("Meta.EnumeratorAnnotationNeedsReflectedEnum", [
            Preamble + "enum class Mode : rocks::uint8 { A ROCKS_ENUMERATOR(Hidden) };\nROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "ROCKS_ENUMERATOR must annotate");
        AddRejectGroup("Meta.NestedReflectedEnumMustBePublic", [
            Preamble + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_ENUM()\nenum class Nested : rocks::uint8 { A };\n};\n}\n"],
            "RMC2401", "must be public");
        // underlying 명시 판정(주석·줄바꿈 사이, enum struct)과 식 초기값·int32 최솟값·uint32 최댓값을 받는다.
        suite.Add("Meta.EnumValuesFollowClangEvaluation", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "constexpr int Base = 21;\n" +
                "ROCKS_ENUM()\nenum struct Mode /* explicit */\n    : rocks::int32 { Min = -2147483647 - 1, A = -3, B, C = B + 10, D = Base * 2 };\n" +
                "struct Outer {\nROCKS_ENUM()\nenum class Inner : rocks::uint32 { Max = 0xffffffffu };\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Mode mode = Mode::A;\nROCKS_PROPERTY() Outer::Inner nested = Outer::Inner::Max;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            long[] Values(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name).GetProperty("values").EnumerateArray()
                .Select(value => value.GetProperty("value").GetInt64()).ToArray();
            Verify.That(Values("probe::Mode").SequenceEqual(new long[] { -2147483648, -3, -2, 8, 42 }), "evaluated enumerator values");
            Verify.That(Values("probe::Outer::Inner").SequenceEqual(new long[] { 4294967295 }), "uint32 maximum");
            string generated = File.ReadAllText(probe.Output);
            Verify.Contains("-2147483648ll", generated, "int32 minimum literal");
            Verify.Contains("static_assert(static_cast<int64>(::probe::Mode::D) == 42ll", generated, "value static_assert");
        });
        AddRejectGroup("Meta.EnumFieldsRejectRequired", [
            Preamble + "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A };\nROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY(Required) Mode count = Mode::A;\n};\n}\n"],
            "RMC2202", "applies only to");
        AddRejectGroup("Meta.EnumIsNotABaseOrReferenceTarget", [
            Preamble.Replace("namespace probe {", "#include <Object/SoftObjectRef.h>\nnamespace probe {", StringComparison.Ordinal) +
                "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { A };\nROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
                "explicit Value(rocks::Allocator& allocator) noexcept : next(allocator) {}\nROCKS_PROPERTY() rocks::SoftObjectRef<Mode> next;\n};\n}\n"],
            "RMC2003", "enum");
        AddRejectGroup("Meta.UnreflectedEnumFieldIsUnsupported", [
            Preamble + "enum class Mode : rocks::uint8 { A };\nROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Mode count = Mode::A;\n};\n}\n"],
            "RMC2000", "unsupported field type");
        AddRejectGroup("Meta.IntegerSpecifierSyntax", new[] { "Version=0", "Version=01", "Version=2147483648", "Version=\"2\"", "Version=1x", "Version", "Version=-1", "Version=+1" }
            .Select(spec => Declaration().Replace("ROCKS_CLASS()", $"ROCKS_CLASS({spec})", StringComparison.Ordinal)), "RMC2000", "specifier");
        AddRejectGroup("Meta.RequiredAndMaxElementsNeedMatchingFieldKinds", [Declaration("ROCKS_PROPERTY(Required) rocks::int32 count = 0;"),
            Declaration("ROCKS_PROPERTY(Required) rocks::AssetId count;")
                .Replace("namespace probe {", "#include <Object/AssetId.h>\nnamespace probe {", StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY(MaxElements=4) rocks::int32 count = 0;")], "RMC2202", "applies only to");
        AddRejectGroup("Meta.ToolNameMustBeAnIdentifier", [Declaration("ROCKS_PROPERTY(Name=\"1bad\") rocks::int32 count = 0;"),
            Declaration("ROCKS_PROPERTY(Name=\"has space\") rocks::int32 count = 0;"),
            Declaration("ROCKS_PROPERTY(Name=\"\") rocks::int32 count = 0;")], "RMC2203", "Name specifier");
        AddRejectGroup("Meta.ToolNamesAreUniqueIgnoringCase", [Declaration("ROCKS_PROPERTY(Name=\"Other\") rocks::int32 count = 0;\nROCKS_PROPERTY() rocks::int32 other = 0;")],
            "RMC2203", "tool name");
        AddRejectGroup("Meta.PropertyAndNativeAreExclusive", [Declaration("ROCKS_PROPERTY() ROCKS_NATIVE() rocks::int32 count = 0;"),
            Declaration("ROCKS_NATIVE() ROCKS_PROPERTY() rocks::int32 count = 0;")], "RMC2204", "not both");
        AddRejectGroup("Meta.NativeTakesNoSpecifiersAndNeedsGeneratedBodyWhenPrivate", [
            Declaration(extra: "ROCKS_NATIVE(Transient) rocks::int32 cache = 0;"),
            Declaration(extra: "private: ROCKS_NATIVE() rocks::int32 cache = 0;\npublic: rocks::int32 Read() const noexcept { return cache; }")], "RMC2000", "NATIVE");
        const string holders = "namespace probe { struct Holder { rocks::ObjectRef<rocks::Object> ref; };\n" +
            "template <class T> struct Box { T value{}; };\n" +
            "template <class T> struct Keep { rocks::ObjectRef<rocks::Object> ref; T value{}; };\n" +
            "template <class A, class B> struct Pair { A first{}; B second{}; };\n" +
            "struct Tagged { union { rocks::Object* object; unsigned long long bits; }; };\n";
        AddRejectGroup("Meta.NativeMembersCannotHoldStrongReferences", new[]
            {
                "rocks::ObjectRef<Value> cache;",
                "rocks::RootRef<Value> cache;",
                "rocks::ObjectRef<Value> cache[2];",
                "Value* cache = nullptr;",
                "const rocks::Object* cache = nullptr;",
                "Holder cache;",
                "Box<Value*> cache;",
                "Keep<int> cache;",
                "Pair<Value*, void (*)()> cache;",
                "Tagged cache;",
                "Value** cache = nullptr;"
            }.Select(member => Declaration(extra: "ROCKS_NATIVE() " + member).Replace("namespace probe {", holders, StringComparison.Ordinal)),
            "RMC2205", "strong object reference");
        const string lease = "namespace probe { struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
            "struct Wrapper { Lease inner; };\n";
        AddRejectGroup("Meta.WorldOnlyOwnerTraitNeedsWorldOnlyClass", [
            Declaration(extra: "ROCKS_NATIVE() Lease lease;").Replace("namespace probe {", lease, StringComparison.Ordinal),
            Declaration(extra: "ROCKS_NATIVE() Wrapper lease;").Replace("namespace probe {", lease, StringComparison.Ordinal)],
            "RMC2206", "WorldOnly owner");
        // private NATIVE 멤버는 GENERATED_BODY가 있으면 생성 static_assert가 friend로 접근해 컴파일된다(검증 컴파일).
        suite.Add("Meta.PrivateNativeMemberCompilesWithGeneratedBody", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\n" +
                "ROCKS_PROPERTY() rocks::int32 count = 0;\nROCKS_NATIVE() rocks::int32 cache[2] = {};\n" +
                "public: rocks::int32 Read() const noexcept { return count + cache[0]; }\n};\n}\n";
            await probe.GenerateAsync(source);
            Verify.Contains("::probe::Value::cache", File.ReadAllText(probe.Output), "private NATIVE static_assert");
        });
        AddRejectGroup("Meta.UnknownDuplicateAndWrongValueSpecifiers", new[] { "UnknownFlag", "ReadOnly, ReadOnly", "ReadOnly=\"yes\"", "Category=12", "DisplayName" }
            .Select(spec => Declaration($"ROCKS_PROPERTY({spec}) rocks::int32 count=0;")), "RMC2000", "specifier");
        AddRejectGroup("Meta.UnannotatedOwnerAndParameterMisuse", [
            Preamble + "struct Plain { ROCKS_PROPERTY() rocks::int32 count=0; };\n}\n",
            Preamble + "void F(ROCKS_PROPERTY() rocks::int32 value);\n}\n"], "RMC200", "PROPERTY");
        AddRejectGroup("Meta.UnknownTypeSpecifier", [Declaration().Replace("ROCKS_CLASS()",
            "ROCKS_CLASS(UnknownClassFlag)", StringComparison.Ordinal)], "RMC2000", "specifier");

        void AddRejectGroup(string name, IEnumerable<string> sources, string code, string reason)
        {
            string[] fixedSources = sources.ToArray();
            suite.Add(name, async () =>
            {
                using Probe probe = new(options, actualEnvironment, generator, compiler);
                foreach (string source in fixedSources)
                {
                    await probe.RejectAsync(source, code, reason);
                }
            });
        }
    }

    private sealed class Probe : IDisposable
    {
        private readonly TemporaryDirectory temporary = new();
        private readonly Options options;
        private readonly IReadOnlyDictionary<string, string?> environment;
        private readonly string generator;
        private readonly string compiler;

        public Probe(Options options, IReadOnlyDictionary<string, string?> environment, string generator, string compiler)
        {
            this.options = options;
            this.environment = environment;
            this.generator = generator;
            this.compiler = compiler;
            MetaProbeIdentity.Create(IdentityPath);
        }

        private string lastInclude = "Probe.h";

        // 마지막으로 생성한 헤더의 생성 경로다(fixture 모드는 fixture 철자).
        public string Output => MetaCompilerArguments.GeneratedPath(temporary.Root, lastInclude);

        public string Descriptor => MetaCompilerArguments.Descriptor(temporary.Root);

        private string IdentityPath => temporary.FilePath("probe.rocksids.json");

        public async Task GenerateAsync(string? source = null, bool fixture = false, IReadOnlyList<string>? extra = null,
            string? expectedCode = null, string? reason = null)
        {
            string include = fixture ? "ReflectionTestFixtures/ReflectionFixture.h" : "Probe.h";
            string input = fixture ? Path.Combine(FixturePublic, "ReflectionTestFixtures", "ReflectionFixture.h") : temporary.FilePath("Probe.h");
            lastInclude = include;
            if (source is not null)
            {
                File.WriteAllText(input, source, new UTF8Encoding(false, true));
            }

            List<string> arguments = [];
            string executable = generator;
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }

            arguments.AddRange([.. MetaCompilerArguments.Single(temporary.Root, input, include), "--compiler", compiler]);
            AddDefaultIncludes(arguments);
            arguments.AddRange(["--identity-manifest", fixture
                ? Path.Combine(options.Root, "Core", "Source", "Developer", "ReflectionTestFixtures", "ReflectionTestFixtures.rocksids.json") : IdentityPath]);
            // 시험이 구성 프로필 전체를 넘기면(구성 매크로 포함) 기본 인자의 구성 매크로는 빼서 중복 정의가 되지 않게 한다.
            bool extraDefinesBuild = extra?.Any(argument => argument.Contains("ROCKS_BUILD_", StringComparison.Ordinal)) == true;
            foreach (string argument in options.ClangArguments)
            {
                if (extraDefinesBuild && argument.Contains("ROCKS_BUILD_", StringComparison.Ordinal))
                {
                    continue;
                }

                arguments.Add("--clang-arg=" + argument);
            }

            if (extra is not null)
            {
                arguments.AddRange(extra);
            }

            var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
            if (expectedCode is null)
            {
                Verify.Equal(0, result.ExitCode, result.StdErr);
            }
            else
            {
                Verify.That(result.ExitCode != 0, "invalid reflection input accepted");
                Verify.Contains(expectedCode, result.StdErr, "stable diagnostic code");
                Verify.Contains("hint:", result.StdErr.ToLowerInvariant(), "actionable diagnostic hint");
                Verify.That(Regex.IsMatch(result.StdErr, @":\d+:.*RMC", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1)), "file/line diagnostic missing:\n" + result.StdErr);
                if (reason is not null)
                {
                    Verify.That(result.StdErr.Contains(reason, StringComparison.OrdinalIgnoreCase), "diagnostic reason missing: " + reason + "\n" + result.StdErr);
                }
            }
        }

        public async Task PrefixDifferentFileAsync()
        {
            string source = Preamble + "ROCKS_CLASS()\n#include <Other.h>\n}\n";
            File.WriteAllText(temporary.FilePath("Other.h"), "struct Value : public rocks::Object {};\n");
            await RejectAsync(source, "RMC2000", "ROCKS_CLASS");
        }

        // 생성 헤더는 원본을 <철자>로 포함하므로 --include 철자는 --include-dir 검색 순서로 원본에 풀려야 한다.
        // 검색 순서에서 앞선 다른 헤더가 같은 철자를 가리면 거부하고(이전 출력 보존), 지운 뒤에는 받아들인다.
        public async Task OutputDirectoryBindingAsync()
        {
            string sourceDirectory = temporary.FilePath("Source");
            string shadowDirectory = temporary.FilePath("Shadow");
            Directory.CreateDirectory(sourceDirectory);
            Directory.CreateDirectory(shadowDirectory);
            string author = Path.Combine(sourceDirectory, "Probe.h");
            string shadow = Path.Combine(shadowDirectory, "Probe.h");
            string output = MetaCompilerArguments.GeneratedPath(temporary.Root, "Probe.h");
            File.WriteAllText(author, Declaration());
            File.WriteAllText(shadow, Declaration(extra: "void Shadow() noexcept {}"));
            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            File.WriteAllText(output, "previous accepted output\n");
            async Task<ProcessResult> Run()
            {
                List<string> arguments = [];
                string executable = generator;
                if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                {
                    executable = options.Optional("dotnet") ?? "dotnet";
                    arguments.Add(generator);
                }

                arguments.AddRange([.. MetaCompilerArguments.Single(temporary.Root, author, "Probe.h"), "--compiler", compiler]);
                IEnumerable<string> searchOrder = File.Exists(shadow) ? [shadowDirectory, sourceDirectory] : [sourceDirectory];
                foreach (string include in searchOrder.Concat(Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                    .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists)))
                {
                    arguments.AddRange(["--include-dir", include]);
                }

                arguments.AddRange(["--identity-manifest", IdentityPath]);
                foreach (string argument in options.ClangArguments)
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
            }

            bool ValidationLeaked() => Directory.EnumerateDirectories(temporary.Root, ".rocks-meta-validation-*").Any();
            var rejected = await Run();
            Verify.That(rejected.ExitCode != 0, "shadowing header accepted");
            Verify.Contains("RMC1000", rejected.StdErr, "shadow error code");
            Verify.Contains(shadow, rejected.StdErr, "actual shadow path");
            Verify.Equal("previous accepted output\n", File.ReadAllText(output), "shadow failure changed accepted output");
            File.Delete(shadow);
            var accepted = await Run();
            Verify.Equal(0, accepted.ExitCode, "original header not selected after shadow removal: " + accepted.StdErr);
            Verify.That(!ValidationLeaked(), "successful validation temporary directory leaked");
            string validOutput = File.ReadAllText(output);
            File.WriteAllText(author, Preamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object { Value() {} };\n}\n");
            var invalid = await Run();
            Verify.That(invalid.ExitCode != 0, "invalid generated contract accepted");
            Verify.Contains("RMC3000", invalid.StdErr, "generated contract rejection code");
            Verify.Equal(validOutput, File.ReadAllText(output), "failed validation published output");
            Verify.That(!ValidationLeaked(), "failed validation temporary directory leaked");
        }

        public async Task SymlinkLocationsAsync()
        {
            string real = temporary.FilePath("real");
            string alias = temporary.FilePath("alias");
            string shadow = temporary.FilePath("shadow");
            Directory.CreateDirectory(real);
            Directory.CreateDirectory(shadow);
            try
            {
                Directory.CreateSymbolicLink(alias, real);
            }
            catch (Exception error) when (OperatingSystem.IsWindows() && error is UnauthorizedAccessException or IOException)
            {
                Console.WriteLine("[SKIP] Meta symlink fixture: Windows account cannot create symbolic links: " + error.Message);
                return;
            }

            string original = Declaration();
            string actualInput = Path.Combine(real, "Probe.h");
            string aliasInput = Path.Combine(alias, "Probe.h");
            File.WriteAllText(actualInput, original);
            File.WriteAllText(Path.Combine(shadow, "Probe.h"), Declaration(extra: "void Shadow() noexcept {}"));
            async Task<ProcessResult> Run(string input, string outputRoot, params string[] includeDirs)
            {
                List<string> arguments = [];
                string executable = generator;
                if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                {
                    executable = options.Optional("dotnet") ?? "dotnet";
                    arguments.Add(generator);
                }

                arguments.AddRange(["--module", MetaCompilerArguments.Module, "--input", input, "--include", "Probe.h", "--output-root", outputRoot,
                    "--descriptor", MetaCompilerArguments.Descriptor(temporary.Root), "--compiler", compiler]);
                foreach (string include in includeDirs.Concat(Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                    .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists)))
                {
                    arguments.AddRange(["--include-dir", include]);
                }

                arguments.AddRange(["--identity-manifest", IdentityPath]);
                foreach (string argument in options.ClangArguments)
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                return await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
            }

            var valid = await Run(aliasInput, MetaCompilerArguments.OutputRoot(temporary.Root), real);
            Verify.Equal(0, valid.ExitCode, "same file through symlinked parent rejected: " + valid.StdErr);
            var shadowed = await Run(aliasInput, MetaCompilerArguments.OutputRoot(temporary.Root), shadow, real);
            Verify.That(shadowed.ExitCode != 0, "different shadow header accepted");
            Verify.Contains("resolved input=", shadowed.StdErr, "resolved input path missing");
            Verify.Contains("resolved target=", shadowed.StdErr, "resolved shadow target missing");
            Verify.Contains(Path.Combine(shadow, "Probe.h"), shadowed.StdErr, "shadow header path missing");
            // 출력 루트가 (symlink 별칭으로) 입력 헤더를 담으면 거부한다(원본 헤더를 덮어쓰지 않는다).
            var sameOutput = await Run(actualInput, alias, real);
            Verify.That(sameOutput.ExitCode != 0, "input/output symlink alias was accepted");
            Verify.Contains("RMC1000", sameOutput.StdErr, "same input/output rejection code");
            Verify.Equal(original, File.ReadAllText(actualInput), "source header changed after rejected output alias");
        }

        public async Task SourceNotesAsync()
        {
            string input = temporary.FilePath("Probe.h");
            string source = "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "namespace a { ROCKS_CLASS()\nstruct Thing : public rocks::Object { Thing() {} }; }\n" +
                "namespace b { ROCKS_CLASS()\nstruct Thing : public rocks::Object {}; }\n" +
                "namespace a { ROCKS_CLASS()\nstruct ThingExtended : public rocks::Object {}; }\n";
            File.WriteAllText(input, source);
            List<string> arguments = [];
            string executable = generator;
            if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
            {
                executable = options.Optional("dotnet") ?? "dotnet";
                arguments.Add(generator);
            }

            arguments.AddRange([.. MetaCompilerArguments.Single(temporary.Root, input, "Probe.h"), "--compiler", compiler]);
            AddDefaultIncludes(arguments);
            arguments.AddRange(["--identity-manifest", IdentityPath]);
            foreach (string argument in options.ClangArguments)
            {
                arguments.Add("--clang-arg=" + argument);
            }

            var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
            Verify.That(result.ExitCode != 0, "throwing constructor was accepted");
            Verify.Contains("RMC3000", result.StdErr, "generated contract failure");
            Verify.Contains("diagnostic references CLASS a::Thing", result.StdErr, "qualified class reference");
            Verify.That(!result.StdErr.Contains("note: diagnostic references CLASS b::Thing", StringComparison.Ordinal),
                "unrelated namespaced class was falsely identified");
            Verify.That(!result.StdErr.Contains("note: diagnostic references CLASS a::ThingExtended", StringComparison.Ordinal),
                "prefix name was falsely identified");
        }

        public async Task NestedLocationAsync()
        {
            string main = temporary.FilePath("Main.h");
            string a = temporary.FilePath("First.h");
            string middle = temporary.FilePath("Middle.h");
            string bad = temporary.FilePath("Deep.h");
            File.WriteAllText(main, "#pragma once\n#include <First.h>\n#include <Middle.h>\n");
            File.WriteAllText(a, "#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n" +
                "namespace first { ROCKS_CLASS()\nstruct Good : public rocks::Object {}; }\n");
            File.WriteAllText(middle, "#pragma once\n#include <Deep.h>\n");
            var fixtures = new[]
            {
                ("#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n\n\nnamespace example {\n" +
                    "ROCKS_CLASS(UnknownClassFlag)\nstruct Invalid : public rocks::Object {};\n}\n", 7),
                ("#pragma once\n#include <Meta/Annotations.h>\n#include <Object/Object.h>\n\nnamespace example {\n" +
                    "ROCKS_CLASS()\nstruct Invalid : public rocks::Object {\n" +
                    "ROCKS_PROPERTY() int valid;\n\n" +
                    "ROCKS_PROPERTY(ReadOnyl) int bad;\n};\n}\n", 10)
            };
            foreach (var (source, line) in fixtures)
            {
                File.WriteAllText(bad, source);
                List<string> arguments = [];
                string executable = generator;
                if (generator.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                {
                    executable = options.Optional("dotnet") ?? "dotnet";
                    arguments.Add(generator);
                }

                // 리플렉션 선언은 모듈 입력 헤더에 있어야 한다: 오류 선언이 있는 Deep.h를 입력으로 두고 진단 위치를 본다.
                arguments.AddRange(["--module", MetaCompilerArguments.Module, "--input", a, "--include", "First.h", "--input", bad, "--include", "Deep.h",
                    "--output-root", MetaCompilerArguments.OutputRoot(temporary.Root), "--descriptor", MetaCompilerArguments.Descriptor(temporary.Root), "--compiler", compiler]);
                AddDefaultIncludes(arguments);
                arguments.AddRange(["--identity-manifest", IdentityPath]);
                foreach (string argument in options.ClangArguments)
                {
                    arguments.Add("--clang-arg=" + argument);
                }

                var result = await ProcessRunner.RunAsync(new(executable, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
                Verify.That(result.ExitCode != 0, "invalid nested declaration was accepted");
                Verify.Contains($"{bad}:{line}: error RMC2000".Replace('\\', '/'), result.StdErr.Replace('\\', '/'), "nested source file and physical line");
                Verify.Contains("hint:", result.StdErr, "nested declaration correction guidance");
            }
        }

        public async Task RejectAsync(string source, string code, string reason)
        {
            byte[] previous = Encoding.UTF8.GetBytes("previous accepted output\n");
            Directory.CreateDirectory(Path.GetDirectoryName(Output)!);
            File.WriteAllBytes(Output, previous);
            await GenerateAsync(source, expectedCode: code, reason: reason);
            Verify.That(File.ReadAllBytes(Output).AsSpan().SequenceEqual(previous), "rejected input changed previous accepted output");
            Verify.That(!Directory.EnumerateFiles(temporary.Root, "*.tmp*").Any(), "temporary generated output leaked");
        }

        private void AddDefaultIncludes(List<string> arguments)
        {
            foreach (string directory in Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal)
                .Concat([FixturePublic, options.Root, temporary.Root]))
            {
                arguments.AddRange(["--include-dir", directory]);
            }
        }

        private string FixturePublic => Path.Combine(options.Root, "Core", "Source", "Developer", "ReflectionTestFixtures", "Public");

        public void Dispose() => temporary.Dispose();
    }
}

// 각 테스트는 실제 도구와 동일한 읽기 전용 명세를 제공한다. 테스트 데이터의 ID는
// 저작 API가 한 번 발급하며 선언 본문에 GUID를 직접 넣지 않는다.
internal static class MetaProbeIdentity
{
    public static void Create(string path)
    {
        StableIdManifest.Update(path, true, manifest =>
        {
            foreach (string name in new[] { "probe::Value", "probe::Outer::Inner", "probe::Object", "probe::Parent",
                "first::Good", "example::Invalid", "a::Thing", "b::Thing", "a::ThingExtended", "probe::Mode", "probe::Value::Nested" })
            {
                manifest.Register("type", name);
            }

            foreach (string name in new[] { "probe::Value::count", "probe::Value::next", "probe::Value::other", "probe::Object::value",
                "probe::Value::mode", "probe::Value::nested",
                "example::Invalid::valid", "example::Invalid::bad" })
            {
                manifest.Register("field", name);
            }

            return 0;
        });
    }
}
