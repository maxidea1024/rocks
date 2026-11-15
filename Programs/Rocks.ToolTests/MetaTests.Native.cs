namespace Rocks.ToolTests;

// meta 시험 중 정수 지정자·필드 지정자 제약·ROCKS_NATIVE 시험이다.
internal static partial class MetaTests
{
    // 정수 지정자·Required/MaxElements·도구 이름·ROCKS_NATIVE 탐침·지정자 오용 시험이다.
    private static void RegisterNative(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        AddRejectGroup(context, "Meta.IntegerSpecifierSyntax", new[] { "Version=0", "Version=01", "Version=2147483648", "Version=\"2\"", "Version=1x", "Version", "Version=-1", "Version=+1" }
            .Select(spec => Declaration().Replace("ROCKS_CLASS()", $"ROCKS_CLASS({spec})", StringComparison.Ordinal)), "RMC2000", "specifier");
        AddRejectGroup(context, "Meta.RequiredAndMaxElementsNeedMatchingFieldKinds", [Declaration("ROCKS_PROPERTY(Required) rocks::int32 count = 0;"),
            Declaration("ROCKS_PROPERTY(Required) rocks::AssetId count;")
                .Replace("namespace probe {", "#include <Object/AssetId.h>\nnamespace probe {", StringComparison.Ordinal),
            Declaration("ROCKS_PROPERTY(MaxElements=4) rocks::int32 count = 0;")], "RMC2202", "applies only to");
        AddRejectGroup(context, "Meta.ToolNameMustBeAnIdentifier", [Declaration("ROCKS_PROPERTY(Name=\"1bad\") rocks::int32 count = 0;"),
            Declaration("ROCKS_PROPERTY(Name=\"has space\") rocks::int32 count = 0;"),
            Declaration("ROCKS_PROPERTY(Name=\"\") rocks::int32 count = 0;")], "RMC2203", "Name specifier");
        AddRejectGroup(context, "Meta.ToolNamesAreUniqueIgnoringCase", [Declaration("ROCKS_PROPERTY(Name=\"Other\") rocks::int32 count = 0;\nROCKS_PROPERTY() rocks::int32 other = 0;")],
            "RMC2203", "tool name");
        AddRejectGroup(context, "Meta.PropertyAndNativeAreExclusive", [Declaration("ROCKS_PROPERTY() ROCKS_NATIVE() rocks::int32 count = 0;"),
            Declaration("ROCKS_NATIVE() ROCKS_PROPERTY() rocks::int32 count = 0;")], "RMC2204", "not both");
        AddRejectGroup(context, "Meta.NativeTakesNoSpecifiersAndNeedsGeneratedBodyWhenPrivate", [
            Declaration(extra: "ROCKS_NATIVE(Transient) rocks::int32 cache = 0;"),
            Declaration(extra: "private: ROCKS_NATIVE() rocks::int32 cache = 0;\npublic: rocks::int32 Read() const noexcept { return cache; }")], "RMC2000", "NATIVE");
        const string holders = "namespace probe { struct Holder { rocks::ObjectRef<rocks::Object> ref; };\n" +
            "template <class T> struct Box { T value{}; };\n" +
            "template <class T> struct Keep { rocks::ObjectRef<rocks::Object> ref; T value{}; };\n" +
            "template <class A, class B> struct Pair { A first{}; B second{}; };\n" +
            "struct Tagged { union { rocks::Object* object; unsigned long long bits; }; };\n";
        AddRejectGroup(context, "Meta.NativeMembersCannotHoldStrongReferences", new[]
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
        AddRejectGroup(context, "Meta.WorldOnlyOwnerTraitNeedsWorldOnlyClass", [
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
        AddRejectGroup(context, "Meta.UnknownDuplicateAndWrongValueSpecifiers", new[] { "UnknownFlag", "ReadOnly, ReadOnly", "ReadOnly=\"yes\"", "Category=12", "DisplayName" }
            .Select(spec => Declaration($"ROCKS_PROPERTY({spec}) rocks::int32 count=0;")), "RMC2000", "specifier");
        AddRejectGroup(context, "Meta.UnannotatedOwnerAndParameterMisuse", [
            Preamble + "struct Plain { ROCKS_PROPERTY() rocks::int32 count=0; };\n}\n",
            Preamble + "void F(ROCKS_PROPERTY() rocks::int32 value);\n}\n"], "RMC200", "PROPERTY");
        AddRejectGroup(context, "Meta.UnknownTypeSpecifier", [Declaration().Replace("ROCKS_CLASS()",
            "ROCKS_CLASS(UnknownClassFlag)", StringComparison.Ordinal)], "RMC2000", "specifier");

    }
}
