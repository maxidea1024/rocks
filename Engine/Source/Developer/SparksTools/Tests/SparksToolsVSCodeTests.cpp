#include <Core/FileRead.h>
#include <Core/Text/Json.h>
#include <HostConfig/HostSettings.h>
#include <SparksCompiler/LanguageCatalog.h>
#include "SparksToolsTestSupport.h"

// VS Code 확장 문법의 키워드가 컴파일러 lexer 표와 같은지 본다(Docs/Design/SparksTools.ko.md 12.5절
// — LC8g). 문법(TextMate JSON)은 어휘 강조를 위해 키워드를 다시 적으므로, 그 사본이 컴파일러와
// 어긋나지 않게 이 시험이 대조한다(T13). manifest·의존·정규식 검사는 C# 도구
// 시험(SparksVSCode.*)이다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_tools_vscode_tests_file {

using namespace rocks::test::sparks_tools;

namespace {

constexpr StringView GrammarPath = "Tools/VSCode/sparks/syntaxes/sparks.tmLanguage.json";

// 문법 파일 읽기 상한이다(지금 수 KB).
constexpr uint64 GrammarMaxBytes = 1024 * 1024;

// 키워드 무리 규칙 이름의 접두와 패턴 모양(`\b(a|b|…)\b`)이다.
constexpr StringView KeywordRulePrefix = "keywords-";
constexpr StringView KeywordPatternOpen = "\\b(";
constexpr StringView KeywordPatternClose = ")\\b";

bool IsAsciiLetters(StringView word) noexcept {
    if (word.IsEmpty()) {
        return false;
    }

    for (usize at = 0; at < word.ByteCount(); ++at) {
        const char c = word.Data()[at];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            return false;
        }
    }

    return true;
}

bool Contains(const Array<StringView>& words, StringView word) noexcept {
    for (usize at = 0; at < words.Size(); ++at) {
        if (words[at] == word) {
            return true;
        }
    }

    return false;
}

} // namespace

TEST(SparksVSCode_GrammarKeywordsMatchCompiler) {
    SystemAllocator allocator;
    constexpr usize PathCapacity = 4096;
    char engine[PathCapacity];
    auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
    REQUIRE(found);

    // 엔진 폴더(`<저장소>/Engine`)의 부모가 저장소 뿌리다.
    usize parent = found.Value();
    while (parent > 0 && engine[parent - 1] != '/' && engine[parent - 1] != '\\') {
        --parent;
    }

    REQUIRE(parent > 0);
    auto grammarPath = Path::Combine(allocator, StringView(engine, parent - 1), GrammarPath);
    REQUIRE(grammarPath);
    Array<uint8> bytes(allocator);
    bool existed = false;
    REQUIRE(ReadWholeFile(platform::PhysicalFileSystem(), grammarPath.Value().View(),
                          GrammarMaxBytes, bytes, existed));
    REQUIRE(existed);
    JsonDocument document(allocator);
    REQUIRE(document.Parse(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()))
                .Succeeded());
    const JsonValue repository = document.Root().Find("repository");
    REQUIRE(repository.IsValid());
    REQUIRE(repository.IsObject());

    Array<StringView> grammarWords(allocator);
    usize rules = 0;
    for (usize member = 0; member < repository.Size(); ++member) {
        if (!repository.KeyAt(member).StartsWith(KeywordRulePrefix,
                                                 StringComparison::CaseSensitive)) {
            continue;
        }

        ++rules;
        const JsonValue match = repository.ValueAt(member).Find("match");
        REQUIRE(match.IsValid());
        REQUIRE(match.IsString());
        const StringView pattern = match.GetString();
        REQUIRE(pattern.StartsWith(KeywordPatternOpen, StringComparison::CaseSensitive));
        REQUIRE(pattern.EndsWith(KeywordPatternClose, StringComparison::CaseSensitive));
        const usize begin = KeywordPatternOpen.ByteCount();
        const usize end = pattern.ByteCount() - KeywordPatternClose.ByteCount();
        usize start = begin;
        for (usize at = begin; at <= end; ++at) {
            if (at < end && pattern.Data()[at] != '|') {
                continue;
            }

            const StringView word(pattern.Data() + start, at - start);
            CHECK(IsAsciiLetters(word));
            // 같은 키워드가 두 무리에 있으면 앞의 무리만 칠해진다(사본의 실수).
            CHECK(!Contains(grammarWords, word));
            REQUIRE(grammarWords.TryPushBack(word));
            start = at + 1;
        }
    }

    CHECK(rules > 0);
    CHECK(grammarWords.Size() == LanguageKeywordCount());
    for (uint32 index = 0; index < LanguageKeywordCount(); ++index) {
        const StringView keyword = LanguageKeywordAt(index).name;
        CHECK(!keyword.IsEmpty());
        CHECK(Contains(grammarWords, keyword));
    }
}

} // namespace sparks_tools_vscode_tests_file
