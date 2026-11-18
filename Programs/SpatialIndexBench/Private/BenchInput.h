#pragma once

#include "BenchSupport.h"

#include <Core/Math/Shapes.h>

// 입력 생성(Docs/Design/SpatialIndex.ko.md 9.2절)이다. 모든 값은 고정 시드 RandomStream에서 나오고
// 좌표는 1/64 cm 격자로 양자화한다. 그래서 중심 = (min + max)/2, 반폭 = (max − min)/2가 double에서
// 정확하고, center·extent 표현을 쓰는 비교군(B·C)과 min·max 표현을 쓰는 비교군(A·D)이 같은 AABB를
// 본다(정확성 대조에서 반올림 차이가 생기지 않는다). 길이는 cm다.
namespace rocks::spatialbench {

enum class Distribution : uint8 {
    // 균일한 3D(밀도 일정: 한 변 200 m · (N/1000)^(1/3)).
    Uniform,
    // 높이 범위가 좁은 2.5D(수평 넓음, 높이 0–20 m).
    Terrain,
    // 군집 32개(가우스형 퍼짐).
    Clustered,
    // 넓고 희소(한 변 100 km).
    WideSparse,
    // 크기 편차가 큰 분포(반폭 10 cm–100 m 로그 균일).
    SizeVaried,
    // 셀 경계 집중(128 cm · 2^k 경계 ± 1 cm).
    CellBoundary,
    // 분리 가능한 과밀(128 cm 셀당 약 8개, 작은 박스).
    SeparableDense,
    // 실제 AABB가 모두 겹치는 과밀(중심 1 m 정육면체 안, 반폭 2–3 m).
    OverlapDense,
    Count
};

const char* DistributionName(Distribution distribution) noexcept;
bool ParseDistribution(const char* text, Distribution& out) noexcept;

// 질의 결과 규모다(9.3절: 결과 없음·적음·많음).
enum class QueryClass : uint8 {
    Empty,
    Few,
    Many,
    Count
};

const char* QueryClassName(QueryClass queryClass) noexcept;

// 질의 형태다. Nearest·Any·Count는 최근접 ray·존재·개수 질의다(Any·Count는 박스).
enum class QueryKind : uint8 {
    Box,
    Sphere,
    Frustum,
    Ray,
    Nearest,
    Any,
    CountBox,
    Count
};

const char* QueryKindName(QueryKind kind) noexcept;

// 비교군이 공통으로 쓰는 질의 필터 mask다. 프록시 필터는 식별 값에서 유도한 한 bit(4종)이고 질의는
// 그중 3종을 받는다(약 75% 통과). 모든 비교군이 공간 판정보다 필터를 먼저 적용한다.
inline constexpr uint32 QueryFilterMask = 0x7u;

constexpr uint32 ProxyFilter(uint32 id) noexcept {
    return 1u << static_cast<uint32>(Mix64(id) & 3u);
}

struct Dataset {
    explicit Dataset(Allocator& allocator) noexcept : bounds(allocator), filters(allocator) {
    }

    Array<Box<double>> bounds;
    Array<uint32> filters;
    // 중심이 놓이는 영역이다(이동·질의 생성 기준).
    Box<double> domain;
    // 전형적인 반폭과 최대 반폭이다.
    double typicalHalf = 0.0;
    double maxHalf = 0.0;
};

// 분포 하나를 만든다. 실패는 할당 실패뿐이다.
bool GenerateDataset(Distribution distribution, uint32 count, uint64 seed, Dataset& out) noexcept;

// 질의 집합이다. 형태마다 같은 수의 질의를 둔다.
struct QuerySet {
    explicit QuerySet(Allocator& allocator) noexcept
        : boxes(allocator), spheres(allocator), volumes(allocator), rays(allocator),
          rayLengths(allocator) {
    }

    Array<Box<double>> boxes;
    Array<Sphere<double>> spheres;
    Array<ConvexVolume<double>> volumes;
    Array<Ray<double>> rays;
    Array<double> rayLengths;
};

bool GenerateQueries(const Dataset& dataset, QueryClass queryClass, uint32 count, uint64 seed,
                     QuerySet& out) noexcept;

// 이동 종류다(9.3절 연산별: 작은 이동, 경계 통과, 순간 이동, 크기 변경). Walk는 프레임 부하의
// 이동이다.
enum class MoveKind : uint8 {
    Small,
    Cross,
    Teleport,
    Resize,
    Walk,
    Count
};

const char* MoveKindName(MoveKind kind) noexcept;

// 갱신 묶음이다. ids[i]의 새 AABB가 bounds[i]다.
struct MoveBatch {
    explicit MoveBatch(Allocator& allocator) noexcept : ids(allocator), bounds(allocator) {
    }

    Array<uint32> ids;
    Array<Box<double>> bounds;
};

// current(현재 AABB)에서 count개를 골라 새 AABB를 만든다(같은 식별 값을 두 번 고르지 않는다).
bool GenerateMoves(const Dataset& dataset, Span<const Box<double>> current, MoveKind kind,
                   uint32 count, uint64 seed, MoveBatch& out) noexcept;

// 프러스텀을 만든다(꼭짓점 apex, 단위 방향 forward, 반각 45도, near·far 거리).
ConvexVolume<double> MakeFrustum(const Vec3d& apex, const Vec3d& forward, double nearDistance,
                                 double farDistance) noexcept;

// 1/64 cm 격자로 내린다.
double Quantize(double value) noexcept;

} // namespace rocks::spatialbench
