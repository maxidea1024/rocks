#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Localization/Text.h>

using namespace rocks;

TEST(CatalogOwnsInputsAndRetainedSnapshotOutlivesStoreAndPublishSources) {
    SystemAllocator allocator;
    {
        CatalogSnapshot oldReader;
        CatalogSnapshot newReader;
        auto text = Text::Localized(allocator, {1, 2}, 7, "fallback");
        REQUIRE(text);
        {
            CatalogStore store;
            auto empty = store.Acquire();
            CHECK(!empty && empty.Error() == ErrorCode::NotFound);
            {
                char culture[] = "en-US";
                char translation[] = "Hello";
                const CatalogEntry entries[] = {{{1, 2}, 7, translation}};
                auto original = CatalogSnapshot::Create(allocator, culture, 11, {entries, 1});
                REQUIRE(original);
                REQUIRE(store.Publish(original.Value()));
                culture[0] = 'x';
                translation[0] = 'x';
                auto read = store.Acquire();
                REQUIRE(read);
                oldReader = Move(read.Value());
            }
            const CatalogEntry nextEntries[] = {{{1, 2}, 7, "Bonjour"}};
            auto next = CatalogSnapshot::Create(allocator, "fr-FR", 12, {nextEntries, 1});
            REQUIRE(next);
            REQUIRE(store.Publish(next.Value()));
            auto read = store.Acquire();
            REQUIRE(read);
            newReader = Move(read.Value());
            CatalogSnapshot invalid;
            auto refused = store.Publish(invalid);
            CHECK(!refused && refused.Error() == ErrorCode::InvalidArgument);
            auto unchanged = store.Acquire();
            REQUIRE(unchanged);
            CHECK(unchanged.Value().Revision() == 12);
            store.Close();
            store.Close();
            auto closed = store.Acquire();
            CHECK(!closed && closed.Error() == ErrorCode::ShuttingDown);
            auto rejected = store.Publish(next.Value());
            CHECK(!rejected && rejected.Error() == ErrorCode::ShuttingDown);
        }
        CHECK(oldReader.Culture() == StringView("en-US"));
        CHECK(oldReader.Revision() == 11);
        CHECK(oldReader.EntryCount() == 1);
        CHECK(newReader.Culture() == StringView("fr-FR"));
        auto oldOutput = ResolveText(allocator, text.Value(), oldReader);
        auto newOutput = ResolveText(allocator, text.Value(), newReader);
        REQUIRE(oldOutput && newOutput);
        oldReader.Reset();
        newReader.Reset();
        CHECK(oldOutput.Value().value.View() == StringView("Hello"));
        CHECK(newOutput.Value().value.View() == StringView("Bonjour"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CatalogCloneMoveAndIdentityKeepExactOpaqueCulture) {
    SystemAllocator allocator;
    {
        static_assert(!__is_constructible(CatalogSnapshot, const CatalogSnapshot&));
        CatalogSnapshot invalid;
        CHECK(!invalid.IsValid());
        CHECK(invalid.Culture().IsEmpty());
        CHECK(invalid.Revision() == 0);
        CHECK(invalid.EntryCount() == 0);
        auto original = CatalogSnapshot::Create(allocator, "eN-private", 9, {});
        REQUIRE(original);
        auto clone = original.Value().Clone();
        REQUIRE(clone);
        const StringView borrowed = original.Value().Culture();
        CatalogSnapshot moved(Move(original.Value()));
        CHECK(!original.Value().IsValid());
        moved.Reset();
        CHECK(borrowed == StringView("eN-private"));
        CHECK(clone.Value().Culture() == StringView("eN-private"));
        CHECK(clone.Value().Revision() == 9);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CatalogRejectsInvalidIdentityUtf8DuplicatesAndConfiguredLimits) {
    SystemAllocator allocator;
    {
        const CatalogEntry valid[] = {{{1, 1}, 1, "abc"}};
        CHECK(!CatalogSnapshot::Create(allocator, "", 1, {valid, 1}));
        CHECK(!CatalogSnapshot::Create(allocator, StringView("\xC0", 1), 1, {valid, 1}));
        CHECK(!CatalogSnapshot::Create(allocator, "en", 0, {valid, 1}));
        const CatalogEntry zeroId[] = {{{0, 0}, 1, "abc"}};
        CHECK(!CatalogSnapshot::Create(allocator, "en", 1, {zeroId, 1}));
        const CatalogEntry zeroRevision[] = {{{1, 1}, 0, "abc"}};
        CHECK(!CatalogSnapshot::Create(allocator, "en", 1, {zeroRevision, 1}));
        const CatalogEntry duplicate[] = {{{1, 1}, 1, "first"}, {{1, 1}, 2, "second"}};
        auto repeated = CatalogSnapshot::Create(allocator, "en", 1, {duplicate, 2});
        CHECK(!repeated);
        const CatalogEntry invalidUtf8[] = {{{1, 1}, 1, StringView("\xED\xA0\x80", 3)}};
        CHECK(!CatalogSnapshot::Create(allocator, "en", 1, {invalidUtf8, 1}));
        CatalogLimits limits;
        limits.maxEntries = 0;
        auto tooMany = CatalogSnapshot::Create(allocator, "en", 1, {valid, 1}, limits);
        CHECK(!tooMany && tooMany.Error() == ErrorCode::CapacityExceeded);
        limits = {};
        limits.maxTotalBytes = 5;
        auto exact = CatalogSnapshot::Create(allocator, "en", 1, {valid, 1}, limits);
        REQUIRE(exact);
        limits.maxTotalBytes = 4;
        auto tooLong = CatalogSnapshot::Create(allocator, "en", 1, {valid, 1}, limits);
        CHECK(!tooLong && tooLong.Error() == ErrorCode::CapacityExceeded);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CatalogPreservesEmptyAndEmbeddedNullTranslations) {
    SystemAllocator allocator;
    {
        const char embedded[] = {'a', '\0', 'b'};
        const CatalogEntry entries[] = {{{1, 1}, 1, ""}, {{1, 2}, 1, StringView(embedded, 3)}};
        auto catalog = CatalogSnapshot::Create(allocator, "en", 1, {entries, 2});
        auto emptyText = Text::Localized(allocator, {1, 1}, 1, "must not fall back");
        auto nullText = Text::Localized(allocator, {1, 2}, 1, "fallback");
        REQUIRE(catalog && emptyText && nullText);
        auto empty = ResolveText(allocator, emptyText.Value(), catalog.Value());
        auto bytes = ResolveText(allocator, nullText.Value(), catalog.Value());
        REQUIRE(empty && bytes);
        CHECK(empty.Value().value.IsEmpty());
        CHECK(empty.Value().diagnostics.missingTranslations == 0);
        CHECK(bytes.Value().value.View() == StringView(embedded, 3));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
