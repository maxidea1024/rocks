#include "CompilationChecker.h"
#include "IrBuilder.h"

// 문장 → typed IR 내림이다(Docs/Design/SparksBytecode.ko.md 3절, LC4a). 반복·switch·`if (let)`·
// `?? return`은 블록과 분기로, 값 타입 경로의 대입은 바깥 값에 다시 쓰기로 내린다. 지역 유지 자원은
// 블록을 떠나는 모든 경로(블록 끝·return·break·continue)에서 선언 역순으로 RELEASE한다(D5, SL §7.1
// 규칙 2).

namespace rocks::sparks::detail {

void IrBuilder::LowerStatement(uint32 node) noexcept {
    if (node == NoNode || Failed()) {
        return;
    }

    switch (tree_.Node(node).kind) {
    case SyntaxKind::Block:
        LowerBlock(node);
        return;
    case SyntaxKind::LocalVariable:
        LowerLocal(node);
        return;
    case SyntaxKind::Assignment:
        LowerAssignment(node);
        return;
    case SyntaxKind::DiscardAssignment:
    case SyntaxKind::ExpressionStatement: {
        const uint32 value = tree_.Child(node, 0);
        static_cast<void>(Lower(value, c_.ExpressionType(function_.module, value)));
        return;
    }
    case SyntaxKind::If:
        LowerIf(node);
        return;
    case SyntaxKind::While:
        LowerWhile(node);
        return;
    case SyntaxKind::For:
        LowerFor(node);
        return;
    case SyntaxKind::Switch:
        LowerSwitch(node);
        return;
    case SyntaxKind::Return:
        LowerReturn(tree_.Child(node, 0), node);
        return;
    case SyntaxKind::Emit:
        LowerEmit(node);
        return;
    case SyntaxKind::Break:
        LowerJump(true, node);
        return;
    case SyntaxKind::Continue:
        LowerJump(false, node);
        return;
    default:
        return;
    }
}

void IrBuilder::LowerBlock(uint32 node) noexcept {
    const usize localMark = locals_.Size();
    const usize heldMark = held_.Size();
    for (const uint32 statement : tree_.Children(node)) {
        LowerStatement(statement);
    }

    if (!Terminated()) {
        ReleaseHeld(heldMark, node);
    }

    // 축소는 실패하지 않는다.
    static_cast<void>(locals_.TryResize(localMark));
    static_cast<void>(held_.TryResize(heldMark));
}

void IrBuilder::ReleaseHeld(usize mark, uint32 node) noexcept {
    for (usize index = held_.Size(); index > mark; --index) {
        Add(IrOp::Release, held_[index - 1], NoIrRegister, NoIrRegister, 0, node);
    }
}

void IrBuilder::LowerLocal(uint32 node) noexcept {
    const uint32 nameNode = tree_.Child(node, 0);
    const uint32 valueNode = tree_.Child(node, 2);
    const uint32 exit = tree_.Child(node, 3);
    const uint32 symbol = c_.NodeSymbol(function_.module, nameNode);
    const uint32 type = symbol == NoSymbol ? NoType : c_.symbols_[symbol].type;
    const StringView name = checker_.binder_.NodeText(function_.module, nameNode);
    if (exit == NoNode) {
        const uint32 value = Coerce(Lower(valueNode, type), type, valueNode);
        const uint32 local = NewValue(type, name);
        MoveValue(local, value, node);
        if (symbol != NoSymbol) {
            BindLocal(symbol, local);
        }

        if (local != NoIrRegister && checker_.IsHeldType(type)) {
            if (auto pushed = held_.TryPushBack(local); !pushed) {
                Fail(pushed.Error());
            }
        }

        return;
    }

    // `let x = e ?? return r;`: e가 없으면 r로 반환, 있으면 그 값.
    const uint32 optional = Lower(valueNode, c_.ExpressionType(function_.module, valueNode));
    const uint32 some = NewValue(checker_.boolType_);
    Add(IrOp::IsSome, some, optional, NoIrRegister, 0, node);
    const uint32 present = NewBlock();
    const uint32 absent = NewBlock();
    Add(IrOp::Branch, present, some, absent, 0, node);
    StartBlock(absent);
    LowerReturn(tree_.Child(exit, 0), exit);
    StartBlock(present);
    const uint32 local = NewValue(type, name);
    MoveValue(local, Unwrap(optional, type, node), node);
    if (symbol != NoSymbol) {
        BindLocal(symbol, local);
    }

    if (local != NoIrRegister && checker_.IsHeldType(type)) {
        if (auto pushed = held_.TryPushBack(local); !pushed) {
            Fail(pushed.Error());
        }
    }
}

void IrBuilder::LowerAssignment(uint32 node) noexcept {
    const uint32 target = tree_.Child(node, 0);
    const uint32 valueNode = tree_.Child(node, 1);
    const StringView op = checker_.TokenText(function_.module, node);
    const uint32 type = c_.ExpressionType(function_.module, target);
    if (op == "=") {
        Store(target, Coerce(Lower(valueNode, type), type, valueNode));
        return;
    }

    // 복합 대입 `a op= b`: 현재 값을 읽고 연산한 뒤 쓴다(대상 경로는 한 번 더 읽는다 — 경로의 식은
    // 부작용이 없다: 이름·필드·인덱스).
    const uint32 current = Lower(target, type);
    const uint32 right = Lower(valueNode, c_.ExpressionType(function_.module, valueNode) != NoType
                                              ? c_.ExpressionType(function_.module, valueNode)
                                              : type);
    const uint32 result = NewValue(type);
    if (current == NoIrRegister || right == NoIrRegister || result == NoIrRegister) {
        return;
    }

    // 복합 대입은 Scalar 값뿐이다(문자열 `+=`는 없다).
    const IrArith kind = op == "+="   ? IrArith::Add
                         : op == "-=" ? IrArith::Sub
                         : op == "*=" ? IrArith::Mul
                                      : IrArith::Div;
    for (uint32 slot = 0; slot < SlotCount(result); ++slot) {
        const uint32 b = right + (SlotCount(right) == 1 ? 0 : slot);
        Add(IrOp::Binary, result + slot, current + slot, b, 0, node, static_cast<uint8>(kind));
    }

    Store(target, result);
}

void IrBuilder::Store(uint32 target, uint32 value) noexcept {
    while (target != NoNode && tree_.Node(target).kind == SyntaxKind::Parenthesized) {
        target = tree_.Child(target, 0);
    }

    if (target == NoNode || value == NoIrRegister || Failed()) {
        return;
    }

    const SyntaxNode& entry = tree_.Node(target);
    const uint32 symbol = c_.NodeSymbol(function_.module, target);
    if (entry.kind == SyntaxKind::Identifier) {
        if (symbol == NoSymbol) {
            return;
        }

        const SymbolKind kind = c_.symbols_[symbol].kind;
        if (kind == SymbolKind::Local || kind == SymbolKind::Parameter) {
            const uint32 local = LocalRegister(symbol);
            // 유지 자원 지역: 새 값을 얻은 뒤 이전 값을 해제하고 저장한다(SL §7.1).
            if (local != NoIrRegister && checker_.IsHeldType(function_.registers[local].type)) {
                Add(IrOp::Release, local, NoIrRegister, NoIrRegister, 0, target);
            }

            MoveValue(local, value, target);
            return;
        }

        Add(IrOp::SetField, this_, value, NoIrRegister, symbol, target);
        return;
    }

    if (entry.kind == SyntaxKind::Member) {
        const uint32 objectNode = tree_.Child(target, 0);
        const uint32 objectType = c_.ExpressionType(function_.module, objectNode);
        const uint32 object = Lower(objectNode, objectType);
        if (object == NoIrRegister || symbol == NoSymbol) {
            return;
        }

        uint32 offset = 0;
        uint32 count = 0;
        if (MathFieldSlots(objectType, c_.symbols_[symbol].name, offset, count)) {
            for (uint32 slot = 0; slot < count; ++slot) {
                Add(IrOp::Move, object + offset + slot, value + slot, NoIrRegister, 0, target);
            }

            Store(objectNode, object);
            return;
        }

        Add(IrOp::SetField, object, value, NoIrRegister, symbol, target);
        if (function_.registers[object].bank != IrBank::Ref) {
            Store(objectNode, object); // 값 struct: 바꾼 값을 바깥 경로에 다시 쓴다
        }

        return;
    }

    if (entry.kind == SyntaxKind::Index) {
        const uint32 objectNode = tree_.Child(target, 0);
        const uint32 objectType = c_.ExpressionType(function_.module, objectNode);
        const uint32 object = Lower(objectNode, objectType);
        const uint32 keyNode = tree_.Child(target, 1);
        uint32 keyType = checker_.intType_;
        if (objectType != NoType && checker_.IsLanguage(objectType, "Map")) {
            keyType = checker_.Argument(objectType, 0);
        }

        const uint32 key = Coerce(Lower(keyNode, keyType), keyType, keyNode);
        Add(IrOp::SetElement, object, key, value, 0, target);
        Store(objectNode, object);
    }
}

void IrBuilder::LowerIf(uint32 node) noexcept {
    const uint32 condition = tree_.Child(node, 0);
    const uint32 thenNode = tree_.Child(node, 1);
    const uint32 elseNode = tree_.Child(node, 2);
    const uint32 thenBlock = NewBlock();
    const uint32 elseBlock = elseNode == NoNode ? NoIrBlock : NewBlock();
    const uint32 done = NewBlock();
    const usize localMark = locals_.Size();
    uint32 optional = NoIrRegister;
    if (condition != NoNode && tree_.Node(condition).kind == SyntaxKind::IfLet) {
        optional = Lower(tree_.Child(condition, 1),
                         c_.ExpressionType(function_.module, tree_.Child(condition, 1)));
        const uint32 some = NewValue(checker_.boolType_);
        Add(IrOp::IsSome, some, optional, NoIrRegister, 0, condition);
        Add(IrOp::Branch, thenBlock, some, elseNode == NoNode ? done : elseBlock, 0, condition);
    } else {
        LowerCondition(condition, thenBlock, elseNode == NoNode ? done : elseBlock);
    }

    StartBlock(thenBlock);
    if (optional != NoIrRegister) {
        // `if (let x = e)`: 참 분기에서만 보이는 x에 e의 값을 둔다.
        const uint32 nameNode = tree_.Child(condition, 0);
        const uint32 symbol = c_.NodeSymbol(function_.module, nameNode);
        const uint32 type = symbol == NoSymbol ? NoType : c_.symbols_[symbol].type;
        const uint32 local = NewValue(type, checker_.binder_.NodeText(function_.module, nameNode));
        MoveValue(local, Unwrap(optional, type, condition), condition);
        if (symbol != NoSymbol) {
            BindLocal(symbol, local);
        }
    }

    LowerStatement(thenNode);
    static_cast<void>(locals_.TryResize(localMark));
    if (!Terminated()) {
        Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    }

    if (elseNode != NoNode) {
        StartBlock(elseBlock);
        LowerStatement(elseNode);
        if (!Terminated()) {
            Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
        }
    }

    StartBlock(done);
}

void IrBuilder::LowerWhile(uint32 node) noexcept {
    const uint32 head = NewBlock();
    const uint32 body = NewBlock();
    const uint32 exit = NewBlock();
    Add(IrOp::Jump, head, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(head);
    LowerCondition(tree_.Child(node, 0), body, exit);
    StartBlock(body);
    if (auto pushed = loops_.TryPushBack(Loop{exit, head, held_.Size()}); !pushed) {
        Fail(pushed.Error());
        return;
    }

    LowerStatement(tree_.Child(node, 1));
    static_cast<void>(loops_.TryPopBack());
    if (!Terminated()) {
        Add(IrOp::Jump, head, NoIrRegister, NoIrRegister, 0, node);
    }

    StartBlock(exit);
}

void IrBuilder::LowerFor(uint32 node) noexcept {
    // 숨은 번호 i를 0(범위는 시작)부터 끝 전까지 올리며, 회차마다 이름에 값을 둔다. 컨테이너는 시작
    // 때 값을 잡아 둔 것을 돈다(COW — 본문의 변경은 반복에 보이지 않는다).
    const uint32 first = tree_.Child(node, 0);
    const uint32 second = tree_.Child(node, 1);
    const uint32 sourceNode = tree_.Child(node, 2);
    const uint32 endNode = tree_.Child(node, 3);
    const uint32 bodyNode = tree_.Child(node, 4);
    const usize localMark = locals_.Size();
    auto binder = [&](uint32 nameNode) noexcept {
        const uint32 symbol = nameNode == NoNode || tree_.Node(nameNode).kind != SyntaxKind::Name
                                  ? NoSymbol
                                  : c_.NodeSymbol(function_.module, nameNode);
        if (symbol == NoSymbol) {
            return NoIrRegister;
        }

        const uint32 local = NewValue(c_.symbols_[symbol].type,
                                      checker_.binder_.NodeText(function_.module, nameNode));
        BindLocal(symbol, local);
        return local;
    };

    const bool range = endNode != NoNode;
    uint32 indexType = checker_.intType_;
    uint32 source = NoIrRegister;
    uint32 limit = NoIrRegister;
    uint32 index = NoIrRegister;
    if (range) {
        const uint32 firstSymbol = c_.NodeSymbol(function_.module, first);
        indexType = firstSymbol == NoSymbol ? checker_.intType_ : c_.symbols_[firstSymbol].type;
        index = NewValue(indexType);
        MoveValue(index, Coerce(Lower(sourceNode, indexType), indexType, sourceNode), node);
        limit = NewValue(indexType);
        MoveValue(limit, Coerce(Lower(endNode, indexType), indexType, endNode), node);
    } else {
        const uint32 sourceType = c_.ExpressionType(function_.module, sourceNode);
        source = NewValue(sourceType);
        MoveValue(source, Lower(sourceNode, sourceType), node);
        index = NewValue(indexType);
        Add(IrOp::LoadInt, index, 0, NoIrRegister, 0, node);
        limit = NewValue(indexType);
        Add(IrOp::Length, limit, source, NoIrRegister, 0, node);
    }

    const uint32 head = NewBlock();
    const uint32 body = NewBlock();
    const uint32 step = NewBlock();
    const uint32 exit = NewBlock();
    Add(IrOp::Jump, head, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(head);
    const uint32 more = NewValue(checker_.boolType_);
    Add(IrOp::Compare, more, index, limit, 1, node, static_cast<uint8>(IrCompare::Less));
    Add(IrOp::Branch, body, more, exit, 0, node);
    StartBlock(body);
    const uint32 a = binder(first);
    const uint32 b = binder(second);
    if (range) {
        MoveValue(a, index, node);
    } else {
        const uint32 sourceType = function_.registers[source].type;
        const bool map = checker_.IsLanguage(checker_.NonOptional(sourceType), "Map");
        const bool set = checker_.IsLanguage(checker_.NonOptional(sourceType), "Set");
        if (map || set) {
            // Map: (key[, 값]), Set: 원소.
            if (a != NoIrRegister) {
                Add(IrOp::GetEntry, a, source, index, 0, first, IrEntryKey);
            }

            if (b != NoIrRegister) {
                Add(IrOp::GetEntry, b, source, index, 0, second, IrEntryValue);
            }
        } else if (second != NoNode) {
            // Array 이름 둘: (번호, 원소).
            MoveValue(a, index, node);
            if (b != NoIrRegister) {
                Add(IrOp::GetElement, b, source, index, 0, second);
            }
        } else if (a != NoIrRegister) {
            Add(IrOp::GetElement, a, source, index, 0, first);
        }
    }

    if (auto pushed = loops_.TryPushBack(Loop{exit, step, held_.Size()}); !pushed) {
        Fail(pushed.Error());
        return;
    }

    LowerStatement(bodyNode);
    static_cast<void>(loops_.TryPopBack());
    if (!Terminated()) {
        Add(IrOp::Jump, step, NoIrRegister, NoIrRegister, 0, node);
    }

    StartBlock(step);
    const uint32 one = NewValue(indexType);
    Add(IrOp::LoadInt, one, 1, NoIrRegister, 0, node);
    Add(IrOp::Binary, index, index, one, 0, node, static_cast<uint8>(IrArith::Add));
    Add(IrOp::Jump, head, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(exit);
    static_cast<void>(locals_.TryResize(localMark));
}

void IrBuilder::LowerSwitch(uint32 node) noexcept {
    const uint32 subjectNode = tree_.Child(node, 0);
    const uint32 subjectType = c_.ExpressionType(function_.module, subjectNode);
    const uint32 subject = Lower(subjectNode, subjectType);
    const uint32 cases = tree_.Child(node, 1);
    const uint32 defaultNode = tree_.Child(node, 2);
    const Span<const uint32> list = cases != NoNode && tree_.Node(cases).kind == SyntaxKind::List
                                        ? tree_.Children(cases)
                                        : Span<const uint32>();
    const uint32 done = NewBlock();
    const uint32 otherwise = defaultNode == NoNode ? done : NewBlock();
    Array<uint32> blocks(checker_.allocator_);
    for (usize index = 0; index < list.Size(); ++index) {
        if (auto pushed = blocks.TryPushBack(NewBlock()); !pushed) {
            Fail(pushed.Error());
            return;
        }
    }

    // 값이 모두 정해진 정수·enum이면 표(SWITCH), 아니면 차례 비교(문자열 등).
    const bool scalar = subject != NoIrRegister && SlotCount(subject) == 1 &&
                        function_.registers[subject].bank == IrBank::Scalar;
    bool table = scalar;
    const usize firstCase = function_.cases.Size();
    for (usize index = 0; index < list.Size() && table; ++index) {
        const uint32 values = tree_.Child(list[index], 0);
        for (const uint32 valueNode : tree_.Children(values)) {
            ConstantValue value;
            const uint32 symbol = c_.NodeSymbol(function_.module, valueNode);
            int64 number = 0;
            if (symbol != NoSymbol && c_.symbols_[symbol].kind == SymbolKind::EnumCase) {
                number = static_cast<int64>(EnumIndex(symbol));
            } else if (EvaluateConstant(valueNode, value) &&
                       value.kind == ConstantValue::Kind::Integer) {
                number = value.negative ? -static_cast<int64>(value.magnitude)
                                        : static_cast<int64>(value.magnitude);
            } else if (symbol != NoSymbol && c_.symbols_[symbol].kind == SymbolKind::Const &&
                       checker_.ConstOf(symbol).kind == ConstantValue::Kind::Enum) {
                number = static_cast<int64>(EnumIndex(checker_.ConstOf(symbol).symbol));
            } else {
                table = false;
                break;
            }

            if (auto pushed = function_.cases.TryPushBack(IrCase{number, blocks[index]}); !pushed) {
                Fail(pushed.Error());
                return;
            }
        }
    }

    if (table) {
        Add(IrOp::Switch, static_cast<uint32>(firstCase), subject,
            static_cast<uint32>(function_.cases.Size() - firstCase), otherwise, node);
    } else {
        static_cast<void>(function_.cases.TryResize(firstCase));
        for (usize index = 0; index < list.Size(); ++index) {
            const uint32 values = tree_.Child(list[index], 0);
            for (const uint32 valueNode : tree_.Children(values)) {
                const uint32 value = Coerce(Lower(valueNode, subjectType), subjectType, valueNode);
                const uint32 equal = NewValue(checker_.boolType_);
                Add(IrOp::Compare, equal, subject, value, SlotCount(subject), valueNode,
                    static_cast<uint8>(IrCompare::Equal));
                const uint32 next = NewBlock();
                Add(IrOp::Branch, blocks[index], equal, next, 0, valueNode);
                StartBlock(next);
            }
        }

        Add(IrOp::Jump, otherwise, NoIrRegister, NoIrRegister, 0, node);
    }

    for (usize index = 0; index < list.Size(); ++index) {
        StartBlock(blocks[index]);
        LowerStatement(tree_.Child(list[index], 1));
        if (!Terminated()) {
            Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, list[index]);
        }
    }

    if (defaultNode != NoNode) {
        StartBlock(otherwise);
        LowerStatement(tree_.Child(defaultNode, 0));
        if (!Terminated()) {
            Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, defaultNode);
        }
    }

    StartBlock(done);
}

void IrBuilder::LowerReturn(uint32 valueNode, uint32 node) noexcept {
    const bool result = returnType_ != NoType && returnType_ != checker_.voidType_ &&
                        checker_.IsLanguage(returnType_, "Result");
    uint32 value = NoIrRegister;
    if (result) {
        // Result<T>: T 값이면 성공, `Error` 값이면 실패, 값 없는 return(Result<void>)은 성공.
        const uint32 inner = checker_.Argument(returnType_, 0);
        const uint32 valueType =
            valueNode == NoNode ? NoType : c_.ExpressionType(function_.module, valueNode);
        const uint32 valueSymbol = valueType == NoType ? NoSymbol : checker_.SymbolOf(valueType);
        const bool failure =
            valueSymbol != NoSymbol && c_.symbols_[valueSymbol].kind == SymbolKind::EngineType &&
            c_.symbols_[valueSymbol].name == "Error" && checker_.SymbolOf(inner) != valueSymbol;
        const uint32 payload = valueNode == NoNode
                                   ? NoIrRegister
                                   : Coerce(Lower(valueNode, failure ? valueType : inner),
                                            failure ? valueType : inner, valueNode);
        value = NewValue(returnType_);
        if (value != NoIrRegister && function_.registers[value].bank == IrBank::Scalar) {
            // 앞 slot이 오류 코드(0 성공, 아니면 Error case 번호 + 1).
            if (failure) {
                const uint32 one = NewValue(checker_.intType_);
                Add(IrOp::LoadInt, one, 1, NoIrRegister, 0, node);
                Add(IrOp::Binary, value, payload, one, 0, node, static_cast<uint8>(IrArith::Add));
                for (uint32 slot = 1; slot < SlotCount(value); ++slot) {
                    Add(IrOp::LoadInt, value + slot, 0, NoIrRegister, 0, node);
                }
            } else {
                Add(IrOp::LoadInt, value, 0, NoIrRegister, 0, node);
                for (uint32 slot = 1; slot < SlotCount(value) && payload != NoIrRegister; ++slot) {
                    Add(IrOp::Move, value + slot, payload + slot - 1, NoIrRegister, 0, node);
                }
            }
        } else if (value != NoIrRegister) {
            const uint32 count = payload == NoIrRegister ? 0 : 1;
            Add(IrOp::New, value, AddOperands(Span<const uint32>(&payload, count)), count,
                checker_.SymbolOf(returnType_), node, failure ? IrNewResultError : IrNewResultOk);
        }
    } else if (valueNode != NoNode) {
        value = Coerce(Lower(valueNode, returnType_), returnType_, valueNode);
    } else if (node == NoNode && returnType_ != NoType && returnType_ != checker_.voidType_) {
        // 반환 타입이 있는 함수의 본문 끝(도달하지 않는다 — S0409가 막았다).
        Add(IrOp::Fault, NoIrRegister, NoIrRegister, NoIrRegister, IrFaultUnreachable, node);
        return;
    }

    ReleaseHeld(0, node);
    Add(IrOp::Return, NoIrRegister, value, NoIrRegister, 0, node);
}

void IrBuilder::LowerEmit(uint32 node) noexcept {
    const uint32 path = tree_.Child(node, 0);
    const uint32 list = tree_.Child(node, 1);
    if (path == NoNode || tree_.Node(path).childCount == 0) {
        return;
    }

    const Span<const uint32> segments = tree_.Children(path);
    const uint32 last = segments[segments.Size() - 1];
    const uint32 event = c_.NodeSymbol(function_.module, last);
    if (event == NoSymbol) {
        return;
    }

    // 대상: 이름 하나면 자기 객체(this), `this.E`면 this, `x.E`면 x의 값.
    uint32 target = NoIrRegister;
    if (segments.Size() == 2) {
        const uint32 first = segments[0];
        if (tree_.Node(first).kind == SyntaxKind::This) {
            target = this_;
        } else {
            const uint32 symbol = c_.NodeSymbol(function_.module, first);
            const SymbolKind kind =
                symbol == NoSymbol ? SymbolKind::BuiltinType : c_.symbols_[symbol].kind;
            if (kind == SymbolKind::Local || kind == SymbolKind::Parameter) {
                target = LocalRegister(symbol);
            } else if (symbol != NoSymbol) {
                const uint32 type = kind == SymbolKind::Field ? checker_.FieldType(symbol)
                                                              : c_.symbols_[symbol].type;
                target = NewValue(type);
                Add(IrOp::GetField, target, kind == SymbolKind::ContextName ? NoIrRegister : this_,
                    NoIrRegister, symbol, first);
            }
        }
    }

    const uint32 signature = c_.symbols_[event].signature;
    Array<uint32> values(checker_.allocator_);
    const Span<const uint32> arguments = list != NoNode && tree_.Node(list).kind == SyntaxKind::List
                                             ? tree_.Children(list)
                                             : Span<const uint32>();
    for (usize index = 0; index < arguments.Size(); ++index) {
        uint32 parameterType = NoType;
        if (signature != NoSignature && index < c_.signatures_[signature].parameterCount) {
            parameterType =
                c_.signatureParameters_[c_.signatures_[signature].firstParameter + index];
        }

        const uint32 value =
            Coerce(Lower(arguments[index], parameterType), parameterType, arguments[index]);
        if (auto pushed = values.TryPushBack(value); !pushed) {
            Fail(pushed.Error());
            return;
        }
    }

    Add(IrOp::Emit, target, AddOperands(values.View()), static_cast<uint32>(values.Size()), event,
        node);
}

void IrBuilder::LowerJump(bool isBreak, uint32 node) noexcept {
    if (loops_.IsEmpty()) {
        return;
    }

    const Loop& loop = loops_[loops_.Size() - 1];
    ReleaseHeld(loop.heldMark, node);
    Add(IrOp::Jump, isBreak ? loop.breakBlock : loop.continueBlock, NoIrRegister, NoIrRegister, 0,
        node);
}

} // namespace rocks::sparks::detail
