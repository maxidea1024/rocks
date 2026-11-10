#include <Tests/Support/Test.h>
#include <Tests/Support/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Localization/Text.h>

using namespace rocks;

namespace {
constexpr TextId GreetingId{10, 20};
constexpr char Korean[] = "\xED\x95\x9C\xEA\xB8\x80";
constexpr char Emoji[] = "\xF0\x9F\x8E\xAE";

void CheckText(Allocator& allocator, const Text& text, StringView expected,
               const CatalogSnapshot& catalog = {}) {
    auto resolved = ResolveText(allocator, text, catalog);
    REQUIRE(resolved);
    CHECK(resolved.Value().value.View() == expected);
}
} // namespace

TEST(TextInvariantOwnsUtf8AndEmbeddedNullAfterInputDestruction) {
    SystemAllocator allocator;
    {
        Text text;
        {
            char scratch[] = {'a', '\0', '\xED', '\x95', '\x9C'};
            auto made = Text::Invariant(allocator, StringView(scratch, sizeof(scratch)));
            REQUIRE(made);
            text = Move(made.Value());
            for (char& byte : scratch) {
                byte = 'x';
            }
        }
        const char expected[] = {'a', '\0', '\xED', '\x95', '\x9C'};
        auto output = ResolveText(allocator, text, {});
        REQUIRE(output);
        text.Reset();
        CHECK(output.Value().value.View() == StringView(expected, sizeof(expected)));
        CHECK(output.Value().diagnostics.missingTranslations == 0);
        CHECK(output.Value().diagnostics.staleTranslations == 0);
        CHECK(output.Value().value.CStr()[sizeof(expected)] == '\0');
        auto invalid = Text::Invariant(allocator, StringView("\xC0\xAF", 2));
        CHECK(!invalid && invalid.Error() == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextIntegerHasExactSignedDecimalBoundaryGoldens) {
    SystemAllocator allocator;
    {
        const int64 numbers[] = {0, -1, 9223372036854775807LL, (-9223372036854775807LL - 1)};
        const char* expected[] = {"0", "-1", "9223372036854775807", "-9223372036854775808"};
        for (usize index = 0; index < 4; ++index) {
            auto text = Text::Integer(allocator, numbers[index]);
            REQUIRE(text);
            CheckText(allocator, text.Value(), expected[index]);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextLocalizedDistinguishesMissingStaleAndMatchingTranslation) {
    SystemAllocator allocator;
    {
        auto text = Text::Localized(allocator, GreetingId, 2, "source fallback");
        REQUIRE(text);
        auto missing = ResolveText(allocator, text.Value(), {});
        REQUIRE(missing);
        CHECK(missing.Value().value.View() == StringView("source fallback"));
        CHECK(missing.Value().diagnostics.missingTranslations == 1);
        CHECK(missing.Value().diagnostics.staleTranslations == 0);
        const CatalogEntry staleEntry[] = {{GreetingId, 1, "old translation"}};
        auto staleCatalog = CatalogSnapshot::Create(allocator, "ko", 10, {staleEntry, 1});
        REQUIRE(staleCatalog);
        auto stale = ResolveText(allocator, text.Value(), staleCatalog.Value());
        REQUIRE(stale);
        CHECK(stale.Value().value.View() == StringView("source fallback"));
        CHECK(stale.Value().diagnostics.missingTranslations == 0);
        CHECK(stale.Value().diagnostics.staleTranslations == 1);
        const CatalogEntry correctEntry[] = {{GreetingId, 2, Korean}};
        auto correctCatalog = CatalogSnapshot::Create(allocator, "ko", 11, {correctEntry, 1});
        REQUIRE(correctCatalog);
        auto translated = ResolveText(allocator, text.Value(), correctCatalog.Value());
        REQUIRE(translated);
        CHECK(translated.Value().value.View() == StringView(Korean));
        CHECK(translated.Value().diagnostics.missingTranslations == 0);
        CHECK(translated.Value().diagnostics.staleTranslations == 0);
        const CatalogEntry unrelatedEntry[] = {{{100, 200}, 2, "unrelated"}};
        auto unrelated = CatalogSnapshot::Create(allocator, "en", 3, {unrelatedEntry, 1});
        REQUIRE(unrelated);
        auto absent = ResolveText(allocator, text.Value(), unrelated.Value());
        REQUIRE(absent);
        CHECK(absent.Value().value.View() == StringView("source fallback"));
        CHECK(absent.Value().diagnostics.missingTranslations == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextFormatUnicodeEscapesRepeatedNamesAndOpaqueArgumentBraces) {
    SystemAllocator allocator;
    {
        auto pattern = Text::Invariant(
            allocator, "\xED\x95\x9C\xEA\xB8\x80 {{ {item} }} \xF0\x9F\x8E\xAE {item}");
        auto argument = Text::Invariant(allocator, "{unbound} \xF0\x9F\x8E\xAE");
        auto unused = Text::Invariant(allocator, "unused");
        REQUIRE(pattern && argument && unused);
        const TextArgument arguments[] = {{"item", &argument.Value()}, {"unused", &unused.Value()}};
        auto text = Text::Format(allocator, pattern.Value(), {arguments, 2});
        REQUIRE(text);
        CheckText(allocator, text.Value(),
                  "\xED\x95\x9C\xEA\xB8\x80 { {unbound} \xF0\x9F\x8E\xAE } \xF0\x9F\x8E\xAE "
                  "{unbound} \xF0\x9F\x8E\xAE");
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextFormatRetainsHistoryAndCopiesNamesAfterScratchDestruction) {
    SystemAllocator allocator;
    {
        Text formatted;
        {
            String source(allocator);
            String argumentName(allocator);
            REQUIRE(source.TryAssign("hello {player}"));
            REQUIRE(argumentName.TryAssign("player"));
            auto pattern = Text::Invariant(allocator, source.View());
            auto value = Text::Invariant(allocator, Korean);
            REQUIRE(pattern && value);
            const TextArgument args[] = {{argumentName.View(), &value.Value()}};
            auto made = Text::Format(allocator, pattern.Value(), {args, 1});
            REQUIRE(made);
            formatted = Move(made.Value());
            REQUIRE(source.TryAssign("overwritten"));
            REQUIRE(argumentName.TryAssign("different"));
        }
        auto retained = formatted.Clone();
        REQUIRE(retained);
        Text moved(Move(formatted));
        CHECK(!formatted.IsValid());
        moved.Reset();
        CheckText(allocator, retained.Value(), "hello \xED\x95\x9C\xEA\xB8\x80");
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextFormatRejectsInvalidBindingsWithoutChangingInputs) {
    SystemAllocator allocator;
    {
        auto pattern = Text::Invariant(allocator, "{valid}");
        auto value = Text::Invariant(allocator, "ok");
        REQUIRE(pattern && value);
        const char* badNames[] = {"", "0bad", "a-b", "a b", "\xED\x95\x9C"};
        for (const char* name : badNames) {
            const TextArgument args[] = {{name, &value.Value()}};
            auto bad = Text::Format(allocator, pattern.Value(), {args, 1});
            CHECK(!bad && bad.Error() == ErrorCode::InvalidArgument);
        }
        char longName[65];
        for (char& byte : longName) {
            byte = 'a';
        }
        const TextArgument longArg[] = {{StringView(longName, 65), &value.Value()}};
        CHECK(!Text::Format(allocator, pattern.Value(), {longArg, 1}));
        const TextArgument duplicate[] = {{"valid", &value.Value()}, {"valid", &value.Value()}};
        auto duplicateResult = Text::Format(allocator, pattern.Value(), {duplicate, 2});
        CHECK(!duplicateResult && duplicateResult.Error() == ErrorCode::InvalidArgument);
        Text invalid;
        const TextArgument nullArg[] = {{"valid", nullptr}};
        const TextArgument invalidArg[] = {{"valid", &invalid}};
        CHECK(!Text::Format(allocator, pattern.Value(), {nullArg, 1}));
        CHECK(!Text::Format(allocator, pattern.Value(), {invalidArg, 1}));
        CHECK(!Text::Format(allocator, invalid, {}));
        const TextArgument valid[] = {{"valid", &value.Value()}};
        auto good = Text::Format(allocator, pattern.Value(), {valid, 1});
        REQUIRE(good);
        CheckText(allocator, good.Value(), "ok");
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextFormatDefersSyntaxAndMissingBindingErrorsUntilResolve) {
    SystemAllocator allocator;
    {
        const char* patterns[] = {"{", "}", "{}", "{missing}", "{a-b}", "{a{b}", "{{}"};
        for (const char* source : patterns) {
            auto pattern = Text::Invariant(allocator, source);
            REQUIRE(pattern);
            auto format = Text::Format(allocator, pattern.Value(), {});
            REQUIRE(format);
            auto result = ResolveText(allocator, format.Value(), {});
            CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
        }
        auto pattern = Text::Invariant(allocator, "{{}}|");
        REQUIRE(pattern);
        auto format = Text::Format(allocator, pattern.Value(), {});
        REQUIRE(format);
        CheckText(allocator, format.Value(), "{}|");
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextFormatParsesSelectedTranslationAndCountsRepeatedLocalizedVisits) {
    SystemAllocator allocator;
    {
        auto pattern = Text::Localized(allocator, GreetingId, 1, "source {person}");
        auto person = Text::Localized(allocator, {50, 60}, 1, "Rocks");
        REQUIRE(pattern && person);
        const TextArgument args[] = {{"person", &person.Value()}};
        auto text = Text::Format(allocator, pattern.Value(), {args, 1});
        REQUIRE(text);
        const CatalogEntry entries[] = {{GreetingId, 1, "{person} / {person}"}};
        auto catalog = CatalogSnapshot::Create(allocator, "ko", 2, {entries, 1});
        REQUIRE(catalog);
        auto result = ResolveText(allocator, text.Value(), catalog.Value());
        REQUIRE(result);
        CHECK(result.Value().value.View() == StringView("Rocks / Rocks"));
        CHECK(result.Value().diagnostics.missingTranslations == 2);
        CHECK(result.Value().diagnostics.staleTranslations == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextResolveOutputDepthAndWorkLimitsFailWithoutMutatingInputs) {
    SystemAllocator allocator;
    {
        auto literal = Text::Invariant(allocator, Emoji);
        REQUIRE(literal);
        TextResolveLimits limits;
        limits.maxOutputBytes = 4;
        limits.maxDepth = 1;
        auto exact = ResolveText(allocator, literal.Value(), {}, limits);
        REQUIRE(exact);
        CHECK(exact.Value().value.View() == StringView(Emoji));
        limits.maxOutputBytes = 3;
        auto overOutput = ResolveText(allocator, literal.Value(), {}, limits);
        CHECK(!overOutput && overOutput.Error() == ErrorCode::CapacityExceeded);
        limits = {};
        limits.maxWork = 0;
        auto overWork = ResolveText(allocator, literal.Value(), {}, limits);
        CHECK(!overWork && overWork.Error() == ErrorCode::CapacityExceeded);
        limits = {};
        limits.maxDepth = 65;
        auto invalidDepth = ResolveText(allocator, literal.Value(), {}, limits);
        CHECK(!invalidDepth && invalidDepth.Error() == ErrorCode::InvalidArgument);
        auto pattern = Text::Invariant(allocator, "{v}");
        REQUIRE(pattern);
        const TextArgument args[] = {{"v", &literal.Value()}};
        auto formatted = Text::Format(allocator, pattern.Value(), {args, 1});
        REQUIRE(formatted);
        limits = {};
        limits.maxDepth = 1;
        auto tooShallow = ResolveText(allocator, formatted.Value(), {}, limits);
        CHECK(!tooShallow && tooShallow.Error() == ErrorCode::CapacityExceeded);
        limits.maxDepth = 2;
        auto enough = ResolveText(allocator, formatted.Value(), {}, limits);
        REQUIRE(enough);
        CHECK(enough.Value().value.View() == StringView(Emoji));
        CheckText(allocator, literal.Value(), Emoji);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextCreationLimitsAndSharedDeepHistoryReleaseNeedNoAllocation) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    {
        TextCreateLimits limits;
        limits.maxSourceBytes = 3;
        auto tooLong = Text::Invariant(failing, "four", limits);
        CHECK(!tooLong && tooLong.Error() == ErrorCode::CapacityExceeded);
        auto pattern = Text::Invariant(failing, "{v}");
        auto leaf = Text::Invariant(failing, "leaf");
        REQUIRE(pattern && leaf);
        const TextArgument args[] = {{"v", &leaf.Value()}};
        limits = {};
        limits.maxArguments = 0;
        auto tooMany = Text::Format(failing, pattern.Value(), {args, 1}, limits);
        CHECK(!tooMany && tooMany.Error() == ErrorCode::CapacityExceeded);
        Text chain = Move(leaf.Value());
        for (usize depth = 0; depth < 2048; ++depth) {
            const TextArgument nextArgs[] = {{"v", &chain}};
            auto next = Text::Format(failing, pattern.Value(), {nextArgs, 1});
            REQUIRE(next);
            chain = Move(next.Value());
        }
        auto clone = chain.Clone();
        REQUIRE(clone);
        auto bounded = ResolveText(failing, chain, {});
        CHECK(!bounded && bounded.Error() == ErrorCode::CapacityExceeded);
        failing.FailOnAttempt(1);
        chain.Reset();
        clone.Value().Reset();
        pattern.Value().Reset();
        CHECK(failing.Attempts() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextResolveMaximumDepthBoundaryHasExactGolden) {
    SystemAllocator allocator;
    {
        auto pattern = Text::Invariant(allocator, "{v}");
        auto leaf = Text::Invariant(allocator, "x");
        REQUIRE(pattern && leaf);
        Text chain = Move(leaf.Value());
        for (usize count = 0; count < 63; ++count) {
            const TextArgument args[] = {{"v", &chain}};
            auto next = Text::Format(allocator, pattern.Value(), {args, 1});
            REQUIRE(next);
            chain = Move(next.Value());
        }
        CheckText(allocator, chain, "x");
        const TextArgument args[] = {{"v", &chain}};
        auto tooDeep = Text::Format(allocator, pattern.Value(), {args, 1});
        REQUIRE(tooDeep);
        auto rejected = ResolveText(allocator, tooDeep.Value(), {});
        CHECK(!rejected && rejected.Error() == ErrorCode::CapacityExceeded);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
