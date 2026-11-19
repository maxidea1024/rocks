#include "FlatBoundsInternal.h"

#include <Core/Math/Intersection.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Atomics.h>
#include <Platform/PlatformMath.h>
#include <Task/ParallelFor.h>

namespace rocks::spatial {

namespace {

namespace simd = platform::simd;

// float 단위 반올림 u = 2^-24와 double 단위 반올림 ε = 2^-53이다(6.2절 표).
constexpr double FloatUnit = 0x1.0p-24;
constexpr double DoubleUnit = 0x1.0p-53;
constexpr double FloatMinNormal = static_cast<double>(NumericLimits<float>::Min());

// double을 가장 가까운 float로 옮긴다. float 범위를 넘으면 같은 부호의 무한대다(범위 밖 변환은 C++
// 정의가 없으므로 직접 고른다).
float ToFloatSaturating(double value) noexcept {
    constexpr double maximum = static_cast<double>(NumericLimits<float>::Max());
    if (value > maximum) {
        return platform::PlatformMath::Infinity<float>();
    }
    if (value < -maximum) {
        return -platform::PlatformMath::Infinity<float>();
    }
    return static_cast<float>(value);
}

bool IsFiniteVector(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

// 원소의 정확한 그리기 거리 판정이다(6.1절 술어 4). 목록 원소에 쓰며 정답 술어와 같은 식이다.
bool PassesDrawDistance(const Box<double>& bounds, float minDraw, float maxDraw,
                        const Vec3d& origin, float scale) noexcept {
    if (maxDraw > 0.0f) {
        const double limit = static_cast<double>(maxDraw) * static_cast<double>(scale);
        // 경계·기준점은 검사를 마친 유한 값이라 실패하지 않는다.
        const auto nearest = TryDistanceSquared(bounds, origin);
        if (!nearest || nearest.Value() > limit * limit) {
            return false;
        }
    }
    if (minDraw > 0.0f) {
        const double limit = static_cast<double>(minDraw) * static_cast<double>(scale);
        double farthest = 0.0;
        for (uint32 axis = 0; axis < 3; ++axis) {
            const double toMin = math::Abs(origin[axis] - bounds.min[axis]);
            const double toMax = math::Abs(origin[axis] - bounds.max[axis]);
            const double reach = math::Max(toMin, toMax);
            farthest += reach * reach;
        }
        if (farthest < limit * limit) {
            return false;
        }
    }
    return true;
}

// page 하나에 옮긴 평면을 4-lane 레지스터로 펼친 것이다. useMax*는 n_f 성분 부호로 고른 꼭짓점
// (가장 안쪽, S가 가장 큰 쪽)이 max 배열인지다.
struct LanePlane {
    simd::Register4f nx;
    simd::Register4f ny;
    simd::Register4f nz;
    simd::Register4f w;
    simd::Register4f negativeDelta;
    bool useMaxX;
    bool useMaxY;
    bool useMaxZ;
};

// 거리 판정의 page·뷰별 상수다(6.2절 거리 여유). 원소의 배율 적용 거리 t에 대해 상한 판정은
// 최근접 거리² ≤ t · (t · upperScale + linear) + constant, 하한 판정은 가장 먼 거리² ≥
// t · (t · lowerScale − linear) − constant다.
struct LaneDistance {
    simd::Register4f originX;
    simd::Register4f originY;
    simd::Register4f originZ;
    simd::Register4f scale;
    simd::Register4f upperScale;
    simd::Register4f lowerScale;
    simd::Register4f linear;
    simd::Register4f constant;
};

// 뷰마다 질의 전체에서 한 번 계산하는 값이다.
struct ViewSetup {
    const FlatBoundsView* view;
    uint64* words;
};

struct LocalStats {
    uint64 pageViews = 0;
    uint64 rejectedPageViews = 0;
    uint64 testedElements = 0;
    uint64 alwaysVisibleElements = 0;
    uint64 listElements = 0;
    uint64 visibleElements = 0;
};

uint32 LaneBits(const uint32* filters, uint32 mask, uint32 lanes) noexcept {
    uint32 bits = 0;
    for (uint32 lane = 0; lane < lanes; ++lane) {
        bits |= (filters[lane] & mask) != 0 ? (1u << lane) : 0u;
    }
    return bits;
}

} // namespace

// δ 식(Docs/Design/FlatBounds.ko.md 6.2절)이다. 기호: u = 2^-24, ε = 2^-53, R = page에 저장된 float
// 상대 좌표의 최대 절댓값, o = 타일 원점, n·p − d = Core 평면 규약(안쪽 양수), w′ = n·o − d.
//
// float 판정 S_f = n_f·x + w′_f(x는 n_f 성분 부호로 고른 저장 꼭짓점)가 정답 술어의 값과 다른
// 원인과 상한:
// - normal의 float 변환 n → n_f: u·‖n‖₁·R. 고른 꼭짓점은 n_f에 대한 최댓값이므로 저장 상자에서
//   n에 대한 최댓값은 n_f·x + |n − n_f|·R 이하다(n_f 성분이 ±0이 되어도 성립).
// - w′의 float 변환: u·|w′|.
// - float 내적 4항(곱 3, 합 3)의 반올림: γ₄·(‖n_f‖₁·R + |w′_f|) ≈ 4u·(‖n‖₁·R + |w′|).
// - w′의 double 계산 n·o − d의 소거: γ₄(double)·(‖n‖₁·‖o‖∞ + |d|). 타일이 원점에서 멀면 w′가 작아도
//   이 항은 ‖o‖∞에 비례해 크다.
// - 정답 술어(Core Overlap, 월드 double 좌표)의 반올림: γ₄(double)·(‖n‖₁·(‖o‖∞ + R) + |d|).
// - 저장 경계: 한 ULP 바깥 보정으로 덮는다(오차 없음, 5절).
// 앞의 세 float 항의 합(이론값 5u + 2차항)에 여유를 두어 8u·(‖n‖₁·R + |w′|), 두 double 항의 합(8ε +
// 2차항)에 여유를 두어 12ε·(‖n‖₁·(‖o‖∞ + R) + |d|)로 둔다. 둘째 항이 빠지면 원점에서 10⁹ cm 떨어진
// 타일의 꼭짓점 접촉이 누락된다(리뷰 probe: 200만 건 중 222,907건). 셋째 항 4·FLT_MIN·(‖n‖₁ + 1)은
// FTZ·DAZ 하한이다: 비정규 값이 0이 되는 오차는 FLT_MIN 규모(곱 3개, 합 3개, 저장 경계·n_f
// 변환)이고 δ 자체가 비정규면 0이 되므로 이 항이 δ를 정규 값으로 묶는다. FMA 사용 여부와 무관하게
// 성립한다(FMA는 반올림을 줄이기만 한다). δ는 double로 계산한 뒤 float로 옮기며 위쪽으로 넓힌다(δ
// 이상).
//
// page 전체 분류: 저장 꼭짓점의 n·x는 [−‖n‖₁·R, ‖n‖₁·R]에 있으므로 w′ + ‖n‖₁·R + δ < 0이면 page의
// 모든 원소가 정답 술어로도 바깥이고(δ가 w′의 double 오차와 정답 술어 오차를 덮는다), w′ − ‖n‖₁·R −
// 2δ ≥ 0이면 float 판정 S_f ≥ w′ − ‖n‖₁·R − δ ≥ δ라 어떤 원소도 이 평면에서 버리지 않는다(판정
// 생략과 결과가 같다). w′가 double로 무한대(평면 거리가 double 범위 끝)면 부호로 정한다.
detail::FlatPagePlane detail::PreparePagePlane(const Plane<double>& plane, const Vec3d& origin,
                                               float radius) noexcept {
    detail::FlatPagePlane out;
    const Vec3d& normal = plane.normal;
    const double distance = plane.distance;
    const double norm1 = math::Abs(normal.x) + math::Abs(normal.y) + math::Abs(normal.z);
    const double reach = static_cast<double>(radius);
    const double originInf =
        math::Max(math::Abs(origin.x), math::Max(math::Abs(origin.y), math::Abs(origin.z)));
    const double wPrime = Dot(normal, origin) - distance;
    out.nx = static_cast<float>(normal.x);
    out.ny = static_cast<float>(normal.y);
    out.nz = static_cast<float>(normal.z);
    out.w = ToFloatSaturating(wPrime);
    if (!math::IsFinite(wPrime)) {
        out.delta = 0.0f;
        out.relation =
            wPrime < 0.0 ? FlatPagePlane::Relation::AllOutside : FlatPagePlane::Relation::AllInside;
        return out;
    }
    const double delta = 8.0 * FloatUnit * (norm1 * reach + math::Abs(wPrime)) +
                         12.0 * DoubleUnit * (norm1 * (originInf + reach) + math::Abs(distance)) +
                         4.0 * FloatMinNormal * (norm1 + 1.0);
    // float로 옮길 때 위쪽으로 넓힌다: 최근접 변환 f ≥ δ(1 − u)에 (1 + 8u)를 곱하면(float 곱 반올림
    // 포함) δ(1 − u)²(1 + 8u) > δ다. δ ≥ 8·FLT_MIN이라 정규 값이고 FTZ·DAZ와 무관하다. 비트 연산의
    // 방향 반올림(ToFloatRoundedUp)보다 싸서 page·뷰·평면마다 부를 수 있다.
    out.delta = ToFloatSaturating(delta) * (1.0f + 0x1.0p-21f);
    const double spread = norm1 * reach;
    if (wPrime + spread + delta < 0.0) {
        out.relation = FlatPagePlane::Relation::AllOutside;
    } else if (wPrime - spread - 2.0 * delta >= 0.0) {
        out.relation = FlatPagePlane::Relation::AllInside;
    }
    return out;
}

Result<void> FlatBoundsSet::QueryViews(Span<const FlatBoundsView> views, Span<FlatVisibility> out,
                                       WorkerScheduler* scheduler, Allocator& scratch,
                                       uint32 maxHelpers, FlatQueryStats* stats) const noexcept {
    const auto invalid = []() noexcept {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    };
    if (views.Size() != out.Size()) {
        return invalid();
    }
    const uint32 wordCount = WordCount();
    for (usize index = 0; index < views.Size(); ++index) {
        const FlatBoundsView& view = views[index];
        if (!view.volume.IsValid() || !IsFiniteVector(view.distanceOrigin) ||
            !math::IsFinite(view.distanceScale) || !(view.distanceScale > 0.0f)) {
            return invalid();
        }
        if (out[index].words.Size() < wordCount) {
            return invalid();
        }
    }
    // 출력 버퍼끼리 겹치면 결과가 서로를 덮으므로 거부한다(쓰는 범위는 앞 wordCount개 word다).
    for (usize first = 0; first < out.Size(); ++first) {
        for (usize second = first + 1; second < out.Size() && wordCount != 0; ++second) {
            const uint64* a = out[first].words.Data();
            const uint64* b = out[second].words.Data();
            if (a < b + wordCount && b < a + wordCount) {
                return invalid();
            }
        }
    }
    if (views.Size() == 0) {
        return Result<void>::Success();
    }

    constexpr uint32 MaxViews = 16;
    // 뷰 수가 많으면 MaxViews개씩 나눠 같은 순회를 반복한다(page 상수 배열을 스택에 둔다).
    for (usize start = 0; start < views.Size(); start += MaxViews) {
        const uint32 count =
            static_cast<uint32>(views.Size() - start < MaxViews ? views.Size() - start : MaxViews);
        ViewSetup setups[MaxViews];
        for (uint32 index = 0; index < count; ++index) {
            setups[index].view = &views[start + index];
            setups[index].words = out[start + index].words.Data();
        }

        const uint32 pageSlots = pages_.SlotCount();
        const uint32 capacity = config_.pageCapacity;
        const uint32 wordsPerPage = wordsPerPage_;
        const uint32 listWordBase = pageSlots * wordsPerPage;

        // page 하나를 모든 뷰에 대해 판정한다. 해제된 page는 그 word를 0으로 쓴다.
        const auto runPage = [&](uint32 page, LocalStats& local) noexcept {
            const uint32 wordBase = page * wordsPerPage;
            if (!pages_.IsLive(page)) {
                for (uint32 index = 0; index < count; ++index) {
                    for (uint32 word = 0; word < wordsPerPage; ++word) {
                        setups[index].words[wordBase + word] = 0;
                    }
                }
                return;
            }
            const detail::FlatPageHeader& header = pages_[page];
            const uint32 slotBase = page * capacity;
            LanePlane planes[ConvexVolumeMaxPlanes];
            for (uint32 index = 0; index < count; ++index) {
                const FlatBoundsView& view = *setups[index].view;
                uint64* words = setups[index].words + wordBase;
                if (view.filterMask == 0) {
                    for (uint32 word = 0; word < wordsPerPage; ++word) {
                        words[word] = 0;
                    }
                    continue;
                }
                ++local.pageViews;
                bool rejected = false;
                uint32 planeCount = 0;
                for (uint32 plane = 0; plane < view.volume.planeCount; ++plane) {
                    const detail::FlatPagePlane prepared = detail::PreparePagePlane(
                        view.volume.planes[plane], header.origin, header.radius);
                    if (prepared.relation == detail::FlatPagePlane::Relation::AllOutside) {
                        rejected = true;
                        break;
                    }
                    if (prepared.relation == detail::FlatPagePlane::Relation::AllInside) {
                        continue;
                    }
                    LanePlane& lane = planes[planeCount++];
                    lane.nx = simd::Splat(prepared.nx);
                    lane.ny = simd::Splat(prepared.ny);
                    lane.nz = simd::Splat(prepared.nz);
                    lane.w = simd::Splat(prepared.w);
                    lane.negativeDelta = simd::Splat(-prepared.delta);
                    lane.useMaxX = prepared.nx >= 0.0f;
                    lane.useMaxY = prepared.ny >= 0.0f;
                    lane.useMaxZ = prepared.nz >= 0.0f;
                }
                local.rejectedPageViews += rejected ? 1u : 0u;

                // 거리 여유(6.2절): 계산한 거리²와 정확한 거리² d²의 차이는 16u·(d² + R·d) + 8u²·R²
                // + (FTZ 하한) 이하다(기준점 q = p − o의 float 변환 u·|q| ≤ u·(d + R), 축
                // 차이·제곱·합의 반올림). 상한 판정은 원소의 배율 적용 거리 t = maxDraw·s(float 곱,
                // 상대 오차 u)에 대해 t·(t·(1 + 2^-19) + 17u·R) + c, 하한 판정은 t·(t·(1 − 2^-19) −
                // 17u·R) − c와 비교한다. 2^-19 = 32u는 위 상대 오차 16u와 t·k 계산의 반올림 몇 개를
                // 덮고, 17u·R은 16u·R·M을 (1 ± 4u 반올림까지) 덮으며, c = 8u²·R² + 64·FLT_MIN은 d ≈
                // 0 근처의 제곱 오차와 FTZ 하한이다. 상대 여유만 두면 뷰 근처의 하한 판정에서
                // 누락이 생긴다(6.2절 예).
                const bool distance = header.distanceLimited != 0;
                LaneDistance range;
                if (distance) {
                    const Vec3d offset = view.distanceOrigin - header.origin;
                    const double reach = static_cast<double>(header.radius);
                    range.originX = simd::Splat(ToFloatSaturating(offset.x));
                    range.originY = simd::Splat(ToFloatSaturating(offset.y));
                    range.originZ = simd::Splat(ToFloatSaturating(offset.z));
                    range.scale = simd::Splat(view.distanceScale);
                    range.upperScale = simd::Splat(1.0f + 0x1.0p-19f);
                    range.lowerScale = simd::Splat(1.0f - 0x1.0p-19f);
                    range.linear = simd::Splat(math::ToFloatRoundedUp(17.0 * FloatUnit * reach));
                    range.constant = simd::Splat(math::ToFloatRoundedUp(
                        8.0 * FloatUnit * FloatUnit * reach * reach + 64.0 * FloatMinNormal));
                }

                const uint32 used = header.count;
                for (uint32 word = 0; word < wordsPerPage; ++word) {
                    const uint32 base = word * FlatVisibilityBitsPerWord;
                    if (base >= used || rejected) {
                        // 사용 수 뒤의 꼬리는 0이다. page 전체가 바깥이면 항상 보임 원소만 남는다.
                        uint64 bits = 0;
                        if (rejected && base < used) {
                            const uint32 lanes = used - base < FlatVisibilityBitsPerWord
                                                     ? used - base
                                                     : FlatVisibilityBitsPerWord;
                            const uint64 laneMask = lanes == FlatVisibilityBitsPerWord
                                                        ? NumericLimits<uint64>::Max()
                                                        : (uint64{1} << lanes) - 1;
                            bits = always_[wordBase + word] & laneMask;
                            uint64 filtered = 0;
                            for (uint64 pending = bits; pending != 0; pending &= pending - 1) {
                                const uint32 bit =
                                    platform::PlatformMath::CountTrailingZeros64(pending);
                                if ((filter_[slotBase + base + bit] & view.filterMask) != 0) {
                                    filtered |= uint64{1} << bit;
                                }
                            }
                            bits = filtered;
                            local.alwaysVisibleElements += platform::PlatformMath::PopCount64(bits);
                            local.visibleElements += platform::PlatformMath::PopCount64(bits);
                        }
                        words[word] = bits;
                        continue;
                    }
                    const uint32 lanesInWord = used - base < FlatVisibilityBitsPerWord
                                                   ? used - base
                                                   : FlatVisibilityBitsPerWord;
                    const uint64 alwaysWord = always_[wordBase + word];
                    uint64 result = 0;
                    for (uint32 group = 0; group < lanesInWord; group += 4) {
                        const uint32 slot = slotBase + base + group;
                        const uint32 lanes = lanesInWord - group < 4 ? lanesInWord - group : 4;
                        const uint32 filterBits =
                            LaneBits(filter_.Data() + slot, view.filterMask, lanes);
                        if (filterBits == 0) {
                            continue;
                        }
                        const uint32 alwaysBits =
                            static_cast<uint32>((alwaysWord >> group) & 0xFu) & filterBits;
                        uint32 tested = filterBits & ~alwaysBits;
                        local.alwaysVisibleElements +=
                            platform::PlatformMath::PopCount32(alwaysBits);
                        if (tested != 0 && (planeCount != 0 || distance)) {
                            local.testedElements += platform::PlatformMath::PopCount32(tested);
                            const simd::Register4f minX = simd::Load(minX_.Data() + slot);
                            const simd::Register4f minY = simd::Load(minY_.Data() + slot);
                            const simd::Register4f minZ = simd::Load(minZ_.Data() + slot);
                            const simd::Register4f maxX = simd::Load(maxX_.Data() + slot);
                            const simd::Register4f maxY = simd::Load(maxY_.Data() + slot);
                            const simd::Register4f maxZ = simd::Load(maxZ_.Data() + slot);
                            // 평면 4개마다 한 번만 조기 탈출을 본다(UE IntersectBox8Plane과 같은
                            // 묶음 단위). 평면마다 분기하면 탈출 위치가 묶음마다 달라 분기 예측이
                            // 자주 빗나간다. 결과는 평면마다 탈출할 때와 같다.
                            for (uint32 first = 0; first < planeCount && tested != 0; first += 4) {
                                const uint32 last = first + 4 < planeCount ? first + 4 : planeCount;
                                uint32 outside = 0;
                                for (uint32 plane = first; plane < last; ++plane) {
                                    const LanePlane& lane = planes[plane];
                                    const simd::Register4f x = lane.useMaxX ? maxX : minX;
                                    const simd::Register4f y = lane.useMaxY ? maxY : minY;
                                    const simd::Register4f z = lane.useMaxZ ? maxZ : minZ;
                                    // ((n_x·x + n_y·y) + n_z·z) + w′ — 반올림 6번(γ₄ 상한 안).
                                    const simd::Register4f s =
                                        simd::Add(simd::Add(simd::Add(simd::Multiply(lane.nx, x),
                                                                      simd::Multiply(lane.ny, y)),
                                                            simd::Multiply(lane.nz, z)),
                                                  lane.w);
                                    outside |=
                                        simd::MoveMask(simd::CompareLess(s, lane.negativeDelta));
                                }
                                tested &= ~outside;
                            }
                            if (tested != 0 && distance) {
                                const simd::Register4f zero = simd::Zero();
                                const simd::Register4f nearX =
                                    simd::Max(simd::Max(simd::Subtract(minX, range.originX),
                                                        simd::Subtract(range.originX, maxX)),
                                              zero);
                                const simd::Register4f nearY =
                                    simd::Max(simd::Max(simd::Subtract(minY, range.originY),
                                                        simd::Subtract(range.originY, maxY)),
                                              zero);
                                const simd::Register4f nearZ =
                                    simd::Max(simd::Max(simd::Subtract(minZ, range.originZ),
                                                        simd::Subtract(range.originZ, maxZ)),
                                              zero);
                                const simd::Register4f nearSquared =
                                    simd::Add(simd::Add(simd::Multiply(nearX, nearX),
                                                        simd::Multiply(nearY, nearY)),
                                              simd::Multiply(nearZ, nearZ));
                                const simd::Register4f farX =
                                    simd::Max(simd::Abs(simd::Subtract(range.originX, minX)),
                                              simd::Abs(simd::Subtract(range.originX, maxX)));
                                const simd::Register4f farY =
                                    simd::Max(simd::Abs(simd::Subtract(range.originY, minY)),
                                              simd::Abs(simd::Subtract(range.originY, maxY)));
                                const simd::Register4f farZ =
                                    simd::Max(simd::Abs(simd::Subtract(range.originZ, minZ)),
                                              simd::Abs(simd::Subtract(range.originZ, maxZ)));
                                const simd::Register4f farSquared =
                                    simd::Add(simd::Add(simd::Multiply(farX, farX),
                                                        simd::Multiply(farY, farY)),
                                              simd::Multiply(farZ, farZ));
                                // 상한이 없으면 저장 값이 +Inf라 한계도 +Inf이고, 하한이 없으면
                                // 0이라 한계가 −c다(분기 없음).
                                const simd::Register4f upper =
                                    simd::Multiply(simd::Load(drawMax_.Data() + slot), range.scale);
                                const simd::Register4f upperLimit = simd::Add(
                                    simd::Multiply(
                                        upper, simd::Add(simd::Multiply(upper, range.upperScale),
                                                         range.linear)),
                                    range.constant);
                                const simd::Register4f lower =
                                    simd::Multiply(simd::Load(drawMin_.Data() + slot), range.scale);
                                const simd::Register4f lowerLimit = simd::Subtract(
                                    simd::Multiply(
                                        lower,
                                        simd::Subtract(simd::Multiply(lower, range.lowerScale),
                                                       range.linear)),
                                    range.constant);
                                tested &= simd::MoveMask(simd::BitAnd(
                                    simd::CompareLessEqual(nearSquared, upperLimit),
                                    simd::CompareGreaterEqual(farSquared, lowerLimit)));
                            }
                        }
                        const uint32 visible = (tested | alwaysBits) & filterBits;
                        local.visibleElements += platform::PlatformMath::PopCount32(visible);
                        result |= static_cast<uint64>(visible) << group;
                    }
                    words[word] = result;
                }
            }
        };

        // 목록 원소를 정확한 double 정답 술어로 판정한다.
        const auto runList = [&](LocalStats& local) noexcept {
            const uint32 listCount = static_cast<uint32>(list_.Size());
            const uint32 listWords =
                (listCount + FlatVisibilityBitsPerWord - 1) / FlatVisibilityBitsPerWord;
            for (uint32 index = 0; index < count; ++index) {
                const FlatBoundsView& view = *setups[index].view;
                uint64* words = setups[index].words + listWordBase;
                for (uint32 word = 0; word < listWords; ++word) {
                    words[word] = 0;
                }
                if (view.filterMask == 0) {
                    continue;
                }
                for (uint32 entryIndex = 0; entryIndex < listCount; ++entryIndex) {
                    const detail::FlatListEntry& entry = list_[entryIndex];
                    if ((entry.filter & view.filterMask) == 0) {
                        continue;
                    }
                    ++local.listElements;
                    const bool visible =
                        entry.alwaysVisible ||
                        (Overlap(view.volume, entry.bounds) == QueryResult::Hit &&
                         PassesDrawDistance(entry.bounds, entry.minDraw, entry.maxDraw,
                                            view.distanceOrigin, view.distanceScale));
                    if (visible) {
                        words[entryIndex / FlatVisibilityBitsPerWord] |=
                            uint64{1} << (entryIndex % FlatVisibilityBitsPerWord);
                        ++local.visibleElements;
                    }
                }
            }
        };

        const auto publish = [stats](const LocalStats& local) noexcept {
            if (stats == nullptr) {
                return;
            }
            using platform::MemoryOrder;
            platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&stats->pageViews, local.pageViews);
            platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&stats->rejectedPageViews,
                                                           local.rejectedPageViews);
            platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&stats->testedElements,
                                                           local.testedElements);
            platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&stats->alwaysVisibleElements,
                                                           local.alwaysVisibleElements);
            platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&stats->listElements,
                                                           local.listElements);
            platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&stats->visibleElements,
                                                           local.visibleElements);
        };

        // 작업 색인 [0, pageSlots)는 page 하나씩(해제된 page 포함 — 그 word를 0으로 쓰기 위해),
        // pageSlots는 목록 영역 전체다(6.3절). page마다 출력 word가 겹치지 않아 쓰기 경합이 없다.
        const auto body = [&](usize begin, usize end) noexcept {
            LocalStats local;
            for (usize job = begin; job < end; ++job) {
                if (job < pageSlots) {
                    runPage(static_cast<uint32>(job), local);
                } else {
                    runList(local);
                }
            }
            publish(local);
        };
        const usize jobs = static_cast<usize>(pageSlots) + 1;
        if (scheduler == nullptr || maxHelpers == 0) {
            body(0, jobs);
        } else {
            ParallelForOptions options;
            // batch 하나가 원소 약 4,096개(page 용량 64면 page 64개)를 맡게 해 분배 비용을 나눈다.
            options.minBatchSize = capacity >= 4096 ? 1 : 4096 / capacity;
            options.maxHelpers = maxHelpers;
            // body가 오류를 돌려주지 않으므로 실패하지 않는다(helper 할당 실패는 호출 스레드가
            // 처리).
            (void)ParallelFor(*scheduler, scratch, jobs, body, options);
        }
    }

    for (usize index = 0; index < out.Size(); ++index) {
        out[index].epoch = epoch_;
    }
    return Result<void>::Success();
}

} // namespace rocks::spatial
