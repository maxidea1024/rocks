#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>

#include "CompilationChecker.h"
#include <Core/Encoding/Sha256.h>
#include <SparksVm/BytecodeFile.h>

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
    case IrArith::Compose:
        return "COMPOSE";
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
        } else if (tree.Node(function_.node).kind == SyntaxKind::Class) {
            out_.Append("$init"); // class 초기화 함수(SparksRuntime 2.1절)
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
                        : i.sub == IrCallChild    ? "CALL.CHILD "
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
            out_.Append(i.sub == IrAwaitOwned ? "AWAIT.OWN " : "AWAIT ");
            Register(i.b);
            out_.AppendFormat(" -> L{}", i.c);
            break;
        case IrOp::WaitEvent:
            Assign(i.a);
            out_.Append("WAITEV ");
            Register(i.b);
            out_.Append(".");
            SymbolName(i.extra);
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
        case IrOp::EndExecution:
            out_.Append("    END.EXECUTION ");
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
            if (i.b != NoIrRegister) {
                out_.Append(" ");
                Register(i.b);
            }

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

    if (auto resized = c.lowered_.TryResize(c.modules_.Size()); !resized) {
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

            // class 초기화 함수 `<Class>.$init`(SparksRuntime 2.1절 — R6): 초기값이 있거나 0 값이
            // 0·null이 아닌(non-null Buf — 문자열·컨테이너·struct) 필드가 있을 때만.
            bool initializes = false;
            for (const uint32 member : tree.Children(members)) {
                if (kind != SyntaxKind::Class || tree.Node(member).kind != SyntaxKind::Field) {
                    continue;
                }

                const uint32 field = c.NodeSymbol(module, tree.Child(member, 2));
                const uint32 type = field == NoSymbol ? NoType : FieldType(field);
                const IrLayout layout = IrBuilder::Layout(*this, type);
                // 0 값이 0·null이 아닌 타입(SL 3.2절): non-null Buf(문자열·컨테이너·struct)와
                // 단위값 수학 타입(quat·mat4·Transform — LC6a 리뷰 B1). LowerZero와 같은 판정이다.
                const uint32 symbol = type == NoType ? NoSymbol : SymbolOf(NonOptional(type));
                const StringView name = symbol == NoSymbol ? StringView() : c.symbols_[symbol].name;
                const bool identity = symbol != NoSymbol &&
                                      (c.symbols_[symbol].kind == SymbolKind::BuiltinType ||
                                       c.symbols_[symbol].kind == SymbolKind::PreludeType) &&
                                      (name == "quat" || name == "mat4" || name == "Transform");
                initializes = initializes || tree.Child(member, 4) != NoNode ||
                              (type != NoType && !IsOptional(type) &&
                               (layout.bank == IrBank::Buf || identity));
            }

            if (initializes) {
                if (auto pushed = bodies.TryPushBack((uint64(owner) << 32) | declaration);
                    !pushed) {
                    binder_.error_ = pushed.Error();
                }
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
        const bool initializer = tree.Node(node).kind == SyntaxKind::Class;
        IrFunction function(allocator_);
        function.module = module;
        function.node = node;
        function.symbol =
            handler || initializer ? NoSymbol : c.NodeSymbol(module, tree.Child(node, 2));
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

        // body hash: 이 함수 IR 덤프에서 위치 주석(`    ; 줄:열`)을 뺀 글자의 SHA-256(6.2절 — 줄만
        // 옮긴 본문은 같은 hash).
        Sha256 hasher;
        const StringView functionText(out.View().Data() + headerStart,
                                      out.View().ByteCount() - headerStart);
        usize lineStart = 0;
        constexpr StringView LocationMark = "    ; ";
        for (usize at = 0; at <= functionText.ByteCount(); ++at) {
            if (at < functionText.ByteCount() && functionText.Data()[at] != '\n') {
                continue;
            }

            // 줄 끝의 `    ; <줄>:<열>` 접미만 뗀다(문자열 조각 안의 같은 글자는 그대로 둔다).
            StringView line(functionText.Data() + lineStart, at - lineStart);
            for (usize mark = line.ByteCount(); mark >= LocationMark.ByteCount(); --mark) {
                const usize begin = mark - LocationMark.ByteCount();
                if (StringView(line.Data() + begin, LocationMark.ByteCount()) != LocationMark) {
                    continue;
                }

                usize digits = 0;
                usize colons = 0;
                bool location = mark < line.ByteCount();
                for (usize index = mark; index < line.ByteCount() && location; ++index) {
                    const char c = line.Data()[index];
                    if (c == ':') {
                        location = digits > 0 && colons == 0;
                        ++colons;
                        digits = 0;
                    } else {
                        location = c >= '0' && c <= '9';
                        ++digits;
                    }
                }

                if (location && colons == 1 && digits > 0) {
                    line = StringView(line.Data(), begin);
                }

                break;
            }

            hasher.Update(
                Span<const uint8>(reinterpret_cast<const uint8*>(line.Data()), line.ByteCount()));
            hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>("\n"), 1));
            lineStart = at + 1;
        }

        const Sha256Digest body = hasher.Finish();
        BytecodeFunction& encoded = c.bytecode_[module].functions[index];
        for (usize at = 0; at < BytecodeHashBytes; ++at) {
            encoded.bodyHash[at] = body.bytes[at];
        }
    }

    if (!binder_.Succeeded(out)) {
        return;
    }

    if (!encoder.EncodeClasses(bodies.View())) {
        return;
    }

    // 모듈 머리(5절): compiler version, import 경로, 원문 content hash, profile. 그리고 verifier —
    // 컴파일러가 낸 모듈의 거부는 내림의 결함이다.
    BytecodeModule& bytecode = c.bytecode_[module];
    bytecode.compiler = encoder.String(SparksCompilerVersion);
    bytecode.path = encoder.String(c.Text(c.modules_[module].importPath));
    const SourceFile& source = c.sources_->Get(c.modules_[module].source);
    const Sha256Digest digest = Sha256::Hash(Span<const uint8>(
        reinterpret_cast<const uint8*>(source.Text().Data()), source.Text().ByteCount()));
    for (usize index = 0; index < BytecodeHashBytes; ++index) {
        bytecode.contentHash[index] = digest.bytes[index];
    }

    // interface hash(6.2절): 공개 표면의 정규 텍스트.
    StringBuilder<CompilationMessageInlineBytes> surface(allocator_);
    AppendInterfaceText(surface, module);
    if (!binder_.Succeeded(surface)) {
        return;
    }

    const Sha256Digest interfaceDigest = Sha256::Hash(Span<const uint8>(
        reinterpret_cast<const uint8*>(surface.View().Data()), surface.View().ByteCount()));
    for (usize index = 0; index < BytecodeHashBytes; ++index) {
        bytecode.interfaceHash[index] = interfaceDigest.bytes[index];
    }

    const uint32 package = c.modules_[module].package;
    bytecode.profile = package != NoPackage && package < c.packages_.Size() &&
                               c.packages_[package].manifest.GetProfile() == Profile::Full
                           ? 1
                           : 0;
    if (binder_.error_ != ErrorCode::None) {
        return;
    }

    if (const BytecodeRejection rejection = VerifyBytecode(bytecode);
        rejection.error != BytecodeLoadError::None) {
        // 인코딩의 결함: 사유와 자리를 진단 출력에 남기고 멈춘다.
        StringBuilder<256> reason(allocator_);
        reason.AppendFormat("Sparks verifier: {} in function {} at {}\n",
                            BytecodeLoadErrorName(rejection.error), rejection.function,
                            rejection.address);
        platform::WriteDiagnostic(reason.View().Data(), reason.ByteCount());
        platform::Fatal("Sparks bytecode rejected by the verifier");
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
    c.lowered_[module] = 1;
}

} // namespace rocks::sparks::detail

namespace rocks::sparks {

const BytecodeModule* Compilation::ModuleBytecode(uint32 module) const noexcept {
    return module < bytecode_.Size() && module < lowered_.Size() && lowered_[module] != 0 &&
                   lastStage_ >= CompilationStage::Codegen
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
    if (module >= irDumps_.Size() || module >= lowered_.Size() || lowered_[module] == 0 ||
        lastStage_ < CompilationStage::Codegen) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    out.Append(Text(irDumps_[module]));
    return out.Status();
}

} // namespace rocks::sparks
