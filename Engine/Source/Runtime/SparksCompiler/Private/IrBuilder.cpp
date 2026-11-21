#include "IrBuilder.h"

#include <Platform/Fatal.h>

#include "CompilationChecker.h"
#include "LanguageNames.h"

// typed IR 내림의 기반이다: slot 배치(2절 표), 블록·명령·값 묶음, 지역 이름 표.

namespace rocks::sparks::detail {

namespace {

struct LayoutScalar {
    StringView name;
    IrScalar scalar;
};

constexpr LayoutScalar LayoutScalars[] = {
    {"bool", IrScalar::Bool},   {"int8", IrScalar::I8},     {"int16", IrScalar::I16},
    {"int32", IrScalar::I32},   {"int", IrScalar::I64},     {"int64", IrScalar::I64},
    {"uint8", IrScalar::U8},    {"uint16", IrScalar::U16},  {"uint32", IrScalar::U32},
    {"uint64", IrScalar::U64},  {"float32", IrScalar::F32}, {"float", IrScalar::F64},
    {"float64", IrScalar::F64},
};

struct LayoutVector {
    StringView name;
    uint16 slots;
};

// 성분 double로 펼치는 수학 타입이다(2절 표).
constexpr LayoutVector LayoutVectors[] = {{"vec2", 2},  {"vec3", 3},      {"vec4", 4},
                                          {"quat", 4},  {"mat4", 16},     {"Rotator", 3},
                                          {"Color", 4}, {"Transform", 10}};

void Fill(IrLayout& layout, IrScalar scalar, uint16 count) noexcept {
    for (uint16 index = 0; index < count; ++index) {
        if (layout.slots >= MaxIrSlots) {
            platform::Fatal("Sparks value layout exceeds MaxIrSlots"); // 타입 규칙이 막는다
        }

        layout.scalars[layout.slots++] = scalar;
    }
}

} // namespace

const char* IrOpName(IrOp op) noexcept {
    switch (op) {
    case IrOp::Move:
        return "MOVE";
    case IrOp::LoadInt:
        return "LOADINT";
    case IrOp::LoadFloat:
        return "LOADFLOAT";
    case IrOp::LoadString:
        return "LOADSTR";
    case IrOp::LoadNull:
        return "LOADNULL";
    case IrOp::Unary:
        return "UNARY";
    case IrOp::Binary:
        return "BINARY";
    case IrOp::Compare:
        return "COMPARE";
    case IrOp::Convert:
        return "CONVERT";
    case IrOp::Jump:
        return "JUMP";
    case IrOp::Branch:
        return "BRANCH";
    case IrOp::Switch:
        return "SWITCH";
    case IrOp::Return:
        return "RETURN";
    case IrOp::IsSome:
        return "ISSOME";
    case IrOp::GetField:
        return "GETF";
    case IrOp::SetField:
        return "SETF";
    case IrOp::GetElement:
        return "GETE";
    case IrOp::SetElement:
        return "SETE";
    case IrOp::Length:
        return "LEN";
    case IrOp::GetEntry:
        return "GETENTRY";
    case IrOp::Call:
        return "CALL";
    case IrOp::Emit:
        return "EMIT";
    case IrOp::Await:
        return "AWAIT";
    case IrOp::Try:
        return "TRY";
    case IrOp::ReturnError:
        return "RETURN.ERROR";
    case IrOp::New:
        return "NEW";
    case IrOp::Concat:
        return "CONCAT";
    case IrOp::Release:
        return "RELEASE";
    case IrOp::Fault:
        return "FAULT";
    }

    return "?";
}

IrLayout IrBuilder::Layout(TypeChecker& checker, uint32 type) noexcept {
    IrLayout layout;
    if (type == NoType || type == checker.voidType_) {
        return layout;
    }

    const Compilation& c = checker.compilation_;
    if (checker.IsOptional(type)) {
        const IrLayout inner = Layout(checker, checker.NonOptional(type));
        if (inner.bank != IrBank::Scalar) {
            return inner; // null handle
        }

        layout.bank = IrBank::Scalar;
        Fill(layout, IrScalar::Flag, 1);
        for (uint16 index = 0; index < inner.slots; ++index) {
            Fill(layout, inner.scalars[index], 1);
        }

        return layout;
    }

    const uint32 symbol = checker.SymbolOf(type);
    if (symbol == NoSymbol) {
        return layout;
    }

    const Symbol& entry = c.symbols_[symbol];
    switch (entry.kind) {
    case SymbolKind::BuiltinType:
    case SymbolKind::PreludeType: {
        for (const LayoutScalar& name : LayoutScalars) {
            if (entry.name == name.name) {
                Fill(layout, name.scalar, 1);
                return layout;
            }
        }

        for (const LayoutVector& name : LayoutVectors) {
            if (entry.name == name.name) {
                Fill(layout, IrScalar::F64, name.slots);
                return layout;
            }
        }

        if (entry.name == "Tag" || entry.name == "Row" || entry.name == "Class") {
            Fill(layout, IrScalar::Name, 1);
            return layout;
        }

        if (entry.name == "Clock") {
            Fill(layout, IrScalar::Enum, 1);
            return layout;
        }

        if (entry.name == "Soft") {
            Fill(layout, IrScalar::AssetLo, 1);
            Fill(layout, IrScalar::AssetHi, 1);
            return layout;
        }

        if (entry.name == "Result") {
            const IrLayout inner = Layout(checker, checker.Argument(type, 0));
            if (inner.slots == 0 || inner.bank == IrBank::Scalar) {
                Fill(layout, IrScalar::Flag, 1);
                for (uint16 index = 0; index < inner.slots; ++index) {
                    Fill(layout, inner.scalars[index], 1);
                }

                return layout;
            }

            layout.bank = IrBank::Buf;
            layout.slots = 1;
            return layout;
        }

        if (entry.name == "LoadRequest" || entry.name == "InputAction" || entry.name == "CVarRef") {
            layout.bank = IrBank::Ref;
            layout.slots = 1;
            return layout;
        }

        layout.bank = IrBank::Buf; // string·Text·TagSet·Array·Map·Set
        layout.slots = 1;
        return layout;
    }
    case SymbolKind::Enum:
        Fill(layout, IrScalar::Enum, 1);
        return layout;
    case SymbolKind::Struct:
        layout.bank = IrBank::Buf;
        layout.slots = 1;
        return layout;
    case SymbolKind::Class:
    case SymbolKind::Interface:
        layout.bank = IrBank::Ref;
        layout.slots = 1;
        return layout;
    case SymbolKind::EngineType: {
        const SchemaTypeKind kind = c.schemas_[entry.index].schema.Types()[entry.item].kind;
        if (kind == SchemaTypeKind::Enum) {
            Fill(layout, IrScalar::Enum, 1);
        } else {
            layout.bank = kind == SchemaTypeKind::Struct ? IrBank::Buf : IrBank::Ref;
            layout.slots = 1;
        }

        return layout;
    }
    default:
        return layout;
    }
}

IrBuilder::IrBuilder(TypeChecker& checker, IrFunction& function) noexcept
    : checker_(checker), c_(checker.compilation_), function_(function),
      tree_(&checker.compilation_.trees_[function.module]), module_(function.module),
      locals_(checker.allocator_), held_(checker.allocator_), loops_(checker.allocator_) {
}

bool IrBuilder::Failed() const noexcept {
    return checker_.binder_.error_ != ErrorCode::None;
}

void IrBuilder::Fail(ErrorCode error) noexcept {
    checker_.binder_.error_ = error;
}

uint32 IrBuilder::NewBlock() noexcept {
    const uint32 index = static_cast<uint32>(function_.blocks.Size());
    if (auto pushed = function_.blocks.TryPushBack(IrBlock{}); !pushed) {
        Fail(pushed.Error());
        return NoIrBlock;
    }

    return index;
}

void IrBuilder::StartBlock(uint32 block) noexcept {
    if (block == NoIrBlock || Failed()) {
        return;
    }

    current_ = block;
    function_.blocks[block].first = static_cast<uint32>(function_.instructions.Size());
    function_.blocks[block].count = 0;
}

bool IrBuilder::Terminated() const noexcept {
    if (current_ == NoIrBlock) {
        return true;
    }

    const IrBlock& block = function_.blocks[current_];
    if (block.count == 0) {
        return false;
    }

    const IrOp last = function_.instructions[block.first + block.count - 1].op;
    return last == IrOp::Jump || last == IrOp::Branch || last == IrOp::Switch ||
           last == IrOp::Return || last == IrOp::Await || last == IrOp::Fault ||
           last == IrOp::Try || last == IrOp::ReturnError;
}

void IrBuilder::Add(const IrInstruction& instruction) noexcept {
    if (Failed()) {
        return;
    }

    // 닫힌 블록 뒤의 명령(return 뒤의 도달하지 않는 문장)은 새 블록에 둔다.
    if (Terminated()) {
        StartBlock(NewBlock());
    }

    if (auto pushed = function_.instructions.TryPushBack(instruction); !pushed) {
        Fail(pushed.Error());
        return;
    }

    ++function_.blocks[current_].count;
}

void IrBuilder::Add(IrOp op, uint32 a, uint32 b, uint32 c, uint32 extra, uint32 node,
                    uint8 sub) noexcept {
    IrInstruction instruction;
    instruction.op = op;
    instruction.sub = sub;
    instruction.a = a;
    instruction.b = b;
    instruction.c = c;
    instruction.extra = extra;
    instruction.span = overrideSpan_    ? spanOverride_
                       : node == NoNode ? SourceSpan{}
                                        : tree_->Node(node).span;
    Add(instruction);
}

uint32 IrBuilder::NewValue(uint32 type, StringView name) noexcept {
    const IrLayout layout = Layout(checker_, type);
    if (layout.slots == 0) {
        return NoIrRegister;
    }

    const uint32 first = static_cast<uint32>(function_.registers.Size());
    for (uint16 slot = 0; slot < layout.slots; ++slot) {
        IrRegister entry;
        entry.bank = layout.bank;
        entry.scalar = layout.bank == IrBank::Scalar ? layout.scalars[slot] : IrScalar::None;
        entry.type = type;
        entry.slot = slot;
        entry.slots = layout.slots;
        entry.name = name;
        if (auto pushed = function_.registers.TryPushBack(entry); !pushed) {
            Fail(pushed.Error());
            return NoIrRegister;
        }
    }

    return first;
}

uint32 IrBuilder::NewWait() noexcept {
    IrRegister entry;
    entry.bank = IrBank::Ref;
    entry.type = NoType;
    const uint32 index = static_cast<uint32>(function_.registers.Size());
    if (auto pushed = function_.registers.TryPushBack(entry); !pushed) {
        Fail(pushed.Error());
        return NoIrRegister;
    }

    return index;
}

uint32 IrBuilder::SlotCount(uint32 first) const noexcept {
    return first == NoIrRegister ? 0 : function_.registers[first].slots;
}

void IrBuilder::MoveValue(uint32 to, uint32 from, uint32 node) noexcept {
    if (to == NoIrRegister || from == NoIrRegister || to == from) {
        return;
    }

    const uint32 count = SlotCount(to);
    for (uint32 slot = 0; slot < count; ++slot) {
        Add(IrOp::Move, to + slot, from + slot, NoIrRegister, 0, node);
    }
}

uint32 IrBuilder::LocalRegister(uint32 symbol) const noexcept {
    for (usize index = locals_.Size(); index > 0; --index) {
        if (static_cast<uint32>(locals_[index - 1] >> 32) == symbol) {
            return static_cast<uint32>(locals_[index - 1]);
        }
    }

    return NoIrRegister;
}

void IrBuilder::BindLocal(uint32 symbol, uint32 first) noexcept {
    if (auto pushed = locals_.TryPushBack((uint64(symbol) << 32) | first); !pushed) {
        Fail(pushed.Error());
    }
}

bool IrBuilder::Validate() const noexcept {
    // 내림의 구조 불변식이다: 내용 있는 블록은 끝 명령으로 닫히고, 분기 대상은 내용 있는 블록이며,
    // 레지스터·피연산자 번호가 범위 안이고, 이동·비교의 묶음이 맞다.
    const IrFunction& f = function_;
    const uint32 registers = static_cast<uint32>(f.registers.Size());
    auto isRegister = [&](uint32 index) noexcept { return index < registers; };
    auto optional = [&](uint32 index) noexcept {
        return index == NoIrRegister || index < registers;
    };
    auto target = [&](uint32 block) noexcept {
        return block < f.blocks.Size() && f.blocks[block].count > 0;
    };
    auto list = [&](uint32 first, uint32 count, uint32 stride) noexcept {
        if (first > f.operands.Size() || count * stride > f.operands.Size() - first) {
            return false;
        }

        for (uint32 index = 0; index < count * stride; index += stride) {
            if (!optional(f.operands[first + index + stride - 1])) {
                return false;
            }
        }

        return true;
    };
    for (const IrBlock& block : f.blocks) {
        if (block.count == 0) {
            continue;
        }

        if (block.first + block.count > f.instructions.Size()) {
            return false;
        }

        const IrOp last = f.instructions[block.first + block.count - 1].op;
        if (last != IrOp::Jump && last != IrOp::Branch && last != IrOp::Switch &&
            last != IrOp::Return && last != IrOp::Await && last != IrOp::Fault &&
            last != IrOp::Try && last != IrOp::ReturnError) {
            return false;
        }
    }

    for (const IrInstruction& i : f.instructions) {
        bool ok = true;
        switch (i.op) {
        case IrOp::Move:
            ok = isRegister(i.a) && isRegister(i.b) &&
                 f.registers[i.a].bank == f.registers[i.b].bank;
            break;
        case IrOp::LoadInt:
        case IrOp::LoadFloat:
        case IrOp::LoadNull:
        case IrOp::Release:
            ok = isRegister(i.a);
            break;
        case IrOp::LoadString:
            ok = isRegister(i.a) && i.b < f.strings.Size();
            break;
        case IrOp::Unary:
        case IrOp::Convert:
        case IrOp::IsSome:
        case IrOp::Length:
            ok = isRegister(i.a) && isRegister(i.b);
            break;
        case IrOp::Binary:
        case IrOp::GetElement:
        case IrOp::SetElement:
        case IrOp::GetEntry:
            ok = isRegister(i.a) && isRegister(i.b) && isRegister(i.c);
            break;
        case IrOp::Compare:
            ok = isRegister(i.a) && isRegister(i.b) && isRegister(i.c) &&
                 SlotCount(i.b) == i.extra && SlotCount(i.c) == i.extra;
            break;
        case IrOp::Jump:
            ok = target(i.a);
            break;
        case IrOp::Branch:
            ok = target(i.a) && target(i.c) && isRegister(i.b);
            break;
        case IrOp::Switch:
            ok = isRegister(i.b) && target(i.extra) && i.a + i.c <= f.cases.Size();
            for (uint32 index = i.a; ok && index < i.a + i.c; ++index) {
                ok = target(f.cases[index].block);
            }

            break;
        case IrOp::Return:
        case IrOp::ReturnError:
            ok = optional(i.b);
            break;
        case IrOp::GetField:
            ok = isRegister(i.a) && optional(i.b);
            break;
        case IrOp::SetField:
            ok = isRegister(i.a) && isRegister(i.b);
            break;
        case IrOp::Call:
        case IrOp::Concat:
            ok = optional(i.a) && list(i.b, i.c, 1);
            break;
        case IrOp::Emit:
            ok = optional(i.a) && list(i.b, i.c, 1);
            break;
        case IrOp::New:
            ok = isRegister(i.a) &&
                 list(i.b, i.c, i.sub == IrNewStruct || i.sub == IrNewLiteral ? 2 : 1);
            break;
        case IrOp::Await:
            ok = optional(i.a) && optional(i.b) && target(i.c);
            break;
        case IrOp::Try:
            ok = optional(i.a) && isRegister(i.b) && target(i.c) && target(i.extra);
            break;
        case IrOp::Fault:
            break;
        }

        if (!ok) {
            return false;
        }
    }

    return true;
}

} // namespace rocks::sparks::detail
