// This file is a C++ re-implementation, written for benchmarking only, of the behavior and data
// structures of BoundsOctree.cs and BoundsOctreeNode.cs from UnityOctree
// (https://github.com/Nition/UnityOctree, commit bbc473571c8a0879024077a928ca658a52ecbd99).
// No original source text was copied. The original license follows.
//
// Copyright (c) 2014, Nition
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#pragma once

#include "FlatScan.h"

// 비교군 B "UnityOctree C++ 이식"(Docs/Design/SpatialIndex.ko.md 9.1절)이다. 출처 등록은
// Engine/Source/ThirdParty/UnityOctree/ThirdParty.json이다. 원본의 동작과 자료구조(노드별 개별
// 할당, 노드별 동적 배열, 노드마다 따로 할당한 자식 경계 배열, 임계 8, 재귀 순회, 자식 0–7 고정
// 순서, 루트 확장 21회 한도, 삭제마다 1단계 축소 시도)를 그대로 따른다. 최적화를 섞지 않는다.
//
// 원본과 달라진 점(벤치 공통 조건 때문):
// - 자료형: Unity Bounds(float center·extents)와 float 길이를 double로 바꿨다(모든 비교군이 같은
//   double AABB 입력을 쓴다). 표현은 Unity처럼 center·extents이고 min·max는 그때마다 계산한다.
// - 객체 T는 식별 값(uint32)이고, 같은 필터 조건을 위해 필터 mask를 객체에 함께 두고 공간 판정 전에
//   검사한다(원본에 없는 기능).
// - 할당: C# new·GC 대신 벤치 공통 Allocator로 할당하고, 원본이 참조를 버려 GC에 맡기는 곳(병합한
//   자식, 버린 루트, 축소로 버린 형제)에서 바로 해제한다. GC 비용은 측정하지 않는다.
// - List<T>는 C# 규칙(빈 상태에서 첫 추가 때 용량 4, 이후 2배, 줄지 않음, Remove는 앞에서 찾아
//   뒤를 당김, 값 같음은 구조체 필드 전체 비교)을 따르는 CsList로 옮겼다.
// - 원본의 경고·오류 로그(Debug.LogError)는 계수로 바꿨다(addFailures).
// - 질의 결과는 List.Add 대신 벤치 공통 결과 버퍼에 식별 값을 쓴다.
// - 구 질의·최근접·개수는 원본에 없어 GetColliding과 같은 순회로 덧붙였다(벤치 추가 기능).
//
// Unity Bounds 경계 규칙(UnityCsReference master cf583fbe, 2026-11-05 확인):
// - Intersects는 관리 코드이며 축마다 min <= other.max && max >= other.min(닫힌 구간)이다. 확인.
// - min·max는 center ∓ extents로 계산한다. 확인.
// - Contains(point)는 native IsInside, IntersectRay는 native IntersectRayAABB라 소스가 공개되지
// 않았다.
//   Contains는 닫힌 구간(경계 위 점 포함), IntersectRay는 slab 진입 거리(원점이 안이면 Hit)로
//   가정하고 Core Raycast로 계산한다. GeometryUtility.TestPlanesAABB도 native라 "어느 평면에서도
//   완전히 바깥이 아님"으로 가정하고 Core Overlap(ConvexVolume, Box)로 계산한다.
namespace rocks::spatialbench::unity {

// UnityEngine.Bounds의 대응이다(double).
struct Bounds {
    Vec3d center;
    Vec3d extents;

    static Bounds FromCenterSize(const Vec3d& center, const Vec3d& size) noexcept {
        return Bounds{center, size * 0.5};
    }

    static Bounds FromBox(const Box<double>& box) noexcept {
        return Bounds{Vec3d(box.min.x * 0.5 + box.max.x * 0.5, box.min.y * 0.5 + box.max.y * 0.5,
                            box.min.z * 0.5 + box.max.z * 0.5),
                      Vec3d((box.max.x - box.min.x) * 0.5, (box.max.y - box.min.y) * 0.5,
                            (box.max.z - box.min.z) * 0.5)};
    }

    Vec3d Min() const noexcept {
        return center - extents;
    }

    Vec3d Max() const noexcept {
        return center + extents;
    }

    Box<double> ToBox() const noexcept {
        return Box<double>(Min(), Max());
    }

    bool Intersects(const Bounds& other) const noexcept {
        const Vec3d thisMin = Min();
        const Vec3d thisMax = Max();
        const Vec3d otherMin = other.Min();
        const Vec3d otherMax = other.Max();
        return thisMin.x <= otherMax.x && thisMax.x >= otherMin.x && thisMin.y <= otherMax.y &&
               thisMax.y >= otherMin.y && thisMin.z <= otherMax.z && thisMax.z >= otherMin.z;
    }

    // 가정: 닫힌 구간(native IsInside).
    bool Contains(const Vec3d& point) const noexcept {
        const Vec3d minimum = Min();
        const Vec3d maximum = Max();
        return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y &&
               point.y <= maximum.y && point.z >= minimum.z && point.z <= maximum.z;
    }

    // 가정: slab 진입 거리(native IntersectRayAABB).
    bool IntersectRay(const Ray<double>& ray, double& distance) const noexcept {
        const RayHit<double> hit = Raycast(ray, ToBox());
        distance = hit.distance;
        return hit.IsHit();
    }

    bool operator==(const Bounds& other) const noexcept {
        return center == other.center && extents == other.extents;
    }
};

// C# List<T>의 대응이다(위 규칙).
template <typename T>
class CsList {
public:
    CsList() noexcept = default;
    CsList(const CsList&) = delete;
    CsList& operator=(const CsList&) = delete;

    ~CsList() noexcept {
        Release();
    }

    void Bind(Allocator& allocator) noexcept {
        allocator_ = &allocator;
    }

    void Release() noexcept {
        if (items_ != nullptr) {
            (void)allocator_->Deallocate(items_);
            items_ = nullptr;
        }
        count_ = 0;
        capacity_ = 0;
    }

    uint32 Count() const noexcept {
        return count_;
    }

    T& operator[](uint32 index) noexcept {
        return items_[index];
    }

    const T& operator[](uint32 index) const noexcept {
        return items_[index];
    }

    bool Add(const T& value) noexcept {
        if (count_ == capacity_) {
            const uint32 grown = capacity_ == 0 ? 4u : capacity_ * 2u;
            Result<void*> storage = allocator_->Allocate(sizeof(T) * grown, alignof(T));
            if (!storage) {
                return false;
            }
            T* items = static_cast<T*>(storage.Value());
            for (uint32 index = 0; index < count_; ++index) {
                items[index] = items_[index];
            }
            if (items_ != nullptr) {
                (void)allocator_->Deallocate(items_);
            }
            items_ = items;
            capacity_ = grown;
        }
        items_[count_++] = value;
        return true;
    }

    void RemoveAt(uint32 index) noexcept {
        for (uint32 position = index + 1; position < count_; ++position) {
            items_[position - 1] = items_[position];
        }
        --count_;
    }

    // List.Remove(item): 앞에서부터 같은 값을 찾아 지운다.
    bool Remove(const T& value) noexcept {
        for (uint32 index = 0; index < count_; ++index) {
            if (items_[index] == value) {
                RemoveAt(index);
                return true;
            }
        }
        return false;
    }

    usize ReservedBytes() const noexcept {
        return static_cast<usize>(capacity_) * sizeof(T);
    }

private:
    Allocator* allocator_ = nullptr;
    T* items_ = nullptr;
    uint32 count_ = 0;
    uint32 capacity_ = 0;
};

struct OctreeObject {
    uint32 obj;
    uint32 filter;
    Bounds bounds;

    // C# 구조체 기본 Equals(모든 필드 비교)다.
    bool operator==(const OctreeObject& other) const noexcept {
        return obj == other.obj && filter == other.filter && bounds == other.bounds;
    }
};

// BoundsOctreeNode<T>의 대응이다.
class BoundsOctreeNode {
public:
    static constexpr uint32 NumObjectsAllowed = 8;

    BoundsOctreeNode(Allocator& allocator, double baseLength, double minSize, double looseness,
                     const Vec3d& center) noexcept;
    ~BoundsOctreeNode() noexcept;

    BoundsOctreeNode(const BoundsOctreeNode&) = delete;
    BoundsOctreeNode& operator=(const BoundsOctreeNode&) = delete;

    static BoundsOctreeNode* Create(Allocator& allocator, double baseLength, double minSize,
                                    double looseness, const Vec3d& center) noexcept;
    // 노드와 그 아래 전체(자식 배열·객체 목록·자식 경계 배열)를 해제한다. GC 대응이다.
    static void Release(Allocator& allocator, BoundsOctreeNode* node) noexcept;

    bool Add(uint32 obj, uint32 filter, const Bounds& objBounds) noexcept;
    bool Remove(uint32 obj) noexcept;
    bool Remove(uint32 obj, const Bounds& objBounds) noexcept;
    void SetChildren(BoundsOctreeNode** childOctrees) noexcept;
    BoundsOctreeNode* ShrinkIfPossible(double minLength) noexcept;
    int32 BestFitChild(const Vec3d& objBoundsCenter) const noexcept;
    bool HasAnyObjects() const noexcept;

    const Vec3d& Center() const noexcept {
        return center_;
    }

    double BaseLength() const noexcept {
        return baseLength_;
    }

    // ---- 질의(재귀, 자식 0–7 고정 순서) ----
    template <bool Counting, typename Test, typename Emit>
    void Collect(const Test& test, uint32 mask, Emit& emit,
                 QueryCounters& counters) const noexcept {
        if constexpr (Counting) {
            ++counters.visitedNodes;
        }
        if (!test.Node(bounds_)) {
            return;
        }
        for (uint32 index = 0; index < objects_.Count(); ++index) {
            const OctreeObject& object = objects_[index];
            if ((object.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            if (test.Object(object.bounds)) {
                emit(object);
            }
        }
        if (children_ != nullptr) {
            for (uint32 index = 0; index < 8; ++index) {
                children_[index]->Collect<Counting>(test, mask, emit, counters);
            }
        }
    }

    // IsColliding(ref Bounds)의 대응(필터 추가)이다.
    template <bool Counting>
    bool IsColliding(const Bounds& checkBounds, uint32 mask,
                     QueryCounters& counters) const noexcept {
        if constexpr (Counting) {
            ++counters.visitedNodes;
        }
        if (!bounds_.Intersects(checkBounds)) {
            return false;
        }
        for (uint32 index = 0; index < objects_.Count(); ++index) {
            const OctreeObject& object = objects_[index];
            if ((object.filter & mask) == 0) {
                continue;
            }
            if constexpr (Counting) {
                ++counters.testedProxies;
            }
            if (object.bounds.Intersects(checkBounds)) {
                return true;
            }
        }
        if (children_ != nullptr) {
            for (uint32 index = 0; index < 8; ++index) {
                if (children_[index]->IsColliding<Counting>(checkBounds, mask, counters)) {
                    return true;
                }
            }
        }
        return false;
    }

    // 구조 통계(노드 수, 최대 깊이, 객체 목록·배열 예약 byte, 노드당 최대 객체 수)다.
    void Measure(uint32 depth, uint64& nodes, uint32& maxDepth, uint64& reservedBytes,
                 uint32& maxObjects) const noexcept;

private:
    void SetValues(double baseLength, double minSize, double looseness,
                   const Vec3d& center) noexcept;
    void SubAdd(uint32 obj, uint32 filter, const Bounds& objBounds) noexcept;
    bool SubRemove(uint32 obj, const Bounds& objBounds) noexcept;
    void Split() noexcept;
    void Merge() noexcept;
    bool ShouldMerge() const noexcept;
    static bool Encapsulates(const Bounds& outer, const Bounds& inner) noexcept;

    Allocator* allocator_;
    Vec3d center_;
    double baseLength_ = 0.0;
    double looseness_ = 0.0;
    double minSize_ = 0.0;
    double adjLength_ = 0.0;
    Bounds bounds_;
    CsList<OctreeObject> objects_;
    // 자식 8개 포인터 배열(원본 BoundsOctreeNode<T>[])이다. 없으면 nullptr.
    BoundsOctreeNode** children_ = nullptr;
    // 노드마다 따로 할당한 자식 경계 배열(원본 Bounds[8])이다.
    Bounds* childBounds_ = nullptr;

    friend class BoundsOctree;
};

// BoundsOctree<T>의 대응이다.
class BoundsOctree {
public:
    BoundsOctree(Allocator& allocator, double initialWorldSize, const Vec3d& initialWorldPos,
                 double minNodeSize, double loosenessValue) noexcept;
    ~BoundsOctree() noexcept;

    BoundsOctree(const BoundsOctree&) = delete;
    BoundsOctree& operator=(const BoundsOctree&) = delete;

    // 원본은 21번째 확장 뒤 로그만 남기고 반환값 없이 끝난다. 이식은 그 경우 거짓을 돌려주고 센다.
    bool Add(uint32 obj, uint32 filter, const Bounds& objBounds) noexcept;
    bool Remove(uint32 obj) noexcept;
    bool Remove(uint32 obj, const Bounds& objBounds) noexcept;

    const BoundsOctreeNode* Root() const noexcept {
        return rootNode_;
    }

    int32 Count() const noexcept {
        return count_;
    }

    uint64 addFailures = 0;
    uint64 grows = 0;

private:
    void Grow(const Vec3d& direction) noexcept;
    void Shrink() noexcept;

    Allocator* allocator_;
    int32 count_ = 0;
    BoundsOctreeNode* rootNode_ = nullptr;
    double looseness_;
    double initialSize_;
    double minSize_;
};

} // namespace rocks::spatialbench::unity

namespace rocks::spatialbench {

// 비교군 B의 벤치 API 래퍼다. 삭제·이동은 경계를 주는 삭제(원본의 빠른 경로)를 쓴다. 그 삭제가
// 원본 동작대로 실패하면(루트 확장 뒤 경로가 달라진 경우 등) 경계 없는 삭제(DFS)로 지우고 수를
// 센다.
class UnityIndex {
public:
    UnityIndex(Allocator& allocator, double initialWorldSize, double minNodeSize,
               double looseness) noexcept
        : allocator_(&allocator), initialWorldSize_(initialWorldSize), minNodeSize_(minNodeSize),
          looseness_(looseness) {
    }

    ~UnityIndex() noexcept {
        Destroy();
    }

    UnityIndex(const UnityIndex&) = delete;
    UnityIndex& operator=(const UnityIndex&) = delete;

    const char* Name() const noexcept {
        return "B";
    }

    bool Create(uint32 capacity) noexcept;
    void Destroy() noexcept;
    bool Insert(uint32 id, const Box<double>& bounds, uint32 filter) noexcept;
    bool Remove(uint32 id, const Box<double>& bounds) noexcept;
    // 경계 없는 삭제(DFS)다. 별도 행으로 잰다.
    bool RemoveWithoutBounds(uint32 id) noexcept;
    bool Update(uint32 id, const Box<double>& oldBounds, const Box<double>& bounds) noexcept;

    void Maintain(uint32) noexcept {
    }

    void MaintainAll() noexcept {
    }

    void BeginRead() noexcept {
    }

    void EndRead() noexcept {
    }

    template <bool Counting>
    void QueryBox(const Box<double>& box, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        const unity::Bounds check = unity::Bounds::FromBox(box);
        struct Test {
            const unity::Bounds* check;
            bool Node(const unity::Bounds& bounds) const noexcept {
                return bounds.Intersects(*check);
            }
            bool Object(const unity::Bounds& bounds) const noexcept {
                return bounds.Intersects(*check);
            }
        } test{&check};
        auto emit = [&](const unity::OctreeObject& object) noexcept { out.Push(object.obj); };
        tree_->Root()->Collect<Counting>(test, mask, emit, counters);
    }

    template <bool Counting>
    void QuerySphere(const Sphere<double>& sphere, uint32 mask, ResultBuffer& out,
                     QueryCounters& counters) const noexcept {
        struct Test {
            const Sphere<double>* sphere;
            bool Node(const unity::Bounds& bounds) const noexcept {
                return Overlap(*sphere, bounds.ToBox()) == QueryResult::Hit;
            }
            bool Object(const unity::Bounds& bounds) const noexcept {
                return Overlap(*sphere, bounds.ToBox()) == QueryResult::Hit;
            }
        } test{&sphere};
        auto emit = [&](const unity::OctreeObject& object) noexcept { out.Push(object.obj); };
        tree_->Root()->Collect<Counting>(test, mask, emit, counters);
    }

    // GetWithinFrustum의 대응이다(TestPlanesAABB 가정은 위 설명).
    template <bool Counting>
    void QueryFrustum(const ConvexVolume<double>& volume, uint32 mask, ResultBuffer& out,
                      QueryCounters& counters) const noexcept {
        struct Test {
            const ConvexVolume<double>* volume;
            bool Node(const unity::Bounds& bounds) const noexcept {
                return Overlap(*volume, bounds.ToBox()) == QueryResult::Hit;
            }
            bool Object(const unity::Bounds& bounds) const noexcept {
                return Overlap(*volume, bounds.ToBox()) == QueryResult::Hit;
            }
        } test{&volume};
        auto emit = [&](const unity::OctreeObject& object) noexcept { out.Push(object.obj); };
        tree_->Root()->Collect<Counting>(test, mask, emit, counters);
    }

    // GetColliding(ref Ray, …, maxDistance)의 대응이다. 노드는 진입 거리가 maxDistance보다 크면,
    // 객체는 진입 거리가 maxDistance 이하일 때만 받는다(원본 비교식 그대로).
    template <bool Counting>
    void QueryRay(const Ray<double>& ray, double maxDistance, uint32 mask, ResultBuffer& out,
                  QueryCounters& counters) const noexcept {
        RayTest test{&ray, maxDistance};
        auto emit = [&](const unity::OctreeObject& object) noexcept { out.Push(object.obj); };
        tree_->Root()->Collect<Counting>(test, mask, emit, counters);
    }

    // 원본에 최근접이 없다. GetColliding(ray)과 같은 순회로 후보마다 정밀 판정해 가장 가까운 것을
    // 고른다.
    template <bool Counting>
    bool QueryNearest(const Ray<double>& ray, double maxDistance, uint32 mask, uint32& id,
                      double& distance, QueryCounters& counters) const noexcept {
        RayTest test{&ray, maxDistance};
        bool found = false;
        double best = maxDistance;
        auto emit = [&](const unity::OctreeObject& object) noexcept {
            double entry = 0.0;
            if (RayEntry(ray, object.bounds.ToBox(), maxDistance, entry) &&
                (!found || entry < best)) {
                found = true;
                best = entry;
                id = object.obj;
            }
        };
        tree_->Root()->Collect<Counting>(test, mask, emit, counters);
        distance = best;
        return found;
    }

    template <bool Counting>
    bool AnyBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        return tree_->Root()->IsColliding<Counting>(unity::Bounds::FromBox(box), mask, counters);
    }

    template <bool Counting>
    uint64 CountBox(const Box<double>& box, uint32 mask, QueryCounters& counters) const noexcept {
        const unity::Bounds check = unity::Bounds::FromBox(box);
        struct Test {
            const unity::Bounds* check;
            bool Node(const unity::Bounds& bounds) const noexcept {
                return bounds.Intersects(*check);
            }
            bool Object(const unity::Bounds& bounds) const noexcept {
                return bounds.Intersects(*check);
            }
        } test{&check};
        uint64 total = 0;
        auto emit = [&](const unity::OctreeObject&) noexcept { ++total; };
        tree_->Root()->Collect<Counting>(test, mask, emit, counters);
        return total;
    }

    void Describe(JsonWriter& json) const noexcept;

    uint64 boundedRemoveFallbacks = 0;

private:
    struct RayTest {
        const Ray<double>* ray;
        double maxDistance;

        bool Node(const unity::Bounds& bounds) const noexcept {
            double distance = 0.0;
            return bounds.IntersectRay(*ray, distance) && !(distance > maxDistance);
        }

        bool Object(const unity::Bounds& bounds) const noexcept {
            double distance = 0.0;
            return bounds.IntersectRay(*ray, distance) && distance <= maxDistance;
        }
    };

    Allocator* allocator_;
    double initialWorldSize_;
    double minNodeSize_;
    double looseness_;
    unity::BoundsOctree* tree_ = nullptr;
};

} // namespace rocks::spatialbench
