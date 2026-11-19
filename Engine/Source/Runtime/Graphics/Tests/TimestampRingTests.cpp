#include <Core/Memory/SystemAllocator.h>
#include <Graphics/TimestampRing.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace timestamp_ring_tests_file {

using namespace rocks::graphics;

namespace {

// pool 칸 번호 × 10을 tick으로 내는 원본이다. 읽은 횟수와 다음 응답을 시험이 정한다.
class FakeSource final : public TimestampSource {
public:
    TimestampReadState ReadTimestamps(uint32 first, uint32 count, uint64* ticks) noexcept override {
        ++reads;
        for (uint32 index = 0; index < count; ++index) {
            ticks[index] = uint64{first + index} * 10;
        }

        return state;
    }

    TimestampReadState state = TimestampReadState::Ready;
    uint32 reads = 0;
};

} // namespace

TEST(TimestampRing_ValuesAppearOnlyAfterTheirFence) {
    SystemAllocator allocator;
    TimestampRing ring(allocator);
    FakeSource source;
    CHECK(ring.Allocate().Error() == ErrorCode::Unsupported);
    REQUIRE(ring.Reset(2, 4));
    auto first = ring.Allocate();
    auto second = ring.Allocate();
    REQUIRE(first && second);
    CHECK(first.Value().query == 1 && first.Value().index == 0);
    CHECK(second.Value().query == 2 && second.Value().index == 1);
    const TimestampRange range = ring.OpenRange();
    CHECK(range.first == 0 && range.count == 2);
    ring.Close(5, true);
    CHECK(ring.OpenRange().count == 0);

    // fence 5가 끝나기 전에는 원본을 읽지도 않는다(fence gate).
    TimestampSample samples[4];
    ring.Harvest(4, source);
    CHECK(source.reads == 0);
    CHECK(ring.Collect(Span<TimestampSample>(samples)) == 0);

    // 끝났지만 결과가 아직 보이지 않으면 slot을 남기고 다음에 다시 묻는다.
    source.state = TimestampReadState::NotReady;
    ring.Harvest(5, source);
    CHECK(ring.Ready() == 0);
    source.state = TimestampReadState::Ready;
    ring.Harvest(5, source);
    REQUIRE(ring.Collect(Span<TimestampSample>(samples)) == 2);
    CHECK(samples[0].query == 1 && samples[0].ticks == 0 && !samples[0].disjoint);
    CHECK(samples[1].query == 2 && samples[1].ticks == 10 && !samples[1].disjoint);
}

TEST(TimestampRing_BusyWhenEverySlotIsInFlight) {
    SystemAllocator allocator;
    TimestampRing ring(allocator);
    FakeSource source;
    REQUIRE(ring.Reset(2, 2));
    REQUIRE(ring.Allocate());
    ring.Close(1, true);
    auto second = ring.Allocate();
    REQUIRE(second);
    CHECK(second.Value().index == 2);
    REQUIRE(ring.Allocate());
    // slot당 한도를 넘으면 CapacityExceeded다.
    CHECK(ring.Allocate().Error() == ErrorCode::CapacityExceeded);
    ring.Close(2, true);
    // 두 slot이 모두 GPU 완료 전이다. 기다리지 않고 Busy다.
    CHECK(ring.Allocate().Error() == ErrorCode::Busy);
    ring.Harvest(1, source);
    auto third = ring.Allocate();
    REQUIRE(third);
    CHECK(third.Value().index == 0 && third.Value().query == 4);
    ring.Close(3, true);
    ring.Harvest(3, source);
    // 쓴 순서대로 나오며 out이 작으면 나머지는 다음 호출로 간다.
    TimestampSample samples[3];
    REQUIRE(ring.Collect(Span<TimestampSample>(samples, 2)) == 2);
    CHECK(samples[0].query == 1 && samples[1].query == 2);
    REQUIRE(ring.Collect(Span<TimestampSample>(samples)) == 2);
    CHECK(samples[0].query == 3 && samples[1].query == 4);
    CHECK(ring.Dropped() == 0);
}

TEST(TimestampRing_DisjointAndFailedSubmissionsAreMarkedInvalid) {
    SystemAllocator allocator;
    TimestampRing ring(allocator);
    FakeSource source;
    REQUIRE(ring.Reset(4, 4));
    REQUIRE(ring.Allocate());
    ring.Close(1, true);
    REQUIRE(ring.Allocate());
    // 제출 실패: 원본을 읽지 않고 무효로 낸다.
    ring.Close(1, false);
    source.state = TimestampReadState::Disjoint;
    ring.Harvest(1, source);
    CHECK(source.reads == 1);
    TimestampSample samples[2];
    REQUIRE(ring.Collect(Span<TimestampSample>(samples)) == 2);
    CHECK(samples[0].disjoint && samples[0].ticks == 0);
    CHECK(samples[1].disjoint && samples[1].ticks == 0);
}

TEST(TimestampRing_CalibrationMapsGpuTicksToCpuTime) {
    ClockCalibration calibration;
    calibration.gpuTicks = 1000;
    calibration.cpuNanoseconds = 5000000;
    calibration.gpuFrequency = 1000000; // 1 tick = 1000 ns
    CHECK(GpuTicksToCpuNanoseconds(calibration, 1000) == 5000000);
    CHECK(GpuTicksToCpuNanoseconds(calibration, 1500) == 5500000);
    CHECK(GpuTicksToCpuNanoseconds(calibration, 900) == 4900000);
    CHECK(GpuTicksToCpuNanoseconds(calibration, 0) == 4000000);
    // 기준보다 한참 앞이면 0 아래로 내려가지 않는다.
    calibration.cpuNanoseconds = 10;
    CHECK(GpuTicksToCpuNanoseconds(calibration, 0) == 0);
    // 큰 tick도 중간 곱 overflow 없이 바꾼다(10 GHz, 약 58년).
    calibration.gpuTicks = 0;
    calibration.cpuNanoseconds = 0;
    calibration.gpuFrequency = 10000000000ull;
    CHECK(GpuTicksToCpuNanoseconds(calibration, 18000000000000000000ull) == 1800000000000000000ull);
    calibration.gpuFrequency = 0;
    CHECK(GpuTicksToCpuNanoseconds(calibration, 77) == 0);
}
} // namespace timestamp_ring_tests_file
