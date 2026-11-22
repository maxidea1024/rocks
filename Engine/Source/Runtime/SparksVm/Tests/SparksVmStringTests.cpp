#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include "VmTestSupport.h"

// 값 버퍼·문자열 시험이다(Docs/Design/SparksVm.ko.md 2.2절, LC5b1): LOADSTR·CONCAT·TOSTR·문자열
// 비교, Buf 레지스터의 참조계수(복사·덮어쓰기·frame 해제·fault 해제), host 문자열 인자·반환, ledger
// 0, 메모리 예산·단일 버퍼 상한, 할당 실패.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_vm_string_tests_file {

using namespace rocks::sparks::vm_test_support;

namespace {

// Buf 레지스터 buffers개(창 window개 더)를 쓰고 Buf 하나를 돌려주는 모양이다.
Shape BufferShape(uint16 registers, uint16 window, uint16 parameters) noexcept {
    Shape shape;
    shape.registers[2] = registers;
    shape.frame[2] = static_cast<uint16>(registers + window);
    shape.parameters[2] = parameters;
    shape.returnBank = 2;
    shape.returnSlots = 1;
    return shape;
}

// 반환 문자열(없으면 빈 view)이다.
StringView Returned(const VmExecution& execution) noexcept {
    return execution.ReturnedBuffers().IsEmpty() ? StringView()
                                                 : VmStringOf(execution.ReturnedBuffers()[0]);
}

// Scalar 묶음 하나(count slot)를 TOSTR로 바꿔 돌려주는 함수다.
StringView Format(VmExecution& execution, Assembler& assembler, uint32 display,
                  Span<const uint64> slots, Outcome& outcome) noexcept {
    const uint32 count = static_cast<uint32>(slots.Size());
    Shape shape;
    shape.registers[0] = static_cast<uint16>(count);
    shape.frame[0] = static_cast<uint16>(count);
    shape.parameters[0] = static_cast<uint16>(count);
    shape.registers[2] = 1;
    shape.frame[2] = 1;
    shape.returnBank = 2;
    shape.returnSlots = 1;
    const uint32 function = assembler.Begin("Format", shape);
    assembler.Emit(EncodeABC(Opcode::ToStr, 0, 0, count));
    // enum 표시는 EXTRA 아래 16비트에 enum 타입 참조를 둔다(verifier가 본다).
    const StringView cases[] = {"Left", "Right"};
    const uint32 enumType = display == static_cast<uint32>(ScalarKind::Enum)
                                ? assembler.EnumRef("Side", Span<const StringView>(cases, 2))
                                : 0;
    assembler.Emit(EncodeAx(Opcode::Extra, (display << 16) | enumType));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    if (!Verify(assembler)) {
        return StringView();
    }

    outcome = Run(execution, assembler.Linked(), function, slots);
    return Returned(execution);
}

} // namespace

TEST(SparksVmString_LoadConcatAndLedger) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    // b0 = "ab" ++ "" ++ "cd"(창 b1..b3), 덮어쓴 이전 값은 놓인다.
    assembler.Begin("Join", BufferShape(1, 3, 0));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 0, assembler.String("old")));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 1, assembler.String("ab")));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 2, assembler.String("")));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 3, assembler.String("cd")));
    assembler.Emit(EncodeABC(Opcode::Concat, 0, 1, 3));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    const Outcome joined = Run(execution, assembler.Linked(), 0, Span<const uint64>());
    CHECK(joined.outcome == VmOutcome::Returned);
    CHECK(Returned(execution) == "abcd");
    CHECK(execution.ReturnedSlots().IsEmpty());
    CHECK(execution.LiveBuffers() == 1); // 반환 값만 살아 있다
    execution.ReleaseReturned();
    CHECK(execution.LiveBuffers() == 0);
    CHECK(execution.LiveBytes() == 0);
}

TEST(SparksVmString_ComparisonsAndHostStrings) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    // (b0, b1) → s0..s5 = EQ.STR, NE.STR, LT.STR, LE.STR, EQ.B, NE.B를 2진 자리로 합친 값.
    Shape shape;
    shape.registers[0] = 2;
    shape.frame[0] = 2;
    shape.registers[2] = 2;
    shape.frame[2] = 2;
    shape.parameters[2] = 2;
    shape.returnSlots = 1;
    assembler.Begin("Compare", shape);
    const Opcode ops[] = {Opcode::EqStr, Opcode::NeStr, Opcode::LtStr,
                          Opcode::LeStr, Opcode::EqB,   Opcode::NeB};
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 0, 0));
    for (const Opcode op : ops) {
        assembler.Emit(EncodeABC(op, 1, 0, 1));
        assembler.Emit(EncodeABC(Opcode::AddI64, 0, 0, 0)); // s0 *= 2
        assembler.Emit(EncodeABC(Opcode::AddI64, 0, 0, 1)); // s0 += 비교 결과
    }

    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    auto compare = [&](VmBuffer* left, VmBuffer* right) noexcept {
        VmBuffer* const arguments[] = {left, right};
        auto called = execution.Call(assembler.Linked(), 0, Span<const uint64>(), {},
                                     Span<VmBuffer* const>(arguments, 2));
        CHECK(called);
        return called && !execution.ReturnedSlots().IsEmpty() ? execution.ReturnedSlots()[0]
                                                              : NumericLimits<uint64>::Max();
    };
    auto apple = execution.NewString("apple");
    auto apricot = execution.NewString("apricot");
    auto same = execution.NewString("apple");
    auto empty = execution.NewString("");
    REQUIRE(apple && apricot && same && empty);
    // 비트: EQ NE LT LE EQB NEB
    CHECK(compare(apple.Value(), apricot.Value()) == 0b011101);
    CHECK(compare(apricot.Value(), apple.Value()) == 0b010001);
    CHECK(compare(apple.Value(), same.Value()) == 0b100110);
    CHECK(compare(empty.Value(), apple.Value()) == 0b011101);
    // null(`string?`)은 null끼리만 같고 어떤 문자열보다 작다.
    CHECK(compare(nullptr, nullptr) == 0b100110);
    CHECK(compare(nullptr, empty.Value()) == 0b011101);
    CHECK(compare(empty.Value(), nullptr) == 0b010001);
    // host 버퍼는 호출이 빌린다: 호출 뒤에도 host 참조 하나씩이다.
    CHECK(apple.Value()->references == 1);
    CHECK(execution.LiveBuffers() == 4);
    VmBuffer* const owned[] = {apple.Value(), apricot.Value(), same.Value(), empty.Value()};
    for (VmBuffer* buffer : owned) {
        execution.Release(buffer);
    }

    CHECK(execution.LiveBuffers() == 0);
    CHECK(execution.LiveBytes() == 0);
}

TEST(SparksVmString_ReturnedParameterKeepsReferences) {
    // 매개변수를 그대로 돌려주면 같은 버퍼에 참조가 하나 더(반환) 생긴다. MOV.B 복사와 덮어쓰기도
    // 참조계수를 맞춘다.
    SystemAllocator allocator;
    Assembler assembler(allocator);
    assembler.Begin("Echo", BufferShape(3, 0, 1));
    assembler.Emit(EncodeABC(Opcode::MovB, 1, 0, 0));
    assembler.Emit(EncodeABC(Opcode::MovB, 2, 1, 0));
    assembler.Emit(EncodeABC(Opcode::LoadNullB, 1, 0, 0));
    assembler.Emit(EncodeABC(Opcode::MovB, 2, 2, 0)); // 자기 자신 복사
    assembler.Emit(EncodeABC(Opcode::Ret, 2, 1, 2));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    auto text = execution.NewString("hello");
    REQUIRE(text);
    VmBuffer* const arguments[] = {text.Value()};
    auto called = execution.Call(assembler.Linked(), 0, Span<const uint64>(), {},
                                 Span<VmBuffer* const>(arguments, 1));
    REQUIRE(called);
    CHECK(execution.ReturnedBuffers()[0] == text.Value());
    CHECK(text.Value()->references == 2);
    execution.Release(text.Value());
    CHECK(execution.LiveBuffers() == 1);
    execution.ReleaseReturned();
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmString_BuffersAreReleasedOnEveryExit) {
    // fault·ENDEXEC·중첩 frame의 반환에서 frame의 Buf 레지스터를 모두 놓는다(ledger 0).
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Shape shape = BufferShape(2, 0, 0);
    shape.registers[0] = 1;
    shape.frame[0] = 1;
    shape.returnBank = 0;
    shape.returnSlots = 0;
    // 0: 문자열 둘을 만든 뒤 FAULT 1.
    assembler.Begin("Fault", shape);
    assembler.Emit(EncodeABx(Opcode::LoadStr, 0, assembler.String("a")));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 1, assembler.String("b")));
    assembler.Emit(EncodeAx(Opcode::Fault, 1));
    assembler.End();
    // 1: 문자열을 만든 뒤 ENDEXEC.
    assembler.Begin("End", shape);
    assembler.Emit(EncodeABx(Opcode::LoadStr, 0, assembler.String("a")));
    assembler.Emit(EncodeABC(Opcode::EndExecS, 0, 1, 0));
    assembler.End();
    // 2: 문자열을 돌려주는 callee, 3: 그것을 두 번 불러 결과를 버리고 값 없이 반환.
    const uint32 make = assembler.Begin("Make", BufferShape(1, 0, 0));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 0, assembler.String("made")));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    Shape outer;
    outer.registers[2] = 1;
    outer.frame[2] = 2;
    const uint32 drop = assembler.Begin("Drop", outer);
    const uint32 ref = assembler.Ref(BytecodeRefKind::Function, "Make", make);
    assembler.Emit(EncodeABx(Opcode::Call, 0, ref));
    assembler.Emit(EncodeABC(Opcode::MovB, 0, 1, 0));
    assembler.Emit(EncodeABx(Opcode::Call, 0, ref));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    CHECK(Faults(Run(execution, assembler.Linked(), 0, Span<const uint64>()),
                 VmFaultKind::Unreachable));
    CHECK(execution.LiveBuffers() == 0);
    CHECK(Run(execution, assembler.Linked(), 1, Span<const uint64>()).outcome == VmOutcome::Ended);
    CHECK(execution.LiveBuffers() == 0);
    CHECK(Run(execution, assembler.Linked(), drop, Span<const uint64>()).outcome ==
          VmOutcome::Returned);
    CHECK(execution.LiveBuffers() == 0);
    CHECK(execution.LiveBytes() == 0);
}

TEST(SparksVmString_ToStrNotation) {
    // 보간 표기(SL 3.2절): 실수는 최단 왕복 10진이고 정수값이면 `.0`, 수학 타입은 성분 괄호.
    SystemAllocator allocator;
    Assembler assembler(allocator);
    VmExecution execution(allocator);
    Outcome outcome;
    auto one = [&](ScalarKind kind, uint64 value) noexcept {
        const uint64 slots[] = {value};
        const StringView text = Format(execution, assembler, static_cast<uint32>(kind),
                                       Span<const uint64>(slots, 1), outcome);
        CHECK(outcome.outcome == VmOutcome::Returned);
        return text;
    };
    CHECK(one(ScalarKind::Bool, 1) == "true");
    CHECK(one(ScalarKind::Bool, 0) == "false");
    CHECK(one(ScalarKind::I8, Signed(-5)) == "-5");
    CHECK(one(ScalarKind::U64, NumericLimits<uint64>::Max()) == "18446744073709551615");
    CHECK(one(ScalarKind::F64, Bits(1.0)) == "1.0");
    CHECK(one(ScalarKind::F64, Bits(-0.0)) == "-0.0");
    CHECK(one(ScalarKind::F64, Bits(0.1)) == "0.1");
    CHECK(one(ScalarKind::F64, Bits(2.5)) == "2.5");
    CHECK(one(ScalarKind::F64, Bits(1e100)) == "1e+100");
    CHECK(one(ScalarKind::F32, Bits(static_cast<double>(0.1f))) == "0.1");
    CHECK(one(ScalarKind::F32, Bits(16777216.0)) == "16777216.0");
    const uint64 vector[] = {Bits(1.0), Bits(-2.5), Bits(0.0)};
    CHECK(Format(execution, assembler, static_cast<uint32>(DisplayKind::Vec3),
                 Span<const uint64>(vector, 3), outcome) == "(1.0, -2.5, 0.0)");
    const uint64 transform[] = {Bits(1.0), Bits(2.0), Bits(3.0), Bits(0.0), Bits(0.0),
                                Bits(0.0), Bits(1.0), Bits(1.0), Bits(1.0), Bits(1.0)};
    CHECK(Format(execution, assembler, static_cast<uint32>(DisplayKind::Transform),
                 Span<const uint64>(transform, 10),
                 outcome) == "((1.0, 2.0, 3.0), (0.0, 0.0, 0.0, 1.0), (1.0, 1.0, 1.0))");
    // enum은 TYPE 구역의 case 이름이다(LC5b3). 이름·Tag(LC5d)는 아직 자리표시다.
    const uint64 slot[] = {1};
    CHECK(Format(execution, assembler, static_cast<uint32>(ScalarKind::Enum),
                 Span<const uint64>(slot, 1), outcome) == "Right");
    Format(execution, assembler, static_cast<uint32>(ScalarKind::Name), Span<const uint64>(slot, 1),
           outcome);
    CHECK(Faults(outcome, VmFaultKind::ContractViolation));
    execution.ReleaseReturned();
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmString_MemoryBudgetAndLimits) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    // 문자열 둘을 이어 붙인다: 예산이 모자라면 그 할당 명령 자리의 BudgetExceeded다.
    assembler.Begin("Grow", BufferShape(1, 2, 0));
    char text[100] = {};
    for (char& c : text) {
        c = 'x';
    }

    const uint32 piece = assembler.String(StringView(text, sizeof(text)));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 1, piece));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 2, piece));
    assembler.Emit(EncodeABC(Opcode::Concat, 0, 1, 2));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    VmLimits limits;
    const uint64 one = VmBufferHeaderBytes + 104; // 100 byte를 8의 배수로
    limits.memoryBytes = one * 2;                 // 조각 둘까지만
    auto concat = execution.Call(assembler.Linked(), 0, Span<const uint64>(), {}, {}, limits);
    REQUIRE(concat);
    CHECK(concat.Value().fault.kind == VmFaultKind::BudgetExceeded);
    CHECK(concat.Value().fault.address == 2);
    CHECK(execution.LiveBuffers() == 0);
    limits.memoryBytes = one - 1; // 첫 조각부터
    auto first = execution.Call(assembler.Linked(), 0, Span<const uint64>(), {}, {}, limits);
    REQUIRE(first);
    CHECK(first.Value().fault.address == 0);
    limits.memoryBytes = DefaultMemoryBudget;
    CHECK(Run(execution, assembler.Linked(), 0, Span<const uint64>(), Span<const uint64>(), limits)
              .outcome == VmOutcome::Returned);
    CHECK(Returned(execution).ByteCount() == 200);
    execution.ReleaseReturned();

    // 단일 버퍼 상한: host 문자열도 16,777,216 byte를 넘을 수 없다.
    Array<char> huge(allocator);
    REQUIRE(huge.TryResize(MaxBufferElements + 1));
    CHECK(execution.NewString(StringView(huge.Data(), huge.Size())).Error() ==
          ErrorCode::CapacityExceeded);
    auto largest = execution.NewString(StringView(huge.Data(), MaxBufferElements));
    REQUIRE(largest);
    execution.Release(largest.Value());
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmString_AllocationFailureEverywhere) {
    // 문자열을 만들고 잇고 표기하는 함수의 모든 할당 실패가 OutOfMemory로 돌아오고 새지 않는다.
    SystemAllocator backing;
    Assembler assembler(backing);
    Shape shape = BufferShape(1, 2, 1);
    shape.registers[0] = 1;
    shape.frame[0] = 1;
    shape.parameters[0] = 1;
    assembler.Begin("Mix", shape);
    assembler.Emit(EncodeABC(Opcode::ToStr, 1, 0, 1));
    assembler.Emit(EncodeAx(Opcode::Extra, static_cast<uint32>(ScalarKind::I64) << 16));
    assembler.Emit(EncodeABC(Opcode::MovB, 2, 0, 0));
    assembler.Emit(EncodeABC(Opcode::Concat, 0, 1, 2));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    REQUIRE(Verify(assembler));
    uint64 attempts = 0;
    uint64 failures = 0;
    const uint64 number[] = {42};
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        const uint64 before = backing.Stats().liveAllocations;
        {
            VmExecution execution(allocator);
            auto suffix = execution.NewString("!");
            REQUIRE(suffix);
            if (failAt != 0) {
                allocator.FailOnAttempt(allocator.Attempts() + failAt);
            }

            VmBuffer* const arguments[] = {suffix.Value()};
            auto called = execution.Call(assembler.Linked(), 0, Span<const uint64>(number, 1), {},
                                         Span<VmBuffer* const>(arguments, 1));
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                REQUIRE(called);
                CHECK(Returned(execution) == "42!");
                attempts = used;
            } else if (!called) {
                CHECK(called.Error() == ErrorCode::OutOfMemory);
                ++failures;
            }

            execution.Release(suffix.Value());
            execution.ReleaseReturned();
            CHECK(execution.LiveBuffers() == 0);
        }

        CHECK(backing.Stats().liveAllocations == before);
    }

    CHECK(failures > 0);
}

} // namespace sparks_vm_string_tests_file
