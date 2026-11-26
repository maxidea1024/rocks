#pragma once

#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <Object/ObjectHandle.h>
#include <World/Physics/QueryMobility.h>

namespace rocks {

// 장면 질의의 공통 인자다(Docs/Design/PhysicsModule 4.2절 — UE FCollisionQueryParams,
// CollisionQueryParams.h:42-126, 기본값 :210-228). 무시 목록 Span은 호출 동안만 읽는다.
//
// 사용 예:
//   const ObjectHandle self[] = {myActor};
//   QueryParams params;
//   params.ignoreActors = self;   // 자기 Actor의 body는 맞지 않는다
struct QueryParams {
    // 디버그 표시·통계용 이름이다(질의 결과에는 영향이 없다).
    StringView traceTag;
    StringView ownerTag;
    // 참이면 complex(삼각형) 형상과 판정한다. UE 함수 기본 인자(참)와 달리 기본은 거짓이다(PMD9).
    bool traceComplex = false;
    // shape trace의 시작점 겹침을 hit로 돌려준다(PH3c).
    bool findInitialOverlaps = true;
    // complex hit의 삼각형 번호를 채운다(complex 형상 단계에서).
    bool returnFaceIndex = false;
    // hit의 물리 재질을 채운다(재질 에셋 뒤).
    bool returnPhysicalMaterial = false;
    // 막음·겹침 hit를 버린다.
    bool ignoreBlocks = false;
    bool ignoreTouches = false;
    // 질의할 body 운동 종류다.
    QueryMobility mobility = QueryMobility::Any;
    // 맞지 않을 컴포넌트·Actor다.
    Span<const ObjectHandle> ignoreComponents;
    Span<const ObjectHandle> ignoreActors;
};

} // namespace rocks
