#include <RenderBase/RenderTransfer.h>

namespace rocks::render::detail {

bool SchemaHasNoObjectReferences(const TypeSchema& schema) noexcept {
    for (const FieldSchema& field : schema.fields) {
        // 이벤트 필드는 강한 참조가 아니지만 구독(대상 객체·함수)과 발행자를 붙잡는다 — 렌더 lane이
        // 게임 객체를 만지게 되므로 거부한다(Docs/Design/RenderBase 5.2절 "전달 가능한 타입").
        if (field.kind == FieldKind::ObjectReference || IsEventField(field)) {
            return false;
        }

        // 컨테이너면 원소·값 형식, 아니면 값 형식이다(Array<WeakObjectRef>·OrderedMap<K,
        // WeakObjectRef> 포함).
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
