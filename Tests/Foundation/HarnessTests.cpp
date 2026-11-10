#include "Tests/Support/Test.h"
#include <Core/Diagnostics/ProfileScope.h>
#include <Core/Lifetime/ModuleLease.h>
#include <Core/Memory/SystemAllocator.h>

using namespace rocks;

TEST(Harness_SeedIsAccessible) {
    const uint64 first = test::CurrentSeed();
    CHECK(test::CurrentSeed() == first);
}

TEST(Diagnostics_BoundedCaptureReportsLoss) {
    LogBuffer buffer;
    for (usize i = 0; i < LogBuffer::Capacity; ++i) {
        REQUIRE(buffer.Write(LogLevel::Info, 7, "test").IsOk());
    }
    const auto overflow = buffer.Write(LogLevel::Info, 7, "dropped");
    CHECK(!overflow.IsOk());
    CHECK(overflow.Error() == ErrorCode::CapacityExceeded);
    CHECK(buffer.Size() == LogBuffer::Capacity);
    CHECK(buffer.Dropped() == 1);
    CHECK(buffer.At(LogBuffer::Capacity) == nullptr);
    REQUIRE(buffer.Reset().IsOk());
    CHECK(buffer.Size() == 0);
    CHECK(buffer.Dropped() == 0);
}

TEST(Diagnostics_ProfileAndTruncation) {
    LogBuffer buffer;
    {
        ProfileScope scope(buffer, "work", 2);
    }
    REQUIRE(buffer.Size() == 2);
    CHECK(buffer.At(0)->kind == DiagnosticKind::ScopeBegin);
    CHECK(buffer.At(1)->kind == DiagnosticKind::ScopeEnd);
    CHECK(buffer.At(1)->timestamp >= buffer.At(0)->timestamp);
    char longName[200];
    for (usize i = 0; i < sizeof(longName) - 1; ++i) {
        longName[i] = 'x';
    }
    longName[sizeof(longName) - 1] = '\0';
    REQUIRE(buffer.Write(LogLevel::Warning, 0, longName).IsOk());
    CHECK(buffer.At(2)->truncated);
    CHECK(buffer.At(2)->text[119] == '\0');
}

TEST(ModuleLease_CopyMoveReleaseExactlyOnce) {
    uint32 references = 0;
    auto result = ModuleLease::Acquire(
        &references, [](void* value) noexcept { ++*static_cast<uint32*>(value); },
        [](void* value) noexcept { --*static_cast<uint32*>(value); });
    REQUIRE(result.IsOk());
    CHECK(references == 1);
    ModuleLease first(static_cast<ModuleLease&&>(result.Value()));
    ModuleLease second(first);
    CHECK(references == 2);
    ModuleLease third(static_cast<ModuleLease&&>(second));
    CHECK(!second);
    third.Reset();
    CHECK(references == 1);
    first.Reset();
    CHECK(references == 0);
    CHECK(!ModuleLease::Acquire(nullptr, nullptr, nullptr).IsOk());
}
