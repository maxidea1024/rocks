#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>

#include "CompilationChecker.h"
#include "BcEncoder.h"
#include "IrBuilder.h"

// 모듈 전체의 IR 내림과 IR 덤프(`.ir`)다(Docs/Design/SparksBytecode.ko.md 3.2절, LC4a). 오류 없는
// 컴파일에서 Codegen 단계가 모듈마다 함수·handler를 선언 순으로 내리고 덤프 글자를 Compilation에
// 둔다. 덤프는 결정적 영어 텍스트다.

namespace rocks::sparks::detail {

namespace {

char BankLetter(IrBank bank) noexcept {
    return bank == IrBank::Scalar ? 's' : bank == IrBank::Ref ? 'r' : 'b';
}

const char* ScalarName(IrScalar scalar) noexcept {
    switch (scalar) {
    case IrScalar::None:
        return "";
    case IrScalar::Bool:
        return "Bool";
    case IrScalar::I8:
        return "I8";
    case IrScalar::I16:
        return "I16";
    case IrScalar::I32:
        return "I32";
    case IrScalar::I64:
        return "I64";
    case IrScalar::U8:
        return "U8";
    case IrScalar::U16:
        return "U16";
    case IrScalar::U32:
        return "U32";
    case IrScalar::U64:
        return "U64";
    case IrScalar::F32:
        return "F32";
    case IrScalar::F64:
        return "F64";
    case IrScalar::Enum:
        return "Enum";
    case IrScalar::Name:
        return "Name";
    case IrScalar::Flag:
        return "Flag";
    case IrScalar::AssetLo:
        return "AssetLo";
    case IrScalar::AssetHi:
        return "AssetHi";
    }

    return "?";
}

const char* ArithName(uint8 sub) noexcept {
    switch (static_cast<IrArith>(sub)) {
    case IrArith::Add:
        return "ADD";
    case IrArith::Sub:
        return "SUB";
    case IrArith::Mul:
        return "MUL";
    case IrArith::Div:
        return "DIV";
    case IrArith::Mod:
        return "MOD";
    case IrArith::Neg:
        return "NEG";
    case IrArith::Not:
        return "NOT";
    }

    return "?";
}

const char* CompareName(uint8 sub) noexcept {
    switch (static_cast<IrCompare>(sub)) {
    case IrCompare::Equal:
        return "EQ";
    case IrCompare::NotEqual:
        return "NE";
    case IrCompare::Less:
        return "LT";
    case IrCompare::LessEqual:
        return "LE";
    case IrCompare::Greater:
        return "GT";
    case IrCompare::GreaterEqual:
        return "GE";
    }

    return "?";
}

} // namespace

// 덤프 한 함수의 쓰기 문맥이다.
class IrWriter final {
public:
    IrWriter(TypeChecker& checker, const IrFunction& function, StringBuilderBase& out) noexcept
        : checker_(checker), c_(checker.compilation_), function_(function), out_(out) {
    }

    void Write() noexcept {
        WriteHeader();
        for (uint32 index = 0; index < function_.registers.Size(); ++index) {
            const IrRegister& entry = function_.registers[index];
            if (entry.slot != 0) {
                continue;
            }

            out_.Append("  ");
            Register(index);
            if (entry.slots > 1) {
                out_.Append("..");
                Register(index + entry.slots - 1);
            }

            if (!entry.name.IsEmpty()) {
                out_.AppendFormat(" {}", entry.name);
            }

            out_.Append(": ");
            if (entry.type == NoType) {
                out_.Append("wait");
            } else {
                checker_.AppendDisplayType(out_, entry.type);
            }

            if (entry.bank == IrBank::Scalar) {
                out_.Append(" [");
                for (uint32 slot = 0; slot < entry.slots; ++slot) {
                    out_.AppendFormat("{}{}", slot == 0 ? "" : " ",
                                      ScalarName(function_.registers[index + slot].scalar));
                }

                out_.Append("]");
            }

            out_.Append("\n");
        }

        for (uint32 block = 0; block < function_.blocks.Size(); ++block) {
            const IrBlock& entry = function_.blocks[block];
            out_.AppendFormat("L{}:\n", block);
            for (uint32 index = entry.first; index < entry.first + entry.count; ++index) {
                Instruction(function_.instructions[index]);
            }
        }
    }

private:
    void WriteHeader() noexcept {
        const SyntaxTree& tree = c_.trees_[function_.module];
        const bool handler = tree.Node(function_.node).kind == SyntaxKind::Handler;
        out_.Append(handler ? "handler " : "function ");
        if (function_.owner != NoSymbol) {
            out_.AppendFormat("{}.", c_.symbols_[function_.owner].name);
        }

        if (handler) {
            out_.Append("on ");
            const uint32 path = tree.Child(function_.node, 2);
            bool first = true;
            for (const uint32 segment : tree.Children(path)) {
                out_.AppendFormat("{}{}", first ? "" : ".",
                                  checker_.TokenText(function_.module, segment));
                first = false;
            }
        } else if (function_.symbol != NoSymbol) {
            out_.Append(c_.symbols_[function_.symbol].name);
        }

        out_.Append("(");
        for (usize index = 0; index < function_.parameters.Size(); ++index) {
            if (index != 0) {
                out_.Append(", ");
            }

            Value(function_.parameters[index]);
        }

        out_.Append(")\n");
    }

    void Register(uint32 index) noexcept {
        if (index == NoIrRegister) {
            out_.Append("_");
            return;
        }

        out_.AppendFormat("{}{}", BankLetter(function_.registers[index].bank), index);
    }

    void Value(uint32 index) noexcept {
        Register(index);
    }

    void SymbolName(uint32 symbol) noexcept {
        if (symbol == NoSymbol || symbol >= c_.symbols_.Size()) {
            out_.Append("?");
            return;
        }

        const Symbol& entry = c_.symbols_[symbol];
        if (entry.owner != NoSymbol && entry.owner < c_.symbols_.Size()) {
            out_.AppendFormat("{}.", c_.symbols_[entry.owner].name);
        }

        out_.Append(entry.name);
    }

    void List(uint32 first, uint32 count) noexcept {
        out_.Append("(");
        for (uint32 index = 0; index < count; ++index) {
            if (index != 0) {
                out_.Append(", ");
            }

            Value(function_.operands[first + index]);
        }

        out_.Append(")");
    }

    void Location(const IrInstruction& instruction) noexcept {
        const SourceFile& file = c_.sources_->Get(c_.modules_[function_.module].source);
        const SourceLocation at = file.Locate(instruction.span.begin);
        out_.AppendFormat("    ; {}:{}\n", at.line, at.column);
    }

    void Assign(uint32 a) noexcept {
        out_.Append("    ");
        if (a != NoIrRegister) {
            Register(a);
            out_.Append(" = ");
        }
    }

    void Instruction(const IrInstruction& i) noexcept {
        const char* name = IrOpName(i.op);
        const uint64 wide = (uint64(i.extra) << 32) | i.b;
        switch (i.op) {
        case IrOp::Move:
            Assign(i.a);
            out_.Append("MOVE ");
            Register(i.b);
            break;
        case IrOp::LoadInt:
            Assign(i.a);
            out_.AppendFormat("LOADINT {}", static_cast<int64>(wide));
            break;
        case IrOp::LoadFloat:
            Assign(i.a);
            out_.AppendFormat("LOADFLOAT {}", platform::BitCast<double>(wide));
            break;
        case IrOp::LoadString:
            Assign(i.a);
            if (i.sub == IrStringPiece) {
                out_.AppendFormat("LOADSTR.PIECE \"{}\"", function_.strings[i.b]);
            } else {
                out_.AppendFormat("LOADSTR {}", function_.strings[i.b]);
            }

            break;
        case IrOp::LoadNull:
            Assign(i.a);
            out_.Append("LOADNULL");
            break;
        case IrOp::Unary:
            Assign(i.a);
            out_.AppendFormat("{}.{} ", ArithName(i.sub),
                              ScalarName(function_.registers[i.a].scalar));
            Register(i.b);
            break;
        case IrOp::Binary:
            Assign(i.a);
            out_.AppendFormat("{}.{} ", ArithName(i.sub),
                              ScalarName(function_.registers[i.a].scalar));
            Register(i.b);
            out_.Append(", ");
            Register(i.c);
            break;
        case IrOp::Compare:
            Assign(i.a);
            out_.AppendFormat("{}", CompareName(i.sub));
            if (i.extra > 1) {
                out_.AppendFormat("x{}", i.extra);
            }

            out_.Append(" ");
            Register(i.b);
            out_.Append(", ");
            Register(i.c);
            break;
        case IrOp::Convert:
            Assign(i.a);
            if (i.sub == IrConvertCast) {
                out_.Append("CAST ");
                Register(i.b);
                out_.Append(" as ");
                SymbolName(i.extra);
            } else {
                out_.Append("CONVERT ");
                Register(i.b);
            }

            break;
        case IrOp::Jump:
            out_.AppendFormat("    JUMP L{}", i.a);
            break;
        case IrOp::Branch:
            out_.Append("    BRANCH ");
            Register(i.b);
            out_.AppendFormat(", L{}, L{}", i.a, i.c);
            break;
        case IrOp::Switch:
            out_.Append("    SWITCH ");
            Register(i.b);
            out_.Append(" [");
            for (uint32 index = i.a; index < i.a + i.c; ++index) {
                out_.AppendFormat("{}{}: L{}", index == i.a ? "" : ", ",
                                  function_.cases[index].value, function_.cases[index].block);
            }

            out_.AppendFormat("] else L{}", i.extra);
            break;
        case IrOp::Return:
            out_.Append("    RETURN");
            if (i.b != NoIrRegister) {
                out_.Append(" ");
                Register(i.b);
            }

            break;
        case IrOp::IsSome:
            Assign(i.a);
            out_.Append("ISSOME ");
            Register(i.b);
            break;
        case IrOp::GetField:
            Assign(i.a);
            out_.Append("GETF ");
            if (i.b != NoIrRegister) {
                Register(i.b);
                out_.Append(".");
            }

            SymbolName(i.extra);
            break;
        case IrOp::SetField:
            out_.Append("    SETF ");
            Register(i.a);
            out_.Append(".");
            SymbolName(i.extra);
            out_.Append(", ");
            Register(i.b);
            break;
        case IrOp::GetElement:
            Assign(i.a);
            out_.Append("GETE ");
            Register(i.b);
            out_.Append("[");
            Register(i.c);
            out_.Append("]");
            break;
        case IrOp::SetElement:
            out_.Append("    SETE ");
            Register(i.a);
            out_.Append("[");
            Register(i.b);
            out_.Append("], ");
            Register(i.c);
            break;
        case IrOp::Length:
            Assign(i.a);
            out_.Append("LEN ");
            Register(i.b);
            break;
        case IrOp::GetEntry:
            Assign(i.a);
            out_.Append(i.sub == IrEntryKey ? "GETKEY " : "GETVALUE ");
            Register(i.b);
            out_.Append("[");
            Register(i.c);
            out_.Append("]");
            break;
        case IrOp::Call:
            Assign(i.a);
            out_.Append(i.sub == IrCallRpc        ? "CALL.RPC "
                        : i.sub == IrCallMutating ? "CALL.MUT "
                                                  : "CALL ");
            SymbolName(i.extra);
            if (i.types != NoIrRegister) {
                out_.Append("<");
                for (uint32 index = 0; index < function_.operands[i.types]; ++index) {
                    if (index != 0) {
                        out_.Append(", ");
                    }

                    checker_.AppendDisplayType(out_, function_.operands[i.types + 1 + index]);
                }

                out_.Append(">");
            }

            List(i.b, i.c);
            break;
        case IrOp::Emit:
            out_.Append("    EMIT ");
            if (i.a == NoIrRegister) {
                out_.Append("this");
            } else {
                Register(i.a);
            }

            out_.Append(".");
            SymbolName(i.extra);
            List(i.b, i.c);
            break;
        case IrOp::Await:
            Assign(i.a);
            out_.Append("AWAIT ");
            Register(i.b);
            if (i.sub == IrAwaitEvent) {
                out_.Append(".");
                SymbolName(i.extra);
            }

            out_.AppendFormat(" -> L{}", i.c);
            break;
        case IrOp::Try:
            Assign(i.a);
            out_.Append("TRY ");
            Register(i.b);
            out_.AppendFormat(" -> L{} else L{}", i.extra, i.c);
            break;
        case IrOp::ReturnError:
            out_.Append("    RETURN.ERROR ");
            Register(i.b);
            break;
        case IrOp::New:
            Assign(i.a);
            NewInstruction(i);
            break;
        case IrOp::Concat:
            Assign(i.a);
            out_.Append("CONCAT");
            List(i.b, i.c);
            break;
        case IrOp::Release:
            out_.Append("    RELEASE ");
            Register(i.a);
            break;
        case IrOp::Fault:
            out_.AppendFormat("    FAULT {}", i.extra);
            break;
        default:
            out_.AppendFormat("    {}", name);
            break;
        }

        Location(i);
    }

    void NewInstruction(const IrInstruction& i) noexcept {
        switch (i.sub) {
        case IrNewStruct:
        case IrNewLiteral:
            out_.Append(i.sub == IrNewLiteral ? "NEW.LITERAL " : "NEW ");
            SymbolName(i.extra);
            out_.Append("(");
            for (uint32 pair = 0; pair < i.c; ++pair) {
                const uint32 key = function_.operands[i.b + pair * 2];
                const uint32 value = function_.operands[i.b + pair * 2 + 1];
                out_.Append(pair == 0 ? "" : ", ");
                if (i.sub == IrNewStruct) {
                    out_.AppendFormat("{}: ", c_.symbols_[key].name);
                } else if (key != NoSymbol) {
                    out_.AppendFormat("{}: ", function_.strings[key]);
                }

                Register(value);
            }

            out_.Append(")");
            return;
        case IrNewCollection:
            out_.Append("NEW.COLLECTION ");
            SymbolName(i.extra);
            List(i.b, i.c);
            return;
        default:
            out_.Append(i.sub == IrNewResultError ? "NEW.ERROR " : "NEW.OK ");
            SymbolName(i.extra);
            List(i.b, i.c);
            return;
        }
    }

    TypeChecker& checker_;
    const Compilation& c_;
    const IrFunction& function_;
    StringBuilderBase& out_;
};

void TypeChecker::LowerProgram() noexcept {
    Compilation& c = compilation_;
    if (auto resized = c.irDumps_.TryResize(c.modules_.Size()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 module = 0; module < c.modules_.Size() && binder_.error_ == ErrorCode::None;
         ++module) {
        if (auto pushed = c.bytecode_.TryPushBack(BytecodeModule(*c.allocator_)); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        if (c.nodeBase_[module] != NoNode) {
            LowerModule(module);
        }
    }
}

void TypeChecker::LowerModule(uint32 module) noexcept {
    // 최상위 함수와 class·struct의 함수·handler를 선언 순으로 내린다. 같은 모듈 함수의 번호를 먼저
    // 정해 두고(앞선 함수가 뒤 함수를 부르는 CALL의 참조) 함수마다 IR → bytecode로 쓴다.
    Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    StringBuilder<CompilationMessageInlineBytes> out(allocator_);
    Array<uint64> bodies(allocator_); // (owner << 32) | node
    const uint32 root = tree.Root();
    const uint32 declarations = root == NoNode ? NoNode : tree.Child(root, 1);
    auto collect = [&](uint32 node, uint32 owner) noexcept {
        const SyntaxNode& entry = tree.Node(node);
        const bool handler = entry.kind == SyntaxKind::Handler;
        if ((!handler && entry.kind != SyntaxKind::Function) ||
            tree.Child(node, handler ? 4 : 5) == NoNode) {
            return;
        }

        if (auto pushed = bodies.TryPushBack((uint64(owner) << 32) | node); !pushed) {
            binder_.error_ = pushed.Error();
        }
    };
    if (declarations != NoNode) {
        for (const uint32 declaration : tree.Children(declarations)) {
            const SyntaxKind kind = tree.Node(declaration).kind;
            if (kind == SyntaxKind::Function) {
                collect(declaration, NoSymbol);
                continue;
            }

            if (kind != SyntaxKind::Class && kind != SyntaxKind::Struct) {
                continue;
            }

            const uint32 owner = c.NodeSymbol(module, tree.Child(declaration, 2));
            const uint32 members = tree.Child(declaration, kind == SyntaxKind::Class ? 4 : 3);
            if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
                continue;
            }

            for (const uint32 member : tree.Children(members)) {
                collect(member, owner);
            }
        }
    }

    BytecodeEncoder encoder(*this, module, c.bytecode_[module]);
    for (usize index = 0; index < bodies.Size(); ++index) {
        const uint32 node = static_cast<uint32>(bodies[index]);
        if (tree.Node(node).kind == SyntaxKind::Function) {
            const uint32 symbol = c.NodeSymbol(module, tree.Child(node, 2));
            if (symbol != NoSymbol) {
                encoder.Declare(symbol, static_cast<uint32>(index));
            }
        }
    }

    for (usize index = 0; index < bodies.Size() && binder_.error_ == ErrorCode::None; ++index) {
        const uint32 node = static_cast<uint32>(bodies[index]);
        const uint32 owner = static_cast<uint32>(bodies[index] >> 32);
        const bool handler = tree.Node(node).kind == SyntaxKind::Handler;
        IrFunction function(allocator_);
        function.module = module;
        function.node = node;
        function.symbol = handler ? NoSymbol : c.NodeSymbol(module, tree.Child(node, 2));
        function.owner = owner;
        IrBuilder builder(*this, function);
        if (!builder.Build()) {
            return;
        }

        if (!builder.Validate()) {
            platform::Fatal("Sparks typed IR invariant violated"); // 내림의 결함
        }

        if (!out.View().IsEmpty()) {
            out.Append("\n"); // 함수 사이의 빈 줄
        }

        const usize headerStart = out.View().ByteCount();
        IrWriter(*this, function, out).Write();
        // 함수 표시 이름은 IR 덤프 머리 줄의 "function "·"handler " 뒤, 매개변수 앞이다.
        const StringView dump = out.View();
        usize begin = headerStart;
        while (begin < dump.ByteCount() && dump.Data()[begin] != ' ') {
            ++begin;
        }

        usize finish = begin + 1;
        while (finish < dump.ByteCount() && dump.Data()[finish] != '(') {
            ++finish;
        }

        const StringView name(dump.Data() + begin + 1, finish > begin + 1 ? finish - begin - 1 : 0);
        StringBuilder<128> owned(allocator_);
        owned.Append(name);
        if (!binder_.Succeeded(owned)) {
            return;
        }

        const uint32 nameNode = tree.Child(node, 2);
        if (!encoder.Encode(function, static_cast<uint32>(index), owned.View(), nameNode)) {
            return;
        }
    }

    if (!binder_.Succeeded(out)) {
        return;
    }

    const StringView text = out.View();
    const usize offset = c.text_.Size();
    if (auto appended = c.text_.TryAppend(Span<const char>(text.Data(), text.ByteCount()));
        !appended) {
        binder_.error_ = appended.Error();
        return;
    }

    c.irDumps_[module] =
        Compilation::TextRef{static_cast<uint32>(offset), static_cast<uint32>(text.ByteCount())};
}

} // namespace rocks::sparks::detail

namespace rocks::sparks {

const BytecodeModule* Compilation::ModuleBytecode(uint32 module) const noexcept {
    return module < bytecode_.Size() && lastStage_ >= CompilationStage::Codegen &&
                   module < irDumps_.Size()
               ? &bytecode_[module]
               : nullptr;
}

Result<void> Compilation::AppendBytecodeDump(StringBuilderBase& out, uint32 module) const noexcept {
    const BytecodeModule* bytecode = ModuleBytecode(module);
    if (bytecode == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return rocks::sparks::AppendBytecodeDump(out, *bytecode);
}

Result<void> Compilation::AppendIrDump(StringBuilderBase& out, uint32 module) const noexcept {
    if (module >= irDumps_.Size() || lastStage_ < CompilationStage::Codegen) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    out.Append(Text(irDumps_[module]));
    return out.Status();
}

} // namespace rocks::sparks
