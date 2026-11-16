#include "TextTestSupport.h"

// 리플렉션 B9i3b 값 표기 시험이다(Docs/Design/ReflectionExtensions.ko.md 10.4절, fixture
// ReflectionTestFixtures/TextFixture.h): 단일 값 종류별 왕복과 경계값, 비유한 실수, enum 접두·숨김
// 열거자·Flags `+`, Name NFC·intern, 참조 resolver(없음·있음·Hard·Weak·Soft 구분), ClassRef 텍스트,
// 목록 escape 표·원소 수, Set·Map 중복 키와 첫 `:` 분해, struct·Optional·원소 자리, 오류 모두
// 모으기.
using namespace rocks;
using namespace rocks::test::text;

namespace {

constexpr ObjectHandle ResolvedTarget{1, 2, 3};
constexpr Guid SoftAsset{0x1111222233334444ULL, 0x5555666677778888ULL};

// 논리 경로 둘을 아는 resolver다(Hard·Weak `/Game/A`, Soft `/Game/Rock.Rock` = SoftAsset + "Rock").
class FixedResolver final : public TextReferenceResolver {
public:
    Result<void> Resolve(StringView text, TypeId expected, TextReferenceKind kind,
                         FieldValue& out) noexcept override {
        ++calls;
        lastKind = kind;
        lastExpected = expected;
        if (kind == TextReferenceKind::Soft) {
            if (text != "/Game/Rock.Rock") {
                return Result<void>::Failure(ErrorCode::NotFound);
            }

            out.guid = SoftAsset;
            return out.string.TryAssign("Rock");
        }

        if (text != "/Game/A") {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        out.reference = ResolvedTarget;
        return Result<void>::Success();
    }

    Result<void> Format(TextReferenceKind kind, const FieldValue& value,
                        StringBuilderBase& out) noexcept override {
        if (kind == TextReferenceKind::Soft) {
            if (value.guid != SoftAsset || value.string.View() != "Rock") {
                return Result<void>::Failure(ErrorCode::NotFound);
            }

            return out.TryAppend("/Game/Rock.Rock");
        }

        if (value.reference != ResolvedTarget) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        return out.TryAppend("/Game/A");
    }

    usize calls = 0;
    TextReferenceKind lastKind = TextReferenceKind::Hard;
    TypeId lastExpected{};
};

// 단일 값 하나를 읽는다(성공해야 한다).
FieldValue ReadSingle(TextCodecContext& context, const TextSlot& slot, StringView text) noexcept {
    TextValue value(context.GetAllocator());
    auto parsed = ParseTextValue(context, slot, text, TextSource::Assignment(0), "p", value);
    CHECK(parsed && parsed.Value() && value.shape == TextValueShape::Single);
    return Move(value.single);
}

} // namespace

// bool·정수 8종·Guid·AssetId·Digest·String의 왕복과 경계값: bool 입력 on·off(대소문자 무시)·출력
// true·false, 정수는 정규 10진(앞자리 0·`+`·`-0` 거부)이고 좁은 폭·64비트 범위 밖은 OutOfRange,
// Guid·AssetId는 36자 소문자만, Digest는 64자 소문자만, String은 목록이 아니면 escape 없이
// 그대로다.
TEST(TextValue_ScalarRoundTripAndBounds) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TypeLease& holder = setup.holder;

    const TextSlot enabled = SlotOf(context, holder, "enabled");
    CHECK(RoundTrip(context, enabled, "true"));
    CHECK(RoundTrip(context, enabled, "ON", "true"));
    CHECK(RoundTrip(context, enabled, "off", "false"));
    CHECK(Rejects(context, enabled, "yes", TextDiagnosticCode::InvalidValue));

    const TextSlot small = SlotOf(context, holder, "small");
    CHECK(RoundTrip(context, small, "255"));
    CHECK(RoundTrip(context, small, "0"));
    CHECK(Rejects(context, small, "256", TextDiagnosticCode::OutOfRange));
    CHECK(Rejects(context, small, "007", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, small, "+1", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, small, "-1", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, small, "", TextDiagnosticCode::InvalidValue));

    const TextSlot medium = SlotOf(context, holder, "medium");
    CHECK(RoundTrip(context, medium, "65535"));
    CHECK(Rejects(context, medium, "65536", TextDiagnosticCode::OutOfRange));
    const TextSlot count = SlotOf(context, holder, "count");
    CHECK(RoundTrip(context, count, "4294967295"));
    CHECK(Rejects(context, count, "4294967296", TextDiagnosticCode::OutOfRange));
    const TextSlot big = SlotOf(context, holder, "big");
    CHECK(RoundTrip(context, big, "18446744073709551615"));
    CHECK(Rejects(context, big, "18446744073709551616", TextDiagnosticCode::OutOfRange));

    const TextSlot tiny = SlotOf(context, holder, "tiny");
    CHECK(RoundTrip(context, tiny, "-128"));
    CHECK(RoundTrip(context, tiny, "127"));
    CHECK(Rejects(context, tiny, "-129", TextDiagnosticCode::OutOfRange));
    CHECK(Rejects(context, tiny, "-0", TextDiagnosticCode::InvalidValue));
    const TextSlot narrow = SlotOf(context, holder, "narrow");
    CHECK(RoundTrip(context, narrow, "-32768"));
    CHECK(Rejects(context, narrow, "32768", TextDiagnosticCode::OutOfRange));
    const TextSlot offset = SlotOf(context, holder, "offset");
    CHECK(RoundTrip(context, offset, "-2147483648"));
    CHECK(Rejects(context, offset, "2147483648", TextDiagnosticCode::OutOfRange));
    const TextSlot wide = SlotOf(context, holder, "wide");
    CHECK(RoundTrip(context, wide, "-9223372036854775808"));
    CHECK(Rejects(context, wide, "9223372036854775808", TextDiagnosticCode::OutOfRange));

    const TextSlot ratio = SlotOf(context, holder, "ratio");
    CHECK(RoundTrip(context, ratio, "0.1"));
    CHECK(RoundTrip(context, ratio, "-2.25"));
    CHECK(ReadSingle(context, ratio, "0.1").float32 == 0.1f);
    const TextSlot precise = SlotOf(context, holder, "precise");
    CHECK(RoundTrip(context, precise, "0.1"));
    CHECK(ReadSingle(context, precise, "0.1").float64 == 0.1);
    CHECK(Rejects(context, precise, "1,5", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, precise, "0x10", TextDiagnosticCode::InvalidValue));

    const TextSlot id = SlotOf(context, holder, "id");
    CHECK(RoundTrip(context, id, "3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7b"));
    CHECK(Rejects(context, id, "3F2A9C1E-7B4D-4E8F-9A01-2C3D4E5F6A7B",
                  TextDiagnosticCode::InvalidValue));
    CHECK(
        Rejects(context, id, "3f2a9c1e7b4d4e8f9a012c3d4e5f6a7b", TextDiagnosticCode::InvalidValue));
    const TextSlot asset = SlotOf(context, holder, "asset");
    CHECK(RoundTrip(context, asset, "00000000-0000-0000-0000-000000000000"));
    const TextSlot digest = SlotOf(context, holder, "digest");
    const StringView hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    CHECK(RoundTrip(context, digest, hex));
    CHECK(Rejects(context, digest,
                  "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef",
                  TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, digest, "0123", TextDiagnosticCode::InvalidValue));

    const TextSlot label = SlotOf(context, holder, "label");
    CHECK(RoundTrip(context, label, "a,b; c:\\d"));
    CHECK(RoundTrip(context, label, ""));
    CHECK(Rejects(context, label, StringView("\xff", 1), TextDiagnosticCode::InvalidValue));
}

// 비유한 실수: `nan`·`inf`·`-inf`만(소문자 그대로) 받고 출력도 같다. Core 해석이 받는 `NaN`·
// `infinity`·`+inf`는 거부하고, 범위 밖 10진은 OutOfRange다.
TEST(TextValue_NonFiniteReals) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const StringView names[] = {"ratio", "precise"};
    for (StringView name : names) {
        const TextSlot slot = SlotOf(context, setup.holder, name);
        CHECK(RoundTrip(context, slot, "nan"));
        CHECK(RoundTrip(context, slot, "inf"));
        CHECK(RoundTrip(context, slot, "-inf"));
        CHECK(Rejects(context, slot, "NaN", TextDiagnosticCode::InvalidValue));
        CHECK(Rejects(context, slot, "infinity", TextDiagnosticCode::InvalidValue));
        CHECK(Rejects(context, slot, "+inf", TextDiagnosticCode::InvalidValue));
    }

    const TextSlot ratio = SlotOf(context, setup.holder, "ratio");
    const FieldValue nan = ReadSingle(context, ratio, "nan");
    CHECK(nan.float32 != nan.float32);
    CHECK(Rejects(context, ratio, "1e39", TextDiagnosticCode::OutOfRange));
    const TextSlot precise = SlotOf(context, setup.holder, "precise");
    CHECK(Rejects(context, precise, "1e400", TextDiagnosticCode::OutOfRange));
    CHECK(ReadSingle(context, precise, "-inf").float64 < -1e308);
}

// enum: 도구 이름(대소문자 무시)과 ToolPrefix가 붙은 이름을 받고 출력은 실효 도구 이름이다. 숨김
// 열거자는 HiddenEnumerator다. Flags는 `A+B`·`A|B`를 받아 선언 순 `+`로 쓰고 0은 빈 값이며, 숨김
// 비트(Alpha)를 쓰면 HiddenEnumerator, 빈 조각·모르는 이름은 InvalidValue다.
TEST(TextValue_EnumAndFlags) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TextSlot quality = SlotOf(context, setup.holder, "quality");
    CHECK(RoundTrip(context, quality, "High"));
    CHECK(RoundTrip(context, quality, "high", "High"));
    CHECK(RoundTrip(context, quality, "TQ_LOW", "Low"));
    CHECK(Rejects(context, quality, "Secret", TextDiagnosticCode::HiddenEnumerator));
    CHECK(Rejects(context, quality, "Medium", TextDiagnosticCode::InvalidValue));
    const FieldValue high = ReadSingle(context, quality, "High");
    CHECK(high.kind == ValueKind::Enum);
    CHECK(high.type == GeneratedType<reflection_fixture::TextQuality>::Schema().id);
    CHECK(high.signedInteger == 1);

    const TextSlot channels = SlotOf(context, setup.holder, "channels");
    CHECK(RoundTrip(context, channels, "Red+Blue"));
    CHECK(RoundTrip(context, channels, "blue|red", "Red+Blue"));
    CHECK(RoundTrip(context, channels, "Green+Red+Green", "Red+Green"));
    CHECK(RoundTrip(context, channels, ""));
    CHECK(Rejects(context, channels, "Red+Alpha", TextDiagnosticCode::HiddenEnumerator));
    CHECK(Rejects(context, channels, "Red+", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, channels, "Pink", TextDiagnosticCode::InvalidValue));
}

// Name: NFC로 정규화한 뒤 pool에 intern한다(분해형 é → 조합형). `None`은 None 토큰이고 출력도
// `None`이다. pool이 없으면 Unsupported, 잘못된 UTF-8·NUL은 InvalidValue다.
TEST(TextValue_NameNormalizesAndInterns) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TextSlot tag = SlotOf(context, setup.holder, "tag");
    CHECK(RoundTrip(context, tag, "Cafe\xCC\x81", "Caf\xC3\xA9"));
    CHECK(RoundTrip(context, tag, "None"));
    CHECK(ReadSingle(context, tag, "None").name.IsNone());
    const FieldValue first = ReadSingle(context, tag, "Rock_2");
    const FieldValue second = ReadSingle(context, tag, "rock_2");
    CHECK(first.name == second.name);
    CHECK(Rejects(context, tag, StringView("a\0b", 3), TextDiagnosticCode::InvalidValue));

    TextCodecOptions noPool;
    TextDiagnostics diagnostics(allocator);
    TextCodecContext bare(setup.types, noPool, diagnostics, allocator);
    CHECK(Rejects(bare, tag, "Rock", TextDiagnosticCode::Unsupported));
}

// 참조: `none`(대소문자 무시)은 null이고 출력은 `none`이다. resolver가 없으면 Soft는 36자
// AssetId(빈 객체 경로)만 받고 non-null Hard·Weak와 경로 Soft는 Unsupported다. resolver가 있으면
// 종류(Hard·Weak· Soft)와 기대 타입을 넘겨 풀고, 풀지 못하면 Unresolved, 출력은 resolver의
// 텍스트다.
TEST(TextValue_ReferencesUseResolver) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    {
        TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
        const TextSlot next = SlotOf(context, setup.holder, "next");
        CHECK(RoundTrip(context, next, "none"));
        CHECK(RoundTrip(context, next, "NONE", "none"));
        CHECK(ReadSingle(context, next, "none").reference.IsNull());
        CHECK(Rejects(context, next, "/Game/A", TextDiagnosticCode::Unsupported));
        const TextSlot source = SlotOf(context, setup.holder, "source");
        CHECK(RoundTrip(context, source, "11112222-3333-4444-5555-666677778888"));
        CHECK(Rejects(context, source, "/Game/Rock.Rock", TextDiagnosticCode::Unsupported));
    }

    FixedResolver resolver;
    setup.options.resolver = &resolver;
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TextSlot next = SlotOf(context, setup.holder, "next");
    CHECK(RoundTrip(context, next, "/Game/A"));
    CHECK(resolver.lastKind == TextReferenceKind::Hard);
    CHECK(resolver.lastExpected == GeneratedType<TextHolder>::Schema().id);
    CHECK(ReadSingle(context, next, "/Game/A").reference == ResolvedTarget);
    CHECK(Rejects(context, next, "/Game/Missing", TextDiagnosticCode::Unresolved));
    const usize before = resolver.calls;
    CHECK(RoundTrip(context, next, "none"));
    CHECK(resolver.calls == before);

    const TextSlot peer = SlotOf(context, setup.holder, "peer");
    CHECK(RoundTrip(context, peer, "/Game/A"));
    CHECK(resolver.lastKind == TextReferenceKind::Weak);
    const TextSlot source = SlotOf(context, setup.holder, "source");
    CHECK(RoundTrip(context, source, "/Game/Rock.Rock"));
    CHECK(resolver.lastKind == TextReferenceKind::Soft);
    const FieldValue soft = ReadSingle(context, source, "/Game/Rock.Rock");
    CHECK(soft.guid == SoftAsset);
    CHECK(soft.string.View() == "Rock");
    // resolver가 있으면 AssetId 텍스트도 resolver가 푼다(이 resolver는 모른다). resolver가 쓰지
    // 못한 경로 없는 Soft는 36자 AssetId로 쓰고, 경로 있는 Soft·Hard는 Unsupported다.
    CHECK(Rejects(context, source, "11112222-3333-4444-5555-666677778888",
                  TextDiagnosticCode::Unresolved));
    FieldValue bare(allocator);
    bare.kind = ValueKind::SoftObjectReference;
    bare.guid = SoftAsset;
    StringBuilder<64> text(allocator);
    REQUIRE(FormatTextScalar(context, SlotValueType(source), bare, text));
    CHECK(text.View() == "11112222-3333-4444-5555-666677778888");
    REQUIRE(bare.string.TryAssign("Other"));
    StringBuilder<64> unwritten(allocator);
    auto failed = FormatTextScalar(context, SlotValueType(source), bare, unwritten);
    REQUIRE(!failed);
    CHECK(failed.Error() == ErrorCode::Unsupported);
}

// ClassRef: 타입 도구 이름(이름을 바꾼 `Text.Renamed` 포함), TypeId 36자 소문자 하이픈·32자 소문자
// 16진을 받고, 출력은 등록된 클래스의 도구 이름(미등록 TypeId는 36자)이다. `none`은 null, 미등록
// 이름은 UnknownType, Base의 파생이 아닌 클래스·대문자 TypeId·형식 밖 텍스트는 InvalidValue다.
TEST(TextValue_ClassRefText) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TextSlot kind = SlotOf(context, setup.holder, "kind");
    const TypeId derived = GeneratedType<reflection_fixture::TextKindDerived>::Schema().id;
    CHECK(RoundTrip(context, kind, "TextKindDerived"));
    CHECK(RoundTrip(context, kind, "textkindderived", "TextKindDerived"));
    CHECK(RoundTrip(context, kind, "Text.Renamed"));
    CHECK(RoundTrip(context, kind, "TextKindBase"));
    CHECK(RoundTrip(context, kind, "none"));
    CHECK(RoundTrip(context, kind, "None", "none"));
    CHECK(ReadSingle(context, kind, "TextKindDerived").guid == derived);

    StringBuilder<64> dashed(allocator);
    REQUIRE(dashed.TryAppendFormat("{}", derived));
    CHECK(RoundTrip(context, kind, dashed.View(), "TextKindDerived"));
    StringBuilder<64> compact(allocator);
    for (char c : dashed.View()) {
        if (c != '-') {
            REQUIRE(compact.TryAppendAscii(c));
        }
    }

    CHECK(ReadSingle(context, kind, compact.View()).guid == derived);
    StringBuilder<64> upper(allocator);
    for (char c : dashed.View()) {
        REQUIRE(upper.TryAppendAscii(c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c));
    }

    CHECK(Rejects(context, kind, upper.View(), TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, kind, "TextStranger", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, kind, "TextPoint", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, kind, "Missing", TextDiagnosticCode::UnknownType));
    CHECK(Rejects(context, kind, "a b", TextDiagnosticCode::InvalidValue));

    FieldValue unknown(allocator);
    unknown.kind = ValueKind::ClassReference;
    unknown.guid = Guid{0x0123456789abcdefULL, 0x0fedcba987654321ULL};
    StringBuilder<64> text(allocator);
    REQUIRE(FormatTextScalar(context, SlotValueType(kind), unknown, text));
    CHECK(text.View() == "01234567-89ab-cdef-0fed-cba987654321");
}

// 목록: 쉼표 목록(빈 값 = 빈 배열), 원소 안 `\,`·`\;`는 그 글자, 그 밖의 `\`는 그대로(`C:\x`), 원래
// `\` 뒤의 `:`는 `\\:`로 왕복한다. 원소 하나뿐인 빈 문자열과 다음 `,`와 쌍이 되는 `\`로 끝나는
// 원소는 쓸 수 없다(Unsupported). 원소 진단은 원소마다 모두 남고, 원소 수는 maxElements·정적 배열
// N으로 본다. struct 원소 배열은 빈 값만 받는다.
TEST(TextValue_ListsEscapesAndCounts) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TextSlot numbers = SlotOf(context, setup.holder, "numbers");
    CHECK(RoundTrip(context, numbers, "1,2,3"));
    CHECK(RoundTrip(context, numbers, ""));
    CHECK(Rejects(context, numbers, "1,,3", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, numbers, "1, 2", TextDiagnosticCode::InvalidValue));
    context.Diagnostics().Clear();
    TextValue value(allocator);
    auto parsed =
        ParseTextValue(context, numbers, "1,x,2,y", TextSource::Assignment(4), "numbers", value);
    REQUIRE(parsed);
    CHECK(!parsed.Value());
    CHECK(context.Diagnostics().Count() == 2);
    CHECK(context.Diagnostics().Get(1).path == "numbers");

    const TextSlot words = SlotOf(context, setup.holder, "words");
    CHECK(RoundTrip(context, words, "a\\,b,c\\;d,C:\\x"));
    TextValue split(allocator);
    REQUIRE(
        ParseTextValue(context, words, "a\\,b,c\\;d,C:\\x,p\\\\:q", TextSource{}, "words", split));
    REQUIRE(split.elements.Size() == 4);
    CHECK(split.elements[0].string.View() == "a,b");
    CHECK(split.elements[1].string.View() == "c;d");
    CHECK(split.elements[2].string.View() == "C:\\x");
    CHECK(split.elements[3].string.View() == "p\\:q");
    String text(allocator);
    REQUIRE(FormatTextValue(context, words, split, text));
    CHECK(text.View() == "a\\,b,c\\;d,C:\\x,p\\\\:q");
    CHECK(RoundTrip(context, words, ",", ","));
    CHECK(RoundTrip(context, words, "end\\"));

    TextValue lone(allocator);
    lone.shape = TextValueShape::List;
    FieldValue empty(allocator);
    empty.kind = ValueKind::Utf8String;
    REQUIRE(lone.elements.TryPushBack(Move(empty)));
    String out(allocator);
    auto unsupported = FormatTextValue(context, words, lone, out);
    REQUIRE(!unsupported);
    CHECK(unsupported.Error() == ErrorCode::Unsupported);
    TextValue trailing(allocator);
    REQUIRE(ParseTextValue(context, words, "a\\", TextSource{}, "words", trailing));
    FieldValue more(allocator);
    more.kind = ValueKind::Utf8String;
    REQUIRE(more.string.TryAssign("b"));
    REQUIRE(trailing.elements.TryPushBack(Move(more)));
    auto pairing = FormatTextValue(context, words, trailing, out);
    REQUIRE(!pairing);
    CHECK(pairing.Error() == ErrorCode::Unsupported);

    const TextSlot triple = SlotOf(context, setup.holder, "triple");
    CHECK(RoundTrip(context, triple, "1,2,3"));
    CHECK(Rejects(context, triple, "1,2", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, triple, "", TextDiagnosticCode::InvalidValue));

    const TextSlot points = SlotOf(context, setup.holder, "points");
    CHECK(RoundTrip(context, points, ""));
    CHECK(Rejects(context, points, "{}", TextDiagnosticCode::InvalidValue));

    setup.options.maxElements = 2;
    TextDiagnostics limited(allocator);
    TextCodecContext small(setup.types, setup.options, limited, allocator);
    CHECK(RoundTrip(small, numbers, "1,2"));
    CHECK(Rejects(small, numbers, "1,2,3", TextDiagnosticCode::OutOfRange));
}

// Set·Map: Set 원소와 Map 키의 중복은 DuplicateKey다(Name은 pool 비교 — 대소문자 무시, String은
// byte). Map 항목은 첫 escape되지 않은 `:`에서 나누고(값 안 `C:\x`는 그대로), 키 안 `:`는 `\:`로
// 왕복한다. `:` 없는 항목은 InvalidValue, 숨김 열거자 키는 HiddenEnumerator다. ClassRef·Soft 키도
// 값 표기 규칙이다. struct 값 Map은 빈 값만 받는다.
TEST(TextValue_SetAndMapEntries) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    const TextSlot tags = SlotOf(context, setup.holder, "tags");
    CHECK(RoundTrip(context, tags, "Rock,Grass"));
    CHECK(Rejects(context, tags, "Rock,ROCK", TextDiagnosticCode::DuplicateKey));

    const TextSlot scores = SlotOf(context, setup.holder, "scores");
    CHECK(RoundTrip(context, scores, "a:1,b:2"));
    CHECK(RoundTrip(context, scores, "A:1,a:2"));
    CHECK(RoundTrip(context, scores, "k\\:1:2"));
    CHECK(RoundTrip(context, scores, ""));
    CHECK(Rejects(context, scores, "a:1,a:2", TextDiagnosticCode::DuplicateKey));
    CHECK(Rejects(context, scores, "a", TextDiagnosticCode::InvalidValue));
    CHECK(Rejects(context, scores, "a:x", TextDiagnosticCode::InvalidValue));
    TextValue entries(allocator);
    REQUIRE(ParseTextValue(context, scores, "k\\:1:2", TextSource{}, "scores", entries));
    REQUIRE(entries.shape == TextValueShape::Entries);
    REQUIRE(entries.elements.Size() == 1);
    CHECK(entries.elements[0].string.View() == "k:1");
    CHECK(entries.values[0].signedInteger == 2);

    const TextSlot byQuality = SlotOf(context, setup.holder, "byQuality");
    CHECK(RoundTrip(context, byQuality, "High:C:\\x,Low:a\\,b"));
    TextValue paths(allocator);
    REQUIRE(ParseTextValue(context, byQuality, "High:C:\\x", TextSource{}, "byQuality", paths));
    CHECK(paths.values[0].string.View() == "C:\\x");
    CHECK(Rejects(context, byQuality, "Secret:x", TextDiagnosticCode::HiddenEnumerator));

    const TextSlot byClass = SlotOf(context, setup.holder, "byClass");
    CHECK(RoundTrip(context, byClass, "TextKindDerived:1,Text.Renamed:2"));
    CHECK(Rejects(context, byClass, "TextKindDerived:1,textkindderived:2",
                  TextDiagnosticCode::DuplicateKey));
    const TextSlot bySoft = SlotOf(context, setup.holder, "bySoft");
    CHECK(RoundTrip(context, bySoft, "11112222-3333-4444-5555-666677778888:1"));

    const TextSlot byName = SlotOf(context, setup.holder, "byName");
    CHECK(RoundTrip(context, byName, ""));
    CHECK(Rejects(context, byName, "a:{}", TextDiagnosticCode::InvalidValue));
}

// struct·Optional·원소 자리: struct 필드·Optional<struct>·struct 원소·Map 값 struct는
// `{}`(기본값)만, Optional<단일 값>은 내부 표기(present), 원소·Map 값 자리는 원소 형식 하나다. Set
// 원소 자리는 값이 없다(Unsupported).
TEST(TextValue_StructOptionalAndElementSlots) {
    SystemAllocator allocator;
    TextSetup setup(allocator);
    TextCodecContext context(setup.types, setup.options, setup.diagnostics, allocator);
    CHECK(RoundTrip(context, SlotOf(context, setup.holder, "origin"), "{}"));
    CHECK(Rejects(context, SlotOf(context, setup.holder, "origin"), "1",
                  TextDiagnosticCode::InvalidValue));
    CHECK(RoundTrip(context, SlotOf(context, setup.holder, "anchor"), "{}"));
    CHECK(RoundTrip(context, SlotOf(context, setup.holder, "origin.x"), "-4"));

    const TextSlot limit = SlotOf(context, setup.holder, "limit");
    CHECK(RoundTrip(context, limit, "5"));
    const FieldValue five = ReadSingle(context, limit, "5");
    CHECK(five.present);
    CHECK(five.kind == ValueKind::Int32);

    const TextSlot element = SlotOf(context, setup.holder, "numbers[1]");
    CHECK(element.part == TextSlotPart::Element);
    CHECK(RoundTrip(context, element, "7"));
    CHECK(Rejects(context, element, "1,2", TextDiagnosticCode::InvalidValue));
    CHECK(RoundTrip(context, SlotOf(context, setup.holder, "points[0]"), "{}"));
    CHECK(RoundTrip(context, SlotOf(context, setup.holder, "triple[2]"), "9"));
    const TextSlot entry = SlotOf(context, setup.holder, "scores[a]");
    CHECK(entry.part == TextSlotPart::MapValue);
    CHECK(RoundTrip(context, entry, "3"));
    CHECK(RoundTrip(context, SlotOf(context, setup.holder, "byName[Rock]"), "{}"));
    CHECK(RoundTrip(context, SlotOf(context, setup.holder, "byName[Rock].y"), "2"));
    const TextSlot member = SlotOf(context, setup.holder, "tags[Rock]");
    CHECK(member.part == TextSlotPart::SetElement);
    CHECK(Rejects(context, member, "1", TextDiagnosticCode::Unsupported));
}
