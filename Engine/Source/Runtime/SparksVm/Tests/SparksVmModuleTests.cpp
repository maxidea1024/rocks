#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

#include "VmTestSupport.h"

// 모듈 사이 호출 시험이다(Docs/Design/SparksVm.ko.md 6.5절, LC5d2): LinkImports(순환 import·거부
// 사유·모양 대조), 풀기 전 Call 거부, frame마다 모듈(다른 모듈 함수의 상수·fault 위치), 창 부족.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_vm_module_tests_file {

using namespace rocks::sparks::vm_test_support;

namespace {

// name의 ImportedFunction 참조(서술자 descriptor)를 더한다.
uint32 Import(Assembler& assembler, StringView name, StringView descriptor) noexcept {
    BytecodeRef ref;
    ref.kind = BytecodeRefKind::ImportedFunction;
    ref.name = assembler.String(name);
    ref.arguments = assembler.String(descriptor);
    CHECK(assembler.module.refs.TryPushBack(ref));
    return static_cast<uint32>(assembler.module.refs.Size() - 1);
}

// Scalar 1개를 받아 1개를 돌려주는 함수 모양(창 2칸)이다.
Shape Unary() noexcept {
    Shape shape;
    shape.registers[0] = 2;
    shape.frame[0] = 4;
    shape.parameters[0] = 1;
    shape.returnSlots = 1;
    return shape;
}

} // namespace

TEST(SparksVmModule_ImportsAcrossModules) {
    SystemAllocator allocator;
    // Lib(경로 Game/Lib): Twice(x) = Main:Base(x) * 2 — Main을 다시 부른다(순환 import).
    Assembler lib(allocator);
    lib.module.path = lib.String("Game/Lib");
    const uint32 base = Import(lib, "Main:Base", "i64>i64");
    lib.Begin("Twice", Unary());
    lib.Emit(EncodeABC(Opcode::MovS, 2, 0, 0));
    lib.Emit(EncodeABx(Opcode::Call, 0, base));
    lib.Emit(EncodeAsBx(Opcode::LoadI, 1, 2));
    lib.Emit(EncodeABC(Opcode::MulI64, 0, 2, 1));
    lib.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    lib.End();
    // Lib.Overflow(x) = x * 큰 수(IntegerOverflow — 위치는 Lib의 원문 위치).
    lib.Begin("Overflow", Unary());
    lib.Emit(EncodeABx(Opcode::LoadK, 1, lib.Constant(uint64(1) << 62)));
    lib.Emit(EncodeABC(Opcode::MulI64, 0, 0, 1));
    lib.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    lib.End();
    REQUIRE(lib.Verified());

    // Main: Base(x) = x + 1, Run(x) = Game/Lib:Twice(x), Boom(x) = Game/Lib:Overflow(x).
    Assembler main(allocator);
    const uint32 twice = Import(main, "Game/Lib:Twice", "i64>i64");
    const uint32 overflow = Import(main, "Game/Lib:Overflow", "i64>i64");
    main.Begin("Base", Unary());
    main.Emit(EncodeAsBx(Opcode::LoadI, 1, 1));
    main.Emit(EncodeABC(Opcode::AddI64, 0, 0, 1));
    main.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    main.End();
    main.Begin("Run", Unary());
    main.Emit(EncodeABC(Opcode::MovS, 2, 0, 0));
    main.Emit(EncodeABx(Opcode::Call, 0, twice));
    main.Emit(EncodeABC(Opcode::Ret, 2, 1, 0));
    main.End();
    main.Begin("Boom", Unary());
    main.Emit(EncodeABC(Opcode::MovS, 2, 0, 0));
    main.Emit(EncodeABx(Opcode::Call, 0, overflow));
    main.Emit(EncodeABC(Opcode::Ret, 2, 1, 0));
    main.End();
    REQUIRE(main.Verified());

    StringBuilder<256> reason(allocator);
    VmLinkedModule linkedLib(allocator);
    VmLinkedModule linkedMain(allocator);
    REQUIRE(LinkModule(lib.module, nullptr, linkedLib, reason));
    REQUIRE(LinkModule(main.module, nullptr, linkedMain, reason));
    CHECK(!linkedMain.ImportsLinked());
    VmExecution execution(allocator);
    const uint64 five[] = {5};
    // 풀기 전에는 부르지 않는다.
    CHECK(execution.Call(linkedMain, 1, Span<const uint64>(five, 1), {}, {}).Error() ==
          ErrorCode::InvalidArgument);
    VmLinkedModule* const both[] = {&linkedMain, &linkedLib};
    REQUIRE(LinkImports(Span<VmLinkedModule* const>(both, 2), reason));
    CHECK(linkedMain.ImportsLinked());
    CHECK(linkedLib.ImportsLinked());
    // Run(5) = Twice(5) = Base(5) * 2 = 12.
    CHECK(Returns(Run(execution, linkedMain, 1, Span<const uint64>(five, 1)), 12));
    // 다른 모듈 함수의 fault는 그 모듈의 함수·위치다.
    auto boom = execution.Call(linkedMain, 2, Span<const uint64>(five, 1), {}, {});
    REQUIRE(boom);
    CHECK(boom.Value().fault.kind == VmFaultKind::IntegerOverflow);
    CHECK(boom.Value().fault.function == 1);
    CHECK(boom.Value().fault.location.line == lib.module.functions[1].firstWord + 2);
    // 호출 뒤 실행 문맥은 다시 Main을 부를 수 있다.
    CHECK(Returns(Run(execution, linkedMain, 0, Span<const uint64>(five, 1)), 6));
}

TEST(SparksVmModule_LinkImportsRejects) {
    SystemAllocator allocator;
    Assembler lib(allocator);
    lib.module.path = lib.String("Game/Lib");
    lib.Begin("Twice", Unary());
    lib.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    lib.End();
    REQUIRE(lib.Verified());
    struct Case {
        StringView name;
        StringView descriptor;
        StringView expected;
    };
    const Case cases[] = {
        {"Twice", "i64>i64", "ref 0 (import Twice): an import name needs '<path>:'"},
        {"Game/Nope:Twice", "i64>i64",
         "ref 0 (import Game/Nope:Twice): no linked module has this path"},
        {"Game/Lib:Thrice", "i64>i64",
         "ref 0 (import Game/Lib:Thrice): the module has no such function"},
        {"Game/Lib:Twice", "i64,i64>i64",
         "ref 0 (import Game/Lib:Twice): the descriptor does not match the function"},
        {"Game/Lib:Twice", "i64>s",
         "ref 0 (import Game/Lib:Twice): the descriptor does not match the function"},
        {"Game/Lib:Twice", "i64>i64>", "ref 0 (import Game/Lib:Twice): invalid import descriptor"},
    };
    StringBuilder<256> reason(allocator);
    for (const Case& entry : cases) {
        Assembler main(allocator);
        Import(main, entry.name, entry.descriptor);
        VmLinkedModule linkedLib(allocator);
        VmLinkedModule linkedMain(allocator);
        REQUIRE(LinkModule(lib.module, nullptr, linkedLib, reason));
        REQUIRE(LinkModule(main.module, nullptr, linkedMain, reason));
        VmLinkedModule* const both[] = {&linkedMain, &linkedLib};
        reason.Clear();
        auto linked = LinkImports(Span<VmLinkedModule* const>(both, 2), reason);
        REQUIRE(!linked);
        CHECK(linked.Error() == ErrorCode::InvalidArgument);
        CHECK(reason.View() == entry.expected);
        CHECK(!linkedMain.ImportsLinked());
    }
}

TEST(SparksVmModule_ImportWindowTooSmall) {
    // 다른 모듈 함수의 매개변수가 부르는 쪽 창에 들어가지 않으면 ContractViolation이다(verifier는
    // 다른 모듈 함수 표를 모른다).
    SystemAllocator allocator;
    Assembler lib(allocator);
    lib.module.path = lib.String("Game/Lib");
    Shape wide;
    wide.registers[0] = 3;
    wide.frame[0] = 3;
    wide.parameters[0] = 3;
    wide.returnSlots = 1;
    lib.Begin("Sum", wide);
    lib.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    lib.End();
    REQUIRE(lib.Verified());
    Assembler main(allocator);
    const uint32 sum = Import(main, "Game/Lib:Sum", "i64,i64,i64>i64");
    Shape narrow;
    narrow.registers[0] = 1;
    narrow.frame[0] = 2; // 창 1칸
    narrow.returnSlots = 1;
    main.Begin("Call", narrow);
    main.Emit(EncodeABx(Opcode::Call, 0, sum));
    main.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    main.End();
    REQUIRE(main.Verified());
    StringBuilder<256> reason(allocator);
    VmLinkedModule linkedLib(allocator);
    VmLinkedModule linkedMain(allocator);
    REQUIRE(LinkModule(lib.module, nullptr, linkedLib, reason));
    REQUIRE(LinkModule(main.module, nullptr, linkedMain, reason));
    VmLinkedModule* const both[] = {&linkedMain, &linkedLib};
    REQUIRE(LinkImports(Span<VmLinkedModule* const>(both, 2), reason));
    VmExecution execution(allocator);
    CHECK(Faults(Run(execution, linkedMain, 0, {}), VmFaultKind::ContractViolation));
}

} // namespace sparks_vm_module_tests_file
