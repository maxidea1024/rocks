#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

// RFC 8259 JSON 읽기·쓰기 시험이다(CoreDataFoundations.ko.md 3.5). JSONTestSuite(데이터 전용
// ThirdParty)의 y_·n_ 사례 전체와 i_ 사례의 결정 표, 왕복·고정점, 중복 키, 한도, 오류 위치, 쓰기
// 규칙을 본다.
namespace {

constexpr const char* SuiteRoot = "Core/Source/ThirdParty/JsonTestSuite/";
constexpr const char* SuiteDirectory =
    "Core/Source/ThirdParty/JsonTestSuite/JSONTestSuite-1ef36fa01286/";

bool ReadFile(const char* path, Array<char>& out) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    bool ok = static_cast<bool>(file.Size(size)) &&
              static_cast<bool>(out.TryResize(static_cast<usize>(size)));
    if (ok && size != 0) {
        ok = static_cast<bool>(file.ReadExact(out.Data(), static_cast<usize>(size)).status);
    }

    const bool closed = static_cast<bool>(file.Close());
    return ok && closed;
}

JsonError ParseText(JsonDocument& document, const char* text,
                    const JsonReadOptions& options = {}) noexcept {
    return document.Parse(StringView(text, strlen(text)), options);
}

// 엄격 비교: 종류와 값 비트까지 같아야 한다(JsonEquals는 1과 1.0, 0.0과 −0.0을 같게 보므로 왕복에서
// 종류·부호를 잃어도 잡지 못한다). 시험 문서는 얕아서 재귀해도 된다.
bool StrictSame(JsonValue left, JsonValue right) noexcept {
    if (left.Type() != right.Type()) {
        return false;
    }

    switch (left.Type()) {
    case JsonType::Null:
        return true;
    case JsonType::Bool:
        return left.GetBool() == right.GetBool();
    case JsonType::Integer:
        return left.TryGetInt64().Value() == right.TryGetInt64().Value();
    case JsonType::Unsigned:
        return left.TryGetUInt64().Value() == right.TryGetUInt64().Value();
    case JsonType::Double:
        return platform::BitCast<uint64>(left.TryGetDouble().Value()) ==
               platform::BitCast<uint64>(right.TryGetDouble().Value());
    case JsonType::String:
        return left.GetString() == right.GetString();
    case JsonType::Array:
    case JsonType::Object:
        if (left.Size() != right.Size()) {
            return false;
        }

        for (usize index = 0; index < left.Size(); ++index) {
            const bool object = left.Type() == JsonType::Object;
            if (object && left.KeyAt(index) != right.KeyAt(index)) {
                return false;
            }

            if (!StrictSame(object ? left.ValueAt(index) : left.At(index),
                            object ? right.ValueAt(index) : right.At(index))) {
                return false;
            }
        }

        return true;
    }

    return false;
}

bool Writes(JsonValue value, JsonNumberMode mode, StringBuilderBase& out) noexcept {
    out.Clear();
    JsonWriter writer(out, mode);
    return static_cast<bool>(writer.Value(value)) && static_cast<bool>(writer.Finish());
}

// i_ 사례(구현 정의)의 이 구현 결정이다. true = 받아들인다.
struct Decision {
    const char* name;
    bool accept;
};

const Decision ImplementationDefined[] = {
    {"i_number_double_huge_neg_exp.json", true}, // 0으로 underflow
    {"i_number_huge_exp.json", false},           // double 범위 밖 → NumberOutOfRange
    {"i_number_neg_int_huge_exp.json", false},
    {"i_number_pos_double_huge_exp.json", false},
    {"i_number_real_neg_overflow.json", false},
    {"i_number_real_pos_overflow.json", false},
    {"i_number_real_underflow.json", true},
    {"i_number_too_big_neg_int.json", true}, // int64·uint64를 넘는 정수 → double
    {"i_number_too_big_pos_int.json", true},
    {"i_number_very_big_negative_int.json", true},
    {"i_object_key_lone_2nd_surrogate.json", false}, // 짝 없는 surrogate
    {"i_string_1st_surrogate_but_2nd_missing.json", false},
    {"i_string_1st_valid_surrogate_2nd_invalid.json", false},
    {"i_string_UTF-16LE_with_BOM.json", false}, // UTF-8이 아니다
    {"i_string_UTF-8_invalid_sequence.json", false},
    {"i_string_UTF8_surrogate_U+D800.json", false},
    {"i_string_incomplete_surrogate_and_escape_valid.json", false},
    {"i_string_incomplete_surrogate_pair.json", false},
    {"i_string_incomplete_surrogates_escape_valid.json", false},
    {"i_string_invalid_lonely_surrogate.json", false},
    {"i_string_invalid_surrogate.json", false},
    {"i_string_invalid_utf-8.json", false},
    {"i_string_inverted_surrogates_U+1D11E.json", false},
    {"i_string_iso_latin_1.json", false},
    {"i_string_lone_second_surrogate.json", false},
    {"i_string_lone_utf8_continuation_byte.json", false},
    {"i_string_not_in_unicode_range.json", false},
    {"i_string_overlong_sequence_2_bytes.json", false},
    {"i_string_overlong_sequence_6_bytes.json", false},
    {"i_string_overlong_sequence_6_bytes_null.json", false},
    {"i_string_truncated-utf-8.json", false},
    {"i_string_utf16BE_no_BOM.json", false},
    {"i_string_utf16LE_no_BOM.json", false},
    {"i_structure_500_nested_arrays.json", false},     // 기본 깊이 한도 128
    {"i_structure_UTF-8_BOM_empty_object.json", true}, // BOM 하나는 경고와 함께 받는다
};

// y_ 사례 중 의도적으로 거부하는 것(중복 키 거부 — 조용히 덮어쓰지 않는다).
const char* const RejectedValid[] = {"y_object_duplicated_key.json",
                                     "y_object_duplicated_key_and_value.json"};

} // namespace

TEST(Json_TestSuiteCasesAndRoundTrip) {
    SystemAllocator allocator;
    // 사례 목록은 데이터 라이브러리 manifest의 files에서 읽는다(이 파서로).
    Array<char> manifestText(allocator);
    char manifestPath[256];
    snprintf(manifestPath, sizeof(manifestPath), "%sThirdParty.json", SuiteRoot);
    REQUIRE(ReadFile(manifestPath, manifestText));
    JsonDocument manifest(allocator);
    REQUIRE(manifest.Parse(StringView(manifestText.Data(), manifestText.Size())).Succeeded());
    const JsonValue files = manifest.Root().Find("files");
    REQUIRE(files.IsObject());

    usize accepted = 0;
    usize rejected = 0;
    usize decided = 0;
    usize cases = 0;
    Array<char> content(allocator);
    JsonDocument document(allocator);
    JsonDocument reparsed(allocator);
    StringBuilder<> first(allocator);
    StringBuilder<> second(allocator);
    for (usize index = 0; index < files.Size(); ++index) {
        const StringView key = files.KeyAt(index);
        const StringView prefix("test_parsing/");
        if (!key.StartsWith(prefix)) {
            continue;
        }

        char name[160];
        snprintf(name, sizeof(name), "%.*s", static_cast<int>(key.ByteCount() - prefix.ByteCount()),
                 key.Data() + prefix.ByteCount());
        char path[320];
        snprintf(path, sizeof(path), "%s%.*s", SuiteDirectory, static_cast<int>(key.ByteCount()),
                 key.Data());
        REQUIRE(ReadFile(path, content));
        ++cases;
        bool expectAccept = name[0] == 'y';
        if (name[0] == 'i') {
            bool found = false;
            for (const Decision& decision : ImplementationDefined) {
                if (strcmp(decision.name, name) == 0) {
                    expectAccept = decision.accept;
                    found = true;
                    ++decided;
                }
            }

            if (!found) {
                printf("JSONTestSuite: no decision for %s\n", name);
                CHECK(found);
            }
        }

        for (const char* rejectedName : RejectedValid) {
            if (strcmp(rejectedName, name) == 0) {
                expectAccept = false;
            }
        }

        const JsonError error = document.Parse(StringView(content.Data(), content.Size()));
        if (error.Succeeded() != expectAccept) {
            printf("JSONTestSuite %s: %s (%s at %u:%u)\n", name,
                   error.Succeeded() ? "accepted" : "rejected", JsonErrorMessage(error.code),
                   error.position.line, error.position.column);
            CHECK(error.Succeeded() == expectAccept);
            continue;
        }

        if (!error.Succeeded()) {
            ++rejected;
            continue;
        }

        ++accepted;
        // 왕복·고정점: Canonical로 쓰고 다시 읽으면 같은 DOM이고, 다시 쓰면 같은 byte다.
        REQUIRE(Writes(document.Root(), JsonNumberMode::Canonical, first));
        const JsonError again = reparsed.Parse(first.View());
        if (!again.Succeeded() || !JsonEquals(document.Root(), reparsed.Root()) ||
            !StrictSame(document.Root(), reparsed.Root())) {
            printf("JSONTestSuite %s: canonical output does not round-trip\n", name);
            CHECK(false);
            continue;
        }

        REQUIRE(Writes(reparsed.Root(), JsonNumberMode::Canonical, second));
        CHECK(first.View() == second.View());
        // Preserve도 다시 읽으면 같은 DOM이다(숫자 원문 유지).
        REQUIRE(Writes(document.Root(), JsonNumberMode::Preserve, second));
        CHECK(reparsed.Parse(second.View()).Succeeded() &&
              StrictSame(document.Root(), reparsed.Root()));
    }

    printf("JSONTestSuite: %llu cases, %llu accepted, %llu rejected, %llu implementation-defined\n",
           static_cast<unsigned long long>(cases), static_cast<unsigned long long>(accepted),
           static_cast<unsigned long long>(rejected), static_cast<unsigned long long>(decided));
    CHECK(cases == 318);
    CHECK(decided == sizeof(ImplementationDefined) / sizeof(ImplementationDefined[0]));
}

TEST(Json_ValuesNumbersAndLookup) {
    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(ParseText(document,
                      "{\"i\": -12, \"u\": 18446744073709551615, \"big\": 18446744073709551616,"
                      " \"d\": 1.50, \"z\": -0, \"s\": \"a\\u00e9\\ud83d\\ude00\", \"t\": true,"
                      " \"n\": null, \"a\": [1, [], {}]}")
                .Succeeded());
    const JsonValue root = document.Root();
    CHECK(root.IsObject() && root.Size() == 9 && root.KeyAt(0) == StringView("i"));
    CHECK(root.Find("i").Type() == JsonType::Integer &&
          root.Find("i").TryGetInt64().Value() == -12);
    CHECK(root.Find("u").Type() == JsonType::Unsigned &&
          root.Find("u").TryGetUInt64().Value() == ~0ull);
    CHECK(!root.Find("u").TryGetInt64());
    CHECK(root.Find("big").Type() == JsonType::Double &&
          root.Find("big").TryGetDouble().Value() == 18446744073709551616.0);
    CHECK(root.Find("d").Type() == JsonType::Double &&
          root.Find("d").RawNumber() == StringView("1.50"));
    CHECK(!root.Find("d").TryGetInt64() && root.Find("d").TryGetDouble().Value() == 1.5);
    const JsonValue zero = root.Find("z");
    CHECK(zero.Type() == JsonType::Double &&
          platform::BitCast<uint64>(zero.TryGetDouble().Value()) == 0x8000000000000000ull);
    CHECK(root.Find("s").GetString() == StringView("a\xC3\xA9\xF0\x9F\x98\x80"));
    CHECK(root.Find("t").GetBool() && root.Find("n").IsNull());
    CHECK(root.Find("a").Size() == 3 && root.Find("a").At(1).IsArray() &&
          root.Find("a").At(2).Size() == 0);
    CHECK(!root.Find("missing").IsValid() && !root.Find("missing").IsString());
    CHECK(!root.Find("a").Find("x").IsValid());
    // 최상위 스칼라, 범위 밖 double, 원문 보존.
    REQUIRE(ParseText(document, " 42 ").Succeeded());
    CHECK(document.Root().TryGetInt64().Value() == 42);
    const JsonError overflow = ParseText(document, "[1e400]");
    CHECK(overflow.code == JsonErrorCode::NumberOutOfRange && overflow.position.column == 2);
    CHECK(!document.Root().IsValid());
}

TEST(Json_DuplicateKeysAndEscapedKeys) {
    SystemAllocator allocator;
    JsonDocument document(allocator);
    // 이스케이프를 푼 뒤 byte가 같으면 같은 키다.
    const JsonError duplicate = ParseText(document, "{\"a\": 1,\n \"\\u0061\": 2}");
    CHECK(duplicate.code == JsonErrorCode::DuplicateKey && duplicate.position.line == 2 &&
          duplicate.position.column == 2);
    // 정규화하지 않는다: NFC é(U+00E9)와 NFD e + U+0301은 다른 키.
    REQUIRE(ParseText(document, "{\"\xC3\xA9\": 1, \"e\xCC\x81\": 2}").Succeeded());
    CHECK(document.Root().Size() == 2 &&
          document.Root().Find("e\xCC\x81").TryGetInt64().Value() == 2);
    // 다른 객체의 같은 키는 괜찮다.
    REQUIRE(ParseText(document, "[{\"k\": 1}, {\"k\": 2}, {\"k\": {\"k\": 3}}]").Succeeded());
    CHECK(document.Root().At(2).Find("k").Find("k").TryGetInt64().Value() == 3);
}

TEST(Json_ErrorPositionsAndCodes) {
    SystemAllocator allocator;
    JsonDocument document(allocator);
    struct Case {
        const char* text;
        JsonErrorCode code;
        uint32 line;
        uint32 column;
    };
    const Case cases[] = {
        {"{\r\n  \"a\": tru }", JsonErrorCode::InvalidLiteral, 2, 8}, // CRLF는 줄바꿈 1개
        {"[1,\r2,\r\t\x01]", JsonErrorCode::UnexpectedCharacter, 3,
         2}, // CR 단독도 줄바꿈, tab은 1칸
        {"[\"\xED\x95\x9C\xEA\xB8\x80\", x]", JsonErrorCode::UnexpectedCharacter, 1,
         8}, // column은 code point
        {"[1,]", JsonErrorCode::UnexpectedCharacter, 1, 4},
        {"{\"a\" 1}", JsonErrorCode::UnexpectedCharacter, 1, 6},
        {"[01]", JsonErrorCode::InvalidNumber, 1, 3},
        {"[1.]", JsonErrorCode::InvalidNumber, 1, 4},
        {"[\"\\x\"]", JsonErrorCode::InvalidEscape, 1, 3},
        {"[\"\\ud800\"]", JsonErrorCode::UnpairedSurrogate, 1, 3},
        {"[\"\\ud800\\u12G4\"]", JsonErrorCode::InvalidEscape, 1, 9}, // 잘못된 쪽은 두 번째 \u
        {"[\"\\ud800\\u", JsonErrorCode::UnexpectedEnd, 1, 11},       // surrogate 쌍 도중 끝
        {"[\"\\ud800", JsonErrorCode::UnexpectedEnd, 1, 9},
        {"[\"a\x01\"]", JsonErrorCode::ControlCharacter, 1, 4},
        {"[\"\xC0\xAF\"]", JsonErrorCode::InvalidUtf8, 1, 3},
        {"[1] 2", JsonErrorCode::TrailingContent, 1, 5},
        {"[1, 2", JsonErrorCode::UnexpectedEnd, 1, 6},
        {"\"abc", JsonErrorCode::UnexpectedEnd, 1, 5},
        {"", JsonErrorCode::UnexpectedEnd, 1, 1},
        {"/* c */ 1", JsonErrorCode::UnexpectedCharacter, 1, 1},
        {"['a']", JsonErrorCode::UnexpectedCharacter, 1, 2},
        {"[NaN]", JsonErrorCode::UnexpectedCharacter, 1, 2},
    };
    for (const Case& item : cases) {
        const JsonError error = ParseText(document, item.text);
        if (error.code != item.code || error.position.line != item.line ||
            error.position.column != item.column) {
            printf("json error case: %s at %u:%u (offset %llu)\n", JsonErrorMessage(error.code),
                   error.position.line, error.position.column,
                   static_cast<unsigned long long>(error.position.byteOffset));
        }

        CHECK(error.code == item.code && error.position.line == item.line &&
              error.position.column == item.column);
    }

    // BOM 하나는 경고와 함께 받고 위치 계산에서 빠진다.
    REQUIRE(ParseText(document, "\xEF\xBB\xBF{}").Succeeded());
    CHECK(document.Warnings().Size() == 1 &&
          document.Warnings()[0].code == JsonWarningCode::BomIgnored);
    const JsonError afterBom = ParseText(document, "\xEF\xBB\xBF[x]");
    CHECK(afterBom.code == JsonErrorCode::UnexpectedCharacter && afterBom.position.column == 2 &&
          afterBom.position.byteOffset == 4);
    CHECK(ParseText(document, "\xEF\xBB\xBF\xEF\xBB\xBF{}").code ==
          JsonErrorCode::UnexpectedCharacter);
}

TEST(Json_ReadLimits) {
    SystemAllocator allocator;
    JsonDocument document(allocator);
    StringBuilder<> text(allocator);
    // 깊이: 128까지 받고 129는 거부(반복형 파서라 10만 단계도 stack을 쓰지 않는다).
    REQUIRE(text.TryAppendRepeated('[', 128) && text.TryAppendRepeated(']', 128));
    CHECK(document.Parse(text.View()).Succeeded());
    text.Clear();
    REQUIRE(text.TryAppendRepeated('[', 129) && text.TryAppendRepeated(']', 129));
    CHECK(document.Parse(text.View()).code == JsonErrorCode::DepthLimitExceeded);
    text.Clear();
    REQUIRE(text.TryAppendRepeated('[', 100000));
    const JsonError deep = document.Parse(text.View());
    CHECK(deep.code == JsonErrorCode::DepthLimitExceeded && deep.position.column == 129);
    JsonReadOptions options;
    options.maxDepth = 1000;
    text.Clear();
    REQUIRE(text.TryAppendRepeated('[', 500) && text.TryAppendRepeated(']', 500));
    CHECK(document.Parse(text.View(), options).Succeeded());
    // 문자열·노드·입력 한도.
    options = JsonReadOptions{};
    options.maxStringBytes = 4;
    CHECK(ParseText(document, "[\"abcd\"]", options).Succeeded());
    CHECK(ParseText(document, "[\"abcde\"]", options).code == JsonErrorCode::StringTooLong);
    CHECK(ParseText(document, "[\"\\u00e9\\u00e9\\u00e9\"]", options).code ==
          JsonErrorCode::StringTooLong);
    options = JsonReadOptions{};
    options.maxNodes = 3;
    CHECK(ParseText(document, "[1, 2]", options).Succeeded());
    CHECK(ParseText(document, "[1, 2, 3]", options).code == JsonErrorCode::TooManyNodes);
    // 한도를 넘긴 값의 시작에서 보고한다.
    options.maxNodes = 2;
    const JsonError tooMany = ParseText(document, "[\"abcdef\", \"ghijkl\"]", options);
    CHECK(tooMany.code == JsonErrorCode::TooManyNodes && tooMany.position.column == 12);
    options = JsonReadOptions{};
    options.maxInputBytes = 4;
    CHECK(ParseText(document, "[11]", options).Succeeded());
    CHECK(ParseText(document, "[111]", options).code == JsonErrorCode::InputTooLarge);
}

TEST(Json_WriterCanonicalBytes) {
    SystemAllocator allocator;
    StringBuilder<> text(allocator);
    JsonWriter writer(text);
    REQUIRE(writer.BeginObject());
    REQUIRE(writer.Key("name") && writer.String("a\"b\\c\n\t\x01\x7F\xC3\xA9"));
    REQUIRE(writer.Key("list") && writer.BeginArray());
    REQUIRE(writer.Int64(-1) && writer.UInt64(18446744073709551615ull) && writer.Double(1.0) &&
            writer.Double(-0.0));
    REQUIRE(writer.Double(0.1) && writer.Double(1e20) && writer.Null() && writer.Bool(false));
    REQUIRE(writer.BeginArray() && writer.EndArray() && writer.BeginObject() && writer.EndObject());
    REQUIRE(writer.EndArray());
    REQUIRE(writer.Key("\x08") && writer.Double(2.5));
    REQUIRE(writer.EndObject() && writer.Finish());
    const char* expected = "{\n"
                           "  \"name\": \"a\\\"b\\\\c\\n\\t\\u0001\x7F\xC3\xA9\",\n"
                           "  \"list\": [\n"
                           "    -1,\n"
                           "    18446744073709551615,\n"
                           "    1.0,\n"
                           "    -0.0,\n"
                           "    0.1,\n"
                           "    1e+20,\n"
                           "    null,\n"
                           "    false,\n"
                           "    [],\n"
                           "    {}\n"
                           "  ],\n"
                           "  \"\\b\": 2.5\n"
                           "}\n";
    if (text.View() != StringView(expected)) {
        printf("writer output:\n%.*s", static_cast<int>(text.ByteCount()), text.View().Data());
    }

    CHECK(text.View() == StringView(expected));
    // 다시 읽으면 숫자 종류가 유지된다(1.0·-0.0은 double).
    JsonDocument document(allocator);
    REQUIRE(document.Parse(text.View()).Succeeded());
    CHECK(document.Root().Find("list").At(2).Type() == JsonType::Double);
    CHECK(document.Root().Find("list").At(0).Type() == JsonType::Integer);

    // Preserve는 원문 숫자 토큰을 쓴다.
    REQUIRE(document.Parse(StringView("[1.50, 1E2, -0]")).Succeeded());
    text.Clear();
    JsonWriter preserve(text, JsonNumberMode::Preserve);
    REQUIRE(preserve.Value(document.Root()) && preserve.Finish());
    CHECK(text.View() == StringView("[\n  1.50,\n  1E2,\n  -0\n]\n"));
    text.Clear();
    JsonWriter canonical(text);
    REQUIRE(canonical.Value(document.Root()) && canonical.Finish());
    CHECK(text.View() == StringView("[\n  1.5,\n  100.0,\n  -0.0\n]\n"));
}

TEST(Json_WriterRejectsInvalidUse) {
    SystemAllocator allocator;
    StringBuilder<> text(allocator);
    const double nan = platform::BitCast<double>(uint64{0x7FF8000000000000});
    const double infinity = platform::BitCast<double>(uint64{0x7FF0000000000000});
    {
        JsonWriter writer(text);
        CHECK(!writer.Double(nan));
        CHECK(!writer.Null()); // 한 번 실패하면 이후 호출도 실패한다
    }
    {
        JsonWriter writer(text);
        CHECK(!writer.Double(infinity));
    }
    {
        JsonWriter writer(text);
        CHECK(!writer.String(StringView("\xC0\xAF", 2)));
    }
    {
        JsonWriter writer(text);
        REQUIRE(writer.BeginObject());
        CHECK(!writer.Int64(1)); // 키 없이 값
    }
    {
        JsonWriter writer(text);
        REQUIRE(writer.BeginArray());
        CHECK(!writer.Key("k") && !writer.Finish());
    }
    {
        JsonWriter writer(text);
        REQUIRE(writer.Int64(1));
        CHECK(!writer.Int64(2)); // 최상위 값은 하나
    }
    {
        JsonWriter writer(text);
        CHECK(!writer.Finish()); // 값 없음
    }
    {
        JsonWriter writer(text);
        bool ok = true;
        for (int depth = 0; depth < 128 && ok; ++depth) {
            ok = depth % 2 == 0 ? static_cast<bool>(writer.BeginArray())
                                : (writer.BeginObject() && writer.Key("k"));
        }

        CHECK(ok);
        auto deeper = writer.BeginArray(); // 기본 깊이 한도는 reader와 같은 128
        CHECK(!deeper && deeper.Error() == ErrorCode::CapacityExceeded);
    }

    // 상태 기계: Key 뒤 EndObject, Key 두 번, 객체 안 EndArray, 배열 안 EndObject·Key, Finish 뒤
    // 값, 무효 값, 같은 객체의 중복 키(자기 reader가 거부한다). 다른 객체의 같은 키는 괜찮다.
    for (int scenario = 0; scenario < 8; ++scenario) {
        text.Clear();
        JsonWriter writer(text);
        bool ok = true;
        switch (scenario) {
        case 0:
            ok = writer.BeginObject() && writer.Key("a") && !writer.EndObject();
            break;
        case 1:
            ok = writer.BeginObject() && writer.Key("a") && !writer.Key("b");
            break;
        case 2:
            ok = writer.BeginObject() && !writer.EndArray();
            break;
        case 3:
            ok = writer.BeginArray() && !writer.EndObject();
            break;
        case 4:
            ok = writer.Null() && writer.Finish() && !writer.Null() && !writer.Finish();
            break;
        case 5:
            ok = !writer.Value(JsonValue());
            break;
        case 6: {
            ok = writer.BeginObject() && writer.Key("a") && writer.Int64(1);
            auto duplicate = writer.Key("a");
            ok = ok && !duplicate && duplicate.Error() == ErrorCode::InvalidArgument;
            break;
        }
        default:
            ok = writer.BeginArray() && writer.BeginObject() && writer.Key("a") &&
                 writer.Int64(1) && writer.EndObject() && writer.BeginObject() && writer.Key("a") &&
                 writer.Int64(2) && writer.EndObject() && writer.EndArray() && writer.Finish();
            break;
        }

        if (!ok) {
            printf("json writer misuse scenario %d\n", scenario);
        }

        CHECK(ok);
    }
}

TEST(Json_WriterEscapesNumbersAndAliasing) {
    SystemAllocator allocator;
    StringBuilder<> text(allocator);
    {
        // 제어 문자: \b\f\n\r\t와 소문자 \u00xx(NUL·0x1F 포함). '/'·U+007F·U+2028·astral 문자는
        // 그대로.
        JsonWriter writer(text);
        const char raw[] = "\x00\x08\x0C\x0A\x0D\x09\x1F/\x7F\xE2\x80\xA8\xF0\x9F\x98\x80";
        REQUIRE(writer.String(StringView(raw, sizeof(raw) - 1)) && writer.Finish());
        CHECK(text.View() ==
              StringView("\"\\u0000\\b\\f\\n\\r\\t\\u001f/\x7F\xE2\x80\xA8\xF0\x9F\x98\x80\"\n"));
    }

    // 최단 표기 배치: 1e16 경계, 1e-4 경계, 비정규 수, 2^53 + 1 정수.
    struct Case {
        double value;
        const char* expected;
    };
    const Case doubles[] = {{1e16, "1e+16"},
                            {1e15, "1000000000000000.0"},
                            {9999999999999998.0, "9999999999999998.0"},
                            {1e-4, "0.0001"},
                            {1e-5, "1e-05"},
                            {platform::BitCast<double>(uint64{1}), "5e-324"},
                            {-2.5, "-2.5"}};
    for (const Case& item : doubles) {
        text.Clear();
        JsonWriter writer(text);
        REQUIRE(writer.Double(item.value) && writer.Finish());
        StringBuilder<> expected(allocator);
        REQUIRE(expected.TryAppend(StringView(item.expected)) && expected.TryAppendAscii('\n'));
        if (text.View() != expected.View()) {
            printf("json double: got %.*s", static_cast<int>(text.ByteCount()), text.View().Data());
        }

        CHECK(text.View() == expected.View());
    }

    text.Clear();
    {
        JsonWriter writer(text);
        REQUIRE(writer.Int64(9007199254740993ll) && writer.Finish());
        CHECK(text.View() == StringView("9007199254740993\n"));
    }

    // 입력 문자열이 out의 View()여도 된다(쓰는 동안 주소가 바뀌어도 복사본을 쓴다).
    text.Clear();
    REQUIRE(text.TryAppendRepeated('x', 400));
    {
        JsonWriter writer(text);
        REQUIRE(writer.BeginArray());
        // "[" 앞의 400 byte(지금 buffer를 가리키는 view — 쓰는 동안 재할당돼도 된다).
        REQUIRE(writer.String(StringView(text.View().Data(), 400)) && writer.EndArray() &&
                writer.Finish());
    }

    JsonDocument document(allocator);
    REQUIRE(
        document.Parse(StringView(text.View().Data() + 400, text.ByteCount() - 400)).Succeeded());
    CHECK(document.Root().At(0).GetString().ByteCount() == 400);
}

TEST(Json_EqualsHandlesAndDepth) {
    SystemAllocator allocator;
    JsonDocument left(allocator);
    JsonDocument right(allocator);
    // 같지 않은 쌍: 키 이름·순서, 문자열, 숫자 값, 정수와 정수가 아닌 double, 길이, 종류.
    struct Pair {
        const char* a;
        const char* b;
        bool same;
    };
    const Pair pairs[] = {
        {"{\"a\": 1, \"b\": 2}", "{\"a\": 1, \"b\": 2}", true},
        {"{\"a\": 1, \"b\": 2}", "{\"b\": 2, \"a\": 1}", false},
        {"{\"a\": 1}", "{\"c\": 1}", false},
        {"[\"x\"]", "[\"y\"]", false},
        {"[1]", "[1.0]", true},
        {"[0.0]", "[-0.0]", true},
        {"[9007199254740993]", "[9007199254740992.0]", false},
        {"[9223372036854775808]", "[9223372036854775808.0]", true},
        {"[18446744073709551615]", "[18446744073709551616.0]", false},
        {"[-9223372036854775808]", "[-9223372036854775808.0]", true},
        {"[1.5]", "[1]", false},
        {"[1, 2]", "[1]", false},
        {"[[1], 2]", "[[1, 2]]", false},
        {"[null]", "[false]", false},
        {"[true]", "[false]", false},
    };
    for (const Pair& pair : pairs) {
        REQUIRE(ParseText(left, pair.a).Succeeded() && ParseText(right, pair.b).Succeeded());
        if (JsonEquals(left.Root(), right.Root()) != pair.same) {
            printf("JsonEquals(%s, %s) != %d\n", pair.a, pair.b, pair.same ? 1 : 0);
        }

        CHECK(JsonEquals(left.Root(), right.Root()) == pair.same);
        CHECK(JsonEquals(right.Root(), left.Root()) == pair.same);
    }

    // 정수 연산만 쓰므로 DAZ에서도 비정규 수를 0과 구별한다.
    REQUIRE(ParseText(left, "[0, 5e-324, 1e-310]").Succeeded());
    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original | platform::detail::DenormalsAreZeroBit() |
                                                platform::detail::FlushToZeroBit());
    const bool zeroVsSubnormal = JsonEquals(left.Root().At(0), left.Root().At(1));
    const bool subnormals = JsonEquals(left.Root().At(1), left.Root().At(2));
    platform::detail::WriteFloatingPointControl(original);
    CHECK(!zeroVsSubnormal && !subnormals);

    // 깊이와 무관하게 재귀하지 않는다: 10만 단계 문서 비교와 같은 maxDepth의 writer.
    StringBuilder<> text(allocator);
    REQUIRE(text.TryAppendRepeated('[', 100000) && text.TryAppendRepeated(']', 100000));
    JsonReadOptions options;
    options.maxDepth = 100000;
    REQUIRE(left.Parse(text.View(), options).Succeeded() &&
            right.Parse(text.View(), options).Succeeded());
    CHECK(JsonEquals(left.Root(), right.Root()));
    StringBuilder<> written(allocator);
    JsonWriter writer(written, JsonNumberMode::Canonical, 100000);
    CHECK(writer.Value(left.Root()) && writer.Finish());

    // 다시 Parse하거나 Clear하면 이전 핸들은 무효다(새 내용을 조용히 가리키지 않는다).
    REQUIRE(ParseText(left, "{\"x\": \"old\"}").Succeeded());
    const JsonValue stale = left.Root().Find("x");
    CHECK(stale.IsString());
    REQUIRE(ParseText(left, "{\"x\": \"new\"}").Succeeded());
    CHECK(!stale.IsValid() && !stale.IsString() && stale.Size() == 0);
    const JsonValue root = left.Root();
    left.Clear();
    CHECK(!root.IsValid() && !left.Root().IsValid());

    // 자기 문자열을 입력으로 넘겨도 오류 위치가 복사본과 같다: 루트 문자열 "\n\n\n\n" x(역슬래시-n
    // 네 개)를 다시 읽으면 새 텍스트(실제 줄바꿈 네 개)가 입력 앞부분을 덮던 결함이 있었다.
    REQUIRE(ParseText(left, R"("\"\\n\\n\\n\\n\" x")").Succeeded());
    const JsonError aliased = left.Parse(left.Root().GetString());
    CHECK(aliased.code == JsonErrorCode::TrailingContent && aliased.position.line == 1 &&
          aliased.position.column == 12);
}
