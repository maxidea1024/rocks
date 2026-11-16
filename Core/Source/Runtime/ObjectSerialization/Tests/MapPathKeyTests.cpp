#include "MapTestSupport.h"

// 리플렉션 B9f3 경로 key 단계의 표현 시험이다(Docs/Design/ReflectionExtensions.ko.md 1.5절, 17절
// B9f 경로 항목): PathKey wire 정규형(그래프 저장의 ValueArray 원소와 같은 byte)과 정규형 검사,
// 문자열 경로의 key 텍스트(종류별 해석·따옴표 문법·escape·금지 글자)와 FormatPropertyPath 왕복,
// PropertyPathBuffer 복사의 key 재배치, SamePropertyPath의 key 비교(wire byte, Name은 Core Name
// 규칙).
using namespace rocks;
using namespace rocks::test::map;

namespace {

// key 하나의 wire를 out에 만든다(실패하면 시험 중단).
Array<uint8> KeyWire(Allocator& allocator, const PathKeyValue& value) noexcept {
    Array<uint8> wire(allocator);
    if (!AppendPathKey(value, wire)) {
        platform::Fatal("test path key encode failed");
    }

    return wire;
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

PathKeyValue KeyOf(ValueKind kind) noexcept {
    PathKeyValue value;
    value.kind = kind;
    return value;
}

// text를 해석해 형식화한 결과가 expected인지다(해석·형식화가 실패하면 거짓).
bool RoundTrips(const TypeLease& lease, Allocator& allocator, StringView text,
                StringView expected) noexcept {
    PropertyPathBuffer path;
    if (!ParsePropertyPath(lease, text, path)) {
        return false;
    }

    String formatted(allocator);
    if (!FormatPropertyPath(lease, path.View(), formatted)) {
        return false;
    }

    PropertyPathBuffer again;
    return formatted.View() == expected && ParsePropertyPath(lease, formatted.View(), again) &&
           SamePropertyPath(path.View(), again.View());
}

// 해석 실패의 오류 코드·사유·byte 위치다.
struct ParseFailure {
    ErrorCode code = ErrorCode::None;
    PropertyPathFailure failure;
};

ParseFailure ParseError(const TypeLease& lease, StringView text) noexcept {
    PropertyPathBuffer path;
    ParseFailure result;
    auto parsed = ParsePropertyPath(lease, text, path, &result.failure);
    result.code = parsed ? ErrorCode::None : parsed.Error();
    return result;
}

} // namespace

// AppendPathKey의 byte는 그래프 저장의 원소 wire와 같다(Bool·U32·U64·I64·Guid·Bytes·String·Soft —
// ArchiveWriter로 쓴 byte와 비교). ReadPathKey는 왕복하고 정규형이 아닌 wire(길이·Bool 0·1·좁은
// 정수 범위·길이 prefix·UTF-8·Digest 길이·Soft null 규칙)와 키가 아닌 종류(실수)를
// InvalidArgument로 거부한다. 좁은 정수 범위 밖 값은 Overflow이고 out은 그대로다.
TEST(PathKey_WireMatchesElementBytesAndRejectsNonCanonical) {
    SystemAllocator allocator;
    const auto archive = [&](auto&& write) noexcept {
        ArchiveWriter out(allocator);
        CHECK(write(out));
        return out.TakeBytes();
    };

    PathKeyValue flag = KeyOf(ValueKind::Bool);
    flag.boolean = true;
    PathKeyValue small = KeyOf(ValueKind::UInt16);
    small.unsignedInteger = 65535;
    PathKeyValue wide = KeyOf(ValueKind::UInt64);
    wide.unsignedInteger = NumericLimits<uint64>::Max();
    PathKeyValue negative = KeyOf(ValueKind::Int8);
    negative.signedInteger = -128;
    PathKeyValue color = KeyOf(ValueKind::Enum);
    color.signedInteger = 4;
    PathKeyValue guid = KeyOf(ValueKind::AssetId);
    guid.guid = Guid{0x0102030405060708, 0x1112131415161718};
    PathKeyValue digest = KeyOf(ValueKind::Digest256);
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        digest.digest.bytes[index] = static_cast<uint8>(index * 7);
    }

    PathKeyValue text = KeyOf(ValueKind::Name);
    text.text = "Android_2";
    PathKeyValue soft = KeyOf(ValueKind::SoftObjectReference);
    soft.guid = Guid{3, 4};
    soft.text = "Body.Socket";
    struct Case {
        const PathKeyValue* value;
        Array<uint8> expected;
    };

    Case cases[] = {
        {&flag, archive([](ArchiveWriter& out) noexcept { return out.WriteBool(true).IsOk(); })},
        {&small, archive([](ArchiveWriter& out) noexcept { return out.WriteU32(65535).IsOk(); })},
        {&wide, archive([](ArchiveWriter& out) noexcept {
             return out.WriteU64(NumericLimits<uint64>::Max()).IsOk();
         })},
        {&negative, archive([](ArchiveWriter& out) noexcept { return out.WriteI64(-128).IsOk(); })},
        {&color, archive([](ArchiveWriter& out) noexcept { return out.WriteI64(4).IsOk(); })},
        {&guid,
         archive([&](ArchiveWriter& out) noexcept { return out.WriteGuid(guid.guid).IsOk(); })},
        {&digest, archive([&](ArchiveWriter& out) noexcept {
             return out.WriteBytes(Span<const uint8>(digest.digest.bytes, Sha256Digest::Bytes))
                 .IsOk();
         })},
        {&text,
         archive([](ArchiveWriter& out) noexcept { return out.WriteString("Android_2").IsOk(); })},
        {&soft, archive([&](ArchiveWriter& out) noexcept {
             return out.WriteGuid(soft.guid).IsOk() && out.WriteString("Body.Socket").IsOk();
         })}};
    for (const Case& item : cases) {
        const Array<uint8> wire = KeyWire(allocator, *item.value);
        CHECK(SameBytes(wire.View(), item.expected.View()));
        auto read = ReadPathKey(PathKey{item.value->kind, wire.View()});
        REQUIRE(read);
        const PathKeyValue& value = read.Value();
        CHECK(value.kind == item.value->kind && value.boolean == item.value->boolean &&
              value.unsignedInteger == item.value->unsignedInteger &&
              value.signedInteger == item.value->signedInteger && value.guid == item.value->guid &&
              value.digest == item.value->digest && value.text == item.value->text);
    }

    // 정규형이 아닌 wire.
    const uint8 two[] = {2};
    const uint8 bigByte[] = {0x00, 0x01, 0x00, 0x00};
    const uint8 shortText[] = {3, 0, 0, 0, 0, 0, 0, 0, 'a', 'b'};
    const uint8 badUtf8[] = {1, 0, 0, 0, 0, 0, 0, 0, 0xff};
    const uint8 shortDigest[8 + 31] = {31};
    const uint8 trailing[] = {1, 0, 0, 0, 0};
    const uint8 floatBytes[] = {0, 0, 0x80, 0x3f};
    const uint8 noneText[] = {4, 0, 0, 0, 0, 0, 0, 0, 'N', 'o', 'n', 'e'};
    const PathKey rejected[] = {{ValueKind::Name, Span<const uint8>(noneText, 12)},
                                {ValueKind::Bool, Span<const uint8>(two, 1)},
                                {ValueKind::UInt8, Span<const uint8>(bigByte, 4)},
                                {ValueKind::Utf8String, Span<const uint8>(shortText, 10)},
                                {ValueKind::Name, Span<const uint8>(badUtf8, 9)},
                                {ValueKind::Digest256, Span<const uint8>(shortDigest, 39)},
                                {ValueKind::UInt32, Span<const uint8>(trailing, 5)},
                                {ValueKind::Float32, Span<const uint8>(floatBytes, 4)}};
    for (const PathKey& key : rejected) {
        auto read = ReadPathKey(key);
        CHECK(!read && read.Error() == ErrorCode::InvalidArgument);
    }

    Array<uint8> out(allocator);
    PathKeyValue tooWide = KeyOf(ValueKind::Int8);
    tooWide.signedInteger = 128;
    CHECK(ErrorOf(AppendPathKey(tooWide, out)) == ErrorCode::Overflow && out.Size() == 0);
    PathKeyValue invalidSoft = KeyOf(ValueKind::SoftObjectReference);
    invalidSoft.text = "orphan";
    CHECK(ErrorOf(AppendPathKey(invalidSoft, out)) == ErrorCode::InvalidArgument &&
          out.Size() == 0);
    CHECK(ErrorOf(AppendPathKey(KeyOf(ValueKind::Float64), out)) == ErrorCode::InvalidArgument);

    // Name None 원문("NONE"·빈 문자열)은 저장 wire와 같은 빈 문자열 하나로 쓴다.
    PathKeyValue none = KeyOf(ValueKind::Name);
    none.text = "NONE";
    const Array<uint8> noneWire = KeyWire(allocator, none);
    const uint8 empty[8] = {};
    CHECK(SameBytes(noneWire.View(), Span<const uint8>(empty, 8)));
}

// 문자열 경로의 key: 키 종류로 해석하고(정규 10진 정수·bool on·off·enum 도구 이름 대소문자
// 무시·36자 소문자 Guid·AssetId·64자 소문자 Digest·String·Name — None은 `None`), 필요할 때만
// 큰따옴표로 감싼다. 따옴표 안 escape는 UE ParseQuotedString의 escape 가운데 다섯
// (`\\`·`\"`·`\n`·`\r`·`\t`)이고, 그래서 `\`로 끝나는 키(`C:\Program Files\` 꼴)도 쓸 수 있다.
// FormatPropertyPath는 정규 표기(불필요한 따옴표 없음, bool true·false, enum 실효 도구 이름, 16진
// 소문자, `\`는 늘 따옴표 안 `\\`)로 되돌리고 다시 해석한 경로는 SamePropertyPath로 같다. 값
// struct 항목 뒤의 `.`, struct 멤버 Map, 원소 struct 안 Map도 된다.
TEST(PropertyPath_KeyTextQuotingAndRoundTrip) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        auto lease = types.Acquire(TypeIdOf<MapHolder>());
        REQUIRE(lease);
        const TypeLease& holder = lease.Value();
        struct Case {
            const char* text;
            const char* canonical;
        };

        const Case cases[] = {
            {"ratios[metal]", "ratios[metal]"},
            {"ratios[\"a b\"]", "ratios[\"a b\"]"},
            {"ratios[\"say \\\"hi\\\"\"]", "ratios[\"say \\\"hi\\\"\"]"},
            {"ratios[\"plain\"]", "ratios[plain]"},
            {"ratios[]", "ratios[]"},
            {"ratios[\"C:\\\\x\"]", "ratios[\"C:\\\\x\"]"},
            {"ratios[\"C:\\\\Program Files\\\\\"]", "ratios[\"C:\\\\Program Files\\\\\"]"},
            {"ratios[\"line\\nbreak\\ttab\\rcr\"]", "ratios[\"line\\nbreak\\ttab\\rcr\"]"},
            {"ratios[\"k=v,w\"]", "ratios[\"k=v,w\"]"},
            {"ratios[\"a]b\"]", "ratios[\"a]b\"]"},
            {"RATIOS[x[y]", "ratios[x[y]"},
            {"counts[7]", "counts[7]"},
            {"counts[0]", "counts[0]"},
            {"counts[4294967295]", "counts[4294967295]"},
            {"small[-128]", "small[-128]"},
            {"small[0]", "small[0]"},
            {"colorNames[green]", "colorNames[Green]"},
            {"flags[ON]", "flags[true]"},
            {"flags[off]", "flags[false]"},
            {"guidColors[3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7b]",
             "guidColors[3f2a9c1e-7b4d-4e8f-9a01-2c3d4e5f6a7b]"},
            {"digestFlags[000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f]",
             "digestFlags[000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f]"},
            {"digests[00000000-0000-0000-0000-000000000001]",
             "digests[00000000-0000-0000-0000-000000000001]"},
            {"perPlatform[Android]", "perPlatform[Android]"},
            {"perPlatform[None]", "perPlatform[None]"},
            {"perPlatform[NONE]", "perPlatform[None]"},
            {"perPlatform[]", "perPlatform[None]"},
            {"tags[\"Hero, Main\"]", "tags[\"Hero, Main\"]"},
            {"points[hero].x", "points[hero].x"},
            {"bag.points[3].label", "bag.points[3].label"},
            {"bags[0].points[4].x", "bags[0].points[4].x"},
            {"colors[Blue]", "colors[Blue]"}};
        for (const Case& item : cases) {
            if (!RoundTrips(holder, allocator, item.text, item.canonical)) {
                CHECK(false);
            }
        }

        // 해석한 key 값: escape를 푼 String 텍스트, enum 값, Name None은 빈 wire.
        PropertyPathBuffer path;
        REQUIRE(ParsePropertyPath(holder, "ratios[\"say \\\"hi\\\"\\\\\"]", path));
        REQUIRE(path.count == 1 && path.KeyCount() == 1 &&
                path.steps[0].selector == PathSelector::Key && path.steps[0].index == 0);
        auto said = ReadPathKey(path.Key(0));
        REQUIRE(said);
        CHECK(said.Value().kind == ValueKind::Utf8String && said.Value().text == "say \"hi\"\\");
        REQUIRE(ParsePropertyPath(holder, "colorNames[BLUE]", path));
        auto blue = ReadPathKey(path.Key(0));
        REQUIRE(blue);
        CHECK(blue.Value().kind == ValueKind::Enum && blue.Value().signedInteger == 4);
        REQUIRE(ParsePropertyPath(holder, "perPlatform[none]", path));
        auto none = ReadPathKey(path.Key(0));
        REQUIRE(none);
        CHECK(none.Value().kind == ValueKind::Name && none.Value().text.ByteCount() == 0);

        // 해석 오류: 따옴표 없는 금지 글자(`\`·공백·`,`·`=`·`"`)·모르는 escape·따옴표 안 날 제어
        // 문자· 닫히지 않은 따옴표·`]` 없음은 Syntax, 키 종류의 값이 아니면(정규 10진이 아닌
        // 정수·대문자나 32자 Guid·AssetId·대문자 Digest 포함) InvalidKey(offset은 `[`), hook 없는
        // Soft·ClassRef 키는 Unsupported(InvalidKey), 중첩 `[`는 NotArray, 단일 값 항목·Set 원소
        // 뒤의 `.`는 NotStruct다.
        struct Failure {
            const char* text;
            ErrorCode code;
            PropertyPathError reason;
            usize offset;
        };

        const Failure failures[] = {
            {"ratios[a b]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 8},
            {"ratios[a,b]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 8},
            {"ratios[a=b]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 8},
            {"ratios[a\"b]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 8},
            {"ratios[C:\\x]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 9},
            {"ratios[\"a\\xb\"]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 9},
            {"ratios[\"a\tb\"]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 9},
            {"ratios[\"a\\", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 9},
            {"ratios[\"open]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 13},
            {"ratios[\"a\"b]", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 10},
            {"ratios[open", ErrorCode::InvalidArgument, PropertyPathError::Syntax, 11},
            {"counts[-1]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 6},
            {"counts[+7]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 6},
            {"counts[007]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 6},
            {"counts[x]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 6},
            {"counts[4294967296]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 6},
            {"small[128]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 5},
            {"small[-0]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 5},
            {"colorNames[Purple]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 10},
            {"flags[yes]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 5},
            {"guidColors[3F2A9C1E-7B4D-4E8F-9A01-2C3D4E5F6A7B]", ErrorCode::InvalidArgument,
             PropertyPathError::InvalidKey, 10},
            {"digests[00000000000000000000000000000001]", ErrorCode::InvalidArgument,
             PropertyPathError::InvalidKey, 7},
            {"digestFlags[000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F]",
             ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 11},
            {"digestFlags[00]", ErrorCode::InvalidArgument, PropertyPathError::InvalidKey, 11},
            {"softKeys[x]", ErrorCode::Unsupported, PropertyPathError::InvalidKey, 8},
            {"classNames[MapKeyDerived]", ErrorCode::Unsupported, PropertyPathError::InvalidKey,
             10},
            {"counts[1][2]", ErrorCode::TypeMismatch, PropertyPathError::NotArray, 9},
            {"counts[1].x", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 9},
            {"words[a].x", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 8},
            {"counts.x", ErrorCode::TypeMismatch, PropertyPathError::NotStruct, 6}};
        for (const Failure& item : failures) {
            const ParseFailure result = ParseError(holder, item.text);
            if (result.code != item.code || result.failure.reason != item.reason ||
                result.failure.offset != item.offset || result.failure.step != 0) {
                CHECK(false);
            }
        }

        // 복사한 버퍼의 key는 사본의 byte를 가리킨다(원본을 다시 써도 사본은 그대로다).
        PropertyPathBuffer copy;
        PropertyPathBuffer original;
        REQUIRE(ParsePropertyPath(holder, "bags[0].points[4].x", original));
        copy = original;
        REQUIRE(ParsePropertyPath(holder, "counts[9]", original));
        REQUIRE(copy.KeyCount() == 1);
        CHECK(copy.Key(0).wire.Data() != original.Key(0).wire.Data());
        auto four = ReadPathKey(copy.Key(0));
        CHECK(four && four.Value().unsignedInteger == 4);

        // 형식화: `\`로 끝나는 키는 따옴표 안 `\\`로 쓰고, 개행·CR·탭이 아닌 제어 문자·hook 없는
        // Soft 키·이름 없는 enum 값은 Unsupported, 필드와 다른 키 종류는 TypeMismatch다(실패하면
        // out은 그대로다).
        String out(allocator);
        KeyPath keyed(allocator);
        PathKeyValue trailing = KeyOf(ValueKind::Utf8String);
        trailing.text = "ab\\";
        MakeKeyPath(keyed, HolderField("ratios"), trailing);
        REQUIRE(FormatPropertyPath(holder, keyed.View(), out));
        CHECK(out.View() == "ratios[\"ab\\\\\"]");
        PathKeyValue control = KeyOf(ValueKind::Utf8String);
        control.text = StringView("a\x01", 2);
        MakeKeyPath(keyed, HolderField("ratios"), control);
        CHECK(ErrorOf(FormatPropertyPath(holder, keyed.View(), out)) == ErrorCode::Unsupported);
        PathKeyValue soft = KeyOf(ValueKind::SoftObjectReference);
        MakeKeyPath(keyed, HolderField("softKeys"), soft);
        CHECK(ErrorOf(FormatPropertyPath(holder, keyed.View(), out)) == ErrorCode::Unsupported);
        PathKeyValue mixed = KeyOf(ValueKind::Enum);
        mixed.signedInteger = 3;
        MakeKeyPath(keyed, HolderField("colorNames"), mixed);
        CHECK(ErrorOf(FormatPropertyPath(holder, keyed.View(), out)) == ErrorCode::Unsupported);
        MakeKeyPath(keyed, HolderField("counts"), trailing);
        CHECK(ErrorOf(FormatPropertyPath(holder, keyed.View(), out)) == ErrorCode::TypeMismatch);
        CHECK(out.View() == "ratios[\"ab\\\\\"]");
    }

    CHECK(types.Shutdown());
}

// SamePropertyPath: Key 단계는 index가 아니라 키 값으로 비교한다(keys 안 위치가 달라도 같은 값이면
// 같다). wire byte가 같아야 하고(String은 대소문자를 구별한다), Name 키는 Core Name 규칙(ASCII
// 대소문자 무시, 번호 접미는 정확히)이다. 종류·선택자가 다르면 다르다. 진단 경로(keys 없음)의 Key
// 단계와 keys 밖 위치는 정규형이 아니다.
TEST(PropertyPath_SamePropertyPathComparesKeys) {
    SystemAllocator allocator;
    const auto name = [&](StringView text) noexcept {
        PathKeyValue value = KeyOf(ValueKind::Name);
        value.text = text;
        return KeyWire(allocator, value);
    };

    const auto string = [&](StringView text) noexcept {
        PathKeyValue value = KeyOf(ValueKind::Utf8String);
        value.text = text;
        return KeyWire(allocator, value);
    };

    const Array<uint8> android = name("Android");
    const Array<uint8> upper = name("ANDROID");
    const Array<uint8> numbered = name("Android_1");
    const Array<uint8> padded = name("Android_01");
    const Array<uint8> text = string("Android");
    const Array<uint8> textUpper = string("ANDROID");
    const Guid field{0x53414d45, 1};
    const PathStep first[] = {{field, 0, PathSelector::Key}};
    const PathStep second[] = {{field, 1, PathSelector::Key}};
    const auto same = [&](Span<const PathStep> leftSteps, const PathKey(&leftKeys)[2],
                          Span<const PathStep> rightSteps, const PathKey(&rightKeys)[2]) noexcept {
        return SamePropertyPath(PropertyPath{leftSteps, leftKeys},
                                PropertyPath{rightSteps, rightKeys});
    };

    const PathKey nameKeys[2] = {{ValueKind::Name, android.View()},
                                 {ValueKind::Name, upper.View()}};
    const PathKey numberKeys[2] = {{ValueKind::Name, numbered.View()},
                                   {ValueKind::Name, padded.View()}};
    const PathKey textKeys[2] = {{ValueKind::Utf8String, text.View()},
                                 {ValueKind::Utf8String, textUpper.View()}};
    const PathKey mixedKeys[2] = {{ValueKind::Utf8String, text.View()},
                                  {ValueKind::Name, android.View()}};
    CHECK(same(first, nameKeys, second, nameKeys));
    CHECK(!same(first, numberKeys, second, numberKeys));
    CHECK(same(first, numberKeys, first, numberKeys));
    CHECK(!same(first, textKeys, second, textKeys));
    CHECK(same(first, textKeys, first, mixedKeys));
    CHECK(!same(first, mixedKeys, second, mixedKeys));

    const PathStep index[] = {{field, 0, PathSelector::Index}};
    CHECK(!same(first, nameKeys, index, nameKeys));
    CHECK(SamePropertyPath(PropertyPath{index}, PropertyPath{index}));
    CHECK(!IsCanonicalPathStep(first[0]));
    CHECK(IsCanonicalPathStep(second[0], 2) && !IsCanonicalPathStep(second[0], 1));
    CHECK(!SamePropertyPath(PropertyPath{second, Span<const PathKey>(nameKeys, 1)},
                            PropertyPath{second, Span<const PathKey>(nameKeys, 1)}));
    const PathStep none[] = {{field, 3, PathSelector::None}};
    CHECK(!IsCanonicalPathStep(none[0]));
}
