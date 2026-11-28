#pragma once

#include <AssetCooker/CookProcessor.h>
#include <Object/ObjectRegistry.h>

// 번역 입력 모으기의 문맥이다(Docs/Design/MaterialSystem 12.8절 MS3c "번역 입력 모으기"). 쿠킹이면
// 쿠킹 문맥(CookContext)의 Registry·접근 문맥·의존 열기를, 에디터면 열린 패키지 집합의 것을 준다.
//
// 사용 예(쿠킹 처리기):
//   MaterialCookGatherContext gather{context.registry, context.access, context.dependencies};
namespace rocks {

struct MaterialCookGatherContext {
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
    // 참조 패키지 열기다(필수 — 에디터는 열린 패키지 집합 위 opener를 준다, 12.8절).
    CookDependencyOpener* opener = nullptr;
};

} // namespace rocks
