#pragma once

#include <Config/CVarAccess.h>
#include <Config/CVarBuilder.h>
#include <Config/ConfigContext.h>

// CVar의 공개 헤더다(Docs/Design/CVarSystem.ko.md — CVR1: 핸들·빌더·enum 이름·등록과 번호).
// 정의는 사용하는 곳(.cpp) 근처에 빌더 한 문장으로 쓰고, 모듈 등록 함수에서
// ConfigContext::Register로 올린다. 등록 누락은 Policy `cvar-registration`이 빌드에서 잡는다(VD1).
//
// 사용 예:
//   #include <Config/CVar.h>
//
//   namespace rocks::render {
//
//   CVar<float> ShadowDistance = cvar::Var("render.shadow.distance", 8000.f)
//       .Help("Max shadow distance (cm)").Range(500, 20000).Clamp();
//   BootCVar<String> GraphicsBackend = cvar::Boot("render.backend", "vulkan")
//       .Help("Graphics backend");
//
//   Result<void> RegisterRenderVars(config::ConfigContext& config) noexcept {
//       return config.Register(ShadowDistance, GraphicsBackend);
//   }
//
//   } // namespace rocks::render
//
// UE 대응: CVar<T>는 TAutoConsoleVariable<T>, BootCVar<T>는 ECVF_ReadOnly + 시작 시 값이다(3절 표).
namespace rocks {

// 정의 문장을 짧게 하려고 핸들 타입을 rocks 바로 아래에도 둔다(4절).
using cvar::BootCVar;
using cvar::CVar;

} // namespace rocks
