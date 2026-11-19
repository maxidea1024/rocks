#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Intersection.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/RandomStream.h>
#include <Core/Memory/Allocator.h>
#include <Platform/Fatal.h>
#include <Platform/PlatformMath.h>
#include <SpatialIndex/FlatBoundsSet.h>
#include <TestSupport/Test.h>

// FlatBoundsSet 시험(Docs/Design/FlatBounds.ko.md 9절 FB01–FB09)이 함께 쓰는 정답 모형과 입력
// 생성이다. 정답 기준은 6.1절 술어를 정확한 double AABB와 같은 Core 술어(Overlap,
// TryDistanceSquared)로 전수 계산한 결과다. 집합의 결과를 정답으로 보지 않는다.
namespace rocks::spatial::flattest {

inline constexpr double FloatUnit = 0x1.0p-24;

// 평면 집합으로 볼록 영역을 만든다. 시험 입력은 늘 유효하다(실패는 시험 코드 오류다).
inline ConvexVolume<double> VolumeOf(Span<const Plane<double>> planes) noexcept {
    auto made = ConvexVolume<double>::TryMake(planes);
    if (!made) {
        platform::Fatal("FlatBounds test volume was rejected");
    }
    return made.Value();
}

// 정답 술어(6.1절)다.
inline bool ReferenceVisible(const FlatBoundsDesc& desc, const FlatBoundsView& view) noexcept {
    if ((desc.filterMask & view.filterMask) == 0) {
        return false;
    }
    if (desc.alwaysVisible) {
        return true;
    }
    if (Overlap(view.volume, desc.bounds) != QueryResult::Hit) {
        return false;
    }
    const Vec3d& origin = view.distanceOrigin;
    const double scale = static_cast<double>(view.distanceScale);
    if (desc.maxDrawDistance > 0.0f) {
        const double limit = static_cast<double>(desc.maxDrawDistance) * scale;
        const auto nearest = TryDistanceSquared(desc.bounds, origin);
        if (!nearest || nearest.Value() > limit * limit) {
            return false;
        }
    }
    if (desc.minDrawDistance > 0.0f) {
        const double limit = static_cast<double>(desc.minDrawDistance) * scale;
        double farthest = 0.0;
        for (uint32 axis = 0; axis < 3; ++axis) {
            const double reach = math::Max(math::Abs(origin[axis] - desc.bounds.min[axis]),
                                           math::Abs(origin[axis] - desc.bounds.max[axis]));
            farthest += reach * reach;
        }
        if (farthest < limit * limit) {
            return false;
        }
    }
    return true;
}

// 집합이 보인다고 했지만 정답 술어로는 보이지 않는 원소가 6.2절 초과 띠 안인지다. page 원소만
// 초과가 있을 수 있다(목록은 정답 술어와 같은 double 판정). 평면: 정답이 바깥이라고 본 평면마다
// 실제 상자의 가장 안쪽 꼭짓점의 double 부호 거리가 −3δ 이상이어야 한다(float 판정이 S_f ≥ −δ였고
// float 오차와 저장 경계 보정의 합이 2δ 이하). 거리: 계산 거리²와 정확한 거리²의 차이 16u·(d² +
// R·d) + 8u²·R²에 여유를 둔 띠 안이어야 한다.
inline bool WithinExcessBand(const FlatBoundsDesc& desc, const FlatBoundsView& view,
                             const FlatBoundsPlacement& placement) noexcept {
    if (placement.inList || (desc.filterMask & view.filterMask) == 0) {
        return false;
    }
    for (uint32 index = 0; index < view.volume.planeCount; ++index) {
        const Plane<double>& plane = view.volume.planes[index];
        const Vec3d highest(plane.normal.x >= 0.0 ? desc.bounds.max.x : desc.bounds.min.x,
                            plane.normal.y >= 0.0 ? desc.bounds.max.y : desc.bounds.min.y,
                            plane.normal.z >= 0.0 ? desc.bounds.max.z : desc.bounds.min.z);
        const double signedDistance = plane.SignedDistance(highest);
        if (signedDistance >= 0.0) {
            continue;
        }
        const detail::FlatPagePlane prepared =
            detail::PreparePagePlane(plane, placement.tileOrigin, placement.pageRadius);
        if (signedDistance < -3.0 * static_cast<double>(prepared.delta)) {
            return false;
        }
    }
    const double reach = static_cast<double>(placement.pageRadius);
    const double scale = static_cast<double>(view.distanceScale);
    const double slack = 32.0 * FloatUnit * FloatUnit * reach * reach + 1.0e-30;
    if (desc.maxDrawDistance > 0.0f) {
        const double limit = static_cast<double>(desc.maxDrawDistance) * scale;
        const auto measured = TryDistanceSquared(desc.bounds, view.distanceOrigin);
        const double nearest = measured ? measured.Value() : 0.0;
        if (nearest >
            limit * limit * (1.0 + 96.0 * FloatUnit) + 40.0 * FloatUnit * reach * limit + slack) {
            return false;
        }
    }
    if (desc.minDrawDistance > 0.0f) {
        const double limit = static_cast<double>(desc.minDrawDistance) * scale;
        double farthest = 0.0;
        for (uint32 axis = 0; axis < 3; ++axis) {
            const double reachAxis =
                math::Max(math::Abs(view.distanceOrigin[axis] - desc.bounds.min[axis]),
                          math::Abs(view.distanceOrigin[axis] - desc.bounds.max[axis]));
            farthest += reachAxis * reachAxis;
        }
        if (farthest <
            limit * limit * (1.0 - 96.0 * FloatUnit) - 40.0 * FloatUnit * reach * limit - slack) {
            return false;
        }
    }
    return true;
}

// 정답 모형의 원소 하나다.
struct ModelElement {
    FlatBoundsHandle handle;
    FlatBoundsDesc desc;
    uint32 slot = 0;
    bool live = false;
};

// 대조 결과다.
struct CompareCounts {
    uint64 referenceVisible = 0;
    uint64 reported = 0;
    uint64 misses = 0;
    uint64 excess = 0;
    // 초과 띠 밖의 초과, 살아 있지 않은 슬롯의 bit, 슬롯·handle 불일치다.
    uint64 violations = 0;
};

// 모형의 살아 있는 원소마다 슬롯의 bit를 정답과 대조하고, 나머지 bit(해제 page·꼬리·목록 끝)가
// 0인지 확인한다.
inline void CompareVisibility(const FlatBoundsReader& reader, const Array<ModelElement>& model,
                              const FlatBoundsView& view, const FlatVisibility& bits,
                              CompareCounts& counts) noexcept {
    const uint32 wordCount = reader.WordCount();
    // 살아 있는 원소의 bit 외에는 0이어야 한다. 켜진 bit 전체 수와 살아 있는 원소 중 켜진 수를
    // 비교한다.
    uint64 expectedOnes = 0;
    uint64 setBits = 0;
    for (uint32 word = 0; word < wordCount; ++word) {
        setBits += platform::PlatformMath::PopCount64(bits.words[word]);
    }
    // 목록 슬롯은 page 영역 뒤에 이어진다. page 영역 bit 수는 WordCount에서 목록 word를 뺀 값이다.
    uint32 listCount = 0;
    for (usize index = 0; index < model.Size(); ++index) {
        if (model[index].live && IsFlatBoundsListSlot(model[index].slot)) {
            ++listCount;
        }
    }
    const uint32 listWords =
        (listCount + FlatVisibilityBitsPerWord - 1) / FlatVisibilityBitsPerWord;
    const uint32 pageWordCount = wordCount - listWords;
    for (usize index = 0; index < model.Size(); ++index) {
        const ModelElement& element = model[index];
        if (!element.live) {
            continue;
        }
        auto slot = reader.TrySlotOf(element.handle);
        if (!slot || slot.Value() != element.slot) {
            ++counts.violations;
            continue;
        }
        const uint32 bit = IsFlatBoundsListSlot(element.slot)
                               ? pageWordCount * FlatVisibilityBitsPerWord +
                                     (element.slot - FlatBoundsListSlotBase)
                               : element.slot;
        const bool reported =
            ((bits.words[bit / FlatVisibilityBitsPerWord] >> (bit % FlatVisibilityBitsPerWord)) &
             1u) != 0;
        const bool expected = ReferenceVisible(element.desc, view);
        counts.referenceVisible += expected ? 1u : 0u;
        counts.reported += reported ? 1u : 0u;
        expectedOnes += reported ? 1u : 0u;
        if (expected && !reported) {
            ++counts.misses;
        } else if (!expected && reported) {
            ++counts.excess;
            auto placement = reader.TryPlacementOf(element.handle);
            if (!placement || !WithinExcessBand(element.desc, view, placement.Value())) {
                ++counts.violations;
            }
        }
    }
    // 살아 있는 원소가 아닌 bit가 켜져 있으면 위반이다.
    if (setBits != expectedOnes) {
        counts.violations += setBits > expectedOnes ? setBits - expectedOnes : 1u;
    }
}

// 원점이 apex, 방향 forward(단위), 반각 45도의 near·far 프러스텀(평면 6개, 안쪽 normal)이다.
// extraPlanes만큼 무작위 평면(apex 근처를 지나는)을 더한다.
inline ConvexVolume<double> MakeFrustum(const Vec3d& apex, const Vec3d& forward,
                                        double nearDistance, double farDistance,
                                        RandomStream* random = nullptr,
                                        uint32 extraPlanes = 0) noexcept {
    const Vec3d helper = math::Abs(forward.z) < 0.9 ? Vec3d(0.0, 0.0, 1.0) : Vec3d(1.0, 0.0, 0.0);
    const Vec3d right = Cross(helper, forward).NormalizedOr(Vec3d(0.0, 1.0, 0.0));
    const Vec3d up = Cross(forward, right).NormalizedOr(Vec3d(0.0, 0.0, 1.0));
    Plane<double> planes[ConvexVolumeMaxPlanes];
    uint32 count = 0;
    const auto add = [&](const Vec3d& direction, const Vec3d& point) noexcept {
        const Vec3d normal = direction.NormalizedOr(forward);
        planes[count++] = Plane<double>(normal, Dot(normal, point));
    };
    add(forward, apex + forward * nearDistance);
    add(-forward, apex + forward * farDistance);
    add(forward + right, apex);
    add(forward - right, apex);
    add(forward + up, apex);
    add(forward - up, apex);
    for (uint32 index = 0; index < extraPlanes && random != nullptr; ++index) {
        Vec3d direction(random->NextDouble01() * 2.0 - 1.0, random->NextDouble01() * 2.0 - 1.0,
                        random->NextDouble01() * 2.0 - 1.0);
        if (!direction.TryNormalize()) {
            direction = forward;
        }
        add(direction, apex + forward * (farDistance * random->NextDouble01()));
    }
    return VolumeOf(Span<const Plane<double>>(planes, count));
}

inline double Uniform(RandomStream& random, double low, double high) noexcept {
    return low + (high - low) * random.NextDouble01();
}

inline Vec3d RandomDirection(RandomStream& random) noexcept {
    for (;;) {
        const Vec3d direction(Uniform(random, -1.0, 1.0), Uniform(random, -1.0, 1.0),
                              Uniform(random, -1.0, 1.0));
        auto normalized = direction.TryNormalize(1.0e-3);
        if (normalized) {
            return normalized.Value();
        }
    }
}

} // namespace rocks::spatial::flattest
