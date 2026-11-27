using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// meta 시험 중 struct 검증 hook 시험이다(Docs/Design/CurveData/README.ko.md 9.1절 — CV3): `static Result<void> Validate(const S&) noexcept`의
// 감지와 생성(RuntimeStructOps::validate·validateTree, ValidateSelf·ValidateTree, detail::ValueValidate 특수화)과 모양 규칙의 거부(RMC2309).
internal static partial class MetaTests
{
    private const string ValidateRangeBody = "ROCKS_PROPERTY() rocks::int32 low = 0;\nROCKS_PROPERTY() rocks::int32 high = 0;\n";

    private const string ValidateHook = "static rocks::Result<void> Validate(const Range& value) noexcept { return value.low <= value.high ? " +
        "rocks::Result<void>::Success() : rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument); }\n";

    // Range struct 하나(member는 Validate 자리)와 그것을 담는 클래스다.
    private static string ValidateSource(string member, string keyword = "struct", string body = ValidateRangeBody) =>
        Preamble + $"ROCKS_STRUCT()\n{keyword} Range {{\n" + (keyword == "class" ? "public:\n" : "") + body + member + "};\n" +
        "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Range range;\n};\n}\n";

    private static void RegisterValidateHooks(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        // 검증 hook이 있는 struct(public·GENERATED_BODY private), 그 struct를 담는 struct, hook을 물려받는 파생 struct, hook 없는 struct의
        // 생성 코드다.
        suite.Add("Meta.ValidateHookReachesGeneratedOps", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            string source = Preamble +
                "ROCKS_STRUCT()\nstruct Range {\n" + ValidateRangeBody + ValidateHook + "};\n" +
                "ROCKS_STRUCT()\nclass Hidden {\nROCKS_GENERATED_BODY()\npublic:\nROCKS_PROPERTY() Range range;\n" +
                "private:\nstatic rocks::Result<void> Validate(const Hidden&) noexcept { return rocks::Result<void>::Success(); }\n};\n" +
                "ROCKS_STRUCT()\nstruct Child : Range {\nROCKS_PROPERTY() rocks::int32 extra = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Plain {\nROCKS_PROPERTY() rocks::int32 count = 0;\n};\n" +
                "ROCKS_STRUCT()\nstruct Holder {\nROCKS_PROPERTY() Range range;\nROCKS_PROPERTY() Plain plain;\n};\n" +
                "ROCKS_CLASS()\nstruct Value : public rocks::Object {\nROCKS_PROPERTY() Holder holder;\nROCKS_PROPERTY() Hidden hidden;\n" +
                "ROCKS_PROPERTY() Child child;\n};\n}\n";
            await probe.GenerateAsync(source);
            string generated = Compact(File.ReadAllText(probe.Output));
            string generatedSource = File.ReadAllText(probe.Source);
            // 타입 하나의 생성 코드다: 생성 헤더의 특수화(형질·선언)와 생성 소스의 그 타입 구간(본문·정적 검사)을 잇는다.
            string Section(string name)
            {
                int start = generated.IndexOf($"structGeneratedType<::probe::{name}>", StringComparison.Ordinal);
                Verify.That(start >= 0, "specialization missing: " + name);
                int end = generated.IndexOf("structGeneratedType<", start + 10, StringComparison.Ordinal);
                return (end < 0 ? generated[start..] : generated[start..end]) + Compact(SourceSection(generatedSource, "probe::" + name));
            }

            foreach ((string name, string expected, string reason) in new[]
            {
                ("Range", "staticconstexprboolHasValidateSelf=true;", "own hook"),
                ("Range", Compact("return ::rocks::detail::StructValidationResult(::probe::Range::Validate(value));"), "ValidateSelf calls Validate"),
                ("Range", Compact("ops.validate = +[](const void* value) noexcept -> Result<void> { return ValidateSelf(*static_cast<const ::probe::Range*>(value)); };"), "validate op"),
                ("Range", Compact("ops.validateTree = +[](const void* value) noexcept -> Result<void> { return ValidateTree(*static_cast<const ::probe::Range*>(value)); };"), "validateTree op"),
                ("Range", Compact("static_assert(::rocks::IsSameV<decltype(&::probe::Range::Validate), ::rocks::Result<void> (*)(const ::probe::Range&) noexcept>"), "signature static_assert"),
                ("Hidden", "staticconstexprboolHasValidateSelf=true;", "private hook through GENERATED_BODY"),
                ("Child", Compact("static constexpr bool HasValidateSelf = false || GeneratedType<::probe::Range>::HasValidateSelf;"), "inherited base hook"),
                ("Child", Compact("GeneratedType<::probe::Range>::ValidateSelf(static_cast<const ::probe::Range&>(value))"), "base hook runs first"),
                ("Holder", "staticconstexprboolHasValidateSelf=false;", "no own hook"),
                ("Holder", Compact("::rocks::detail::ValueValidate<decltype(::probe::Holder::range)>::HasHook"), "member hook in the tree"),
                ("Plain", "staticconstexprboolHasValidateSelf=false;", "plain struct")
            })
            {
                Verify.That(Section(name).Contains(expected, StringComparison.Ordinal), $"generated code missing {reason} for {name}: {expected}");
            }

            Verify.That(generated.Contains(Compact("template <> struct detail::ValueValidate<::probe::Range> {"), StringComparison.Ordinal),
                "ValueValidate specialization");
            ProcessResult compiled = await probe.CompileGeneratedAsync();
            Verify.Equal(0, compiled.ExitCode, "generated validate hooks compile: " + compiled.StdOut + compiled.StdErr);
        });
        // 모양이 다른 Validate는 조용히 hook 없이 등록되지 않고 RMC2309다: 비정적, 인자·반환·noexcept가 다른 시그니처, 함수 템플릿, overload,
        // GENERATED_BODY 없는 private.
        AddRejectGroup(context, "Meta.ValidateHookShapeIsChecked", [
            ValidateSource("rocks::Result<void> Validate() const noexcept { return rocks::Result<void>::Success(); }\n"),
            ValidateSource("static rocks::Result<void> Validate(Range value) noexcept { (void)value; return rocks::Result<void>::Success(); }\n"),
            ValidateSource("static rocks::Result<void> Validate(const Range&) { return rocks::Result<void>::Success(); }\n"),
            ValidateSource("static bool Validate(const Range&) noexcept { return true; }\n"),
            ValidateSource("template <class T>\nstatic rocks::Result<void> Validate(const T&) noexcept { return rocks::Result<void>::Success(); }\n"),
            ValidateSource(ValidateHook + "static rocks::Result<void> Validate(const Range&, int) noexcept { return rocks::Result<void>::Success(); }\n"),
            ValidateSource("private:\n" + ValidateHook),
            ValidateSource("static consteval rocks::Result<void> Validate(const Range&) noexcept { return rocks::Result<void>::Success(); }\n")],
            "RMC2309", "validation hook");
        // 숨은 friend Validate와 using-declaration(CV3 리뷰 10)도 RMC2309다.
        AddRejectGroup(context, "Meta.ValidateHookRejectsFriendAndUsing", [
            ValidateSource("friend rocks::Result<void> Validate(const Range&) noexcept { return rocks::Result<void>::Success(); }\n"),
            ValidateSource(ValidateHook + "using HookBase::Validate;\n").Replace("ROCKS_STRUCT()\nstruct Range {",
                "struct HookBase { static rocks::Result<void> Validate(const HookBase&) noexcept { return rocks::Result<void>::Success(); } };\n" +
                "ROCKS_STRUCT()\nstruct Range : HookBase {", StringComparison.Ordinal)],
            "RMC2309", "Validate");
        // 정상 선언의 다른 철자(이름공간 처음 `::`, 별칭 `const Self&`)는 hook으로 받는다(CV3 리뷰 10).
        suite.Add("Meta.ValidateHookAcceptsQualifiedAndAliasSpellings", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ValidateSource("using Self = Range;\n" +
                "static ::rocks::Result<void> Validate(const Self& value) noexcept { return value.low <= value.high ? " +
                "rocks::Result<void>::Success() : rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument); }\n"));
            string generated = Compact(probe.GeneratedText());
            Verify.That(generated.Contains("staticconstexprboolHasValidateSelf=true;", StringComparison.Ordinal), "alias spelling hook detected");
        });
        // consteval 판정은 주석을 걷어낸 원문으로 한다(CV4 리뷰 6): 바로 앞 줄·블록 주석의 낱말 `consteval`은 정상 hook을 거부하지 않는다(숫자 구분자 `1'000` 뒤의 주석도 — CV5 리뷰 10).
        // 중첩 struct의 별칭은 바깥 struct의 별칭 철자 풀이를 지우지 않는다(중첩 선언은 바깥 멤버 순회가 끝난 뒤 처리된다).
        suite.Add("Meta.ValidateHookIgnoresCommentsAndNestedAliases", async () =>
        {
            using Probe probe = new(options, actualEnvironment, generator, compiler);
            await probe.GenerateAsync(ValidateSource("using Self = Range;\n" +
                "ROCKS_STRUCT()\nstruct Inner {\nusing Self = Inner;\nROCKS_PROPERTY() rocks::int32 n = 0;\n" +
                "static rocks::Result<void> Validate(const Self&) noexcept { return rocks::Result<void>::Success(); }\n};\n" +
                "static constexpr rocks::int32 Big = 1'000; // the separator must not open a character literal (consteval)\n" +
                "// must not be consteval\n" +
                "/* nor consteval */ static rocks::Result<void> Validate(const Self& value) noexcept { return value.low <= value.high ? " +
                "rocks::Result<void>::Success() : rocks::Result<void>::Failure(rocks::ErrorCode::InvalidArgument); }\n"));
            string generated = Compact(probe.GeneratedText());
            Verify.That(generated.Contains(Compact("static_assert(::rocks::IsSameV<decltype(&::probe::Range::Validate)"), StringComparison.Ordinal),
                "outer hook detected after a comment naming consteval");
            Verify.That(generated.Contains(Compact("static_assert(::rocks::IsSameV<decltype(&::probe::Range::Inner::Validate)"), StringComparison.Ordinal),
                "nested hook detected");
        });
        // hook struct는 참조 없는 값이다(리플렉션 쓰기가 통째로 복사해 검증한다).
        AddRejectGroup(context, "Meta.ValidateHookRejectsReferenceStructs", [
            ValidateSource(ValidateHook, body: ValidateRangeBody + "ROCKS_PROPERTY() rocks::ObjectRef<Value> owner;\n").Replace(
                "ROCKS_STRUCT()", "struct Value;\nROCKS_STRUCT()", StringComparison.Ordinal)],
            "RMC2309", "Validate hook cannot hold strong object references");
    }
}
