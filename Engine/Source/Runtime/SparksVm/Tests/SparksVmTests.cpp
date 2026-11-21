#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringView.h>
#include <Platform/Memory.h>
#include <SparksVm/BytecodeFile.h>
#include <SparksVm/Opcode.h>
#include <SparksVm/Vm.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include "VmTestSupport.h"

// VM 실행 시험이다(Docs/Design/SparksVm.ko.md, LC5a). 언어가 내지 않거나 corpus로 닿기 어려운 명령
// (정수 DIV, 실수·묶음 비교, 정의하지 않은 CONV, FAULT·ENDEXEC·RETERR, Ref 명령, 미구현 명령의
// 자리표시)과 예산 경계·호출자 오류·할당 실패를 손으로 조립한 bytecode로 본다. 조립한 모듈은 모두
// verifier를 통과해야 한다. 컴파일러 출력의 실행은 SparksTestHost의 `.run` corpus가 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_vm_tests_file {

using namespace rocks::sparks::vm_test_support;

TEST(SparksVm_IntegerDivision) {
    // 정수 DIV는 언어에 연산자가 없다(Math.Div — LC5c). 내림 방식(SL 3.3절), 최솟값 / -1은
    // overflow.
    CHECK(Returns(RunBinary(Opcode::DivI64, 7, Signed(-2)), Signed(-4)));
    CHECK(Returns(RunBinary(Opcode::DivI64, Signed(-7), 2), Signed(-4)));
    CHECK(Returns(RunBinary(Opcode::DivI64, Signed(-8), 2), Signed(-4)));
    CHECK(Returns(RunBinary(Opcode::DivI64, Signed(-7), Signed(-2)), 3));
    CHECK(Returns(RunBinary(Opcode::DivI8, Signed(-128), 3), Signed(-43)));
    CHECK(Faults(RunBinary(Opcode::DivI64, 1, 0), VmFaultKind::DivideByZero));
    CHECK(Faults(RunBinary(Opcode::DivI64, Signed(NumericLimits<int64>::Min()), Signed(-1)),
                 VmFaultKind::IntegerOverflow));
    CHECK(Faults(RunBinary(Opcode::DivI8, Signed(-128), Signed(-1)), VmFaultKind::IntegerOverflow));
    CHECK(Returns(RunBinary(Opcode::DivI8, Signed(-128), 2), Signed(-64)));
    CHECK(Returns(RunBinary(Opcode::DivU32, 0xFFFFFFFFu, 2), 0x7FFFFFFFu));
    CHECK(Faults(RunBinary(Opcode::DivU8, 1, 0), VmFaultKind::DivideByZero));
    CHECK(Returns(RunBinary(Opcode::ModI32, Signed(NumericLimits<int32>::Min()), Signed(-1)), 0));
    CHECK(Returns(RunBinary(Opcode::ModI16, Signed(-7), 3), 2));
    CHECK(Returns(RunBinary(Opcode::ModI16, 7, Signed(-3)), Signed(-2)));
    CHECK(Returns(RunBinary(Opcode::ModI64, Signed(-6), 3), 0));
    CHECK(Returns(RunBinary(Opcode::ModU8, 250, 7), 5));
    CHECK(Faults(RunBinary(Opcode::ModU64, 1, 0), VmFaultKind::DivideByZero));
    // 결과가 범위 밖이면 대상 레지스터를 바꾸지 않는다(fault로 끝나므로 관찰은 steps로만).
    CHECK(RunBinary(Opcode::AddU32, 0xFFFFFFFFu, 1).steps == 1);
}

TEST(SparksVm_FloatComparisons) {
    // 실수 비교: -0.0 == 0.0, NaN은 어떤 비교도 거짓(NE만 참).
    const uint64 nan = Bits(platform::BitCast<double>(uint64(0x7FF8000000000000)));
    CHECK(Returns(RunBinary(Opcode::EqF, Bits(-0.0), Bits(0.0)), 1));
    CHECK(Returns(RunBinary(Opcode::NeF, Bits(-0.0), Bits(0.0)), 0));
    CHECK(Returns(RunBinary(Opcode::EqF, nan, nan), 0));
    CHECK(Returns(RunBinary(Opcode::NeF, nan, nan), 1));
    CHECK(Returns(RunBinary(Opcode::LtF, nan, Bits(1.0)), 0));
    CHECK(Returns(RunBinary(Opcode::LeF, Bits(1.0), Bits(1.0)), 1));
    // 부호 있는·없는 정수 순서.
    CHECK(Returns(RunBinary(Opcode::LtI, Signed(-1), 0), 1));
    CHECK(Returns(RunBinary(Opcode::LtU, Signed(-1), 0), 0));
    CHECK(Returns(RunBinary(Opcode::LeU, 5, 5), 1));
    CHECK(Returns(RunBinary(Opcode::NeI, 5, 5), 0));
}

TEST(SparksVm_TupleComparisons) {
    // s4 = (s0, s1) == (s2, s3): EQN은 비트 비교, EQN.F는 성분 실수 비교.
    auto compare = [](Opcode op, uint64 a0, uint64 a1, uint64 b0, uint64 b1) noexcept -> Outcome {
        SystemAllocator allocator;
        Assembler assembler(allocator);
        Shape shape;
        shape.registers[0] = 5;
        shape.frame[0] = 5;
        shape.parameters[0] = 4;
        shape.returnSlots = 1;
        assembler.Begin("Tuple", shape);
        assembler.Emit(EncodeABC(op, 4, 0, 2));
        assembler.Emit(EncodeAx(Opcode::Extra, 2));
        assembler.Emit(EncodeABC(Opcode::Ret, 4, 1, 0));
        assembler.End();
        if (!Verify(assembler)) {
            return Outcome{};
        }

        VmExecution execution(allocator);
        const uint64 arguments[] = {a0, a1, b0, b1};
        return Run(execution, assembler.Linked(), 0, Span<const uint64>(arguments, 4));
    };
    CHECK(Returns(compare(Opcode::EqNF, 1, Bits(-0.0), 1, Bits(0.0)), 1));
    CHECK(Returns(compare(Opcode::EqN, 1, Bits(-0.0), 1, Bits(0.0)), 0));
    CHECK(Returns(compare(Opcode::NeNF, 0, 0, 1, 0), 1));
    CHECK(Returns(compare(Opcode::NeN, 7, 8, 7, 8), 0));
    CHECK(Returns(compare(Opcode::EqN, 7, 8, 7, 9), 0));
}

TEST(SparksVm_ConversionsOutsideTheLanguage) {
    // 컴파일러가 내지 않는 CONV(실수 → 정수, 숫자가 아닌 종류)는 Unreachable이다.
    auto convert = [](ScalarKind from, ScalarKind to, uint64 value) noexcept -> Outcome {
        SystemAllocator allocator;
        Assembler assembler(allocator);
        Shape shape;
        shape.registers[0] = 2;
        shape.frame[0] = 2;
        shape.parameters[0] = 1;
        shape.returnSlots = 1;
        assembler.Begin("Convert", shape);
        assembler.Emit(
            EncodeABC(Opcode::Conv, 1, 0, (static_cast<uint32>(from) << 4) | uint32(to)));
        assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
        assembler.End();
        if (!Verify(assembler)) {
            return Outcome{};
        }

        VmExecution execution(allocator);
        const uint64 arguments[] = {value};
        return Run(execution, assembler.Linked(), 0, Span<const uint64>(arguments, 1));
    };
    CHECK(Faults(convert(ScalarKind::F64, ScalarKind::I64, Bits(1.5)), VmFaultKind::Unreachable));
    CHECK(Faults(convert(ScalarKind::F32, ScalarKind::U8, Bits(2.0)), VmFaultKind::Unreachable));
    // 숫자가 아닌 종류 사이의 CONV는 verifier가 먼저 거부한다(VM까지 오지 않는다).
    const ScalarKind others[] = {ScalarKind::Bool, ScalarKind::Enum, ScalarKind::Name};
    for (const ScalarKind other : others) {
        SystemAllocator allocator;
        Assembler assembler(allocator);
        Shape shape;
        shape.registers[0] = 2;
        shape.frame[0] = 2;
        shape.parameters[0] = 1;
        shape.returnSlots = 1;
        assembler.Begin("Convert", shape);
        assembler.Emit(EncodeABC(Opcode::Conv, 1, 0, (static_cast<uint32>(other) << 4) | 4));
        assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
        assembler.End();
        CHECK(VerifyBytecode(assembler.module).error == BytecodeLoadError::BadOperand);
    }
    CHECK(Returns(convert(ScalarKind::F32, ScalarKind::F64, Bits(0.5)), Bits(0.5)));
    CHECK(Returns(convert(ScalarKind::U8, ScalarKind::I8, 127), 127));
    CHECK(Faults(convert(ScalarKind::U8, ScalarKind::I8, 128), VmFaultKind::IntegerOverflow));
    CHECK(Returns(convert(ScalarKind::I16, ScalarKind::F32, Signed(-3)), Bits(-3.0)));
}

TEST(SparksVm_FaultsEndsAndResultErrors) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Shape one;
    one.registers[0] = 1;
    one.frame[0] = 1;
    one.parameters[0] = 1;
    // 0: FAULT 1, 1: FAULT 2, 2: FAULT 7(모르는 번호도 Unreachable)
    const uint32 codes[] = {1, 2, 7};
    for (const uint32 code : codes) {
        assembler.Begin("Fault", one);
        assembler.Emit(EncodeAx(Opcode::Fault, code));
        assembler.End();
    }

    // 3: ENDEXEC.S s0 x1 — Result가 아닌 함수의 try 실패.
    assembler.Begin("End", one);
    assembler.Emit(EncodeABC(Opcode::EndExecS, 0, 1, 0));
    assembler.End();
    // 4: RETERR.S s1 x2 — Scalar Result<int>(2 slot) 함수가 다른 Result의 오류를 돌려준다.
    Shape result;
    result.registers[0] = 3;
    result.frame[0] = 3;
    result.parameters[0] = 1;
    result.returnSlots = 2;
    assembler.Begin("Forward", result);
    assembler.Emit(EncodeABC(Opcode::MovS, 1, 0, 0));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 2, 99));
    assembler.Emit(EncodeABC(Opcode::RetErrS, 1, 2, 0));
    assembler.End();
    REQUIRE(assembler.Verified());

    VmExecution execution(allocator);
    const uint64 code[] = {3};
    const Span<const uint64> argument(code, 1);
    CHECK(Faults(Run(execution, assembler.Linked(), 0, argument), VmFaultKind::Unreachable));
    CHECK(Faults(Run(execution, assembler.Linked(), 1, argument), VmFaultKind::HookTryFailed));
    CHECK(Faults(Run(execution, assembler.Linked(), 2, argument), VmFaultKind::Unreachable));
    const Outcome ended = Run(execution, assembler.Linked(), 3, argument);
    CHECK(ended.outcome == VmOutcome::Ended);
    CHECK(ended.value == 3);
    CHECK(ended.steps == 1);
    const Outcome forwarded = Run(execution, assembler.Linked(), 4, argument);
    CHECK(Returns(forwarded, 3));
    REQUIRE(execution.ReturnedSlots().Size() == 2);
    CHECK(execution.ReturnedSlots()[1] == 0); // 값 slot은 0이다(99가 아니다)

    // fault의 자리: 함수 번호, 함수 안 주소, 원문 위치.
    auto called = execution.Call(assembler.Linked(), 1, argument, Span<const uint64>(), {});
    REQUIRE(called);
    CHECK(called.Value().fault.function == 1);
    CHECK(called.Value().fault.address == 0);
    CHECK(called.Value().fault.location.line == 2);
    CHECK(called.Value().fault.location.column == 1);
}

TEST(SparksVm_ComponentPlaceholder) {
    // NEWC(component 생성)는 LC6 전 자리표시다(V5). handle·native는 SparksVmHostTests.cpp가 본다.
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Shape empty;
    empty.registers[1] = 1;
    empty.frame[1] = 1;
    empty.registers[2] = 1;
    empty.frame[2] = 1;
    assembler.Begin("Component", empty);
    assembler.Emit(EncodeABx(Opcode::NewC, 0, assembler.Ref(BytecodeRefKind::Type, "Door")));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    CHECK(Faults(Run(execution, assembler.Linked(), 0, Span<const uint64>()),
                 VmFaultKind::ContractViolation));
}

TEST(SparksVm_BudgetsAndDepth) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    // 0: s0 += 1 다섯 번 뒤 반환(명령 6개).
    Shape counter;
    counter.registers[0] = 2;
    counter.frame[0] = 2;
    counter.parameters[0] = 1;
    counter.returnSlots = 1;
    assembler.Begin("Count", counter);
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 1, 1));
    for (uint32 index = 0; index < 4; ++index) {
        assembler.Emit(EncodeABC(Opcode::AddI64, 0, 0, 1));
    }

    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    // 1: 자기 자신을 부르는 함수(인자 하나를 창에 넘긴다). 창 base는 할당 수 1.
    Shape recurse;
    recurse.registers[0] = 1;
    recurse.frame[0] = 2;
    recurse.parameters[0] = 1;
    recurse.returnSlots = 1;
    const uint32 self = assembler.Begin("Recurse", recurse);
    const uint32 ref = assembler.Ref(BytecodeRefKind::Function, "Recurse", self);
    assembler.Emit(EncodeABC(Opcode::MovS, 1, 0, 0));
    assembler.Emit(EncodeABx(Opcode::Call, 0, ref));
    assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
    assembler.End();
    REQUIRE(assembler.Verified());

    VmExecution execution(allocator);
    const uint64 start[] = {10};
    const Span<const uint64> argument(start, 1);
    VmLimits limits;
    limits.instructions = 6;
    const Outcome exact =
        Run(execution, assembler.Linked(), 0, argument, Span<const uint64>(), limits);
    CHECK(Returns(exact, 14));
    CHECK(exact.steps == 6);
    limits.instructions = 5;
    auto limited =
        execution.Call(assembler.Linked(), 0, argument, Span<const uint64>(), {}, limits);
    REQUIRE(limited);
    CHECK(limited.Value().outcome == VmOutcome::Faulted);
    CHECK(limited.Value().fault.kind == VmFaultKind::BudgetExceeded);
    CHECK(limited.Value().steps == 5);
    CHECK(limited.Value().fault.address == 5); // 막힌 명령(RET)의 자리
    limits.instructions = 0;
    CHECK(Faults(Run(execution, assembler.Linked(), 0, argument, Span<const uint64>(), limits),
                 VmFaultKind::BudgetExceeded));

    // 호출 깊이: frame 3개까지(MOV·CALL 두 번씩 뒤 세 번째 CALL에서 막힌다).
    limits = VmLimits();
    limits.callDepth = 3;
    auto deep = execution.Call(assembler.Linked(), 1, argument, Span<const uint64>(), {}, limits);
    REQUIRE(deep);
    CHECK(deep.Value().fault.kind == VmFaultKind::BudgetExceeded);
    CHECK(deep.Value().steps == 6);
    CHECK(deep.Value().fault.address == 1);
    // 기본 깊이(256)는 끝없는 재귀를 막는다(stack은 깊이만큼 자란다).
    CHECK(Faults(Run(execution, assembler.Linked(), 1, argument), VmFaultKind::BudgetExceeded));
}

TEST(SparksVm_CallerErrors) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Binary(assembler, Opcode::AddI64);
    Shape buffer;
    buffer.registers[2] = 1;
    buffer.frame[2] = 1;
    buffer.parameters[2] = 1;
    assembler.Begin("Buffer", buffer);
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    REQUIRE(assembler.Verified());
    VmExecution execution(allocator);
    const uint64 arguments[] = {1, 2, 3};
    CHECK(execution.Call(assembler.Linked(), 2, Span<const uint64>(arguments, 2), {}, {}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(execution.Call(assembler.Linked(), 0, Span<const uint64>(arguments, 3), {}, {}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(execution
              .Call(assembler.Linked(), 0, Span<const uint64>(arguments, 2),
                    Span<const uint64>(arguments, 1), {})
              .Error() == ErrorCode::InvalidArgument);
    CHECK(execution.Call(assembler.Linked(), 1, Span<const uint64>(), {}, {}).Error() ==
          ErrorCode::InvalidArgument); // Buf 매개변수 하나에 인자 없음
    VmBuffer* const missing[] = {nullptr, nullptr};
    CHECK(execution
              .Call(assembler.Linked(), 1, Span<const uint64>(), {},
                    Span<VmBuffer* const>(missing, 2))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(Returns(Run(execution, assembler.Linked(), 0, Span<const uint64>(arguments, 2)), 3));
}

TEST(SparksVm_CompositeProducts) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    // 0: mat4 곱(s0..15 × s16..31 → s32..47).
    Shape mat;
    mat.registers[0] = 48;
    mat.frame[0] = 48;
    mat.parameters[0] = 32;
    mat.returnSlots = 16;
    assembler.Begin("Matrix", mat);
    assembler.Emit(EncodeABC(Opcode::MulM, 32, 0, 16));
    assembler.Emit(EncodeABC(Opcode::Ret, 32, 16, 0));
    assembler.End();
    // 1: quat 곱(s0..3 × s4..7 → s8..11).
    Shape quat;
    quat.registers[0] = 12;
    quat.frame[0] = 12;
    quat.parameters[0] = 8;
    quat.returnSlots = 4;
    assembler.Begin("Quat", quat);
    assembler.Emit(EncodeABC(Opcode::MulQ, 8, 0, 4));
    assembler.Emit(EncodeABC(Opcode::Ret, 8, 4, 0));
    assembler.End();
    REQUIRE(assembler.Verified());

    VmExecution execution(allocator);
    uint64 matrices[32] = {};
    for (uint32 index = 0; index < 16; ++index) {
        matrices[index] = Bits(index % 5 == 0 ? 1.0 : 0.0); // 단위 행렬
        matrices[16 + index] = Bits(static_cast<double>(index) + 0.5);
    }

    CHECK(Returns(Run(execution, assembler.Linked(), 0, Span<const uint64>(matrices, 32)),
                  matrices[16]));
    for (uint32 index = 0; index < 16; ++index) {
        CHECK(execution.ReturnedSlots()[index] == matrices[16 + index]);
    }

    matrices[16] = Bits(1e300);
    matrices[0] = Bits(1e300);
    CHECK(Faults(Run(execution, assembler.Linked(), 0, Span<const uint64>(matrices, 32)),
                 VmFaultKind::NonFinite));
    const uint64 quats[] = {0, 0, 0, Bits(1.0), Bits(1.0), Bits(2.0), Bits(3.0), Bits(4.0)};
    CHECK(Returns(Run(execution, assembler.Linked(), 1, Span<const uint64>(quats, 8)), Bits(1.0)));
    CHECK(execution.ReturnedSlots()[3] == Bits(4.0));
}

TEST(SparksVm_FaultKindNames) {
    CHECK(StringView(VmFaultKindName(VmFaultKind::IntegerOverflow)) == "IntegerOverflow");
    CHECK(StringView(VmFaultKindName(VmFaultKind::HookTryFailed)) == "HookTryFailed");
    CHECK(StringView(VmFaultKindName(static_cast<VmFaultKind>(200))) == "?");
}

TEST(SparksVm_AllocationFailureEverywhere) {
    // 깊은 재귀(stack·frame 표가 자란다)에서 모든 할당 실패가 OutOfMemory로 돌아오고 새지 않는다.
    SystemAllocator backing;
    Assembler assembler(backing);
    Shape recurse;
    recurse.registers[0] = 1;
    recurse.frame[0] = 2;
    recurse.parameters[0] = 1;
    recurse.returnSlots = 1;
    const uint32 self = assembler.Begin("Recurse", recurse);
    const uint32 ref = assembler.Ref(BytecodeRefKind::Function, "Recurse", self);
    assembler.Emit(EncodeABC(Opcode::MovS, 1, 0, 0));
    assembler.Emit(EncodeABx(Opcode::Call, 0, ref));
    assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
    assembler.End();
    REQUIRE(assembler.Verified());
    uint64 attempts = 0;
    uint64 failures = 0;
    const uint64 start[] = {0};
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            VmExecution execution(allocator);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto called = execution.Call(assembler.Linked(), 0, Span<const uint64>(start, 1),
                                         Span<const uint64>(), {});
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                REQUIRE(called);
                CHECK(called.Value().fault.kind == VmFaultKind::BudgetExceeded);
                attempts = used;
            } else if (!called) {
                CHECK(called.Error() == ErrorCode::OutOfMemory);
                ++failures;
            }
        }
    }

    CHECK(failures > 0);
}

} // namespace sparks_vm_tests_file
