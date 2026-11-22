#include <Core/Math/Matrix4.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/FloatingPointEnvironment.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include "VmTestSupport.h"

// VM 경계 시험이다(Docs/Design/SparksVm.ko.md 2–4절, LC5a 리뷰): 중첩 frame의 fault 자리, 새
// frame의 0 채우기, TRY.S 겹침·여러 slot, SWITCH의 64비트 case, 정수·변환 경계, 묶음 실수 비교,
// 비가환 합성 곱의 순서, 부동소수점 환경(FTZ·DAZ), callee 안의 예산, 할당 실패 뒤 재사용.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_vm_edge_tests_file {

using namespace rocks::sparks::vm_test_support;

namespace {

// Scalar 하나를 받아 Scalar 하나를 돌려주는 모양이다(registers·frame은 따로 정한다).
Shape OneToOne(uint16 registers, uint16 frame) noexcept {
    Shape shape;
    shape.registers[0] = registers;
    shape.frame[0] = frame;
    shape.parameters[0] = 1;
    shape.returnSlots = 1;
    return shape;
}

// s0을 창(s1)에 두고 callee를 부른 뒤 결과를 돌려주는 caller를 조립한다.
uint32 Caller(Assembler& assembler, uint32 callee, StringView name) noexcept {
    const uint32 caller = assembler.Begin(name, OneToOne(1, 2));
    assembler.Emit(EncodeABC(Opcode::MovS, 1, 0, 0));
    assembler.Emit(
        EncodeABx(Opcode::Call, 0, assembler.Ref(BytecodeRefKind::Function, "f", callee)));
    assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
    assembler.End();
    return caller;
}

Outcome Convert(ScalarKind from, ScalarKind to, uint64 value) noexcept {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    assembler.Begin("Convert", OneToOne(2, 2));
    assembler.Emit(EncodeABC(Opcode::Conv, 1, 0, (static_cast<uint32>(from) << 4) | uint32(to)));
    assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
    assembler.End();
    if (!Verify(assembler)) {
        return Outcome{};
    }

    VmExecution execution(allocator);
    const uint64 arguments[] = {value};
    return Run(execution, assembler.Linked(), 0, Span<const uint64>(arguments, 1));
}

uint64 FloatBits(float value) noexcept {
    return Bits(static_cast<double>(value));
}

} // namespace

TEST(SparksVmEdge_NestedFrames) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    // 0: callee — s0 + 127을 int8로(1이면 overflow).
    const uint32 add = assembler.Begin("Add127", OneToOne(2, 2));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 1, 127));
    assembler.Emit(EncodeABC(Opcode::AddI8, 0, 0, 1));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    const uint32 outer = Caller(assembler, add, "Outer");
    // 2: callee — ENDEXEC(실행 전체를 끝낸다 — caller로 돌아가지 않는다).
    const uint32 end = assembler.Begin("End", OneToOne(1, 1));
    assembler.Emit(EncodeABC(Opcode::EndExecS, 0, 1, 0));
    assembler.End();
    const uint32 ender = Caller(assembler, end, "Ender");
    REQUIRE(Verify(assembler));

    VmExecution execution(allocator);
    const uint64 zero[] = {0};
    const uint64 one[] = {1};
    CHECK(Returns(Run(execution, assembler.Linked(), outer, Span<const uint64>(zero, 1)), 127));
    auto faulted = execution.Call(assembler.Linked(), outer, Span<const uint64>(one, 1), {}, {});
    REQUIRE(faulted);
    CHECK(faulted.Value().fault.kind == VmFaultKind::IntegerOverflow);
    CHECK(faulted.Value().fault.function == add); // callee의 자리다
    CHECK(faulted.Value().fault.address == 1);
    CHECK(faulted.Value().steps == 4);
    const uint64 code[] = {5};
    const Outcome ended = Run(execution, assembler.Linked(), ender, Span<const uint64>(code, 1));
    CHECK(ended.outcome == VmOutcome::Ended);
    CHECK(ended.value == 5);
    CHECK(ended.steps == 3);
}

TEST(SparksVmEdge_NewFramesStartZeroed) {
    // 앞선 호출이 같은 stack 자리에 남긴 값은 새 frame에서 보이지 않는다(V2 — 0 채우기).
    SystemAllocator allocator;
    Assembler assembler(allocator);
    const uint32 dirty = assembler.Begin("Dirty", OneToOne(4, 4));
    for (uint32 at = 1; at < 4; ++at) {
        assembler.Emit(EncodeAsBx(Opcode::LoadI, at, 77));
    }

    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    const uint32 read = assembler.Begin("Read", OneToOne(3, 3));
    assembler.Emit(EncodeABC(Opcode::AddI64, 0, 1, 2)); // 초기화 전 s1 + s2
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    Shape both = OneToOne(1, 2);
    const uint32 main = assembler.Begin("Main", both);
    assembler.Emit(EncodeABC(Opcode::MovS, 1, 0, 0));
    assembler.Emit(
        EncodeABx(Opcode::Call, 0, assembler.Ref(BytecodeRefKind::Function, "d", dirty)));
    assembler.Emit(EncodeABC(Opcode::MovS, 1, 0, 0));
    assembler.Emit(EncodeABx(Opcode::Call, 0, assembler.Ref(BytecodeRefKind::Function, "r", read)));
    assembler.Emit(EncodeABC(Opcode::Ret, 1, 1, 0));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    const uint64 argument[] = {9};
    CHECK(Returns(Run(execution, assembler.Linked(), main, Span<const uint64>(argument, 1)), 0));
}

TEST(SparksVmEdge_TryMovesOverlappingTuples) {
    // TRY.S A B C: Result<vec3>(코드, x, y, z)의 값 3 slot을 A로. A가 원본 뒤에 겹쳐도(memmove
    // 의미) A == B여도 원본 값이다. 실패는 EXTRA 거리의 실패 블록이다.
    const uint32 targets[] = {0, 1, 2};
    for (const uint32 target : targets) {
        SystemAllocator allocator;
        Assembler assembler(allocator);
        Shape shape;
        shape.registers[0] = 5;
        shape.frame[0] = 5;
        shape.parameters[0] = 4;
        shape.returnSlots = 3;
        assembler.Begin("Try", shape);
        assembler.Emit(EncodeABC(Opcode::TryS, target, 0, 4));
        assembler.Emit(EncodeAx(Opcode::Extra, 1)); // 실패면 RET s0 x3(코드, x, y)
        assembler.Emit(EncodeABC(Opcode::Ret, target, 3, 0));
        assembler.Emit(EncodeABC(Opcode::Ret, 0, 3, 0));
        assembler.End();
        REQUIRE(Verify(assembler));
        VmExecution execution(allocator);
        const uint64 ok[] = {0, Bits(1.0), Bits(2.0), Bits(3.0)};
        CHECK(Returns(Run(execution, assembler.Linked(), 0, Span<const uint64>(ok, 4)), Bits(1.0)));
        REQUIRE(execution.ReturnedSlots().Size() == 3);
        CHECK(execution.ReturnedSlots()[1] == Bits(2.0));
        CHECK(execution.ReturnedSlots()[2] == Bits(3.0));
        const uint64 failed[] = {3, Bits(1.0), Bits(2.0), Bits(3.0)};
        CHECK(Returns(Run(execution, assembler.Linked(), 0, Span<const uint64>(failed, 4)), 3));
    }
}

TEST(SparksVmEdge_SwitchCases) {
    // case 값은 int64 원문 값이고 레지스터는 확장값이다: 2^63 이상의 uint64와 음수가 비트로 맞는다.
    SystemAllocator allocator;
    Assembler assembler(allocator);
    assembler.Begin("Switch", OneToOne(1, 1));
    BytecodeSwitchTable table;
    table.firstCase = 0;
    table.caseCount = 2;
    table.defaultTarget = 5;
    CHECK(assembler.module.cases.TryPushBack(
        BytecodeSwitchCase{NumericLimits<int64>::Min(), 1})); // uint64 2^63
    CHECK(assembler.module.cases.TryPushBack(BytecodeSwitchCase{-1, 3}));
    CHECK(assembler.module.switches.TryPushBack(table));
    assembler.module.functions[0].switchCount = 1;
    assembler.Emit(EncodeABx(Opcode::Switch, 0, 0));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 0, 1));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 0, 2));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 0, 3));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    auto pick = [&](uint64 value) noexcept {
        const uint64 argument[] = {value};
        return Run(execution, assembler.Linked(), 0, Span<const uint64>(argument, 1));
    };
    CHECK(Returns(pick(uint64(1) << 63), 1));
    CHECK(Returns(pick(Signed(-1)), 2));
    CHECK(Returns(pick(0xFF), 3)); // 확장하지 않은 int8 -1은 맞지 않는다(정규형 — V8)
    CHECK(Returns(pick(5), 3));
}

TEST(SparksVmEdge_IntegerBoundaries) {
    const uint64 min64 = Signed(NumericLimits<int64>::Min());
    CHECK(Faults(RunBinary(Opcode::MulI64, min64, Signed(-1)), VmFaultKind::IntegerOverflow));
    CHECK(Faults(RunBinary(Opcode::MulI8, Signed(-128), Signed(-1)), VmFaultKind::IntegerOverflow));
    CHECK(Returns(RunBinary(Opcode::MulI8, Signed(-128), 1), Signed(-128)));
    CHECK(Faults(RunBinary(Opcode::SubI64, min64, 1), VmFaultKind::IntegerOverflow));
    CHECK(Faults(RunBinary(Opcode::NegI64, min64, 0), VmFaultKind::IntegerOverflow));
    CHECK(Returns(RunBinary(Opcode::NegI16, Signed(-32767), 0), 32767));
    CHECK(Faults(RunBinary(Opcode::DivI16, Signed(-32768), Signed(-1)),
                 VmFaultKind::IntegerOverflow));
    CHECK(Returns(RunBinary(Opcode::ModI8, Signed(-128), Signed(-1)), 0));
    CHECK(Faults(RunBinary(Opcode::AddU64, NumericLimits<uint64>::Max(), 1),
                 VmFaultKind::IntegerOverflow));
    CHECK(Faults(RunBinary(Opcode::MulU32, 65536, 65536), VmFaultKind::IntegerOverflow));
    CHECK(Faults(RunBinary(Opcode::SubU64, 0, 1), VmFaultKind::IntegerOverflow));
    CHECK(
        Returns(RunBinary(Opcode::AddI32, Signed(-2147483647), Signed(-1)), Signed(-2147483648LL)));
}

TEST(SparksVmEdge_ConversionBoundaries) {
    CHECK(
        Faults(Convert(ScalarKind::I8, ScalarKind::U8, Signed(-1)), VmFaultKind::IntegerOverflow));
    CHECK(Returns(Convert(ScalarKind::I64, ScalarKind::I32, Signed(-2147483648LL)),
                  Signed(-2147483648LL)));
    CHECK(Faults(Convert(ScalarKind::I64, ScalarKind::I32, Signed(-2147483649LL)),
                 VmFaultKind::IntegerOverflow));
    CHECK(Faults(Convert(ScalarKind::U64, ScalarKind::I64, uint64(1) << 63),
                 VmFaultKind::IntegerOverflow));
    CHECK(Returns(Convert(ScalarKind::I64, ScalarKind::F32, Signed(NumericLimits<int64>::Min())),
                  Bits(-9223372036854775808.0)));
    CHECK(Faults(Convert(ScalarKind::I64, ScalarKind::F32, (1 << 24) + 1),
                 VmFaultKind::IntegerOverflow));
    CHECK(Faults(Convert(ScalarKind::U64, ScalarKind::F32, NumericLimits<uint64>::Max()),
                 VmFaultKind::IntegerOverflow));
    CHECK(Faults(Convert(ScalarKind::U64, ScalarKind::F64, NumericLimits<uint64>::Max()),
                 VmFaultKind::IntegerOverflow));
    // F64 → F32: 최근접 짝수(1 + 2^-24는 1.0으로, 1 + 3·2^-24는 1 + 2^-22로), 비정규 수와 -0.0
    // 보존.
    const double ulp = 1.0 / 16777216.0;
    CHECK(Returns(Convert(ScalarKind::F64, ScalarKind::F32, Bits(1.0 + ulp)), Bits(1.0)));
    CHECK(Returns(Convert(ScalarKind::F64, ScalarKind::F32, Bits(1.0 + 3 * ulp)),
                  Bits(1.0 + 4 * ulp)));
    CHECK(Returns(Convert(ScalarKind::F64, ScalarKind::F32, Bits(1e-40)),
                  FloatBits(static_cast<float>(1e-40))));
    CHECK(Returns(Convert(ScalarKind::F64, ScalarKind::F32, Bits(-0.0)), Bits(-0.0)));
}

TEST(SparksVmEdge_TupleFloatComparisons) {
    // EQN.F: 성분 NaN은 같지 않고, 있음 표지(0·1)는 비정규 수 비트라도 비트로 다르다. 3 slot.
    auto compare = [](Opcode op, const uint64(&left)[3],
                      const uint64(&right)[3]) noexcept -> Outcome {
        SystemAllocator allocator;
        Assembler assembler(allocator);
        Shape shape;
        shape.registers[0] = 7;
        shape.frame[0] = 7;
        shape.parameters[0] = 6;
        shape.returnSlots = 1;
        assembler.Begin("Tuple", shape);
        assembler.Emit(EncodeABC(op, 6, 0, 3));
        assembler.Emit(EncodeAx(Opcode::Extra, 3));
        assembler.Emit(EncodeABC(Opcode::Ret, 6, 1, 0));
        assembler.End();
        if (!Verify(assembler)) {
            return Outcome{};
        }

        VmExecution execution(allocator);
        const uint64 arguments[] = {left[0], left[1], left[2], right[0], right[1], right[2]};
        return Run(execution, assembler.Linked(), 0, Span<const uint64>(arguments, 6));
    };
    const uint64 nan = 0x7FF8000000000000ull;
    const uint64 a[] = {1, Bits(0.0), Bits(2.0)};
    const uint64 b[] = {1, Bits(-0.0), Bits(2.0)};
    const uint64 none[] = {0, Bits(0.0), Bits(2.0)};
    const uint64 withNan[] = {1, nan, Bits(2.0)};
    CHECK(Returns(compare(Opcode::EqNF, a, b), 1));
    CHECK(Returns(compare(Opcode::EqNF, a, none), 0));
    CHECK(Returns(compare(Opcode::EqNF, withNan, withNan), 0));
    CHECK(Returns(compare(Opcode::NeNF, withNan, withNan), 1));
}

TEST(SparksVmEdge_FloatEnvironmentIsControlled) {
    // 스레드에 FTZ·DAZ가 켜져 있어도 VM은 기대 환경(비정규 수 보존)에서 계산한다.
    const uint64 saved = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(saved | platform::detail::FlushToZeroBit() |
                                                platform::detail::DenormalsAreZeroBit());
    const Outcome product = RunBinary(Opcode::MulF32, FloatBits(1e-20f), FloatBits(1e-20f));
    const Outcome flag = RunBinary(Opcode::EqF, 1, 0); // 비정규 수 비트 1 대 0.0
    const uint64 after = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(saved);
    CHECK(Returns(product, FloatBits(1e-20f * 1e-20f)));
    CHECK(product.value != 0);
    CHECK(Returns(flag, 0));
    // 호출 뒤 스레드 환경은 호출 전 값으로 돌아온다.
    CHECK(
        (after & (platform::detail::FlushToZeroBit() | platform::detail::DenormalsAreZeroBit())) ==
        (platform::detail::FlushToZeroBit() | platform::detail::DenormalsAreZeroBit()));
}

TEST(SparksVmEdge_CompositeProductOrder) {
    // MUL.QUAT·MUL.MAT4는 B * C(Core·UE의 operator* 순서)다. 교환되지 않는 입력으로 순서를 본다.
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Shape quat;
    quat.registers[0] = 12;
    quat.frame[0] = 12;
    quat.parameters[0] = 8;
    quat.returnSlots = 4;
    assembler.Begin("Quat", quat);
    assembler.Emit(EncodeABC(Opcode::MulQ, 8, 0, 4));
    assembler.Emit(EncodeABC(Opcode::Ret, 8, 4, 0));
    assembler.End();
    Shape mat;
    mat.registers[0] = 48;
    mat.frame[0] = 48;
    mat.parameters[0] = 32;
    mat.returnSlots = 16;
    assembler.Begin("Matrix", mat);
    assembler.Emit(EncodeABC(Opcode::MulM, 32, 0, 16));
    assembler.Emit(EncodeABC(Opcode::Ret, 32, 16, 0));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);

    const double half = 0.7071067811865476;
    const Quaternion<double> z(0.0, 0.0, half, half);
    const Quaternion<double> x(half, 0.0, 0.0, half);
    const Quaternion<double> expected = z * x;
    const Quaternion<double> reversed = x * z;
    const uint64 quats[] = {Bits(z.x), Bits(z.y), Bits(z.z), Bits(z.w),
                            Bits(x.x), Bits(x.y), Bits(x.z), Bits(x.w)};
    REQUIRE(Returns(Run(execution, assembler.Linked(), 0, Span<const uint64>(quats, 8)),
                    Bits(expected.x)));
    const double got[] = {expected.x, expected.y, expected.z, expected.w};
    const double other[] = {reversed.x, reversed.y, reversed.z, reversed.w};
    bool differs = false;
    for (uint32 index = 0; index < 4; ++index) {
        CHECK(execution.ReturnedSlots()[index] == Bits(got[index]));
        differs = differs || got[index] != other[index];
    }

    CHECK(differs);

    const Matrix4<double> left = Matrix4<double>::Translation(Vector3<double>(1.0, 2.0, 3.0));
    const Matrix4<double> right = Matrix4<double>::Scale(Vector3<double>(2.0, 3.0, 4.0));
    const Matrix4<double> product = left * right;
    const Matrix4<double> swapped = right * left;
    uint64 matrices[32] = {};
    for (uint32 index = 0; index < 16; ++index) {
        matrices[index] = Bits(left.m[index]);
        matrices[16 + index] = Bits(right.m[index]);
    }

    REQUIRE(Returns(Run(execution, assembler.Linked(), 1, Span<const uint64>(matrices, 32)),
                    Bits(product.m[0])));
    differs = false;
    for (uint32 index = 0; index < 16; ++index) {
        CHECK(execution.ReturnedSlots()[index] == Bits(product.m[index]));
        differs = differs || product.m[index] != swapped.m[index];
    }

    CHECK(differs);
}

TEST(SparksVmEdge_BudgetInsideCallee) {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    const uint32 callee = assembler.Begin("Callee", OneToOne(1, 1));
    assembler.Emit(EncodeABC(Opcode::MovS, 0, 0, 0));
    assembler.Emit(EncodeABC(Opcode::MovS, 0, 0, 0));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    const uint32 caller = Caller(assembler, callee, "Caller");
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    const uint64 argument[] = {1};
    VmLimits limits;
    // 예산이 CALL에서 끝난다: CALL은 실행하지 않고 그 자리의 fault다.
    limits.instructions = 1;
    auto atCall =
        execution.Call(assembler.Linked(), caller, Span<const uint64>(argument, 1), {}, {}, limits);
    REQUIRE(atCall);
    CHECK(atCall.Value().fault.function == caller);
    CHECK(atCall.Value().fault.address == 1);
    CHECK(atCall.Value().steps == 1);
    // callee 안에서 끝난다: 자리는 callee다.
    limits.instructions = 3;
    auto inside =
        execution.Call(assembler.Linked(), caller, Span<const uint64>(argument, 1), {}, {}, limits);
    REQUIRE(inside);
    CHECK(inside.Value().fault.kind == VmFaultKind::BudgetExceeded);
    CHECK(inside.Value().fault.function == callee);
    CHECK(inside.Value().fault.address == 1);
    limits.instructions = 6;
    CHECK(Returns(Run(execution, assembler.Linked(), caller, Span<const uint64>(argument, 1),
                      Span<const uint64>(), limits),
                  1));
}

TEST(SparksVmEdge_ReuseAfterAllocationFailure) {
    // 할당 실패로 끝난 실행 문맥도 다음 Call에서 처음부터 쓸 수 있다(frame·stack이 남지 않는다).
    SystemAllocator backing;
    Assembler assembler(backing);
    const uint32 callee = assembler.Begin("Callee", OneToOne(1, 1));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    const uint32 caller = Caller(assembler, callee, "Caller");
    REQUIRE(Verify(assembler));
    test::NFailAllocator allocator(backing);
    VmExecution execution(allocator);
    const uint64 argument[] = {4};
    allocator.FailOnAttempt(1);
    auto failed =
        execution.Call(assembler.Linked(), caller, Span<const uint64>(argument, 1), {}, {});
    CHECK(!failed);
    allocator.DisableFailure();
    CHECK(Returns(Run(execution, assembler.Linked(), caller, Span<const uint64>(argument, 1)), 4));
}

} // namespace sparks_vm_edge_tests_file
