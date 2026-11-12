#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Localization/Text.h>

using namespace rocks;

namespace {
// 입력/중간 결과/게시/clone/최종 문자열을 한 수명 범위에 두고 모든 할당 지점을 주입한다.
// 구현의 parser나 reference counter를 복제하지 않고 최종 golden과 외부 allocator만 관찰한다.
Result<void> RunTextLifecycle(Allocator& allocator) noexcept {
    const CatalogEntry entries[] = {{{1, 2}, 1, "{name}: {count} / {name}"}, {{3, 4}, 1, "Rocks"}};
    auto snapshot = CatalogSnapshot::Create(allocator, "ko-KR", 10, {entries, 2});
    if (!snapshot) {
        return Result<void>::Failure(snapshot.Error());
    }
    auto pattern = Text::Localized(allocator, {1, 2}, 1, "{name} {count}");
    if (!pattern) {
        return Result<void>::Failure(pattern.Error());
    }
    auto name = Text::Localized(allocator, {3, 4}, 1, "fallback");
    if (!name) {
        return Result<void>::Failure(name.Error());
    }
    auto count = Text::Integer(allocator, -42);
    if (!count) {
        return Result<void>::Failure(count.Error());
    }
    const TextArgument args[] = {{"name", &name.Value()}, {"count", &count.Value()}};
    auto formatted = Text::Format(allocator, pattern.Value(), {args, 2});
    if (!formatted) {
        return Result<void>::Failure(formatted.Error());
    }
    auto textClone = formatted.Value().Clone();
    if (!textClone) {
        return Result<void>::Failure(textClone.Error());
    }
    CatalogStore store;
    auto published = store.Publish(snapshot.Value());
    if (!published) {
        return published;
    }
    auto read = store.Acquire();
    if (!read) {
        return Result<void>::Failure(read.Error());
    }
    snapshot.Value().Reset();
    formatted.Value().Reset();
    pattern.Value().Reset();
    name.Value().Reset();
    count.Value().Reset();
    store.Close();
    auto output = ResolveText(allocator, textClone.Value(), read.Value());
    if (!output) {
        return Result<void>::Failure(output.Error());
    }
    if (output.Value().value.View() != StringView("Rocks: -42 / Rocks")) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    return Result<void>::Success();
}
} // namespace

TEST(TextAndCatalogEveryLifecycleAllocationFailureLeavesNoLeaks) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    REQUIRE(RunTextLifecycle(allocator));
    const uint64 attempts = allocator.Attempts();
    REQUIRE(attempts > 0);
    CHECK(backing.Stats().liveAllocations == 0);
    for (uint64 nth = 1; nth <= attempts; ++nth) {
        allocator.FailOnAttempt(nth);
        auto result = RunTextLifecycle(allocator);
        CHECK(!result && result.Error() == ErrorCode::OutOfMemory);
        CHECK(backing.Stats().liveAllocations == 0);
        CHECK(backing.Stats().liveBytes == 0);
    }
    allocator.FailOnAttempt(attempts + 1);
    CHECK(RunTextLifecycle(allocator));
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(TextResolveEachAllocationFailurePreservesReusableHistoryAndSnapshot) {
    SystemAllocator owner;
    SystemAllocator outputBacking;
    test::NFailAllocator output(outputBacking);
    {
        auto pattern = Text::Invariant(owner, "{a}-{b}-{a}");
        auto a = Text::Invariant(owner, "Alpha");
        auto b = Text::Integer(owner, 17);
        REQUIRE(pattern && a && b);
        const TextArgument args[] = {{"a", &a.Value()}, {"b", &b.Value()}};
        auto text = Text::Format(owner, pattern.Value(), {args, 2});
        auto catalog = CatalogSnapshot::Create(owner, "en", 1, {});
        REQUIRE(text && catalog);
        uint64 attempts;
        {
            auto result = ResolveText(output, text.Value(), catalog.Value());
            REQUIRE(result);
            CHECK(result.Value().value.View() == StringView("Alpha-17-Alpha"));
            attempts = output.Attempts();
        }
        REQUIRE(attempts > 0);
        const uint64 retained = owner.Stats().liveAllocations;
        for (uint64 nth = 1; nth <= attempts; ++nth) {
            output.FailOnAttempt(nth);
            auto failed = ResolveText(output, text.Value(), catalog.Value());
            CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
            CHECK(outputBacking.Stats().liveAllocations == 0);
            CHECK(owner.Stats().liveAllocations == retained);
            CHECK(catalog.Value().Culture() == StringView("en"));
            output.DisableFailure();
            {
                auto retry = ResolveText(output, text.Value(), catalog.Value());
                REQUIRE(retry);
                CHECK(retry.Value().value.View() == StringView("Alpha-17-Alpha"));
            }
            CHECK(outputBacking.Stats().liveAllocations == 0);
        }
    }
    CHECK(owner.Stats().liveAllocations == 0);
}

TEST(TextFormatAllocationFailureLeavesExistingArgumentsUsable) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        auto pattern = Text::Invariant(allocator, "{first}/{second}");
        auto a = Text::Invariant(allocator, "one");
        auto b = Text::Invariant(allocator, "two");
        REQUIRE(pattern && a && b);
        const TextArgument args[] = {{"first", &a.Value()}, {"second", &b.Value()}};
        allocator.DisableFailure();
        uint64 attempts;
        {
            auto probe = Text::Format(allocator, pattern.Value(), {args, 2});
            REQUIRE(probe);
            attempts = allocator.Attempts();
        }
        const uint64 baseline = backing.Stats().liveAllocations;
        for (uint64 nth = 1; nth <= attempts; ++nth) {
            allocator.FailOnAttempt(nth);
            auto failed = Text::Format(allocator, pattern.Value(), {args, 2});
            CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
            CHECK(backing.Stats().liveAllocations == baseline);
            allocator.DisableFailure();
            auto retry = Text::Format(allocator, pattern.Value(), {args, 2});
            REQUIRE(retry);
            auto output = ResolveText(allocator, retry.Value(), {});
            REQUIRE(output);
            CHECK(output.Value().value.View() == StringView("one/two"));
        }
    }
    CHECK(backing.Stats().liveAllocations == 0);
}
