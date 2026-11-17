#pragma once

#include "TextInternal.h"

#include <ReflectionText/TextJson.h>

// JSON export·import(리플렉션 B9i4 — ReflectionText/TextJson.h)의 공용 도우미다.
namespace rocks::reflection_text::detail {

// JSON을 스키마와 함께 따라가며 만드는 대상 기준 경로다(필드 단계, 원소 index, Map·Set key). key
// wire는 key마다 따로 담아 경로 view가 다음 바꾸기까지 유효하다. 깊이는 MaxPropertyPathSteps까지다.
class JsonPathBuilder {
public:
    explicit JsonPathBuilder(Allocator& allocator) noexcept : keys_(allocator), wires_(allocator) {
    }

    JsonPathBuilder(const JsonPathBuilder&) = delete;
    JsonPathBuilder& operator=(const JsonPathBuilder&) = delete;

    usize Depth() const noexcept {
        return count_;
    }

    // 필드 단계를 더한다(선택자 없음). 이미 MaxPropertyPathSteps 단계면 false다(경로는 그대로).
    bool Push(Guid fieldId) noexcept {
        if (count_ >= MaxPropertyPathSteps) {
            return false;
        }

        steps_[count_] = PathStep{};
        steps_[count_].fieldId = fieldId;
        ++count_;
        return true;
    }

    // 끝 단계를 뗀다(그 단계의 key도).
    void Pop() noexcept {
        ClearSelector();
        --count_;
    }

    // 끝 단계의 선택자를 원소 index로 바꾼다.
    void SetIndex(uint32 index) noexcept {
        ClearSelector();
        steps_[count_ - 1].selector = PathSelector::Index;
        steps_[count_ - 1].index = index;
    }

    // 끝 단계의 선택자를 key(Map 키·Set 원소 값)로 바꾼다. 오류: wire 할당·Name 조회의 오류(경로는
    // 선택자 없는 끝 단계가 된다).
    Result<void> SetKey(NamePool* names, Allocator& allocator, const FieldValue& key) noexcept;

    // 끝 단계의 선택자를 지운다.
    void ClearSelector() noexcept {
        PathStep& last = steps_[count_ - 1];
        if (last.selector == PathSelector::Key) {
            // 끝 key는 늘 있으므로 실패하지 않는다.
            [[maybe_unused]] auto key = keys_.TryPopBack();
            [[maybe_unused]] auto wire = wires_.TryPopBack();
        }

        last.selector = PathSelector::None;
        last.index = 0;
    }

    PropertyPath View() const noexcept {
        return PropertyPath{Span<const PathStep>(steps_, count_), keys_.View()};
    }

private:
    PathStep steps_[MaxPropertyPathSteps];
    usize count_ = 0;
    Array<PathKey> keys_;
    Array<Array<uint8>> wires_;
};

// 필드 하나(scope의 index번째)의 자리다: 부모 단계 플래그 합 parentFlags에 필드 플래그를 더한다
// (DescribeTextPath와 같은 자리 — part는 Field).
inline TextSlot FieldSlot(const SchemaScope& scope, usize index, uint32 parentFlags) noexcept {
    TextSlot slot;
    slot.field = &scope.Schema().fields[index];
    slot.part = TextSlotPart::Field;
    slot.valueEnum = scope.FieldEnum(index);
    slot.keyEnum = scope.FieldKeyEnum(index);
    slot.structView = scope.FieldStruct(index);
    slot.flags = parentFlags | slot.field->flags;
    return slot;
}

// slot의 part만 바꾼 자리다(원소·Map 값).
inline TextSlot PartSlot(const TextSlot& slot, TextSlotPart part) noexcept {
    TextSlot copy = slot;
    copy.part = part;
    return copy;
}

// JSON 표기가 없는 필드 종류인지다(접근기 없는 Opaque — InstancedStruct·Text는 그 종류를 더하는
// B9j·B9l이 표기를 더한다).
inline bool HasNoJsonForm(const FieldSchema& field) noexcept {
    return field.value.kind == ValueKind::Opaque;
}

// 비유한 실수의 JSON 문자열 표기인지다("nan"·"inf"·"-inf" — 명령행 표기와 같다).
inline bool IsNonFiniteText(StringView text) noexcept {
    return text == "nan" || text == "inf" || text == "-inf";
}

} // namespace rocks::reflection_text::detail
