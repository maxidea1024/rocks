#pragma once

#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <Object/ObjectHandle.h>

namespace rocks {

// complex 형상 삼각형 번호가 없음이다(UE INDEX_NONE).
inline constexpr int32 NoHitFaceIndex = -1;

// 장면 질의 hit 하나다(Docs/Design/PhysicsModule 4.5절 — UE FHitResult). 위치는 월드 좌표 cm다.
// item·boneName·physicalMaterial은 instanced·skeletal·재질 단계에서 더한다. 리플렉션
// struct다(이벤트 인자 — 7.1.14절). component·actor는 ObjectHandle이라 NATIVE다(스크립트는 이벤트의
// otherActor·otherComponent 인자로 받는다). 이벤트를 발행하는 파일은
// World/Physics/HitResult.generated.h를 include한다.
ROCKS_STRUCT()
struct HitResult {
    ROCKS_GENERATED_BODY()

    // 시작 → 끝 비율(0–1)이다. hit가 없으면 1이다(UE 기본값).
    ROCKS_PROPERTY() double time = 1.0;
    // 시작점에서 location까지 거리(cm)다. 시작 겹침이면 0이다.
    ROCKS_PROPERTY() double distance = 0.0;
    // 형상 중심이 닿은 위치다(시작 + time·(끝 − 시작), double 계산). line은 impactPoint와 같은
    // 점이다 (impactPoint는 Jolt float 점이라 반올림만큼 다를 수 있다 — UE도 둘을 따로 계산한다).
    ROCKS_PROPERTY() Vec3d location;
    // 실제 접촉점이다. 시작 겹침이면 location이다.
    ROCKS_PROPERTY() Vec3d impactPoint;
    // 쓸고 간 형상 쪽 법선이다(line은 impactNormal과 같다).
    ROCKS_PROPERTY() Vec3d normal;
    // 맞은 표면의 바깥 법선이다.
    ROCKS_PROPERTY() Vec3d impactNormal;
    // 시작·시도한 끝이다.
    ROCKS_PROPERTY() Vec3d traceStart;
    ROCKS_PROPERTY() Vec3d traceEnd;
    // 시작 겹침의 MTD 깊이(normal 방향, cm)다. line trace는 0이다.
    ROCKS_PROPERTY() double penetrationDepth = 0.0;
    // complex hit의 삼각형 번호다(QueryParams::returnFaceIndex).
    ROCKS_PROPERTY() int32 faceIndex = NoHitFaceIndex;
    // 막음 여부·시작 겹침 여부다.
    ROCKS_PROPERTY() bool blockingHit = false;
    ROCKS_PROPERTY() bool startPenetrating = false;
    // 맞은 컴포넌트와 그 소유 Actor다.
    ROCKS_NATIVE() ObjectHandle component;
    ROCKS_NATIVE() ObjectHandle actor;

    // 시작 겹침이 아닌 막는 hit인지다(UE IsValidBlockingHit).
    bool IsValidBlockingHit() const noexcept {
        return blockingHit && !startPenetrating;
    }
};

} // namespace rocks
