// 광선 교차다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 광선은 origin + t·direction(cm)이고, 따로
// 적지 않으면 direction은 단위 벡터이며 결과 t는 광선 매개변수다. 평면은 UE FPlane과 같이 float4(n,
// w)이고 점 p가 dot(n, p) = w면 평면 위다.
// 근거: Haines, Günther, Akenine-Möller, "Precision Improvements for Ray/Sphere Intersection", Ray
// Tracing Gems 7장 2019(구 — 상쇄 오차를 피한 판별식), Kay, Kajiya, "Ray Tracing Complex Scenes",
// SIGGRAPH 1986(slab), Woop, Benthin, Wald, "Watertight Ray/Triangle Intersection", JCGT 2(1) 2013
// (삼각형 — 공유 모서리에서 빠지는 광선이 없다. 논문의 double 대체 경로는 두지 않는다: 모서리 함수가
// 정확히 0이면 안쪽으로 본다).
#pragma once

#include "/Engine/Private/Math/ScalarMath.rsh"

// 평면과의 교차다. 평행(|n·d| < SmallestSafeDivisor)이면 false다. t는 음수일 수 있다(뒤쪽 교차).
bool RayPlaneIntersection(float3 origin, float3 direction, float4 plane, out float t) {
    const float denominator = dot(plane.xyz, direction);
    const bool parallel = abs(denominator) < SmallestSafeDivisor;
    t = parallel ? 0.0 : (plane.w - dot(plane.xyz, origin)) / (parallel ? 1.0 : denominator);
    return !parallel;
}

// 구와의 교차다. 맞으면 hits = (가까운 t, 먼 t)이고 둘 다 음수일 수 있다(호출자가 t ≥ 0을 고른다).
// direction은 단위 벡터여야 한다.
bool RaySphereIntersection(float3 origin, float3 direction, float3 center, float radius, out float2 hits) {
    const float3 f = origin - center;
    const float b = -dot(f, direction);
    const float3 closest = f + b * direction;
    const float radiusSquared = radius * radius;
    const float discriminant = radiusSquared - dot(closest, closest);
    const float c = dot(f, f) - radiusSquared;
    const float q = b + SignNotZero(b) * sqrt(max(discriminant, 0.0));
    const float t0 = q != 0.0 ? c / q : 0.0;
    const float t1 = q;
    const bool hit = discriminant >= 0.0;
    hits = hit ? float2(min(t0, t1), max(t0, t1)) : float2(0.0, 0.0);
    return hit;
}

// AABB와의 교차(slab)다. inverseDirection = 1 / direction(성분이 0이면 ±Inf — 그 축의 slab 경계에
// origin이 정확히 있으면 정의하지 않는다). 맞으면(들어가는 t ≤ 나가는 t, 나가는 t ≥ 0) range = (들어가는
// t, 나가는 t)다. direction은 단위일 필요가 없다.
bool RayAabbIntersection(float3 origin, float3 inverseDirection, float3 boxMin, float3 boxMax,
                         out float2 range) {
    const float3 t0 = (boxMin - origin) * inverseDirection;
    const float3 t1 = (boxMax - origin) * inverseDirection;
    const float3 nearT = min(t0, t1);
    const float3 farT = max(t0, t1);
    const float tEnter = max(nearT.x, max(nearT.y, nearT.z));
    const float tExit = min(farT.x, min(farT.y, farT.z));
    range = float2(tEnter, tExit);
    return tEnter <= tExit && tExit >= 0.0;
}

// 2차식 a t² + b t + c = 0의 실근(작은 쪽, 큰 쪽)이다(상쇄 없는 Vieta 형). 근이 없으면 false다.
bool SolveQuadratic(float a, float b, float c, out float2 roots) {
    const bool isLinear = abs(a) < SmallestSafeDivisor;
    const bool degenerate = isLinear && abs(b) < SmallestSafeDivisor;
    const float discriminant = b * b - 4.0 * a * c;
    const float q = -0.5 * (b + SignNotZero(b) * sqrt(max(discriminant, 0.0)));
    const float r0 = q / (isLinear ? 1.0 : a);
    const float r1 = q != 0.0 ? c / q : r0;
    const float linearRoot = -c / (degenerate ? 1.0 : b);
    roots = isLinear ? float2(linearRoot, linearRoot) : float2(min(r0, r1), max(r0, r1));
    const bool solved = isLinear ? !degenerate : discriminant >= 0.0;
    if (!solved) {
        roots = float2(0.0, 0.0);
    }

    return solved;
}

// 원뿔 옆면(꼭짓점 apex, 단위 축 axis 쪽 한 겹, 반각 cos cosHalfAngle ∈ (0, 1), 축 높이 [0, height])과의
// 가장 가까운 t ≥ 0 교차다. 밑면 원판은 포함하지 않는다.
bool RayConeIntersection(float3 origin, float3 direction, float3 apex, float3 axis, float cosHalfAngle,
                         float height, out float t) {
    const float3 co = origin - apex;
    const float dv = dot(direction, axis);
    const float cv = dot(co, axis);
    const float cosSquared = cosHalfAngle * cosHalfAngle;
    const float a = dv * dv - cosSquared * dot(direction, direction);
    const float b = 2.0 * (dv * cv - cosSquared * dot(direction, co));
    const float c = cv * cv - cosSquared * dot(co, co);
    float2 roots = float2(0.0, 0.0);
    const bool solved = SolveQuadratic(a, b, c, roots);
    // 두 근을 가까운 순서로 본다(축 높이가 [0, height]인 한 겹만).
    const float h0 = cv + roots.x * dv;
    const float h1 = cv + roots.y * dv;
    const bool first = solved && roots.x >= 0.0 && h0 >= 0.0 && h0 <= height;
    const bool second = solved && roots.y >= 0.0 && h1 >= 0.0 && h1 <= height;
    t = first ? roots.x : (second ? roots.y : 0.0);
    return first || second;
}

// 원기둥(밑면 중심 base, 단위 축 axis, 반지름 radius, 높이 [0, height], 위아래 원판 포함)과의 가장
// 가까운 t ≥ 0 교차다. 광선이 원기둥 안에서 시작하면 나가는 점이다.
bool RayCylinderIntersection(float3 origin, float3 direction, float3 base, float3 axis, float radius,
                             float height, out float t) {
    const float3 co = origin - base;
    const float dv = dot(direction, axis);
    const float cv = dot(co, axis);
    const float3 dPerp = direction - dv * axis;
    const float3 oPerp = co - cv * axis;
    float best = FloatMaxValue;
    float2 roots = float2(0.0, 0.0);
    if (SolveQuadratic(dot(dPerp, dPerp), 2.0 * dot(dPerp, oPerp), dot(oPerp, oPerp) - radius * radius,
                       roots)) {
        const float h0 = cv + roots.x * dv;
        const float h1 = cv + roots.y * dv;
        if (roots.x >= 0.0 && h0 >= 0.0 && h0 <= height) {
            best = roots.x;
        } else if (roots.y >= 0.0 && h1 >= 0.0 && h1 <= height) {
            best = roots.y;
        }
    }

    if (abs(dv) >= SmallestSafeDivisor) {
        const float radiusSquared = radius * radius;
        const float tBottom = -cv / dv;
        const float3 bottom = oPerp + tBottom * dPerp;
        if (tBottom >= 0.0 && tBottom < best && dot(bottom, bottom) <= radiusSquared) {
            best = tBottom;
        }

        const float tTop = (height - cv) / dv;
        const float3 top = oPerp + tTop * dPerp;
        if (tTop >= 0.0 && tTop < best && dot(top, top) <= radiusSquared) {
            best = tTop;
        }
    }

    t = best < FloatMaxValue ? best : 0.0;
    return best < FloatMaxValue;
}

// 2D 모서리 함수 p.x·q.y − p.y·q.x다. 두 점을 정해진 순서(x, 그다음 y 사전순)로 바꿔 계산하고 부호를
// 되돌린다 — 이웃한 두 삼각형이 공유 모서리를 반대 방향으로 지나도 같은 곱을 같은 순서로 계산하므로
// 결과가 정확히 부호만 다르다(FMA 축약이 있어도 같다 — precise를 무시하는 드라이버 대비).
float WatertightEdgeFunction(float px, float py, float qx, float qy) {
    const bool ordered = px < qx || (px == qx && py <= qy);
    const float ux = ordered ? px : qx;
    const float uy = ordered ? py : qy;
    const float vx = ordered ? qx : px;
    const float vy = ordered ? qy : py;
    precise float value = ux * vy - uy * vx;
    return ordered ? value : -value;
}

// 삼각형(v0, v1, v2, 양면)과의 watertight 교차다(Woop 2013). 맞고 tMin ≤ t ≤ tMax면 true이고
// barycentrics = (v0, v1, v2의 가중 — 합 1)다. direction은 단위일 필요가 없다. 모서리 함수는 같은
// 곱을 같은 순서로 계산하고 precise로 축약을 막아, 이웃한 삼각형이 공유 모서리에서 정확히 부호만 다른
// 값을 낸다.
bool RayTriangleIntersection(float3 origin, float3 direction, float3 v0, float3 v1, float3 v2, float tMin,
                             float tMax, out float t, out float3 barycentrics) {
    // 가장 큰 방향 성분을 z로 둔다(kz), 나머지는 회전 순서. 방향 z가 음수면 x·y를 바꿔 감김을 지킨다.
    const float3 absDirection = abs(direction);
    const uint kz = absDirection.x > absDirection.y ? (absDirection.x > absDirection.z ? 0u : 2u)
                                                    : (absDirection.y > absDirection.z ? 1u : 2u);
    const uint kxBase = kz == 2u ? 0u : kz + 1u;
    const uint kyBase = kxBase == 2u ? 0u : kxBase + 1u;
    const bool swap = direction[kz] < 0.0;
    const uint kx = swap ? kyBase : kxBase;
    const uint ky = swap ? kxBase : kyBase;
    precise float shearX = direction[kx] / direction[kz];
    precise float shearY = direction[ky] / direction[kz];
    precise float shearZ = 1.0 / direction[kz];
    precise float3 a = v0 - origin;
    precise float3 b = v1 - origin;
    precise float3 c = v2 - origin;
    precise float ax = a[kx] - shearX * a[kz];
    precise float ay = a[ky] - shearY * a[kz];
    precise float bx = b[kx] - shearX * b[kz];
    precise float by = b[ky] - shearY * b[kz];
    precise float cx = c[kx] - shearX * c[kz];
    precise float cy = c[ky] - shearY * c[kz];
    const float u = WatertightEdgeFunction(cx, cy, bx, by);
    const float v = WatertightEdgeFunction(ax, ay, cx, cy);
    const float w = WatertightEdgeFunction(bx, by, ax, ay);
    const bool mixedSigns = (u < 0.0 || v < 0.0 || w < 0.0) && (u > 0.0 || v > 0.0 || w > 0.0);
    precise float determinant = u + v + w;
    precise float az = shearZ * a[kz];
    precise float bz = shearZ * b[kz];
    precise float cz = shearZ * c[kz];
    precise float scaledT = u * az + v * bz + w * cz;
    const float inverseDeterminant = determinant == 0.0 ? 0.0 : 1.0 / determinant;
    const float hit = scaledT * inverseDeterminant;
    const bool accepted = !mixedSigns && determinant != 0.0 && hit >= tMin && hit <= tMax;
    t = accepted ? hit : 0.0;
    barycentrics = accepted ? float3(u, v, w) * inverseDeterminant : float3(0.0, 0.0, 0.0);
    return accepted;
}
