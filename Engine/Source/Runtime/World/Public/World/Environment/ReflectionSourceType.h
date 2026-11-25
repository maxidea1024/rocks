#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

namespace rocks {

// 반사 캡처의 원천이다(UE EReflectionSourceType — Docs/Design/EngineRuntime 27.6절).
ROCKS_ENUM()
enum class ReflectionSourceType : uint8 {
    CapturedScene,
    // 지정한 cubemap(에셋 참조 — World 위 모듈에서)을 쓴다.
    SpecifiedCubemap
};

} // namespace rocks
