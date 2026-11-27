#pragma once

#include <Object/RuntimeTypeOps.h>

// 클래스 등록의 리플렉션 함수·이벤트 필드 서술 검사·블록 배치·복사다(Docs/Design/
// FunctionReflection.ko.md FD10, EventReflection.ko.md ED15 — 정의는 TypeRegistryFunctions.cpp).
namespace rocks::detail {

// 레코드 블록 안 함수 배열의 배치다. ValidateFunctionList가 수·문자열 크기를 세고, Place가 주어진
// 위치 뒤에 네 배열(FunctionSchema·ParameterSchema·RuntimeFunctionOps·RuntimeParameterOps)의
// 위치를 정해 끝 위치를 돌려준다. 이름 byte(textBytes)는 레코드의 문자열 영역에 더한다.
struct FunctionBlockLayout {
    usize functions = 0;
    usize parameters = 0;
    usize textBytes = 0;
    usize functionOffset = 0;
    usize parameterOffset = 0;
    usize functionOpsOffset = 0;
    usize parameterOpsOffset = 0;

    usize Place(usize offset) noexcept;
};

// 함수 서술을 검사한다(같은 타입 안의 ID·실효 이름 유일성 포함 — 겹치면 AlreadyExists, 그 밖의
// 위반은 InvalidArgument, 크기 넘침은 Overflow). 기반 체인과의 겹침은 FunctionsConflictWithBase다.
Result<FunctionBlockLayout> ValidateFunctionList(const TypeSchema& schema,
                                                 const RuntimeTypeOps& ops) noexcept;

// functions의 ID·실효 이름(대소문자 무시)이 기반 타입의 함수와 겹치는지다.
bool FunctionsConflictWithBase(Span<const FunctionSchema> functions,
                               Span<const FunctionSchema> base) noexcept;

// 검사를 마친 함수 서술·연산을 block(layout으로 배치한 레코드 블록)에 깊게 복사하고 이름을 text
// 위치에 이어 쓴다. 복사본의 span을 functions·functionOps에 둔다.
void CopyFunctions(const FunctionBlockLayout& layout, uint8* block, const TypeSchema& schema,
                   const RuntimeTypeOps& ops, uint8*& text, Span<const FunctionSchema>& functions,
                   Span<const RuntimeFunctionOps>& functionOps) noexcept;

// 레코드 블록 안 이벤트 필드 배열의 배치다(ED15). ValidateEventList가 수·문자열 크기를 세고,
// Place가 세 배열(ParameterSchema·RuntimeEventOps·RuntimeParameterOps)의 위치를 정해 끝 위치를
// 돌려준다.
struct EventBlockLayout {
    usize events = 0;
    usize parameters = 0;
    usize textBytes = 0;
    usize parameterOffset = 0;
    usize eventOpsOffset = 0;
    usize parameterOpsOffset = 0;

    usize Place(usize offset) noexcept;
};

// 이벤트 필드(FieldKind::Event)의 인자와 ops.events를 검사한다(필드 자체의 모양은
// ValidateFieldList). 인자 이름 겹침은 AlreadyExists, 그 밖의 위반은 InvalidArgument, 크기 넘침은
// Overflow다.
Result<EventBlockLayout> ValidateEventList(const TypeSchema& schema,
                                           const RuntimeTypeOps& ops) noexcept;

// 두 이벤트 필드의 인자 서술(이름·형식·flag·원소 상한·offset)이 같은지다(상속 필드 대조).
bool SameEventParameters(const FieldSchema& left, const FieldSchema& right) noexcept;

// 검사를 마친 이벤트 인자·연산을 block에 깊게 복사한다. fields는 레코드에 복사한 필드 배열(schema와
// 같은 순서)이고 이벤트 필드의 eventParameters를 복사본으로 바꾼다. 연산 복사본의 span을
// eventOps에 둔다.
void CopyEvents(const EventBlockLayout& layout, uint8* block, const TypeSchema& schema,
                const RuntimeTypeOps& ops, uint8*& text, FieldSchema* fields,
                Span<const RuntimeEventOps>& eventOps) noexcept;

} // namespace rocks::detail
