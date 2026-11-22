#include <SparksVm/Bytecode.h>

// 참조 표 종류의 이름이다(덤프).

namespace rocks::sparks {

const char* BytecodeHookKindName(BytecodeHookKind kind) noexcept {
    switch (kind) {
    case BytecodeHookKind::Awake:
        return "Awake";
    case BytecodeHookKind::Enable:
        return "Enable";
    case BytecodeHookKind::Start:
        return "Start";
    case BytecodeHookKind::Tick:
        return "Tick";
    case BytecodeHookKind::FixedTick:
        return "FixedTick";
    case BytecodeHookKind::LateTick:
        return "LateTick";
    case BytecodeHookKind::Disable:
        return "Disable";
    case BytecodeHookKind::Destroy:
        return "Destroy";
    case BytecodeHookKind::TriggerEnter:
        return "TriggerEnter";
    case BytecodeHookKind::TriggerExit:
        return "TriggerExit";
    case BytecodeHookKind::CollisionEnter:
        return "CollisionEnter";
    case BytecodeHookKind::CollisionExit:
        return "CollisionExit";
    case BytecodeHookKind::Validate:
        return "Validate";
    case BytecodeHookKind::Count:
        break;
    }

    return "?";
}

const char* BytecodeRefKindName(BytecodeRefKind kind) noexcept {
    switch (kind) {
    case BytecodeRefKind::Function:
        return "function";
    case BytecodeRefKind::ImportedFunction:
        return "import";
    case BytecodeRefKind::VirtualFunction:
        return "virtual";
    case BytecodeRefKind::NativeFunction:
        return "native";
    case BytecodeRefKind::LanguageFunction:
        return "language";
    case BytecodeRefKind::RpcFunction:
        return "rpc";
    case BytecodeRefKind::Field:
        return "field";
    case BytecodeRefKind::StructField:
        return "struct-field";
    case BytecodeRefKind::Type:
        return "type";
    case BytecodeRefKind::Event:
        return "event";
    case BytecodeRefKind::Context:
        return "context";
    case BytecodeRefKind::Literal:
        return "literal";
    }

    return "?";
}

} // namespace rocks::sparks
