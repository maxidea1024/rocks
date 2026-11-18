#include "BenchInput.h"

#include <Core/Math/MathUtility.h>
#include <Core/Math/RandomStream.h>
#include <Platform/PlatformMath.h>
#include <Platform/Memory.h>

namespace rocks::spatialbench {

namespace {

using platform::PlatformMath;

constexpr double GridScale = 64.0;

struct Sampler {
    RandomStream random;

    double Uniform(double low, double high) noexcept {
        return low + (high - low) * random.NextDouble01();
    }

    uint32 Below(uint32 bound) noexcept {
        return random.NextUInt32Below(bound);
    }

    Vec3d InBox(const Box<double>& box) noexcept {
        return Vec3d(Uniform(box.min.x, box.max.x), Uniform(box.min.y, box.max.y),
                     Uniform(box.min.z, box.max.z));
    }

    Vec3d UnitDirection() noexcept {
        while (true) {
            const Vec3d candidate(Uniform(-1.0, 1.0), Uniform(-1.0, 1.0), Uniform(-1.0, 1.0));
            const double lengthSquared = candidate.LengthSquared();
            if (lengthSquared > 1.0e-4 && lengthSquared <= 1.0) {
                return candidate * (1.0 / math::Sqrt(lengthSquared));
            }
        }
    }
};

Box<double> MakeBox(const Vec3d& center, const Vec3d& half) noexcept {
    return Box<double>(Vec3d(Quantize(center.x - half.x), Quantize(center.y - half.y),
                             Quantize(center.z - half.z)),
                       Vec3d(Quantize(center.x + half.x), Quantize(center.y + half.y),
                             Quantize(center.z + half.z)));
}

Box<double> CubeAround(const Vec3d& center, double half) noexcept {
    return Box<double>(center - Vec3d(half, half, half), center + Vec3d(half, half, half));
}

double CubeRootScale(uint32 count) noexcept {
    return PlatformMath::Pow(static_cast<double>(count) / 1000.0, 1.0 / 3.0);
}

Vec3d BoxCenter(const Box<double>& box) noexcept {
    return Vec3d(box.min.x * 0.5 + box.max.x * 0.5, box.min.y * 0.5 + box.max.y * 0.5,
                 box.min.z * 0.5 + box.max.z * 0.5);
}

Vec3d BoxHalf(const Box<double>& box) noexcept {
    return Vec3d((box.max.x - box.min.x) * 0.5, (box.max.y - box.min.y) * 0.5,
                 (box.max.z - box.min.z) * 0.5);
}

Vec3d ClampToBox(const Vec3d& point, const Box<double>& box) noexcept {
    return Vec3d(math::Clamp(point.x, box.min.x, box.max.x),
                 math::Clamp(point.y, box.min.y, box.max.y),
                 math::Clamp(point.z, box.min.z, box.max.z));
}

double MaxComponent(const Vec3d& value) noexcept {
    const double xy = value.x > value.y ? value.x : value.y;
    return xy > value.z ? xy : value.z;
}

} // namespace

double Quantize(double value) noexcept {
    return PlatformMath::Floor(value * GridScale) / GridScale;
}

const char* DistributionName(Distribution distribution) noexcept {
    switch (distribution) {
    case Distribution::Uniform:
        return "uniform";
    case Distribution::Terrain:
        return "terrain";
    case Distribution::Clustered:
        return "clustered";
    case Distribution::WideSparse:
        return "wide-sparse";
    case Distribution::SizeVaried:
        return "size-varied";
    case Distribution::CellBoundary:
        return "cell-boundary";
    case Distribution::SeparableDense:
        return "separable-dense";
    case Distribution::OverlapDense:
        return "overlap-dense";
    case Distribution::Count:
        break;
    }
    return "?";
}

bool ParseDistribution(const char* text, Distribution& out) noexcept {
    for (uint32 index = 0; index < static_cast<uint32>(Distribution::Count); ++index) {
        if (platform::CStringEquals(text, DistributionName(static_cast<Distribution>(index)))) {
            out = static_cast<Distribution>(index);
            return true;
        }
    }
    return false;
}

const char* QueryClassName(QueryClass queryClass) noexcept {
    switch (queryClass) {
    case QueryClass::Empty:
        return "empty";
    case QueryClass::Few:
        return "few";
    case QueryClass::Many:
        return "many";
    case QueryClass::Count:
        break;
    }
    return "?";
}

const char* QueryKindName(QueryKind kind) noexcept {
    switch (kind) {
    case QueryKind::Box:
        return "box";
    case QueryKind::Sphere:
        return "sphere";
    case QueryKind::Frustum:
        return "frustum";
    case QueryKind::Ray:
        return "ray";
    case QueryKind::Nearest:
        return "nearest";
    case QueryKind::Any:
        return "any";
    case QueryKind::CountBox:
        return "count";
    case QueryKind::Count:
        break;
    }
    return "?";
}

const char* MoveKindName(MoveKind kind) noexcept {
    switch (kind) {
    case MoveKind::Small:
        return "small";
    case MoveKind::Cross:
        return "cross";
    case MoveKind::Teleport:
        return "teleport";
    case MoveKind::Resize:
        return "resize";
    case MoveKind::Walk:
        return "walk";
    case MoveKind::Count:
        break;
    }
    return "?";
}

bool GenerateDataset(Distribution distribution, uint32 count, uint64 seed, Dataset& out) noexcept {
    out.bounds.Clear();
    out.filters.Clear();
    if (!out.bounds.TryReserve(count) || !out.filters.TryReserve(count)) {
        return false;
    }
    Sampler sampler{RandomStream(seed, 0x51a7ull + static_cast<uint64>(distribution))};
    const double uniformSide = 20000.0 * CubeRootScale(count);
    // 군집 중심은 따로 고정 시드 흐름에서 뽑는다.
    Sampler clusterSampler{RandomStream(seed, 0xc1u)};
    Vec3d clusters[32];
    const Box<double> uniformDomain = CubeAround(Vec3d(0.0, 0.0, 0.0), uniformSide * 0.5);
    for (Vec3d& cluster : clusters) {
        cluster = clusterSampler.InBox(uniformDomain);
    }
    const double clusterSigma = uniformSide / 40.0;

    switch (distribution) {
    case Distribution::Uniform:
        out.domain = uniformDomain;
        out.typicalHalf = 125.0;
        out.maxHalf = 200.0;
        break;
    case Distribution::Terrain: {
        const double side = 40000.0 * PlatformMath::Sqrt(static_cast<double>(count) / 1000.0) * 0.5;
        out.domain = Box<double>(Vec3d(-side, -side, 0.0), Vec3d(side, side, 2000.0));
        out.typicalHalf = 150.0;
        out.maxHalf = 300.0;
        break;
    }
    case Distribution::Clustered:
        out.domain = CubeAround(Vec3d(0.0, 0.0, 0.0), uniformSide * 0.5 + 3.0 * clusterSigma);
        out.typicalHalf = 62.0;
        out.maxHalf = 100.0;
        break;
    case Distribution::WideSparse:
        out.domain = CubeAround(Vec3d(0.0, 0.0, 0.0), 5.0e6);
        out.typicalHalf = 275.0;
        out.maxHalf = 500.0;
        break;
    case Distribution::SizeVaried:
        out.domain = uniformDomain;
        out.typicalHalf = 316.0;
        out.maxHalf = 10000.0;
        break;
    case Distribution::CellBoundary:
        out.domain = uniformDomain;
        out.typicalHalf = 60.0;
        out.maxHalf = 100.0;
        break;
    case Distribution::SeparableDense: {
        const double half =
            128.0 * PlatformMath::Pow(static_cast<double>(count) / 8.0, 1.0 / 3.0) * 0.5;
        out.domain = CubeAround(Vec3d(36864.0, -51200.0, 1024.0), half);
        out.typicalHalf = 5.0;
        out.maxHalf = 8.0;
        break;
    }
    case Distribution::OverlapDense:
        out.domain = CubeAround(Vec3d(5120.0, 5120.0, 512.0), 50.0);
        out.typicalHalf = 250.0;
        out.maxHalf = 300.0;
        break;
    case Distribution::Count:
        return false;
    }

    for (uint32 id = 0; id < count; ++id) {
        Vec3d center;
        Vec3d half;
        switch (distribution) {
        case Distribution::Uniform:
            center = sampler.InBox(out.domain);
            half = Vec3d(sampler.Uniform(50.0, 200.0), sampler.Uniform(50.0, 200.0),
                         sampler.Uniform(50.0, 200.0));
            break;
        case Distribution::Terrain:
            center = sampler.InBox(out.domain);
            half = Vec3d(sampler.Uniform(50.0, 300.0), sampler.Uniform(50.0, 300.0),
                         sampler.Uniform(50.0, 200.0));
            break;
        case Distribution::Clustered: {
            const Vec3d& cluster = clusters[sampler.Below(32)];
            const auto spread = [&]() noexcept {
                return (sampler.Uniform(-1.0, 1.0) + sampler.Uniform(-1.0, 1.0) +
                        sampler.Uniform(-1.0, 1.0)) *
                       clusterSigma;
            };
            center = cluster + Vec3d(spread(), spread(), spread());
            half = Vec3d(sampler.Uniform(25.0, 100.0), sampler.Uniform(25.0, 100.0),
                         sampler.Uniform(25.0, 100.0));
            break;
        }
        case Distribution::WideSparse:
            center = sampler.InBox(out.domain);
            half = Vec3d(sampler.Uniform(50.0, 500.0), sampler.Uniform(50.0, 500.0),
                         sampler.Uniform(50.0, 500.0));
            break;
        case Distribution::SizeVaried: {
            center = sampler.InBox(out.domain);
            const double size = PlatformMath::Exp(
                sampler.Uniform(PlatformMath::Log(10.0), PlatformMath::Log(10000.0)));
            half = Vec3d(size * sampler.Uniform(0.5, 1.0), size * sampler.Uniform(0.5, 1.0),
                         size * sampler.Uniform(0.5, 1.0));
            break;
        }
        case Distribution::CellBoundary: {
            const Vec3d raw = sampler.InBox(out.domain);
            double axes[3] = {raw.x, raw.y, raw.z};
            for (double& axis : axes) {
                const double step = 128.0 * static_cast<double>(1u << sampler.Below(7));
                axis = PlatformMath::Floor(axis / step + 0.5) * step + sampler.Uniform(-1.0, 1.0);
            }
            center = Vec3d(axes[0], axes[1], axes[2]);
            half = Vec3d(sampler.Uniform(20.0, 100.0), sampler.Uniform(20.0, 100.0),
                         sampler.Uniform(20.0, 100.0));
            break;
        }
        case Distribution::SeparableDense:
            center = sampler.InBox(out.domain);
            half = Vec3d(sampler.Uniform(2.0, 8.0), sampler.Uniform(2.0, 8.0),
                         sampler.Uniform(2.0, 8.0));
            break;
        case Distribution::OverlapDense:
            center = sampler.InBox(out.domain);
            half = Vec3d(sampler.Uniform(200.0, 300.0), sampler.Uniform(200.0, 300.0),
                         sampler.Uniform(200.0, 300.0));
            break;
        case Distribution::Count:
            return false;
        }
        (void)out.bounds.TryPushBack(MakeBox(center, half));
        (void)out.filters.TryPushBack(ProxyFilter(id));
    }
    return true;
}

ConvexVolume<double> MakeFrustum(const Vec3d& apex, const Vec3d& forward, double nearDistance,
                                 double farDistance) noexcept {
    const Vec3d upHint =
        forward.z > 0.9 || forward.z < -0.9 ? Vec3d(1.0, 0.0, 0.0) : Vec3d(0.0, 0.0, 1.0);
    const Vec3d right = Cross(forward, upHint).NormalizedOr(Vec3d(0.0, 1.0, 0.0));
    const Vec3d up = Cross(right, forward).NormalizedOr(Vec3d(0.0, 0.0, 1.0));
    const auto planeThrough = [](const Vec3d& normal, const Vec3d& point) noexcept {
        const Vec3d unit = normal.NormalizedOr(Vec3d(0.0, 0.0, 1.0));
        return Plane<double>(unit, Dot(unit, point));
    };
    // 법선은 안쪽을 향한다. 옆면 네 개는 반각 45도다.
    const Plane<double> planes[6] = {planeThrough(forward, apex + forward * nearDistance),
                                     planeThrough(-forward, apex + forward * farDistance),
                                     planeThrough(forward - right, apex),
                                     planeThrough(forward + right, apex),
                                     planeThrough(forward - up, apex),
                                     planeThrough(forward + up, apex)};
    ConvexVolume<double> volume;
    for (uint32 index = 0; index < 6; ++index) {
        volume.planes[index] = planes[index];
    }
    volume.planeCount = 6;
    return volume;
}

bool GenerateQueries(const Dataset& dataset, QueryClass queryClass, uint32 count, uint64 seed,
                     QuerySet& out) noexcept {
    out.boxes.Clear();
    out.spheres.Clear();
    out.volumes.Clear();
    out.rays.Clear();
    out.rayLengths.Clear();
    if (!out.boxes.TryReserve(count) || !out.spheres.TryReserve(count) ||
        !out.volumes.TryReserve(count) || !out.rays.TryReserve(count) ||
        !out.rayLengths.TryReserve(count)) {
        return false;
    }
    Sampler sampler{RandomStream(seed, 0x9e000ull + static_cast<uint64>(queryClass))};
    const Vec3d domainHalf = BoxHalf(dataset.domain);
    const Vec3d domainCenter = BoxCenter(dataset.domain);
    const double domainLargest = MaxComponent(domainHalf) * 2.0;
    const double diagonal = (domainHalf * 2.0).Length();
    const double fewHalf = 2.0 * dataset.typicalHalf;
    // 결과 없음: 영역과 가장 큰 프록시가 닿을 수 있는 범위 밖, 영역에서 멀어지는 방향.
    const double emptyOffset = 2.0 * dataset.maxHalf + 4.0 * fewHalf + 1000.0;
    const Vec3d away = Vec3d(1.0, 1.0, 1.0).NormalizedOr(Vec3d(0.0, 0.0, 1.0));

    for (uint32 index = 0; index < count; ++index) {
        Vec3d center;
        Vec3d half;
        Vec3d direction = sampler.UnitDirection();
        double rayLength = 0.0;
        double farDistance = 0.0;
        switch (queryClass) {
        case QueryClass::Empty:
            center = dataset.domain.max + Vec3d(emptyOffset, emptyOffset, emptyOffset) +
                     Vec3d(sampler.Uniform(0.0, 100.0), sampler.Uniform(0.0, 100.0),
                           sampler.Uniform(0.0, 100.0));
            half = Vec3d(fewHalf, fewHalf, fewHalf);
            // 영역에서 멀어지는 방향(모든 성분 양수)으로 돌린다.
            direction = Vec3d(direction.x < 0.0 ? -direction.x : direction.x,
                              direction.y < 0.0 ? -direction.y : direction.y,
                              direction.z < 0.0 ? -direction.z : direction.z) +
                        away;
            direction = direction.NormalizedOr(away);
            rayLength = 10.0 * dataset.typicalHalf;
            farDistance = 4.0 * dataset.typicalHalf;
            break;
        case QueryClass::Few: {
            const Box<double>& item =
                dataset.bounds[sampler.Below(static_cast<uint32>(dataset.bounds.Size()))];
            center = BoxCenter(item);
            half = Vec3d(fewHalf, fewHalf, fewHalf);
            rayLength = 10.0 * dataset.typicalHalf;
            farDistance = 4.0 * dataset.typicalHalf;
            break;
        }
        case QueryClass::Many:
            center = domainCenter + Vec3d(sampler.Uniform(-0.25, 0.25) * domainHalf.x,
                                          sampler.Uniform(-0.25, 0.25) * domainHalf.y,
                                          sampler.Uniform(-0.25, 0.25) * domainHalf.z);
            half = domainHalf * 0.5 + Vec3d(fewHalf, fewHalf, fewHalf);
            rayLength = diagonal;
            farDistance = domainLargest * 0.5;
            break;
        case QueryClass::Count:
            return false;
        }
        const Box<double> box = MakeBox(center, half);
        (void)out.boxes.TryPushBack(box);
        (void)out.spheres.TryPushBack(Sphere<double>(BoxCenter(box), Quantize(MaxComponent(half))));
        (void)out.volumes.TryPushBack(MakeFrustum(center, direction, 1.0, farDistance));
        (void)out.rays.TryPushBack(Ray<double>(center, direction));
        (void)out.rayLengths.TryPushBack(rayLength);
    }
    return true;
}

bool GenerateMoves(const Dataset& dataset, Span<const Box<double>> current, MoveKind kind,
                   uint32 count, uint64 seed, MoveBatch& out) noexcept {
    const uint32 total = static_cast<uint32>(current.Size());
    count = count > total ? total : count;
    out.ids.Clear();
    out.bounds.Clear();
    Array<uint32> order(out.ids.GetAllocator());
    if (!order.TryResize(total) || !out.ids.TryReserve(count) || !out.bounds.TryReserve(count)) {
        return false;
    }
    for (uint32 index = 0; index < total; ++index) {
        order[index] = index;
    }
    Sampler sampler{RandomStream(seed, 0x30e0ull + static_cast<uint64>(kind))};
    for (uint32 index = 0; index < count; ++index) {
        // 부분 Fisher–Yates로 중복 없이 고른다.
        const uint32 pick = index + sampler.Below(total - index);
        const uint32 id = order[pick];
        order[pick] = order[index];
        order[index] = id;

        const Box<double>& box = current[id];
        Vec3d center = BoxCenter(box);
        Vec3d half = BoxHalf(box);
        switch (kind) {
        case MoveKind::Small:
            center = center + Vec3d(sampler.Uniform(-10.0, 10.0), sampler.Uniform(-10.0, 10.0),
                                    sampler.Uniform(-10.0, 10.0));
            break;
        case MoveKind::Cross:
            center = center + Vec3d(sampler.Uniform(-500.0, 500.0), sampler.Uniform(-500.0, 500.0),
                                    sampler.Uniform(-500.0, 500.0));
            break;
        case MoveKind::Teleport:
            center = sampler.InBox(dataset.domain);
            break;
        case MoveKind::Resize: {
            const double scale = sampler.Uniform(0.5, 2.0);
            half = half * scale;
            half = Vec3d(half.x < 1.0 ? 1.0 : half.x, half.y < 1.0 ? 1.0 : half.y,
                         half.z < 1.0 ? 1.0 : half.z);
            break;
        }
        case MoveKind::Walk:
            center = ClampToBox(center + Vec3d(sampler.Uniform(-50.0, 50.0),
                                               sampler.Uniform(-50.0, 50.0),
                                               sampler.Uniform(-10.0, 10.0)),
                                dataset.domain);
            break;
        case MoveKind::Count:
            return false;
        }
        (void)out.ids.TryPushBack(id);
        (void)out.bounds.TryPushBack(MakeBox(center, half));
    }
    return true;
}

} // namespace rocks::spatialbench
