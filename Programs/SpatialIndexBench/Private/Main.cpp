#include "BenchInput.h"
#include "BenchSupport.h"
#include "FlatScan.h"
#include "Harness.h"
#include "LooseOctreeAdapter.h"
#include "SceneOctree.h"
#include "UnityOctreePort.h"

#include <Base/Compiler.h>
#include <Core/Algorithms.h>
#include <Core/Math/RandomStream.h>
#include <Platform/Clock.h>
#include <Platform/Cpu.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

#include <stdio.h>

// 공간 인덱스 벤치마크(Docs/Design/SpatialIndex.ko.md 9절, OT5, SIP8)다. 비교군 하나를 한
// 프로세스에서 재고 원시 결과를 JSON으로 쓴다. 비교군 사이 순서 회전·반복은 실행 스크립트가
// 프로세스를 나눠 한다.
//
// 비교군(--group):
//   A1    조밀한 AoS double 전수 검색(scalar)
//   A2    float SoA + SIMD(SIP3) 전수 검색, D와 같은 정밀도·필터 선적용
//   A2s   A2와 같은 데이터의 scalar 판정(SIMD 효과 분리용 변형)
//   B     UnityOctree C++ 이식(bbc4735, looseness 2, minNodeSize 128 cm, 초기 131072 cm)
//   C16   TOctree2 설계 기반 독립 구현, 잎 용량 16(Unreal의 성능이 아니다)
//   C256  같은 구현, 잎 용량 256
//   D     LooseOctree(6.2절 초기 제안값)
//   D-k1 D-k65_64 D-k5_4 D-k3_2 D-s8 D-s32 D-s64 D-noinplace D-immediate D-boxdouble
//         9.4절 단계별 효과 변형(기준 D에서 한 요인씩)
//
// 사용:
//   SpatialIndexBench --group D --out d.json                    기본 사례 전체(8분포 × 10^3–10^5,
//   대표 4분포 × 10^6) SpatialIndexBench --group D --profile variant --out v.json  변형 측정용
//   부분집합(대표 4분포 × 10^4·10^6) SpatialIndexBench --group D --counters --out c.json 계수
//   모드(방문·검사·후보 수, 구조·메모리; 시간 없음)
//   --dists uniform,clustered   --sizes 1000,10000   --seed N   --scenarios-only   --no-scenarios
//
// 결과는 기기·빌드 구성에 따라 다르다. Release로 빌드해 같은 기기에서 비교한다.
namespace {

using namespace rocks;
using namespace rocks::spatialbench;

constexpr uint64 DefaultSeed = 20261118;
constexpr uint32 MaxCases = 64;
constexpr uint32 FrameLightCount = 64;
constexpr uint32 FrameCasterCount = 4;
constexpr uint32 FrameMaintenanceOperations = 256;

struct CaseSpec {
    Distribution distribution;
    uint32 size;
};

struct Options {
    const char* group = nullptr;
    const char* out = nullptr;
    const char* profile = "full";
    bool counters = false;
    bool scenarios = true;
    bool scenariosOnly = false;
    uint64 seed = DefaultSeed;
    CaseSpec cases[MaxCases];
    uint32 caseCount = 0;
    Distribution dists[static_cast<uint32>(Distribution::Count)];
    uint32 distCount = 0;
    uint32 sizes[8];
    uint32 sizeCount = 0;
};

bool ParseUnsigned(const char* text, uint64& value) noexcept {
    value = 0;
    if (*text == '\0') {
        return false;
    }
    for (const char* digit = text; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9' || value > NumericLimits<uint64>::Max() / 10) {
            return false;
        }
        value = value * 10 + static_cast<uint64>(*digit - '0');
    }
    return true;
}

// 쉼표 목록을 하나씩 넘긴다. token 버퍼에 복사한다.
template <typename Visit>
bool ForEachToken(const char* list, Visit&& visit) noexcept {
    char token[64];
    usize length = 0;
    for (const char* cursor = list;; ++cursor) {
        if (*cursor == ',' || *cursor == '\0') {
            token[length] = '\0';
            if (length == 0 || !visit(static_cast<const char*>(token))) {
                return false;
            }
            length = 0;
            if (*cursor == '\0') {
                return true;
            }
            continue;
        }
        if (length + 1 >= sizeof(token)) {
            return false;
        }
        token[length++] = *cursor;
    }
}

bool Parse(int argc, char** argv, Options& options) noexcept {
    for (int index = 1; index < argc; ++index) {
        const bool hasValue = index + 1 < argc;
        if (platform::CStringEquals(argv[index], "--group") && hasValue) {
            options.group = argv[++index];
        } else if (platform::CStringEquals(argv[index], "--out") && hasValue) {
            options.out = argv[++index];
        } else if (platform::CStringEquals(argv[index], "--profile") && hasValue) {
            options.profile = argv[++index];
        } else if (platform::CStringEquals(argv[index], "--counters")) {
            options.counters = true;
        } else if (platform::CStringEquals(argv[index], "--no-scenarios")) {
            options.scenarios = false;
        } else if (platform::CStringEquals(argv[index], "--scenarios-only")) {
            options.scenariosOnly = true;
        } else if (platform::CStringEquals(argv[index], "--seed") && hasValue) {
            if (!ParseUnsigned(argv[++index], options.seed)) {
                return false;
            }
        } else if (platform::CStringEquals(argv[index], "--dists") && hasValue) {
            if (!ForEachToken(argv[++index], [&](const char* token) noexcept {
                    Distribution distribution;
                    if (!ParseDistribution(token, distribution) ||
                        options.distCount >= static_cast<uint32>(Distribution::Count)) {
                        return false;
                    }
                    options.dists[options.distCount++] = distribution;
                    return true;
                })) {
                return false;
            }
        } else if (platform::CStringEquals(argv[index], "--sizes") && hasValue) {
            if (!ForEachToken(argv[++index], [&](const char* token) noexcept {
                    uint64 value = 0;
                    if (!ParseUnsigned(token, value) || value < 16 || value > 1000000 ||
                        options.sizeCount >= 8) {
                        return false;
                    }
                    options.sizes[options.sizeCount++] = static_cast<uint32>(value);
                    return true;
                })) {
                return false;
            }
        } else {
            return false;
        }
    }
    return options.group != nullptr && options.out != nullptr;
}

void AddCase(Options& options, Distribution distribution, uint32 size) noexcept {
    if (options.caseCount < MaxCases) {
        options.cases[options.caseCount++] = CaseSpec{distribution, size};
    }
}

constexpr Distribution RepresentativeDistributions[] = {
    Distribution::Uniform, Distribution::Clustered, Distribution::SizeVaried,
    Distribution::OverlapDense};

// 사례 목록을 정한다. full은 8분포 × 10^3–10^5, large는 대표 4분포(9.4절 대표 부분집합) × 10^6이다.
// 10^6은 실행 시간 때문에 대표 분포만 잰다.
void BuildCases(Options& options) noexcept {
    if (options.distCount != 0 || options.sizeCount != 0) {
        const uint32 distCount =
            options.distCount != 0 ? options.distCount : static_cast<uint32>(Distribution::Count);
        for (uint32 size = 0; size < (options.sizeCount != 0 ? options.sizeCount : 1); ++size) {
            for (uint32 dist = 0; dist < distCount; ++dist) {
                AddCase(options,
                        options.distCount != 0 ? options.dists[dist]
                                               : static_cast<Distribution>(dist),
                        options.sizeCount != 0 ? options.sizes[size] : 10000);
            }
        }
        return;
    }
    if (platform::CStringEquals(options.profile, "variant")) {
        constexpr uint32 VariantSizes[] = {10000u, 1000000u};
        for (const uint32 size : VariantSizes) {
            for (const Distribution distribution : RepresentativeDistributions) {
                AddCase(options, distribution, size);
            }
        }
        return;
    }
    if (platform::CStringEquals(options.profile, "large")) {
        for (const Distribution distribution : RepresentativeDistributions) {
            AddCase(options, distribution, 1000000);
        }
        return;
    }
    constexpr uint32 FullSizes[] = {1000u, 10000u, 100000u};
    for (const uint32 size : FullSizes) {
        for (uint32 dist = 0; dist < static_cast<uint32>(Distribution::Count); ++dist) {
            AddCase(options, static_cast<Distribution>(dist), size);
        }
    }
}

// 비교군 C의 고정 루트다. 데이터 영역(이동 범위 포함)을 덮는 128 cm · 2^k 반폭, 중심은 128 cm
// 격자로 내린 영역 중심이다(노드 경계 계산이 정확하도록).
void SceneRootFor(const Dataset& dataset, double (&center)[3], double& extent) noexcept {
    const double cx = dataset.domain.min.x * 0.5 + dataset.domain.max.x * 0.5;
    const double cy = dataset.domain.min.y * 0.5 + dataset.domain.max.y * 0.5;
    const double cz = dataset.domain.min.z * 0.5 + dataset.domain.max.z * 0.5;
    center[0] = platform::PlatformMath::Floor(cx / 128.0) * 128.0;
    center[1] = platform::PlatformMath::Floor(cy / 128.0) * 128.0;
    center[2] = platform::PlatformMath::Floor(cz / 128.0) * 128.0;
    double need = 0.0;
    const double distances[6] = {
        center[0] - dataset.domain.min.x, dataset.domain.max.x - center[0],
        center[1] - dataset.domain.min.y, dataset.domain.max.y - center[1],
        center[2] - dataset.domain.min.z, dataset.domain.max.z - center[2]};
    for (const double distance : distances) {
        need = distance > need ? distance : need;
    }
    // 크기 변경(최대 2배)과 큰 프록시가 루트 밖으로 나가지 않을 여유다.
    need += 4.0 * dataset.maxHalf + 1000.0;
    extent = 128.0;
    while (extent < need) {
        extent *= 2.0;
    }
}

// ---- 실행 문맥 ----
struct Context {
    Options* options;
    BenchAllocator* allocator;
    JsonWriter* json;
    Array<uint32>* resultStorage;
    Array<uint32>* leftIds;
    Array<uint32>* rightIds;
    Array<uint32>* referenceStorage;
};

ResultBuffer MakeBuffer(Array<uint32>& storage) noexcept {
    ResultBuffer buffer;
    buffer.ids = storage.Data();
    buffer.capacity = static_cast<uint32>(storage.Size());
    return buffer;
}

void MakeOrder(uint32 size, uint64 seed, Array<uint32>& order) noexcept {
    order.Clear();
    (void)order.TryResize(size);
    for (uint32 index = 0; index < size; ++index) {
        order[index] = index;
    }
    RandomStream random(seed, 0x0dull);
    for (uint32 index = size; index > 1; --index) {
        const uint32 pick = random.NextUInt32Below(index);
        const uint32 swap = order[index - 1];
        order[index - 1] = order[pick];
        order[pick] = swap;
    }
}

template <typename Index>
void TimeQueries(Index& index, const CaseInput& input, QueryKind kind, QueryClass queryClass,
                 const char* state, Context& context) noexcept {
    const QuerySet& set = *input.queries[static_cast<uint32>(queryClass)];
    const uint32 count = input.plan.queryCount;
    const uint32 batch = input.plan.queryBatch;
    SampleSet samples(*context.allocator);
    ResultBuffer out = MakeBuffer(*context.resultStorage);
    QueryCounters counters;
    uint64 candidates = 0;
    uint64 checksum = 0;
    index.BeginRead();
    // 워밍업: 첫 묶음을 재지 않고 한 번 돈다.
    for (uint32 position = 0; position < batch && position < count; ++position) {
        out.Reset();
        (void)RunQuery<false>(index, kind, set, position, out, counters);
    }
    Stopwatch watch;
    for (uint32 start = 0; start + batch <= count; start += batch) {
        uint64 batchCandidates = 0;
        uint64 batchChecksum = 0;
        watch.Start();
        for (uint32 position = start; position < start + batch; ++position) {
            out.Reset();
            batchCandidates += RunQuery<false>(index, kind, set, position, out, counters);
            batchChecksum += out.checksum;
        }
        const uint64 elapsed = watch.Elapsed();
        (void)samples.Add(elapsed, batch);
        candidates += batchCandidates;
        checksum += batchChecksum;
    }
    index.EndRead();
    JsonWriter& json = *context.json;
    json.BeginObject();
    json.Field("state", state);
    json.Field("kind", QueryKindName(kind));
    json.Field("class", QueryClassName(queryClass));
    json.FieldInteger("queries", count);
    json.Summary("time", samples.Summarize());
    json.FieldInteger("candidates", candidates);
    json.FieldInteger("checksum", checksum);
    json.EndObject();
}

template <typename Index>
void CountQueries(Index& index, const CaseInput& input, QueryKind kind, QueryClass queryClass,
                  const char* state, Context& context) noexcept {
    const QuerySet& set = *input.queries[static_cast<uint32>(queryClass)];
    ResultBuffer out = MakeBuffer(*context.resultStorage);
    QueryCounters counters;
    uint64 candidates = 0;
    index.BeginRead();
    for (uint32 position = 0; position < input.plan.queryCount; ++position) {
        out.Reset();
        candidates += RunQuery<true>(index, kind, set, position, out, counters);
    }
    index.EndRead();
    JsonWriter& json = *context.json;
    json.BeginObject();
    json.Field("state", state);
    json.Field("kind", QueryKindName(kind));
    json.Field("class", QueryClassName(queryClass));
    json.FieldInteger("queries", input.plan.queryCount);
    json.FieldInteger("visitedNodes", counters.visitedNodes);
    json.FieldInteger("testedProxies", counters.testedProxies);
    json.FieldInteger("containedProxies", counters.containedProxies);
    json.FieldInteger("results", counters.results);
    json.FieldInteger("candidates", candidates);
    json.EndObject();
}

template <typename Index>
void MeasureQueries(Index& index, const CaseInput& input, const char* state, bool allKinds,
                    Context& context) noexcept {
    const QueryKind reducedKinds[] = {QueryKind::Box, QueryKind::Sphere};
    const uint32 kindCount = allKinds ? static_cast<uint32>(QueryKind::Count) : 2;
    for (uint32 kindIndex = 0; kindIndex < kindCount; ++kindIndex) {
        const QueryKind kind =
            allKinds ? static_cast<QueryKind>(kindIndex) : reducedKinds[kindIndex];
        for (uint32 classIndex = 0; classIndex < static_cast<uint32>(QueryClass::Count);
             ++classIndex) {
            const QueryClass queryClass = static_cast<QueryClass>(classIndex);
            if (!allKinds && queryClass == QueryClass::Empty) {
                continue;
            }
            if (context.options->counters) {
                CountQueries(index, input, kind, queryClass, state, context);
            } else {
                TimeQueries(index, input, kind, queryClass, state, context);
            }
        }
    }
}

template <typename Index>
bool Verify(Index& index, const CaseInput& input, Span<const Box<double>> current,
            const GroupTraits& traits, const char* state, Context& context) noexcept {
    FlatScanDouble reference(*context.allocator);
    if (!reference.Create(input.size)) {
        return false;
    }
    for (uint32 id = 0; id < input.size; ++id) {
        (void)reference.Insert(id, current[id], ProxyFilter(id));
    }
    ResultBuffer left = MakeBuffer(*context.referenceStorage);
    ResultBuffer right = MakeBuffer(*context.resultStorage);
    JsonWriter& json = *context.json;
    json.BeginObject();
    json.Field("state", state);
    bool ok = true;
    index.BeginRead();
    json.Key("kinds");
    json.BeginArray();
    for (uint32 kindIndex = 0; kindIndex < static_cast<uint32>(QueryKind::Count); ++kindIndex) {
        const QueryKind kind = static_cast<QueryKind>(kindIndex);
        VerifyTally tally;
        for (uint32 classIndex = 0; classIndex < static_cast<uint32>(QueryClass::Count);
             ++classIndex) {
            VerifyKind(index, reference, kind, *input.queries[classIndex], input.plan.verifyQueries,
                       current, left, right, *context.leftIds, *context.rightIds, tally);
        }
        const bool boxFamily =
            kind == QueryKind::Box || kind == QueryKind::Any || kind == QueryKind::CountBox;
        // 판정: 누락 0. 초과 후보는 float 박스 계열(A2·D)만 허용. 접촉 누락은 C만 허용.
        bool kindOk = tally.missing == 0 && tally.nearestMismatch == 0;
        if (!(traits.conservativeBox && boxFamily)) {
            kindOk = kindOk && tally.extra == 0;
        }
        if (!traits.touchMissAllowed) {
            kindOk = kindOk && tally.touchOnlyMissing == 0;
        }
        if (!(boxFamily && (traits.conservativeBox || traits.touchMissAllowed))) {
            kindOk = kindOk && tally.scalarMismatch == 0;
        }
        ok = ok && kindOk;
        json.BeginObject();
        json.Field("kind", QueryKindName(kind));
        json.FieldInteger("queries", tally.queries);
        json.FieldInteger("missing", tally.missing);
        json.FieldInteger("touchOnlyMissing", tally.touchOnlyMissing);
        json.FieldInteger("extra", tally.extra);
        json.FieldInteger("nearestMismatch", tally.nearestMismatch);
        json.FieldInteger("scalarMismatch", tally.scalarMismatch);
        json.FieldBool("ok", kindOk);
        json.EndObject();
    }
    json.EndArray();
    if constexpr (requires { index.Validate(); }) {
        const bool valid = index.Validate();
        json.FieldBool("validate", valid);
        ok = ok && valid;
    }
    index.EndRead();
    json.FieldBool("ok", ok);
    json.EndObject();
    reference.Destroy();
    if (!ok) {
        printf("  VERIFY FAILED (%s)\n", state);
    }
    return ok;
}

// 변경 연산 하나를 K 묶음으로 잰다. apply(id, 새 경계 색인)가 거짓이면 실패로 센다.
template <typename Apply>
void TimeMutations(const char* name, uint32 count, uint32 batch, Context& context,
                   Apply&& apply) noexcept {
    SampleSet samples(*context.allocator);
    uint64 failures = 0;
    Stopwatch watch;
    for (uint32 start = 0; start < count; start += batch) {
        const uint32 end = start + batch < count ? start + batch : count;
        watch.Start();
        for (uint32 position = start; position < end; ++position) {
            failures += apply(position) ? 0 : 1;
        }
        (void)samples.Add(watch.Elapsed(), end - start);
    }
    JsonWriter& json = *context.json;
    json.BeginObject();
    json.Field("op", name);
    json.Summary("time", samples.Summarize());
    json.FieldInteger("failures", failures);
    json.EndObject();
}

template <typename Index>
void Describe(Index& index, Context& context) noexcept {
    JsonWriter& json = *context.json;
    json.Key("structure");
    json.BeginObject();
    index.Describe(json);
    json.FieldInteger("allocatorLiveBytes", context.allocator->liveBytes);
    json.FieldInteger("allocatorPeakBytes", context.allocator->peakBytes);
    json.FieldInteger("allocatorAllocations", context.allocator->allocations);
    json.FieldInteger("allocatorDeallocations", context.allocator->deallocations);
    json.EndObject();
}

// 프레임 부하 한 설정(9.3절): 이동 비율 r의 갱신 + 광원 L개 구 질의 + caster 프러스텀 4개 + 작업 수
// 예산 유지보수(256)를 한 프레임으로 잰다.
template <typename Index>
void RunFrames(Index& index, const CaseInput& input, Array<Box<double>>& current,
               uint32 ratioPercentTimes10, uint32 lights, Context& context, auto&& statsBefore,
               auto&& statsAfter) noexcept {
    Allocator& allocator = *context.allocator;
    const uint32 moving =
        static_cast<uint32>(static_cast<uint64>(input.size) * ratioPercentTimes10 / 1000);
    SampleSet total(allocator);
    SampleSet update(allocator);
    SampleSet query(allocator);
    SampleSet maintenance(allocator);
    MoveBatch moves(allocator);
    Array<Sphere<double>> lightSpheres(allocator);
    Array<ConvexVolume<double>> casters(allocator);
    ResultBuffer out = MakeBuffer(*context.resultStorage);
    QueryCounters counters;
    uint64 candidates = 0;
    uint64 updateFailures = 0;
    statsBefore();
    for (uint32 frame = 0; frame < input.plan.frames; ++frame) {
        const uint64 frameSeed =
            input.seed + 0xf000ull * (frame + 1) + ratioPercentTimes10 * 7 + lights;
        (void)GenerateMoves(*input.dataset, current.View(), MoveKind::Walk, moving, frameSeed,
                            moves);
        RandomStream random(frameSeed, 0x11ull);
        lightSpheres.Clear();
        for (uint32 light = 0; light < lights; ++light) {
            const Box<double>& item = current[random.NextUInt32Below(input.size)];
            const double radius = platform::PlatformMath::Exp(
                platform::PlatformMath::Log(300.0) +
                (platform::PlatformMath::Log(3000.0) - platform::PlatformMath::Log(300.0)) *
                    random.NextDouble01());
            (void)lightSpheres.TryPushBack(Sphere<double>(
                Vec3d(item.min.x * 0.5 + item.max.x * 0.5, item.min.y * 0.5 + item.max.y * 0.5,
                      item.min.z * 0.5 + item.max.z * 0.5),
                Quantize(radius)));
        }
        casters.Clear();
        const Box<double>& domain = input.dataset->domain;
        const double largest = (domain.max.x - domain.min.x) > (domain.max.y - domain.min.y)
                                   ? (domain.max.x - domain.min.x)
                                   : (domain.max.y - domain.min.y);
        for (uint32 caster = 0; caster < FrameCasterCount; ++caster) {
            const Vec3d apex(domain.min.x + (domain.max.x - domain.min.x) * random.NextDouble01(),
                             domain.min.y + (domain.max.y - domain.min.y) * random.NextDouble01(),
                             domain.min.z + (domain.max.z - domain.min.z) * random.NextDouble01());
            Vec3d direction(random.NextDouble01() - 0.5, random.NextDouble01() - 0.5,
                            random.NextDouble01() - 0.5);
            direction = direction.NormalizedOr(Vec3d(0.0, 0.0, -1.0));
            (void)casters.TryPushBack(MakeFrustum(apex, direction, 1.0, largest * 0.25));
        }

        const uint64 start = platform::MonotonicNanoseconds();
        for (usize move = 0; move < moves.ids.Size(); ++move) {
            const uint32 id = moves.ids[move];
            updateFailures += index.Update(id, current[id], moves.bounds[move]) ? 0 : 1;
            current[id] = moves.bounds[move];
        }
        const uint64 afterUpdate = platform::MonotonicNanoseconds();
        index.BeginRead();
        for (usize light = 0; light < lightSpheres.Size(); ++light) {
            out.Reset();
            index.template QuerySphere<false>(lightSpheres[light], QueryFilterMask, out, counters);
            candidates += out.count;
        }
        for (usize caster = 0; caster < casters.Size(); ++caster) {
            out.Reset();
            index.template QueryFrustum<false>(casters[caster], QueryFilterMask, out, counters);
            candidates += out.count;
        }
        index.EndRead();
        const uint64 afterQuery = platform::MonotonicNanoseconds();
        index.Maintain(FrameMaintenanceOperations);
        const uint64 end = platform::MonotonicNanoseconds();
        (void)total.Add(end - start, 1);
        (void)update.Add(afterUpdate - start, 1);
        (void)query.Add(afterQuery - afterUpdate, 1);
        (void)maintenance.Add(end - afterQuery, 1);
    }
    JsonWriter& json = *context.json;
    json.BeginObject();
    json.Field("moveRatio", static_cast<double>(ratioPercentTimes10) / 1000.0);
    json.FieldInteger("movingPerFrame", moving);
    json.FieldInteger("lights", lights);
    json.FieldInteger("casters", FrameCasterCount);
    json.FieldInteger("maintenanceOperations", FrameMaintenanceOperations);
    json.FieldInteger("frames", input.plan.frames);
    json.Summary("total", total.Summarize());
    json.Summary("update", update.Summarize());
    json.Summary("query", query.Summarize());
    json.Summary("maintenance", maintenance.Summarize());
    json.FieldInteger("candidates", candidates);
    json.FieldInteger("updateFailures", updateFailures);
    statsAfter();
    json.EndObject();
}

// D의 누적 계수 변화를 쓴다. 다른 비교군은 아무것도 쓰지 않는다.
struct LooseStats {
    spatial::OctreeStats before;

    void Capture(LooseIndex& index) noexcept {
        before = index.Stats();
    }

    void Write(LooseIndex& index, JsonWriter& json) noexcept {
        const spatial::OctreeStats after = index.Stats();
        json.Key("structureDelta");
        json.BeginObject();
        json.FieldInteger("reinserts", after.reinserts - before.reinserts);
        json.FieldInteger("inPlaceUpdates", after.inPlaceUpdates - before.inPlaceUpdates);
        json.FieldInteger("splits", after.splits - before.splits);
        json.FieldInteger("merges", after.merges - before.merges);
        json.FieldInteger("reclaims", after.reclaims - before.reclaims);
        json.FieldInteger("relocatedProxies", after.relocatedProxies - before.relocatedProxies);
        json.FieldInteger("rootExpansions", after.rootExpansions - before.rootExpansions);
        json.FieldInteger("rootShrinks", after.rootShrinks - before.rootShrinks);
        json.FieldInteger("forcedMaintenanceRuns",
                          after.forcedMaintenanceRuns - before.forcedMaintenanceRuns);
        json.FieldInteger("pendingAfter", after.pendingTotal);
        json.EndObject();
    }
};

template <typename Index>
struct StatsHook {
    void Before(Index&) noexcept {
    }

    void After(Index&, JsonWriter&) noexcept {
    }
};

template <>
struct StatsHook<LooseIndex> {
    LooseStats stats;

    void Before(LooseIndex& index) noexcept {
        stats.Capture(index);
    }

    void After(LooseIndex& index, JsonWriter& json) noexcept {
        stats.Write(index, json);
    }
};

void Skip(JsonWriter& json, const char* what, const char* reason) noexcept {
    json.BeginObject();
    json.Field("section", what);
    json.Field("reason", reason);
    json.EndObject();
}

// 사례 하나(분포 × 규모)를 잰다.
template <typename Index>
void RunCase(Index& index, const CaseInput& input, const GroupTraits& traits,
             const Sections& sections, Context& context) noexcept {
    JsonWriter& json = *context.json;
    Allocator& allocator = *context.allocator;
    const Dataset& dataset = *input.dataset;
    const uint32 size = input.size;
    Array<Box<double>> current(allocator);
    (void)current.TryAppend(dataset.bounds.View());
    StatsHook<Index> hook;
    // B는 한 노드의 객체 List가 수만 개 이상으로 커지는 조건에서 삭제가 O(목록)(List.Remove의
    // 앞당김)이다. 겹치는 과밀 10^5 이상(모든 객체가 노드 하나), 10^6(고정 크기 루트의 loose 띠에
    // 수만 개가 루트에 직접 저장됨)이 그렇다. 그 조건에서 전체 삭제·대량 이동은 O(n^2)라 재지
    // 않는다(성능이 나빠지는 조건으로 보고). 이동 프레임은 1%만, 겹치는 과밀은 이동 없는 프레임만
    // 잰다.
    const bool overlapHeavy = traits.boundedRemoveRow &&
                              input.distribution == Distribution::OverlapDense && size >= 100000;
    const bool heavyMutationsSkipped = overlapHeavy || (traits.boundedRemoveRow && size >= 1000000);

    json.BeginObject();
    json.Field("distribution", DistributionName(input.distribution));
    json.FieldInteger("size", size);
    json.Key("plan");
    json.BeginObject();
    json.FieldInteger("queryCount", input.plan.queryCount);
    json.FieldInteger("queryBatch", input.plan.queryBatch);
    json.FieldInteger("mutationCount", input.plan.mutationCount);
    json.FieldInteger("mutationBatch", input.plan.mutationBatch);
    json.FieldInteger("frames", input.plan.frames);
    json.FieldInteger("verifyQueriesPerClass", input.plan.verifyQueries);
    json.EndObject();
    json.Key("domain");
    json.BeginArray();
    json.Number(dataset.domain.min.x);
    json.Number(dataset.domain.min.y);
    json.Number(dataset.domain.min.z);
    json.Number(dataset.domain.max.x);
    json.Number(dataset.domain.max.y);
    json.Number(dataset.domain.max.z);
    json.EndArray();

    printf("  %s n=%u\n", DistributionName(input.distribution), size);
    fflush(stdout);

    // ---- 구축 ----
    context.allocator->ResetCounters();
    const uint64 liveBeforeBuild = context.allocator->liveBytes;
    if (!index.Create(size)) {
        json.Field("error", "create failed");
        json.EndObject();
        return;
    }
    {
        SampleSet samples(allocator);
        uint64 failures = 0;
        const uint32 batch = 256;
        Stopwatch watch;
        const uint64 start = platform::MonotonicNanoseconds();
        for (uint32 first = 0; first < size; first += batch) {
            const uint32 end = first + batch < size ? first + batch : size;
            watch.Start();
            for (uint32 id = first; id < end; ++id) {
                failures += index.Insert(id, current[id], dataset.filters[id]) ? 0 : 1;
            }
            (void)samples.Add(watch.Elapsed(), end - first);
        }
        const uint64 buildNanoseconds = platform::MonotonicNanoseconds() - start;
        json.Key("build");
        json.BeginObject();
        json.FieldInteger("totalNs", buildNanoseconds);
        json.Summary("insert", samples.Summarize());
        json.FieldInteger("failures", failures);
        json.FieldInteger("liveBytes", context.allocator->liveBytes - liveBeforeBuild);
        json.FieldInteger("peakBytes", context.allocator->peakBytes - liveBeforeBuild);
        json.FieldInteger("allocations", context.allocator->allocations);
        json.EndObject();
    }
    Describe(index, context);

    // ---- 정확성 대조(타이밍 밖) ----
    json.Key("verification");
    json.BeginArray();
    (void)Verify(index, input, current.View(), traits, "built", context);
    json.EndArray();

    // ---- 질의(구축 직후) ----
    json.Key("queries");
    json.BeginArray();
    MeasureQueries(index, input, "built", sections.allQueries, context);

    Array<uint32> order(allocator);
    MakeOrder(size, input.seed ^ 0x5eedull, order);
    uint32 mutationCount = input.plan.mutationCount < size ? input.plan.mutationCount : size;
    if (heavyMutationsSkipped) {
        // 삭제 한 번이 O(n)이므로 연산별 측정 수를 줄인다.
        mutationCount = mutationCount < 256 ? mutationCount : 256;
    }
    json.EndArray();

    json.Key("skipped");
    json.BeginArray();
    if (heavyMutationsSkipped) {
        Skip(
            json,
            overlapHeavy ? "removeAll, treeStates, frames(moveRatio > 0), removeWithoutBounds"
                         : "removeAll, treeStates, frames(moveRatio >= 0.1), removeWithoutBounds",
            "B: one node list holds tens of thousands of objects (overlap-dense >= 1e5: all in one "
            "node; 1e6: fixed-size root keeps the loose-band objects), List.Remove is O(list) and "
            "the section would be O(n^2); per-operation rows use 256 operations");
    }
    json.EndArray();

    // ---- 변경 연산별(9.3절) ----
    json.Key("operations");
    json.BeginArray();
    if (sections.mutations && !context.options->counters) {
        TimeMutations("remove", mutationCount, input.plan.mutationBatch, context,
                      [&](uint32 position) noexcept {
                          const uint32 id = order[position];
                          return index.Remove(id, current[id]);
                      });
        TimeMutations("insert", mutationCount, input.plan.mutationBatch, context,
                      [&](uint32 position) noexcept {
                          const uint32 id = order[position];
                          return index.Insert(id, current[id], dataset.filters[id]);
                      });
        if constexpr (requires { index.RemoveWithoutBounds(0u); }) {
            if (!heavyMutationsSkipped) {
                // 경계 없는 삭제(DFS)는 트리 전체를 돌아 비싸므로 적게 잰다.
                const uint32 dfsCount = mutationCount < 512 ? mutationCount : 512;
                TimeMutations("removeWithoutBounds", dfsCount, 32, context,
                              [&](uint32 position) noexcept {
                                  return index.RemoveWithoutBounds(order[position]);
                              });
                for (uint32 position = 0; position < dfsCount; ++position) {
                    const uint32 id = order[position];
                    (void)index.Insert(id, current[id], dataset.filters[id]);
                }
            }
        }
        const MoveKind kinds[] = {MoveKind::Small, MoveKind::Cross, MoveKind::Teleport,
                                  MoveKind::Resize};
        MoveBatch moves(allocator);
        for (const MoveKind kind : kinds) {
            (void)GenerateMoves(dataset, current.View(), kind, mutationCount,
                                input.seed + 0x3000ull + static_cast<uint64>(kind), moves);
            char name[32];
            snprintf(name, sizeof(name), "update-%s", MoveKindName(kind));
            TimeMutations(name, static_cast<uint32>(moves.ids.Size()), input.plan.mutationBatch,
                          context, [&](uint32 position) noexcept {
                              const uint32 id = moves.ids[position];
                              const bool ok = index.Update(id, current[id], moves.bounds[position]);
                              current[id] = moves.bounds[position];
                              return ok;
                          });
        }
    }
    json.EndArray();

    // ---- 트리 상태(9.3절): 유지보수 대기 중, 유지보수 완료 ----
    json.Key("treeStates");
    json.BeginArray();
    if (sections.treeStates && !heavyMutationsSkipped) {
        MoveBatch moves(allocator);
        for (uint32 round = 0; round < 3; ++round) {
            (void)GenerateMoves(dataset, current.View(), MoveKind::Teleport, size / 10,
                                input.seed + 0x7000ull + round, moves);
            for (usize move = 0; move < moves.ids.Size(); ++move) {
                const uint32 id = moves.ids[move];
                (void)index.Update(id, current[id], moves.bounds[move]);
                current[id] = moves.bounds[move];
            }
        }
        if (traits.deferredMaintenance) {
            MeasureQueries(index, input, "pending", false, context);
        }
        const uint64 start = platform::MonotonicNanoseconds();
        index.MaintainAll();
        const uint64 maintainNanoseconds = platform::MonotonicNanoseconds() - start;
        MeasureQueries(index, input, "maintained", false, context);
        json.BeginObject();
        json.Field("state", "maintainAllAfterChurn");
        json.FieldInteger("totalNs", maintainNanoseconds);
        json.EndObject();
    }
    json.EndArray();
    json.Key("verificationAfterChurn");
    json.BeginArray();
    (void)Verify(index, input, current.View(), traits, "maintained", context);
    json.EndArray();
    if (context.options->counters) {
        Describe(index, context);
    }

    // ---- 프레임 부하(9.3절) ----
    json.Key("frames");
    json.BeginArray();
    if (sections.frames && !context.options->counters) {
        // 이동 비율(천분율): 0%, 1%, 10%, 100%.
        const uint32 ratios[] = {0, 10, 100, 1000};
        for (const uint32 ratio : ratios) {
            if (heavyMutationsSkipped && (overlapHeavy ? ratio > 0 : ratio > 10)) {
                continue;
            }
            RunFrames(
                index, input, current, ratio, FrameLightCount, context,
                [&]() noexcept { hook.Before(index); },
                [&]() noexcept { hook.After(index, json); });
        }
        if (sections.lightVariants && !heavyMutationsSkipped) {
            constexpr uint32 LightVariants[] = {16u, 256u};
            for (const uint32 lights : LightVariants) {
                RunFrames(
                    index, input, current, 100, lights, context,
                    [&]() noexcept { hook.Before(index); },
                    [&]() noexcept { hook.After(index, json); });
            }
        }
    }
    json.EndArray();

    // ---- 전체 삭제(대량 삭제) ----
    json.Key("teardown");
    json.BeginArray();
    if (sections.removeAll && !heavyMutationsSkipped && !context.options->counters) {
        TimeMutations("removeAll", size, 256, context, [&](uint32 position) noexcept {
            const uint32 id = order[position];
            return index.Remove(id, current[id]);
        });
        const uint64 start = platform::MonotonicNanoseconds();
        index.MaintainAll();
        json.BeginObject();
        json.Field("op", "maintainAllAfterRemoveAll");
        json.FieldInteger("totalNs", platform::MonotonicNanoseconds() - start);
        json.EndObject();
    }
    json.EndArray();
    index.Destroy();
    json.EndObject();
}

// ---- 별도 시나리오(9.3절) ----

// 루트 확장·축소: 원점에서 바깥으로 지수적으로 멀어지는 프록시를 넣고(최대 약 163 km) 역순으로
// 뺀다.
template <typename Index>
void RunRootScenario(Index& index, Context& context) noexcept {
    constexpr uint32 Count = 20000;
    JsonWriter& json = *context.json;
    Allocator& allocator = *context.allocator;
    Array<Box<double>> bounds(allocator);
    RandomStream random(DefaultSeed, 0x700ull);
    double distance = 100.0;
    for (uint32 id = 0; id < Count; ++id) {
        Vec3d direction(random.NextDouble01() - 0.5, random.NextDouble01() - 0.5,
                        random.NextDouble01() - 0.5);
        direction = direction.NormalizedOr(Vec3d(1.0, 0.0, 0.0));
        const Vec3d center = direction * distance;
        (void)bounds.TryPushBack(
            Box<double>(Vec3d(Quantize(center.x - 100.0), Quantize(center.y - 100.0),
                              Quantize(center.z - 100.0)),
                        Vec3d(Quantize(center.x + 100.0), Quantize(center.y + 100.0),
                              Quantize(center.z + 100.0))));
        distance *= 1.0006;
    }
    json.BeginObject();
    json.Field("scenario", "rootGrowShrink");
    json.FieldInteger("size", Count);
    json.Field("maxDistance", distance);
    if (!index.Create(Count)) {
        json.Field("error", "create failed");
        json.EndObject();
        return;
    }
    StatsHook<Index> hook;
    hook.Before(index);
    json.Key("operations");
    json.BeginArray();
    TimeMutations("insertOutward", Count, 64, context, [&](uint32 id) noexcept {
        return index.Insert(id, bounds[id], ProxyFilter(id));
    });
    TimeMutations("removeInward", Count, 64, context, [&](uint32 position) noexcept {
        const uint32 id = Count - 1 - position;
        return index.Remove(id, bounds[id]);
    });
    const uint64 start = platform::MonotonicNanoseconds();
    index.MaintainAll();
    json.BeginObject();
    json.Field("op", "maintainAll");
    json.FieldInteger("totalNs", platform::MonotonicNanoseconds() - start);
    json.EndObject();
    json.EndArray();
    hook.After(index, json);
    Describe(index, context);
    index.Destroy();
    json.EndObject();
}

// 분할·병합 집중: 분리 가능한 과밀 10^4 위에서, 한 곳에 작은 프록시 24개를 넣었다 빼는 주기를
// 반복한다(분할 임계 부근). 주기마다 작업 수 예산 유지보수(16)를 포함한다.
constexpr uint32 SplitMergeBase = 10000;

template <typename Index>
void RunSplitMergeScenario(Index& index, const Dataset& dataset, Context& context) noexcept {
    constexpr uint32 Base = SplitMergeBase;
    constexpr uint32 Burst = 24;
    constexpr uint32 Cycles = 2000;
    JsonWriter& json = *context.json;
    Allocator& allocator = *context.allocator;
    json.BeginObject();
    json.Field("scenario", "splitMergeBurst");
    json.FieldInteger("base", Base);
    json.FieldInteger("burst", Burst);
    json.FieldInteger("cycles", Cycles);
    if (!index.Create(Base + Burst)) {
        json.Field("error", "create failed");
        json.EndObject();
        return;
    }
    for (uint32 id = 0; id < Base; ++id) {
        (void)index.Insert(id, dataset.bounds[id], dataset.filters[id]);
    }
    index.MaintainAll();
    StatsHook<Index> hook;
    hook.Before(index);
    RandomStream random(DefaultSeed, 0x5911ull);
    Box<double> burst[Burst];
    SampleSet samples(allocator);
    Stopwatch watch;
    for (uint32 cycle = 0; cycle < Cycles; ++cycle) {
        const Box<double>& anchor = dataset.bounds[random.NextUInt32Below(Base)];
        for (Box<double>& box : burst) {
            const Vec3d center(anchor.min.x + 40.0 * random.NextDouble01(),
                               anchor.min.y + 40.0 * random.NextDouble01(),
                               anchor.min.z + 40.0 * random.NextDouble01());
            box = Box<double>(
                Vec3d(Quantize(center.x - 2.0), Quantize(center.y - 2.0), Quantize(center.z - 2.0)),
                Vec3d(Quantize(center.x + 2.0), Quantize(center.y + 2.0),
                      Quantize(center.z + 2.0)));
        }
        watch.Start();
        for (uint32 item = 0; item < Burst; ++item) {
            (void)index.Insert(Base + item, burst[item], ProxyFilter(Base + item));
        }
        for (uint32 item = 0; item < Burst; ++item) {
            (void)index.Remove(Base + item, burst[item]);
        }
        index.Maintain(16);
        (void)samples.Add(watch.Elapsed(), Burst * 2);
    }
    json.Summary("perOperation", samples.Summarize());
    hook.After(index, json);
    index.Destroy();
    json.EndObject();
}

// ---- 비교군 선택 ----
enum class GroupKind : uint8 {
    A1,
    A2,
    A2s,
    B,
    C16,
    C256,
    D,
    Invalid
};

struct GroupChoice {
    GroupKind kind = GroupKind::Invalid;
    LooseVariant variant;
};

bool ChooseGroup(const char* name, GroupChoice& choice) noexcept {
    choice.variant.name = name;
    if (platform::CStringEquals(name, "A1")) {
        choice.kind = GroupKind::A1;
    } else if (platform::CStringEquals(name, "A2")) {
        choice.kind = GroupKind::A2;
    } else if (platform::CStringEquals(name, "A2s")) {
        choice.kind = GroupKind::A2s;
    } else if (platform::CStringEquals(name, "B")) {
        choice.kind = GroupKind::B;
    } else if (platform::CStringEquals(name, "C16")) {
        choice.kind = GroupKind::C16;
    } else if (platform::CStringEquals(name, "C256")) {
        choice.kind = GroupKind::C256;
    } else if (platform::CStringEquals(name, "D")) {
        choice.kind = GroupKind::D;
    } else {
        choice.kind = GroupKind::D;
        spatial::LooseOctreeConfig& config = choice.variant.config;
        if (platform::CStringEquals(name, "D-k1")) {
            config.looseness = 1.0;
        } else if (platform::CStringEquals(name, "D-k65_64")) {
            config.looseness = 65.0 / 64.0;
        } else if (platform::CStringEquals(name, "D-k5_4")) {
            config.looseness = 1.25;
        } else if (platform::CStringEquals(name, "D-k3_2")) {
            config.looseness = 1.5;
        } else if (platform::CStringEquals(name, "D-s8")) {
            config.splitThreshold = 8;
            config.mergeThreshold = 4;
        } else if (platform::CStringEquals(name, "D-s32")) {
            config.splitThreshold = 32;
        } else if (platform::CStringEquals(name, "D-s64")) {
            config.splitThreshold = 64;
        } else if (platform::CStringEquals(name, "D-noinplace")) {
            choice.variant.inPlaceUpdate = false;
        } else if (platform::CStringEquals(name, "D-immediate")) {
            choice.variant.immediateMaintenance = true;
        } else if (platform::CStringEquals(name, "D-boxdouble")) {
            choice.variant.boxAsVolume = true;
        } else {
            choice.kind = GroupKind::Invalid;
        }
    }
    return choice.kind != GroupKind::Invalid;
}

void WriteVariant(JsonWriter& json, const GroupChoice& choice) noexcept {
    const spatial::LooseOctreeConfig& config = choice.variant.config;
    json.Key("looseOctreeConfig");
    json.BeginObject();
    json.Field("minCellSize", config.minCellSize);
    json.FieldInteger("initialRootLevel", config.initialRootLevel);
    json.FieldInteger("maxRootLevel", config.maxRootLevel);
    json.Field("looseness", config.looseness);
    json.FieldInteger("splitThreshold", config.splitThreshold);
    json.FieldInteger("mergeThreshold", config.mergeThreshold);
    json.FieldInteger("chunkCapacity", spatial::LooseOctreeChunkCapacity);
    json.FieldBool("inPlaceUpdate", choice.variant.inPlaceUpdate);
    json.FieldBool("immediateMaintenance", choice.variant.immediateMaintenance);
    json.FieldBool("boxAsVolume", choice.variant.boxAsVolume);
    json.EndObject();
}

template <typename Body>
void ForGroup(const GroupChoice& choice, Allocator& allocator, const Dataset* dataset,
              Body&& body) noexcept {
    switch (choice.kind) {
    case GroupKind::A1: {
        FlatScanDouble index(allocator);
        GroupTraits traits;
        body(index, traits);
        break;
    }
    case GroupKind::A2: {
        FlatScanFloat<true> index(allocator);
        GroupTraits traits;
        traits.conservativeBox = true;
        body(index, traits);
        break;
    }
    case GroupKind::A2s: {
        FlatScanFloat<false> index(allocator);
        GroupTraits traits;
        traits.conservativeBox = true;
        body(index, traits);
        break;
    }
    case GroupKind::B: {
        UnityIndex index(allocator, 131072.0, 128.0, 2.0);
        GroupTraits traits;
        traits.boundedRemoveRow = true;
        body(index, traits);
        break;
    }
    case GroupKind::C16:
    case GroupKind::C256: {
        double center[3] = {0.0, 0.0, 0.0};
        double extent = 1048576.0;
        if (dataset != nullptr) {
            SceneRootFor(*dataset, center, extent);
        } else {
            // 루트 시나리오: 최대 약 1.7e7 cm까지 덮는 고정 루트.
            extent = 33554432.0;
        }
        SceneIndex index(allocator, choice.kind == GroupKind::C16 ? 16u : 256u, center, extent);
        GroupTraits traits;
        traits.touchMissAllowed = true;
        body(index, traits);
        break;
    }
    case GroupKind::D: {
        LooseIndex index(allocator, choice.variant);
        GroupTraits traits;
        traits.conservativeBox = !choice.variant.boxAsVolume;
        traits.deferredMaintenance = true;
        body(index, traits);
        break;
    }
    case GroupKind::Invalid:
        break;
    }
}

void WriteEnvironment(JsonWriter& json, const Options& options, int argc, char** argv) noexcept {
    json.Field("tool", "SpatialIndexBench");
    json.FieldInteger("schema", 1);
    json.Field("group", options.group);
    json.Field("mode", options.counters ? "counters" : "timing");
    json.Field("profile", options.profile);
    json.FieldInteger("seed", options.seed);
    json.Key("argv");
    json.BeginArray();
    for (int index = 0; index < argc; ++index) {
        json.String(argv[index]);
    }
    json.EndArray();
    json.Key("build");
    json.BeginObject();
    json.Field("compiler", __VERSION__);
    json.Field("configuration", ROCKS_BUILD_RELEASE       ? "Release"
                                : ROCKS_BUILD_DEVELOPMENT ? "Development"
                                                          : "Debug");
    json.Field("mathBackend", platform::MathBackendName);
    const platform::CpuFeatures& cpu = platform::QueryCpuFeatures();
    json.FieldBool("cpuAvx2", cpu.avx2);
    json.FieldBool("cpuSse42", cpu.sse42);
    json.EndObject();
    json.Field("allocator", "BenchAllocator: platform::AlignedAllocate with request-size counters, "
                            "shared by every group");
    json.FieldInteger("threads", 1);
    json.Field("hardwareCounters", "not measured (decision 10)");
    json.Field("countersInTimingPath",
               "A/B/C: compiled out (Counting=false template). D: QueryStats pointer is nullptr "
               "(runtime branch remains)");
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!Parse(argc, argv, options)) {
        printf(
            "usage: SpatialIndexBench --group <A1|A2|A2s|B|C16|C256|D|D-variant> --out <file.json> "
            "[--profile full|large|variant] [--counters] [--dists a,b] [--sizes n,m] [--seed N] "
            "[--no-scenarios|--scenarios-only]\n");
        return 2;
    }
    GroupChoice choice;
    if (!ChooseGroup(options.group, choice)) {
        printf("unknown group %s\n", options.group);
        return 2;
    }
    BuildCases(options);

    BenchAllocator allocator;
    JsonWriter json(allocator);
    Array<uint32> resultStorage(allocator);
    Array<uint32> referenceStorage(allocator);
    Array<uint32> leftIds(allocator);
    Array<uint32> rightIds(allocator);
    uint32 maxSize = 30000;
    for (uint32 index = 0; index < options.caseCount; ++index) {
        maxSize = options.cases[index].size > maxSize ? options.cases[index].size : maxSize;
    }
    if (!resultStorage.TryResize(maxSize) || !referenceStorage.TryResize(maxSize)) {
        printf("result buffer allocation failed\n");
        return 1;
    }
    Context context{&options,  &allocator,       &json, &resultStorage, &leftIds,
                    &rightIds, &referenceStorage};

    json.BeginObject();
    WriteEnvironment(json, options, argc, argv);
    if (choice.kind == GroupKind::D) {
        WriteVariant(json, choice);
    }
    json.FieldInteger("startMonotonicNs", platform::MonotonicNanoseconds());
    printf("SpatialIndexBench group=%s mode=%s cases=%u\n", options.group,
           options.counters ? "counters" : "timing", options.scenariosOnly ? 0 : options.caseCount);

    json.Key("cases");
    json.BeginArray();
    if (!options.scenariosOnly) {
        for (uint32 caseIndex = 0; caseIndex < options.caseCount; ++caseIndex) {
            const CaseSpec& spec = options.cases[caseIndex];
            Dataset dataset(allocator);
            QuerySet empty(allocator);
            QuerySet few(allocator);
            QuerySet many(allocator);
            const uint64 seed = options.seed + static_cast<uint64>(spec.size) * 131 +
                                static_cast<uint64>(spec.distribution) * 7919;
            const Plan plan = PlanFor(spec.size, spec.distribution);
            if (!GenerateDataset(spec.distribution, spec.size, seed, dataset) ||
                !GenerateQueries(dataset, QueryClass::Empty, plan.queryCount, seed, empty) ||
                !GenerateQueries(dataset, QueryClass::Few, plan.queryCount, seed, few) ||
                !GenerateQueries(dataset, QueryClass::Many, plan.queryCount, seed, many)) {
                printf("input generation failed\n");
                return 1;
            }
            CaseInput input{spec.distribution,    spec.size, seed, plan, &dataset,
                            {&empty, &few, &many}};
            Sections sections;
            // 광원 수 변형은 10^5 이하에서만 잰다(10^6은 프레임당 수백 ms라 시간 예산 밖).
            sections.lightVariants =
                spec.size <= 100000 &&
                !(spec.distribution == Distribution::OverlapDense && spec.size >= 100000);
            if (platform::CStringEquals(options.profile, "variant")) {
                sections.lightVariants = false;
                sections.removeAll = false;
            }
            ForGroup(choice, allocator, &dataset,
                     [&](auto& index, const GroupTraits& traits) noexcept {
                         RunCase(index, input, traits, sections, context);
                     });
        }
    }
    json.EndArray();

    json.Key("scenarios");
    json.BeginArray();
    const bool structured = choice.kind != GroupKind::A2 && choice.kind != GroupKind::A2s;
    if (options.scenarios && !options.counters && structured &&
        platform::CStringEquals(options.profile, "full")) {
        printf("  scenarios\n");
        ForGroup(choice, allocator, nullptr, [&](auto& index, const GroupTraits&) noexcept {
            RunRootScenario(index, context);
        });
        Dataset dense(allocator);
        if (!GenerateDataset(Distribution::SeparableDense, SplitMergeBase, DefaultSeed, dense)) {
            printf("input generation failed\n");
            return 1;
        }
        ForGroup(choice, allocator, &dense, [&](auto& index, const GroupTraits&) noexcept {
            RunSplitMergeScenario(index, dense, context);
        });
    }
    json.EndArray();
    json.FieldInteger("endMonotonicNs", platform::MonotonicNanoseconds());
    json.EndObject();

    if (!json.WriteToFile(options.out)) {
        printf("failed to write %s\n", options.out);
        return 1;
    }
    printf("wrote %s\n", options.out);
    return 0;
}
