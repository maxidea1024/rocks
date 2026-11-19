#pragma once

#include "BenchInput.h"
#include "BenchSupport.h"
#include "FlatScan.h"
#include "Harness.h"
#include "LooseOctreeAdapter.h"
#include "ParallelBench.h"

#include <Core/Math/Intersection.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include <SpatialIndex/FlatBoundsSet.h>

#include <stdio.h>

// FlatBoundsSet 측정(Docs/Design/FlatBounds.ko.md 10절 FB2, 계획 10.4절 SI01)이다. 같은 입력(분포·
// 크기·시드)으로 FlatBoundsSet과 A1(scalar double 전수)·A2(float SoA 전수, 프러스텀은 double 술어)·
// D(LooseOctree 프러스텀 질의), UE 방식 변형(원소 하나 대 평면 4개 SIMD)을 잰다.
//
// 뷰는 메인 프러스텀 1개 + cascade 3개(카메라 둘레 축 정렬 상자)다. 원소의 1/4에 그리기 거리 상한,
// 1/8에 하한을 준다. 정답은 6.1절 술어의 double 전수 계산이고, 측정 전에 누락 0과 초과 비율을
// 확인한다. 시간은 라운드 중앙값이다. 뷰당 원소 판정 시간 = 질의 시간 / (원소 수 × 뷰 수), 확장
// 효율 = 단독 시간 / (P × P 참여자 시간)이다.
namespace rocks::spatialbench {

inline constexpr uint32 FlatRounds = 15;
inline constexpr uint32 FlatBaselineRounds = 5;
inline constexpr uint32 FlatViewCount = 4;

inline ConvexVolume<double> AxisBoxVolume(const Vec3d& center, double half) noexcept {
    ConvexVolume<double> volume;
    volume.planes[0] = Plane<double>(Vec3d(1, 0, 0), center.x - half);
    volume.planes[1] = Plane<double>(Vec3d(-1, 0, 0), -(center.x + half));
    volume.planes[2] = Plane<double>(Vec3d(0, 1, 0), center.y - half);
    volume.planes[3] = Plane<double>(Vec3d(0, -1, 0), -(center.y + half));
    volume.planes[4] = Plane<double>(Vec3d(0, 0, 1), center.z - half);
    volume.planes[5] = Plane<double>(Vec3d(0, 0, -1), -(center.z + half));
    volume.planeCount = 6;
    return volume;
}

inline double LargestSide(const Box<double>& domain) noexcept {
    const Vec3d size = domain.max - domain.min;
    return math::Max(size.x, math::Max(size.y, size.z));
}

// 메인 프러스텀(영역 중심, 최대 변 1/2 깊이)과 cascade 3개(한 변 1/8·1/3·1 크기의 상자)다.
inline void MakeFlatViews(const Dataset& dataset,
                          spatial::FlatBoundsView (&views)[FlatViewCount]) noexcept {
    const Box<double>& domain = dataset.domain;
    const Vec3d apex = domain.Center();
    const double largest = LargestSide(domain);
    const Vec3d forward = Vec3d(1.0, 0.35, -0.12).NormalizedOr(Vec3d(1.0, 0.0, 0.0));
    views[0].volume = MakeFrustum(apex, forward, 10.0, largest * 0.5);
    views[1].volume = AxisBoxVolume(apex, largest / 16.0);
    views[2].volume = AxisBoxVolume(apex, largest / 6.0);
    views[3].volume = AxisBoxVolume(apex, largest / 2.0);
    for (spatial::FlatBoundsView& view : views) {
        view.distanceOrigin = apex;
        view.distanceScale = 1.0f;
        view.filterMask = QueryFilterMask;
    }
}

// 원소의 그리기 거리다(1/4 상한, 1/8 하한).
inline void DrawRange(uint32 id, double largest, float& minDraw, float& maxDraw) noexcept {
    minDraw = 0.0f;
    maxDraw = 0.0f;
    const uint64 pick = Mix64(id ^ 0xD15ull) % 8;
    if (pick < 2) {
        maxDraw = static_cast<float>(largest * 0.25);
    } else if (pick == 2) {
        minDraw = static_cast<float>(largest * 0.05);
    }
}

// 정답 술어(6.1절)다.
inline bool FlatReference(const Box<double>& bounds, uint32 filter, float minDraw, float maxDraw,
                          const spatial::FlatBoundsView& view) noexcept {
    if ((filter & view.filterMask) == 0 || Overlap(view.volume, bounds) != QueryResult::Hit) {
        return false;
    }
    const Vec3d& origin = view.distanceOrigin;
    if (maxDraw > 0.0f) {
        const double limit = static_cast<double>(maxDraw) * view.distanceScale;
        const auto nearest = TryDistanceSquared(bounds, origin);
        if (!nearest || nearest.Value() > limit * limit) {
            return false;
        }
    }
    if (minDraw > 0.0f) {
        const double limit = static_cast<double>(minDraw) * view.distanceScale;
        double farthest = 0.0;
        for (uint32 axis = 0; axis < 3; ++axis) {
            const double reach = math::Max(math::Abs(origin[axis] - bounds.min[axis]),
                                           math::Abs(origin[axis] - bounds.max[axis]));
            farthest += reach * reach;
        }
        if (farthest < limit * limit) {
            return false;
        }
    }
    return true;
}

// 한 구성(page 용량·큰 원소 기준·입력 경계)으로 만든 집합과 측정 도구다.
class FlatSubject {
public:
    FlatSubject(Allocator& allocator, const spatial::FlatBoundsConfig& config) noexcept
        : set_(Create(config, allocator)), slots_(allocator), words_(allocator) {
    }

    bool Build(const Dataset& dataset, Span<const Box<double>> bounds, uint64& buildNs) noexcept {
        const double largest = LargestSide(dataset.domain);
        if (!slots_.TryResize(bounds.Size()) ||
            !set_.TryReserve(static_cast<uint32>(bounds.Size()))) {
            return false;
        }
        Stopwatch watch;
        watch.Start();
        for (uint32 id = 0; id < bounds.Size(); ++id) {
            spatial::FlatBoundsDesc desc;
            desc.bounds = bounds[id];
            desc.filterMask = dataset.filters[id];
            DrawRange(id, largest, desc.minDrawDistance, desc.maxDrawDistance);
            desc.userValue = id;
            spatial::FlatBoundsMoves moves;
            if (!set_.TryAdd(desc, &moves)) {
                return false;
            }
            slots_[id] = moves.moves[0].newSlot;
        }
        buildNs = watch.Elapsed();
        return true;
    }

    spatial::FlatBoundsSet& Set() noexcept {
        return set_;
    }

    // 뷰 count개의 출력 저장소를 잡는다.
    bool Prepare(uint32 wordCount, uint32 count) noexcept {
        wordCount_ = wordCount;
        if (!words_.TryResize(static_cast<usize>(wordCount) * count)) {
            return false;
        }
        for (uint32 index = 0; index < count; ++index) {
            out_[index].words =
                Span<uint64>(words_.Data() + static_cast<usize>(index) * wordCount, wordCount);
        }
        return true;
    }

    bool Query(const spatial::FlatBoundsReader& reader, const spatial::FlatBoundsView* views,
               uint32 count, uint32 participants,
               spatial::FlatQueryStats* stats = nullptr) noexcept {
        return static_cast<bool>(
            reader.QueryViews(Span<const spatial::FlatBoundsView>(views, count),
                              Span<spatial::FlatVisibility>(out_, count),
                              participants > 1 ? &Runtime().scheduler : nullptr,
                              Runtime().allocator, stats, participants - 1));
    }

    bool Bit(uint32 view, uint32 id, uint32 listWordBase) const noexcept {
        const uint32 slot = slots_[id];
        const uint32 bit = spatial::IsFlatBoundsListSlot(slot)
                               ? listWordBase * spatial::FlatVisibilityBitsPerWord +
                                     (slot - spatial::FlatBoundsListSlotBase)
                               : slot;
        return ((out_[view].words[bit / spatial::FlatVisibilityBitsPerWord] >>
                 (bit % spatial::FlatVisibilityBitsPerWord)) &
                1u) != 0;
    }

private:
    static spatial::FlatBoundsSet Create(const spatial::FlatBoundsConfig& config,
                                         Allocator& allocator) noexcept {
        auto created = spatial::FlatBoundsSet::TryCreate(config, allocator);
        if (!created) {
            platform::Fatal("FlatBounds bench configuration was rejected");
        }
        return Move(created.Value());
    }

    spatial::FlatBoundsSet set_;
    Array<uint32> slots_;
    Array<uint64> words_;
    spatial::FlatVisibility out_[FlatViewCount];
    uint32 wordCount_ = 0;
};

// 정답 bit(원소 × 뷰)다.
inline bool ComputeReference(const Dataset& dataset, Span<const Box<double>> bounds,
                             const spatial::FlatBoundsView (&views)[FlatViewCount],
                             Array<uint8>& reference, uint64 (&visible)[FlatViewCount]) noexcept {
    const double largest = LargestSide(dataset.domain);
    if (!reference.TryResize(bounds.Size())) {
        return false;
    }
    for (uint32 view = 0; view < FlatViewCount; ++view) {
        visible[view] = 0;
    }
    for (uint32 id = 0; id < bounds.Size(); ++id) {
        float minDraw = 0.0f;
        float maxDraw = 0.0f;
        DrawRange(id, largest, minDraw, maxDraw);
        uint8 bits = 0;
        for (uint32 view = 0; view < FlatViewCount; ++view) {
            if (FlatReference(bounds[id], dataset.filters[id], minDraw, maxDraw, views[view])) {
                bits |= static_cast<uint8>(1u << view);
                ++visible[view];
            }
        }
        reference[id] = bits;
    }
    return true;
}

struct FlatAccuracy {
    uint64 misses = 0;
    uint64 excess = 0;
    uint64 reference = 0;
};

// 4개 뷰를 한 번에 질의해 정답과 대조한다.
inline FlatAccuracy CheckAccuracy(FlatSubject& subject, const spatial::FlatBoundsReader& reader,
                                  const spatial::FlatBoundsView (&views)[FlatViewCount],
                                  const Array<uint8>& reference) noexcept {
    FlatAccuracy accuracy;
    if (!subject.Query(reader, views, FlatViewCount, 1)) {
        accuracy.misses = NumericLimits<uint64>::Max();
        return accuracy;
    }
    const spatial::FlatBoundsStats stats = reader.Stats();
    const uint32 listCount = stats.largeCount + stats.outOfRangeCount;
    const uint32 listWordBase =
        reader.WordCount() -
        (listCount + spatial::FlatVisibilityBitsPerWord - 1) / spatial::FlatVisibilityBitsPerWord;
    for (uint32 id = 0; id < reference.Size(); ++id) {
        for (uint32 view = 0; view < FlatViewCount; ++view) {
            const bool expected = ((reference[id] >> view) & 1u) != 0;
            const bool reported = subject.Bit(view, id, listWordBase);
            accuracy.reference += expected ? 1u : 0u;
            accuracy.misses += expected && !reported ? 1u : 0u;
            accuracy.excess += !expected && reported ? 1u : 0u;
        }
    }
    return accuracy;
}

inline double TimeFlat(FlatSubject& subject, const spatial::FlatBoundsReader& reader,
                       const spatial::FlatBoundsView (&views)[FlatViewCount], uint32 viewCount,
                       uint32 participants, uint32 rounds, Allocator& allocator) noexcept {
    Array<uint64> samples(allocator);
    (void)subject.Query(reader, views, viewCount, participants); // 예열
    for (uint32 round = 0; round < rounds; ++round) {
        Stopwatch watch;
        watch.Start();
        (void)subject.Query(reader, views, viewCount, participants);
        (void)samples.TryPushBack(watch.Elapsed());
    }
    return MedianOf(samples);
}

// UE 방식 변형(2절): 원소 하나(float 중심·반폭) 대 평면 4개 SoA 묶음, 앞 4평면에서 조기 탈출한다.
// 정밀도(타일 상대·δ)와 거리 판정은 없다. SIMD 축 비교(결정 필요 3)만을 위한 측정이다.
class UeStyleScan {
public:
    explicit UeStyleScan(Allocator& allocator) noexcept : elements_(allocator), words_(allocator) {
    }

    bool Build(const Dataset& dataset, Span<const Box<double>> bounds) noexcept {
        if (!elements_.TryResize(bounds.Size()) ||
            !words_.TryResize((bounds.Size() + 63) / 64 * FlatViewCount)) {
            return false;
        }
        for (uint32 id = 0; id < bounds.Size(); ++id) {
            const Vec3d center = bounds[id].Center();
            const Vec3d half = bounds[id].HalfExtents();
            Element& element = elements_[id];
            element.center[0] = static_cast<float>(center.x);
            element.center[1] = static_cast<float>(center.y);
            element.center[2] = static_cast<float>(center.z);
            element.center[3] = 0.0f;
            element.extent[0] = static_cast<float>(half.x);
            element.extent[1] = static_cast<float>(half.y);
            element.extent[2] = static_cast<float>(half.z);
            element.extent[3] = 0.0f;
            element.filter = dataset.filters[id];
        }
        return true;
    }

    // 뷰 count개를 판정하고 보이는 수를 돌려준다.
    uint64 Query(const spatial::FlatBoundsView* views, uint32 count) noexcept {
        namespace simd = platform::simd;
        uint64 visible = 0;
        const usize wordsPerView = (elements_.Size() + 63) / 64;
        for (uint32 view = 0; view < count; ++view) {
            // 평면을 4개씩 SoA로 묶는다. 남는 칸은 늘 안쪽인 평면(n = 0, 거리 큰 양수)이다.
            const ConvexVolume<double>& volume = views[view].volume;
            const uint32 groups = (volume.planeCount + 3) / 4;
            simd::Register4f nx[8];
            simd::Register4f ny[8];
            simd::Register4f nz[8];
            simd::Register4f ax[8];
            simd::Register4f ay[8];
            simd::Register4f az[8];
            simd::Register4f d[8];
            for (uint32 group = 0; group < groups; ++group) {
                float values[7][4];
                for (uint32 lane = 0; lane < 4; ++lane) {
                    const uint32 plane = group * 4 + lane;
                    const bool used = plane < volume.planeCount;
                    const Plane<double>& source = volume.planes[used ? plane : 0];
                    values[0][lane] = used ? static_cast<float>(source.normal.x) : 0.0f;
                    values[1][lane] = used ? static_cast<float>(source.normal.y) : 0.0f;
                    values[2][lane] = used ? static_cast<float>(source.normal.z) : 0.0f;
                    values[3][lane] = math::Abs(values[0][lane]);
                    values[4][lane] = math::Abs(values[1][lane]);
                    values[5][lane] = math::Abs(values[2][lane]);
                    values[6][lane] = used ? static_cast<float>(source.distance) : -1.0e30f;
                }
                nx[group] = simd::Load(values[0]);
                ny[group] = simd::Load(values[1]);
                nz[group] = simd::Load(values[2]);
                ax[group] = simd::Load(values[3]);
                ay[group] = simd::Load(values[4]);
                az[group] = simd::Load(values[5]);
                d[group] = simd::Load(values[6]);
            }
            uint64* words = words_.Data() + view * wordsPerView;
            const uint32 mask = views[view].filterMask;
            for (usize word = 0; word < wordsPerView; ++word) {
                uint64 bits = 0;
                const usize end =
                    (word + 1) * 64 < elements_.Size() ? (word + 1) * 64 : elements_.Size();
                for (usize id = word * 64; id < end; ++id) {
                    const Element& element = elements_[id];
                    if ((element.filter & mask) == 0) {
                        continue;
                    }
                    const simd::Register4f cx = simd::Splat(element.center[0]);
                    const simd::Register4f cy = simd::Splat(element.center[1]);
                    const simd::Register4f cz = simd::Splat(element.center[2]);
                    const simd::Register4f ex = simd::Splat(element.extent[0]);
                    const simd::Register4f ey = simd::Splat(element.extent[1]);
                    const simd::Register4f ez = simd::Splat(element.extent[2]);
                    bool outside = false;
                    for (uint32 group = 0; group < groups && !outside; ++group) {
                        // n·c − d + |n|·e < 0이면 바깥이다.
                        const simd::Register4f distance =
                            simd::Subtract(simd::Add(simd::Add(simd::Multiply(nx[group], cx),
                                                               simd::Multiply(ny[group], cy)),
                                                     simd::Multiply(nz[group], cz)),
                                           d[group]);
                        const simd::Register4f push = simd::Add(
                            simd::Add(simd::Multiply(ax[group], ex), simd::Multiply(ay[group], ey)),
                            simd::Multiply(az[group], ez));
                        outside = simd::MoveMask(simd::CompareLess(simd::Add(distance, push),
                                                                   simd::Zero())) != 0;
                    }
                    if (!outside) {
                        bits |= uint64{1} << (id - word * 64);
                    }
                }
                words[word] = bits;
                visible += platform::PlatformMath::PopCount64(bits);
            }
        }
        return visible;
    }

private:
    struct alignas(16) Element {
        float center[4];
        float extent[4];
        uint32 filter;
    };

    Array<Element> elements_;
    Array<uint64> words_;
};

template <typename Scan>
double TimeBaseline(Scan& scan, const spatial::FlatBoundsView (&views)[FlatViewCount],
                    uint32 viewCount, ResultBuffer& buffer, Allocator& allocator) noexcept {
    Array<uint64> samples(allocator);
    QueryCounters counters;
    for (uint32 round = 0; round <= FlatBaselineRounds; ++round) {
        Stopwatch watch;
        watch.Start();
        for (uint32 view = 0; view < viewCount; ++view) {
            buffer.Reset();
            scan.template QueryFrustum<false>(views[view].volume, views[view].filterMask, buffer,
                                              counters);
        }
        const uint64 elapsed = watch.Elapsed();
        if (round != 0) {
            (void)samples.TryPushBack(elapsed);
        }
    }
    return MedianOf(samples);
}

inline void WriteStats(JsonWriter& json, const spatial::FlatBoundsStats& stats,
                       uint32 size) noexcept {
    json.Key("stats");
    json.BeginObject();
    json.FieldInteger("elements", stats.elementCount);
    json.FieldInteger("tiles", stats.tileCount);
    json.FieldInteger("livePages", stats.livePageCount);
    json.FieldInteger("pageSlots", stats.pageSlotCount);
    json.FieldInteger("maxPagesPerTile", stats.maxPagesPerTile);
    json.FieldInteger("largeCount", stats.largeCount);
    json.FieldInteger("outOfRangeCount", stats.outOfRangeCount);
    json.FieldInteger("wordCount", stats.wordCount);
    json.Field("bitsetBytesPerView", static_cast<double>(stats.wordCount) * 8.0);
    json.Field("maxPageRadius", static_cast<double>(stats.maxPageRadius));
    json.Field("meanPageRadius", stats.meanPageRadius);
    json.FieldInteger("pageBytesUsed", stats.pageBytesUsed);
    json.FieldInteger("recordBytesUsed", stats.recordBytesUsed);
    json.FieldInteger("tileMapBytesReserved", stats.tileMapBytesReserved);
    json.FieldInteger("listBytesUsed", stats.listBytesUsed);
    const double denominator = size == 0 ? 1.0 : static_cast<double>(size);
    json.Field("pageBytesPerElement", static_cast<double>(stats.pageBytesUsed) / denominator);
    json.Field("totalBytesPerElement",
               static_cast<double>(stats.pageBytesUsed + stats.recordBytesUsed +
                                   stats.tileMapBytesReserved + stats.listBytesUsed) /
                   denominator);
    json.Field("bitsetBitsPerElement", static_cast<double>(stats.wordCount) * 64.0 / denominator);
    json.EndObject();
}

// FlatBoundsSet 구성 하나를 만들고 정확도·메모리·시간을 쓴다. participantsList가 비면 P = 1만 잰다.
inline bool MeasureFlatConfig(const Dataset& dataset, Span<const Box<double>> bounds,
                              const spatial::FlatBoundsConfig& config,
                              const spatial::FlatBoundsView (&views)[FlatViewCount],
                              const Array<uint8>& reference, const uint64 (&visible)[FlatViewCount],
                              Span<const uint32> participantsList, uint32 rounds,
                              Allocator& allocator, JsonWriter& json,
                              uint32* tilesOut = nullptr) noexcept {
    const uint32 size = static_cast<uint32>(bounds.Size());
    FlatSubject subject(Runtime().allocator, config);
    uint64 buildNs = 0;
    if (!subject.Build(dataset, bounds, buildNs)) {
        json.Field("error", "build failed");
        return false;
    }
    json.FieldInteger("pageCapacity", config.pageCapacity);
    json.Field("largeRelativeExtent", config.largeRelativeExtent);
    json.Field("buildMs", static_cast<double>(buildNs) * 1.0e-6);
    spatial::FlatBoundsReadScope read(subject.Set());
    const spatial::FlatBoundsReader reader = read.Reader();
    const spatial::FlatBoundsStats stats = reader.Stats();
    WriteStats(json, stats, size);
    if (tilesOut != nullptr) {
        *tilesOut = stats.tileCount;
    }
    if (!subject.Prepare(reader.WordCount(), FlatViewCount)) {
        json.Field("error", "output allocation failed");
        return false;
    }
    const FlatAccuracy accuracy = CheckAccuracy(subject, reader, views, reference);
    json.Key("accuracy");
    json.BeginObject();
    json.FieldInteger("misses", accuracy.misses);
    json.FieldInteger("excess", accuracy.excess);
    json.FieldInteger("referenceVisible", accuracy.reference);
    json.Field("excessRatio", accuracy.reference == 0
                                  ? 0.0
                                  : static_cast<double>(accuracy.excess) /
                                        static_cast<double>(accuracy.reference));
    json.EndObject();
    spatial::FlatQueryStats queryStats;
    (void)subject.Query(reader, views, FlatViewCount, 1, &queryStats);
    json.Key("queryStats");
    json.BeginObject();
    json.FieldInteger("pageViews", queryStats.pageViews);
    json.FieldInteger("rejectedPageViews", queryStats.rejectedPageViews);
    json.FieldInteger("testedElements", queryStats.testedElements);
    json.FieldInteger("listElements", queryStats.listElements);
    json.FieldInteger("visibleElements", queryStats.visibleElements);
    json.EndObject();
    (void)visible;

    json.Key("timing");
    json.BeginArray();
    const uint32 viewCounts[] = {1, FlatViewCount};
    for (const uint32 viewCount : viewCounts) {
        double single = 0.0;
        for (usize index = 0; index < participantsList.Size(); ++index) {
            const uint32 participants = participantsList[index];
            const double median =
                TimeFlat(subject, reader, views, viewCount, participants, rounds, allocator);
            if (participants == 1) {
                single = median;
            }
            json.BeginObject();
            json.FieldInteger("views", viewCount);
            json.FieldInteger("participants", participants);
            json.Field("medianNs", median);
            json.Field("nsPerElementView",
                       median / (static_cast<double>(size) * static_cast<double>(viewCount)));
            if (single > 0.0) {
                json.Field("efficiency", single / (static_cast<double>(participants) * median));
            }
            json.EndObject();
        }
    }
    json.EndArray();
    printf("    flat cap=%u large=%.0f misses=%llu excess=%llu\n", config.pageCapacity,
           config.largeRelativeExtent, static_cast<unsigned long long>(accuracy.misses),
           static_cast<unsigned long long>(accuracy.excess));
    fflush(stdout);
    return true;
}

inline void RunFlatCase(const CaseInput& input, Allocator* allocator, JsonWriter& json) noexcept {
    const Dataset& dataset = *input.dataset;
    const uint32 size = input.size;
    json.BeginObject();
    json.Field("distribution", DistributionName(input.distribution));
    json.FieldInteger("size", size);
    printf("  flat %s n=%u\n", DistributionName(input.distribution), size);
    fflush(stdout);
    if (!Runtime().Start()) {
        json.Field("error", "scheduler start failed");
        json.EndObject();
        return;
    }
    spatial::FlatBoundsView views[FlatViewCount];
    MakeFlatViews(dataset, views);
    const Span<const Box<double>> bounds(dataset.bounds.Data(), dataset.bounds.Size());
    Array<uint8> reference(*allocator);
    uint64 visible[FlatViewCount] = {};
    if (!ComputeReference(dataset, bounds, views, reference, visible)) {
        json.Field("error", "reference failed");
        json.EndObject();
        return;
    }
    json.Key("referenceVisiblePerView");
    json.BeginArray();
    for (const uint64 count : visible) {
        json.Integer(count);
    }
    json.EndArray();

    // page 용량 64·256·1,024(결정 필요 1). 64는 참여자 1·2·4·8, 나머지는 1·8.
    json.Key("flat");
    json.BeginArray();
    const uint32 allParticipants[] = {1, 2, 4, 8};
    const uint32 endParticipants[] = {1, 8};
    const uint32 capacities[] = {64, 256, 1024};
    uint32 tiles = 0;
    for (const uint32 capacity : capacities) {
        spatial::FlatBoundsConfig config;
        config.pageCapacity = capacity;
        json.BeginObject();
        // 희소한 타일 분포에서 큰 page 용량은 타일마다 page 하나 × 용량 × 원소당 약 48 byte를
        // 잡는다. 용량 64에서 잰 타일 수로 예상해 이 기기에서 감당하기 어려운 크기는 건너뛴다(그
        // 예상치 자체가 결정 필요 1의 근거다).
        const double estimate = static_cast<double>(tiles) * capacity * 48.0;
        if (capacity != 64 && estimate > 1.0e9) {
            json.FieldInteger("pageCapacity", capacity);
            json.Field("skipped", "estimated page memory above 1 GB");
            json.Field("estimatedPageBytes", estimate);
            json.EndObject();
            printf("    flat cap=%u skipped (estimated %.2f GB)\n", capacity, estimate * 1.0e-9);
            continue;
        }
        (void)MeasureFlatConfig(dataset, bounds, config, views, reference, visible,
                                capacity == 64 ? Span<const uint32>(allParticipants, 4)
                                               : Span<const uint32>(endParticipants, 2),
                                FlatRounds, *allocator, json, capacity == 64 ? &tiles : nullptr);
        json.EndObject();
    }
    json.EndArray();

    // 큰 원소 목록 변형(결정 필요 2): 원소 1/1,000·1/100을 반폭 3·10⁵ cm로 키우고 기준 2·4 타일을
    // 비교한다. 목록 크기·page 반경 R·초과 비율·시간(참여자 1, 뷰 4).
    json.Key("largeVariants");
    json.BeginArray();
    struct LargeVariant {
        uint32 every;
        double extentTiles;
    };
    const LargeVariant largeVariants[] = {{1000, 2.0}, {1000, 4.0}, {100, 2.0}, {100, 4.0}};
    Array<Box<double>> grown(*allocator);
    Array<uint8> grownReference(*allocator);
    const uint32 single[] = {1};
    for (const LargeVariant& variant : largeVariants) {
        if (!grown.TryResize(bounds.Size())) {
            break;
        }
        for (uint32 id = 0; id < bounds.Size(); ++id) {
            grown[id] = bounds[id];
            if (id % variant.every == 0) {
                const Vec3d center = bounds[id].Center();
                grown[id] =
                    Box<double>(center - Vec3d::Splat(300000.0), center + Vec3d::Splat(300000.0));
            }
        }
        const Span<const Box<double>> grownBounds(grown.Data(), grown.Size());
        uint64 grownVisible[FlatViewCount] = {};
        if (!ComputeReference(dataset, grownBounds, views, grownReference, grownVisible)) {
            break;
        }
        spatial::FlatBoundsConfig config;
        config.largeRelativeExtent = config.tileSize * variant.extentTiles;
        json.BeginObject();
        json.FieldInteger("hugeEvery", variant.every);
        (void)MeasureFlatConfig(dataset, grownBounds, config, views, grownReference, grownVisible,
                                Span<const uint32>(single, 1), FlatBaselineRounds, *allocator,
                                json);
        json.EndObject();
    }
    json.EndArray();

    // UE 방식 변형(결정 필요 3).
    {
        UeStyleScan scan(*allocator);
        json.Key("ueStyle");
        json.BeginObject();
        if (scan.Build(dataset, bounds)) {
            const uint32 viewCounts[] = {1, FlatViewCount};
            for (const uint32 viewCount : viewCounts) {
                Array<uint64> samples(*allocator);
                uint64 count = scan.Query(views, viewCount);
                for (uint32 round = 0; round < FlatRounds; ++round) {
                    Stopwatch watch;
                    watch.Start();
                    count = scan.Query(views, viewCount);
                    (void)samples.TryPushBack(watch.Elapsed());
                }
                const double median = MedianOf(samples);
                json.Key(viewCount == 1 ? "views1" : "views4");
                json.BeginObject();
                json.Field("medianNs", median);
                json.Field("nsPerElementView",
                           median / (static_cast<double>(size) * static_cast<double>(viewCount)));
                json.FieldInteger("visible", count);
                json.EndObject();
            }
        }
        json.EndObject();
    }

    // 기존 비교군(같은 입력, 프러스텀 질의를 뷰마다): A1·A2·D.
    Array<uint32> resultStorage(*allocator);
    (void)resultStorage.TryResize(size);
    ResultBuffer buffer;
    buffer.ids = resultStorage.Data();
    buffer.capacity = size;
    const auto writeBaseline = [&](const char* name, double one, double four) noexcept {
        json.Key(name);
        json.BeginObject();
        json.Field("views1Ns", one);
        json.Field("views4Ns", four);
        json.Field("nsPerElementView1", one / static_cast<double>(size));
        json.Field("nsPerElementView4", four / (static_cast<double>(size) * FlatViewCount));
        json.EndObject();
    };
    {
        FlatScanDouble scan(*allocator);
        if (scan.Create(size)) {
            for (uint32 id = 0; id < size; ++id) {
                (void)scan.Insert(id, bounds[id], dataset.filters[id]);
            }
            writeBaseline("A1", TimeBaseline(scan, views, 1, buffer, *allocator),
                          TimeBaseline(scan, views, FlatViewCount, buffer, *allocator));
        }
    }
    {
        FlatScanFloat<true> scan(*allocator);
        if (scan.Create(size)) {
            for (uint32 id = 0; id < size; ++id) {
                (void)scan.Insert(id, bounds[id], dataset.filters[id]);
            }
            writeBaseline("A2", TimeBaseline(scan, views, 1, buffer, *allocator),
                          TimeBaseline(scan, views, FlatViewCount, buffer, *allocator));
        }
    }
    {
        LooseVariant variant;
        LooseIndex index(*allocator, variant);
        if (index.Create(size)) {
            for (uint32 id = 0; id < size; ++id) {
                (void)index.Insert(id, bounds[id], dataset.filters[id]);
            }
            index.MaintainAll();
            index.BeginRead();
            writeBaseline("D", TimeBaseline(index, views, 1, buffer, *allocator),
                          TimeBaseline(index, views, FlatViewCount, buffer, *allocator));
            index.EndRead();
        }
    }
    json.EndObject();
}

} // namespace rocks::spatialbench
