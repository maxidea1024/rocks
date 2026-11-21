#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include "VmTestSupport.h"

// struct·Buf Result 시험이다(Docs/Design/SparksVm.ko.md 2.2절, LC5b3): TYPE 배치의 NEWS·GETSF·
// SETSF와 COW, 원소 버퍼 해제(ledger 0), Buf Result(OK·ERR·TRY.B·RETERR.B·RETERR.S·ENDEXEC.B),
// TYPE 구역 verifier 거부, 할당 실패. 컴파일러 출력의 의미는 `.run` corpus(Run/Structs)가 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_vm_struct_tests_file {

using namespace rocks::sparks::vm_test_support;

namespace {

// Pair { A: int(Scalar 1), B: string(Buf 1), C: vec3(Scalar 3) } — bank 묶음: A s0, C s1..3, B b4.
uint32 PairType(Assembler& assembler) noexcept {
    const StringView names[] = {"A", "B", "C"};
    const uint8 banks[] = {0, 2, 0};
    const uint8 slots[] = {1, 1, 3};
    return assembler.StructRef("Pair", Span<const StringView>(names, 3),
                               Span<const uint8>(banks, 3), Span<const uint8>(slots, 3));
}

// b0 = Pair(), b0.B = "x", b2 = b0(공유), b2.A = 5(COW 복제), b1 = b0.B를 읽은 뒤 b2를 돌려준다.
uint32 BuildPair(Assembler& assembler, uint32 type) noexcept {
    Shape shape;
    shape.registers[0] = 1;
    shape.frame[0] = 1;
    shape.registers[2] = 3;
    shape.frame[2] = 3;
    shape.returnBank = 2;
    shape.returnSlots = 1;
    const uint32 function = assembler.Begin("Pair", shape);
    assembler.Emit(EncodeABx(Opcode::NewS, 0, type));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 1, assembler.String("x")));
    assembler.Emit(EncodeABC(Opcode::SetSFB, 0, 1, 1));
    assembler.Emit(EncodeAx(Opcode::Extra, assembler.FieldRef(type, 1)));
    assembler.Emit(EncodeABC(Opcode::MovB, 2, 0, 0));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 0, 5));
    assembler.Emit(EncodeABC(Opcode::SetSFS, 2, 0, 1));
    assembler.Emit(EncodeAx(Opcode::Extra, assembler.FieldRef(type, 0)));
    assembler.Emit(EncodeABC(Opcode::GetSFB, 1, 0, 1));
    assembler.Emit(EncodeAx(Opcode::Extra, assembler.FieldRef(type, 1)));
    assembler.Emit(EncodeABC(Opcode::Ret, 2, 1, 2));
    assembler.End();
    return function;
}

const uint64* Payload(const VmBuffer* buffer) noexcept {
    return reinterpret_cast<const uint64*>(buffer->Bytes());
}

} // namespace

TEST(SparksVmStruct_FieldsAndCopyOnWrite) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    const uint32 type = PairType(assembler);
    const uint32 function = BuildPair(assembler, type);
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    REQUIRE(Run(execution, assembler.module, function, Span<const uint64>()).outcome ==
            VmOutcome::Returned);
    const VmBuffer* pair = execution.ReturnedBuffers()[0];
    REQUIRE(pair != nullptr && pair->kind == VmBufferKind::Struct);
    CHECK(pair->count == 5);    // s 4 + b 1
    CHECK(pair->capacity == 1); // 끝의 Buf slot 하나
    CHECK(Payload(pair)[0] == 5);
    CHECK(Payload(pair)[1] == 0); // C는 0 slot(NEWS)
    const VmBuffer* text = reinterpret_cast<const VmBuffer*>(static_cast<usize>(Payload(pair)[4]));
    CHECK(VmStringOf(text) == "x");
    CHECK(text->references == 1); // 원본 struct·레지스터는 반환 때 놓였다
    CHECK(execution.LiveBuffers() == 2);
    execution.ReleaseReturned();
    CHECK(execution.LiveBuffers() == 0);
    CHECK(execution.LiveBytes() == 0);
}

TEST(SparksVmStruct_BufferResults) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    // 0: s0 == 0이면 OK("v"), 아니면 ERR(s0 — Error case). Result<string>(Buf) 반환.
    Shape make;
    make.registers[0] = 1;
    make.frame[0] = 1;
    make.parameters[0] = 1;
    make.registers[2] = 2;
    make.frame[2] = 2;
    make.returnBank = 2;
    make.returnSlots = 1;
    const uint32 produce = assembler.Begin("Produce", make);
    assembler.Emit(EncodeAsBx(Opcode::Brt, 0, 4));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 1, assembler.String("v")));
    assembler.Emit(EncodeABC(Opcode::Ok, 0, 1, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, (2u << 8) | 1u));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.Emit(EncodeABC(Opcode::Err, 0, 0, 0));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    // 1: Produce(s0)를 TRY.B로 꺼내 문자열을 돌려주는 Result<string> 함수(실패면 RETERR.B).
    Shape forward = make;
    forward.frame[0] = 2;
    forward.frame[2] = 3;
    const uint32 ref = assembler.Ref(BytecodeRefKind::Function, "Produce", produce);
    const uint32 chain = assembler.Begin("Chain", forward);
    assembler.Emit(EncodeABC(Opcode::MovS, 1, 0, 0));
    assembler.Emit(EncodeABx(Opcode::Call, 0, ref));
    assembler.Emit(EncodeABC(Opcode::TryB, 1, 2, (2u << 5) | 1u));
    assembler.Emit(EncodeAx(Opcode::Extra, 3));
    assembler.Emit(EncodeABC(Opcode::Ok, 0, 1, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, (2u << 8) | 1u));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.Emit(EncodeABC(Opcode::RetErrB, 2, 0, 0));
    assembler.End();
    // 2: 같은 실패를 Scalar Result<int>(2 slot)로 돌려준다(RETERR.B → Scalar 모양).
    Shape scalar = forward;
    scalar.registers[0] = 2;
    scalar.frame[0] = 3;
    scalar.returnBank = 0;
    scalar.returnSlots = 2;
    const uint32 narrow = assembler.Begin("Narrow", scalar);
    assembler.Emit(EncodeABC(Opcode::MovS, 2, 0, 0));
    assembler.Emit(EncodeABx(Opcode::Call, 0, ref));
    assembler.Emit(EncodeABC(Opcode::RetErrB, 2, 0, 0));
    assembler.End();
    // 3: Scalar 오류 코드를 Buf Result 함수가 돌려준다(RETERR.S → Buf 모양).
    Shape widen = make;
    widen.registers[0] = 2;
    widen.frame[0] = 2;
    const uint32 grow = assembler.Begin("Grow", widen);
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 1, 0));
    assembler.Emit(EncodeABC(Opcode::RetErrS, 0, 2, 0));
    assembler.End();
    // 4: ENDEXEC.B — Produce(s0)의 실패로 실행을 끝낸다.
    Shape end = forward;
    end.returnBank = 0;
    end.returnSlots = 0;
    const uint32 stop = assembler.Begin("Stop", end);
    assembler.Emit(EncodeABC(Opcode::MovS, 1, 0, 0));
    assembler.Emit(EncodeABx(Opcode::Call, 0, ref));
    assembler.Emit(EncodeABC(Opcode::EndExecB, 2, 0, 0));
    assembler.End();
    REQUIRE(Verify(assembler));

    VmExecution execution(allocator);
    const uint64 ok[] = {0};
    const uint64 bad[] = {2};
    auto code = [&]() noexcept {
        const VmBuffer* result = execution.ReturnedBuffers()[0];
        return result == nullptr ? NumericLimits<uint64>::Max() : Payload(result)[0];
    };
    REQUIRE(Run(execution, assembler.module, produce, Span<const uint64>(ok, 1)).outcome ==
            VmOutcome::Returned);
    CHECK(code() == 0);
    REQUIRE(Run(execution, assembler.module, produce, Span<const uint64>(bad, 1)).outcome ==
            VmOutcome::Returned);
    CHECK(code() == 3); // case 2 + 1
    REQUIRE(Run(execution, assembler.module, chain, Span<const uint64>(ok, 1)).outcome ==
            VmOutcome::Returned);
    CHECK(code() == 0);
    CHECK(VmStringOf(reinterpret_cast<const VmBuffer*>(
              static_cast<usize>(Payload(execution.ReturnedBuffers()[0])[1]))) == "v");
    REQUIRE(Run(execution, assembler.module, chain, Span<const uint64>(bad, 1)).outcome ==
            VmOutcome::Returned);
    CHECK(code() == 3);
    CHECK(Returns(Run(execution, assembler.module, narrow, Span<const uint64>(bad, 1)), 3));
    CHECK(execution.ReturnedSlots()[1] == 0);
    REQUIRE(Run(execution, assembler.module, grow, Span<const uint64>(bad, 1)).outcome ==
            VmOutcome::Returned);
    CHECK(code() == 2);
    const Outcome stopped = Run(execution, assembler.module, stop, Span<const uint64>(bad, 1));
    CHECK(stopped.outcome == VmOutcome::Ended);
    CHECK(stopped.value == 3);
    execution.ReleaseReturned();
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmStruct_VerifierChecksTypeTable) {
    SystemAllocator allocator;
    auto rejection = [&](auto&& change) noexcept {
        Assembler assembler(allocator);
        const uint32 type = PairType(assembler);
        BuildPair(assembler, type);
        change(assembler, type);
        return VerifyBytecode(assembler.module).error;
    };
    CHECK(rejection([](Assembler&, uint32) noexcept {}) == BytecodeLoadError::None);
    // NEWS의 Type 참조에 TYPE이 없으면(엔진 struct·TagSet — host 배치, LC5d) verifier는 받고 VM이
    // ContractViolation이다(아래 시험).
    CHECK(rejection([](Assembler& assembler, uint32 type) noexcept {
              assembler.module.refs[type].detail = NoBytecodeIndex;
          }) == BytecodeLoadError::None);
    // TYPE이 enum인 참조로 NEWS하면 거부다.
    CHECK(rejection([](Assembler& assembler, uint32 type) noexcept {
              const StringView cases[] = {"A"};
              const uint32 other = assembler.EnumRef("E", Span<const StringView>(cases, 1));
              assembler.module.refs[type].detail = assembler.module.refs[other].detail;
          }) == BytecodeLoadError::BadReference);
    // TYPE 번호가 표 밖이다.
    CHECK(rejection([](Assembler& assembler, uint32 type) noexcept {
              assembler.module.refs[type].detail = 7;
          }) == BytecodeLoadError::BadTable);
    // 필드가 자기 bank 묶음 밖이다.
    CHECK(rejection([](Assembler& assembler, uint32) noexcept {
              assembler.module.typeFields[1].offset = 0;
          }) == BytecodeLoadError::BadTable);
    // Buf 필드가 slot 둘이다.
    CHECK(rejection([](Assembler& assembler, uint32) noexcept {
              assembler.module.typeFields[1].slots = 2;
          }) == BytecodeLoadError::BadTable);
    // SETSF의 slot 수가 필드와 다르다(C 1 대신 3 — C 필드의 bank·slot과도 맞지 않게 A 필드).
    CHECK(rejection([](Assembler& assembler, uint32) noexcept {
              assembler.module.typeFields[0].slots = 3;
              assembler.module.types[0].slots[0] = 6;
              assembler.module.typeFields[2].offset = 3;
              assembler.module.typeFields[1].offset = 6;
          }) == BytecodeLoadError::BadOperand);
    // enum이 아닌 TYPE으로 enum을 표시한다.
    CHECK(rejection([](Assembler& assembler, uint32 type) noexcept {
              Shape shape;
              shape.registers[0] = 1;
              shape.frame[0] = 1;
              shape.registers[2] = 1;
              shape.frame[2] = 1;
              assembler.Begin("Show", shape);
              assembler.Emit(EncodeABC(Opcode::ToStr, 0, 0, 1));
              assembler.Emit(
                  EncodeAx(Opcode::Extra, (static_cast<uint32>(ScalarKind::Enum) << 16) | type));
              assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
              assembler.End();
          }) == BytecodeLoadError::BadReference);
}

TEST(SparksVmStruct_MismatchedFieldsAndPlaceholders) {
    // 다른 struct의 필드 참조는 버퍼의 bank 구역 밖이면 ContractViolation이다(실행 중 대조). TYPE이
    // 없는 NEWS(엔진 struct·TagSet)는 자리표시다.
    SystemAllocator allocator;
    Assembler assembler(allocator);
    const uint32 pair = PairType(assembler);
    const StringView names[] = {"N"};
    const uint8 banks[] = {0};
    const uint8 slots[] = {1};
    const uint32 small =
        assembler.StructRef("Small", Span<const StringView>(names, 1), Span<const uint8>(banks, 1),
                            Span<const uint8>(slots, 1));
    Shape shape;
    shape.registers[2] = 2;
    shape.frame[2] = 2;
    assembler.Begin("Mismatch", shape);
    assembler.Emit(EncodeABx(Opcode::NewS, 0, small));
    assembler.Emit(EncodeABC(Opcode::GetSFB, 1, 0, 1)); // Pair.B(Buf, offset 4) — Small 밖
    assembler.Emit(EncodeAx(Opcode::Extra, assembler.FieldRef(pair, 1)));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    assembler.Begin("Engine", shape);
    assembler.Emit(EncodeABx(Opcode::NewS, 0, assembler.Ref(BytecodeRefKind::Type, "SpawnParams")));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    CHECK(Faults(Run(execution, assembler.module, 0, Span<const uint64>()),
                 VmFaultKind::ContractViolation));
    CHECK(Faults(Run(execution, assembler.module, 1, Span<const uint64>()),
                 VmFaultKind::ContractViolation));
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmStruct_AllocationFailureEverywhere) {
    SystemAllocator backing;
    Assembler assembler(backing);
    const uint32 type = PairType(assembler);
    const uint32 function = BuildPair(assembler, type);
    REQUIRE(Verify(assembler));
    uint64 attempts = 0;
    uint64 failures = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        const uint64 before = backing.Stats().liveAllocations;
        {
            VmExecution execution(allocator);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto called = execution.Call(assembler.module, function, Span<const uint64>(), {}, {});
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                REQUIRE(called);
                attempts = used;
            } else if (!called) {
                CHECK(called.Error() == ErrorCode::OutOfMemory);
                ++failures;
            }

            execution.ReleaseReturned();
            CHECK(execution.LiveBuffers() == 0);
        }

        CHECK(backing.Stats().liveAllocations == before);
    }

    CHECK(failures > 0);
}

} // namespace sparks_vm_struct_tests_file
