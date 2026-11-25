namespace Rocks.ToolTests;

// meta 시험 중 ROCKS_STRUCT 시험이다(Docs/Design/ReflectionFoundations.ko.md 4절).
internal static partial class MetaTests
{
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
        "ROCKS_PROPERTY() Leaf leaf;\n#if ROCKS_WITH_EDITOR_DATA\nROCKS_PROPERTY(EditorOnly) rocks::int64 offset = 0;\n#endif\nROCKS_NATIVE() rocks::uint32 cache = 0;\n};\n" +
        "ROCKS_STRUCT()\nclass Holder {\nROCKS_GENERATED_BODY()\npublic:\nexplicit Holder(rocks::Allocator& allocator) noexcept : middle(allocator) {}\n" +
        "rocks::uint8 Flag() const noexcept { return flag; }\nROCKS_PROPERTY(Required) rocks::ObjectRef<Value> target;\nROCKS_PROPERTY() Middle middle;\n" +
        "private:\nROCKS_PROPERTY() rocks::uint8 flag = 0;\n};\n" +
        "ROCKS_STRUCT()\nstruct Outer {\nexplicit Outer(rocks::Allocator& allocator) noexcept : holder(allocator) {}\nROCKS_PROPERTY() Holder holder;\n};\n" +
        "ROCKS_STRUCT()\nstruct WorldBits {\nROCKS_NATIVE() Lease lease;\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        "ROCKS_CLASS()\nstruct Value : public rocks::Object {\n" +
        "explicit Value(rocks::Allocator& allocator) noexcept : holder(allocator),\n#if ROCKS_WITH_EDITOR_DATA\nextra(allocator),\n#endif\nscratch(allocator) {}\n" +
        "ROCKS_PROPERTY() Holder holder;\nROCKS_PROPERTY() rocks::uint32 plain = 0;\n#if ROCKS_WITH_EDITOR_DATA\nROCKS_PROPERTY(EditorOnly) Leaf extra;\n#endif\nROCKS_NATIVE() Leaf scratch;\n};\n" +
        "ROCKS_CLASS(WorldOnly)\nstruct WorldValue : public rocks::Object {\nROCKS_PROPERTY() WorldBits bits;\n};\n}\n";

    private const string StructLeaf = "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n";

    private const string StructWorldBits = "struct Lease { static constexpr bool RocksRequiresWorldOnlyOwner = true; };\n" +
        "ROCKS_STRUCT()\nstruct WorldBits {\nROCKS_NATIVE() Lease lease;\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n";

    // 깊이 depth의 struct 사슬이다(Chain1 ← Chain2 ← … ← Chain<depth>).
    private static string ChainSource(int depth) => Preamble + "ROCKS_STRUCT()\nstruct Chain1 {\nROCKS_PROPERTY() rocks::int32 value = 0;\n};\n" +
        string.Concat(Enumerable.Range(2, depth - 1).Select(level => $"ROCKS_STRUCT()\nstruct Chain{level} {{\nROCKS_PROPERTY() Chain{level - 1} inner;\n}};\n")) + "}\n";

    // ROCKS_STRUCT 시험이다(B4b — 스키마·Trace·깊이·Atomic·Migrate·거부 규칙).
    private static void RegisterStructs(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
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
            Verify.Equal("", leaf.GetProperty("base").GetString(), "struct descriptor without a reflected base writes an empty base");
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
            string generated = Compact(probe.GeneratedText());
            foreach ((string expected, string reason) in new[]
            {
                ("struct GeneratedType<::probe::Leaf>", "struct specialization"),
                ("static void Trace(const ::probe::Leaf&, TraceVisitor&) noexcept {}", "plain struct has an empty Trace"),
                ("visitor.Visit(value.*(&::probe::Holder::target));", "reference struct visits its ObjectRef member"),
                ("GeneratedType<::probe::Middle>::Trace(value.*(&::probe::Holder::middle), visitor);", "reference struct traces member structs"),
                ("GeneratedType<::probe::Holder>::Trace(value.*(&::probe::Value::holder), visitor);", "class Trace calls struct Trace"),
                ("registry.RegisterStruct(Schema(), Ops(Move(module)))", "struct Register"),
                ("MakeRuntimeStructOps<::probe::Leaf>(Move(module))", "struct ops helper (Migrate detection)"),
                ("detail::StructFieldOps<::probe::Holder,&::probe::Holder::flag>", "private member thunk through GENERATED_BODY"),
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
            string aggregate = Compact(File.ReadAllText(probe.ModuleSource));
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
            string generated = Compact(probe.GeneratedText());
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
            string generated = Compact(probe.GeneratedText());
            int zeta = generated.IndexOf("structGeneratedType<::probe::Zeta>", StringComparison.Ordinal);
            int alpha = generated.IndexOf("structGeneratedType<::probe::Alpha>", StringComparison.Ordinal);
            Verify.That(zeta >= 0 && alpha > zeta, "NATIVE member struct is emitted before its user");
            Verify.That(generated.Contains(Compact("static_assert(!GeneratedType<::probe::Zeta>::ContainsStrongReference"), StringComparison.Ordinal), "NATIVE struct member trait static_assert");
            Verify.That(generated.Contains(Compact("ops.migrate = &::probe::Alpha::Migrate;"), StringComparison.Ordinal), "private Migrate is wired");
            Verify.That(!generated.Contains(Compact("ops.migrate = &::probe::Zeta::Migrate;"), StringComparison.Ordinal), "struct without Migrate has no migrate");
            string aggregate = Compact(File.ReadAllText(probe.ModuleSource));
            Verify.That(aggregate.IndexOf("GeneratedType<::probe::Zeta>::Schema()", StringComparison.Ordinal) <
                aggregate.IndexOf("GeneratedType<::probe::Alpha>::Schema()", StringComparison.Ordinal), "aggregate registers the NATIVE member struct first");
        });
        // 깊이 16은 받고 17은 RMC2303이다(멤버 struct 사슬).
        suite.Add("Meta.StructNestingDepthIsLimited", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ChainSource(16));
            Verify.That(Compact(probe.GeneratedText()).Contains(Compact("static constexpr uint32 Depth = 16u;"), StringComparison.Ordinal), "depth 16 is accepted");
            await probe.RejectAsync(ChainSource(17), "RMC2303", "nesting depth 17");
        });
        // struct 상속 규칙(리플렉션 B9k)은 MetaTests.StructInheritance.cs가 본다.
        AddRejectGroup(context, "Meta.StructTemplatesAndPlacementAreRejected", [
            Preamble + "ROCKS_STRUCT()\ntemplate <class T> struct Leaf { ROCKS_PROPERTY() T count{}; };\n}\n",
            Preamble + "ROCKS_STRUCT()\nint intervening;\nstruct Leaf { ROCKS_PROPERTY() rocks::int32 count = 0; };\n}\n",
            Preamble + "void Test() {\nROCKS_STRUCT()\nstruct Leaf { rocks::int32 count = 0; };\n}\n}\n"],
            "RMC2000", "ROCKS_STRUCT");
        AddRejectGroup(context, "Meta.ClassAndStructMarkersAreExclusive", [
            Preamble + "ROCKS_CLASS()\nROCKS_STRUCT()\nstruct Value : public rocks::Object {};\n}\n",
            Preamble + "ROCKS_STRUCT()\nROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "stacked");
        AddRejectGroup(context, "Meta.StructMembersNeedPropertyOrNative", [
            Preamble + "ROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\nrocks::int32 hidden = 0;\n};\n}\n"],
            "RMC2000", "PROPERTY or NATIVE");
        AddRejectGroup(context, "Meta.StructSpecifiersAreRestricted", new[] { "WorldOnly", "Abstract", "Version=0", "Version=\"2\"", "Binds=1", "Atomic=1", "Atomic=\"yes\"", "Atomic, Atomic" }
            .Select(spec => Preamble + $"ROCKS_STRUCT({spec})\nstruct Leaf {{\nROCKS_PROPERTY() rocks::int32 count = 0;\n}};\n}}\n"), "RMC2000", "specifier");
        // 리플렉션 B9b2: Atomic은 STRUCT 전용 지정자다(Docs/Design/ReflectionExtensions.ko.md 3.1·15절). CLASS 지정자 표에는 없으므로
        // ROCKS_CLASS(Atomic)은 모르는 지정자 오류다.
        AddRejectGroup(context, "Meta.ClassAtomicSpecifierIsUnknown", new[] { "Atomic", "WorldOnly, Atomic" }
            .Select(spec => Preamble + $"ROCKS_CLASS({spec})\nstruct Value : public rocks::Object {{\nROCKS_PROPERTY() rocks::int32 count = 0;\n}};\n}}\n"),
            "RMC2000", "unsupported CLASS specifier/value Atomic");
        // 리플렉션 B9b2: ROCKS_STRUCT(Atomic)은 descriptor struct flags의 Atomic(형질보다 앞)과 생성 스키마 flags 16u에 닿고, Version·
        // 형질과 함께 쓸 수 있다. Atomic은 멤버에서 바깥으로 전파되지 않는다(Atomic 멤버를 담은 Plain은 flags 0).
        suite.Add("Meta.StructAtomicReachesDescriptorAndSchemaFlags", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble + "struct Value;\n" +
                "ROCKS_STRUCT(Atomic)\nstruct Point {\nROCKS_PROPERTY() rocks::int32 x = 0;\nROCKS_PROPERTY() rocks::int32 y = 0;\n};\n" +
                "ROCKS_STRUCT(Version = 3, Atomic)\nstruct Segment {\nROCKS_PROPERTY() Point from;\nROCKS_PROPERTY() rocks::ObjectRef<Value> owner;\n};\n" +
                "ROCKS_STRUCT()\nstruct Plain {\nROCKS_PROPERTY() Point at;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Point point;\nROCKS_PROPERTY() Segment segment;\nROCKS_PROPERTY() Plain plain;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            string Flags(string name) => string.Join(",", Type(name).GetProperty("flags").EnumerateArray().Select(flag => flag.GetString()));
            Verify.Equal("Atomic", Flags("probe::Point"), "Atomic struct flag");
            Verify.Equal("Atomic,ContainsStrongReference", Flags("probe::Segment"), "Atomic comes before generated traits");
            Verify.Equal(3, Type("probe::Segment").GetProperty("version").GetInt32(), "Version with Atomic");
            Verify.Equal("", Flags("probe::Plain"), "Atomic is not propagated from a member");
            Verify.Equal("", Flags("probe::Value"), "class flags");
            string generated = Compact(probe.GeneratedText());
            foreach ((string expected, string reason) in new[]
            {
                ("Span<const char>(\"probe::Point\", 12), Guid{}, 16u, Span<const char>(\"Point\", 5)};", "Atomic struct schema flags"),
                ("Span<const char>(\"probe::Segment\", 14), Guid{}, 16u, Span<const char>(\"Segment\", 7)};", "Atomic reference struct schema flags"),
                ("Span<const char>(\"probe::Plain\", 12), Guid{}, 0u, Span<const char>(\"Plain\", 5)};", "plain struct schema flags")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }
        });
        AddRejectGroup(context, "Meta.StructIsNotABaseOrReferenceTarget", [
            Preamble.Replace("namespace probe {", "#include <Object/SoftObjectRef.h>\nnamespace probe {", StringComparison.Ordinal) + StructLeaf +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nexplicit Value(rocks::Allocator& allocator) noexcept : next(allocator) {}\n" +
                "ROCKS_PROPERTY() rocks::SoftObjectRef<Leaf> next;\n};\n}\n",
            Preamble + StructLeaf + "ROCKS_CLASS()\nstruct Value : public Leaf {};\n}\n",
            Preamble + StructLeaf + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::ObjectRef<Leaf> next;\n};\n}\n"],
            "RMC2003", "is a reflected struct");
        AddRejectGroup(context, "Meta.ConstStructFieldIsUnsupported", [
            Preamble + StructLeaf + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() const Leaf leaf{};\n};\n}\n"],
            "RMC2000", "unsupported field type");
        // NATIVE 멤버로 둔 참조 struct는 AST 재귀 검사가 강한 참조를 찾는다(RMC2205 — 클래스·struct 모두).
        const string referenceHolder = "struct Value;\nROCKS_STRUCT()\nstruct Holder {\nROCKS_PROPERTY() rocks::ObjectRef<Value> target;\n};\n";
        AddRejectGroup(context, "Meta.NativeReferenceStructIsRejected", [
            Preamble + referenceHolder + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_NATIVE() Holder cache;\n};\n}\n",
            Preamble + referenceHolder + "ROCKS_STRUCT()\nstruct Box {\nROCKS_PROPERTY() rocks::int32 count = 0;\nROCKS_NATIVE() Holder cache;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2205", "strong object reference");
        // WorldOnly 소유자 형질(NATIVE)은 struct에서 형질로 전파되고(중첩 포함), 그 struct를 필드로 둔 WorldOnly 아닌
        // 클래스가 RMC2206이다.
        AddRejectGroup(context, "Meta.StructWorldOnlyTraitNeedsWorldOnlyClass", new[]
            {
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() WorldBits bits;\n};\n",
                "ROCKS_STRUCT()\nstruct Outer {\nROCKS_PROPERTY() WorldBits inner;\n};\nROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Outer bits;\n};\n",
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_NATIVE() WorldBits bits;\n};\n"
            }.Select(declaration => Preamble + StructWorldBits + declaration + "}\n"),
            "RMC2206", "WorldOnly owner");
        // Migrate라는 이름이 있는데 시그니처가 다르면 생성 코드 검증 컴파일이 거부한다(조용히 마이그레이션 없이 등록되지 않는다).
        AddRejectGroup(context, "Meta.StructMigrateSignatureIsChecked", new[]
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
        AddRejectGroup(context, "Meta.PrivateStructMigrateNeedsGeneratedBody", [
            Preamble + "ROCKS_STRUCT()\nclass Leaf {\npublic:\nROCKS_PROPERTY() rocks::int32 count = 0;\nprivate:\nstatic void Migrate() noexcept {}\n};\n}\n"],
            "RMC2000", "Migrate requires ROCKS_GENERATED_BODY");
        // 바깥에서 이름을 쓰는 생성 코드가 접근할 수 있도록 중첩 리플렉션 struct는 public 구역에 둔다.
        AddRejectGroup(context, "Meta.NestedReflectedStructMustBePublic", [
            Preamble + "class Outer {\nROCKS_STRUCT()\nstruct Leaf {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {};\n}\n"],
            "RMC2000", "must be public");
    }
}
