#include <SparksVm/Bytecode.h>

// 참조 표 종류의 이름이다(덤프).

namespace rocks::sparks {

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
