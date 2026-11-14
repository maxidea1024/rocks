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

    // B4b struct 시험 원본이다: enum 멤버·String·Migrate(Version 2)를 가진 Leaf, Leaf·EditorOnly·NATIVE를 가진 Middle,
    // GENERATED_BODY·private 멤버·Required 참조를 가진 참조 struct Holder(class 키워드), Holder를 담아 참조 형질이
    // 전파되는 Outer, NATIVE WorldOnly 소유자 형질의 WorldBits, 그것들을 쓰는 클래스(NATIVE 리플렉션 struct 멤버 포함).
    private const string StructSource = "#include <Core/Containers/Array.h>\n" + Preamble + "struct Value;\n" +
        "struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
        "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off = 0, On = 1 };\n" +
        "ROCKS_STRUCT(Version = 2)\nstruct Leaf {\nexplicit Leaf(rocks::Allocator& allocator) noexcept : label(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::uint32 count = 0;\nROCKS_PROPERTY() rocks::String label;\nROCKS_PROPERTY() Mode mode = Mode::Off;\n" +
        "static rocks::Result<rocks::Array<rocks::uint8>> Migrate(rocks::Allocator& allocator, rocks::TypeId, rocks::uint32, rocks::uint32, " +
        "rocks::Span<const rocks::uint8>) noexcept { return rocks::Result<rocks::Array<rocks::uint8>>::Success(rocks::Array<rocks::uint8>(allocator)); }\n};\n" +
        "ROCKS_STRUCT()\nstruct Middle {\nexplicit Middle(rocks::Allocator& allocator) noexcept : leaf(allocator) {}\n" +
        "ROCKS_PROPERTY() Leaf leaf;\nROCKS_PROPERTY(EditorOnly) rocks::int64 offset = 0;\nROCKS_NATIVE() rocks::uint32 cache = 0;\n};\n" +
        "ROCKS_STRUCT()\nclass Holder {\nROCKS_GENERATED_BODY()\npublic:\nexplicit Holder(rocks::Allocator& allocator) noexcept : middle(allocator) {}\n" +
        "rocks::uint8 Flag() const noexcept { return flag; }\nROCKS_PROPERTY(Required) rocks::ObjectRef<Value> target;\nROCKS_PROPERTY() Middle middle;\n" +
        "private:\nROCKS_PROPERTY() rocks::uint8 flag = 0;\n};\n" +
        "ROCKS_STRUCT()\nstruct Outer {\nexplicit Outer(rocks::Allocator& allocator) noexcept : holder(allocator) {}\nROCKS_PROPERTY() Holder holder;\n};\n" +
        "ROCKS_STRUCT()\nstruct WorldBits {\nROCKS_NATIVE() Lease lease;\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
        "explicit Value(rocks::Allocator& allocator) noexcept : holder(allocator), extra(allocator), scratch(allocator) {}\n" +
        "ROCKS_PROPERTY() Holder holder;\nROCKS_PROPERTY() rocks::uint32 plain = 0;\nROCKS_PROPERTY(EditorOnly) Leaf extra;\nROCKS_NATIVE() Leaf scratch;\n};\n" +
        "ROCKS_CLASS(WorldOnly)\nstruct WorldValue : public rocks::Object {\nROCKS_PROPERTY() WorldBits bits;\n};\n}\n";

    private const string StructLeaf = "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n";

    // B4c Binds 시험은 CoreTypeBindings 모듈로 실행한다(Binds는 그 모듈에서만 받는다).
    private const string BindsModule = "CoreTypeBindings";

    private const string BindsPreamble = "#include <Core/Math/Vector3.h>\n" + Preamble;

    private const string BindsVec3 = "ROCKS_STRUCT(Binds = \"rocks::Vec3\")\nstruct Vec3Binding {\n" +
        "ROCKS_PROPERTY() float x = 0;\nROCKS_PROPERTY() float y = 0;\nROCKS_PROPERTY() float z = 0;\n};\n";

    private const string StructWorldBits = "struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
        "ROCKS_STRUCT()\nstruct WorldBits {\nROCKS_NATIVE() Lease lease;\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n";

    // B5c 배열 시험의 include 앞부분이다(컨테이너 세 종류와 소프트 참조).
    private const string ArrayPreamble = "#include <Core/Containers/Array.h>\n#include <Object/ObjectRefArray.h>\n#include <Object/StructRefArray.h>\n" +
        "#include <Object/SoftObjectRef.h>\n" + Preamble;

    // B5c 배열 원소 struct다: 배열 멤버를 가진 참조 없는 Leaf, 배열 멤버를 가진 참조 struct Holder, Registry 컨테이너를 담는 인라인
    // 참조 struct Bundle(ContainsReferenceArray).
    private const string ArrayStructs = "ROCKS_STRUCT()\nstruct Leaf {\nexplicit Leaf(rocks::Allocator& allocator) noexcept : codes(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::uint32 count = 0;\nROCKS_PROPERTY() rocks::Array<rocks::uint16> codes;\n};\n" +
        "ROCKS_STRUCT()\nstruct Holder {\nexplicit Holder(rocks::Allocator& allocator) noexcept : weights(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::ObjectRef<Value> target;\nROCKS_PROPERTY() rocks::Array<float> weights;\n};\n" +
        "ROCKS_STRUCT()\nstruct Bundle {\nexplicit Bundle(rocks::Allocator& allocator) noexcept : refs(allocator), holders(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::ObjectRefArray<Value> refs;\nROCKS_PROPERTY() rocks::StructRefArray<Holder> holders;\n};\n";

    // B5c 배열 시험 원본이다: 모든 컨테이너(Array·ObjectRefArray·ObjectRefArray<>·StructRefArray)와 원소 종류(스칼라·String·
    // enum·SoftObjectRef·참조 없는 struct), MaxElements 기본·명시·최댓값, Required 참조 배열, 인라인 struct 안 컨테이너, 세 형질을
    // 모두 가진 WorldBundle(NATIVE WorldOnly 소유자 형질과 Bundle 멤버 — flags 쓰기 순서).
    private const string ArraySource = ArrayPreamble + "struct Value;\n" +
        "struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
        "ROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off = 0, On = 1 };\n" + ArrayStructs +
        "ROCKS_STRUCT()\nstruct WorldBundle {\nexplicit WorldBundle(rocks::Allocator& allocator) noexcept : inner(allocator) {}\n" +
        "ROCKS_NATIVE() Lease lease;\nROCKS_PROPERTY() Bundle inner;\n};\n" +
        "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
        "explicit Value(rocks::Allocator& allocator) noexcept : ints(allocator), names(allocator), modes(allocator), softs(allocator), " +
        "leaves(allocator), refs(allocator), anyRefs(allocator), holders(allocator), bundle(allocator) {}\n" +
        "ROCKS_PROPERTY() rocks::Array<rocks::int32> ints;\nROCKS_PROPERTY(MaxElements = 8) rocks::Array<rocks::String> names;\n" +
        "ROCKS_PROPERTY(MaxElements = 1048576) rocks::Array<Mode> modes;\nROCKS_PROPERTY(Required) rocks::Array<rocks::SoftObjectRef<Value>> softs;\n" +
        "ROCKS_PROPERTY() rocks::Array<Leaf> leaves;\nROCKS_PROPERTY(Required) rocks::ObjectRefArray<Value> refs;\n" +
        "ROCKS_PROPERTY() rocks::ObjectRefArray<> anyRefs;\nROCKS_PROPERTY() rocks::StructRefArray<Holder> holders;\nROCKS_PROPERTY() Bundle bundle;\n};\n" +
        "ROCKS_CLASS(WorldOnly)\nstruct WorldValue : public rocks::Object {\nexplicit WorldValue(rocks::Allocator& allocator) noexcept : bundle(allocator) {}\n" +
        "ROCKS_PROPERTY() WorldBundle bundle;\n};\n}\n";

    // 배열을 거친 깊이 depth의 struct 사슬이다(Chain1 ← Array<Chain1>을 담는 Chain2 ← …).
    private static string ArrayChainSource(int depth) => ArrayPreamble + "ROCKS_STRUCT()\nstruct Chain1 {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        string.Concat(Enumerable.Range(2, depth - 1).Select(level => $"ROCKS_STRUCT()\nstruct Chain{level} {{\n" +
            $"explicit Chain{level}(rocks::Allocator& allocator) noexcept : inner(allocator) {{}}\nROCKS_PROPERTY() rocks::Array<Chain{level - 1}> inner;\n}};\n")) + "}\n";

    // 깊이 depth의 struct 사슬이다(Chain1 ← Chain2 ← … ← Chain<depth>).
    private static string ChainSource(int depth) => Preamble + "ROCKS_STRUCT()\nstruct Chain1 {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        string.Concat(Enumerable.Range(2, depth - 1).Select(level => $"ROCKS_STRUCT()\nstruct Chain{level} {{\nROCKS_PROPERTY() Chain{level - 1} inner;\n}};\n")) + "}\n";

    // 공백을 모두 지운 텍스트다(clang-format 줄바꿈과 무관하게 생성 코드 조각을 찾는다).
    private static string Compact(string text) => Regex.Replace(text, @"\s+", "", RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));

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
        NativeClassCompilerTests.Register(suite, options, actualEnvironment);
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
        // B4b: ROCKS_STRUCT가 descriptor·생성 특수화(형질·깊이·Trace·Ops·Migrate)·클래스 struct 필드·집계 등록 순서에
        // 닿는다(Docs/Design/ReflectionFoundations.ko.md 4.6절).
        suite.Add("Meta.StructSchemaReachesDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(StructSource);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            string[] Flags(string name) => Type(name).GetProperty("flags").EnumerateArray().Select(flag => flag.GetString()!).ToArray();
            System.Text.Json.JsonElement Field(string type, string name) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement leaf = Type("probe::Leaf");
            Verify.Equal("struct", leaf.GetProperty("kind").GetString(), "struct descriptor kind");
            Verify.Equal(2, leaf.GetProperty("version").GetInt32(), "struct Version specifier");
            Verify.Equal("", leaf.GetProperty("binds").GetString(), "binds is empty until Core value type bindings");
            Verify.That(!leaf.TryGetProperty("base", out _), "struct descriptor has no base");
            Verify.Equal(0, Flags("probe::Leaf").Length, "plain struct has no trait flags");
            Verify.That(Flags("probe::Holder").SequenceEqual(["ContainsStrongReference"]), "reference struct flag");
            Verify.That(Flags("probe::Outer").SequenceEqual(["ContainsStrongReference"]), "reference trait propagates through a member struct");
            Verify.That(Flags("probe::WorldBits").SequenceEqual(["RequiresWorldOnlyOwner"]), "NATIVE WorldOnly owner trait becomes a struct trait");
            Verify.Equal(2, Type("probe::Middle").GetProperty("fields").GetArrayLength(), "NATIVE struct members are not schema fields");
            Verify.Equal("Struct", Field("probe::Middle", "leaf").GetProperty("value").GetProperty("kind").GetString(), "struct member kind");
            Verify.Equal(leaf.GetProperty("id").GetString(), Field("probe::Middle", "leaf").GetProperty("value").GetProperty("type").GetString(), "struct member target");
            Verify.Equal("Value", Field("probe::Value", "holder").GetProperty("fieldKind").GetString(), "class struct field is a value field");
            Verify.Equal(Type("probe::Holder").GetProperty("id").GetString(), Field("probe::Value", "holder").GetProperty("value").GetProperty("type").GetString(), "class struct field target");

            // 생성물은 clang-format을 거치므로 공백을 지운 텍스트로 비교한다(줄바꿈 위치에 기대지 않는다).
            string generated = Compact(File.ReadAllText(probe.Output));
            foreach ((string expected, string reason) in new[]
            {
                ("struct GeneratedType<::probe::Leaf>", "struct specialization"),
                ("static void Trace(const ::probe::Leaf&, TraceVisitor&) noexcept {}", "plain struct has an empty Trace"),
                ("visitor.Visit(value.*(&::probe::Holder::target));", "reference struct visits its ObjectRef member"),
                ("GeneratedType<::probe::Middle>::Trace(value.*(&::probe::Holder::middle), visitor);", "reference struct traces member structs"),
                ("GeneratedType<::probe::Holder>::Trace(value.*(&::probe::Value::holder), visitor);", "class Trace calls struct Trace"),
                ("registry.RegisterStruct(Schema(), Ops(Move(module)))", "struct Register"),
                ("MakeRuntimeStructOps<::probe::Leaf>(Move(module))", "struct ops helper (Migrate detection)"),
                ("detail::StructFieldOps<&::probe::Holder::flag>", "private member thunk through GENERATED_BODY"),
                ("static_assert(Depth <= ::rocks::MaxStructDepth", "depth static_assert"),
                ("static constexpr uint32 Depth = 3u;", "three-level depth"),
                ("ops.trace = +[](const void* value, TraceVisitor& visitor) noexcept { Trace(*static_cast<const ::probe::Holder*>(value), visitor); };", "reference struct trace op"),
                ("ops.requiresWorldOnlyOwner = RequiresWorldOnlyOwner;", "struct WorldOnly owner trait"),
                ("static_assert(!GeneratedType<::probe::Holder>::RequiresWorldOnlyOwner", "non-WorldOnly class checks struct traits"),
                ("static_assert(!GeneratedType<::probe::Leaf>::ContainsStrongReference", "NATIVE reflected struct member is checked by its trait"),
                ("nullptr, {}, nullptr,", "class struct field has no read/write/writeStaged"),
                ("NativeTypeKey<::probe::Holder>()", "class struct field native key")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            Verify.That(!generated.Contains("!GeneratedType<::probe::WorldBits>::RequiresWorldOnlyOwner", StringComparison.Ordinal), "WorldOnly class needs no RMC2206 struct static_assert");
            Verify.That(!generated.Contains(Compact("Trace(*static_cast<const ::probe::Leaf*>(value), visitor)"), StringComparison.Ordinal), "plain struct has no trace op");
            int Position(string name) => generated.IndexOf($"structGeneratedType<::probe::{name}>", StringComparison.Ordinal);
            Verify.That(Position("Mode") < Position("Leaf") && Position("Leaf") < Position("Middle") && Position("Middle") < Position("Holder") &&
                Position("Holder") < Position("Outer") && Position("Outer") < Position("Value"), "specializations are topologically ordered");

            // 집계 등록 순서: enum → struct(순위 순, 같은 순위는 이름 순) → 클래스.
            string aggregate = Compact(File.ReadAllText(probe.Aggregate));
            int Registered(string name) => aggregate.IndexOf($"GeneratedType<::probe::{name}>::Schema()", StringComparison.Ordinal);
            Verify.Contains("RegisterStructIfAbsent", aggregate, "aggregate registers structs");
            Verify.That(Registered("Mode") >= 0 && Registered("Mode") < Registered("Leaf") && Registered("Leaf") < Registered("WorldBits") &&
                Registered("WorldBits") < Registered("Middle") && Registered("Middle") < Registered("Holder") && Registered("Holder") < Registered("Outer") &&
                Registered("Outer") < Registered("Value") && Registered("Value") < Registered("WorldValue"), "aggregate registration order:\n" + aggregate);
        });
        // B8a: 클래스 GeneratedType은 상속 필드까지 방문하는 static Trace를 내고 ops.trace는 그것을 감싼다(리플렉션 base를 둔
        // native 파생의 Trace 사슬이 리플렉션 조상에서 부른다 — Docs/Design/ReflectionFoundations.ko.md 9.1절). 방문할 필드가 없는
        // 클래스는 매개변수 이름 없는 빈 본문이다(MetaCompiler 검증 컴파일이 경고 오류로 사용하지 않는 매개변수를 잡는다).
        suite.Add("Meta.ClassStaticTraceWrapsOpsTrace", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "ROCKS_CLASS()\nstruct Box : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_CLASS()\nstruct Node : public rocks::Object {\nROCKS_PROPERTY() rocks::ObjectRef<Node> target;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public Node {\nROCKS_PROPERTY() rocks::ObjectRef<Box> plain;\n};\n}\n";
            await probe.GenerateAsync(source);
            string generated = Compact(File.ReadAllText(probe.Output));
            foreach ((string expected, string reason) in new[]
            {
                ("static void Trace(const ::probe::Box&, TraceVisitor&) noexcept {}", "class without traced fields has an unnamed empty Trace"),
                ("ops.trace = +[](const Object& object, TraceVisitor& visitor) noexcept { Trace(static_cast<const ::probe::Box&>(object), visitor); };",
                    "ops.trace wraps the empty Trace"),
                ("static void Trace(const ::probe::Node& value, TraceVisitor& visitor) noexcept { visitor.Visit(value.*(&::probe::Node::target)); }",
                    "base class Trace visits its reference field"),
                ("ops.trace = +[](const Object& object, TraceVisitor& visitor) noexcept { Trace(static_cast<const ::probe::Value&>(object), visitor); };",
                    "derived ops.trace wraps its Trace")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            int Count(string text, string needle) => (text.Length - text.Replace(needle, "", StringComparison.Ordinal).Length) / needle.Length;
            // 파생 클래스 Trace는 상속 필드와 자기 필드를 한 번씩 방문한다(필드 순서는 모델 순서를 따른다).
            string derivedHead = Compact("static void Trace(const ::probe::Value& value, TraceVisitor& visitor) noexcept {");
            int derivedStart = generated.IndexOf(derivedHead, StringComparison.Ordinal);
            Verify.That(derivedStart >= 0, "derived class Trace missing");
            int bodyStart = derivedStart + derivedHead.Length;
            string derivedBody = generated[bodyStart..generated.IndexOf('}', bodyStart)];
            Verify.Equal(2, Count(derivedBody, "visitor.Visit("), "derived class Trace visits two references: " + derivedBody);
            Verify.That(derivedBody.Contains(Compact("visitor.Visit(value.*(&::probe::Node::target));"), StringComparison.Ordinal) &&
                derivedBody.Contains(Compact("visitor.Visit(value.*(&::probe::Value::plain));"), StringComparison.Ordinal),
                "derived class Trace visits the inherited field too: " + derivedBody);
            Verify.Equal(3, Count(generated, Compact("static void Trace(const ::probe::")), "one static Trace per class");
            Verify.Equal(3, Count(generated, Compact("ops.trace = +[](const Object& object, TraceVisitor& visitor) noexcept { Trace(")), "every ops.trace wraps the static Trace");
            Verify.That(!generated.Contains("(void)object;", StringComparison.Ordinal) && !generated.Contains("constauto&typed", StringComparison.Ordinal),
                "no inline trace body remains in Ops()");
        });
        // NATIVE 멤버로 둔 리플렉션 struct도 생성 특수화가 static_assert로 쓰므로 먼저 나와야 한다(이름 순이면 Alpha가
        // 앞이지만 Zeta가 먼저 — 깊이는 둘 다 1이다). private Migrate는 GENERATED_BODY의 friend로 연결된다.
        suite.Add("Meta.StructNativeStructMemberOrderAndPrivateMigrate", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = "#include <Core/Containers/Array.h>\n" + Preamble +
                "ROCKS_STRUCT()\nstruct Zeta {\nROCKS_PROPERTY() rocks::int32 v = 0;\n};\n" +
                "ROCKS_STRUCT()\nclass Alpha {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY() rocks::int32 x = 0;\nROCKS_NATIVE() Zeta z;\n" +
                "private:\nstatic rocks::Result<rocks::Array<rocks::uint8>> Migrate(rocks::Allocator& allocator, rocks::TypeId, rocks::uint32, rocks::uint32, " +
                "rocks::Span<const rocks::uint8>) noexcept { return rocks::Result<rocks::Array<rocks::uint8>>::Success(rocks::Array<rocks::uint8>(allocator)); }\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Alpha alpha;\n};\n}\n";
            await probe.GenerateAsync(source);
            string generated = Compact(File.ReadAllText(probe.Output));
            int zeta = generated.IndexOf("structGeneratedType<::probe::Zeta>", StringComparison.Ordinal);
            int alpha = generated.IndexOf("structGeneratedType<::probe::Alpha>", StringComparison.Ordinal);
            Verify.That(zeta >= 0 && alpha > zeta, "NATIVE member struct is emitted before its user");
            Verify.That(generated.Contains(Compact("static_assert(!GeneratedType<::probe::Zeta>::ContainsStrongReference"), StringComparison.Ordinal), "NATIVE struct member trait static_assert");
            Verify.That(generated.Contains(Compact("ops.migrate = &::probe::Alpha::Migrate;"), StringComparison.Ordinal), "private Migrate is wired");
            Verify.That(!generated.Contains(Compact("ops.migrate = &::probe::Zeta::Migrate;"), StringComparison.Ordinal), "struct without Migrate has no migrate");
            string aggregate = Compact(File.ReadAllText(probe.Aggregate));
            Verify.That(aggregate.IndexOf("GeneratedType<::probe::Zeta>::Schema()", StringComparison.Ordinal) <
                aggregate.IndexOf("GeneratedType<::probe::Alpha>::Schema()", StringComparison.Ordinal), "aggregate registers the NATIVE member struct first");
        });
        // 깊이 16은 받고 17은 RMC2303이다(멤버 struct 사슬).
        suite.Add("Meta.StructNestingDepthIsLimited", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ChainSource(16));
            Verify.That(Compact(File.ReadAllText(probe.Output)).Contains(Compact("static constexpr uint32 Depth = 16u;"), StringComparison.Ordinal), "depth 16 is accepted");
            await probe.RejectAsync(ChainSource(17), "RMC2303", "nesting depth 17");
        });
        AddRejectGroup("Meta.StructCannotInherit", [
            Preamble + "struct Shared { rocks::int32 base = 0; };\nROCKS_STRUCT()\nstruct Leaf : Shared {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n",
            Preamble + "ROCKS_STRUCT()\nstruct Leaf : public rocks::Object {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n"],
            "RMC2301", "base class");
        AddRejectGroup("Meta.StructTemplatesAndPlacementAreRejected", [
            Preamble + "ROCKS_STRUCT()\ntemplate <class T> struct Leaf { ROCKS_PROPERTY() T count{}; };\n}\n",
            Preamble + "ROCKS_STRUCT()\nint intervening;\nstruct Leaf { ROCKS_PROPERTY() rocks::int32 count = 0; };\n}\n",
            Preamble + "void Test() {\nROCKS_STRUCT()\nstruct Leaf { rocks::int32 count = 0; };\n}\n}\n"],
            "RMC2000", "ROCKS_STRUCT");
        AddRejectGroup("Meta.ClassAndStructMarkersAreExclusive", [
            Preamble + "ROCKS_CLASS()\nROCKS_STRUCT()\nstruct Value : public rocks::Object {};\n}\n",
            Preamble + "ROCKS_STRUCT()\nROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "stacked");
        AddRejectGroup("Meta.StructMembersNeedPropertyOrNative", [
            Preamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\nrocks::int32 hidden = 0;\n};\n}\n"],
            "RMC2000", "PROPERTY or NATIVE");
        AddRejectGroup("Meta.StructSpecifiersAreRestricted", new[] { "WorldOnly", "Abstract", "Version=0", "Version=\"2\"", "Binds=1" }
            .Select(spec => Preamble + $"ROCKS_STRUCT({spec})\nstruct Leaf {{\nROCKS_PROPERTY() rocks::int32 count = 0;\n}};\n}}\n"), "RMC2000", "specifier");
        AddRejectGroup("Meta.StructIsNotABaseOrReferenceTarget", [
            Preamble.Replace("namespace probe {", "#include <Object/SoftObjectRef.h>\nnamespace probe {", StringComparison.Ordinal) + StructLeaf +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& allocator) noexcept : next(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::SoftObjectRef<Leaf> next;\n};\n}\n",
            Preamble + StructLeaf + "ROCKS_CLASS()\nstruct Value : public Leaf {};\n}\n",
            Preamble + StructLeaf + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::ObjectRef<Leaf> next;\n};\n}\n"],
            "RMC2003", "is a reflected struct");
        AddRejectGroup("Meta.ConstStructFieldIsUnsupported", [
            Preamble + StructLeaf + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() const Leaf leaf{};\n};\n}\n"],
            "RMC2000", "unsupported field type");
        // NATIVE 멤버로 둔 참조 struct는 AST 재귀 검사가 강한 참조를 찾는다(RMC2205 — 클래스·struct 모두).
        const string referenceHolder = "struct Value;\nROCKS_STRUCT()\nstruct Holder {\nROCKS_PROPERTY() rocks::ObjectRef<Value> target;\n};\n";
        AddRejectGroup("Meta.NativeReferenceStructIsRejected", [
            Preamble + referenceHolder + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_NATIVE() Holder cache;\n};\n}\n",
            Preamble + referenceHolder + "ROCKS_STRUCT()\nstruct Box {\nROCKS_PROPERTY() rocks::int32 count = 0;\nROCKS_NATIVE() Holder cache;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2205", "strong object reference");
        // WorldOnly 소유자 형질(NATIVE)은 struct에서 형질로 전파되고(중첩 포함), 그 struct를 필드로 둔 WorldOnly 아닌
        // 클래스가 RMC2206이다.
        AddRejectGroup("Meta.StructWorldOnlyTraitNeedsWorldOnlyClass", new[]
            {
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() WorldBits bits;\n};\n",
                "ROCKS_STRUCT()\nstruct Outer {\nROCKS_PROPERTY() WorldBits inner;\n};\nROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Outer bits;\n};\n",
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_NATIVE() WorldBits bits;\n};\n"
            }.Select(declaration => Preamble + StructWorldBits + declaration + "}\n"),
            "RMC2206", "WorldOnly owner");
        // Migrate라는 이름이 있는데 시그니처가 다르면 생성 코드 검증 컴파일이 거부한다(조용히 마이그레이션 없이 등록되지 않는다).
        AddRejectGroup("Meta.StructMigrateSignatureIsChecked", new[]
            {
                "static void Migrate() noexcept {}",
                // noexcept가 없다.
                "static rocks::Result<rocks::Array<rocks::uint8>> Migrate(rocks::Allocator& allocator, rocks::TypeId, rocks::uint32, rocks::uint32, " +
                    "rocks::Span<const rocks::uint8>) { return rocks::Result<rocks::Array<rocks::uint8>>::Success(rocks::Array<rocks::uint8>(allocator)); }",
                // static이 아니다.
                "rocks::Result<rocks::Array<rocks::uint8>> Migrate(rocks::Allocator& allocator, rocks::TypeId, rocks::uint32, rocks::uint32, " +
                    "rocks::Span<const rocks::uint8>) noexcept { return rocks::Result<rocks::Array<rocks::uint8>>::Success(rocks::Array<rocks::uint8>(allocator)); }"
            }.Select(migrate => "#include <Core/Containers/Array.h>\n" + Preamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n" +
                migrate + "\n};\n}\n"), "RMC3000", "Migrate");
        AddRejectGroup("Meta.PrivateStructMigrateNeedsGeneratedBody", [
            Preamble + "ROCKS_STRUCT()\nclass Leaf {\npublic:\nROCKS_PROPERTY() rocks::int32 count = 0;\nprivate:\nstatic void Migrate() noexcept {}\n};\n}\n"],
            "RMC2000", "Migrate requires ROCKS_GENERATED_BODY");
        // 바깥에서 이름을 쓰는 생성 코드가 접근할 수 있도록 중첩 리플렉션 struct는 public 구역에 둔다.
        AddRejectGroup("Meta.NestedReflectedStructMustBePublic", [
            Preamble + "class Outer {\nROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "must be public");
        // B4c: Binds 미러는 별칭을 Clang 정규 철자로 풀고(rocks::Vec3 → rocks::Vector3<float>), 생성 코드는 실제 타입의
        // GeneratedType을 내며, 실제 타입 필드(별칭 포함)가 미러로 풀린다. 다른 미러에 의존하는 미러는 그 뒤에 나온다.
        suite.Add("Meta.BindsResolveAliasesAndReflectTheRealType", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler, BindsModule);
            string source = BindsPreamble + BindsVec3 +
                "ROCKS_STRUCT(Binds = \"rocks::Vector3<double>\")\nstruct Vec3dBinding {\nROCKS_PROPERTY() double x = 0;\nROCKS_PROPERTY() double y = 0;\nROCKS_PROPERTY() double z = 0;\n};\n" +
                "struct Pair {\nrocks::Vec3 a;\nrocks::Vec3d b;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::Pair\")\nstruct PairBinding {\nROCKS_PROPERTY() rocks::Vec3 a;\nROCKS_PROPERTY() rocks::Vector3<double> b;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Vec3 position;\nROCKS_PROPERTY() Pair pair;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            string Target(string type, string field) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == field).GetProperty("value").GetProperty("type").GetString()!;
            Verify.Equal("rocks::Vector3<float>", Type("probe::Vec3Binding").GetProperty("binds").GetString(), "alias Binds is normalized by Clang");
            Verify.Equal("rocks::Vector3<double>", Type("probe::Vec3dBinding").GetProperty("binds").GetString(), "template Binds spelling");
            Verify.Equal("probe::Pair", Type("probe::PairBinding").GetProperty("binds").GetString(), "Binds of a probe type");
            Verify.Equal(Type("probe::Vec3Binding").GetProperty("id").GetString(), Target("probe::Value", "position"), "alias field resolves to the mirror");
            Verify.Equal(Type("probe::PairBinding").GetProperty("id").GetString(), Target("probe::Value", "pair"), "real type field resolves to the mirror");
            Verify.Equal(Type("probe::Vec3dBinding").GetProperty("id").GetString(), Target("probe::PairBinding", "b"), "mirror member resolves to another mirror");
            string generated = Compact(File.ReadAllText(probe.Output));
            foreach ((string expected, string reason) in new[]
            {
                ("struct GeneratedType<::rocks::Vector3<float>>", "specialization of the real type"),
                ("Span<const char>(\"probe::Vec3Binding\", 18)", "schema name is the mirror name"),
                ("using BoundType = ::rocks::Vector3<float>;", "verification alias of the real type"),
                ("static_assert(__is_same(decltype(BoundType::x), float)", "member type static_assert"),
                ("static_assert(__is_same(decltype(&BoundType::x), float (BoundType::*))", "member declared in the real type itself"),
                ("static_assert(__is_standard_layout(BoundType)", "standard layout for the order check"),
                ("static_assert(ROCKS_OFFSET_OF(BoundType, x) < ROCKS_OFFSET_OF(BoundType, y)", "declaration order check"),
                ("[[maybe_unused]] const auto& [m0, m1, m2] = value;", "structured binding count check"),
                ("detail::StructFieldOps<&::probe::Pair::b>", "member thunk of the real type"),
                ("MakeRuntimeStructOps<::probe::Pair>", "construct and destroy the real type"),
                ("NativeTypeKey<::rocks::Vector3<float>>()", "class field native key of the real type"),
                ("GeneratedType<::rocks::Vector3<double>>::Depth < Depth", "mirror member uses the real type specialization")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            Verify.That(!generated.Contains("GeneratedType<::probe::Vec3Binding>", StringComparison.Ordinal), "no specialization of the mirror type");
            Verify.That(generated.IndexOf("structGeneratedType<::rocks::Vector3<double>>", StringComparison.Ordinal) <
                generated.IndexOf("structGeneratedType<::probe::Pair>", StringComparison.Ordinal), "mirrors used by a mirror come first");
        });
        AddRejectGroup("Meta.BindsIsAllowedOnlyInCoreTypeBindings", [BindsPreamble + BindsVec3 + "}\n"], "RMC2004", "allowed only in module CoreTypeBindings");
        // 별칭·앞의 ::·템플릿 인자 공백은 같은 정규 철자로 모이므로 같은 실제 타입의 두 번째 미러다.
        AddRejectGroup("Meta.BindsConflictsAreRejected", [
            BindsPreamble + BindsVec3 + BindsVec3.Replace("Vec3Binding", "OtherBinding", StringComparison.Ordinal).Replace("rocks::Vec3", "rocks::Vector3<float>", StringComparison.Ordinal) + "}\n",
            BindsPreamble + BindsVec3.Replace("rocks::Vec3", "::rocks::Vec3", StringComparison.Ordinal) +
                BindsVec3.Replace("Vec3Binding", "OtherBinding", StringComparison.Ordinal).Replace("rocks::Vec3", "rocks::Vector3< float >", StringComparison.Ordinal) + "}\n",
            BindsPreamble + StructLeaf + "ROCKS_STRUCT(Binds = \"probe::Leaf\")\nstruct Vec3Binding {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n"],
            "RMC2004", "binds", BindsModule);
        AddRejectGroup("Meta.BindsTargetMustBeACompleteClass", [
            BindsPreamble + BindsVec3.Replace("rocks::Vec3", "rocks::uint8", StringComparison.Ordinal) + "}\n",
            BindsPreamble + BindsVec3.Replace("rocks::Vec3", "probe::Missing", StringComparison.Ordinal) + "}\n",
            BindsPreamble + "struct Later;\n" + BindsVec3.Replace("rocks::Vec3", "probe::Later", StringComparison.Ordinal) + "}\n"],
            "RMC3000", "does not name a complete class type", BindsModule);
        // 철자 문법, 정규 철자가 descriptor binds 문법 밖(별칭으로 묶은 Fixed<-3>), 미러의 NATIVE 멤버·Migrate·필드 없음.
        AddRejectGroup("Meta.BindsSpellingAndMirrorMembersAreRestricted", [
            BindsPreamble + BindsVec3.Replace("rocks::Vec3", "rocks::Vec3; int x", StringComparison.Ordinal) + "}\n",
            BindsPreamble + "template <int N>\nstruct Fixed {\nrocks::int32 v = 0;\n};\nusing FixedAlias = Fixed<-3>;\n" +
                "ROCKS_STRUCT(Binds = \"probe::FixedAlias\")\nstruct FixedBinding {\nROCKS_PROPERTY() rocks::int32 v = 0;\n};\n}\n",
            BindsPreamble + BindsVec3.Replace("ROCKS_PROPERTY() float z = 0;", "ROCKS_PROPERTY() float z = 0;\nROCKS_NATIVE() rocks::int32 cache = 0;", StringComparison.Ordinal) + "}\n",
            BindsPreamble + BindsVec3.Replace("ROCKS_PROPERTY() float z = 0;", "ROCKS_PROPERTY() float z = 0;\nstatic void Migrate() noexcept {}", StringComparison.Ordinal) + "}\n",
            BindsPreamble + "ROCKS_STRUCT(Binds = \"rocks::Vec3\")\nstruct Vec3Binding {};\n}\n"],
            "RMC2000", "Binds", BindsModule);
        // 미러 타입은 필드 형식으로 쓰지 않는다(실제 타입을 쓴다).
        AddRejectGroup("Meta.BindsMirrorIsNotAFieldType", [
            BindsPreamble + BindsVec3 + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Vec3Binding position;\n};\n}\n"],
            "RMC2000", "is a Binds mirror declaration", BindsModule);
        // CoreTypeBindings에 의존하지 않고 Core 값 타입 필드를 쓰면 의존을 더하라고 안내한다.
        AddRejectGroup("Meta.CoreValueFieldWithoutBindingsHintsTheDependency", [
            BindsPreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Vec3 position;\n};\n}\n"],
            "RMC2000", "depend on CoreTypeBindings");
        // 미러가 자기가 묶는 실제 타입을 값으로 담으면(직접·서로) C++에서도 struct 순환이다(RMC2302).
        AddRejectGroup("Meta.BindsCycleIsRejected", [
            BindsPreamble + "struct Real {\nrocks::int32 a = 0;\n};\nROCKS_STRUCT(Binds = \"probe::Real\")\nstruct RealBinding {\nROCKS_PROPERTY() Real inner;\n};\n}\n",
            BindsPreamble + "struct RealA {\nrocks::int32 a = 0;\n};\nstruct RealB {\nrocks::int32 b = 0;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::RealA\")\nstruct ABinding {\nROCKS_PROPERTY() RealB inner;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::RealB\")\nstruct BBinding {\nROCKS_PROPERTY() RealA inner;\n};\n}\n"],
            "RMC2302", "cycle", BindsModule);
        // 미러와 실제 타입의 대조 실패는 생성 코드 검증 컴파일이 거부한다: 멤버 이름·형식·개수·순서(같은 형식 멤버끼리
        // 바뀐 순서 포함), private 데이터 멤버, base에만 있는 멤버.
        AddRejectGroup("Meta.BindsMismatchFailsToCompile", new[]
            {
                BindsVec3.Replace("float x", "float a", StringComparison.Ordinal),
                BindsVec3.Replace("float x", "double x", StringComparison.Ordinal),
                BindsVec3.Replace("ROCKS_PROPERTY() float z = 0;\n", "", StringComparison.Ordinal),
                BindsVec3.Replace("ROCKS_PROPERTY() float x = 0;\nROCKS_PROPERTY() float y = 0;", "ROCKS_PROPERTY() float y = 0;\nROCKS_PROPERTY() float x = 0;", StringComparison.Ordinal),
                "struct VecBase {\nfloat x = 0;\nfloat y = 0;\nfloat z = 0;\n};\nstruct Split : VecBase {};\n" +
                    BindsVec3.Replace("Vec3Binding", "SplitBinding", StringComparison.Ordinal).Replace("rocks::Vec3", "probe::Split", StringComparison.Ordinal),
                "struct Mixed {\nrocks::int32 a = 0;\nfloat b = 0;\n};\nROCKS_STRUCT(Binds = \"probe::Mixed\")\nstruct MixedBinding {\nROCKS_PROPERTY() float b = 0;\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n",
                "struct Hidden {\nrocks::int32 a = 0;\nint Read() const noexcept { return b; }\nprivate:\nrocks::int32 b = 0;\n};\n" +
                    "ROCKS_STRUCT(Binds = \"probe::Hidden\")\nstruct HiddenBinding {\nROCKS_PROPERTY() rocks::int32 a = 0;\n};\n"
            }.Select(declaration => BindsPreamble + declaration + "}\n"), "RMC3000", "generated metadata contract rejected", BindsModule);
        // B5c: 배열 필드가 descriptor(value {Array}·element·maxElements·fieldKind)와 생성 코드(스키마·함수표·Trace·형질·
        // static_assert·등록 순서)에 닿고 생성 코드가 컴파일된다(Docs/Design/ReflectionFoundations.ko.md 5.6절 MetaCompiler).
        suite.Add("Meta.ArraySchemaReachesDescriptorAndGeneratedCode", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ArraySource);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            string[] Flags(string name) => Type(name).GetProperty("flags").EnumerateArray().Select(flag => flag.GetString()!).ToArray();
            System.Text.Json.JsonElement Field(string name) => Type("probe::Value").GetProperty("fields").EnumerateArray()
                .Single(field => field.GetProperty("name").GetString() == name);
            string Id(string name) => Type(name).GetProperty("id").GetString()!;
            void Expect(string name, string fieldKind, string element, string? target, uint maxElements, bool required)
            {
                System.Text.Json.JsonElement field = Field(name);
                Verify.Equal(fieldKind, field.GetProperty("fieldKind").GetString(), name + " fieldKind");
                System.Text.Json.JsonElement value = field.GetProperty("value");
                Verify.Equal("Array", value.GetProperty("kind").GetString(), name + " value kind");
                Verify.That(!value.TryGetProperty("type", out _), name + " array value has no target type");
                System.Text.Json.JsonElement item = field.GetProperty("element");
                Verify.Equal(element, item.GetProperty("kind").GetString(), name + " element kind");
                Verify.Equal(target, item.TryGetProperty("type", out System.Text.Json.JsonElement type) ? type.GetString() : null, name + " element target");
                Verify.Equal(maxElements, field.GetProperty("maxElements").GetUInt32(), name + " maxElements");
                Verify.Equal(required, field.GetProperty("flags").EnumerateArray().Any(flag => flag.GetString() == "Required"), name + " Required");
            }

            Expect("ints", "Value", "Int32", null, 65536, false);
            Expect("names", "Value", "Utf8String", null, 8, false);
            Expect("modes", "Value", "Enum", Id("probe::Mode"), 1048576, false);
            Expect("softs", "Value", "SoftObjectReference", Id("probe::Value"), 65536, true);
            Expect("leaves", "Value", "Struct", Id("probe::Leaf"), 65536, false);
            Expect("refs", "ObjectReference", "ObjectReference", Id("probe::Value"), 65536, true);
            Expect("anyRefs", "ObjectReference", "ObjectReference", "", 65536, false);
            Expect("holders", "Value", "Struct", Id("probe::Holder"), 65536, false);
            Verify.Equal(System.Text.Json.JsonValueKind.Null, Field("bundle").GetProperty("element").ValueKind, "non-array field has a null element");
            Verify.Equal(0u, Field("bundle").GetProperty("maxElements").GetUInt32(), "non-array field has maxElements 0");
            Verify.Equal(0, Flags("probe::Leaf").Length, "plain struct with an array member has no trait flags");
            Verify.That(Flags("probe::Holder").SequenceEqual(["ContainsStrongReference"]), "element reference struct flags");
            Verify.That(Flags("probe::Bundle").SequenceEqual(["ContainsStrongReference", "ContainsReferenceArray"]), "reference array struct flags");
            Verify.That(Flags("probe::WorldBundle").SequenceEqual(["ContainsStrongReference", "RequiresWorldOnlyOwner", "ContainsReferenceArray"]),
                "struct flags are written in a fixed order");

            string generated = Compact(File.ReadAllText(probe.Output));
            foreach ((string expected, string reason) in new[]
            {
                ("#include <Object/NativeMemberTraits.h>", "struct specializations include the member trait header"),
                ("{ValueKind::Array, Guid{}}, Span<const char>(\"\", 0), Span<const char>(\"\", 0), Span<const char>(\"\", 0), {ValueKind::Int32, Guid{}}, 65536u}", "array schema with element and default maxElements"),
                ("{ValueKind::Utf8String, Guid{}}, 8u}", "explicit MaxElements"),
                (", 1048576u}", "largest MaxElements"),
                ("FieldKind::ObjectReference, 8u, Span<const char>(\"refs\", 4)", "ObjectRefArray field kind and Required flag"),
                ("NativeTypeKey<::probe::Mode>()", "enum element native key"),
                ("NativeTypeKey<::probe::Leaf>()", "struct element native key"),
                ("NativeTypeKey<::probe::Holder>()", "reference struct element native key"),
                ("detail::TraceArrayField(value.*(&::probe::Value::ints), visitor);", "class Trace visits every array field"),
                ("detail::TraceArrayField(value.*(&::probe::Value::refs), visitor);", "class Trace visits ObjectRefArray"),
                ("detail::TraceArrayField(value.*(&::probe::Value::holders), visitor);", "class Trace visits StructRefArray"),
                ("GeneratedType<::probe::Bundle>::Trace(value.*(&::probe::Value::bundle), visitor);", "class Trace calls the inline struct Trace"),
                ("detail::TraceArrayField(value.*(&::probe::Bundle::refs), visitor);", "reference struct Trace visits its ObjectRefArray"),
                ("detail::TraceArrayField(value.*(&::probe::Bundle::holders), visitor);", "reference struct Trace visits its StructRefArray"),
                ("detail::TraceArrayField(value.*(&::probe::Holder::weights), visitor);", "reference struct Trace visits plain arrays too"),
                ("static void Trace(const ::probe::Leaf&, TraceVisitor&) noexcept {}", "plain struct with arrays has an empty Trace"),
                ("static constexpr bool ContainsReferenceArray = true;", "reference array trait"),
                ("static constexpr uint32 Depth = 2u;", "element struct depth + 1"),
                ("static_assert(GeneratedType<::probe::Holder>::Depth < Depth", "element struct depth check"),
                ("static_assert(GeneratedType<::probe::Holder>::ContainsStrongReference && !GeneratedType<::probe::Holder>::ContainsReferenceArray", "StructRefArray element check"),
                ("static_assert(!GeneratedType<::probe::Leaf>::ContainsStrongReference", "Array<S> element check"),
                ("static_assert(ContainsReferenceArray || !::rocks::detail::RegistryContainer<decltype(::probe::Bundle::refs)>::value", "member container trait check"),
                ("static_assert(ContainsStrongReference || !::rocks::IsNativeStrongReference<decltype(::probe::Holder::target)>", "member strong reference trait check"),
                ("static_assert(ContainsReferenceArray || !GeneratedType<::probe::Bundle>::ContainsReferenceArray", "member struct reference array check")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            int Position(string name) => generated.IndexOf($"structGeneratedType<::probe::{name}>", StringComparison.Ordinal);
            Verify.That(Position("Leaf") >= 0 && Position("Holder") < Position("Bundle") && Position("Bundle") < Position("Value"),
                "element structs are specialized before their users (StructRefArray<S>::Trace needs GeneratedType<S>)");
            string aggregate = Compact(File.ReadAllText(probe.Aggregate));
            int Registered(string name) => aggregate.IndexOf($"GeneratedType<::probe::{name}>::Schema()", StringComparison.Ordinal);
            Verify.That(Registered("Mode") < Registered("Holder") && Registered("Holder") < Registered("Bundle") && Registered("Bundle") < Registered("Value"),
                "element structs are registered first:\n" + aggregate);
        });
        // Binds 미러 원소: Array<rocks::Vec3>의 원소가 미러로 풀리고 실제 타입의 native 키·생성 형질을 쓴다.
        suite.Add("Meta.ArrayOfBindsMirrorElements", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler, BindsModule);
            string source = "#include <Core/Containers/Array.h>\n" + BindsPreamble + BindsVec3 +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& allocator) noexcept : points(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::Array<rocks::Vec3> points;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            System.Text.Json.JsonElement points = Type("probe::Value").GetProperty("fields")[0];
            Verify.Equal(Type("probe::Vec3Binding").GetProperty("id").GetString(), points.GetProperty("element").GetProperty("type").GetString(),
                "Binds real type element resolves to the mirror");
            string generated = Compact(File.ReadAllText(probe.Output));
            Verify.That(generated.Contains(Compact("NativeTypeKey<::rocks::Vector3<float>>()"), StringComparison.Ordinal), "element native key of the real type");
            Verify.That(generated.Contains(Compact("static_assert(!GeneratedType<::rocks::Vector3<float>>::ContainsStrongReference"), StringComparison.Ordinal),
                "Array<S> element check uses the real type specialization");
        });
        // 생성 static_assert가 모델 형질을 C++ 멤버 타입과 대조한다: 생성 헤더의 형질 값을 거짓으로 바꾸면 컴파일이 실패한다
        // (encode 고정이 ContainsReferenceArray에 기대므로 틀린 모델 값이 고정 누락이 되지 않게).
        suite.Add("Meta.ArrayTraitStaticAssertsCompareTheCppMembers", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ArraySource);
            string original = File.ReadAllText(probe.Output);
            ProcessResult control = await probe.CompileGeneratedAsync();
            Verify.Equal(0, control.ExitCode, "generated header compiles: " + control.StdOut + control.StdErr);
            string bundle = original[original.IndexOf("struct GeneratedType<::probe::Bundle>", StringComparison.Ordinal)..];
            foreach ((string from, string to, string diagnostic) in new[]
            {
                ("static constexpr bool ContainsReferenceArray = true;", "static constexpr bool ContainsReferenceArray = false;", "is a reference array"),
                ("static constexpr bool ContainsStrongReference = true;", "static constexpr bool ContainsStrongReference = false;", "holds a strong object reference")
            })
            {
                int start = original.Length - bundle.Length;
                int at = original.IndexOf(from, start, StringComparison.Ordinal);
                Verify.That(at >= 0, "generated Bundle trait missing: " + from);
                File.WriteAllText(probe.Output, original[..at] + to + original[(at + from.Length)..]);
                ProcessResult rejected = await probe.CompileGeneratedAsync();
                Verify.That(rejected.ExitCode != 0, "tampered trait compiled: " + to);
                Verify.Contains(diagnostic, rejected.StdOut + rejected.StdErr, "trait static_assert diagnostic");
            }
        });
        // B6b: 클래스 Struct·Array 필드의 함수표는 valueNativeType(없으면 nullptr 명시) 뒤에 PathClassField 경로 thunk를, 모든 struct
        // 멤버는 StructFieldOps의 둘째 인자로 PathStructMember를 내며 대상 종류는 TargetKind(배열이면 원소 종류)다. 단일 값 클래스
        // 필드에는 path가 없다(Docs/Design/ReflectionFoundations.ko.md 7.1절 MetaCompiler).
        suite.Add("Meta.PathThunksReachStructAndArrayFields", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ArraySource);
            string generated = Compact(File.ReadAllText(probe.Output));
            const string lambda = "+[](const ::rocks::RuntimeFieldOps&, ::rocks::detail::PathCursor& cursor) noexcept -> ::rocks::Result<void> { return ::rocks::detail::PathClassField<";
            foreach ((string expected, string reason) in new[]
            {
                ("#include <Object/GeneratedPathAccess.h>", "generated header includes the path thunk templates"),
                ("&::probe::Value::ints, field, context); }, nullptr, " + lambda + "::probe::Value, ::rocks::ValueKind::Int32>(cursor, &::probe::Value::ints); } }",
                    "array without an Enum/Struct element spells nullptr before its path thunk"),
                ("NativeTypeKey<::probe::Mode>(), " + lambda + "::probe::Value, ::rocks::ValueKind::Enum>(cursor, &::probe::Value::modes); } }",
                    "enum element array path thunk follows the element native key"),
                (lambda + "::probe::Value, ::rocks::ValueKind::Utf8String>(cursor, &::probe::Value::names)", "string element target kind"),
                (lambda + "::probe::Value, ::rocks::ValueKind::SoftObjectReference>(cursor, &::probe::Value::softs)", "soft reference element target kind"),
                ("NativeTypeKey<::probe::Leaf>(), " + lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::leaves)", "Array<S> target kind"),
                (lambda + "::probe::Value, ::rocks::ValueKind::ObjectReference>(cursor, &::probe::Value::refs)", "ObjectRefArray target kind"),
                (lambda + "::probe::Value, ::rocks::ValueKind::ObjectReference>(cursor, &::probe::Value::anyRefs)", "ObjectRefArray<> target kind"),
                ("NativeTypeKey<::probe::Holder>(), " + lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::holders)", "StructRefArray target kind"),
                ("NativeTypeKey<::probe::Bundle>(), " + lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::bundle)", "struct field target kind"),
                (lambda + "::probe::WorldValue, ::rocks::ValueKind::Struct>(cursor, &::probe::WorldValue::bundle)", "owner is the class that emits the table"),
                ("detail::StructFieldOps<&::probe::Leaf::count>(", "member codec is unchanged"),
                ("&detail::PathStructMember<&::probe::Leaf::count, ValueKind::UInt32>)", "scalar member path thunk"),
                ("&detail::PathStructMember<&::probe::Leaf::codes, ValueKind::UInt16>)", "array member path thunk uses the element kind"),
                ("&detail::PathStructMember<&::probe::Holder::target, ValueKind::ObjectReference>)", "reference member path thunk"),
                ("&detail::PathStructMember<&::probe::Holder::weights, ValueKind::Float32>)", "float array member path thunk"),
                ("&detail::PathStructMember<&::probe::Bundle::refs, ValueKind::ObjectReference>)", "ObjectRefArray member path thunk"),
                ("&detail::PathStructMember<&::probe::Bundle::holders, ValueKind::Struct>)", "StructRefArray member path thunk"),
                ("&detail::PathStructMember<&::probe::WorldBundle::inner, ValueKind::Struct>)", "struct member path thunk")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            int Count(string text, string needle) => (text.Length - text.Replace(needle, "", StringComparison.Ordinal).Length) / needle.Length;
            Verify.Equal(9, Count(generated, "PathClassField<::probe::Value,"), "every Value array and struct field has a path thunk");
            Verify.Equal(1, Count(generated, "PathClassField<::probe::WorldValue,"), "WorldValue struct field has a path thunk");
            Verify.Equal(Count(generated, "detail::StructFieldOps<"), Count(generated, "&detail::PathStructMember<"), "every struct member has a path thunk");
            Verify.Equal(7, Count(generated, "&detail::PathStructMember<"), "struct member count");
            Verify.That(!generated.Contains("PathStructMember<&::probe::WorldBundle::lease", StringComparison.Ordinal), "NATIVE members have no path thunk");

            // 단일 값 클래스 필드(plain)에는 path가 없고(함수표가 decode 뒤에 끝난다) private 멤버·enum·String·EditorOnly 멤버에도 thunk가
            // 나온다.
            await probe.GenerateAsync(StructSource);
            string structs = Compact(File.ReadAllText(probe.Output));
            foreach ((string expected, string reason) in new[]
            {
                ("&::probe::Value::plain, field, context); } },", "scalar class field table ends after decode"),
                (lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::holder)", "struct field path thunk"),
                (lambda + "::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::extra)", "EditorOnly struct field path thunk"),
                (lambda + "::probe::WorldValue, ::rocks::ValueKind::Struct>(cursor, &::probe::WorldValue::bits)", "WorldOnly class struct field path thunk"),
                ("&detail::PathStructMember<&::probe::Holder::flag, ValueKind::UInt8>)", "private member path thunk through GENERATED_BODY"),
                ("&detail::PathStructMember<&::probe::Leaf::mode, ValueKind::Enum>)", "enum member path thunk"),
                ("&detail::PathStructMember<&::probe::Leaf::label, ValueKind::Utf8String>)", "string member path thunk"),
                ("&detail::PathStructMember<&::probe::Middle::offset, ValueKind::Int64>)", "EditorOnly member path thunk"),
                ("&detail::PathStructMember<&::probe::Middle::leaf, ValueKind::Struct>)", "nested struct member path thunk"),
                ("&detail::PathStructMember<&::probe::Holder::target, ValueKind::ObjectReference>)", "Required reference member path thunk")
            })
            {
                Verify.That(structs.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            Verify.Equal(2, Count(structs, "PathClassField<::probe::Value,"), "only the struct fields of Value have path thunks");
            Verify.That(!structs.Contains("(cursor,&::probe::Value::plain)", StringComparison.Ordinal), "scalar class field has no path thunk");
            Verify.That(!structs.Contains("PathStructMember<&::probe::Middle::cache", StringComparison.Ordinal), "NATIVE struct member has no path thunk");
            Verify.Equal(Count(structs, "detail::StructFieldOps<"), Count(structs, "&detail::PathStructMember<"), "every struct member has a path thunk");
        });
        // Binds 미러 멤버의 path thunk는 미러가 아니라 실제 타입의 멤버 포인터를 쓴다(멤버가 다른 미러인 struct 멤버 포함).
        suite.Add("Meta.PathThunksOfBindsMirrorsUseTheRealType", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler, BindsModule);
            string source = "#include <Core/Containers/Array.h>\n" + BindsPreamble + BindsVec3 +
                "ROCKS_STRUCT(Binds = \"rocks::Vector3<double>\")\nstruct Vec3dBinding {\nROCKS_PROPERTY() double x = 0;\nROCKS_PROPERTY() double y = 0;\nROCKS_PROPERTY() double z = 0;\n};\n" +
                "struct Pair {\nrocks::Vec3 a;\nrocks::Vec3d b;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::Pair\")\nstruct PairBinding {\nROCKS_PROPERTY() rocks::Vec3 a;\nROCKS_PROPERTY() rocks::Vector3<double> b;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& allocator) noexcept : points(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::Vec3 position;\nROCKS_PROPERTY() Pair pair;\nROCKS_PROPERTY() rocks::Array<rocks::Vec3> points;\n};\n}\n";
            await probe.GenerateAsync(source);
            string generated = Compact(File.ReadAllText(probe.Output));
            foreach ((string expected, string reason) in new[]
            {
                ("&detail::PathStructMember<&::rocks::Vector3<float>::x, ValueKind::Float32>)", "mirror member of the real type"),
                ("&detail::PathStructMember<&::rocks::Vector3<double>::z, ValueKind::Float64>)", "template mirror member of the real type"),
                ("&detail::PathStructMember<&::probe::Pair::a, ValueKind::Struct>)", "mirror member that is another mirror"),
                ("&detail::PathStructMember<&::probe::Pair::b, ValueKind::Struct>)", "mirror member spelled as the real template"),
                ("NativeTypeKey<::rocks::Vector3<float>>(), +[](const ::rocks::RuntimeFieldOps&, ::rocks::detail::PathCursor& cursor) noexcept -> ::rocks::Result<void> { " +
                    "return ::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::position); }",
                    "class field of a Core value type"),
                ("::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::pair)", "class field of a Binds probe type"),
                ("::rocks::detail::PathClassField<::probe::Value, ::rocks::ValueKind::Struct>(cursor, &::probe::Value::points)", "array of a Core value type")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            Verify.That(!generated.Contains("&::probe::Vec3Binding::", StringComparison.Ordinal) && !generated.Contains("&::probe::Vec3dBinding::", StringComparison.Ordinal) &&
                !generated.Contains("&::probe::PairBinding::", StringComparison.Ordinal), "no member pointer of a mirror declaration");
        });
        // 대상 종류가 C++ 멤버 형식과 다르면(오래된 생성 헤더·잘못된 descriptor) TargetValueCheck static_assert가 컴파일을 실패시킨다:
        // 생성 헤더의 대상 종류를 바꾸면 클래스 배열·enum 원소 배열·struct 필드·ObjectRefArray·struct 멤버 모두 거부된다.
        suite.Add("Meta.PathThunkTargetMismatchFailsToCompile", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ArraySource);
            string original = File.ReadAllText(probe.Output);
            ProcessResult control = await probe.CompileGeneratedAsync();
            Verify.Equal(0, control.ExitCode, "generated header compiles: " + control.StdOut + control.StdErr);
            foreach ((string anchor, bool before, string from, string to) in new[]
            {
                ("&::probe::Value::ints);", true, "ValueKind::Int32>", "ValueKind::UInt32>"),
                ("&::probe::Value::modes);", true, "ValueKind::Enum>", "ValueKind::UInt8>"),
                ("&::probe::Value::bundle);", true, "ValueKind::Struct>", "ValueKind::Enum>"),
                ("&::probe::Value::refs);", true, "ValueKind::ObjectReference>", "ValueKind::Struct>"),
                ("PathStructMember<&::probe::Leaf::count,", false, "ValueKind::UInt32>", "ValueKind::Int32>"),
                ("PathStructMember<&::probe::Holder::weights,", false, "ValueKind::Float32>", "ValueKind::Float64>")
            })
            {
                int site = original.IndexOf(anchor, StringComparison.Ordinal);
                Verify.That(site >= 0, "generated path thunk missing: " + anchor);
                int at = before ? original.LastIndexOf(from, site, StringComparison.Ordinal) : original.IndexOf(from, site, StringComparison.Ordinal);
                Verify.That(at >= 0 && Math.Abs(at - site) < 200, "path thunk target kind missing near " + anchor);
                File.WriteAllText(probe.Output, original[..at] + to + original[(at + from.Length)..]);
                ProcessResult rejected = await probe.CompileGeneratedAsync();
                Verify.That(rejected.ExitCode != 0, "path thunk with a mismatched target kind compiled: " + anchor + " " + to);
                Verify.Contains("path thunk target kind does not match the C++ member type", rejected.StdOut + rejected.StdErr, "target kind static_assert diagnostic");
            }
        });
        // Array<ObjectRef<T>>·Array<참조 struct>·Array<ObjectRefArray를 담은 struct>는 멤버 선언에서 Array의 nothrow-move
        // static_assert로 Clang이 먼저 실패한다. 그 진단을 멤버 선언 위치의 RMC2304로(필드 이름·원소 철자), NATIVE 멤버는 RMC2205로
        // 바꾼다(clang과 clang-cl 위치 형식 해석은 Meta.ArrayInstantiationSiteParsesClangAndClangClDiagnostics).
        suite.Add("Meta.ArrayStaticAssertFailureIsReportedAtTheMember", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            const string box = "ROCKS_STRUCT()\nstruct Box {\nexplicit Box(rocks::Allocator& allocator) noexcept : refs(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::ObjectRefArray<Value> refs;\n};\n";
            const string wrap = "template <class T>\nstruct Wrap {\nT value;\n};\n";
            foreach ((string member, string name, string code, string reason) in new[]
            {
                ("ROCKS_PROPERTY() rocks::Array<rocks::ObjectRef<Value>> refs;", "refs", "RMC2304", "probe::Value::refs: rocks::Array element 'rocks::ObjectRef<probe::Value>' holds a strong object reference"),
                ("ROCKS_PROPERTY()\nrocks::Array<Holder>\n    holders;", "holders", "RMC2304", "probe::Value::holders: rocks::Array element 'probe::Holder' holds a strong object reference"),
                ("ROCKS_PROPERTY() rocks::Array<Box> boxes;", "boxes", "RMC2304", "probe::Value::boxes: rocks::Array element 'probe::Box' holds a strong object reference"),
                ("ROCKS_PROPERTY() rocks::Array<Wrap<rocks::ObjectRef<Value>>> wraps;", "wraps", "RMC2304",
                    "probe::Value::wraps: rocks::Array element 'probe::Wrap<rocks::ObjectRef<probe::Value>>' holds a strong object reference (probe::Wrap<rocks::ObjectRef<probe::Value>> -> rocks::ObjectRef)"),
                ("ROCKS_NATIVE() rocks::Array<rocks::ObjectRef<Value>> cache;", "cache", "RMC2205", "probe::Value::cache: ROCKS_NATIVE member"),
                ("ROCKS_PROPERTY() ROCKS_NATIVE() rocks::Array<rocks::ObjectRef<Value>> both;", "both", "RMC2204", "probe::Value::both: a field is either ROCKS_PROPERTY or ROCKS_NATIVE")
            })
            {
                string head = ArrayPreamble + "struct Value;\n" + ArrayStructs + box + wrap + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n";
                string source = head + member + "\n};\n}\n";
                int line = 1 + source[..(head.Length + member.LastIndexOf(name, StringComparison.Ordinal))].Count(c => c == '\n');
                await probe.RejectAsync(source, code, $"Probe.h:{line}: error {code}: {reason}");
            }
        });
        // Array.h가 실패해도 원소가 강한 참조를 담지 않으면(이동·소멸 noexcept 위반 — 참조 없는 struct의 throwing 이동, 이동할 수
        // 없는 NATIVE 멤버를 가진 struct, noexcept(false) 소멸자) 또는 첫 오류가 Array.h가 아니면 바꾸지 않고 Clang 원문을 담은
        // RMC3000을 낸다.
        suite.Add("Meta.ArrayStaticAssertFailureWithoutReferencesKeepsClangDiagnostic", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            const string elements = "struct Pinned {\nPinned() noexcept = default;\nPinned(Pinned&&) = delete;\n};\n" +
                "ROCKS_STRUCT()\nstruct Cache {\nCache() noexcept = default;\nCache(Cache&&) {}\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Sticky {\nROCKS_NATIVE() Pinned pin;\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Fragile {\n~Fragile() noexcept(false) {}\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n";
            foreach ((string prefix, string member) in new[]
            {
                ("", "ROCKS_PROPERTY() rocks::Array<Cache> caches;"),
                ("", "ROCKS_PROPERTY() rocks::Array<Sticky> stickies;"),
                ("", "ROCKS_PROPERTY() rocks::Array<Fragile> fragiles;"),
                ("static_assert(sizeof(int) == 0, \"first failure\");\n", "ROCKS_PROPERTY() rocks::Array<rocks::ObjectRef<Value>> refs;")
            })
            {
                await probe.RejectAsync(ArrayPreamble + "struct Value;\n" + elements + prefix + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" + member + "\n};\n}\n",
                    "RMC3000", "Clang rejected reflection input");
            }
        });
        AddRejectGroup("Meta.ArrayElementTypeIsRestricted", new[]
            {
                "rocks::Array<const rocks::int32>", "rocks::Array<rocks::int32*>", "rocks::Array<const rocks::int32*>",
                "rocks::ObjectRefArray<const Value>", "rocks::StructRefArray<const Holder>"
            }.Select(type => ArrayPreamble + "struct Value;\n" + ArrayStructs + $"ROCKS_CLASS()\nstruct Value : public rocks::Object {{\nROCKS_PROPERTY() {type} ints;\n}};\n}}\n"),
            "RMC2000", "array element");
        AddRejectGroup("Meta.ArrayMaxElementsRangeIsChecked", [
            ArrayPreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY(MaxElements = 1048577) rocks::Array<rocks::int32> ints;\n};\n}\n"],
            "RMC2000", "exceeds 1048576");
        AddRejectGroup("Meta.ArrayRequiredNeedsReferenceElements", new[]
            {
                "ROCKS_PROPERTY(Required) rocks::Array<rocks::int32> ints;",
                "ROCKS_PROPERTY(Required) rocks::Array<rocks::String> names;",
                "ROCKS_PROPERTY(Required) rocks::StructRefArray<Holder> holders;"
            }.Select(member => ArrayPreamble + "struct Value;\n" + ArrayStructs + $"ROCKS_CLASS()\nstruct Value : public rocks::Object {{\n{member}\n}};\n}}\n"),
            "RMC2202", "applies only to");
        // 중첩 배열·컨테이너 인자의 배열(ObjectRefArray는 RMC2003보다 먼저), 리플렉션 struct가 아닌 StructRefArray 원소, 철자
        // 컨테이너와 파생 컨테이너 불일치(참조 없는 struct의 StructRefArray, noexcept 이동을 직접 정의해 Clang을 지나는
        // Array<참조 struct>), Registry 컨테이너를 담는 StructRefArray 원소는 RMC2304다.
        suite.Add("Meta.ArrayContainersRejectUnsupportedElements", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            foreach ((string member, string reason) in new[]
            {
                ("rocks::Array<rocks::Array<rocks::int32>> nested;", "Array element 'rocks::Array<int>' is itself an array"),
                ("rocks::ObjectRefArray<rocks::Array<rocks::int32>> nested;", "ObjectRefArray element 'rocks::Array<int>' is itself an array"),
                ("rocks::StructRefArray<rocks::ObjectRefArray<Value>> nested;", "StructRefArray element 'rocks::ObjectRefArray<probe::Value>' is itself an array"),
                ("rocks::StructRefArray<Leaf> leaves;", "holds no strong object references and needs rocks::Array<S>"),
                ("rocks::StructRefArray<Value> values;", "StructRefArray element 'probe::Value' is not a reflected ROCKS_STRUCT"),
                ("rocks::StructRefArray<Bundle> bundles;", "StructRefArray element struct probe::Bundle holds ObjectRefArray or StructRefArray members"),
                ("rocks::StructRefArray<Mode> values;", "StructRefArray element 'probe::Mode' is not a reflected ROCKS_STRUCT"),
                ("rocks::StructRefArray<rocks::int32> values;", "StructRefArray element 'int' is not a reflected ROCKS_STRUCT"),
                ("rocks::Array<Movable> movables;", "element struct probe::Movable holds strong object references and needs rocks::StructRefArray<S>"),
                // 참조 컨테이너를 담는 원소 struct는 어느 컨테이너로도 쓸 수 없으므로 StructRefArray로 바꾸라는 안내보다 먼저 알린다.
                ("rocks::Array<MovableBundle> bundles;", "Array element struct probe::MovableBundle holds ObjectRefArray or StructRefArray members")
            })
            {
                await probe.RejectAsync(ArrayPreamble + "struct Value;\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off = 0 };\n" + ArrayStructs +
                    "ROCKS_STRUCT()\nstruct Movable {\nMovable() noexcept = default;\nMovable(Movable&&) noexcept {}\nROCKS_PROPERTY() rocks::ObjectRef<Value> target;\n};\n" +
                    "ROCKS_STRUCT()\nstruct MovableBundle {\nexplicit MovableBundle(rocks::Allocator& allocator) noexcept : allocator(&allocator), refs(allocator) {}\n" +
                    "MovableBundle(MovableBundle&& other) noexcept : allocator(other.allocator), refs(*other.allocator) {}\n" +
                    "ROCKS_NATIVE() rocks::Allocator* allocator = nullptr;\nROCKS_PROPERTY() rocks::ObjectRefArray<Value> refs;\n};\n" +
                    $"ROCKS_CLASS()\nstruct Value : public rocks::Object {{\nROCKS_PROPERTY() {member}\n}};\n}}\n", "RMC2304", reason);
            }
        });
        AddRejectGroup("Meta.ObjectRefArrayTargetMustBeAClass", new[] { "rocks::ObjectRefArray<Leaf>", "rocks::ObjectRefArray<Mode>" }
            .Select(type => ArrayPreamble + "struct Value;\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off = 0 };\n" + ArrayStructs +
                $"ROCKS_CLASS()\nstruct Value : public rocks::Object {{\nROCKS_PROPERTY() {type} refs;\n}};\n}}\n"),
            "RMC2003", "not a class");
        // 배열을 거친 자기 참조는 RMC2302이고 깊이는 원소 깊이 + 1로 센다(16은 받고 17은 RMC2303).
        AddRejectGroup("Meta.ArraySelfReferenceIsACycle", [
            ArrayPreamble + "struct Value;\nROCKS_STRUCT()\nstruct Node {\nexplicit Node(rocks::Allocator& allocator) noexcept : children(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::StructRefArray<Node> children;\nROCKS_PROPERTY() rocks::ObjectRef<Value> target;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2302", "cycle");
        suite.Add("Meta.ArrayNestingDepthIsLimited", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ArrayChainSource(16));
            Verify.That(Compact(File.ReadAllText(probe.Output)).Contains(Compact("static constexpr uint32 Depth = 16u;"), StringComparison.Ordinal), "depth 16 through arrays is accepted");
            await probe.RejectAsync(ArrayChainSource(17), "RMC2303", "nesting depth 17");
        });
        // WorldOnly 소유자 형질의 원소 struct는 WorldOnly가 아닌 클래스에 둘 수 없고(직접·원소 struct를 담은 struct), NATIVE
        // StructRefArray·Array<참조 struct>는 강한 참조다(RMC2205).
        AddRejectGroup("Meta.ArrayWorldOnlyElementNeedsWorldOnlyClass", new[]
            {
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Array<WorldBits> bits;\n};\n",
                "ROCKS_STRUCT()\nstruct Outer {\nexplicit Outer(rocks::Allocator& allocator) noexcept : inner(allocator) {}\nROCKS_PROPERTY() rocks::Array<WorldBits> inner;\n};\n" +
                    "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Outer bits;\n};\n"
            }.Select(declaration => ArrayPreamble + StructWorldBits + declaration + "}\n"),
            "RMC2206", "WorldOnly owner");
        AddRejectGroup("Meta.NativeReferenceArraysAreRejected", new[] { "rocks::StructRefArray<Holder>", "rocks::Array<Holder>" }
            .Select(type => ArrayPreamble + "struct Value;\n" + ArrayStructs +
                $"ROCKS_CLASS()\nstruct Value : public rocks::Object {{\nROCKS_NATIVE() {type} cache;\n}};\n}}\n"),
            "RMC2205", "strong object reference");
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
        // Object 파생 타입을 가리키는 직접 포인터·참조는 MetaCompiler의 Clang 탐침이 판정한다(NativeMemberProbe — 생성 static_assert의
        // IsNativeStrongReference는 대상의 완전성에 기대지 않아 래퍼만 본다). 이름 해석(같은 namespace·inline namespace·별칭)·특수화
        // 선택(부분 특수화, bool·enum·char·식 인자 명시 특수화, 기본 인자)·base 판정(CRTP·pack base)과 cv·배열·여러 겹·restrict를
        // Clang이 하고, 진단은 탐침 덤프의 정규 이름을 쓴다.
        const string probeTypes = "namespace probe {\ntemplate <class T> struct CrtpBase : rocks::Object {};\nstruct Crtp : CrtpBase<Crtp> {};\n" +
            "struct Node : rocks::Object {};\nstruct Plain {};\nclass Fwd;\ntemplate <class T> struct W {};\ntemplate <class T> struct W<T*> : rocks::Object {};\n" +
            "template <bool B> struct Sel {};\ntemplate <> struct Sel<true> : rocks::Object {};\nenum class Kind { A, B };\n" +
            "template <Kind K> struct ByEnum {};\ntemplate <> struct ByEnum<Kind::B> : rocks::Object {};\n" +
            "template <char C> struct ByChar {};\ntemplate <> struct ByChar<'>'> : rocks::Object {};\n" +
            "template <int N> struct Pick {};\ntemplate <> struct Pick<2> : rocks::Object {};\n" +
            "template <class T, class B = rocks::Object> struct WithBase : B {};\ntemplate <class... Bs> struct Multi : Bs... {};\n" +
            "inline namespace v1 { struct Versioned : rocks::Object {}; }\nnamespace lib { inline namespace v2 { struct Deep : rocks::Object {}; } }\n" +
            "using CrtpAlias = Crtp;\ntypedef Node NodeAlias;\n" +
            "template <class T> struct Holder { T value; };\ntemplate <int N> struct Endless : Endless<N + 1> {};\n";
        string ProbeSource(string members) => Declaration(extra: "ROCKS_NATIVE() " + members).Replace("namespace probe {\n", probeTypes, StringComparison.Ordinal);
        const string objectPointer = "error RMC2205: probe::Value::cache: ROCKS_NATIVE member holds a strong object reference (raw pointer to Object-derived ";
        suite.Add("Meta.NativeRawPointerToObjectIsJudgedByClangProbe", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            foreach ((string member, string reason) in new[]
            {
                ("Value* cache = nullptr;", objectPointer + "probe::Value)"),
                ("Crtp* cache = nullptr;", objectPointer + "probe::Crtp)"),
                ("Crtp& cache;", objectPointer + "probe::Crtp)"),
                ("Crtp** cache = nullptr;", objectPointer + "probe::Crtp)"),
                ("const Crtp* const cache = nullptr;", objectPointer + "probe::Crtp)"),
                ("Crtp* cache[2] = {};", objectPointer + "probe::Crtp)"),
                ("Versioned* cache = nullptr;", objectPointer + "probe::"),
                ("lib::Deep& cache;", objectPointer + "probe::lib::"),
                ("CrtpAlias* cache = nullptr;", objectPointer + "probe::Crtp)"),
                ("NodeAlias* const* cache = nullptr;", objectPointer + "probe::Node)"),
                ("W<int*>* cache = nullptr;", objectPointer + "probe::W<int *>)"),
                ("Sel<true>* cache = nullptr;", objectPointer + "probe::Sel<true>)"),
                ("ByEnum<Kind::B>* cache = nullptr;", objectPointer + "probe::ByEnum<"),
                ("ByChar<'>'>* cache = nullptr;", objectPointer + "probe::ByChar<'>'>)"),
                ("Pick<1 + 1>* cache = nullptr;", objectPointer + "probe::Pick<2>)"),
                ("WithBase<int>* cache = nullptr;", objectPointer + "probe::WithBase<int"),
                ("Multi<Plain, rocks::Object>* cache = nullptr;", objectPointer + "probe::Multi<probe::Plain, rocks::Object>)"),
                ("Node* __restrict cache = nullptr;", objectPointer + "probe::Node)"),
                // 래퍼 포인터는 Clang 철자에 따라 AST 검사(macOS)나 탐침(clang-cl)이 거부하므로 공통 앞부분만 대조한다.
                ("rocks::ObjectRef<Node>* __restrict cache = nullptr;", "probe::Value::cache: ROCKS_NATIVE member holds a strong object reference (rocks::ObjectRef<")
            })
            {
                await probe.RejectAsync(ProbeSource(member), "RMC2205", reason);
            }
        });
        // 대상이 Object 파생이 아니면(특수화가 비Object, 함수 포인터의 반환·인자, 멤버 포인터, 정의를 끝내 못 찾는 전방 선언) 받는다.
        suite.Add("Meta.NativeNonObjectPointeesAreAccepted", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ProbeSource("Sel<false>* sel = nullptr;\nROCKS_NATIVE() void* (*factory)(rocks::usize) = nullptr;\n" +
                "ROCKS_NATIVE() const char* (*label)() = nullptr;\nROCKS_NATIVE() rocks::int32 Plain::* member = nullptr;\n" +
                "ROCKS_NATIVE() Plain* const* plain = nullptr;\nROCKS_NATIVE() Fwd* forward = nullptr;"));
            Verify.Contains("::probe::Value::forward", File.ReadAllText(probe.Output), "accepted NATIVE members keep the generated static_assert");
        });
        // 첫 탐침에서 불완전한 대상은 모듈 public 헤더를 더한 두 번째 탐침이 다시 판정한다: 정의가 비Object면(World) 받고 Object
        // 파생이면(Spy) 거부한다.
        suite.Add("Meta.NativeForwardDeclaredPointeeIsJudgedWithPublicHeaders", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            probe.WriteHeader("WorldDefinition.h", "#pragma once\n#include <Object/Object.h>\nnamespace probe {\nclass World {\npublic:\n    int frame = 0;\n};\n" +
                "struct Spy : rocks::Object {};\n}\n");
            string Forward(string member) => Declaration(extra: "ROCKS_NATIVE() " + member)
                .Replace("namespace probe {\n", "namespace probe {\nclass World;\nstruct Spy;\n", StringComparison.Ordinal);
            await probe.GenerateAsync(Forward("World* world = nullptr;"));
            await probe.RejectAsync(Forward("Spy* spy = nullptr;"), "RMC2205",
                "error RMC2205: probe::Value::spy: ROCKS_NATIVE member holds a strong object reference (raw pointer to Object-derived probe::Spy)");
        });
        // 탐침의 인스턴스화 hard error(Holder<Fwd>의 불완전한 필드)·치명적 오류(재귀 인스턴스화 깊이)는 그 멤버만 cannot verify로
        // 보고하고, 다른 멤버의 판정은 유지한다(Object 파생 대상이 있으면 그것을 먼저 보고한다).
        suite.Add("Meta.NativeProbeFailureIsIsolatedPerMember", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.RejectAsync(ProbeSource("Plain* fine = nullptr;\nROCKS_NATIVE() Holder<Fwd>* broken = nullptr;"), "RMC2205",
                "error RMC2205: probe::Value::broken: cannot verify that ROCKS_NATIVE member holds no strong object reference (Clang: ");
            await probe.RejectAsync(ProbeSource("Holder<Fwd>* broken = nullptr;\nROCKS_NATIVE() Node* bad = nullptr;"), "RMC2205",
                "error RMC2205: probe::Value::bad: ROCKS_NATIVE member holds a strong object reference (raw pointer to Object-derived probe::Node)");
            await probe.RejectAsync(ProbeSource("Plain* fine = nullptr;\nROCKS_NATIVE() Endless<0>* deep = nullptr;"), "RMC2205",
                "error RMC2205: probe::Value::deep: cannot verify that ROCKS_NATIVE member holds no strong object reference (Clang: ");
            await probe.RejectAsync(ProbeSource("Endless<0>* deep = nullptr;\nROCKS_NATIVE() Node* bad = nullptr;"), "RMC2205",
                "error RMC2205: probe::Value::bad: ROCKS_NATIVE member holds a strong object reference (raw pointer to Object-derived probe::Node)");
        });
        // private NATIVE 멤버도 탐침이 GeneratedType 특수화(GENERATED_BODY의 friend) 안에서 접근 검사를 끄지 않고 판정한다.
        suite.Add("Meta.PrivateNativeMemberIsJudgedThroughGeneratedBody", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string Private(string type) => Preamble.Replace("namespace probe {\n", probeTypes, StringComparison.Ordinal) +
                "ROCKS_CLASS()\nclass Value : public rocks::Object {\nROCKS_GENERATED_BODY()\nROCKS_PROPERTY() rocks::int32 count = 0;\n" +
                $"ROCKS_NATIVE() {type}* hidden = nullptr;\npublic:\n    bool Has() const noexcept {{ return hidden != nullptr && count == 0; }}\n}};\n}}\n";
            await probe.GenerateAsync(Private("Plain"));
            await probe.RejectAsync(Private("Node"), "RMC2205",
                "error RMC2205: probe::Value::hidden: ROCKS_NATIVE member holds a strong object reference (raw pointer to Object-derived probe::Node)");
        });
        // 두 번째 탐침(public 헤더)도 멤버별로 격리한다: public 헤더로 정의가 보이게 된 Box<Missing>의 인스턴스화 오류가 같은
        // 탐침의 Spy* 거부를 가리지 않는다.
        const string forwardSpy = "namespace probe {\nstruct Spy;\ntemplate <class T> struct Box;\nstruct Missing;\n";
        const string spyDefinition = "#pragma once\n#include <Object/Object.h>\nnamespace probe {\nstruct Spy : rocks::Object {};\n}\n";
        const string spyRejected = "error RMC2205: probe::Value::spy: ROCKS_NATIVE member holds a strong object reference (raw pointer to Object-derived probe::Spy)";
        string ForwardSpy(string members) => Declaration(extra: "ROCKS_NATIVE() " + members).Replace("namespace probe {\n", forwardSpy, StringComparison.Ordinal);
        suite.Add("Meta.NativePublicHeaderProbeFailureDoesNotMaskOtherMembers", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            probe.WriteHeader("BoxDefinition.h", "#pragma once\nnamespace probe {\ntemplate <class T> struct Box { T value; };\n}\n");
            probe.WriteHeader("SpyDefinition.h", spyDefinition);
            await probe.RejectAsync(ForwardSpy("Spy* spy = nullptr;\nROCKS_NATIVE() Box<Missing>* box = nullptr;"), "RMC2205", spyRejected);
        });
        // 컴파일되지 않는 public 헤더 하나(다른 플랫폼 전용)가 두 번째 탐침 전체를 끄지 않는다 — 헤더를 하나씩 컴파일해 그 헤더만 뺀다.
        suite.Add("Meta.NativeBrokenPublicHeaderIsSkippedByTheProbe", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            probe.WriteHeader("AndroidOnly.h", "#pragma once\n#if !defined(PLATFORM_ANDROID)\n#error \"Android only\"\n#endif\n");
            probe.WriteHeader("SpyDefinition.h", spyDefinition);
            await probe.RejectAsync(ForwardSpy("Spy* spy = nullptr;"), "RMC2205", spyRejected);
        });
        // 탐침은 이 모듈의 출력(생성) 루트를 include 검색 순서에서 뺀다: 생성 집계 헤더를 (다른 헤더를 거쳐) include하는 public
        // 헤더는 깨끗한 빌드와 출력을 유지한 빌드에서 똑같이 쓰이지 않아 판정이 갈리지 않는다(둘 다 Spy 정의를 못 봐 허용).
        suite.Add("Meta.NativeProbeIgnoresTheModuleOutputRoot", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string generatedPublic = Path.GetDirectoryName(Path.GetDirectoryName(probe.Aggregate))!;
            Directory.CreateDirectory(generatedPublic);
            probe.WriteHeader("Pawn.h", $"#pragma once\n#include <{MetaCompilerArguments.Module}/{MetaCompilerArguments.Module}.module.generated.h>\n" +
                "namespace probe {\nstruct PawnLike {};\n}\n");
            probe.WriteHeader("SpyDefinition.h", "#pragma once\n#include \"Pawn.h\"\n#include <Object/Object.h>\nnamespace probe {\nstruct Spy : rocks::Object {};\n}\n");
            string source = ForwardSpy("Spy* spy = nullptr;");
            await probe.GenerateAsync(source, extra: ["--include-dir", generatedPublic]);
            Verify.That(File.Exists(probe.Aggregate), "first run published the aggregate header");
            await probe.GenerateAsync(source, extra: ["--include-dir", generatedPublic]);
        });
        // 탐침 텍스트의 이름은 모두 접두를 달아 리플렉션 헤더의 매크로와 겹치지 않는다.
        suite.Add("Meta.NativeProbeNamesDoNotCollideWithMacros", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            const string macros = "#define Indirect 1\n#define Complete 1\n#define Unqualified 1\n#define Layer 1\n#define Walk 1\n#define Leaf 1\n";
            await probe.GenerateAsync(Declaration(extra: "ROCKS_NATIVE() Plain* plain = nullptr;\nROCKS_NATIVE() rocks::int32 cache = 0;")
                .Replace("namespace probe {\n", macros + "namespace probe {\nstruct Plain {};\n", StringComparison.Ordinal));
        });
        // 포인터 겹은 형질 내장으로 벗기므로 주소 공간·__unaligned(clang-cl — MS 확장) 한정 포인터도 대상을 본다.
        suite.Add("Meta.NativeQualifiedPointerToObjectIsRejected", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            List<string> members = ["__attribute__((address_space(1))) Node* cache = nullptr;", "Node* __attribute__((address_space(1)))* cache = nullptr;"];
            if (Path.GetFileNameWithoutExtension(compiler).Contains("clang-cl", StringComparison.OrdinalIgnoreCase))
            {
                members.AddRange(["Node* __unaligned cache = nullptr;", "__unaligned Node* cache = nullptr;"]);
            }

            foreach (string member in members)
            {
                await probe.RejectAsync(ProbeSource(member), "RMC2205", objectPointer);
            }
        });
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

        void AddRejectGroup(string name, IEnumerable<string> sources, string code, string reason, string module = MetaCompilerArguments.Module)
        {
            string[] fixedSources = sources.ToArray();
            suite.Add(name, async () =>
            {
                using Probe probe = new(options, actualEnvironment, generator, compiler, module);
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
        private readonly string module;

        // module은 생성할 모듈 이름이다(Binds는 CoreTypeBindings 모듈에서만 받으므로 Binds 시험은 그 이름으로 실행한다).
        public Probe(Options options, IReadOnlyDictionary<string, string?> environment, string generator, string compiler,
            string module = MetaCompilerArguments.Module)
        {
            this.options = options;
            this.environment = environment;
            this.generator = generator;
            this.compiler = compiler;
            this.module = module;
            MetaProbeIdentity.Create(IdentityPath);
        }

        private string lastInclude = "Probe.h";

        // 마지막으로 생성한 헤더의 생성 경로다(fixture 모드는 fixture 철자).
        public string Output => MetaCompilerArguments.GeneratedPath(temporary.Root, lastInclude);

        public string Descriptor => MetaCompilerArguments.Descriptor(temporary.Root);

        public string Aggregate => MetaCompilerArguments.AggregatePath(temporary.Root);

        private string IdentityPath => temporary.FilePath("probe.rocksids.json");

        // 입력 헤더 옆(include 루트 바로 아래)에 헤더를 둔다 — MetaCompiler 탐침이 모듈 public 헤더로 더한다.
        public void WriteHeader(string name, string text) => temporary.Write(name, text);

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

            arguments.AddRange([.. MetaCompilerArguments.Single(temporary.Root, input, include, module), "--compiler", compiler]);
            AddDefaultIncludes(arguments);
            arguments.AddRange(["--identity-manifest", fixture
                ? Path.Combine(options.Root, "Core", "Source", "Developer", "ReflectionTestFixtures", "ReflectionTestFixtures.rocksids.json") : IdentityPath]);
            // 시험이 구성 프로필 전체를 넘기면(구성·대상 OS 매크로 포함) 기본 인자의 같은 매크로는 빼서 중복 정의가 되지
            // 않게 한다.
            bool extraDefinesBuild = extra?.Any(argument => argument.Contains("ROCKS_BUILD_", StringComparison.Ordinal)) == true;
            bool extraDefinesPlatform = extra?.Any(argument => argument.Contains("ROCKS_PLATFORM_", StringComparison.Ordinal)) == true;
            foreach (string argument in options.ClangArguments)
            {
                if ((extraDefinesBuild && argument.Contains("ROCKS_BUILD_", StringComparison.Ordinal)) ||
                    (extraDefinesPlatform && argument.Contains("ROCKS_PLATFORM_", StringComparison.Ordinal)))
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

        // 마지막으로 생성한 헤더를 include하는 번역 단위를 소비자와 같은 경고 프로필로 컴파일한다(생성 static_assert probe — 생성
        // 헤더를 고친 뒤 부른다).
        public async Task<ProcessResult> CompileGeneratedAsync()
        {
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Contains("clang-cl", StringComparison.OrdinalIgnoreCase);
            string source = temporary.FilePath("generated-check.cpp");
            File.WriteAllText(source, $"#include <{lastInclude[..^2]}.generated.h>\n", new UTF8Encoding(false, true));
            List<string> arguments = clangCl
                ? ["/nologo", "/TP", "/utf-8", "/clang:-std=c++23", "/W4", "/WX", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/Zs", source]
                : ["-x", "c++", "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-exceptions", "-fno-rtti", "-fsyntax-only", source];
            List<string> includes = [];
            AddDefaultIncludes(includes);
            foreach (string directory in new[] { Path.Combine(MetaCompilerArguments.OutputRoot(temporary.Root), "Public") }
                .Concat(includes.Where(argument => argument != "--include-dir")))
            {
                arguments.Add((clangCl ? "/I" : "-I") + directory);
            }

            arguments.AddRange(options.ClangArguments);
            return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment, Timeout: TimeSpan.FromSeconds(60)));
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
                "first::Good", "example::Invalid", "a::Thing", "b::Thing", "a::ThingExtended", "probe::Mode", "probe::Value::Nested",
                "probe::Leaf", "probe::Middle", "probe::Holder", "probe::Outer", "probe::WorldBits", "probe::WorldValue", "probe::Box",
                "probe::Zeta", "probe::Alpha", "probe::Vec3Binding", "probe::Vec3dBinding", "probe::PairBinding", "probe::OtherBinding",
                "probe::RealBinding", "probe::ABinding", "probe::BBinding", "probe::MixedBinding", "probe::HiddenBinding",
                "probe::FixedBinding", "probe::SplitBinding", "probe::Bundle", "probe::WorldBundle", "probe::Movable", "probe::Node", "probe::MovableBundle" }
                .Concat(Enumerable.Range(1, 17).Select(level => $"probe::Chain{level}")))
            {
                manifest.Register("type", name);
            }

            foreach (string name in new[] { "probe::Value::count", "probe::Value::next", "probe::Value::other", "probe::Object::value",
                "probe::Value::mode", "probe::Value::nested",
                "example::Invalid::valid", "example::Invalid::bad",
                "probe::Leaf::count", "probe::Leaf::label", "probe::Leaf::mode", "probe::Middle::leaf", "probe::Middle::offset",
                "probe::Holder::target", "probe::Holder::middle", "probe::Holder::flag", "probe::Outer::holder", "probe::Outer::inner",
                "probe::WorldBits::value", "probe::Value::holder", "probe::Value::plain", "probe::Value::extra", "probe::Value::bits",
                "probe::Value::leaf", "probe::WorldValue::bits", "probe::Box::count", "probe::Chain1::value",
                "probe::Zeta::v", "probe::Alpha::x", "probe::Value::alpha",
                "probe::Vec3Binding::x", "probe::Vec3Binding::y", "probe::Vec3Binding::z", "probe::Vec3Binding::a", "probe::Vec3Binding::count",
                "probe::Vec3dBinding::x", "probe::Vec3dBinding::y", "probe::Vec3dBinding::z", "probe::PairBinding::a", "probe::PairBinding::b",
                "probe::OtherBinding::x", "probe::OtherBinding::y", "probe::OtherBinding::z", "probe::RealBinding::inner", "probe::ABinding::inner",
                "probe::BBinding::inner", "probe::MixedBinding::a", "probe::MixedBinding::b", "probe::HiddenBinding::a",
                "probe::Value::position", "probe::Value::pair", "probe::FixedBinding::v", "probe::SplitBinding::x", "probe::SplitBinding::y",
                "probe::SplitBinding::z",
                "probe::Leaf::codes", "probe::Holder::weights", "probe::Bundle::refs", "probe::Bundle::holders", "probe::WorldBundle::inner",
                "probe::WorldValue::bundle", "probe::Movable::target", "probe::Node::children", "probe::Node::target",
                "probe::Value::ints", "probe::Value::names", "probe::Value::modes", "probe::Value::softs", "probe::Value::leaves",
                "probe::Value::refs", "probe::Value::anyRefs", "probe::Value::holders", "probe::Value::bundle", "probe::Value::boxes",
                "probe::Value::cache", "probe::Value::values", "probe::Value::bundles", "probe::Value::movables",
                "probe::Value::points", "probe::Box::refs", "probe::MovableBundle::refs" }
                .Concat(Enumerable.Range(2, 16).Select(level => $"probe::Chain{level}::inner")))
            {
                manifest.Register("field", name);
            }

            return 0;
        });
    }
}
