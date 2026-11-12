#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Localization/Text.h>

using namespace rocks;

TEST(TextPositiveWorkBudgetCountsNodesSourceOutputAndCatalogCandidate) {
    SystemAllocator allocator;
    {
        auto invariant = Text::Invariant(allocator, "x");
        auto localized = Text::Localized(allocator, {1, 1}, 1, "fallback");
        const CatalogEntry entries[] = {{{1, 1}, 1, "x"}};
        auto catalog = CatalogSnapshot::Create(allocator, "en", 1, {entries, 1});
        REQUIRE(invariant && localized && catalog);
        TextResolveLimits limits;
        // 공개 계약: node 1 + source byte 1 + output byte 1 = 3.
        limits.maxWork = 2;
        auto shortLiteral = ResolveText(allocator, invariant.Value(), {}, limits);
        CHECK(!shortLiteral && shortLiteral.Error() == ErrorCode::CapacityExceeded);
        limits.maxWork = 3;
        auto exactLiteral = ResolveText(allocator, invariant.Value(), {}, limits);
        REQUIRE(exactLiteral);
        CHECK(exactLiteral.Value().value.View() == StringView("x"));
        // matching catalog 후보 1개 방문을 더하면 4다. 0만 특별 취급하는 구현은 실패한다.
        auto shortLocalized = ResolveText(allocator, localized.Value(), catalog.Value(), limits);
        CHECK(!shortLocalized && shortLocalized.Error() == ErrorCode::CapacityExceeded);
        limits.maxWork = 4;
        auto exactLocalized = ResolveText(allocator, localized.Value(), catalog.Value(), limits);
        REQUIRE(exactLocalized);
        CHECK(exactLocalized.Value().value.View() == StringView("x"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(TextPositiveWorkBudgetBoundsFormatResolution) {
    SystemAllocator allocator;
    {
        auto pattern = Text::Invariant(allocator, "{last}{last}{last}{last}");
        auto value = Text::Invariant(allocator, "x");
        REQUIRE(pattern && value);
        const TextArgument searched[] = {{"aaaa", &value.Value()},
                                         {"bbbb", &value.Value()},
                                         {"cccc", &value.Value()},
                                         {"last", &value.Value()}};
        auto withSearch = Text::Format(allocator, pattern.Value(), {searched, 4});
        REQUIRE(withSearch);
        TextResolveLimits limits;
        // 선형/색인 검색 구현을 고정하지 않는다. 원문조차 검사할 수 없는 양수 예산과
        // 충분한 예산에서의 고정 출력을 대조하여 0 전용 거부만 구현한 회귀를 검출한다.
        limits.maxWork = 8;
        auto shortBudget = ResolveText(allocator, withSearch.Value(), {}, limits);
        CHECK(!shortBudget && shortBudget.Error() == ErrorCode::CapacityExceeded);
        limits.maxWork = 1000;
        auto ampleBudget = ResolveText(allocator, withSearch.Value(), {}, limits);
        REQUIRE(ampleBudget);
        CHECK(ampleBudget.Value().value.View() == StringView("xxxx"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
