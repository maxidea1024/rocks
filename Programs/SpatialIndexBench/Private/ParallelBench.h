#pragma once

#include "BenchInput.h"
#include "BenchSupport.h"
#include "Harness.h"
#include "LooseOctreeAdapter.h"

#include <Core/Memory/SystemAllocator.h>
#include <Core/Math/RandomStream.h>
#include <Task/ParallelFor.h>
#include <Task/WorkerScheduler.h>

#include <stdio.h>

// 병렬 검색 측정(Docs/Design/SpatialIndex.ko.md 10절 OT6, 9.6절 노드 묶음 기준)이다. 범위 분할 병렬
// 실행은 Core Task의 ParallelFor(SIP5)를 쓴다. 참여자 P(호출 스레드 1 + helper P − 1)마다 같은 일을
// 하고, 확장 효율 = 단일 스레드 시간 / (P × P 참여자 시간)이다. 결과가 단일 스레드와 같은지(개수와
// 식별 값 합)를 측정 전에 대조한다.
namespace rocks::spatialbench {

inline constexpr uint32 ParallelWorkerCount = 7;
inline constexpr uint32 ParallelRounds = 15;
inline constexpr uint32 ParallelCasterCount = 8;
inline constexpr uint32 ParallelLightCount = 64;
inline constexpr usize ParallelBatchCapacity = usize{1} << 18;

// 결과 하나의 요약이다(개수와 식별 값 합).
struct ParallelTally {
    uint64 count = 0;
    uint64 checksum = 0;
};

// 프로세스에 하나인 worker scheduler다. 할당기는 여러 스레드에서 쓰므로 잠금 기반
// SystemAllocator다.
struct ParallelRuntime {
    SystemAllocator allocator;
    WorkerScheduler scheduler{allocator};
    bool started = false;

    bool Start() noexcept {
        if (started) {
            return true;
        }
        WorkerSchedulerConfig config;
        config.workerCount = ParallelWorkerCount;
        started = static_cast<bool>(scheduler.Start(config));
        return started;
    }

    ~ParallelRuntime() noexcept {
        if (started) {
            (void)scheduler.Shutdown(WorkerShutdownMode::Drain);
        }
    }
};

inline ParallelRuntime& Runtime() noexcept {
    static ParallelRuntime runtime;
    return runtime;
}

inline double MedianOf(Array<uint64>& values) noexcept {
    algo::Sort(values.View());
    return values.Size() == 0 ? 0.0 : static_cast<double>(values[(values.Size() - 1) / 2]);
}

// 노드 묶음 병렬 수집 한 번이다: CollectNodeBatches(단일 스레드) → 묶음을 ParallelFor로 나눠
// QueryBatch. 묶음마다 결과 칸이 따로라 원자 연산 없이 모은다.
inline bool CollectCasterParallel(const spatial::LooseOctreeReader& reader,
                                  const ConvexVolume<double>& volume, uint32 participants,
                                  Span<spatial::SpatialNodeBatch> batches,
                                  Span<ParallelTally> slots, ParallelTally& total) noexcept {
    auto collected = reader.CollectNodeBatches(volume, batches);
    if (!collected || collected.Value().truncated) {
        return false;
    }
    const usize count = collected.Value().written;
    ParallelForOptions options;
    options.maxHelpers = participants - 1;
    auto done = ParallelFor(
        Runtime().scheduler, Runtime().allocator, count,
        [&](usize begin, usize end) noexcept {
            for (usize index = begin; index < end; ++index) {
                ParallelTally tally;
                (void)reader.QueryBatch(batches[index], volume, QueryFilterMask,
                                        [&](const spatial::SpatialProxyHit& hit) noexcept {
                                            ++tally.count;
                                            tally.checksum += hit.userValue;
                                            return spatial::QueryControl::Continue;
                                        });
                slots[index] = tally;
            }
        },
        options);
    if (!done) {
        return false;
    }
    total = ParallelTally{};
    for (usize index = 0; index < count; ++index) {
        total.count += slots[index].count;
        total.checksum += slots[index].checksum;
    }
    return true;
}

// 광원 구 질의 묶음이다. 광원마다 결과 칸이 따로다.
inline bool QueryLightsParallel(const spatial::LooseOctreeReader& reader,
                                Span<const Sphere<double>> lights, uint32 participants,
                                Span<ParallelTally> slots, ParallelTally& total) noexcept {
    ParallelForOptions options;
    options.maxHelpers = participants - 1;
    auto done = ParallelFor(
        Runtime().scheduler, Runtime().allocator, lights.Size(),
        [&](usize begin, usize end) noexcept {
            for (usize index = begin; index < end; ++index) {
                ParallelTally tally;
                (void)reader.QuerySphere(lights[index], QueryFilterMask,
                                         [&](const spatial::SpatialProxyHit& hit) noexcept {
                                             ++tally.count;
                                             tally.checksum += hit.userValue;
                                             return spatial::QueryControl::Continue;
                                         });
                slots[index] = tally;
            }
        },
        options);
    if (!done) {
        return false;
    }
    total = ParallelTally{};
    for (usize index = 0; index < lights.Size(); ++index) {
        total.count += slots[index].count;
        total.checksum += slots[index].checksum;
    }
    return true;
}

// 사례 하나의 병렬 측정이다. 구축 → 대조 → 참여자 1·2·4·8마다 caster 수집과 광원 질의를 잰다.
inline void RunParallelCase(LooseIndex& index, const CaseInput& input, BenchAllocator* allocator,
                            JsonWriter& json) noexcept {
    const Dataset& dataset = *input.dataset;
    json.BeginObject();
    json.Field("distribution", DistributionName(input.distribution));
    json.FieldInteger("size", input.size);
    printf("  parallel %s n=%u\n", DistributionName(input.distribution), input.size);
    fflush(stdout);
    if (!Runtime().Start() || !index.Create(input.size)) {
        json.Field("error", "setup failed");
        json.EndObject();
        return;
    }
    for (uint32 id = 0; id < input.size; ++id) {
        (void)index.Insert(id, dataset.bounds[id], dataset.filters[id]);
    }
    index.MaintainAll();

    // caster 프러스텀(프레임 부하와 같은 식: 영역 안 무작위 꼭짓점, 최대 변 1/4 깊이)과 광원 구.
    RandomStream random(input.seed, 0x7a11ull);
    const Box<double>& domain = dataset.domain;
    const double largest = (domain.max.x - domain.min.x) > (domain.max.y - domain.min.y)
                               ? (domain.max.x - domain.min.x)
                               : (domain.max.y - domain.min.y);
    Array<ConvexVolume<double>> casters(*allocator);
    for (uint32 caster = 0; caster < ParallelCasterCount; ++caster) {
        const Vec3d apex(domain.min.x + (domain.max.x - domain.min.x) * random.NextDouble01(),
                         domain.min.y + (domain.max.y - domain.min.y) * random.NextDouble01(),
                         domain.min.z + (domain.max.z - domain.min.z) * random.NextDouble01());
        Vec3d direction(random.NextDouble01() - 0.5, random.NextDouble01() - 0.5,
                        random.NextDouble01() - 0.5);
        direction = direction.NormalizedOr(Vec3d(0.0, 0.0, -1.0));
        (void)casters.TryPushBack(MakeFrustum(apex, direction, 1.0, largest * 0.5));
    }
    Array<Sphere<double>> lights(*allocator);
    for (uint32 light = 0; light < ParallelLightCount; ++light) {
        const Box<double>& item = dataset.bounds[random.NextUInt32Below(input.size)];
        const double radius = platform::PlatformMath::Exp(
            platform::PlatformMath::Log(300.0) +
            (platform::PlatformMath::Log(3000.0) - platform::PlatformMath::Log(300.0)) *
                random.NextDouble01());
        (void)lights.TryPushBack(Sphere<double>(item.Center(), Quantize(radius)));
    }

    Array<spatial::SpatialNodeBatch> batches(*allocator);
    Array<ParallelTally> slots(*allocator);
    if (!batches.TryResize(ParallelBatchCapacity) || !slots.TryResize(ParallelBatchCapacity)) {
        json.Field("error", "buffer allocation failed");
        json.EndObject();
        return;
    }
    index.BeginRead();
    const spatial::LooseOctreeReader reader = index.Reader();

    // 단일 스레드 기준값과 결과 대조.
    ParallelTally casterReference[ParallelCasterCount];
    uint64 batchCounts[ParallelCasterCount] = {};
    for (uint32 caster = 0; caster < ParallelCasterCount; ++caster) {
        (void)reader.QueryFrustum(casters[caster], QueryFilterMask,
                                  [&](const spatial::SpatialProxyHit& hit) noexcept {
                                      ++casterReference[caster].count;
                                      casterReference[caster].checksum += hit.userValue;
                                      return spatial::QueryControl::Continue;
                                  });
        auto collected = reader.CollectNodeBatches(casters[caster], batches.View());
        batchCounts[caster] = collected ? collected.Value().total : 0;
    }
    ParallelTally lightReference;
    for (uint32 light = 0; light < ParallelLightCount; ++light) {
        (void)reader.QuerySphere(lights[light], QueryFilterMask,
                                 [&](const spatial::SpatialProxyHit& hit) noexcept {
                                     ++lightReference.count;
                                     lightReference.checksum += hit.userValue;
                                     return spatial::QueryControl::Continue;
                                 });
    }
    bool same = true;
    constexpr uint32 Participants[] = {1, 2, 4, 8};
    for (const uint32 participants : Participants) {
        for (uint32 caster = 0; caster < ParallelCasterCount; ++caster) {
            ParallelTally tally;
            same = same &&
                   CollectCasterParallel(reader, casters[caster], participants, batches.View(),
                                         slots.View(), tally) &&
                   tally.count == casterReference[caster].count &&
                   tally.checksum == casterReference[caster].checksum;
        }
        ParallelTally tally;
        same = same &&
               QueryLightsParallel(reader, lights.View(), participants, slots.View(), tally) &&
               tally.count == lightReference.count && tally.checksum == lightReference.checksum;
    }
    json.FieldBool("resultsMatchSingleThread", same);
    uint64 casterResults = 0;
    uint64 batchTotal = 0;
    for (uint32 caster = 0; caster < ParallelCasterCount; ++caster) {
        casterResults += casterReference[caster].count;
        batchTotal += batchCounts[caster];
    }
    json.FieldInteger("casterResultsPerQuery", casterResults / ParallelCasterCount);
    json.FieldInteger("casterBatchesPerQuery", batchTotal / ParallelCasterCount);
    json.FieldInteger("lightResultsPerFrame", lightReference.count);

    // 시간: 라운드마다 참여자 순서를 돌려 잰다(중앙값). 단일 질의 기준선은 QueryFrustum 한 번,
    // 병렬은 CollectNodeBatches + ParallelFor(QueryBatch)다. 광원은 64개를 한 묶음으로 잰다.
    Array<uint64> singleCaster(*allocator);
    Array<uint64> singleLights(*allocator);
    Array<uint64> casterTimes[4] = {Array<uint64>(*allocator), Array<uint64>(*allocator),
                                    Array<uint64>(*allocator), Array<uint64>(*allocator)};
    Array<uint64> lightTimes[4] = {Array<uint64>(*allocator), Array<uint64>(*allocator),
                                   Array<uint64>(*allocator), Array<uint64>(*allocator)};
    Stopwatch watch;
    for (uint32 round = 0; round < ParallelRounds; ++round) {
        watch.Start();
        uint64 sink = 0;
        for (uint32 caster = 0; caster < ParallelCasterCount; ++caster) {
            (void)reader.QueryFrustum(casters[caster], QueryFilterMask,
                                      [&](const spatial::SpatialProxyHit& hit) noexcept {
                                          sink += hit.userValue;
                                          return spatial::QueryControl::Continue;
                                      });
        }
        (void)singleCaster.TryPushBack(watch.Elapsed() / ParallelCasterCount);
        watch.Start();
        for (uint32 light = 0; light < ParallelLightCount; ++light) {
            (void)reader.QuerySphere(lights[light], QueryFilterMask,
                                     [&](const spatial::SpatialProxyHit& hit) noexcept {
                                         sink += hit.userValue;
                                         return spatial::QueryControl::Continue;
                                     });
        }
        (void)singleLights.TryPushBack(watch.Elapsed());
        for (uint32 step = 0; step < 4; ++step) {
            const uint32 slot = (step + round) % 4;
            const uint32 participants = Participants[slot];
            ParallelTally tally;
            watch.Start();
            for (uint32 caster = 0; caster < ParallelCasterCount; ++caster) {
                (void)CollectCasterParallel(reader, casters[caster], participants, batches.View(),
                                            slots.View(), tally);
            }
            (void)casterTimes[slot].TryPushBack(watch.Elapsed() / ParallelCasterCount);
            watch.Start();
            (void)QueryLightsParallel(reader, lights.View(), participants, slots.View(), tally);
            (void)lightTimes[slot].TryPushBack(watch.Elapsed());
        }
        if (sink == 1) {
            printf(" ");
        }
    }
    index.EndRead();

    const double singleCasterNs = MedianOf(singleCaster);
    const double singleLightsNs = MedianOf(singleLights);
    json.Field("singleCasterMedianNs", singleCasterNs);
    json.Field("singleLightsMedianNs", singleLightsNs);
    json.FieldInteger("rounds", ParallelRounds);
    json.Key("participants");
    json.BeginArray();
    for (uint32 slot = 0; slot < 4; ++slot) {
        const double casterNs = MedianOf(casterTimes[slot]);
        const double lightNs = MedianOf(lightTimes[slot]);
        json.BeginObject();
        json.FieldInteger("participants", Participants[slot]);
        json.Field("casterMedianNs", casterNs);
        json.Field("casterEfficiency",
                   casterNs > 0.0 ? singleCasterNs / (Participants[slot] * casterNs) : 0.0);
        json.Field("lightsMedianNs", lightNs);
        json.Field("lightsEfficiency",
                   lightNs > 0.0 ? singleLightsNs / (Participants[slot] * lightNs) : 0.0);
        json.EndObject();
    }
    json.EndArray();
    index.Destroy();
    json.EndObject();
}

} // namespace rocks::spatialbench
