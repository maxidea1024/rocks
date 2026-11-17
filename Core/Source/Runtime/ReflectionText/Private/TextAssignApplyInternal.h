#pragma once

#include "TextAssignInternal.h"

// 대입 하나의 적용(리플렉션 B9i3c1–c3)을 기능별 파일로 나눈 내부 선언이다: 해석·준비 단위 찾기와
// 공용 도우미(TextAssignApply.cpp), 값·컨테이너 단위의 적용 문맥(TextAssignValueUnit.cpp), 단일
// 강한 참조 단위(TextAssignReferenceUnit.cpp), 참조 struct 분해(TextAssignReferenceStruct.cpp),
// StructRefArray 원소 지우기·비우기(TextAssignElements.cpp — 리플렉션 B9i3c4).
namespace rocks::reflection_text::detail {

// 두 단일 값이 같은 값인지다(실수는 bit 단위).
bool SameValue(const FieldValue& left, const FieldValue& right) noexcept;

// 호출 오류(대입 내용과 무관한 실패)인지다.
bool IsCallError(ErrorCode code) noexcept;

// 새로 가리키는 Hard·Weak 대상을 고정한다. 살아 있지 않으면 진단을 남기고 false다.
Result<bool> PinReferences(AssignmentState& state, const TextValue& value, TextSource source,
                           StringView pathText) noexcept;

// 대입 값을 out에 읽는다: JSON import가 미리 읽은 값(state.preparsed)이 있으면 그것을 옮기고,
// 아니면 값 텍스트를 읽는다(ParseTextValue). false면 진단을 남겼다.
Result<bool> ReadAssignmentValue(AssignmentState& state, const TextSlot& slot,
                                 const TextAssignment& assignment, TextSource source,
                                 TextValue& out) noexcept;

// Map 키·Set 원소 값의 key wire를 wire 뒤에 붙인다.
Result<void> AppendKeyWire(AssignmentState& state, const FieldValue& value,
                           Array<uint8>& wire) noexcept;

// 멤버마다 나누는 적용이 이번 호출의 진단만 보도록 묶음의 실패 표시를 잠시 내려 두고 나갈 때
// 합친다(앞 대입의 진단이 뒤 대입의 진단 모으기를 멈추지 않게 한다).
class FreshFailure {
public:
    explicit FreshFailure(AssignmentState& state) noexcept
        : state_(state), previous_(state.failed) {
        state_.failed = false;
    }

    ~FreshFailure() {
        state_.failed = state_.failed || previous_;
    }

    FreshFailure(const FreshFailure&) = delete;
    FreshFailure& operator=(const FreshFailure&) = delete;

private:
    AssignmentState& state_;
    bool previous_;
};

// 대입 하나의 적용 문맥이다. 값 단위는 상자(start = PathTarget::Value(box), relative는 상자 기준
// 경로), Registry 컨테이너 단위는 준비 사본을 건 대상(start = Root().WithReferences, relative는
// 대상 기준 경로 — box 없음)이다.
struct Apply {
    AssignmentState& state;
    PathTarget start;
    bool& dirty;
    DetachedValue* box;
    const TextAssignment& assignment;
    const TextSlot& slot;
    // 대상 기준 끝 경로(override 경로의 원본)와 start 기준 경로.
    PropertyPath path;
    PropertyPath relative;
    TextSource source;
    PropertyPathFailure failure{};

    ObjectRegistry& Registry() const noexcept {
        return state.target.Registry();
    }

    const ObjectAccessContext& Access() const noexcept {
        return state.target.Access();
    }

    PathTarget Start() const noexcept {
        return start;
    }

    Result<void> Diagnostic(TextDiagnosticCode code, StringView message) noexcept {
        return Diagnose(state, code, source, assignment.path, message);
    }

    Result<void> Failed(ErrorCode code) noexcept {
        return ReportPathFailure(state, code, failure, source, assignment.path);
    }

    // 사본을 바꾼 대입을 기록한다.
    Result<void> Changed() noexcept {
        dirty = true;
        return AddOverride(state, path);
    }

    Result<void> Run() noexcept;
    Result<void> RunSet(TextValue& value) noexcept;
    Result<void> ResetStruct() noexcept;
    // 상자 안 자리 at(필드·멤버 하나)을 떼어 기본값으로 바꿔 되돌린다.
    Result<void> ResetAt(PropertyPath at) noexcept;
    // 정적 배열 struct 원소의 멤버마다 기본값으로 되돌린다(리플렉션 B9i3c3).
    Result<void> ResetMembers() noexcept;
    Result<void> ReplaceContents(const TextValue& value) noexcept;
};

// 단일 강한 참조 위치 path를 target으로 바꾸는 간선 변경 단위를 둔다(TextAssignReferenceUnit.cpp).
Result<void> AssignReference(AssignmentState& state, PropertyPath path, ObjectHandle target,
                             bool readOnly, bool record, const TextValue* pinValue,
                             TextSource source, StringView pathText) noexcept;

// 단일 강한 참조 위치의 대입이다(TextAssignReferenceUnit.cpp).
Result<void> ApplyReference(AssignmentState& state, const TextAssignment& assignment,
                            const TextSlot& slot, PropertyPath path, TextSource source) noexcept;

// 참조를 담은 struct 자리 path를 기본값으로 되돌린다(TextAssignReferenceStruct.cpp).
Result<void> ResetReferenceStruct(AssignmentState& state, PropertyPath path, StructView view,
                                  TextSource source, StringView pathText) noexcept;

// 준비 단위가 없는 자리의 대입이다(TextAssignReferenceStruct.cpp).
Result<void> ApplyReferenceStruct(AssignmentState& state, const TextAssignment& assignment,
                                  const TextSlot& slot, PropertyPath path,
                                  TextSource source) noexcept;

// 슬롯이 StructRefArray 필드·멤버(통째 또는 원소)인지다.
bool IsStructRefArraySlot(const TextSlot& slot) noexcept;

// path 안 StructRefArray 원소 index(지금 index — 앞 대입이 지운 원소를 뺀 자리)를 묶을 때 index로
// 옮겨 steps(MaxPropertyPathSteps칸)에 담는다. 그 배열의 원소 지우기 단위가 없으면 그대로다. 지금
// 원소 수 밖이면 진단 OutOfRange를 남기고 false다.
Result<bool> MapElementIndices(AssignmentState& state, PropertyPath path, PathStep* steps,
                               TextSource source, StringView pathText) noexcept;

// StructRefArray 필드·멤버 통째(비우기 `items=`)와 원소(지우기 `--erase items[i]`)의 대입이다
// (TextAssignElements.cpp). path는 묶을 때 index로 옮긴 경로다.
Result<void> ApplyElements(AssignmentState& state, const TextAssignment& assignment,
                           const TextSlot& slot, PropertyPath path, TextSource source) noexcept;

// 배열 경로 arrayPath(선택자 없음)의 StructRefArray를 비운다(참조 struct `{}`의 멤버 — 이미
// 비었으면 무동작, ReadOnly면 진단).
Result<void> ClearElements(AssignmentState& state, PropertyPath arrayPath, bool readOnly,
                           TextSource source, StringView pathText) noexcept;

} // namespace rocks::reflection_text::detail
