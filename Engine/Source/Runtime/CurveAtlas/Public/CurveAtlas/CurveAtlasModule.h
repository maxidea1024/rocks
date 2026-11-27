#pragma once

#include <Core/Result.h>
#include <Object/ModuleRegistration.h>

namespace rocks {

// CurveAtlas 모듈의 리플렉션 타입(ColorCurveAtlas — Docs/Design/CurveAssets 4장)을 등록한다. 의존
// 모듈(CurveAssets·Texture)의 타입도 생성 등록 코드가 먼저 등록한다. 이미 등록되어 있으면 아무것도
// 하지 않는다. 실패: ModuleRegistration의 오류.
//
// 사용 예:
//   auto registered = RegisterCurveAtlasTypes(registration);
//   if (!registered) { return registered; }
ROCKS_CURVEATLAS_API Result<void>
RegisterCurveAtlasTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
