#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include "VmTestSupport.h"

// 컨테이너 시험이다(Docs/Design/SparksVm.ko.md 2.2절, LC5b2): 증가와 COW 복제(공유한 쪽은 그대로),
// 원소 버퍼의 참조계수와 해제(ledger 0), 증가의 메모리 예산, 원소 번호 범위, 종류가 맞지 않는 명령,
// 할당 실패. 의미(삽입 순서·Map 교체·Set 중복)는 `.run` corpus(Run/Containers)가 본다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_vm_container_tests_file {

using namespace rocks::sparks::vm_test_support;

namespace {

constexpr uint32 ScalarBank = 0;
constexpr uint32 BufBank = 2;
constexpr uint32 Appended = 10;

uint32 Marker(uint32 valueBank, uint32 valueSlots, uint32 keyBank = 0,
              uint32 keySlots = 0) noexcept {
    return EncodeContainerMarker(valueBank, valueSlots, keyBank, keySlots);
}

// 함수 0: b0 = [0..9]을 APPEND로 만든 뒤 b1 = b0(공유), b1[0] = 99(COW), returnFirst면 b0을, 아니면
// b1을 돌려준다. Scalar s0..s2, Buf b0..b1.
void BuildShared(Assembler& assembler, bool returnFirst) noexcept {
    Shape shape;
    shape.registers[0] = 3;
    shape.frame[0] = 3;
    shape.registers[2] = 2;
    shape.frame[2] = 2;
    shape.returnBank = 2;
    shape.returnSlots = 1;
    assembler.Begin(returnFirst ? "First" : "Second", shape);
    assembler.Emit(EncodeABx(Opcode::NewA, 0, assembler.Ref(BytecodeRefKind::Type, "Array<int>")));
    for (uint32 value = 0; value < Appended; ++value) {
        assembler.Emit(EncodeAsBx(Opcode::LoadI, 1, static_cast<int32>(value)));
        assembler.Emit(EncodeABC(Opcode::Append, 0, 1, 0));
        assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 1)));
    }

    assembler.Emit(EncodeABC(Opcode::MovB, 1, 0, 0));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 0, 0));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 2, 99));
    assembler.Emit(EncodeABC(Opcode::SetE, 1, 0, 2));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 1, ScalarBank, 1)));
    assembler.Emit(EncodeABC(Opcode::Ret, returnFirst ? 0 : 1, 1, 2));
    assembler.End();
}

// 함수: Map<string, Array<string>> 하나와 그 공유 사본을 만들고 사본만 바꿔 돌려준다(원소 버퍼의
// 참조계수와 해제를 본다). Buf b0 = map, b1 = 사본, b2 = key, b3 = 값 배열, b4 = 문자열.
uint32 BuildNested(Assembler& assembler) noexcept {
    Shape shape;
    shape.registers[2] = 5;
    shape.frame[2] = 5;
    shape.returnBank = 2;
    shape.returnSlots = 1;
    const uint32 function = assembler.Begin("Nested", shape);
    const uint32 words = Marker(BufBank, 1);
    const uint32 entry = Marker(BufBank, 1, BufBank, 1);
    assembler.Emit(EncodeABx(Opcode::NewM, 0, assembler.Ref(BytecodeRefKind::Type, "Map")));
    assembler.Emit(EncodeABx(Opcode::NewA, 3, assembler.Ref(BytecodeRefKind::Type, "Array")));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 4, assembler.String("x")));
    assembler.Emit(EncodeABC(Opcode::Append, 3, 4, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, words));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 2, assembler.String("key")));
    assembler.Emit(EncodeABC(Opcode::Put, 0, 2, 3));
    assembler.Emit(EncodeAx(Opcode::Extra, entry));
    assembler.Emit(EncodeABC(Opcode::MovB, 1, 0, 0));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 4, assembler.String("y")));
    assembler.Emit(EncodeABC(Opcode::Append, 3, 4, 0)); // 배열은 map 안과 공유 — 복제된다
    assembler.Emit(EncodeAx(Opcode::Extra, words));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 2, assembler.String("other")));
    assembler.Emit(EncodeABC(Opcode::SetE, 1, 2, 3)); // 사본 map만 바뀐다
    assembler.Emit(EncodeAx(Opcode::Extra, entry));
    assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 2));
    assembler.End();
    return function;
}

uint64 Element(const VmBuffer* buffer, uint32 index) noexcept {
    return reinterpret_cast<const uint64*>(buffer->Bytes())[index];
}

} // namespace

TEST(SparksVmContainer_GrowthAndCopyOnWrite) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    BuildShared(assembler, true);
    BuildShared(assembler, false);
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    for (uint32 function = 0; function < 2; ++function) {
        const Outcome outcome = Run(execution, assembler.module, function, Span<const uint64>());
        REQUIRE(outcome.outcome == VmOutcome::Returned);
        const VmBuffer* array = execution.ReturnedBuffers()[0];
        REQUIRE(array != nullptr);
        CHECK(array->kind == VmBufferKind::Array);
        CHECK(array->count == Appended);
        CHECK(array->capacity >= Appended); // 4 → 8 → 16
        CHECK(array->references == 1);
        CHECK(Element(array, 0) == (function == 0 ? 0u : 99u)); // 공유한 원본은 그대로
        CHECK(Element(array, Appended - 1) == Appended - 1);
        CHECK(execution.LiveBuffers() == 1);
        execution.ReleaseReturned();
        CHECK(execution.LiveBuffers() == 0);
        CHECK(execution.LiveBytes() == 0);
    }
}

TEST(SparksVmContainer_NestedBuffersAreShared) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    const uint32 nested = BuildNested(assembler);
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    REQUIRE(Run(execution, assembler.module, nested, Span<const uint64>()).outcome ==
            VmOutcome::Returned);
    const VmBuffer* map = execution.ReturnedBuffers()[0];
    REQUIRE(map != nullptr && map->kind == VmBufferKind::Map);
    REQUIRE(map->count == 2);
    // 원소 0("key")의 값 배열은 ["x"](map에 넣은 뒤 바꾼 배열은 복제본이다), 원소 1("other")은
    // ["x", "y"].
    const VmBuffer* firstKey =
        reinterpret_cast<const VmBuffer*>(static_cast<usize>(Element(map, 0)));
    const VmBuffer* firstArray =
        reinterpret_cast<const VmBuffer*>(static_cast<usize>(Element(map, 1)));
    const VmBuffer* secondArray =
        reinterpret_cast<const VmBuffer*>(static_cast<usize>(Element(map, 3)));
    CHECK(VmStringOf(firstKey) == "key");
    CHECK(firstArray->count == 1);
    CHECK(secondArray->count == 2);
    CHECK(VmStringOf(reinterpret_cast<const VmBuffer*>(
              static_cast<usize>(Element(secondArray, 1)))) == "y");
    // "x" 문자열 하나를 두 배열이 나눈다.
    CHECK(Element(firstArray, 0) == Element(secondArray, 0));
    CHECK(
        reinterpret_cast<const VmBuffer*>(static_cast<usize>(Element(firstArray, 0)))->references ==
        2);
    execution.ReleaseReturned();
    CHECK(execution.LiveBuffers() == 0);
    CHECK(execution.LiveBytes() == 0);
}

TEST(SparksVmContainer_BudgetOnGrowth) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    BuildShared(assembler, true);
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    VmLimits limits;
    limits.memoryBytes = 160; // 빈 배열과 용량 4까지, 8로 늘리는 할당에서 넘는다
    auto called = execution.Call(assembler.module, 0, Span<const uint64>(), {}, {}, limits);
    REQUIRE(called);
    CHECK(called.Value().fault.kind == VmFaultKind::BudgetExceeded);
    CHECK(called.Value().fault.address > 0);
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmContainer_EntriesAndMismatchedKinds) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Shape shape;
    shape.registers[0] = 2;
    shape.frame[0] = 2;
    shape.parameters[0] = 1;
    shape.registers[2] = 1;
    shape.frame[2] = 1;
    shape.returnSlots = 1;
    // 0: [7]의 GETVAL s0(범위 밖이면 IndexOutOfRange).
    assembler.Begin("Value", shape);
    assembler.Emit(EncodeABx(Opcode::NewA, 0, assembler.Ref(BytecodeRefKind::Type, "Array<int>")));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 1, 7));
    assembler.Emit(EncodeABC(Opcode::Append, 0, 1, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 1)));
    assembler.Emit(EncodeABC(Opcode::GetVal, 1, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 1)));
    assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
    assembler.End();
    // 1: Array의 GETKEY, 2: Map에 APPEND — 종류가 맞지 않는 명령은 ContractViolation.
    assembler.Begin("Key", shape);
    assembler.Emit(EncodeABx(Opcode::NewA, 0, assembler.Ref(BytecodeRefKind::Type, "Array<int>")));
    assembler.Emit(EncodeABC(Opcode::Append, 0, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 1)));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 1, 0));
    assembler.Emit(EncodeABC(Opcode::GetKey, 1, 0, 1));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 1)));
    assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
    assembler.End();
    assembler.Begin("Append", shape);
    assembler.Emit(EncodeABx(Opcode::NewM, 0, assembler.Ref(BytecodeRefKind::Type, "Map")));
    assembler.Emit(EncodeABC(Opcode::Append, 0, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 1)));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    // 3: 빈 Map의 GETE는 없음(표지 0과 0), LEN은 0.
    Shape optional = shape;
    optional.registers[0] = 3;
    optional.frame[0] = 3;
    optional.returnSlots = 3;
    assembler.Begin("Missing", optional);
    assembler.Emit(EncodeABx(Opcode::NewM, 0, assembler.Ref(BytecodeRefKind::Type, "Map")));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 1, 5));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 2, 5));
    assembler.Emit(EncodeABC(Opcode::GetE, 1, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 2, ScalarBank, 1)));
    assembler.Emit(EncodeABC(Opcode::Len, 0, 0, 0));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 3, 0));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    const uint64 zero[] = {0};
    const uint64 one[] = {1};
    CHECK(Returns(Run(execution, assembler.module, 0, Span<const uint64>(zero, 1)), 7));
    CHECK(Faults(Run(execution, assembler.module, 0, Span<const uint64>(one, 1)),
                 VmFaultKind::IndexOutOfRange));
    CHECK(Faults(Run(execution, assembler.module, 1, Span<const uint64>(zero, 1)),
                 VmFaultKind::ContractViolation));
    CHECK(Faults(Run(execution, assembler.module, 2, Span<const uint64>(zero, 1)),
                 VmFaultKind::ContractViolation));
    CHECK(Returns(Run(execution, assembler.module, 3, Span<const uint64>(zero, 1)), 0));
    REQUIRE(execution.ReturnedSlots().Size() == 3);
    CHECK(execution.ReturnedSlots()[1] == 0);
    CHECK(execution.ReturnedSlots()[2] == 0);
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmContainer_AllocationFailureEverywhere) {
    SystemAllocator backing;
    Assembler assembler(backing);
    const uint32 nested = BuildNested(assembler);
    BuildShared(assembler, false);
    REQUIRE(Verify(assembler));
    for (uint32 function = nested; function < nested + 2; ++function) {
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

                auto called =
                    execution.Call(assembler.module, function, Span<const uint64>(), {}, {});
                const uint64 used = allocator.Attempts();
                allocator.DisableFailure();
                if (failAt == 0) {
                    REQUIRE(called);
                    CHECK(called.Value().outcome == VmOutcome::Returned);
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
}

} // namespace sparks_vm_container_tests_file
