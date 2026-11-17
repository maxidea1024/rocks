#include <RenderBase/RenderTransfer.h>

namespace rocks::render::detail {

bool SchemaHasNoObjectReferences(const TypeSchema& schema) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (field.kind == FieldKind::ObjectReference) {
            return false;
        }

        // 컨테이너면 원소·값 형식, 아니면 값 형식이다(Array<WeakRef>·OrderedMap<K, WeakRef> 포함).
        const ValueKind target = FieldTargetType(field).kind;
        if (field.value.kind == ValueKind::ObjectReference ||
            field.value.kind == ValueKind::WeakObjectReference ||
            target == ValueKind::ObjectReference || target == ValueKind::WeakObjectReference) {
            return false;
        }
    }

    return true;
}

} // namespace rocks::render::detail
