#pragma once

#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <Platform/Memory.h>
#include <SparksVm/BytecodeFile.h>
#include <SparksVm/Opcode.h>
#include <SparksVm/Vm.h>
#include <TestSupport/Test.h>

// VM 시험의 손 조립 도구다(SparksVmTests.cpp·SparksVmEdgeTests.cpp·SparksVmStringTests.cpp가 쓴다).
// 조립한 모듈은 verifier를 통과해야 실행한다.

namespace rocks::sparks::vm_test_support {

// 함수 하나의 frame 모양이다(bank별 Scalar·Ref·Buf).
struct Shape {
    uint16 registers[BytecodeBankCount] = {};
    uint16 frame[BytecodeBankCount] = {};
    uint16 parameters[BytecodeBankCount] = {};
    uint8 returnBank = 0;
    uint8 returnSlots = 0;
};

// 손 조립기다. 명령 단어마다 원문 위치(줄 = 모듈 단어 번호 + 1, 열 1)를 둔다.
class Assembler final {
public:
    explicit Assembler(Allocator& allocator) noexcept
        : module(allocator), linked(allocator), allocator_(&allocator) {
        module.compiler = String("test");
        module.path = String("Main");
    }

    uint32 String(StringView text) noexcept {
        const uint32 index = static_cast<uint32>(module.stringOffsets.Size());
        CHECK(module.stringOffsets.TryPushBack(static_cast<uint32>(module.stringBytes.Size())));
        CHECK(module.stringLengths.TryPushBack(static_cast<uint32>(text.ByteCount())));
        CHECK(module.stringBytes.TryAppend(Span<const char>(text.Data(), text.ByteCount())));
        return index;
    }

    uint32 Begin(StringView name, const Shape& shape) noexcept {
        BytecodeFunction function;
        function.name = String(name);
        function.firstWord = static_cast<uint32>(module.words.Size());
        for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
            function.registers[bank] = shape.registers[bank];
            function.frame[bank] = shape.frame[bank];
            function.parameters[bank] = shape.parameters[bank];
        }

        function.returnBank = shape.returnBank;
        function.returnSlots = shape.returnSlots;
        CHECK(module.functions.TryPushBack(function));
        return static_cast<uint32>(module.functions.Size() - 1);
    }

    void Emit(uint32 word) noexcept {
        CHECK(module.words.TryPushBack(word));
        CHECK(module.locations.TryPushBack(
            BytecodeLocation{static_cast<uint32>(module.words.Size()), 1}));
    }

    void End() noexcept {
        BytecodeFunction& function = module.functions[module.functions.Size() - 1];
        function.wordCount = static_cast<uint32>(module.words.Size()) - function.firstWord;
    }

    uint32 Ref(BytecodeRefKind kind, StringView name, uint32 detail = NoBytecodeIndex) noexcept {
        BytecodeRef ref;
        ref.kind = kind;
        ref.name = String(name);
        ref.detail = detail;
        CHECK(module.refs.TryPushBack(ref));
        return static_cast<uint32>(module.refs.Size() - 1);
    }

    // TYPE 구역의 enum(case 이름들)과 그 Type 참조다.
    uint32 EnumRef(StringView name, Span<const StringView> cases) noexcept {
        BytecodeType type;
        type.name = String(name);
        type.kind = BytecodeTypeKind::Enum;
        type.firstField = static_cast<uint32>(module.typeFields.Size());
        type.fieldCount = static_cast<uint32>(cases.Size());
        for (const StringView item : cases) {
            BytecodeTypeField field;
            field.name = String(item);
            CHECK(module.typeFields.TryPushBack(field));
        }

        CHECK(module.types.TryPushBack(type));
        return Ref(BytecodeRefKind::Type, name, static_cast<uint32>(module.types.Size() - 1));
    }

    // TYPE 구역의 struct(필드는 bank·slot 수 — 선언 순으로 bank 묶음 안 절대 위치를 매긴다)와 그
    // Type 참조다. 필드 참조는 FieldRef로 만든다.
    uint32 StructRef(StringView name, Span<const StringView> names, Span<const uint8> banks,
                     Span<const uint8> slots) noexcept {
        BytecodeType type;
        type.name = String(name);
        type.kind = BytecodeTypeKind::Struct;
        type.firstField = static_cast<uint32>(module.typeFields.Size());
        type.fieldCount = static_cast<uint32>(names.Size());
        for (usize index = 0; index < names.Size(); ++index) {
            type.slots[banks[index]] = static_cast<uint16>(type.slots[banks[index]] + slots[index]);
        }

        uint32 cursor[BytecodeBankCount] = {0, type.slots[0],
                                            uint32(type.slots[0]) + type.slots[1]};
        for (usize index = 0; index < names.Size(); ++index) {
            BytecodeTypeField field;
            field.name = String(names[index]);
            field.bank = banks[index];
            field.slots = slots[index];
            field.offset = cursor[banks[index]];
            cursor[banks[index]] += slots[index];
            CHECK(module.typeFields.TryPushBack(field));
        }

        CHECK(module.types.TryPushBack(type));
        return Ref(BytecodeRefKind::Type, name, static_cast<uint32>(module.types.Size() - 1));
    }

    // struct 타입 참조 typeRef의 index번 필드의 StructField 참조다.
    uint32 FieldRef(uint32 typeRef, uint32 index) noexcept {
        const BytecodeType& type = module.types[module.refs[typeRef].detail];
        return Ref(BytecodeRefKind::StructField, "field", type.firstField + index);
    }

    uint32 Constant(uint64 value) noexcept {
        CHECK(module.scalars.TryPushBack(value));
        return static_cast<uint32>(module.scalars.Size() - 1);
    }

    bool Verified() const noexcept {
        return VerifyBytecode(module).error == BytecodeLoadError::None;
    }

    // 모듈을 (다시) 링크한 결과다(host 없는 모듈 — host 참조가 있으면 링크가 실패해 CHECK가
    // 잡는다). 모듈 모양(단어·참조 수)이나 host가 바뀌었을 때만 다시 링크한다(할당 실패 시험이 링크
    // 배열을 반복 할당으로 세지 않게 — 시험 앞에서 한 번 부른다).
    const VmLinkedModule& Linked(VmHost* host = nullptr) noexcept {
        if (!linkedOnce_ || linkedWords_ != module.words.Size() ||
            linkedRefs_ != module.refs.Size() || linkedHost_ != host) {
            StringBuilder<256> reason(*allocator_);
            CHECK(LinkModule(module, host, linked, reason));
            linkedOnce_ = true;
            linkedWords_ = module.words.Size();
            linkedRefs_ = module.refs.Size();
            linkedHost_ = host;
        }

        return linked;
    }

    BytecodeModule module;
    VmLinkedModule linked;
    Allocator* allocator_;
    bool linkedOnce_ = false;
    usize linkedWords_ = 0;
    usize linkedRefs_ = 0;
    VmHost* linkedHost_ = nullptr;
};

// Scalar 둘을 받아 op 한 번의 결과 하나를 돌려주는 함수(s2 = s0 op s1)다.
inline Shape BinaryShape() noexcept {
    Shape shape;
    shape.registers[0] = 3;
    shape.frame[0] = 3;
    shape.parameters[0] = 2;
    shape.returnSlots = 1;
    return shape;
}

inline void Binary(Assembler& assembler, Opcode op) noexcept {
    assembler.Begin("Binary", BinaryShape());
    assembler.Emit(EncodeABC(op, 2, 0, 1));
    assembler.Emit(EncodeABC(Opcode::Ret, 2, 1, 0));
    assembler.End();
}

// 조립한 모듈이 verifier를 통과하는지 본다(실패는 시험 실패 — 통과하지 않은 모듈은 실행하지
// 않는다).
// verifier를 통과하면 host 없이 미리 링크해 둔다(할당 실패 시험의 반복에 링크 할당이 섞이지 않게).
inline bool Verify(Assembler& assembler) noexcept {
    const bool verified = assembler.Verified();
    CHECK(verified);
    if (verified) {
        static_cast<void>(assembler.Linked());
    }

    return verified;
}

inline uint64 Bits(double value) noexcept {
    return platform::BitCast<uint64>(value);
}

inline uint64 Signed(int64 value) noexcept {
    return static_cast<uint64>(value);
}

// 결과: 반환이면 값 slot 0, fault면 종류를 돌려준다.
struct Outcome {
    VmOutcome outcome = VmOutcome::Returned;
    VmFaultKind fault = VmFaultKind::None;
    uint64 value = 0;
    uint64 steps = 0;
};

inline Outcome Run(VmExecution& execution, const VmLinkedModule& module, uint32 function,
                   Span<const uint64> scalars, Span<const uint64> refs = Span<const uint64>(),
                   const VmLimits& limits = VmLimits()) noexcept {
    Outcome outcome;
    auto called = execution.Call(module, function, scalars, refs, {}, limits);
    CHECK(called);
    if (!called) {
        return outcome;
    }

    outcome.outcome = called.Value().outcome;
    outcome.fault = called.Value().fault.kind;
    outcome.steps = called.Value().steps;
    if (outcome.outcome == VmOutcome::Returned && !execution.ReturnedSlots().IsEmpty()) {
        outcome.value = execution.ReturnedSlots()[0];
    } else if (outcome.outcome == VmOutcome::Ended) {
        outcome.value = called.Value().errorCode;
    }

    return outcome;
}

inline Outcome RunBinary(Opcode op, uint64 left, uint64 right) noexcept {
    SystemAllocator allocator;
    Assembler assembler(allocator);
    Binary(assembler, op);
    if (!Verify(assembler)) {
        return Outcome{};
    }

    VmExecution execution(allocator);
    const uint64 arguments[] = {left, right};
    return Run(execution, assembler.Linked(), 0, Span<const uint64>(arguments, 2));
}

inline bool Returns(const Outcome& outcome, uint64 value) noexcept {
    return outcome.outcome == VmOutcome::Returned && outcome.value == value;
}

inline bool Faults(const Outcome& outcome, VmFaultKind kind) noexcept {
    return outcome.outcome == VmOutcome::Faulted && outcome.fault == kind;
}

} // namespace rocks::sparks::vm_test_support
