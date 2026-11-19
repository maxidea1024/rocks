#include "FlatBoundsTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/PlatformMath.h>
#include <Task/ParallelFor.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/NFailAllocator.h>

using namespace rocks;
using namespace rocks::spatial;
using namespace rocks::spatial::flattest;

// FlatBoundsSet 시험이다(Docs/Design/FlatBounds.ko.md 9절 FB01–FB07·FB09, FB08은
// FlatBoundsConcurrencyTests.cpp). 정답은 6.1절 술어의 double 전수 계산이다.

namespace {

constexpr uint64 AllOnes = NumericLimits<uint64>::Max();

// 시험 설정은 늘 유효하다(생성 실패는 시험 코드 오류다).
FlatBoundsSet MakeSet(const FlatBoundsConfig& config, Allocator& allocator) noexcept {
    auto created = FlatBoundsSet::TryCreate(config, allocator);
    if (!created) {
        platform::Fatal("FlatBounds test configuration was rejected");
    }
    return Move(created.Value());
}

// 작은 타일(1,024 cm)로 수천 개 원소에서도 타일 경계 걸침·타일 이동·여러 page가 생기게 한다.
FlatBoundsConfig SmallTileConfig() noexcept {
    FlatBoundsConfig config;
    config.tileSize = 1024.0;
    config.largeRelativeExtent = 2048.0;
    config.pageCapacity = 64;
    return config;
}

// 집합과 정답 모형을 함께 바꾸는 시험 대상이다. 변경 결과의 FlatBoundsMoves로 모형의 슬롯을 고친다
// (집합에 다시 묻지 않는다). 그래서 대조가 이동 반환의 정확성도 검사한다. 모형과 대조용 배열은
// 집합의 allocator가 아닌 자기 allocator를 쓴다(실패 주입이 모형을 건드리지 않게).
class Harness {
public:
    Harness(const FlatBoundsConfig& config, Allocator& allocator) noexcept
        : set_(MakeSet(config, allocator)), model_(modelAllocator_), byRecord_(modelAllocator_),
          allocator_(modelAllocator_) {
    }

    FlatBoundsSet& Set() noexcept {
        return set_;
    }

    Array<ModelElement>& Model() noexcept {
        return model_;
    }

    // 성공하면 모형 색인을, 실패하면 NumericLimits<uint32>::Max()를 돌려준다.
    uint32 Add(const FlatBoundsDesc& desc) noexcept {
        FlatBoundsMoves moves;
        auto added = set_.TryAdd(desc, &moves);
        if (!added) {
            return NumericLimits<uint32>::Max();
        }
        ModelElement element;
        element.handle = added.Value();
        element.desc = desc;
        element.live = true;
        const uint32 index = static_cast<uint32>(model_.Size());
        (void)model_.TryPushBack(element);
        const uint32 record = element.handle.slot.index;
        if (record >= byRecord_.Size()) {
            (void)byRecord_.TryResize(record + 1);
        }
        byRecord_[record] = index;
        movesOk_ = movesOk_ && moves.count == 1 && moves.moves[0].handle == element.handle;
        Apply(moves);
        return index;
    }

    bool Remove(uint32 index) noexcept {
        FlatBoundsMoves moves;
        if (!set_.TryRemove(model_[index].handle, &moves)) {
            return false;
        }
        model_[index].live = false;
        movesOk_ = movesOk_ && moves.count <= 1;
        Apply(moves);
        return true;
    }

    bool Update(uint32 index, const Box<double>& bounds) noexcept {
        FlatBoundsMoves moves;
        if (!set_.TryUpdateBounds(model_[index].handle, bounds, &moves)) {
            return false;
        }
        model_[index].desc.bounds = bounds;
        Apply(moves);
        return true;
    }

    // 모형과 집합의 비트 집합을 뷰마다 대조한다. 출력은 미리 1로 채워 해제 page·꼬리 bit를 0으로
    // 쓰는지도 본다.
    CompareCounts Check(Span<const FlatBoundsView> views,
                        WorkerScheduler* scheduler = nullptr) noexcept {
        CompareCounts counts;
        FlatBoundsReadScope read(set_);
        const FlatBoundsReader reader = read.Reader();
        auto report = reader.Validate();
        if (!report || !report.Value().IsValid()) {
            ++counts.violations;
        }
        if (!movesOk_) {
            ++counts.violations;
        }
        const uint32 wordCount = reader.WordCount();
        Array<uint64> storage(allocator_);
        (void)storage.TryResize(static_cast<usize>(wordCount) * views.Size() + 1);
        Array<FlatVisibility> out(allocator_);
        for (usize index = 0; index < views.Size(); ++index) {
            FlatVisibility visibility;
            visibility.words = Span<uint64>(storage.Data() + index * wordCount, wordCount);
            for (usize word = 0; word < wordCount; ++word) {
                visibility.words[word] = AllOnes;
            }
            (void)out.TryPushBack(visibility);
        }
        SystemAllocator scratch;
        auto queried = reader.QueryViews(views, Span<FlatVisibility>(out.Data(), out.Size()),
                                         scheduler, scratch);
        if (!queried) {
            ++counts.violations;
            return counts;
        }
        for (usize index = 0; index < views.Size(); ++index) {
            CompareVisibility(reader, model_, views[index], out[index], counts);
        }
        return counts;
    }

private:
    void Apply(const FlatBoundsMoves& moves) noexcept {
        for (uint32 index = 0; index < moves.count; ++index) {
            const FlatBoundsMove& move = moves.moves[index];
            const uint32 record = move.handle.slot.index;
            if (record >= byRecord_.Size() || !(model_[byRecord_[record]].handle == move.handle)) {
                movesOk_ = false;
                continue;
            }
            model_[byRecord_[record]].slot = move.newSlot;
        }
    }

    SystemAllocator modelAllocator_;
    FlatBoundsSet set_;
    Array<ModelElement> model_;
    Array<uint32> byRecord_;
    Allocator& allocator_;
    bool movesOk_ = true;
};

// FB01 분포: 균일, 군집, 크기 편차(큰 원소 목록 포함), 타일 경계 걸침, 범위 밖(key 넘침).
FlatBoundsDesc RandomDesc(RandomStream& random, uint32 kind) noexcept {
    FlatBoundsDesc desc;
    Vec3d center;
    double half = Uniform(random, 0.0, 20.0);
    switch (kind % 5) {
    case 0:
        center = Vec3d(Uniform(random, -4000, 4000), Uniform(random, -4000, 4000),
                       Uniform(random, -4000, 4000));
        break;
    case 1: {
        const Vec3d clusters[] = {Vec3d(500, 500, 0), Vec3d(-2000, 1500, 300),
                                  Vec3d(3000, -3000, -1000)};
        center = clusters[random.NextUInt32Below(3)] + Vec3d(Uniform(random, -150, 150),
                                                             Uniform(random, -150, 150),
                                                             Uniform(random, -150, 150));
        break;
    }
    case 2:
        center = Vec3d(Uniform(random, -4000, 4000), Uniform(random, -4000, 4000),
                       Uniform(random, -4000, 4000));
        half = random.NextUInt32Below(10) < 7 ? Uniform(random, 20.0, 600.0)
                                              : Uniform(random, 600.0, 4000.0);
        break;
    case 3:
        // 타일 경계(1,024의 배수)에 중심을 둔다(±1 ULP 포함).
        center = Vec3d(1024.0 * random.NextInt32InRange(-4, 4),
                       1024.0 * random.NextInt32InRange(-4, 4), Uniform(random, -2000, 2000));
        if (random.NextBool()) {
            center.x = math::NextDown(center.x);
        } else {
            center.y = math::NextUp(center.y);
        }
        half = Uniform(random, 0.0, 300.0);
        break;
    default:
        // key가 int32 범위 밖인 원소(범위 밖 목록)다.
        center = Vec3d(Uniform(random, 1.0e13, 2.0e13), Uniform(random, -4000, 4000), 0.0);
        break;
    }
    const Vec3d halfExtents(half * Uniform(random, 0.2, 1.0), half * Uniform(random, 0.2, 1.0),
                            half * Uniform(random, 0.2, 1.0));
    desc.bounds = Box<double>(center - halfExtents, center + halfExtents);
    desc.filterMask = 1u << random.NextUInt32Below(4);
    desc.alwaysVisible = random.NextUInt32Below(10) == 0;
    if (random.NextUInt32Below(10) < 3) {
        desc.minDrawDistance =
            random.NextBool() ? 0.0f : static_cast<float>(Uniform(random, 0.0, 2000.0));
        desc.maxDrawDistance =
            random.NextBool()
                ? 0.0f
                : desc.minDrawDistance + static_cast<float>(Uniform(random, 1.0, 6000.0));
    }
    desc.userValue = random.NextUInt32();
    return desc;
}

// FB01 뷰: 프러스텀 6·8평면, 축 평행 상자(−0 성분 포함), 퇴화(두께 0 판·빈 영역), 필터·배율 조합.
FlatBoundsView RandomView(RandomStream& random) noexcept {
    FlatBoundsView view;
    const Vec3d apex(Uniform(random, -4000, 4000), Uniform(random, -4000, 4000),
                     Uniform(random, -4000, 4000));
    const uint32 kind = random.NextUInt32Below(10);
    if (kind < 6) {
        view.volume = MakeFrustum(apex, RandomDirection(random), Uniform(random, 1, 50),
                                  Uniform(random, 500, 9000), &random, kind < 3 ? 0u : 2u);
    } else if (kind < 8) {
        const double half = Uniform(random, 100, 3000);
        const Plane<double> planes[] = {Plane<double>(Vec3d(1.0, -0.0, 0.0), apex.x - half),
                                        Plane<double>(Vec3d(-1.0, 0.0, -0.0), -(apex.x + half)),
                                        Plane<double>(Vec3d(-0.0, 1.0, 0.0), apex.y - half),
                                        Plane<double>(Vec3d(0.0, -1.0, -0.0), -(apex.y + half)),
                                        Plane<double>(Vec3d(-0.0, -0.0, 1.0), apex.z - half),
                                        Plane<double>(Vec3d(0.0, 0.0, -1.0), -(apex.z + half))};
        view.volume = VolumeOf(planes);
    } else {
        // 퇴화: 두께 0인 판(n·p ≥ d와 n·p ≤ d) 또는 빈 영역(n·p ≥ d와 n·p ≤ d − gap).
        const Vec3d normal = RandomDirection(random);
        const double distance = Dot(normal, apex);
        const double gap = kind == 8 ? 0.0 : Uniform(random, 1.0, 100.0);
        const Plane<double> planes[] = {Plane<double>(normal, distance),
                                        Plane<double>(-normal, -distance + gap)};
        view.volume = VolumeOf(planes);
    }
    view.distanceOrigin = apex;
    const float scales[] = {1.0f, 0.5f, 2.0f, 3.7f};
    view.distanceScale = scales[random.NextUInt32Below(4)];
    view.filterMask = random.NextUInt32Below(8) == 0 ? 0u : (1u + random.NextUInt32Below(15));
    return view;
}

FlatBoundsDesc SimpleDesc(const Box<double>& bounds) noexcept {
    FlatBoundsDesc desc;
    desc.bounds = bounds;
    desc.filterMask = 1;
    return desc;
}

Box<double> Cube(const Vec3d& center, double half) noexcept {
    return Box<double>(center - Vec3d::Splat(half), center + Vec3d::Splat(half));
}

FlatBoundsView EverythingView() noexcept {
    FlatBoundsView view;
    const Plane<double> planes[] = {Plane<double>(Vec3d(1, 0, 0), -1.0e300)};
    view.volume = VolumeOf(planes);
    view.filterMask = AllOnes & 0xFFFFFFFFu;
    return view;
}

} // namespace

TEST(FlatBounds_FB01_RandomElementsAndViewsMatchReference) {
    SystemAllocator allocator;
    Harness harness(SmallTileConfig(), allocator);
    RandomStream random(0xFB01u ^ test::CurrentSeed());
    for (uint32 index = 0; index < 3000; ++index) {
        REQUIRE(harness.Add(RandomDesc(random, index)) != NumericLimits<uint32>::Max());
    }
    FlatBoundsView views[64];
    for (FlatBoundsView& view : views) {
        view = RandomView(random);
    }
    const CompareCounts counts = harness.Check(Span<const FlatBoundsView>(views, 64));
    CHECK(counts.misses == 0);
    CHECK(counts.violations == 0);
    CHECK(counts.referenceVisible > 1000);
    // 초과는 띠 안이면 허용되지만 대부분의 판정은 정답과 같아야 한다.
    CHECK(counts.excess * 100 <= counts.referenceVisible);
    FlatBoundsReadScope read(harness.Set());
    const FlatBoundsStats stats = read.Reader().Stats();
    CHECK(stats.largeCount > 0);
    CHECK(stats.outOfRangeCount > 0);
    CHECK(stats.maxPagesPerTile > 1);
}

// FB02: 원점에서 10⁹ cm 떨어진 타일의 원점 근처 작은 원소(R 작음)와 그 꼭짓점을 지나는 평면이다.
// δ의 double 오차 항(12ε·(‖n‖₁·(‖o‖∞ + R) + |d|))이 없으면 이런 접촉이 누락된다(리뷰 probe).
TEST(FlatBounds_FB02_FarTileCornerPlanesNeverMiss) {
    SystemAllocator allocator;
    FlatBoundsConfig config;
    Harness harness(config, allocator);
    RandomStream random(0xFB02u ^ test::CurrentSeed());
    constexpr uint32 Elements = 768;
    Array<FlatBoundsView> views(allocator);
    for (uint32 index = 0; index < Elements; ++index) {
        const double scale = random.NextBool() ? 1.0e9 : 1.0e7;
        const Vec3d world(Uniform(random, -scale, scale), Uniform(random, -scale, scale),
                          Uniform(random, -scale, scale));
        const Vec3d origin(math::Floor(world.x / config.tileSize) * config.tileSize,
                           math::Floor(world.y / config.tileSize) * config.tileSize,
                           math::Floor(world.z / config.tileSize) * config.tileSize);
        const double offset = random.NextBool() ? 1.0e-3 : 1.0;
        const Vec3d center = origin + Vec3d(Uniform(random, 0, offset), Uniform(random, 0, offset),
                                            Uniform(random, 0, offset));
        const double half = Uniform(random, 0, offset * 0.1);
        FlatBoundsDesc desc = SimpleDesc(Cube(center, half));
        REQUIRE(harness.Add(desc) != NumericLimits<uint32>::Max());
        const Vec3d normal = RandomDirection(random);
        const Vec3d highest(normal.x >= 0 ? desc.bounds.max.x : desc.bounds.min.x,
                            normal.y >= 0 ? desc.bounds.max.y : desc.bounds.min.y,
                            normal.z >= 0 ? desc.bounds.max.z : desc.bounds.min.z);
        double distance = Dot(normal, highest);
        if (random.NextUInt32Below(3) == 1) {
            distance = math::NextDown(distance);
        }
        const Plane<double> planes[] = {Plane<double>(normal, distance)};
        FlatBoundsView view;
        view.volume = VolumeOf(planes);
        view.filterMask = 1;
        (void)views.TryPushBack(view);
    }
    uint64 touching = 0;
    CompareCounts total;
    for (usize start = 0; start < views.Size(); start += 16) {
        const usize count = views.Size() - start < 16 ? views.Size() - start : 16;
        const CompareCounts counts =
            harness.Check(Span<const FlatBoundsView>(views.Data() + start, count));
        total.misses += counts.misses;
        total.violations += counts.violations;
        total.excess += counts.excess;
    }
    for (usize index = 0; index < views.Size(); ++index) {
        touching +=
            Overlap(views[index].volume, harness.Model()[index].desc.bounds) == QueryResult::Hit
                ? 1u
                : 0u;
    }
    CHECK(touching > Elements / 2);
    CHECK(total.misses == 0);
    CHECK(total.violations == 0);
}

// FB02: 음수 key, 타일 경계 ±1 ULP 중심, 큰 원소 경계값, key 넘침의 배치와 판정이다.
TEST(FlatBounds_FB02_TileKeyAndListBoundaries) {
    SystemAllocator allocator;
    FlatBoundsConfig config;
    Harness harness(config, allocator);
    const double tile = config.tileSize;
    struct Case {
        Box<double> bounds;
        bool inList;
        bool large;
        double originX;
    };
    const Case cases[] = {
        // 음수 key: 중심 −1 → key −1.
        {Cube(Vec3d(-1.0, 5.0, 5.0), 0.5), false, false, -tile},
        // 타일 경계 정확히: floor(1) = 1. 아래로 한 ULP: key 0.
        {Cube(Vec3d(tile, 5.0, 5.0), 0.0), false, false, tile},
        {Cube(Vec3d(math::NextDown(tile), 5.0, 5.0), 0.0), false, false, 0.0},
        {Cube(Vec3d(math::NextUp(-tile), 5.0, 5.0), 0.0), false, false, -tile},
        // 큰 원소 경계: 상대 좌표 최대 절댓값이 정확히 기준이면 page, 넘으면 큰 원소 목록.
        {Box<double>(Vec3d(-config.largeRelativeExtent, 0, 0),
                     Vec3d(config.largeRelativeExtent, 1, 1)),
         false, false, 0.0},
        {Box<double>(Vec3d(math::NextDown(-config.largeRelativeExtent), 0, 0),
                     Vec3d(config.largeRelativeExtent, 1, 1)),
         true, true, 0.0},
        // key 넘침: 중심 / tileSize > 2^31.
        {Cube(Vec3d(1.0e15, 0.0, 0.0), 1.0), true, false, 0.0},
        {Cube(Vec3d(-1.0e15, 0.0, 0.0), 1.0e3), true, false, 0.0},
    };
    for (const Case& item : cases) {
        REQUIRE(harness.Add(SimpleDesc(item.bounds)) != NumericLimits<uint32>::Max());
    }
    {
        FlatBoundsReadScope read(harness.Set());
        const FlatBoundsReader reader = read.Reader();
        for (usize index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
            auto placement = reader.TryPlacementOf(harness.Model()[index].handle);
            REQUIRE(placement);
            CHECK(placement.Value().inList == cases[index].inList);
            if (cases[index].inList) {
                CHECK(placement.Value().large == cases[index].large);
            } else {
                CHECK(placement.Value().tileOrigin.x == cases[index].originX);
            }
        }
    }
    FlatBoundsView views[3];
    views[0] = EverythingView();
    views[1].volume = MakeFrustum(Vec3d(-1000, 0, 0), Vec3d(1, 0, 0), 1.0, 5.0e5);
    views[1].filterMask = 1;
    const Plane<double> planes[] = {Plane<double>(Vec3d(-1, 0, 0), -tile)};
    views[2].volume = VolumeOf(planes);
    views[2].filterMask = 1;
    const CompareCounts counts = harness.Check(Span<const FlatBoundsView>(views, 3));
    CHECK(counts.misses == 0);
    CHECK(counts.violations == 0);
}

// FB03: 거리 경계(d가 정확히 m·M, 뷰 근처 하한, 배율)와 무작위 거리 조합, 입력 오류다.
TEST(FlatBounds_FB03_DrawDistanceBoundaries) {
    SystemAllocator allocator;
    FlatBoundsConfig config;
    Harness harness(config, allocator);
    // 같은 타일에 큰 원소(상대 좌표 약 4·10⁵ cm)를 두어 page R을 키운다(6.2절 예: 뷰 근처 판정).
    FlatBoundsDesc wide =
        SimpleDesc(Box<double>(Vec3d(1.0, 1.0, 1.0), Vec3d(400000.0, 400000.0, 2.0)));
    wide.filterMask = 2;
    REQUIRE(harness.Add(wide) != NumericLimits<uint32>::Max());
    const Vec3d origin(1000.0, 1000.0, 1000.0);

    // 최근접 거리가 정확히 1,000인 원소와 상한 1,000(보여야 한다).
    FlatBoundsDesc exactMax =
        SimpleDesc(Box<double>(origin + Vec3d(1000, 0, 0), origin + Vec3d(1001, 0, 0)));
    exactMax.maxDrawDistance = 1000.0f;
    // 가장 먼 꼭짓점 거리가 정확히 1,000인 원소와 하한 1,000(보여야 한다).
    FlatBoundsDesc exactMin =
        SimpleDesc(Box<double>(origin - Vec3d(1, 0, 0), origin + Vec3d(1000, 0, 0)));
    exactMin.minDrawDistance = 1000.0f;
    // 뷰 근처: 기준점에서 1 cm 안의 작은 원소, 하한·상한이 모두 걸린다.
    FlatBoundsDesc nearView =
        SimpleDesc(Box<double>(origin + Vec3d(0.25, 0, 0), origin + Vec3d(0.75, 0.5, 0.5)));
    nearView.minDrawDistance = static_cast<float>(math::Sqrt(0.75 * 0.75 + 0.5 * 0.5 + 0.5 * 0.5));
    nearView.maxDrawDistance = 0.25f;
    // 배율: 상한 500 × 배율 2 = 1,000.
    FlatBoundsDesc scaled =
        SimpleDesc(Box<double>(origin + Vec3d(0, 1000, 0), origin + Vec3d(0, 1002, 0)));
    scaled.maxDrawDistance = 500.0f;
    const uint32 exactMaxIndex = harness.Add(exactMax);
    const uint32 exactMinIndex = harness.Add(exactMin);
    const uint32 nearIndex = harness.Add(nearView);
    const uint32 scaledIndex = harness.Add(scaled);
    REQUIRE(scaledIndex != NumericLimits<uint32>::Max());
    RandomStream random(0xFB03u ^ test::CurrentSeed());
    for (uint32 index = 0; index < 1500; ++index) {
        FlatBoundsDesc desc = SimpleDesc(
            Cube(origin + Vec3d(Uniform(random, -3000, 3000), Uniform(random, -3000, 3000),
                                Uniform(random, -3000, 3000)),
                 Uniform(random, 0.0, 200.0)));
        desc.minDrawDistance =
            random.NextBool() ? 0.0f : static_cast<float>(Uniform(random, 0, 3000));
        desc.maxDrawDistance =
            random.NextBool() ? 0.0f
                              : desc.minDrawDistance + static_cast<float>(Uniform(random, 0, 3000));
        REQUIRE(harness.Add(desc) != NumericLimits<uint32>::Max());
    }

    FlatBoundsView views[4];
    for (uint32 index = 0; index < 4; ++index) {
        views[index] = EverythingView();
        views[index].filterMask = 1;
        views[index].distanceOrigin = origin;
    }
    views[1].distanceScale = 2.0f;
    views[2].distanceScale = 0.25f;
    views[3].distanceScale = 4.0f;
    const CompareCounts counts = harness.Check(Span<const FlatBoundsView>(views, 4));
    CHECK(counts.misses == 0);
    CHECK(counts.violations == 0);
    CHECK(ReferenceVisible(harness.Model()[exactMaxIndex].desc, views[0]));
    CHECK(ReferenceVisible(harness.Model()[exactMinIndex].desc, views[0]));
    CHECK(ReferenceVisible(harness.Model()[scaledIndex].desc, views[1]));
    (void)nearIndex;

    // 입력 오류: NaN·음수·Inf·min > max는 InvalidArgument이고 상태를 바꾸지 않는다.
    const float nan = platform::PlatformMath::QuietNaN<float>();
    const float infinity = platform::PlatformMath::Infinity<float>();
    const float bad[][2] = {{nan, 0.0f},      {0.0f, nan},      {-1.0f, 0.0f}, {0.0f, -1.0f},
                            {infinity, 0.0f}, {0.0f, infinity}, {10.0f, 5.0f}};
    const FlatBoundsHandle handle = harness.Model()[exactMaxIndex].handle;
    for (const auto& pair : bad) {
        FlatBoundsDesc desc = SimpleDesc(Cube(origin, 1.0));
        desc.minDrawDistance = pair[0];
        desc.maxDrawDistance = pair[1];
        auto added = harness.Set().TryAdd(desc);
        CHECK(!added && added.Error() == ErrorCode::InvalidArgument);
        auto changed = harness.Set().TrySetDrawDistance(handle, pair[0], pair[1]);
        CHECK(!changed && changed.Error() == ErrorCode::InvalidArgument);
    }
    // 하한만 있는 조합(상한 0)은 유효하다.
    CHECK(harness.Set().TrySetDrawDistance(handle, 10.0f, 0.0f));
    CHECK(harness.Set().TrySetDrawDistance(handle, 0.0f, 1000.0f));
    FlatBoundsReadScope read(harness.Set());
    auto desc = read.Reader().TryGet(handle);
    REQUIRE(desc);
    CHECK(desc.Value().minDrawDistance == 0.0f && desc.Value().maxDrawDistance == 1000.0f);
}

// FB04: 삽입·삭제·같은 타일 갱신·타일 이동·목록 이동과 속성 변경 시퀀스다. FlatBoundsMoves로만 고친
// 모형 슬롯이 실제 슬롯과 같고(Harness::Check의 TrySlotOf 대조), Validate가 위반 없이 끝난다.
TEST(FlatBounds_FB04_MutationSequenceReportsMovesAndKeepsInvariants) {
    SystemAllocator allocator;
    Harness harness(SmallTileConfig(), allocator);
    RandomStream random(0xFB04u ^ test::CurrentSeed());
    Array<uint32> live(allocator);
    uint64 inPlace = 0;
    for (uint32 operation = 0; operation < 4000; ++operation) {
        const uint32 choice = random.NextUInt32Below(100);
        if (live.Size() < 50 || choice < 30) {
            const uint32 index = harness.Add(RandomDesc(random, random.NextUInt32Below(5)));
            REQUIRE(index != NumericLimits<uint32>::Max());
            (void)live.TryPushBack(index);
        } else if (choice < 50) {
            const usize pick = random.NextUInt32Below(static_cast<uint32>(live.Size()));
            REQUIRE(harness.Remove(live[pick]));
            live[pick] = live[live.Size() - 1];
            (void)live.TryResize(live.Size() - 1);
        } else if (choice < 85) {
            const uint32 index = live[random.NextUInt32Below(static_cast<uint32>(live.Size()))];
            Box<double> bounds = harness.Model()[index].desc.bounds;
            const uint32 move = random.NextUInt32Below(4);
            if (move == 0) {
                // 같은 타일 안의 작은 이동(제자리 갱신이 많다).
                const Vec3d delta(Uniform(random, -2, 2), Uniform(random, -2, 2),
                                  Uniform(random, -2, 2));
                bounds = Box<double>(bounds.min + delta, bounds.max + delta);
                ++inPlace;
            } else if (move == 1) {
                bounds = RandomDesc(random, 0).bounds; // 다른 타일
            } else if (move == 2) {
                bounds = RandomDesc(random, 2).bounds; // 크기 변화(목록 이동 포함)
            } else {
                bounds = RandomDesc(random, 4).bounds; // 범위 밖 목록
            }
            REQUIRE(harness.Update(index, bounds));
        } else {
            const uint32 index = live[random.NextUInt32Below(static_cast<uint32>(live.Size()))];
            FlatBoundsSet& set = harness.Set();
            const FlatBoundsHandle handle = harness.Model()[index].handle;
            FlatBoundsDesc& desc = harness.Model()[index].desc;
            switch (random.NextUInt32Below(4)) {
            case 0:
                desc.filterMask = 1u << random.NextUInt32Below(4);
                REQUIRE(set.TrySetFilter(handle, desc.filterMask));
                break;
            case 1:
                desc.minDrawDistance = static_cast<float>(Uniform(random, 0, 500));
                desc.maxDrawDistance = random.NextBool() ? 0.0f : desc.minDrawDistance + 1000.0f;
                REQUIRE(set.TrySetDrawDistance(handle, desc.minDrawDistance, desc.maxDrawDistance));
                break;
            case 2:
                desc.alwaysVisible = !desc.alwaysVisible;
                REQUIRE(set.TrySetAlwaysVisible(handle, desc.alwaysVisible));
                break;
            default:
                desc.userValue = random.NextUInt32();
                REQUIRE(set.TrySetUserValue(handle, desc.userValue));
                break;
            }
        }
        if (operation % 250 == 0) {
            FlatBoundsView view = RandomView(random);
            const CompareCounts counts = harness.Check(Span<const FlatBoundsView>(&view, 1));
            CHECK(counts.misses == 0);
            CHECK(counts.violations == 0);
        }
    }
    // 대부분 지워 빈 page가 풀로 돌아가고 타일 등록이 지워지는지 본다.
    while (live.Size() > 5) {
        REQUIRE(harness.Remove(live[live.Size() - 1]));
        (void)live.TryResize(live.Size() - 1);
    }
    FlatBoundsView view = EverythingView();
    const CompareCounts counts = harness.Check(Span<const FlatBoundsView>(&view, 1));
    CHECK(counts.misses == 0);
    CHECK(counts.violations == 0);
    FlatBoundsReadScope read(harness.Set());
    const FlatBoundsReader reader = read.Reader();
    const FlatBoundsStats stats = reader.Stats();
    CHECK(stats.freePageCount > 0);
    CHECK(stats.livePageCount <= 5);
    CHECK(stats.tileCount <= 5);
    CHECK(stats.pageReleases > 0);
    CHECK(stats.inPlaceUpdates > 0 && stats.relocations > 0 && stats.listInsertions > 0);
    CHECK(inPlace > 0);
    // ForEachVisible이 슬롯·사용자 값·handle을 모형과 같게 돌려준다.
    Array<uint64> words(allocator);
    REQUIRE(words.TryResize(reader.WordCount()));
    FlatVisibility out{Span<uint64>(words.Data(), words.Size()), 0};
    SystemAllocator scratch;
    REQUIRE(reader.QueryViews(Span<const FlatBoundsView>(&view, 1), Span<FlatVisibility>(&out, 1),
                              nullptr, scratch));
    uint32 visited = 0;
    bool consistent = true;
    REQUIRE(reader.ForEachVisible(out, [&](const FlatVisibleElement& element) noexcept {
        ++visited;
        auto slot = reader.TrySlotOf(element.handle);
        auto value = reader.TryUserValueAt(element.slot);
        consistent = consistent && slot && slot.Value() == element.slot && value &&
                     value.Value() == element.userValue;
        return QueryControl::Continue;
    }));
    CHECK(consistent);
    CHECK(visited == reader.Count());
}

// FB05: 출력 계약: 해제 page·꼬리 bit 0, 세대 낡음, out 짧음·겹침·개수 불일치, 뷰 0개, 잘못된 뷰.
TEST(FlatBounds_FB05_OutputContract) {
    SystemAllocator allocator;
    Harness harness(SmallTileConfig(), allocator);
    // 여러 타일에 page를 만들고 일부 타일을 비워 해제 page를 만든다.
    for (int32 tile = 0; tile < 6; ++tile) {
        for (uint32 index = 0; index < 70; ++index) {
            REQUIRE(harness.Add(SimpleDesc(Cube(Vec3d(1024.0 * tile + 10.0 + index, 10, 10),
                                                1.0))) != NumericLimits<uint32>::Max());
        }
    }
    for (uint32 index = 0; index < 140; ++index) {
        REQUIRE(harness.Remove(index));
    }
    REQUIRE(harness.Add(SimpleDesc(Cube(Vec3d(1.0e13, 0, 0), 1.0))) !=
            NumericLimits<uint32>::Max());
    FlatBoundsView view = EverythingView();
    const CompareCounts counts = harness.Check(Span<const FlatBoundsView>(&view, 1));
    CHECK(counts.misses == 0);
    CHECK(counts.violations == 0);

    FlatBoundsSet& set = harness.Set();
    Array<uint64> words(allocator);
    Array<uint64> second(allocator);
    SystemAllocator scratch;
    FlatVisibility out;
    {
        FlatBoundsReadScope read(set);
        const FlatBoundsReader reader = read.Reader();
        const uint32 wordCount = reader.WordCount();
        REQUIRE(words.TryResize(wordCount + 8));
        REQUIRE(second.TryResize(wordCount));
        for (usize index = 0; index < words.Size(); ++index) {
            words[index] = 0x5A5Au;
        }
        const Span<const FlatBoundsView> one(&view, 1);
        // out이 짧음.
        FlatVisibility shortOut{Span<uint64>(words.Data(), wordCount - 1), 7};
        auto failed = reader.QueryViews(one, Span<FlatVisibility>(&shortOut, 1), nullptr, scratch);
        CHECK(!failed && failed.Error() == ErrorCode::InvalidArgument);
        CHECK(words[0] == 0x5A5Au && shortOut.epoch == 7);
        // 개수 불일치.
        FlatVisibility pair[2] = {{Span<uint64>(words.Data(), wordCount), 0},
                                  {Span<uint64>(second.Data(), wordCount), 0}};
        failed = reader.QueryViews(one, Span<FlatVisibility>(pair, 2), nullptr, scratch);
        CHECK(!failed && failed.Error() == ErrorCode::InvalidArgument);
        // 겹침.
        const FlatBoundsView two[2] = {view, view};
        FlatVisibility overlapping[2] = {{Span<uint64>(words.Data(), wordCount), 0},
                                         {Span<uint64>(words.Data() + 1, wordCount), 0}};
        failed = reader.QueryViews(Span<const FlatBoundsView>(two, 2),
                                   Span<FlatVisibility>(overlapping, 2), nullptr, scratch);
        CHECK(!failed && failed.Error() == ErrorCode::InvalidArgument);
        CHECK(words[0] == 0x5A5Au);
        // 잘못된 뷰: 빈 volume, 비유한 기준점, 배율 0·음수·NaN·Inf.
        FlatBoundsView invalid[5] = {view, view, view, view, view};
        invalid[0].volume = ConvexVolume<double>();
        invalid[1].distanceOrigin.x = platform::PlatformMath::QuietNaN<double>();
        invalid[2].distanceScale = 0.0f;
        invalid[3].distanceScale = -1.0f;
        invalid[4].distanceScale = platform::PlatformMath::Infinity<float>();
        for (const FlatBoundsView& bad : invalid) {
            FlatVisibility single{Span<uint64>(words.Data(), wordCount), 0};
            failed = reader.QueryViews(Span<const FlatBoundsView>(&bad, 1),
                                       Span<FlatVisibility>(&single, 1), nullptr, scratch);
            CHECK(!failed && failed.Error() == ErrorCode::InvalidArgument);
        }
        CHECK(words[0] == 0x5A5Au);
        // 뷰 0개는 성공(no-op).
        CHECK(reader.QueryViews(Span<const FlatBoundsView>(), Span<FlatVisibility>(), nullptr,
                                scratch));
        // 정상 질의 뒤 WordCount 밖의 word는 그대로다.
        out = FlatVisibility{Span<uint64>(words.Data(), words.Size()), 0};
        REQUIRE(reader.QueryViews(one, Span<FlatVisibility>(&out, 1), nullptr, scratch));
        CHECK(words[wordCount] == 0x5A5Au);
        // 짧은 bits는 ForEachVisible에서도 거부한다.
        FlatVisibility truncated{Span<uint64>(words.Data(), wordCount - 1), out.epoch};
        auto visited = reader.ForEachVisible(
            truncated, [](const FlatVisibleElement&) noexcept { return QueryControl::Continue; });
        CHECK(!visited && visited.Error() == ErrorCode::InvalidArgument);
        CHECK(reader.ForEachVisible(
            out, [](const FlatVisibleElement&) noexcept { return QueryControl::Stop; }));
        CHECK(!reader.TryUserValueAt(FlatBoundsListSlotBase + 5));
        CHECK(!reader.TryUserValueAt(0)); // 해제된 page의 슬롯
    }
    // 변경 뒤 이전 결과는 낡았다.
    REQUIRE(set.TrySetUserValue(harness.Model()[200].handle, 9));
    FlatBoundsReadScope read(set);
    auto stale = read.Reader().ForEachVisible(
        out, [](const FlatVisibleElement&) noexcept { return QueryControl::Continue; });
    CHECK(!stale && stale.Error() == ErrorCode::StaleHandle);
}

// FB06: 여러 뷰 한 번 = 뷰별 단독, scheduler 있음 = 없음(비트 단위 일치). page 용량 256·1,024도
// 본다.
TEST(FlatBounds_FB06_MultiViewAndParallelMatchSingle) {
    SystemAllocator allocator;
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig schedulerConfig;
    schedulerConfig.workerCount = 3;
    REQUIRE(scheduler.Start(schedulerConfig));
    const uint32 capacities[] = {64, 256, 1024};
    for (const uint32 capacity : capacities) {
        FlatBoundsConfig config = SmallTileConfig();
        config.pageCapacity = capacity;
        Harness harness(config, allocator);
        RandomStream random(0xFB06u ^ capacity ^ test::CurrentSeed());
        for (uint32 index = 0; index < 5000; ++index) {
            REQUIRE(harness.Add(RandomDesc(random, index)) != NumericLimits<uint32>::Max());
        }
        constexpr uint32 Views = 20;
        FlatBoundsView views[Views];
        for (FlatBoundsView& view : views) {
            view = RandomView(random);
        }
        FlatBoundsReadScope read(harness.Set());
        const FlatBoundsReader reader = read.Reader();
        const uint32 wordCount = reader.WordCount();
        Array<uint64> together(allocator);
        Array<uint64> parallel(allocator);
        Array<uint64> single(allocator);
        REQUIRE(together.TryResize(static_cast<usize>(wordCount) * Views));
        REQUIRE(parallel.TryResize(static_cast<usize>(wordCount) * Views));
        REQUIRE(single.TryResize(wordCount));
        FlatVisibility togetherOut[Views];
        FlatVisibility parallelOut[Views];
        for (uint32 index = 0; index < Views; ++index) {
            togetherOut[index].words = Span<uint64>(together.Data() + index * wordCount, wordCount);
            parallelOut[index].words = Span<uint64>(parallel.Data() + index * wordCount, wordCount);
        }
        SystemAllocator scratch;
        FlatQueryStats stats;
        REQUIRE(reader.QueryViews(Span<const FlatBoundsView>(views, Views),
                                  Span<FlatVisibility>(togetherOut, Views), nullptr, scratch,
                                  &stats));
        REQUIRE(reader.QueryViews(Span<const FlatBoundsView>(views, Views),
                                  Span<FlatVisibility>(parallelOut, Views), &scheduler, scratch));
        CHECK(stats.pageViews > 0 && stats.visibleElements > 0);
        uint32 mismatches = 0;
        for (uint32 index = 0; index < Views; ++index) {
            FlatVisibility alone{Span<uint64>(single.Data(), wordCount), 0};
            REQUIRE(reader.QueryViews(Span<const FlatBoundsView>(&views[index], 1),
                                      Span<FlatVisibility>(&alone, 1), &scheduler, scratch));
            for (uint32 word = 0; word < wordCount; ++word) {
                mismatches += togetherOut[index].words[word] != single[word] ? 1u : 0u;
                mismatches += parallelOut[index].words[word] != single[word] ? 1u : 0u;
            }
        }
        CHECK(mismatches == 0);
    }
    REQUIRE(scheduler.Shutdown(WorkerShutdownMode::Drain));
}

// FB07: 실패 주입(N번째 할당), 세대 retire, Clear·이동 뒤 handle, 설정 검증이다.
TEST(FlatBounds_FB07_FailureInjectionHandlesAndConfig) {
    SystemAllocator system;
    test::NFailAllocator allocator(system);
    FlatBoundsConfig config = SmallTileConfig();
    Harness harness(config, allocator);
    RandomStream random(0xFB07u ^ test::CurrentSeed());
    uint64 failures = 0;
    for (uint32 operation = 0; operation < 600; ++operation) {
        const FlatBoundsDesc desc = RandomDesc(random, operation);
        // 첫 시도부터 차례로 실패시켜 모든 할당 지점을 지난다. 실패하면 논리 상태가 그대로다.
        for (uint64 attempt = 1;; ++attempt) {
            allocator.FailOnAttempt(attempt);
            const uint32 index = harness.Add(desc);
            allocator.DisableFailure();
            if (index != NumericLimits<uint32>::Max()) {
                break;
            }
            ++failures;
            REQUIRE(attempt < 64);
        }
        if (operation % 3 == 0 && harness.Model().Size() > 1) {
            const uint32 index =
                random.NextUInt32Below(static_cast<uint32>(harness.Model().Size()));
            if (harness.Model()[index].live) {
                const Box<double> target = RandomDesc(random, operation + 1).bounds;
                allocator.FailOnAttempt(1);
                const bool moved = harness.Update(index, target);
                allocator.DisableFailure();
                if (!moved) {
                    ++failures;
                    REQUIRE(harness.Update(index, target));
                }
            }
        }
    }
    CHECK(failures > 0);
    FlatBoundsView view = EverythingView();
    CompareCounts counts = harness.Check(Span<const FlatBoundsView>(&view, 1));
    CHECK(counts.misses == 0);
    CHECK(counts.violations == 0);

    // 세대 retire: 세대 상한 2이면 같은 slot을 두 번 쓰면 폐기된다. 낡은 handle은 StaleHandle.
    FlatBoundsConfig retire = SmallTileConfig();
    retire.maxGeneration = 2;
    FlatBoundsSet small = MakeSet(retire, system);
    FlatBoundsHandle previous;
    for (uint32 round = 0; round < 6; ++round) {
        auto added = small.TryAdd(SimpleDesc(Cube(Vec3d(5, 5, 5), 1)));
        REQUIRE(added);
        if (!previous.IsNull()) {
            auto stale = small.TryRemove(previous);
            CHECK(!stale && stale.Error() == ErrorCode::StaleHandle);
        }
        REQUIRE(small.TryRemove(added.Value()));
        previous = added.Value();
    }
    auto empty = small.TryRemove(FlatBoundsHandle{});
    CHECK(!empty && empty.Error() == ErrorCode::InvalidArgument);

    // Clear 뒤 handle은 낡고, 이동 뒤 handle은 대상에서 유효하다.
    auto kept = small.TryAdd(SimpleDesc(Cube(Vec3d(5, 5, 5), 1)));
    REQUIRE(kept);
    FlatBoundsSet moved(Move(small));
    {
        FlatBoundsReadScope read(moved);
        CHECK(read.Reader().IsValid(kept.Value()));
        CHECK(read.Reader().Count() == 1);
    }
    {
        FlatBoundsReadScope read(small);
        CHECK(read.Reader().Count() == 0);
        CHECK(read.Reader().WordCount() == 0);
    }
    moved.Clear();
    {
        FlatBoundsReadScope read(moved);
        CHECK(!read.Reader().IsValid(kept.Value()));
        auto report = read.Reader().Validate();
        CHECK(report && report.Value().IsValid());
    }

    // 설정 검증.
    const uint32 badCapacities[] = {0, 32, 100, 65536 + 64};
    for (const uint32 capacity : badCapacities) {
        FlatBoundsConfig bad = SmallTileConfig();
        bad.pageCapacity = capacity;
        auto created = FlatBoundsSet::TryCreate(bad, system);
        CHECK(!created && created.Error() == ErrorCode::InvalidArgument);
    }
    FlatBoundsConfig bad = SmallTileConfig();
    bad.tileSize = 1024.5;
    CHECK(!FlatBoundsSet::TryCreate(bad, system));
    bad = SmallTileConfig();
    bad.anchor.x = 0.5;
    CHECK(!FlatBoundsSet::TryCreate(bad, system));
    bad = SmallTileConfig();
    bad.largeRelativeExtent = 0.0;
    CHECK(!FlatBoundsSet::TryCreate(bad, system));
    bad = SmallTileConfig();
    bad.maxElements = 0;
    CHECK(!FlatBoundsSet::TryCreate(bad, system));
    FlatBoundsConfig good = SmallTileConfig();
    good.pageCapacity = 128;
    CHECK(FlatBoundsSet::TryCreate(good, system));
    // 원소 수 한도.
    good.maxElements = 2;
    FlatBoundsSet limited = MakeSet(good, system);
    REQUIRE(limited.TryAdd(SimpleDesc(Cube(Vec3d(1, 1, 1), 1))));
    REQUIRE(limited.TryAdd(SimpleDesc(Cube(Vec3d(1, 1, 1), 1))));
    auto full = limited.TryAdd(SimpleDesc(Cube(Vec3d(1, 1, 1), 1)));
    CHECK(!full && full.Error() == ErrorCode::CapacityExceeded);
    auto reserve = limited.TryReserve(3);
    CHECK(!reserve && reserve.Error() == ErrorCode::CapacityExceeded);
    // 유효하지 않은 AABB.
    auto invalidBounds =
        limited.TryUpdateBounds(FlatBoundsHandle{}, Box<double>(Vec3d(1, 1, 1), Vec3d(0, 0, 0)));
    CHECK(!invalidBounds && invalidBounds.Error() == ErrorCode::InvalidArgument);
}

// FB09: FTZ·DAZ 상태에서도 누락이 없다. 상대 좌표가 정확히 0이거나 float 비정규 범위인 원소와 그
// 꼭짓점을 지나는 평면, 기준점 근처의 거리 판정을 쓴다.
TEST(FlatBounds_FB09_FlushToZeroKeepsConservative) {
    SystemAllocator allocator;
    FlatBoundsConfig config = SmallTileConfig();
    RandomStream random(0xFB09u ^ test::CurrentSeed());
    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                platform::detail::DenormalsAreZeroBit());
    CompareCounts total;
    {
        Harness harness(config, allocator);
        Array<FlatBoundsView> views(allocator);
        for (uint32 index = 0; index < 400; ++index) {
            const double tiny = index % 2 == 0 ? 0.0 : 2.0e-40 * (1 + random.NextUInt32Below(1000));
            const Vec3d origin(1024.0 * random.NextInt32InRange(-3, 3), 0.0, 1024.0);
            FlatBoundsDesc desc = SimpleDesc(Box<double>(origin + Vec3d(tiny, 0.0, tiny),
                                                         origin + Vec3d(tiny * 3, tiny, 1.0e-3)));
            desc.maxDrawDistance = index % 3 == 0 ? 1.0e-3f : 0.0f;
            REQUIRE(harness.Add(desc) != NumericLimits<uint32>::Max());
            const Vec3d normal = RandomDirection(random);
            const Vec3d highest(normal.x >= 0 ? desc.bounds.max.x : desc.bounds.min.x,
                                normal.y >= 0 ? desc.bounds.max.y : desc.bounds.min.y,
                                normal.z >= 0 ? desc.bounds.max.z : desc.bounds.min.z);
            const Plane<double> planes[] = {Plane<double>(normal, Dot(normal, highest))};
            FlatBoundsView view;
            view.volume = VolumeOf(planes);
            view.filterMask = 1;
            view.distanceOrigin = desc.bounds.min;
            (void)views.TryPushBack(view);
        }
        for (usize start = 0; start < views.Size(); start += 16) {
            const CompareCounts counts =
                harness.Check(Span<const FlatBoundsView>(views.Data() + start, 16));
            total.misses += counts.misses;
            total.violations += counts.violations;
        }
    }
    platform::detail::WriteFloatingPointControl(original);
    CHECK(total.misses == 0);
    CHECK(total.violations == 0);
}
