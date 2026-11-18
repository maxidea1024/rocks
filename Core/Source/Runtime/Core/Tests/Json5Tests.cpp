#include "Json5ParityCases.h"
#include "Json5SuiteExpectations.h"
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

// JSON5 읽기(JsonSyntax::Json5) 시험이다(CoreDataFoundations.ko.md 3절). json5-tests(데이터 전용
// ThirdParty) 전체를 확장자 규칙으로 판정하고, 받은 문서의 값을 C#과 공용인 기대 표
// (Json5SuiteExpectations.h)와, 손으로 쓴 사례를 공용 대조 표(Json5ParityCases.h)와 비교한다. 숫자
// 토큰 정규화·쓰기·한도·엄격 문법 불변도 본다.
namespace {

constexpr const char* SuiteRoot = "Core/Source/ThirdParty/Json5Tests/";
constexpr const char* SuiteDirectory =
    "Core/Source/ThirdParty/Json5Tests/json5-tests-ceb24d408013/";

// Rocks 정책 때문에 확장자 규칙과 판정이 다른 사례다(이유는 표에).
struct SuiteException {
    const char* path;
    bool accept;
    const char* reason;
};

// 확장자는 .json이지만 RFC 8259가 아닌 사례다(원본이 주석을 담고 있다). Json5로는 받고, 엄격 문법과
// 같은 문서인지는 보지 않는다.
const char* const NotStrictJson[] = {"comments/irregular-block-comment.json"};

const SuiteException SuiteExceptions[] = {
    {"objects/duplicate-keys.json", false,
     "duplicate keys are DuplicateKey (JSON5 keeps the last)"},
};

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

JsonReadOptions Json5Options() noexcept {
    JsonReadOptions options;
    options.syntax = JsonSyntax::Json5;
    return options;
}

JsonError ParseJson5(JsonDocument& document, const char* text,
                     JsonReadOptions options = Json5Options()) noexcept {
    options.syntax = JsonSyntax::Json5;
    return document.Parse(StringView(text, strlen(text)), options);
}

const char* ErrorCodeName(JsonErrorCode code) noexcept {
    switch (code) {
    case JsonErrorCode::None:
        return "None";
    case JsonErrorCode::InputTooLarge:
        return "InputTooLarge";
    case JsonErrorCode::UnexpectedEnd:
        return "UnexpectedEnd";
    case JsonErrorCode::UnexpectedCharacter:
        return "UnexpectedCharacter";
    case JsonErrorCode::InvalidLiteral:
        return "InvalidLiteral";
    case JsonErrorCode::InvalidNumber:
        return "InvalidNumber";
    case JsonErrorCode::NumberOutOfRange:
        return "NumberOutOfRange";
    case JsonErrorCode::InvalidEscape:
        return "InvalidEscape";
    case JsonErrorCode::InvalidUtf8:
        return "InvalidUtf8";
    case JsonErrorCode::UnpairedSurrogate:
        return "UnpairedSurrogate";
    case JsonErrorCode::ControlCharacter:
        return "ControlCharacter";
    case JsonErrorCode::DuplicateKey:
        return "DuplicateKey";
    case JsonErrorCode::TrailingContent:
        return "TrailingContent";
    case JsonErrorCode::DepthLimitExceeded:
        return "DepthLimitExceeded";
    case JsonErrorCode::StringTooLong:
        return "StringTooLong";
    case JsonErrorCode::TooManyNodes:
        return "TooManyNodes";
    case JsonErrorCode::OutOfMemory:
        return "OutOfMemory";
    }

    return "Unknown";
}

// 공용 기대값 표기의 문자열 부분이다(Json5ParityCases.h 머리 주석).
void DumpString(StringView text, StringBuilderBase& out) noexcept {
    REQUIRE(out.TryAppend(StringView("\"")));
    // 문서 문자열은 파서가 검증한 UTF-8이다.
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.Data());
    for (usize index = 0; index < text.ByteCount();) {
        const unsigned char lead = bytes[index];
        const usize length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        uint32 codePoint = length == 1   ? lead
                           : length == 2 ? (lead & 0x1Fu)
                           : length == 3 ? (lead & 0x0Fu)
                                         : (lead & 0x07u);
        for (usize next = 1; next < length; ++next) {
            codePoint = (codePoint << 6) | (bytes[index + next] & 0x3Fu);
        }

        index += length;
        if (codePoint == '"' || codePoint == '\\') {
            const char escaped[] = {'\\', static_cast<char>(codePoint)};
            REQUIRE(out.TryAppend(StringView(escaped, 2)));
        } else if (codePoint >= 0x20 && codePoint <= 0x7E) {
            const char plain = static_cast<char>(codePoint);
            REQUIRE(out.TryAppend(StringView(&plain, 1)));
        } else {
            REQUIRE(out.TryAppendFormat("\\u{{{:X}}}", codePoint));
        }
    }

    REQUIRE(out.TryAppend(StringView("\"")));
}

// 공용 기대값 표기다. 시험 문서는 얕아서 재귀해도 된다.
void Dump(JsonValue value, StringBuilderBase& out) noexcept {
    switch (value.Type()) {
    case JsonType::Null:
        REQUIRE(out.TryAppend(StringView("null")));
        return;
    case JsonType::Bool:
        REQUIRE(out.TryAppend(StringView(value.GetBool() ? "true" : "false")));
        return;
    case JsonType::Integer:
        REQUIRE(out.TryAppendFormat("i{}", value.TryGetInt64().Value()));
        return;
    case JsonType::Unsigned:
        REQUIRE(out.TryAppendFormat("u{}", value.TryGetUInt64().Value()));
        return;
    case JsonType::Double:
        REQUIRE(out.TryAppendFormat("d{:016x}",
                                    platform::BitCast<uint64>(value.TryGetDouble().Value())));
        return;
    case JsonType::String:
        DumpString(value.GetString(), out);
        return;
    case JsonType::Array:
        REQUIRE(out.TryAppend(StringView("[")));
        for (usize index = 0; index < value.Size(); ++index) {
            if (index != 0) {
                REQUIRE(out.TryAppend(StringView(",")));
            }

            Dump(value.At(index), out);
        }

        REQUIRE(out.TryAppend(StringView("]")));
        return;
    case JsonType::Object:
        REQUIRE(out.TryAppend(StringView("{")));
        for (usize index = 0; index < value.Size(); ++index) {
            if (index != 0) {
                REQUIRE(out.TryAppend(StringView(",")));
            }

            DumpString(value.KeyAt(index), out);
            REQUIRE(out.TryAppend(StringView(":")));
            Dump(value.ValueAt(index), out);
        }

        REQUIRE(out.TryAppend(StringView("}")));
        return;
    }
}

void DumpResult(const JsonDocument& document, JsonError error, StringBuilderBase& out) noexcept {
    out.Clear();
    if (error.Succeeded()) {
        Dump(document.Root(), out);
        return;
    }

    REQUIRE(out.TryAppendFormat("!{}@{}:{}", ErrorCodeName(error.code), error.position.line,
                                error.position.column));
}

// 기대 표에 붙여 넣을 수 있게 C 문자열로 출력한다(불일치 진단용).
void PrintCString(StringView text) noexcept {
    putchar('"');
    for (usize index = 0; index < text.ByteCount(); ++index) {
        const auto byte = static_cast<unsigned char>(text.Data()[index]);
        if (byte == '"' || byte == '\\') {
            printf("\\%c", byte);
        } else if (byte >= 0x20 && byte < 0x7F) {
            putchar(byte);
        } else {
            printf("\\x%02X", byte);
        }
    }

    putchar('"');
}

bool HasNonFinite(JsonValue value) noexcept {
    switch (value.Type()) {
    case JsonType::Double: {
        const uint64 bits = platform::BitCast<uint64>(value.TryGetDouble().Value());
        return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull;
    }
    case JsonType::Array:
        for (usize index = 0; index < value.Size(); ++index) {
            if (HasNonFinite(value.At(index))) {
                return true;
            }
        }

        return false;
    case JsonType::Object:
        for (usize index = 0; index < value.Size(); ++index) {
            if (HasNonFinite(value.ValueAt(index))) {
                return true;
            }
        }

        return false;
    default:
        return false;
    }
}

// 엄격 비교: 종류와 값 비트, 숫자 원문까지 같아야 한다.
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
    case JsonType::Unsigned:
    case JsonType::Double:
        return JsonEquals(left, right) && left.RawNumber() == right.RawNumber() &&
               (left.Type() != JsonType::Double ||
                platform::BitCast<uint64>(left.TryGetDouble().Value()) ==
                    platform::BitCast<uint64>(right.TryGetDouble().Value()));
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

} // namespace

TEST(Json5_ParityCases) {
    SystemAllocator allocator;
    JsonDocument document(allocator);
    StringBuilder<> actual(allocator);
    usize failures = 0;
    for (const test::Json5ParityCase& item : test::Json5ParityCases) {
        const JsonError error = ParseJson5(document, item.input);
        DumpResult(document, error, actual);
        if (actual.View() != StringView(item.expected, strlen(item.expected))) {
            ++failures;
            printf("Json5 parity case ");
            PrintCString(StringView(item.input, strlen(item.input)));
            printf(": got ");
            PrintCString(actual.View());
            printf(", expected ");
            PrintCString(StringView(item.expected, strlen(item.expected)));
            printf("\n");
        }
    }

    CHECK(failures == 0);
}

TEST(Json5_StringLimitCases) {
    SystemAllocator allocator;
    JsonDocument document(allocator);
    StringBuilder<> actual(allocator);
    JsonReadOptions options;
    options.maxStringBytes = test::Json5StringLimit;
    usize failures = 0;
    for (const test::Json5ParityCase& item : test::Json5StringLimitCases) {
        const StringView input(item.input, strlen(item.input));
        const StringView expected(item.expected, strlen(item.expected));
        options.syntax = JsonSyntax::Json5;
        DumpResult(document, document.Parse(input, options), actual);
        bool same = actual.View() == expected;
        // 큰따옴표 문자열 하나뿐인 입력은 엄격 문법도 같은 한도 판정이다.
        if (input.StartsWith(StringView("\"")) && input.EndsWith(StringView("\""))) {
            options.syntax = JsonSyntax::Strict;
            DumpResult(document, document.Parse(input, options), actual);
            same = same && actual.View() == expected;
        }

        if (!same) {
            ++failures;
            printf("Json5 string limit case ");
            PrintCString(input);
            printf(": got ");
            PrintCString(actual.View());
            printf("\n");
        }
    }

    CHECK(failures == 0);
}

TEST(Json5_TestSuiteCases) {
    SystemAllocator allocator;
    // 사례 목록은 데이터 라이브러리 manifest의 files에서 읽는다.
    Array<char> manifestText(allocator);
    char manifestPath[256];
    snprintf(manifestPath, sizeof(manifestPath), "%sThirdParty.json", SuiteRoot);
    REQUIRE(ReadFile(manifestPath, manifestText));
    JsonDocument manifest(allocator);
    REQUIRE(manifest.Parse(StringView(manifestText.Data(), manifestText.Size())).Succeeded());
    const JsonValue files = manifest.Root().Find("files");
    REQUIRE(files.IsObject());

    constexpr usize ExpectationCount =
        sizeof(test::Json5SuiteExpectations) / sizeof(test::Json5SuiteExpectations[0]);
    bool expectationUsed[ExpectationCount] = {};
    usize cases = 0;
    usize accepted = 0;
    usize rejected = 0;
    usize exceptions = 0;
    Array<char> content(allocator);
    JsonDocument document(allocator);
    JsonDocument strict(allocator);
    StringBuilder<> dump(allocator);
    StringBuilder<> written(allocator);
    for (usize index = 0; index < files.Size(); ++index) {
        const StringView key = files.KeyAt(index);
        const bool json = key.EndsWith(StringView(".json"));
        const bool json5 = key.EndsWith(StringView(".json5"));
        const bool invalid = key.EndsWith(StringView(".js")) || key.EndsWith(StringView(".txt"));
        if (!json && !json5 && !invalid) {
            continue; // LICENSE.md
        }

        char name[160];
        snprintf(name, sizeof(name), "%.*s", static_cast<int>(key.ByteCount()), key.Data());
        char path[320];
        snprintf(path, sizeof(path), "%s%s", SuiteDirectory, name);
        REQUIRE(ReadFile(path, content));
        ++cases;
        bool expectAccept = json || json5;
        for (const SuiteException& exception : SuiteExceptions) {
            if (strcmp(exception.path, name) == 0) {
                expectAccept = exception.accept;
                ++exceptions;
            }
        }

        const StringView text(content.Data(), content.Size());
        const JsonError error = document.Parse(text, Json5Options());
        if (error.Succeeded() != expectAccept) {
            printf("json5-tests %s: %s (%s at %u:%u)\n", name,
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
        // 값: C#과 공용인 기대 표와 같아야 한다.
        DumpResult(document, error, dump);
        bool found = false;
        for (usize expectation = 0; expectation < ExpectationCount; ++expectation) {
            const test::Json5SuiteExpectation& item = test::Json5SuiteExpectations[expectation];
            if (strcmp(item.path, name) != 0) {
                continue;
            }

            found = true;
            expectationUsed[expectation] = true;
            if (dump.View() != StringView(item.expected, strlen(item.expected))) {
                printf("json5-tests %s: got ", name);
                PrintCString(dump.View());
                printf("\n");
                CHECK(false);
            }
        }

        if (!found) {
            printf("json5-tests: no expectation for {\"%s\", ", name);
            PrintCString(dump.View());
            printf("},\n");
            CHECK(found);
        }

        // .json 사례는 엄격 문법으로도 같은 문서다(Json5는 JSON의 상위 집합).
        bool strictJson = json;
        for (const char* notStrict : NotStrictJson) {
            strictJson = strictJson && strcmp(notStrict, name) != 0;
        }

        if (strictJson) {
            const bool same =
                strict.Parse(text).Succeeded() && StrictSame(document.Root(), strict.Root());
            if (!same) {
                printf("json5-tests %s: strict reading differs\n", name);
            }

            CHECK(same);
        }

        // 쓰기: 비유한 수는 거부하고, 그 밖에는 Canonical·Preserve 출력을 엄격 문법이 같은 문서로
        // 다시 읽는다(정규화한 숫자 토큰은 RFC 8259다).
        if (HasNonFinite(document.Root())) {
            CHECK(!Writes(document.Root(), JsonNumberMode::Canonical, written));
            CHECK(!Writes(document.Root(), JsonNumberMode::Preserve, written));
            continue;
        }

        REQUIRE(Writes(document.Root(), JsonNumberMode::Preserve, written));
        const bool preserved =
            strict.Parse(written.View()).Succeeded() && StrictSame(document.Root(), strict.Root());
        if (!preserved) {
            printf("json5-tests %s: preserve output does not re-read as the same document\n", name);
        }

        CHECK(preserved);
        REQUIRE(Writes(document.Root(), JsonNumberMode::Canonical, written));
        CHECK(strict.Parse(written.View()).Succeeded() &&
              JsonEquals(document.Root(), strict.Root()));
    }

    for (usize expectation = 0; expectation < ExpectationCount; ++expectation) {
        if (!expectationUsed[expectation]) {
            printf("json5-tests: unused expectation %s\n",
                   test::Json5SuiteExpectations[expectation].path);
            CHECK(false);
        }
    }

    printf("json5-tests: %llu cases, %llu accepted, %llu rejected, %llu exceptions\n",
           static_cast<unsigned long long>(cases), static_cast<unsigned long long>(accepted),
           static_cast<unsigned long long>(rejected), static_cast<unsigned long long>(exceptions));
    CHECK(cases == 114);
    CHECK(exceptions == sizeof(SuiteExceptions) / sizeof(SuiteExceptions[0]));
}

TEST(Json5_NumberTokensAndWriter) {
    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(ParseJson5(document, "[+1, .5, 5., -.5e1, 0x1F, -0x0, 1e+2, Infinity, -Infinity, -NaN, "
                                 "0.10000000149011612]")
                .Succeeded());
    const JsonValue root = document.Root();
    // 유한한 수의 원문은 엄격 문법이 같은 값으로 읽는 RFC 8259 꼴이다.
    const char* const tokens[] = {"1",  "0.5",  "5.0",      "-0.5e1",    "31",
                                  "-0", "1e+2", "Infinity", "-Infinity", "NaN"};
    for (usize index = 0; index < sizeof(tokens) / sizeof(tokens[0]); ++index) {
        CHECK(root.At(index).RawNumber() == StringView(tokens[index], strlen(tokens[index])));
    }

    CHECK(root.At(0).Type() == JsonType::Integer && root.At(4).TryGetInt64().Value() == 31);
    CHECK(root.At(2).Type() == JsonType::Double && root.At(5).Type() == JsonType::Double);
    // float 왕복 경로(TryParseFloat(RawNumber()))도 정규화한 토큰으로 그대로 동작한다.
    CHECK(TryParseFloat(root.At(1).RawNumber()).Value() == 0.5f);
    CHECK(TryParseFloat(root.At(10).RawNumber()).Value() == 0.1f);
    // NaN은 부호와 무관하게 한 비트 패턴이라 JsonEquals가 같다고 본다. ±Infinity는 다르다.
    JsonDocument other(allocator);
    REQUIRE(ParseJson5(other, "[NaN, +Infinity]").Succeeded());
    CHECK(JsonEquals(root.At(9), other.Root().At(0)));
    CHECK(JsonEquals(root.At(7), other.Root().At(1)));
    CHECK(!JsonEquals(root.At(8), other.Root().At(1)));
    CHECK(!JsonEquals(root.At(9), root.At(7)));
    // 비유한 수는 JSON에 없으므로 쓰기가 거부한다(Preserve도 원문 "NaN"을 쓰지 않는다).
    StringBuilder<> text(allocator);
    JsonWriter canonical(text);
    CHECK(canonical.Value(root.At(7)).Error() == ErrorCode::InvalidArgument);
    text.Clear();
    JsonWriter preserve(text, JsonNumberMode::Preserve);
    CHECK(preserve.Value(root.At(9)).Error() == ErrorCode::InvalidArgument);
    // 유한한 값만 모은 문서의 Preserve 출력은 정규화한 토큰이다.
    REQUIRE(ParseJson5(document, "{n: [+.5, 0xff, 7.,],}").Succeeded());
    REQUIRE(Writes(document.Root(), JsonNumberMode::Preserve, text));
    CHECK(text.View() == "{\n  \"n\": [\n    0.5,\n    255,\n    7.0\n  ]\n}\n");
}

TEST(Json5_OptionsLimitsAndStrictUnchanged) {
    SystemAllocator allocator;
    JsonDocument document(allocator);
    // 엄격 문법은 JSON5 기능을 그대로 거부한다(allowLineComments도 줄 주석만 연다).
    const char* const json5Only[] = {"[1,]", "{a: 1}",     "'a'",        "/* c */ 1",
                                     "0x10", ".5",         "+1",         "Infinity",
                                     "NaN",  "[1 // c\n]", "\"a\\x41\"", "\"\\'\""};
    for (const char* text : json5Only) {
        CHECK(!document.Parse(StringView(text, strlen(text))).Succeeded());
        CHECK(ParseJson5(document, text).Succeeded());
    }

    JsonReadOptions comments;
    comments.allowLineComments = true;
    CHECK(document.Parse(StringView("[1 // c\n]"), comments).Succeeded());
    CHECK(!document.Parse(StringView("[1, // c\n]"), comments).Succeeded());
    // Json5는 allowLineComments와 무관하게 주석을 받는다.
    comments.syntax = JsonSyntax::Json5;
    comments.allowLineComments = false;
    CHECK(document.Parse(StringView("[1, // c\n]"), comments).Succeeded());
    // 맨 앞 BOM은 엄격 문법처럼 경고를 남긴다.
    REQUIRE(ParseJson5(document, "\xEF\xBB\xBF{a: 1}").Succeeded());
    CHECK(document.Warnings().Size() == 1 &&
          document.Warnings()[0].code == JsonWarningCode::BomIgnored);
    // NUL을 포함한 제어 문자는 JSON5 문자열에 그대로 올 수 있다.
    const char withNul[] = {'\'', 'a', '\0', 'b', '\''};
    REQUIRE(document.Parse(StringView(withNul, sizeof(withNul)), Json5Options()).Succeeded());
    CHECK(document.Root().GetString() == StringView("a\0b", 3));
    // 값 위치는 원문 기준이다(주석·이스케이프 포함).
    REQUIRE(ParseJson5(document, "/* x */ {\n  // y\n  'k\\u0041': [ .5 ],\n}").Succeeded());
    const JsonPosition at = document.Root().Find("kA").At(0).Position();
    CHECK(at.line == 3 && at.column == 16 && at.byteOffset == 32);

    // 깊이·문자열·식별자 키·노드·입력 한도는 두 문법이 같다.
    StringBuilder<> text(allocator);
    REQUIRE(text.TryAppendRepeated('[', 128) && text.TryAppendRepeated(']', 128));
    CHECK(document.Parse(text.View(), Json5Options()).Succeeded());
    text.Clear();
    REQUIRE(text.TryAppendRepeated('[', 129));
    const JsonError deep = document.Parse(text.View(), Json5Options());
    CHECK(deep.code == JsonErrorCode::DepthLimitExceeded && deep.position.column == 129);
    JsonReadOptions options = Json5Options();
    options.maxStringBytes = 4;
    CHECK(ParseJson5(document, "['abcd']", options).Succeeded());
    CHECK(ParseJson5(document, "['abcde']", options).code == JsonErrorCode::StringTooLong);
    CHECK(ParseJson5(document, "{abcd: 1}", options).Succeeded());
    const JsonError longKey = ParseJson5(document, "{abcde: 1}", options);
    CHECK(longKey.code == JsonErrorCode::StringTooLong && longKey.position.column == 2);
    CHECK(ParseJson5(document, "['\\xe9\\xe9']", options).Succeeded());
    CHECK(ParseJson5(document, "['\\xe9\\xe9\\x41']", options).code ==
          JsonErrorCode::StringTooLong);
    CHECK(ParseJson5(document, "['ab\\\ncd']", options).Succeeded()); // 줄 이음은 세지 않는다
    options = Json5Options();
    options.maxNodes = 3;
    CHECK(ParseJson5(document, "[1, 2,]", options).Succeeded());
    CHECK(ParseJson5(document, "[1, 2, 3]", options).code == JsonErrorCode::TooManyNodes);
    options = Json5Options();
    options.maxInputBytes = 4;
    CHECK(ParseJson5(document, "[1,]", options).Succeeded());
    CHECK(ParseJson5(document, "[11,]", options).code == JsonErrorCode::InputTooLarge);
}
