#pragma once

#include <Core/Math/Vector3.h>
#include <Object/ObjectHandle.h>

namespace rocks {

// complex 형상 삼각형 번호가 없음이다(UE INDEX_NONE).
inline constexpr int32 NoHitFaceIndex = -1;

// 장면 질의 hit 하나다(Docs/Design/PhysicsModule 4.5절 — UE FHitResult). 위치는 월드 좌표 cm다.
// item·boneName·physicalMaterial은 instanced·skeletal·재질 단계에서 더한다.
struct HitResult {
    // 시작 → 끝 비율(0–1)이다. hit가 없으면 1이다(UE 기본값).
    double time = 1.0;
    // 시작점에서 location까지 거리(cm)다. 시작 겹침이면 0이다.
    double distance = 0.0;
    // 형상 중심이 닿은 위치다(시작 + time·(끝 − 시작), double 계산). line은 impactPoint와 같은
    // 점이다 (impactPoint는 Jolt float 점이라 반올림만큼 다를 수 있다 — UE도 둘을 따로 계산한다).
    Vec3d location;
    // 실제 접촉점이다. 시작 겹침이면 location이다.
    Vec3d impactPoint;
    // 쓸고 간 형상 쪽 법선이다(line은 impactNormal과 같다).
    Vec3d normal;
    // 맞은 표면의 바깥 법선이다.
    Vec3d impactNormal;
    // 시작·시도한 끝이다.
    Vec3d traceStart;
    Vec3d traceEnd;
    // 시작 겹침의 MTD 깊이(normal 방향, cm)다. line trace는 0이다.
    double penetrationDepth = 0.0;
    // complex hit의 삼각형 번호다(QueryParams::returnFaceIndex).
    int32 faceIndex = NoHitFaceIndex;
    // 막음 여부·시작 겹침 여부다.
    bool blockingHit = false;
    bool startPenetrating = false;
    // 맞은 컴포넌트와 그 소유 Actor다.
    ObjectHandle component;
    ObjectHandle actor;

    // 시작 겹침이 아닌 막는 hit인지다(UE IsValidBlockingHit).
    bool IsValidBlockingHit() const noexcept {
        return blockingHit && !startPenetrating;
    }
};

} // namespace rocks
