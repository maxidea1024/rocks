#pragma once

#include "BenchInput.h"

#include <Core/Math/Intersection.h>
#include <Platform/PlatformMath.h>

// 비교군 A(Docs/Design/SpatialIndex.ko.md 9.1절): 조밀한 배열의 전수 검색이다. 삽입은 끝에 추가,
// 갱신은 덮어쓰기, 삭제는 swap-remove다. 식별 값 → 위치 표는 측정 구간 안에서 함께 갱신한다.
namespace rocks::spatialbench {

// 모든 비교군이 공유하는 박스 겹침이다(닫힌 구간: 접촉은 교차). D의 박스 판정과 같은 규약이다.
inline bool BoxesTouch(const Box<double>& a, const Box<double>& b) noexcept {
    return !(a.min.x > b.max.x || a.max.x < b.min.x || a.min.y > b.max.y || a.max.y < b.min.y ||
             a.min.z > b.max.z || a.max.z < b.min.z);
}

// 최근접 ray의 정밀 판정이다. 등록 AABB를 정밀 형상으로 본다(모든 비교군이 같은 식을 쓴다).
inline bool RayEntry(const Ray<double>& ray, const Box<double>& box, double maxDistance,
                     double& distance) noexcept {
    const RayHit<double> hit = Raycast(ray, box, maxDistance);
    distance = hit.distance;
    return hit.IsHit();
}

// A1: AoS double, scalar 전수 검색.
class FlatScanDouble {
public:
    explicit FlatScanDouble(Allocator& allocator) noexcept
        : entries_(allocator), positions_(allocator) {
    }

    const char* Name() const noexcept {
        return "A1";
    }

    bool Create(uint32 capacity) noexcept {
        entries_.Clear();
        positions_.Clear();
        return static_cast<bool>(entries_.TryReserve(capacity)) &&
               static_cast<bool>(positions_.TryResize(capacity));
    }

    void Destroy() noexcept {
        entries_.Empty();
        positions_.Empty();
    }

    bool Insert(uint32 id, const Box<double>& bounds, uint32 filter) noexcept {
        if (id >= positions_.Size() && !positions_.TryResize(id + 1)) {
            return false;
        }
        positions_[id] = static_cast<uint32>(entries_.Size());
        return static_cast<bool>(entries_.TryPushBack(Entry{bounds, filter, id}));
    }

    bool Remove(uint32 id, const Box<double>&) noexcept {
        const uint32 position = positions_[id];
        const uint32 last = static_cast<uint32>(entries_.Size()) - 1;
        if (position != last) {
            entries_[position] = entries_[last];
            positions_[entries_[position].id] = position;
        }
        return static_cast<bool>(entries_.TryRemoveAt(last));
    }

    bool Update(uint32 id, const Box<double>&, const Box<double>& bounds) noexcept {
        entries_[positions_[id]].bounds = bounds;
        return true;
    }

    void Maintain(uint32) noexcept {
    }

    void MaintainAll() noexcept {
    }

    void BeginRead() noexcept {
    }

    void EndRead() noexcept {
    }

    template <bool Counting, typename Test>
    void Scan(uint32 mask, ResultBuffer& out, QueryCounters& counters, Test&& test) const noexcept {
        const Entry* entries = entries_.Data();
        const usize count = entries_.Size();
        for (usize index = 0; index < count; ++index) {
            const Entry& entry = entries[index];
            if ((entry.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            if (test(entry.bounds)) {
                if constexpr (Counting) {
                    ++counters.results;
                }
                out.Push(entry.id);
            }
        }
    }

    template <bool Counting>
    void QueryBox(const Box<double>& box, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        Scan<Counting>(mask, out, counters,
                       [&](const Box<double>& bounds) noexcept { return BoxesTouch(bounds, box); });
    }

    template <bool Counting>
    void QuerySphere(const Sphere<double>& sphere, uint32 mask, ResultBuffer& out,
                     QueryCounters& counters) const noexcept {
        Scan<Counting>(mask, out, counters, [&](const Box<double>& bounds) noexcept {
            return Overlap(sphere, bounds) == QueryResult::Hit;
        });
    }

    template <bool Counting>
    void QueryFrustum(const ConvexVolume<double>& volume, uint32 mask, ResultBuffer& out,
                      QueryCounters& counters) const noexcept {
        Scan<Counting>(mask, out, counters, [&](const Box<double>& bounds) noexcept {
            return Overlap(volume, bounds) == QueryResult::Hit;
        });
    }

    template <bool Counting>
    void QueryRay(const Ray<double>& ray, double maxDistance, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        Scan<Counting>(mask, out, counters, [&](const Box<double>& bounds) noexcept {
            return Raycast(ray, bounds, maxDistance).IsHit();
        });
    }

    template <bool Counting>
    bool QueryNearest(const Ray<double>& ray, double maxDistance, uint32 mask, uint32& id,
                      double& distance, QueryCounters& counters) const noexcept {
        bool found = false;
        double best = maxDistance;
        const usize count = entries_.Size();
        for (usize index = 0; index < count; ++index) {
            const Entry& entry = entries_[index];
            if ((entry.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            double entryDistance = 0.0;
            if (RayEntry(ray, entry.bounds, maxDistance, entryDistance) &&
                (!found || entryDistance < best)) {
                found = true;
                best = entryDistance;
                id = entry.id;
            }
        }
        distance = best;
        return found;
    }

    template <bool Counting>
    bool AnyBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        const usize count = entries_.Size();
        for (usize index = 0; index < count; ++index) {
            const Entry& entry = entries_[index];
            if ((entry.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            if (BoxesTouch(entry.bounds, box)) {
                return true;
            }
        }
        return false;
    }

    template <bool Counting>
    uint64 CountBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        uint64 total = 0;
        const usize count = entries_.Size();
        for (usize index = 0; index < count; ++index) {
            const Entry& entry = entries_[index];
            if ((entry.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            total += BoxesTouch(entry.bounds, box) ? 1u : 0u;
        }
        return total;
    }

    void Describe(JsonWriter& json) const noexcept {
        json.FieldInteger("entryBytes", sizeof(Entry));
        json.FieldInteger("entries", entries_.Size());
        json.FieldInteger("reservedBytes", entries_.Capacity() * sizeof(Entry) +
                                               positions_.Capacity() * sizeof(uint32));
    }

private:
    struct Entry {
        Box<double> bounds;
        uint32 filter;
        uint32 id;
    };

    Array<Entry> entries_;
    Array<uint32> positions_;
};

// A2: float SoA(바깥쪽 반올림) 전수 검색이다. D와 같은 정밀도·필터 선적용이다: 박스 계열 질의는
// float 경계(초과 후보 가능, 누락 없음)로, 구·프러스텀·ray·최근접은 D처럼 정확한 double AABB에 Core
// 술어로 판정한다. Simd가 참이면 박스 계열을 Register4f 4개 묶음(SIP3 비교·mask·MoveMask)으로,
// 거짓이면 같은 데이터를 scalar로 판정한다(SIMD 효과를 분리하는 변형 A2s).
template <bool Simd>
class FlatScanFloat {
public:
    explicit FlatScanFloat(Allocator& allocator) noexcept
        : minX_(allocator), minY_(allocator), minZ_(allocator), maxX_(allocator), maxY_(allocator),
          maxZ_(allocator), filters_(allocator), ids_(allocator), exact_(allocator),
          positions_(allocator) {
    }

    const char* Name() const noexcept {
        return Simd ? "A2" : "A2s";
    }

    bool Create(uint32 capacity) noexcept {
        Destroy();
        return minX_.TryReserve(capacity) && minY_.TryReserve(capacity) &&
               minZ_.TryReserve(capacity) && maxX_.TryReserve(capacity) &&
               maxY_.TryReserve(capacity) && maxZ_.TryReserve(capacity) &&
               filters_.TryReserve(capacity) && ids_.TryReserve(capacity) &&
               exact_.TryReserve(capacity) && positions_.TryResize(capacity);
    }

    void Destroy() noexcept {
        minX_.Empty();
        minY_.Empty();
        minZ_.Empty();
        maxX_.Empty();
        maxY_.Empty();
        maxZ_.Empty();
        filters_.Empty();
        ids_.Empty();
        exact_.Empty();
        positions_.Empty();
    }

    bool Insert(uint32 id, const Box<double>& bounds, uint32 filter) noexcept {
        if (id >= positions_.Size() && !positions_.TryResize(id + 1)) {
            return false;
        }
        positions_[id] = static_cast<uint32>(ids_.Size());
        const Box<float> stored = bounds.ToFloatOutward();
        return minX_.TryPushBack(stored.min.x) && minY_.TryPushBack(stored.min.y) &&
               minZ_.TryPushBack(stored.min.z) && maxX_.TryPushBack(stored.max.x) &&
               maxY_.TryPushBack(stored.max.y) && maxZ_.TryPushBack(stored.max.z) &&
               filters_.TryPushBack(filter) && ids_.TryPushBack(id) && exact_.TryPushBack(bounds);
    }

    bool Remove(uint32 id, const Box<double>&) noexcept {
        const uint32 position = positions_[id];
        const uint32 last = static_cast<uint32>(ids_.Size()) - 1;
        if (position != last) {
            minX_[position] = minX_[last];
            minY_[position] = minY_[last];
            minZ_[position] = minZ_[last];
            maxX_[position] = maxX_[last];
            maxY_[position] = maxY_[last];
            maxZ_[position] = maxZ_[last];
            filters_[position] = filters_[last];
            ids_[position] = ids_[last];
            exact_[position] = exact_[last];
            positions_[ids_[position]] = position;
        }
        return minX_.TryRemoveAt(last) && minY_.TryRemoveAt(last) && minZ_.TryRemoveAt(last) &&
               maxX_.TryRemoveAt(last) && maxY_.TryRemoveAt(last) && maxZ_.TryRemoveAt(last) &&
               filters_.TryRemoveAt(last) && ids_.TryRemoveAt(last) && exact_.TryRemoveAt(last);
    }

    bool Update(uint32 id, const Box<double>&, const Box<double>& bounds) noexcept {
        const uint32 position = positions_[id];
        const Box<float> stored = bounds.ToFloatOutward();
        minX_[position] = stored.min.x;
        minY_[position] = stored.min.y;
        minZ_[position] = stored.min.z;
        maxX_[position] = stored.max.x;
        maxY_[position] = stored.max.y;
        maxZ_[position] = stored.max.z;
        exact_[position] = bounds;
        return true;
    }

    void Maintain(uint32) noexcept {
    }

    void MaintainAll() noexcept {
    }

    void BeginRead() noexcept {
    }

    void EndRead() noexcept {
    }

    // 박스 계열 판정의 공통 순회다. visit(위치)가 거짓이면 멈추고 거짓을 돌려준다.
    template <bool Counting, typename Visit>
    bool ScanBox(const Box<double>& box, uint32 mask, QueryCounters& counters,
                 Visit&& visit) const noexcept {
        const Box<float> query = box.ToFloatOutward();
        const usize count = ids_.Size();
        const float* minX = minX_.Data();
        const float* minY = minY_.Data();
        const float* minZ = minZ_.Data();
        const float* maxX = maxX_.Data();
        const float* maxY = maxY_.Data();
        const float* maxZ = maxZ_.Data();
        const uint32* filters = filters_.Data();
        usize index = 0;
        if constexpr (Simd) {
            namespace simd = platform::simd;
            const simd::Register4f queryMinX = simd::Splat(query.min.x);
            const simd::Register4f queryMinY = simd::Splat(query.min.y);
            const simd::Register4f queryMinZ = simd::Splat(query.min.z);
            const simd::Register4f queryMaxX = simd::Splat(query.max.x);
            const simd::Register4f queryMaxY = simd::Splat(query.max.y);
            const simd::Register4f queryMaxZ = simd::Splat(query.max.z);
            for (; index + 4 <= count; index += 4) {
                // 필터를 먼저 적용한다(통과한 lane이 없으면 경계를 읽지 않는다).
                const uint32 filterBits = ((filters[index] & mask) != 0 ? 1u : 0u) |
                                          ((filters[index + 1] & mask) != 0 ? 2u : 0u) |
                                          ((filters[index + 2] & mask) != 0 ? 4u : 0u) |
                                          ((filters[index + 3] & mask) != 0 ? 8u : 0u);
                if (filterBits == 0) {
                    continue;
                }
                if constexpr (Counting) {
                    counters.testedProxies += platform::PlatformMath::PopCount32(filterBits);
                }
                const simd::Register4f x =
                    simd::BitAnd(simd::CompareLessEqual(simd::Load(minX + index), queryMaxX),
                                 simd::CompareGreaterEqual(simd::Load(maxX + index), queryMinX));
                const simd::Register4f y =
                    simd::BitAnd(simd::CompareLessEqual(simd::Load(minY + index), queryMaxY),
                                 simd::CompareGreaterEqual(simd::Load(maxY + index), queryMinY));
                const simd::Register4f z =
                    simd::BitAnd(simd::CompareLessEqual(simd::Load(minZ + index), queryMaxZ),
                                 simd::CompareGreaterEqual(simd::Load(maxZ + index), queryMinZ));
                uint32 hits = simd::MoveMask(simd::BitAnd(simd::BitAnd(x, y), z)) & filterBits;
                while (hits != 0) {
                    const uint32 lane = platform::PlatformMath::CountTrailingZeros32(hits);
                    hits &= hits - 1;
                    if (!visit(index + lane)) {
                        return false;
                    }
                }
            }
        }
        for (; index < count; ++index) {
            if ((filters[index] & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            if (minX[index] > query.max.x || maxX[index] < query.min.x ||
                minY[index] > query.max.y || maxY[index] < query.min.y ||
                minZ[index] > query.max.z || maxZ[index] < query.min.z) {
                continue;
            }
            if (!visit(index)) {
                return false;
            }
        }
        return true;
    }

    template <bool Counting, typename Test>
    void ScanExact(uint32 mask, ResultBuffer& out, QueryCounters& counters,
                   Test&& test) const noexcept {
        const usize count = ids_.Size();
        for (usize index = 0; index < count; ++index) {
            if ((filters_[index] & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            if (test(exact_[index])) {
                if constexpr (Counting) {
                    ++counters.results;
                }
                out.Push(ids_[index]);
            }
        }
    }

    template <bool Counting>
    void QueryBox(const Box<double>& box, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        const uint32* ids = ids_.Data();
        (void)ScanBox<Counting>(box, mask, counters, [&](usize index) noexcept {
            if constexpr (Counting) {
                ++counters.results;
            }
            out.Push(ids[index]);
            return true;
        });
    }

    template <bool Counting>
    void QuerySphere(const Sphere<double>& sphere, uint32 mask, ResultBuffer& out,
                     QueryCounters& counters) const noexcept {
        ScanExact<Counting>(mask, out, counters, [&](const Box<double>& bounds) noexcept {
            return Overlap(sphere, bounds) == QueryResult::Hit;
        });
    }

    template <bool Counting>
    void QueryFrustum(const ConvexVolume<double>& volume, uint32 mask, ResultBuffer& out,
                      QueryCounters& counters) const noexcept {
        ScanExact<Counting>(mask, out, counters, [&](const Box<double>& bounds) noexcept {
            return Overlap(volume, bounds) == QueryResult::Hit;
        });
    }

    template <bool Counting>
    void QueryRay(const Ray<double>& ray, double maxDistance, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        ScanExact<Counting>(mask, out, counters, [&](const Box<double>& bounds) noexcept {
            return Raycast(ray, bounds, maxDistance).IsHit();
        });
    }

    template <bool Counting>
    bool QueryNearest(const Ray<double>& ray, double maxDistance, uint32 mask, uint32& id,
                      double& distance, QueryCounters& counters) const noexcept {
        bool found = false;
        double best = maxDistance;
        const usize count = ids_.Size();
        for (usize index = 0; index < count; ++index) {
            if ((filters_[index] & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            double entryDistance = 0.0;
            if (RayEntry(ray, exact_[index], maxDistance, entryDistance) &&
                (!found || entryDistance < best)) {
                found = true;
                best = entryDistance;
                id = ids_[index];
            }
        }
        distance = best;
        return found;
    }

    template <bool Counting>
    bool AnyBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        bool any = false;
        (void)ScanBox<Counting>(box, mask, counters, [&](usize) noexcept {
            any = true;
            return false;
        });
        return any;
    }

    template <bool Counting>
    uint64 CountBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        uint64 total = 0;
        (void)ScanBox<Counting>(box, mask, counters, [&](usize) noexcept {
            ++total;
            return true;
        });
        return total;
    }

    void Describe(JsonWriter& json) const noexcept {
        json.FieldInteger("entryBytes",
                          6 * sizeof(float) + 2 * sizeof(uint32) + sizeof(Box<double>));
        json.FieldInteger("entries", ids_.Size());
    }

private:
    Array<float> minX_;
    Array<float> minY_;
    Array<float> minZ_;
    Array<float> maxX_;
    Array<float> maxY_;
    Array<float> maxZ_;
    Array<uint32> filters_;
    Array<uint32> ids_;
    // 구·프러스텀·ray·최근접용 정확한 double AABB다(D의 레코드에 대응).
    Array<Box<double>> exact_;
    Array<uint32> positions_;
};

} // namespace rocks::spatialbench
