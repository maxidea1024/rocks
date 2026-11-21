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

    switch (tree_->Node(node).kind) {
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
        const uint32 value = tree_->Child(node, 0);
        static_cast<void>(Lower(value, c_.ExpressionType(module_, value)));
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
        LowerReturn(tree_->Child(node, 0), node);
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
    for (const uint32 statement : tree_->Children(node)) {
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
    const uint32 nameNode = tree_->Child(node, 0);
    const uint32 valueNode = tree_->Child(node, 2);
    const uint32 exit = tree_->Child(node, 3);
    const uint32 symbol = c_.NodeSymbol(module_, nameNode);
    const uint32 type = symbol == NoSymbol ? NoType : c_.symbols_[symbol].type;
    const StringView name = checker_.binder_.NodeText(module_, nameNode);
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
    const uint32 optional = Lower(valueNode, c_.ExpressionType(module_, valueNode));
    const uint32 some = NewValue(checker_.boolType_);
    Add(IrOp::IsSome, some, optional, NoIrRegister, 0, node);
    const uint32 present = NewBlock();
    const uint32 absent = NewBlock();
    Add(IrOp::Branch, present, some, absent, 0, node);
    StartBlock(absent);
    LowerReturn(tree_->Child(exit, 0), exit);
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

namespace {

// 값 경로 단계의 종류다(IrBuilder::PathStep::kind).
enum PathStepKind : uint8 {
    PathRoot,   // 경로가 아닌 값(호출 결과 등) — 다시 쓰지 않는다
    PathLocal,  // 지역 이름·매개변수
    PathField,  // 객체(Ref)·struct 값(Buf)의 필드
    PathSlots,  // 수학 타입 값의 성분 slot 구간
    PathElement // 컨테이너 원소
};

} // namespace

void IrBuilder::LowerAssignment(uint32 node) noexcept {
    // 평가 순서는 왼쪽에서 오른쪽(SL §3.3): 대상 경로의 객체·key를 먼저 한 번 평가하고, 오른쪽 값을
    // 계산한 뒤 쓴다. 유지 자원은 새 값 획득(오른쪽) → 이전 값 해제 → 저장 순이다(SL §7.1).
    const uint32 target = tree_->Child(node, 0);
    const uint32 valueNode = tree_->Child(node, 1);
    const StringView op = checker_.TokenText(module_, node);
    const uint32 type = c_.ExpressionType(module_, target);
    Array<PathStep> path(checker_.allocator_);
    if (op == "=") {
        static_cast<void>(EvaluatePath(target, path, false));
        StorePath(path.View(), Coerce(Lower(valueNode, type), type, valueNode));
        return;
    }

    // 복합 대입 `a op= b`: 경로의 현재 값을 읽고 연산한 뒤 같은 경로에 쓴다(Scalar 값뿐 — 문자열
    // `+=`는 없다).
    const uint32 current = EvaluatePath(target, path, true);
    const uint32 rightType = c_.ExpressionType(module_, valueNode);
    const uint32 right = Lower(valueNode, rightType != NoType ? rightType : type);
    const uint32 result = NewValue(type);
    if (current == NoIrRegister || right == NoIrRegister || result == NoIrRegister) {
        return;
    }

    const IrArith kind = op == "+="   ? IrArith::Add
                         : op == "-=" ? IrArith::Sub
                         : op == "*=" ? IrArith::Mul
                                      : IrArith::Div;
    for (uint32 slot = 0; slot < SlotCount(result); ++slot) {
        const uint32 b = right + (SlotCount(right) == 1 ? 0 : slot);
        Add(IrOp::Binary, result + slot, current + slot, b, 0, node, static_cast<uint8>(kind));
    }

    StorePath(path.View(), result);
}

uint32 IrBuilder::EvaluatePath(uint32 node, Array<PathStep>& path, bool readLeaf) noexcept {
    while (node != NoNode && tree_->Node(node).kind == SyntaxKind::Parenthesized) {
        node = tree_->Child(node, 0);
    }

    if (node == NoNode || Failed()) {
        return NoIrRegister;
    }

    PathStep step;
    step.node = node;
    const SyntaxNode& entry = tree_->Node(node);
    const uint32 symbol = c_.NodeSymbol(module_, node);
    const uint32 type = c_.ExpressionType(module_, node);
    auto push = [&]() noexcept {
        if (auto pushed = path.TryPushBack(step); !pushed) {
            Fail(pushed.Error());
        }
    };
    const SymbolKind kind = symbol == NoSymbol ? SymbolKind::BuiltinType : c_.symbols_[symbol].kind;
    if (entry.kind == SyntaxKind::Identifier &&
        (kind == SymbolKind::Local || kind == SymbolKind::Parameter)) {
        step.kind = PathLocal;
        step.symbol = symbol;
        push();
        return LocalRegister(symbol);
    }

    if (entry.kind == SyntaxKind::Identifier &&
        (kind == SymbolKind::Field || kind == SymbolKind::EngineField)) {
        step.kind = PathField;
        step.symbol = symbol;
        step.object = this_;
        push();
        if (!readLeaf) {
            return NoIrRegister;
        }

        const uint32 value = NewValue(type);
        Add(IrOp::GetField, value, this_, NoIrRegister, symbol, node);
        return value;
    }

    if (entry.kind == SyntaxKind::Member && symbol != NoSymbol &&
        (kind == SymbolKind::Field || kind == SymbolKind::EngineField ||
         kind == SymbolKind::LanguageField)) {
        // 객체가 값(struct Buf·수학 Scalar)이면 그 경로도 이어서 평가해 다시 쓴다. 객체 참조(Ref)면
        // 여기서 경로가 시작된다.
        const uint32 objectNode = tree_->Child(node, 0);
        const uint32 objectType = c_.ExpressionType(module_, objectNode);
        const bool value = objectType != NoType && Layout(checker_, objectType).bank != IrBank::Ref;
        step.chained = value;
        step.object = value ? EvaluatePath(objectNode, path, true) : Lower(objectNode, NoType);
        step.symbol = symbol;
        if (step.object == NoIrRegister) {
            return NoIrRegister;
        }

        if (MathFieldSlots(objectType, c_.symbols_[symbol].name, step.offset, step.count)) {
            step.kind = PathSlots;
            push();
            if (!readLeaf) {
                return NoIrRegister;
            }

            const uint32 copy = NewValue(type);
            for (uint32 slot = 0; slot < step.count; ++slot) {
                Add(IrOp::Move, copy + slot, step.object + step.offset + slot, NoIrRegister, 0,
                    node);
            }

            return copy;
        }

        step.kind = PathField;
        push();
        if (!readLeaf) {
            return NoIrRegister;
        }

        const uint32 read = NewValue(type);
        Add(IrOp::GetField, read, step.object, NoIrRegister, symbol, node);
        return read;
    }

    if (entry.kind == SyntaxKind::Index) {
        // 컨테이너는 값(Buf)이다: 경로를 이어서 평가하고, key는 한 번 계산한다. 읽은 원소(`T?`)는
        // 꺼낸 T로 다음 단계에 넘긴다(쓰기 경로의 원소는 있는 것으로 본다 — 없으면 실행 중 fault).
        const uint32 objectNode = tree_->Child(node, 0);
        const uint32 objectType = c_.ExpressionType(module_, objectNode);
        step.kind = PathElement;
        step.chained = true;
        step.object = EvaluatePath(objectNode, path, true);
        uint32 keyType = checker_.intType_;
        if (objectType != NoType && checker_.IsLanguage(objectType, "Map")) {
            keyType = checker_.Argument(objectType, 0);
        }

        const uint32 keyNode = tree_->Child(node, 1);
        step.key = Coerce(Lower(keyNode, keyType), keyType, keyNode);
        push();
        if (!readLeaf || step.object == NoIrRegister) {
            return NoIrRegister;
        }

        const uint32 read = NewValue(type);
        Add(IrOp::GetElement, read, step.object, step.key, 0, node);
        return type != NoType && checker_.IsOptional(type)
                   ? Unwrap(read, checker_.NonOptional(type), node)
                   : read;
    }

    // 경로가 아닌 값(호출 결과·`this`·문맥 이름): 한 번 평가하고 다시 쓰지 않는다.
    step.kind = PathRoot;
    push();
    return Lower(node, type);
}

void IrBuilder::StorePath(Span<const PathStep> path, uint32 value) noexcept {
    for (usize index = path.Size(); index > 0 && value != NoIrRegister && !Failed(); --index) {
        const PathStep& step = path[index - 1];
        switch (step.kind) {
        case PathLocal: {
            const uint32 local = LocalRegister(step.symbol);
            // 유지 자원 지역: 새 값을 얻은 뒤 이전 값을 해제하고 저장한다(SL §7.1).
            if (local != NoIrRegister && checker_.IsHeldType(function_.registers[local].type)) {
                Add(IrOp::Release, local, NoIrRegister, NoIrRegister, 0, step.node);
            }

            MoveValue(local, value, step.node);
            return;
        }
        case PathField:
            // 유지 자원 필드의 SETF는 이전 값을 해제한다(실행 중 — 필드 타입이 정한다, 3.1a절).
            Add(IrOp::SetField, step.object, value, NoIrRegister, step.symbol, step.node);
            break;
        case PathSlots:
            for (uint32 slot = 0; slot < step.count; ++slot) {
                Add(IrOp::Move, step.object + step.offset + slot, value + slot, NoIrRegister, 0,
                    step.node);
            }

            break;
        case PathElement:
            Add(IrOp::SetElement, step.object, step.key, value, 0, step.node);
            break;
        default:
            return; // PathRoot
        }

        if (!step.chained) {
            return;
        }

        value = step.object; // 바꾼 값 타입을 바깥 경로에 다시 쓴다
    }
}

void IrBuilder::LowerIf(uint32 node) noexcept {
    const uint32 condition = tree_->Child(node, 0);
    const uint32 thenNode = tree_->Child(node, 1);
    const uint32 elseNode = tree_->Child(node, 2);
    const uint32 thenBlock = NewBlock();
    const uint32 elseBlock = elseNode == NoNode ? NoIrBlock : NewBlock();
    const uint32 done = NewBlock();
    const usize localMark = locals_.Size();
    const usize heldMark = held_.Size();
    uint32 optional = NoIrRegister;
    if (condition != NoNode && tree_->Node(condition).kind == SyntaxKind::IfLet) {
        optional = Lower(tree_->Child(condition, 1),
                         c_.ExpressionType(module_, tree_->Child(condition, 1)));
        const uint32 some = NewValue(checker_.boolType_);
        Add(IrOp::IsSome, some, optional, NoIrRegister, 0, condition);
        Add(IrOp::Branch, thenBlock, some, elseNode == NoNode ? done : elseBlock, 0, condition);
    } else {
        LowerCondition(condition, thenBlock, elseNode == NoNode ? done : elseBlock);
    }

    StartBlock(thenBlock);
    if (optional != NoIrRegister) {
        // `if (let x = e)`: 참 분기에서만 보이는 x에 e의 값을 둔다.
        const uint32 nameNode = tree_->Child(condition, 0);
        const uint32 symbol = c_.NodeSymbol(module_, nameNode);
        const uint32 type = symbol == NoSymbol ? NoType : c_.symbols_[symbol].type;
        const uint32 local = NewValue(type, checker_.binder_.NodeText(module_, nameNode));
        MoveValue(local, Unwrap(optional, type, condition), condition);
        if (symbol != NoSymbol) {
            BindLocal(symbol, local);
        }

        // 유지 자원을 담은 binding은 then 블록이 끝나면 멈춘다(SL §3.14) — return·break는
        // held_로 함께 해제한다.
        if (local != NoIrRegister && checker_.IsHeldType(type)) {
            if (auto pushed = held_.TryPushBack(local); !pushed) {
                Fail(pushed.Error());
                return;
            }
        }
    }

    LowerStatement(thenNode);
    static_cast<void>(locals_.TryResize(localMark));
    if (!Terminated()) {
        ReleaseHeld(heldMark, thenNode);
        Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    }

    static_cast<void>(held_.TryResize(heldMark));

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
    LowerCondition(tree_->Child(node, 0), body, exit);
    StartBlock(body);
    if (auto pushed = loops_.TryPushBack(Loop{exit, head, held_.Size()}); !pushed) {
        Fail(pushed.Error());
        return;
    }

    LowerStatement(tree_->Child(node, 1));
    static_cast<void>(loops_.TryPopBack());
    if (!Terminated()) {
        Add(IrOp::Jump, head, NoIrRegister, NoIrRegister, 0, node);
    }

    StartBlock(exit);
}

void IrBuilder::LowerFor(uint32 node) noexcept {
    // 숨은 번호 i를 0(범위는 시작)부터 끝 전까지 올리며, 회차마다 이름에 값을 둔다. 컨테이너는 시작
    // 때 값을 잡아 둔 것을 돈다(COW — 본문의 변경은 반복에 보이지 않는다).
    const uint32 first = tree_->Child(node, 0);
    const uint32 second = tree_->Child(node, 1);
    const uint32 sourceNode = tree_->Child(node, 2);
    const uint32 endNode = tree_->Child(node, 3);
    const uint32 bodyNode = tree_->Child(node, 4);
    const usize localMark = locals_.Size();
    auto binder = [&](uint32 nameNode) noexcept {
        const uint32 symbol = nameNode == NoNode || tree_->Node(nameNode).kind != SyntaxKind::Name
                                  ? NoSymbol
                                  : c_.NodeSymbol(module_, nameNode);
        if (symbol == NoSymbol) {
            return NoIrRegister;
        }

        const uint32 local =
            NewValue(c_.symbols_[symbol].type, checker_.binder_.NodeText(module_, nameNode));
        BindLocal(symbol, local);
        return local;
    };

    const bool range = endNode != NoNode;
    uint32 indexType = checker_.intType_;
    uint32 source = NoIrRegister;
    uint32 limit = NoIrRegister;
    uint32 index = NoIrRegister;
    if (range) {
        // 범위의 타입은 이름의 타입(검사기가 정했다), 이름이 `_`면 끝·시작 식의 타입이다.
        const uint32 firstSymbol = first == NoNode || tree_->Node(first).kind != SyntaxKind::Name
                                       ? NoSymbol
                                       : c_.NodeSymbol(module_, first);
        const uint32 endType = c_.ExpressionType(module_, endNode);
        const uint32 startType = c_.ExpressionType(module_, sourceNode);
        indexType = firstSymbol != NoSymbol ? c_.symbols_[firstSymbol].type
                    : endType != NoType     ? endType
                    : startType != NoType   ? startType
                                            : checker_.intType_;
        index = NewValue(indexType);
        MoveValue(index, Coerce(Lower(sourceNode, indexType), indexType, sourceNode), node);
        limit = NewValue(indexType);
        MoveValue(limit, Coerce(Lower(endNode, indexType), indexType, endNode), node);
    } else {
        const uint32 sourceType = c_.ExpressionType(module_, sourceNode);
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
    const uint32 subjectNode = tree_->Child(node, 0);
    const uint32 subjectType = c_.ExpressionType(module_, subjectNode);
    const uint32 subject = Lower(subjectNode, subjectType);
    const uint32 cases = tree_->Child(node, 1);
    const uint32 defaultNode = tree_->Child(node, 2);
    const Span<const uint32> list = cases != NoNode && tree_->Node(cases).kind == SyntaxKind::List
                                        ? tree_->Children(cases)
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
        const uint32 values = tree_->Child(list[index], 0);
        for (const uint32 valueNode : tree_->Children(values)) {
            ConstantValue value;
            const uint32 symbol = c_.NodeSymbol(module_, valueNode);
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
            const uint32 values = tree_->Child(list[index], 0);
            for (const uint32 valueNode : tree_->Children(values)) {
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
        LowerStatement(tree_->Child(list[index], 1));
        if (!Terminated()) {
            Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, list[index]);
        }
    }

    if (defaultNode != NoNode) {
        StartBlock(otherwise);
        LowerStatement(tree_->Child(defaultNode, 0));
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
            valueNode == NoNode ? NoType : c_.ExpressionType(module_, valueNode);
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
                // 정해진 오류(`return Error.NotFound;`)는 코드를 바로 싣고, 아니면 case 번호 + 1을
                // 실행 중 계산한다(I64 덧셈 — 코드는 64비트 정수다).
                const uint32 caseSymbol = c_.NodeSymbol(module_, valueNode);
                if (caseSymbol != NoSymbol &&
                    c_.symbols_[caseSymbol].kind == SymbolKind::EnumCase) {
                    const uint64 code = EnumIndex(caseSymbol) + 1;
                    Add(IrOp::LoadInt, value, static_cast<uint32>(code), NoIrRegister,
                        static_cast<uint32>(code >> 32), node);
                } else {
                    const uint32 code = NewValue(checker_.intType_);
                    Add(IrOp::Convert, code, payload, NoIrRegister, 0, node);
                    const uint32 one = NewValue(checker_.intType_);
                    Add(IrOp::LoadInt, one, 1, NoIrRegister, 0, node);
                    Add(IrOp::Binary, value, code, one, 0, node, static_cast<uint8>(IrArith::Add));
                }

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
    const uint32 path = tree_->Child(node, 0);
    const uint32 list = tree_->Child(node, 1);
    if (path == NoNode || tree_->Node(path).childCount == 0) {
        return;
    }

    const Span<const uint32> segments = tree_->Children(path);
    const uint32 last = segments[segments.Size() - 1];
    const uint32 event = c_.NodeSymbol(module_, last);
    if (event == NoSymbol) {
        return;
    }

    // 대상: 이름 하나면 자기 객체(this), `this.E`면 this, `x.E`면 x의 값.
    uint32 target = NoIrRegister;
    if (segments.Size() == 2) {
        const uint32 first = segments[0];
        if (tree_->Node(first).kind == SyntaxKind::This) {
            target = this_;
        } else {
            const uint32 symbol = c_.NodeSymbol(module_, first);
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
    const Span<const uint32> arguments =
        list != NoNode && tree_->Node(list).kind == SyntaxKind::List ? tree_->Children(list)
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
