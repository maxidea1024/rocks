#pragma once

#include <Meta/TypeSchema.h>
#include <Object/ObjectHandle.h>

namespace rocks {

enum class ObjectPhase : uint8 {
    Reserved,
    Alive,
    DestroyRequested,
    Retired,
    Reclaiming
};

// 한 번의 캡처 시점 상태를 복사한 진단 레코드다. 객체 수명을 연장하지 않는다.
// Reclaiming 중에도 type은 유효한 식별자지만, 해당 객체 접근을 허용하지 않는다.
struct ObjectDiagnosticRecord {
    ObjectHandle handle;
    ContextId context;
    TypeId type;
    ObjectPhase phase = ObjectPhase::Reserved;
    uint32 roots = 0;
    uint32 borrows = 0;
    uint32 backendHolds = 0;
    // encode 고정(EncodePin) 계수다.
    uint32 encodePins = 0;
};

} // namespace rocks
