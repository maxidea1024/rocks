namespace Rocks.ToolTests;

// meta 시험 중 Binds 미러 시험이다(Docs/Design/ReflectionFoundations.ko.md 4.5절).
internal static partial class MetaTests
{
    // B4c Binds 시험은 CoreTypeBindings 모듈로 실행한다(Binds는 그 모듈에서만 받는다).
    private const string BindsModule = "CoreTypeBindings";

    private const string BindsPreamble = "#include <Core/Math/Vector3.h>\n" + Preamble;

    private const string BindsVec3 = "ROCKS_STRUCT(Binds = \"rocks::Vec3\")\nstruct Vec3Binding {\n" +
        "ROCKS_PROPERTY() float x = 0;\nROCKS_PROPERTY() float y = 0;\nROCKS_PROPERTY() float z = 0;\n};\n";

    // Binds 미러 시험이다(B4c — CoreTypeBindings 모듈).
    private static void RegisterBinds(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
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
                ("static_assert(::rocks::IsSameV<decltype(BoundType::x), float>", "member type static_assert"),
                ("static_assert(::rocks::IsSameV<decltype(&BoundType::x), float (BoundType::*)>", "member declared in the real type itself"),
                ("static_assert(::rocks::IsStandardLayoutV<BoundType>", "standard layout for the order check"),
                ("static_assert(ROCKS_OFFSET_OF(BoundType, x) < ROCKS_OFFSET_OF(BoundType, y)", "declaration order check"),
                ("[[maybe_unused]] const auto& [m0, m1, m2] = value;", "structured binding count check"),
                ("detail::StructFieldOps<::probe::Pair,&::probe::Pair::b>", "member thunk of the real type"),
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
        // CurveData CV9: Core enum의 Binds 미러(ROCKS_ENUM(Binds = ...))는 실제 enum의 GeneratedType을 내고 열거자 값을 실제
        // enum과 대조한다. 실제 타입 필드(enum)는 미러로 풀린다. 실제 타입이 GeneratedType friend를 선언하면 미러가 private
        // 멤버를 나열할 수 있고, 미러의 static Validate(const 실제 타입&)가 검증 hook이 된다.
        suite.Add("Meta.BindsEnumMirrorPrivateMembersAndValidate", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler, BindsModule);
            string source = BindsPreamble +
                "enum class Mode : rocks::uint8 {\nA = 0,\nB = 2,\n};\n" +
                "ROCKS_ENUM(Binds = \"probe::Mode\")\nenum class ModeBinding : rocks::uint8 {\nA = 0,\nB = 2,\n};\n" +
                "class Keeper {\npublic:\nrocks::int32 Count() const noexcept { return count_; }\nprivate:\ntemplate <class>\nfriend struct ::rocks::GeneratedType;\nrocks::int32 count_ = 0;\nMode mode_ = Mode::A;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::Keeper\")\nstruct KeeperBinding {\nROCKS_PROPERTY(Name = \"count\") rocks::int32 count_ = 0;\nROCKS_PROPERTY() Mode mode_ = Mode::A;\n" +
                "static rocks::Result<void> Validate(const Keeper& value) noexcept { return value.Count() >= 0 ? rocks::Result<void>::Success() : rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument); }\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Mode mode = Mode::A;\nROCKS_PROPERTY() Keeper keeper;\n};\n}\n";
            await probe.GenerateAsync(source);
            using System.Text.Json.JsonDocument descriptor = System.Text.Json.JsonDocument.Parse(File.ReadAllBytes(probe.Descriptor));
            System.Text.Json.JsonElement Type(string name) => descriptor.RootElement.GetProperty("types").EnumerateArray()
                .Single(type => type.GetProperty("name").GetString() == name);
            string Target(string type, string field) => Type(type).GetProperty("fields").EnumerateArray()
                .Single(item => item.GetProperty("name").GetString() == field).GetProperty("value").GetProperty("type").GetString()!;
            Verify.Equal("probe::Mode", Type("probe::ModeBinding").GetProperty("binds").GetString(), "enum Binds in the descriptor");
            Verify.Equal(Type("probe::ModeBinding").GetProperty("id").GetString(), Target("probe::Value", "mode"), "real enum field resolves to the enum mirror");
            Verify.Equal(Type("probe::ModeBinding").GetProperty("id").GetString(), Target("probe::KeeperBinding", "mode_"), "mirror member enum resolves to the enum mirror");
            string generated = Compact(File.ReadAllText(probe.Output));
            foreach ((string expected, string reason) in new[]
            {
                ("struct GeneratedType<::probe::Mode>", "enum specialization of the real enum"),
                ("static_assert(static_cast<int64>(::probe::Mode::B) == 2ll", "enumerator value check against the real enum"),
                ("Span<const char>(\"probe::ModeBinding\", 18)", "enum schema name is the mirror name"),
                ("NativeTypeKey<::probe::Mode>()", "enum field native key of the real enum"),
                ("detail::StructFieldOps<::probe::Keeper,&::probe::Keeper::count_>", "private member thunk through the friend"),
                ("StructValidationResult(::probe::KeeperBinding::Validate(value))", "mirror Validate hook")
            })
            {
                Verify.That(generated.Contains(Compact(expected), StringComparison.Ordinal), "generated code missing " + reason + ": " + expected);
            }

            Verify.That(!generated.Contains("GeneratedType<::probe::ModeBinding>", StringComparison.Ordinal), "no specialization of the enum mirror type");
        });
        // 열거자 값·이름이 실제 enum과 다르거나 underlying이 다르면 생성 코드 검증 컴파일이 거부한다. enum 미러의 대상이 enum이
        // 아니면 RMC3000, CoreTypeBindings 밖은 RMC2004다. friend가 없는 private 멤버는 여전히 컴파일 실패다(아래 Hidden 사례).
        AddRejectGroup(context, "Meta.BindsEnumMismatchFailsToCompile", new[]
            {
                "ROCKS_ENUM(Binds = \"probe::Mode\")\nenum class ModeBinding : rocks::uint8 {\nA = 0,\nB = 3,\n};\n",
                "ROCKS_ENUM(Binds = \"probe::Mode\")\nenum class ModeBinding : rocks::uint8 {\nA = 0,\nC = 2,\n};\n",
                "ROCKS_ENUM(Binds = \"probe::Mode\")\nenum class ModeBinding : rocks::uint16 {\nA = 0,\nB = 2,\n};\n"
            }.Select(declaration => BindsPreamble + "enum class Mode : rocks::uint8 {\nA = 0,\nB = 2,\n};\n" + declaration + "}\n"),
            "RMC3000", "generated metadata contract rejected", BindsModule);
        AddRejectGroup(context, "Meta.BindsEnumTargetMustBeAnEnum", [
            BindsPreamble + "struct NotEnum {\nrocks::int32 a = 0;\n};\nROCKS_ENUM(Binds = \"probe::NotEnum\")\nenum class ModeBinding : rocks::uint8 {\nA = 0,\n};\n}\n"],
            "RMC3000", "does not name an enum type", BindsModule);
        AddRejectGroup(context, "Meta.BindsEnumIsAllowedOnlyInCoreTypeBindings", [
            BindsPreamble + "enum class Mode : rocks::uint8 {\nA = 0,\n};\nROCKS_ENUM(Binds = \"probe::Mode\")\nenum class ModeBinding : rocks::uint8 {\nA = 0,\n};\n}\n"],
            "RMC2004", "allowed only in module CoreTypeBindings");
        AddRejectGroup(context, "Meta.BindsIsAllowedOnlyInCoreTypeBindings", [BindsPreamble + BindsVec3 + "}\n"], "RMC2004", "allowed only in module CoreTypeBindings");
        // 별칭·앞의 ::·템플릿 인자 공백은 같은 정규 철자로 모이므로 같은 실제 타입의 두 번째 미러다.
        AddRejectGroup(context, "Meta.BindsConflictsAreRejected", [
            BindsPreamble + BindsVec3 + BindsVec3.Replace("Vec3Binding", "OtherBinding", StringComparison.Ordinal).Replace("rocks::Vec3", "rocks::Vector3<float>", StringComparison.Ordinal) + "}\n",
            BindsPreamble + BindsVec3.Replace("rocks::Vec3", "::rocks::Vec3", StringComparison.Ordinal) +
                BindsVec3.Replace("Vec3Binding", "OtherBinding", StringComparison.Ordinal).Replace("rocks::Vec3", "rocks::Vector3< float >", StringComparison.Ordinal) + "}\n",
            BindsPreamble + StructLeaf + "ROCKS_STRUCT(Binds = \"probe::Leaf\")\nstruct Vec3Binding {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n}\n"],
            "RMC2004", "binds", BindsModule);
        AddRejectGroup(context, "Meta.BindsTargetMustBeACompleteClass", [
            BindsPreamble + BindsVec3.Replace("rocks::Vec3", "rocks::uint8", StringComparison.Ordinal) + "}\n",
            BindsPreamble + BindsVec3.Replace("rocks::Vec3", "probe::Missing", StringComparison.Ordinal) + "}\n",
            BindsPreamble + "struct Later;\n" + BindsVec3.Replace("rocks::Vec3", "probe::Later", StringComparison.Ordinal) + "}\n"],
            "RMC3000", "does not name a complete class type", BindsModule);
        suite.Add("Meta.BindsSecondFailureNamesTheFailingMirror", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler, BindsModule);
            string first = BindsVec3.Replace("Vec3Binding", "OtherBinding", StringComparison.Ordinal);
            string second = BindsVec3.Replace("rocks::Vec3", "rocks::uint8", StringComparison.Ordinal);
            // 두 번째 static_assert 줄의 오류다. traits include를 진단 줄 계산에서 빠뜨리면 뒤 미러를 가리킨다.
            string third = BindsVec3.Replace("Vec3Binding", "Vec3dBinding", StringComparison.Ordinal)
                .Replace("rocks::Vec3", "rocks::Vector3<double>", StringComparison.Ordinal)
                .Replace("float", "double", StringComparison.Ordinal);
            await probe.RejectAsync(BindsPreamble + first + second + third + "}\n", "RMC3000",
                "probe::Vec3Binding: Binds = \"rocks::uint8\" does not name a complete class type");
        });
        // 철자 문법, 정규 철자가 descriptor binds 문법 밖(별칭으로 묶은 Fixed<-3>), 미러의 NATIVE 멤버·Migrate·필드 없음.
        AddRejectGroup(context, "Meta.BindsSpellingAndMirrorMembersAreRestricted", [
            BindsPreamble + BindsVec3.Replace("rocks::Vec3", "rocks::Vec3; int x", StringComparison.Ordinal) + "}\n",
            BindsPreamble + "template <int N>\nstruct Fixed {\nrocks::int32 v = 0;\n};\nusing FixedAlias = Fixed<-3>;\n" +
                "ROCKS_STRUCT(Binds = \"probe::FixedAlias\")\nstruct FixedBinding {\nROCKS_PROPERTY() rocks::int32 v = 0;\n};\n}\n",
            BindsPreamble + BindsVec3.Replace("ROCKS_PROPERTY() float z = 0;", "ROCKS_PROPERTY() float z = 0;\nROCKS_NATIVE() rocks::int32 cache = 0;", StringComparison.Ordinal) + "}\n",
            BindsPreamble + BindsVec3.Replace("ROCKS_PROPERTY() float z = 0;", "ROCKS_PROPERTY() float z = 0;\nstatic void Migrate() noexcept {}", StringComparison.Ordinal) + "}\n",
            BindsPreamble + "ROCKS_STRUCT(Binds = \"rocks::Vec3\")\nstruct Vec3Binding {};\n}\n"],
            "RMC2000", "Binds", BindsModule);
        // 미러 타입은 필드 형식으로 쓰지 않는다(실제 타입을 쓴다).
        AddRejectGroup(context, "Meta.BindsMirrorIsNotAFieldType", [
            BindsPreamble + BindsVec3 + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Vec3Binding position;\n};\n}\n"],
            "RMC2000", "is a Binds mirror declaration", BindsModule);
        // CoreTypeBindings에 의존하지 않고 Core 값 타입 필드를 쓰면 의존을 더하라고 안내한다.
        AddRejectGroup(context, "Meta.CoreValueFieldWithoutBindingsHintsTheDependency", [
            BindsPreamble + "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() rocks::Vec3 position;\n};\n}\n"],
            "RMC2000", "depend on CoreTypeBindings");
        // 미러가 자기가 묶는 실제 타입을 값으로 담으면(직접·서로) C++에서도 struct 순환이다(RMC2302).
        AddRejectGroup(context, "Meta.BindsCycleIsRejected", [
            BindsPreamble + "struct Real {\nrocks::int32 a = 0;\n};\nROCKS_STRUCT(Binds = \"probe::Real\")\nstruct RealBinding {\nROCKS_PROPERTY() Real inner;\n};\n}\n",
            BindsPreamble + "struct RealA {\nrocks::int32 a = 0;\n};\nstruct RealB {\nrocks::int32 b = 0;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::RealA\")\nstruct ABinding {\nROCKS_PROPERTY() RealB inner;\n};\n" +
                "ROCKS_STRUCT(Binds = \"probe::RealB\")\nstruct BBinding {\nROCKS_PROPERTY() RealA inner;\n};\n}\n"],
            "RMC2302", "cycle", BindsModule);
        // 미러와 실제 타입의 대조 실패는 생성 코드 검증 컴파일이 거부한다: 멤버 이름·형식·개수·순서(같은 형식 멤버끼리
        // 바뀐 순서 포함), private 데이터 멤버, base에만 있는 멤버.
        AddRejectGroup(context, "Meta.BindsMismatchFailsToCompile", new[]
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
    }
}
