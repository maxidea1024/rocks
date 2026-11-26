#pragma once

#include <Core/Result.h>
#include <Object/ModuleRegistration.h>

namespace rocks {

// WorldIntegration 모듈 리플렉션 타입을 등록한다(생성 집계 함수
// generated::WorldIntegration::RegisterModuleTypes — 의존 모듈 World·Mesh·Material 타입도 함께).
// 도구·시험은 직접, 엔진은 모듈 시작이 부른다(Docs/Design/EngineRuntime 30.1절).
//
// 사용 예:
//   ModuleRegistration modules(types, allocator);
//   if (auto registered = RegisterWorldIntegrationTypes(modules); !registered) { ... }
ROCKS_WORLDINTEGRATION_API Result<void>
RegisterWorldIntegrationTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
