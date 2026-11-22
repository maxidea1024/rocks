#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include "VmTestSupport.h"

// host 경계 시험이다(Docs/Design/SparksVm.ko.md 6절, LC5d1): 링크(묶기·거부 사유·필드 명령 대조),
// handle 생존(ISSOME.R·EQ.R·NE.R), GETF·SETF(Scalar·Buf — host 보관 버퍼와 ledger), GETCTX, CALLN,
// NEWLIT, host가 돌려준 fault·할당 실패.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_vm_host_tests_file {

using namespace rocks::sparks::vm_test_support;

namespace {

constexpr uint64 HostHandleShift = 32;

// 시험 host다. 객체는 번호 1..이고 generation 1에서 시작한다(죽이면 증가).
class TestHost final : public VmHost {
public:
    struct Entry {
        BytecodeRefKind kind;
        StringView name;
        StringView descriptor;
        uint8 bank;
        uint8 slots;
    };

    static constexpr Entry Entries[] = {
        {BytecodeRefKind::Field, "Thing.Hp", ">i64", 0, 1},
        {BytecodeRefKind::Field, "Thing.Name", ">s", 2, 1},
        {BytecodeRefKind::Field, "Thing.Pos", ">v3", 0, 3},
        {BytecodeRefKind::Context, "World", "", 1, 1},
        {BytecodeRefKind::Context, "Broken", "", 0, 1}, // 잘못된 문맥 배치
        {BytecodeRefKind::NativeFunction, "Thing.Kill", "x>void", 0, 0},
        {BytecodeRefKind::NativeFunction, "Thing.Twice", "x,i64>i64", 0, 1},
        {BytecodeRefKind::NativeFunction, "Thing.Fail", "x>void", 0, 0},
        {BytecodeRefKind::NativeFunction, "Thing.Oom", "x>void", 0, 0},
        {BytecodeRefKind::NativeFunction, "Thing.Label", "x>s", 2, 1},
        {BytecodeRefKind::Literal, "Tag", "", 0, 1},
        {BytecodeRefKind::Literal, "Empty", "", 0, 0}, // 0 slot — 잘못된 값 배치
        {BytecodeRefKind::Literal, "Wide", "", 0, 3},  // Scalar 3 slot 리터럴
    };

    Result<VmHostBinding> Bind(const VmBindRequest& request,
                               StringBuilderBase& reason) noexcept override {
        for (uint32 index = 0; index < sizeof(Entries) / sizeof(Entries[0]); ++index) {
            const Entry& entry = Entries[index];
            if (entry.kind != request.kind || entry.name != request.name) {
                continue;
            }

            if (entry.kind != BytecodeRefKind::Context && entry.kind != BytecodeRefKind::Literal &&
                entry.descriptor != request.descriptor) {
                reason.AppendFormat("descriptor '{}' differs", request.descriptor);
                return Result<VmHostBinding>::Failure(ErrorCode::InvalidArgument);
            }

            VmHostBinding binding;
            binding.id = index;
            binding.bank = entry.bank;
            binding.slots = entry.slots;
            return Result<VmHostBinding>::Success(binding);
        }

        if (request.name == "Silent") {
            return Result<VmHostBinding>::Failure(ErrorCode::NotFound); // 사유 없음
        }

        reason.AppendFormat("unknown '{}'", request.name);
        return Result<VmHostBinding>::Failure(ErrorCode::NotFound);
    }

    bool IsAlive(uint64 handle) noexcept override {
        const uint64 number = handle >> HostHandleShift;
        return number >= 1 && number <= 4 &&
               generations[number - 1] == (handle & NumericLimits<uint32>::Max());
    }

    VmFaultKind GetContext(VmHostCall& call) noexcept override {
        call.Refs()[0] = Handle(1);
        return VmFaultKind::None;
    }

    VmFaultKind GetField(VmHostCall& call, uint64 object) noexcept override {
        const uint64 number = object >> HostHandleShift;
        if (call.Binding() == 1) {
            call.Execution().Retain(names[number - 1]);
            call.PutBuffer(0, names[number - 1]);
            return VmFaultKind::None;
        }

        for (usize slot = 0; slot < call.Scalars().Size(); ++slot) {
            call.Scalars()[slot] = call.Binding() == 0 ? hp[number - 1] : slot + 1;
        }

        return VmFaultKind::None;
    }

    VmFaultKind SetField(VmHostCall& call, uint64 object) noexcept override {
        const uint64 number = object >> HostHandleShift;
        if (call.Binding() == 1) {
            call.Execution().Retain(call.Buffers()[0]);
            call.Execution().Release(names[number - 1]);
            names[number - 1] = call.Buffers()[0];
            return VmFaultKind::None;
        }

        hp[number - 1] = call.Scalars()[0];
        return VmFaultKind::None;
    }

    VmFaultKind CallNative(VmHostCall& call) noexcept override {
        ++calls;
        const uint64 number = call.Refs()[0] >> HostHandleShift;
        switch (call.Binding()) {
        case 5:
            ++generations[number - 1];
            return VmFaultKind::None;
        case 6:
            call.Scalars()[0] = call.Scalars()[0] * 2;
            return VmFaultKind::None;
        case 7:
            return VmFaultKind::ContractViolation;
        case 8:
            call.Fail(ErrorCode::OutOfMemory);
            return VmFaultKind::None;
        default: {
            auto text = call.Execution().NewString("label");
            if (!text) {
                call.Fail(text.Error());
                return VmFaultKind::None;
            }

            call.PutBuffer(0, text.Value());
            return VmFaultKind::None;
        }
        }
    }

    VmFaultKind MakeLiteral(VmHostCall& call) noexcept override {
        for (usize slot = 0; slot < call.Scalars().Size(); ++slot) {
            call.Scalars()[slot] = 77 + slot;
        }

        return VmFaultKind::None;
    }

    uint64 Handle(uint64 number) const noexcept {
        return (number << HostHandleShift) | generations[number - 1];
    }

    void ReleaseAll(VmExecution& execution) noexcept {
        for (VmBuffer*& name : names) {
            execution.Release(name);
            name = nullptr;
        }
    }

    uint32 generations[4] = {1, 1, 1, 1};
    uint64 hp[4] = {};
    VmBuffer* names[4] = {};
    uint32 calls = 0;
};

// Ref·Scalar·Buf 각 4칸(할당 = frame — 창 없음)의 모양이다.
Shape Plain(uint16 refParameters = 1) noexcept {
    Shape shape;
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        shape.registers[bank] = 4;
        shape.frame[bank] = 4;
    }

    shape.parameters[1] = refParameters;
    return shape;
}

// 호출 창(할당 위 4칸)을 둔 모양이다.
Shape Windowed(uint16 refParameters = 1) noexcept {
    Shape shape = Plain(refParameters);
    for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
        shape.frame[bank] = 8;
    }

    return shape;
}

void EmitField(Assembler& assembler, Opcode op, uint32 a, uint32 b, uint32 c, uint32 ref) noexcept {
    assembler.Emit(EncodeABC(op, a, b, c));
    assembler.Emit(EncodeAx(Opcode::Extra, ref));
}

} // namespace

TEST(SparksVmHost_LinkBindsAndRejects) {
    SystemAllocator allocator;
    TestHost host;
    StringBuilder<256> reason(allocator);
    struct Case {
        BytecodeRefKind kind;
        StringView name;
        StringView descriptor;
        StringView expected;
    };
    const Case cases[] = {
        {BytecodeRefKind::NativeFunction, "Thing.Nope", "x>void",
         "ref 0 (native Thing.Nope): unknown 'Thing.Nope'"},
        {BytecodeRefKind::NativeFunction, "Thing.Kill", "x>i64",
         "ref 0 (native Thing.Kill): descriptor 'x>i64' differs"},
        {BytecodeRefKind::Context, "Broken", "",
         "ref 0 (context Broken): a context value must be one Ref slot"},
        {BytecodeRefKind::LanguageFunction, "Math.Nope", "i64>i64",
         "ref 0 (language Math.Nope): unknown language function or descriptor shape"},
        {BytecodeRefKind::LanguageFunction, "Math.Min", "i64,i64",
         "ref 0 (language Math.Min): invalid language function descriptor"},
        // 모양: 이름은 맞아도 서술자가 그 함수의 모양이 아니면 거부한다(리뷰 A1).
        {BytecodeRefKind::LanguageFunction, "Math.Min", "s,s>s",
         "ref 0 (language Math.Min): unknown language function or descriptor shape"},
        {BytecodeRefKind::LanguageFunction, "Math.Scale", "m,m>m",
         "ref 0 (language Math.Scale): unknown language function or descriptor shape"},
        {BytecodeRefKind::LanguageFunction, "Math.Min", "i64,i32>i64",
         "ref 0 (language Math.Min): unknown language function or descriptor shape"},
        {BytecodeRefKind::LanguageFunction, "Array.Add", "A<i64>,s>void",
         "ref 0 (language Array.Add): unknown language function or descriptor shape"},
        {BytecodeRefKind::LanguageFunction, "Array.Sort", "A<v3>>void",
         "ref 0 (language Array.Sort): unknown language function or descriptor shape"},
        // slot 상한: Scalar 32 넘음·Ref·Buf 2 slot·0 slot·Optional 묶음 33(리뷰 A1).
        {BytecodeRefKind::LanguageFunction, "Array.Add", "A<zs40>,zs40>void",
         "ref 0 (language Array.Add): invalid language function descriptor"},
        {BytecodeRefKind::LanguageFunction, "Array.Add", "A<zb2>,zb2>void",
         "ref 0 (language Array.Add): invalid language function descriptor"},
        {BytecodeRefKind::LanguageFunction, "Array.Add", "A<zs0>,zs0>void",
         "ref 0 (language Array.Add): invalid language function descriptor"},
        {BytecodeRefKind::LanguageFunction, "Result.Value", "R<zs32>>?<zs32>",
         "ref 0 (language Result.Value): invalid language function descriptor"},
        {BytecodeRefKind::LanguageFunction, "Math.Min", "i64,i64>i64x",
         "ref 0 (language Math.Min): invalid language function descriptor"},
        {BytecodeRefKind::LanguageFunction, "Math.Min", "",
         "ref 0 (language Math.Min): invalid language function descriptor"},
        // 표지 5비트 slot 필드 최댓값(31)을 넘는 원소(LC5d1 리뷰 B1).
        {BytecodeRefKind::LanguageFunction, "Array.Add", "A<zs32>,zs32>void",
         "ref 0 (language Array.Add): invalid language function descriptor"},
        // 같은 자리표시는 같은 타입이어야 한다, 컨테이너 글자는 자리표시가 아니다.
        {BytecodeRefKind::LanguageFunction, "Set.Add", "A<s>,s>b",
         "ref 0 (language Set.Add): unknown language function or descriptor shape"},
        {BytecodeRefKind::LanguageFunction, "Map.Keys", "M<s,i64>>A<i64>",
         "ref 0 (language Map.Keys): unknown language function or descriptor shape"},
        // 대기·host 언어 함수는 정확한 이름만 미룬다(오타는 거부).
        {BytecodeRefKind::LanguageFunction, "Time.Deltaa", ">f64",
         "ref 0 (language Time.Deltaa): unknown language function or descriptor shape"},
        // host 거부: 사유가 없으면 일반 사유, 값 배치가 틀리면 거부.
        {BytecodeRefKind::Context, "Silent", "", "ref 0 (context Silent): rejected by the host"},
        {BytecodeRefKind::Literal, "Empty", "",
         "ref 0 (literal Empty): the host returned an invalid value layout"},
    };
    for (const Case& entry : cases) {
        Assembler assembler(allocator);
        BytecodeRef ref;
        ref.kind = entry.kind;
        ref.name = assembler.String(entry.name);
        ref.arguments =
            entry.descriptor.IsEmpty() ? NoBytecodeIndex : assembler.String(entry.descriptor);
        ref.detail =
            entry.kind == BytecodeRefKind::Literal ? assembler.String("x") : NoBytecodeIndex;
        REQUIRE(assembler.module.refs.TryPushBack(ref));
        VmLinkedModule linked(allocator);
        reason.Clear();
        auto link = LinkModule(assembler.module, &host, linked, reason);
        REQUIRE(!link);
        CHECK(link.Error() == ErrorCode::InvalidArgument);
        CHECK(reason.View() == entry.expected);
    }

    {
        // 대기 언어 함수는 링크하되(LC6) 부르면 ContractViolation이다. 창에 들어가지 않는 언어
        // 호출은 링크가 거부한다(리뷰 A2).
        Assembler assembler(allocator);
        BytecodeRef tick;
        tick.kind = BytecodeRefKind::LanguageFunction;
        tick.name = assembler.String("NextTick");
        tick.arguments = assembler.String(">w");
        REQUIRE(assembler.module.refs.TryPushBack(tick));
        BytecodeRef concat;
        concat.kind = BytecodeRefKind::LanguageFunction;
        concat.name = assembler.String("string.Slice");
        concat.arguments = assembler.String("s,i64,i64>s");
        REQUIRE(assembler.module.refs.TryPushBack(concat));
        Shape shape;
        shape.registers[1] = 1;
        shape.frame[1] = 2;
        assembler.Begin("Tick", shape);
        assembler.Emit(EncodeABx(Opcode::CallL, 0, 0));
        assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
        assembler.End();
        REQUIRE(assembler.Verified());
        VmExecution execution(allocator);
        CHECK(Faults(Run(execution, assembler.Linked(), 0, {}), VmFaultKind::ContractViolation));

        Shape narrow; // Scalar 창 1칸 — Slice(i64 둘)는 들어가지 않는다
        narrow.frame[0] = 1;
        narrow.frame[2] = 1;
        assembler.Begin("Narrow", narrow);
        assembler.Emit(EncodeABx(Opcode::CallL, 0, 1));
        assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
        assembler.End();
        REQUIRE(assembler.Verified());
        VmLinkedModule linked(allocator);
        reason.Clear();
        CHECK(!LinkModule(assembler.module, nullptr, linked, reason));
        CHECK(reason.View() == "function Narrow word 0: the language call does not fit the call "
                               "window");
    }

    {
        // host 없이는 host 참조를 묶지 못한다.
        Assembler assembler(allocator);
        assembler.Ref(BytecodeRefKind::Context, "World");
        VmLinkedModule linked(allocator);
        reason.Clear();
        CHECK(!LinkModule(assembler.module, nullptr, linked, reason));
        CHECK(reason.View() == "ref 0 (context World): no host to bind this reference");
    }

    {
        // 필드 명령의 slot 수(C)가 묶은 필드와 다르면 거부한다.
        Assembler assembler(allocator);
        BytecodeRef field;
        field.kind = BytecodeRefKind::Field;
        field.name = assembler.String("Thing.Hp");
        field.arguments = assembler.String(">i64");
        REQUIRE(assembler.module.refs.TryPushBack(field));
        assembler.Begin("Read", Plain());
        EmitField(assembler, Opcode::GetFS, 0, 0, 2, 0);
        assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
        assembler.End();
        REQUIRE(assembler.Verified());
        VmLinkedModule linked(allocator);
        reason.Clear();
        CHECK(!LinkModule(assembler.module, &host, linked, reason));
        CHECK(reason.View() ==
              "function Read word 0: the field instruction does not match the bound field "
              "'Thing.Hp'");
    }
}

TEST(SparksVmHost_HandlesFieldsAndNatives) {
    SystemAllocator allocator;
    TestHost host;
    Assembler assembler(allocator);
    auto field = [&](StringView name, StringView descriptor) noexcept {
        BytecodeRef ref;
        ref.kind = BytecodeRefKind::Field;
        ref.name = assembler.String(name);
        ref.arguments = assembler.String(descriptor);
        CHECK(assembler.module.refs.TryPushBack(ref));
        return static_cast<uint32>(assembler.module.refs.Size() - 1);
    };
    auto native = [&](StringView name, StringView descriptor) noexcept {
        BytecodeRef ref;
        ref.kind = BytecodeRefKind::NativeFunction;
        ref.name = assembler.String(name);
        ref.arguments = assembler.String(descriptor);
        CHECK(assembler.module.refs.TryPushBack(ref));
        return static_cast<uint32>(assembler.module.refs.Size() - 1);
    };
    const uint32 hp = field("Thing.Hp", ">i64");
    const uint32 name = field("Thing.Name", ">s");
    const uint32 pos = field("Thing.Pos", ">v3");
    const uint32 world = assembler.Ref(BytecodeRefKind::Context, "World");
    const uint32 kill = native("Thing.Kill", "x>void");
    const uint32 twice = native("Thing.Twice", "x,i64>i64");
    const uint32 fail = native("Thing.Fail", "x>void");
    const uint32 oom = native("Thing.Oom", "x>void");
    const uint32 label = native("Thing.Label", "x>s");
    const uint32 tag = assembler.Ref(BytecodeRefKind::Literal, "Tag", assembler.String("Game.A"));

    // 0 Alive(r0, r1): s0 = ISSOME r0, s1 = r0 == r1, s2 = r0 != r1 → s0*4 + s1*2 + s2.
    Shape two = Plain(2);
    two.returnSlots = 1;
    assembler.Begin("Alive", two);
    assembler.Emit(EncodeABC(Opcode::IsSomeR, 0, 0, 0));
    assembler.Emit(EncodeABC(Opcode::EqR, 1, 0, 1));
    assembler.Emit(EncodeABC(Opcode::NeR, 2, 0, 1));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 3, 2));
    assembler.Emit(EncodeABC(Opcode::MulI64, 1, 1, 3));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 3, 4));
    assembler.Emit(EncodeABC(Opcode::MulI64, 0, 0, 3));
    assembler.Emit(EncodeABC(Opcode::AddI64, 0, 0, 1));
    assembler.Emit(EncodeABC(Opcode::AddI64, 0, 0, 2));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    // 1 Fields(r0): Hp = Hp + Pos.Y, Name = Name(그대로 다시 씀) → Hp.
    Shape one = Plain();
    one.returnSlots = 1;
    assembler.Begin("Fields", one);
    EmitField(assembler, Opcode::GetFS, 0, 0, 1, hp);
    EmitField(assembler, Opcode::GetFS, 1, 0, 3, pos);
    assembler.Emit(EncodeABC(Opcode::AddI64, 0, 0, 2));
    EmitField(assembler, Opcode::SetFS, 0, 0, 1, hp);
    EmitField(assembler, Opcode::GetFB, 0, 0, 1, name);
    EmitField(assembler, Opcode::SetFB, 0, 0, 1, name);
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    // 2 Native(r0): Twice(r0, 21) → s0, Label(r0) → b0 버림, World의 Hp, Tag 리터럴 → s0 + s1 + s2.
    Shape window = Windowed();
    window.returnSlots = 1;
    assembler.Begin("Native", window);
    assembler.Emit(EncodeABC(Opcode::MovR, 4, 0, 0));
    assembler.Emit(EncodeAsBx(Opcode::LoadI, 4, 21));
    assembler.Emit(EncodeABx(Opcode::CallN, 0, twice));
    assembler.Emit(EncodeABC(Opcode::MovS, 0, 4, 0));
    assembler.Emit(EncodeABx(Opcode::CallN, 0, label));
    assembler.Emit(EncodeABx(Opcode::GetCtx, 1, world));
    EmitField(assembler, Opcode::GetFS, 1, 1, 1, hp);
    assembler.Emit(EncodeABC(Opcode::NewLit, 2, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, tag));
    assembler.Emit(EncodeABC(Opcode::AddI64, 0, 0, 1));
    assembler.Emit(EncodeABC(Opcode::AddI64, 0, 0, 2));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
    assembler.End();
    // 3 Kill(r0): Kill(r0) 뒤 Hp 읽기(죽은 객체 — ContractViolation).
    assembler.Begin("Kill", Windowed());
    assembler.Emit(EncodeABC(Opcode::MovR, 4, 0, 0));
    assembler.Emit(EncodeABx(Opcode::CallN, 0, kill));
    EmitField(assembler, Opcode::GetFS, 0, 0, 1, hp);
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    // 4 Fail(r0)·5 Oom(r0): host가 fault·할당 실패를 돌려준다.
    assembler.Begin("Fail", Windowed());
    assembler.Emit(EncodeABC(Opcode::MovR, 4, 0, 0));
    assembler.Emit(EncodeABx(Opcode::CallN, 0, fail));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    assembler.Begin("Oom", Windowed());
    assembler.Emit(EncodeABC(Opcode::MovR, 4, 0, 0));
    assembler.Emit(EncodeABx(Opcode::CallN, 0, oom));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    REQUIRE(assembler.Verified());
    const VmLinkedModule& linked = assembler.Linked(&host);
    CHECK(linked.Host() == &host);
    CHECK(linked.HostBinding(world).bank == 1);

    VmExecution execution(allocator);
    const uint64 first = host.Handle(1);
    const uint64 second = host.Handle(2);
    // 살아 있는 서로 다른 둘: 있음, 다름.
    uint64 pair[] = {first, second};
    CHECK(Returns(Run(execution, linked, 0, {}, Span<const uint64>(pair, 2)), 4 + 1));
    // 같은 것.
    pair[1] = first;
    CHECK(Returns(Run(execution, linked, 0, {}, Span<const uint64>(pair, 2)), 4 + 2));
    // 죽은 handle은 null과 같다: 죽은 것과 0은 같다(없음, 같음).
    const uint64 stale = (uint64(3) << HostHandleShift) | 9;
    pair[0] = stale;
    pair[1] = 0;
    CHECK(Returns(Run(execution, linked, 0, {}, Span<const uint64>(pair, 2)), 2));

    // 필드: Hp(0) + Pos.Y(2) = 2를 쓰고 돌려준다. 이름 버퍼는 host가 보관한다.
    auto text = execution.NewString("door");
    REQUIRE(text);
    host.names[0] = text.Value();
    CHECK(Returns(Run(execution, linked, 1, {}, Span<const uint64>(&first, 1)), 2));
    CHECK(host.hp[0] == 2);
    CHECK(host.names[0] == text.Value());
    CHECK(text.Value()->references == 1);
    CHECK(execution.LiveBuffers() == 1);

    // native·문맥·리터럴: Twice(21) = 42, World(#1).Hp = 2, Tag = 77.
    CHECK(Returns(Run(execution, linked, 2, {}, Span<const uint64>(&first, 1)), 42 + 2 + 77));
    CHECK(execution.LiveBuffers() == 1); // Label의 버퍼는 frame과 함께 놓였다

    // 죽인 뒤 필드 읽기는 ContractViolation이고, host fault는 그 명령 자리다.
    CHECK(Faults(Run(execution, linked, 3, {}, Span<const uint64>(&second, 1)),
                 VmFaultKind::ContractViolation));
    CHECK(!host.IsAlive(second));
    CHECK(Faults(Run(execution, linked, 4, {}, Span<const uint64>(&first, 1)),
                 VmFaultKind::ContractViolation));
    auto failed = execution.Call(linked, 5, {}, Span<const uint64>(&first, 1), {});
    REQUIRE(!failed);
    CHECK(failed.Error() == ErrorCode::OutOfMemory);
    host.ReleaseAll(execution);
    CHECK(execution.LiveBuffers() == 0);
    CHECK(execution.LiveBytes() == 0);
}

TEST(SparksVmHost_LanguageCallsCheckBuffers) {
    // 언어 함수는 수신자 버퍼의 종류·배치를 서술자와 대조한다(리뷰 A3): host가 다른 배치의
    // 컨테이너나 다른 종류의 버퍼를 넘기면 ContractViolation이고 버퍼 밖을 쓰지 않는다. 맞는
    // 컨테이너에 Add를 거듭해도 수신자는 공유되지 않아 복제하지 않는다(리뷰 A5 — 창으로 옮김).
    SystemAllocator allocator;
    Assembler assembler(allocator);
    BytecodeRef add;
    add.kind = BytecodeRefKind::LanguageFunction;
    add.name = assembler.String("Array.Add");
    add.arguments = assembler.String("A<v3>,v3>void");
    REQUIRE(assembler.module.refs.TryPushBack(add));
    Shape shape;
    shape.registers[0] = 3;
    shape.frame[0] = 6;
    shape.parameters[0] = 3;
    shape.registers[2] = 1;
    shape.frame[2] = 2;
    shape.parameters[2] = 1;
    shape.returnBank = 2;
    shape.returnSlots = 1;
    assembler.Begin("Add", shape);
    for (uint32 slot = 0; slot < 3; ++slot) {
        assembler.Emit(EncodeABC(Opcode::MovS, 3 + slot, slot, 0));
    }

    // 컴파일러와 같이 수신자를 창으로 옮긴다(원래 레지스터를 비운다 — SparksBytecode 4.3절).
    assembler.Emit(EncodeABC(Opcode::MovB, 1, 0, 0));
    assembler.Emit(EncodeABC(Opcode::LoadNullB, 0, 0, 0));
    assembler.Emit(EncodeABx(Opcode::CallL, 0, 0));
    assembler.Emit(EncodeABC(Opcode::MovB, 0, 1, 0));
    assembler.Emit(EncodeABC(Opcode::LoadNullB, 1, 0, 0));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 2));
    assembler.End();
    REQUIRE(Verify(assembler));
    VmExecution execution(allocator);
    const uint64 vector[] = {Bits(1.0), Bits(2.0), Bits(3.0)};
    auto call = [&](VmBuffer* receiver) noexcept {
        VmBuffer* const buffers[] = {receiver};
        return execution.Call(assembler.Linked(), 0, Span<const uint64>(vector, 3), {},
                              Span<VmBuffer* const>(buffers, 1));
    };

    // 배치가 다른 Array(Scalar 1 slot 원소)와 문자열 버퍼.
    const uint64 one[] = {7};
    auto narrow = execution.NewContainer(VmBufferKind::Array, EncodeContainerMarker(0, 1, 0, 0),
                                         Span<const uint64>(one, 1));
    REQUIRE(narrow);
    auto text = execution.NewString("not an array");
    REQUIRE(text);
    VmBuffer* const wrong[] = {narrow.Value(), text.Value()};
    for (VmBuffer* receiver : wrong) {
        auto called = call(receiver);
        REQUIRE(called);
        CHECK(called.Value().outcome == VmOutcome::Faulted);
        CHECK(called.Value().fault.kind == VmFaultKind::ContractViolation);
    }

    CHECK(narrow.Value()->count == 1);
    execution.Release(narrow.Value());
    execution.Release(text.Value());

    // 맞는 배치: 돌려받은 Array를 다시 넘기며 Add를 거듭한다. 반환 하나만 참조를 쥔다.
    auto empty = execution.NewContainer(VmBufferKind::Array, EncodeContainerMarker(0, 3, 0, 0), {});
    REQUIRE(empty);
    VmBuffer* current = empty.Value();
    REQUIRE(call(current));
    execution.Release(current);
    for (uint32 round = 0; round < 20; ++round) {
        VmBuffer* const again[] = {execution.ReturnedBuffers()[0]};
        REQUIRE(execution.Call(assembler.Linked(), 0, Span<const uint64>(vector, 3), {},
                               Span<VmBuffer* const>(again, 1)));
    }

    const VmBuffer* last = execution.ReturnedBuffers()[0];
    CHECK(last->count == 21);
    CHECK(last->references == 1);
    CHECK(execution.LiveBuffers() == 1);
    execution.ReleaseReturned();
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmHost_LinkChecksWindowsAndState) {
    SystemAllocator allocator;
    TestHost host;
    StringBuilder<256> reason(allocator);
    // NEWLIT: 결과 bank가 묶음과 다르거나, 결과 묶음이 frame 밖이거나, 이름 지정 인자가 있는데 창이
    // 결과를 담지 못하면 거부한다(LC5d1 리뷰 B3).
    struct LiteralCase {
        StringView name;
        uint32 bank;
        uint32 a;
        uint32 count;
        bool accepted;
    };
    const LiteralCase literals[] = {{"Wide", 0, 0, 0, true},
                                    {"Wide", 0, 2, 0, false},
                                    {"Wide", 1, 0, 0, false},
                                    {"Wide", 0, 0, 1, false},
                                    {"Tag", 0, 3, 0, true}};
    for (const LiteralCase& entry : literals) {
        Assembler assembler(allocator);
        const uint32 literal =
            assembler.Ref(BytecodeRefKind::Literal, entry.name, assembler.String("x"));
        Shape shape;
        shape.registers[0] = 4;
        shape.frame[0] = 4; // 창 없음
        shape.registers[1] = 4;
        shape.frame[1] = 4;
        assembler.Begin("Lit", shape);
        assembler.Emit(EncodeABC(Opcode::NewLit, entry.a, 0, 0));
        assembler.Emit(EncodeAx(Opcode::Extra, literal | (entry.count << 16) | (entry.bank << 22)));
        assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
        assembler.End();
        if (!assembler.Verified()) {
            CHECK(!entry.accepted); // verifier가 먼저 막은 모양(인자 수와 창)
            continue;
        }

        VmLinkedModule linked(allocator);
        reason.Clear();
        const bool ok = static_cast<bool>(LinkModule(assembler.module, &host, linked, reason));
        CHECK(ok == entry.accepted);
        if (!ok) {
            CHECK(reason.View() == "function Lit word 0: the literal result does not fit the bound "
                                   "literal");
        }
    }

    {
        // 여러 slot 리터럴은 레지스터 A부터 받는다(창 없음).
        Assembler assembler(allocator);
        const uint32 literal =
            assembler.Ref(BytecodeRefKind::Literal, "Wide", assembler.String("x"));
        Shape shape;
        shape.registers[0] = 4;
        shape.frame[0] = 4;
        shape.returnSlots = 1;
        assembler.Begin("Lit", shape);
        assembler.Emit(EncodeABC(Opcode::NewLit, 1, 0, 0));
        assembler.Emit(EncodeAx(Opcode::Extra, literal));
        assembler.Emit(EncodeABC(Opcode::AddI64, 0, 1, 3));
        assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
        assembler.End();
        REQUIRE(assembler.Verified());
        VmExecution execution(allocator);
        CHECK(Returns(Run(execution, assembler.Linked(&host), 0, {}), 77 + 79));
    }

    {
        // CALLN: 서술자의 결과가 창에 들어가지 않으면 거부한다(LC5d1 리뷰 B4).
        Assembler assembler(allocator);
        BytecodeRef twice;
        twice.kind = BytecodeRefKind::NativeFunction;
        twice.name = assembler.String("Thing.Twice");
        twice.arguments = assembler.String("x,i64>i64");
        REQUIRE(assembler.module.refs.TryPushBack(twice));
        Shape shape = Plain(); // frame == registers — 창 없음
        assembler.Begin("Call", shape);
        assembler.Emit(EncodeABx(Opcode::CallN, 0, 0));
        assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
        assembler.End();
        REQUIRE(assembler.Verified());
        VmLinkedModule linked(allocator);
        reason.Clear();
        CHECK(!LinkModule(assembler.module, &host, linked, reason));
        CHECK(reason.View() ==
              "function Call word 0: the native call does not fit the call window");
        // 실패한 링크·링크하지 않은 객체는 Call이 거부한다.
        VmExecution execution(allocator);
        CHECK(!linked.Linked());
        CHECK(execution.Call(linked, 0, {}, {}, {}).Error() == ErrorCode::InvalidArgument);
        VmLinkedModule never(allocator);
        CHECK(execution.Call(never, 0, {}, {}, {}).Error() == ErrorCode::InvalidArgument);
    }

    {
        // host 없이 링크한 모듈에서 handle은 모두 없음으로 읽힌다(6.2절).
        Assembler assembler(allocator);
        Shape shape = Plain(2);
        shape.returnSlots = 1;
        assembler.Begin("Eq", shape);
        assembler.Emit(EncodeABC(Opcode::EqR, 0, 0, 1));
        assembler.Emit(EncodeABC(Opcode::Ret, 0, 1, 0));
        assembler.End();
        REQUIRE(Verify(assembler));
        VmExecution execution(allocator);
        const uint64 handles[] = {(uint64(1) << HostHandleShift) | 1,
                                  (uint64(2) << HostHandleShift) | 1};
        CHECK(
            Returns(Run(execution, assembler.Linked(), 0, {}, Span<const uint64>(handles, 2)), 1));
    }
}

TEST(SparksVmHost_HostileResultsAndPutBuffer) {
    // 다른 bank 값으로 만든 Buf Result를 `R<s>`·TRY.B(Buf 1 slot)로 읽으면 ContractViolation이다
    // (LC5d1 리뷰 B2 — handle 비트를 포인터로 읽지 않는다).
    SystemAllocator allocator;
    Assembler assembler(allocator);
    BytecodeRef value;
    value.kind = BytecodeRefKind::LanguageFunction;
    value.name = assembler.String("Result.Value");
    value.arguments = assembler.String("R<s>>?<s>");
    REQUIRE(assembler.module.refs.TryPushBack(value));
    // 0 Value(r0): OK.R로 Ref 값 Result를 만들어 Result.Value(R<s>)에 넘긴다.
    Shape shape;
    shape.registers[1] = 1;
    shape.frame[1] = 1;
    shape.parameters[1] = 1;
    shape.registers[2] = 1;
    shape.frame[2] = 2;
    assembler.Begin("Value", shape);
    assembler.Emit(EncodeABC(Opcode::Ok, 1, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, (1u << 8) | 1u));
    assembler.Emit(EncodeABx(Opcode::CallL, 0, 0));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    // 1 Try(r0): 같은 Result를 TRY.B(Buf 값 1 slot)로 꺼낸다.
    Shape tryShape = shape;
    assembler.Begin("Try", tryShape);
    assembler.Emit(EncodeABC(Opcode::Ok, 1, 0, 0));
    assembler.Emit(EncodeAx(Opcode::Extra, (1u << 8) | 1u));
    assembler.Emit(EncodeABC(Opcode::TryB, 0, 1, (2u << 5) | 1u));
    assembler.Emit(EncodeAx(Opcode::Extra, 1));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.Emit(EncodeABC(Opcode::Ret, 0, 0, 0));
    assembler.End();
    REQUIRE(assembler.Verified());

    VmExecution execution(allocator);
    const uint64 handle = (uint64(1) << HostHandleShift) | 1;
    CHECK(Faults(Run(execution, assembler.Linked(), 0, {}, Span<const uint64>(&handle, 1)),
                 VmFaultKind::ContractViolation));
    CHECK(Faults(Run(execution, assembler.Linked(), 1, {}, Span<const uint64>(&handle, 1)),
                 VmFaultKind::ContractViolation));
    CHECK(execution.LiveBuffers() == 0);
}

TEST(SparksVmHost_LinkAllocationFailure) {
    // 링크의 모든 할당 실패는 OutOfMemory이고, 실패한 링크로는 부를 수 없다.
    SystemAllocator backing;
    TestHost host;
    Assembler assembler(backing);
    BytecodeRef min;
    min.kind = BytecodeRefKind::LanguageFunction;
    min.name = assembler.String("Math.Min");
    min.arguments = assembler.String("i64,i64>i64");
    REQUIRE(assembler.module.refs.TryPushBack(min));
    assembler.Ref(BytecodeRefKind::Context, "World");
    uint64 attempts = 0;
    uint64 failures = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            VmLinkedModule linked(allocator);
            StringBuilder<16> reason(allocator); // 작은 inline — 사유 쓰기도 할당한다
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            auto link = LinkModule(assembler.module, &host, linked, reason);
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                REQUIRE(link);
                attempts = used;
            } else if (!link) {
                CHECK(link.Error() == ErrorCode::OutOfMemory);
                CHECK(!linked.Linked());
                ++failures;
            }
        }
    }

    CHECK(failures > 0);
}

} // namespace sparks_vm_host_tests_file
