#include <TestSupport/Test.h>
#include <Core/Diagnostics/LogSink.h>
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

// CPU 구간 계측은 TraceCapture CpuScope로 옮겼다(ProfileScope 삭제, TraceSystem 결정 T8).
TEST(Diagnostics_TruncationIsReported) {
    LogBuffer buffer;
    char longName[200];
    for (usize i = 0; i < sizeof(longName) - 1; ++i) {
        longName[i] = 'x';
    }
    longName[sizeof(longName) - 1] = '\0';
    REQUIRE(buffer.Write(LogLevel::Warning, 0, longName).IsOk());
    CHECK(buffer.At(0)->truncated);
    CHECK(buffer.At(0)->text[119] == '\0');
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
