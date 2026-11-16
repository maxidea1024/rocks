using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 배열 필드 시험이다(Docs/Design/ReflectionFoundations.ko.md 5절).
internal static partial class MetaTests
{
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

    // 배열 필드의 스키마·생성 코드·형질 static_assert 시험이다(B5c).
    private static void RegisterArrays(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
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
    }

    // 배열 static_assert 진단 위치·원소 제한·깊이·WorldOnly 시험이다(B5c).
    private static void RegisterArrayRestrictions(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
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
        AddRejectGroup(context, "Meta.ArrayElementTypeIsRestricted", new[]
            {
                "rocks::Array<const rocks::int32>", "rocks::Array<rocks::int32*>", "rocks::Array<const rocks::int32*>",
                "rocks::ObjectRefArray<const Value>", "rocks::StructRefArray<const Holder>"
            }.Select(type => ArrayPreamble + "struct Value;\n" + ArrayStructs + $"ROCKS_CLASS()\nstruct Value : public rocks::Object {{\nROCKS_PROPERTY() {type} ints;\n}};\n}}\n"),
            "RMC2000", "array element");
        AddRejectGroup(context, "Meta.ArrayMaxElementsRangeIsChecked", [
            ArrayPreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY(MaxElements = 1048577) rocks::Array<rocks::int32> ints;\n};\n}\n"],
            "RMC2000", "exceeds 1048576");
        AddRejectGroup(context, "Meta.ArrayRequiredNeedsReferenceElements", new[]
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
                ("rocks::StructRefArray<Bundle> bundles;", "StructRefArray element struct probe::Bundle holds ObjectRefArray, StructRefArray or ObjectRefMap members"),
                ("rocks::StructRefArray<Mode> values;", "StructRefArray element 'probe::Mode' is not a reflected ROCKS_STRUCT"),
                ("rocks::StructRefArray<rocks::int32> values;", "StructRefArray element 'int' is not a reflected ROCKS_STRUCT"),
                ("rocks::Array<Movable> movables;", "element struct probe::Movable holds strong object references and needs rocks::StructRefArray<S>"),
                // 참조 컨테이너를 담는 원소 struct는 어느 컨테이너로도 쓸 수 없으므로 StructRefArray로 바꾸라는 안내보다 먼저 알린다.
                ("rocks::Array<MovableBundle> bundles;", "Array element struct probe::MovableBundle holds ObjectRefArray, StructRefArray or ObjectRefMap members")
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
        AddRejectGroup(context, "Meta.ObjectRefArrayTargetMustBeAClass", new[] { "rocks::ObjectRefArray<Leaf>", "rocks::ObjectRefArray<Mode>" }
            .Select(type => ArrayPreamble + "struct Value;\nROCKS_ENUM()\nenum class Mode : rocks::uint8 { Off = 0 };\n" + ArrayStructs +
                $"ROCKS_CLASS()\nstruct Value : public rocks::Object {{\nROCKS_PROPERTY() {type} refs;\n}};\n}}\n"),
            "RMC2003", "not a class");
        // 배열을 거친 자기 참조는 RMC2302이고 깊이는 원소 깊이 + 1로 센다(16은 받고 17은 RMC2303).
        AddRejectGroup(context, "Meta.ArraySelfReferenceIsACycle", [
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
        AddRejectGroup(context, "Meta.ArrayWorldOnlyElementNeedsWorldOnlyClass", new[]
            {
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Array<WorldBits> bits;\n};\n",
                "ROCKS_STRUCT()\nstruct Outer {\nexplicit Outer(rocks::Allocator& allocator) noexcept : inner(allocator) {}\nROCKS_PROPERTY() rocks::Array<WorldBits> inner;\n};\n" +
                    "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Outer bits;\n};\n"
            }.Select(declaration => ArrayPreamble + StructWorldBits + declaration + "}\n"),
            "RMC2206", "WorldOnly owner");
        AddRejectGroup(context, "Meta.NativeReferenceArraysAreRejected", new[] { "rocks::StructRefArray<Holder>", "rocks::Array<Holder>" }
            .Select(type => ArrayPreamble + "struct Value;\n" + ArrayStructs +
                $"ROCKS_CLASS()\nstruct Value : public rocks::Object {{\nROCKS_NATIVE() {type} cache;\n}};\n}}\n"),
            "RMC2205", "strong object reference");
    }
}
