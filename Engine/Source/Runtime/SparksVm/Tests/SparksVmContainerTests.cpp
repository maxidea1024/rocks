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
        const Outcome outcome = Run(execution, assembler.Linked(), function, Span<const uint64>());
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
    REQUIRE(Run(execution, assembler.Linked(), nested, Span<const uint64>()).outcome ==
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
    auto called = execution.Call(assembler.Linked(), 0, Span<const uint64>(), {}, {}, limits);
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
    CHECK(Returns(Run(execution, assembler.Linked(), 0, Span<const uint64>(zero, 1)), 7));
    CHECK(Faults(Run(execution, assembler.Linked(), 0, Span<const uint64>(one, 1)),
                 VmFaultKind::IndexOutOfRange));
    CHECK(Faults(Run(execution, assembler.Linked(), 1, Span<const uint64>(zero, 1)),
                 VmFaultKind::ContractViolation));
    CHECK(Faults(Run(execution, assembler.Linked(), 2, Span<const uint64>(zero, 1)),
                 VmFaultKind::ContractViolation));
    CHECK(Returns(Run(execution, assembler.Linked(), 3, Span<const uint64>(zero, 1)), 0));
    REQUIRE(execution.ReturnedSlots().Size() == 3);
    CHECK(execution.ReturnedSlots()[1] == 0);
    CHECK(execution.ReturnedSlots()[2] == 0);
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmContainer_LayoutMismatchAndValueReplacement) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Shape shape;
    shape.registers[0] = 4;
    shape.frame[0] = 4;
    shape.registers[2] = 3;
    shape.frame[2] = 3;
    // 0: Array를 (S,1)로 채운 뒤 (S,3) 표지로 APPEND — 배치 어긋남은 ContractViolation.
    assembler.Begin("Mismatch", shape);
    assembler.Emit(EncodeABx(Opcode::NewA, 0, assembler.Ref(BytecodeRefKind::Type, "Array<int>")));
    assembler.Emit(EncodeABC(Opcode::Append, 0, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 1)));
    assembler.Emit(EncodeABC(Opcode::Append, 0, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, Marker(ScalarBank, 3)));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    // 1: Map<int, string>의 같은 key에 PUT 두 번 — 이전 값 버퍼는 놓인다. 반환은 Map.
    Shape map = shape;
    map.returnBank = 2;
    map.returnSlots = 1;
    assembler.Begin("Replace", map);
    const uint32 entry = Marker(BufBank, 1, ScalarBank, 1);
    assembler.Emit(EncodeABx(Opcode::NewM, 0, assembler.Ref(BytecodeRefKind::Type, "Map")));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 0, 7));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 1, assembler.String("first")));
    assembler.Emit(EncodeABC(Opcode::Put, 0, 0, 1));
    assembler.Emit(EncodeAx(Opcode::Extra, entry));
    assembler.Emit(EncodeABx(Opcode::LoadStr, 1, assembler.String("second")));
    assembler.Emit(EncodeABC(Opcode::Put, 0, 0, 1));
    assembler.Emit(EncodeAx(Opcode::Extra, entry));
    assembler.Emit(EncodeABC(Opcode::LoadNullB, 1, 0, 0));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    CHECK(Faults(Run(execution, assembler.Linked(), 0, Span<const uint64>()),
                 VmFaultKind::ContractViolation));
    REQUIRE(Run(execution, assembler.Linked(), 1, Span<const uint64>()).outcome ==
            VmOutcome::Returned);
    const VmBuffer* result = execution.ReturnedBuffers()[0];
    REQUIRE(result != nullptr && result->count == 1);
    CHECK(VmStringOf(reinterpret_cast<const VmBuffer*>(static_cast<usize>(Element(result, 1)))) ==
          "second");
    CHECK(execution.LiveBuffers() == 2); // map과 "second"("first"는 놓였다)
    execution.ReleaseReturned();
    CHECK(execution.LiveBuffers() == 0);

    // verifier: Ref·Buf 묶음은 slot 하나다(표지·TRY.B·OK).
    Assembler bad(allocator);
    bad.Begin("Wide", shape);
    bad.Emit(EncodeABx(Opcode::NewA, 0, bad.Ref(BytecodeRefKind::Type, "Array<string>")));
    bad.Emit(EncodeABC(Opcode::Append, 0, 1, 0));
    bad.Emit(EncodeAx(Opcode::Extra, Marker(BufBank, 2)));
    bad.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    bad.End();
    CHECK(VerifyBytecode(bad.module).error == BytecodeLoadError::BadOperand);
}

TEST(SparksVmContainer_CallTakesPreviousReturn) {
    // 이전 Call의 반환 버퍼를 다음 Call의 인자로 넘겨도 된다(인자를 먼저 붙잡는다).
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Shape shape;
    shape.registers[2] = 1;
    shape.frame[2] = 1;
    shape.parameters[2] = 1;
    shape.returnBank = 2;
    shape.returnSlots = 1;
    assembler.Begin("Echo", shape);
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    auto text = execution.NewString("kept");
    REQUIRE(text);
    VmBuffer* const first[] = {text.Value()};
    REQUIRE(execution.Call(assembler.Linked(), 0, {}, {}, Span<VmBuffer* const>(first, 1)));
    execution.Release(text.Value()); // 이제 반환만 참조를 가진다
    for (uint32 round = 0; round < 3; ++round) {
        auto again = execution.Call(assembler.Linked(), 0, {}, {}, execution.ReturnedBuffers());
        REQUIRE(again);
        CHECK(VmStringOf(execution.ReturnedBuffers()[0]) == "kept");
    }

    execution.ReleaseReturned();
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
                    execution.Call(assembler.Linked(), function, Span<const uint64>(), {}, {});
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

TEST(SparksVmContainer_HostContainers) {
    // host 컨테이너(NewContainer): 배치 검사, Map·Set의 같은 key 거부, Buf 원소는 빌려 준다(참조
    // +1), 실패해도 ledger는 0이다.
    SystemAllocator allocator;
    VmExecution execution(allocator);
    const uint64 numbers[] = {3, 1, 2};
    auto array = execution.NewContainer(VmBufferKind::Array, Marker(ScalarBank, 1),
                                        Span<const uint64>(numbers, 3));
    REQUIRE(array);
    CHECK(array.Value()->count == 3);
    CHECK(array.Value()->references == 1);
    execution.Release(array.Value());

    auto empty = execution.NewContainer(VmBufferKind::Set, Marker(ScalarBank, 1), {});
    REQUIRE(empty);
    CHECK(empty.Value()->count == 0);
    execution.Release(empty.Value());

    // 같은 key의 Set·Map은 거부한다.
    const uint64 repeated[] = {7, 7};
    CHECK(
        execution
            .NewContainer(VmBufferKind::Set, Marker(ScalarBank, 1), Span<const uint64>(repeated, 2))
            .Error() == ErrorCode::InvalidArgument);
    const uint64 entries[] = {1, 10, 1, 20};
    CHECK(execution
              .NewContainer(VmBufferKind::Map, Marker(ScalarBank, 1, ScalarBank, 1),
                            Span<const uint64>(entries, 4))
              .Error() == ErrorCode::InvalidArgument);

    // 배치: 컨테이너 아닌 종류, 값 slot 0, Map의 key 없음, Array의 key 있음, slot 수가 배수 아님.
    CHECK(execution.NewContainer(VmBufferKind::String, Marker(ScalarBank, 1), {}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(execution.NewContainer(VmBufferKind::Array, Marker(ScalarBank, 0), {}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(execution.NewContainer(VmBufferKind::Map, Marker(ScalarBank, 1), {}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(execution.NewContainer(VmBufferKind::Array, Marker(ScalarBank, 1, ScalarBank, 1), {})
              .Error() == ErrorCode::InvalidArgument);
    CHECK(execution
              .NewContainer(VmBufferKind::Array, Marker(ScalarBank, 2),
                            Span<const uint64>(numbers, 3))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(execution.LiveBuffers() == 0);

    // Buf 원소(문자열 key의 Map): 컨테이너가 참조를 더하고 host는 자기 참조를 놓는다.
    auto a = execution.NewString("a");
    auto b = execution.NewString("b");
    REQUIRE(a);
    REQUIRE(b);
    const uint64 named[] = {static_cast<uint64>(reinterpret_cast<usize>(a.Value())), 1,
                            static_cast<uint64>(reinterpret_cast<usize>(b.Value())), 2};
    auto map = execution.NewContainer(VmBufferKind::Map, Marker(ScalarBank, 1, BufBank, 1),
                                      Span<const uint64>(named, 4));
    REQUIRE(map);
    CHECK(a.Value()->references == 2);
    const uint64 same[] = {static_cast<uint64>(reinterpret_cast<usize>(a.Value())),
                           static_cast<uint64>(reinterpret_cast<usize>(a.Value()))};
    CHECK(execution.NewContainer(VmBufferKind::Set, Marker(BufBank, 1), Span<const uint64>(same, 2))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(a.Value()->references == 2); // 실패한 컨테이너가 더한 참조는 되돌렸다
    execution.Release(a.Value());
    execution.Release(b.Value());
    CHECK(execution.LiveBuffers() == 1 + 2);
    execution.Release(map.Value());
    CHECK(execution.LiveBuffers() == 0);
    CHECK(execution.LiveBytes() == 0);
}

TEST(SparksVmContainer_HostContainerAllocationFailure) {
    SystemAllocator backing;
    const uint64 numbers[] = {1, 2, 3, 4, 5};
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

            auto made = execution.NewContainer(VmBufferKind::Set, Marker(ScalarBank, 1),
                                               Span<const uint64>(numbers, 5));
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                REQUIRE(made);
                attempts = used;
            } else if (!made) {
                CHECK(made.Error() == ErrorCode::OutOfMemory);
                ++failures;
            }

            if (made) {
                execution.Release(made.Value());
            }

            CHECK(execution.LiveBuffers() == 0);
        }

        CHECK(backing.Stats().liveAllocations == before);
    }

    CHECK(failures > 0);
}

} // namespace sparks_vm_container_tests_file
