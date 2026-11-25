#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Vector3.h>
#include <Core/Result.h>

namespace rocks {

// 닫힌 볼록 껍질이다(월드 좌표 꼭짓점과 삼각형 색인 — 출처는 메시 단순 충돌, 28.2절). 삼각형 감기
// 방향은 섞여도 된다. 수명: 질의하는 동안 두 Span이 가리키는 저장소가 살아 있어야 한다.
struct ConvexHullView {
    Span<const Vec3d> vertices;
    Span<const uint32> triangleIndices;
};

// 점에서 볼록 껍질 표면까지의 거리(cm)다. 안이면 0이다. 안 판정은 삼각형이 쓰는 꼭짓점의
// 평균(볼록체 안)을 기준으로 평면 방향을 맞추므로 감기 방향과 무관하다. 오류(InvalidArgument):
// 꼭짓점 4개 미만, 색인 수가 3의 배수가 아니거나 범위 밖, 비유한 값, 퇴화가 아닌 삼각형이 없다,
// 평평하다(중심이 어떤 면 평면 위 — 부피가 없다).
ROCKS_CORE_API Result<double> TryDistanceToConvexHull(const ConvexHullView& hull,
                                                      const Vec3d& point) noexcept;

} // namespace rocks
