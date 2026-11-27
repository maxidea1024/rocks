#pragma once

#include <Core/Result.h>
#include <Object/ModuleRegistration.h>

// Material 모듈의 리플렉션 타입 등록이다(Docs/Design/MaterialSystem 4.6절).
namespace rocks {

// Material 리플렉션 타입을 등록한다(생성 집계 함수 generated::Material::RegisterModuleTypes를
// 부른다 — 의존 모듈 타입도 함께). 도구·시험은 직접, 엔진은 머티리얼 에셋 모듈이 부른다. 오류: 등록
// 오류(이미 등록·할당 실패 등).
ROCKS_MATERIAL_API Result<void> RegisterMaterialTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
