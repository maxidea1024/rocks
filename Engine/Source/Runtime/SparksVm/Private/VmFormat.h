#pragma once

#include <Core/Text/StringBuilder.h>
#include <SparksVm/Vm.h>

// TOSTR의 표기다(Docs/Design/SparksVm.ko.md 2.2절 — SL 3.2절 보간): bool, 정수 10진, 실수 최단 왕복
// 10진(정수값이면 `.0`), 수학 타입은 성분 괄호.

namespace rocks::sparks::detail {

// Scalar 묶음 slots(count개)를 표시 종류 display(ScalarKind 또는 DisplayKind)로 out에 붙인다. 아직
// 쓰지 못하는 종류(enum — LC5b3, 이름·Tag·AssetId — LC5d)는 ContractViolation을 돌려주고 out을
// 바꾸지 않는다. 할당 실패는 out의 실패 상태다.
VmFaultKind VmFormatScalars(StringBuilderBase& out, uint32 display, const uint64* slots,
                            uint32 count) noexcept;

// 실수 하나를 보간 표기로 붙인다(single이면 float 값의 최단 표기).
void VmAppendFloat(StringBuilderBase& out, double value, bool single) noexcept;

} // namespace rocks::sparks::detail
