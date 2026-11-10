#include <Tests/Localization/NativeWorkers.h>
#include <Tests/Support/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Localization/Text.h>

using namespace rocks;
using namespace rocks::test::localization;

namespace {
struct SharedCatalog {
    explicit SharedCatalog(SystemAllocator& backing) noexcept : allocator(backing) {
    }

    SystemAllocator& allocator;
    CatalogStore store;
    platform::Mutex start;
    Text text;
    CatalogSnapshot english;
    CatalogSnapshot korean;
};

struct ReaderContext {
    SharedCatalog* shared = nullptr;
    CatalogSnapshot retained;
    Text text;
    uint64 oldReads = 0;
    uint64 currentReads = 0;
};

void ReadDuringPublication(void* raw) noexcept {
    auto& context = *static_cast<ReaderContext*>(raw);
    SharedCatalog& shared = *context.shared;
    {
        platform::LockGuard wait(shared.start);
    }
    for (usize iteration = 0; iteration < 200; ++iteration) {
        // 공개 순서와 무관한 고정 golden: 시작 전에 보존한 en snapshot은 절대 변하지 않는다.
        auto oldText = ResolveText(shared.allocator, context.text, context.retained);
        REQUIRE(oldText);
        CHECK(oldText.Value().value.View() == StringView("Hello Rocks"));
        CHECK(context.retained.Culture() == StringView("en"));
        CHECK(context.retained.Revision() == 101);
        ++context.oldReads;
        auto current = shared.store.Acquire();
        if (!current) {
            CHECK(current.Error() == ErrorCode::ShuttingDown);
            continue;
        }
        auto result = ResolveText(shared.allocator, context.text, current.Value());
        REQUIRE(result);
        // 동시에 게시된 하나의 tuple이어야 한다. 서로 다른 snapshot의 필드 혼합은 허용하지 않는다.
        if (current.Value().Revision() == 101) {
            CHECK(current.Value().Culture() == StringView("en"));
            CHECK(result.Value().value.View() == StringView("Hello Rocks"));
        } else {
            CHECK(current.Value().Revision() == 202);
            CHECK(current.Value().Culture() == StringView("ko"));
            CHECK(result.Value().value.View() == StringView("\xED\x95\x9C\xEA\xB8\x80 Rocks"));
        }
        CHECK(result.Value().diagnostics.missingTranslations == 0);
        CHECK(result.Value().diagnostics.staleTranslations == 0);
        ++context.currentReads;
        auto clone = current.Value().Clone();
        REQUIRE(clone);
        current.Value().Reset();
        CHECK(clone.Value().IsValid());
    }
    // 원 생성 스레드와 다른 스레드에서 보존분을 반환한다.
    context.retained.Reset();
    context.text.Reset();
}

void PublishLanguages(void* raw) noexcept {
    auto& shared = *static_cast<SharedCatalog*>(raw);
    {
        platform::LockGuard wait(shared.start);
    }
    for (usize iteration = 0; iteration < 200; ++iteration) {
        CHECK(shared.store.Publish(iteration % 2 == 0 ? shared.korean : shared.english));
        if (iteration % 10 == 0) {
            YieldNativeWorker();
        }
    }
    shared.store.Close();
}
} // namespace

TEST(TextNativeReadersKeepCoherentSnapshotsDuringLanguagePublicationAndClose) {
    SystemAllocator allocator;
    {
        SharedCatalog shared(allocator);
        auto pattern = Text::Localized(shared.allocator, {1, 1}, 1, "fallback {name}");
        auto name = Text::Invariant(shared.allocator, "Rocks");
        REQUIRE(pattern && name);
        const TextArgument args[] = {{"name", &name.Value()}};
        auto text = Text::Format(shared.allocator, pattern.Value(), {args, 1});
        REQUIRE(text);
        shared.text = Move(text.Value());
        const CatalogEntry en[] = {{{1, 1}, 1, "Hello {name}"}};
        const CatalogEntry ko[] = {{{1, 1}, 1, "\xED\x95\x9C\xEA\xB8\x80 {name}"}};
        auto english = CatalogSnapshot::Create(shared.allocator, "en", 101, {en, 1});
        auto korean = CatalogSnapshot::Create(shared.allocator, "ko", 202, {ko, 1});
        REQUIRE(english && korean);
        shared.english = Move(english.Value());
        shared.korean = Move(korean.Value());
        REQUIRE(shared.store.Publish(shared.english));
        ReaderContext readers[3];
        for (auto& reader : readers) {
            reader.shared = &shared;
            auto snapshot = shared.english.Clone();
            auto textClone = shared.text.Clone();
            REQUIRE(snapshot && textClone);
            reader.retained = Move(snapshot.Value());
            reader.text = Move(textClone.Value());
        }
        NativeWorkers workers;
        bool started = true;
        {
            platform::LockGuard gate(shared.start);
            for (auto& reader : readers) {
                started = workers.Start(ReadDuringPublication, &reader) && started;
            }
            started = workers.Start(PublishLanguages, &shared) && started;
        }
        workers.JoinAll();
        REQUIRE(started);
        for (const auto& reader : readers) {
            CHECK(reader.oldReads == 200);
        }
        shared.store.Close();
        CHECK(!shared.store.Acquire());
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
