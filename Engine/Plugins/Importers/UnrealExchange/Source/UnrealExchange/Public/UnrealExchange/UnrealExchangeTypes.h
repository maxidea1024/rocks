#pragma once

#include <Core/Result.h>
#include <Object/ModuleRegistration.h>

// UnrealExchange 모듈의 리플렉션 타입 등록이다(Docs/Design/UnrealExchange 10.6절 3번).
namespace rocks {

// 모듈 리플렉션 타입(임포트 옵션 UnrealExchangeImportOptions)을 등록한다(생성 집계 함수
// generated::UnrealExchange::RegisterModuleTypes — 의존 모듈 타입도 함께). 시험은 직접, 명령은 모듈
// 시작이 부른다. 오류: 등록 오류, 할당 실패.
ROCKS_UNREALEXCHANGE_API Result<void>
RegisterUnrealExchangeTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
