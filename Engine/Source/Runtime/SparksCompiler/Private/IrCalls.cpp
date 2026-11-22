#include "CompilationChecker.h"
#include "IrBuilder.h"
#include "LanguageNames.h"

// 멤버·호출·생성·인덱스·문자열·컬렉션·`as?`·`await`·`try` 내림이다(Docs/Design/SparksBytecode.ko.md
// 3절, LC4a). 호출·생성·발행·문자열 잇기의 인자는 함수의 피연산자 목록(operands)에 둔다.

namespace rocks::sparks::detail {

namespace {

// `Math.Pi`의 double 값이다(SL 부록 B — 접는 상수).
constexpr double MathPi = 3.141592653589793;

} // namespace

namespace {

// 수학 타입 필드의 slot 구간이다(2절 표의 성분 순서).
struct MathField {
    StringView type;
    StringView field;
    uint16 offset;
    uint16 count;
};

constexpr MathField MathFields[] = {
    {"vec2", "X", 0, 1},
    {"vec2", "Y", 1, 1},
    {"vec3", "X", 0, 1},
    {"vec3", "Y", 1, 1},
    {"vec3", "Z", 2, 1},
    {"vec4", "X", 0, 1},
    {"vec4", "Y", 1, 1},
    {"vec4", "Z", 2, 1},
    {"vec4", "W", 3, 1},
    {"quat", "X", 0, 1},
    {"quat", "Y", 1, 1},
    {"quat", "Z", 2, 1},
    {"quat", "W", 3, 1},
    {"Rotator", "Pitch", 0, 1},
    {"Rotator", "Yaw", 1, 1},
    {"Rotator", "Roll", 2, 1},
    {"Color", "R", 0, 1},
    {"Color", "G", 1, 1},
    {"Color", "B", 2, 1},
    {"Color", "A", 3, 1},
    {"Transform", "Location", 0, 3},
    {"Transform", "Rotation", 3, 4},
    {"Transform", "Scale", 7, 3},
};

} // namespace

bool IrBuilder::MathFieldSlots(uint32 type, StringView field, uint32& offset,
                               uint32& count) const noexcept {
    const uint32 symbol = checker_.SymbolOf(type);
    if (symbol == NoSymbol) {
        return false;
    }

    const StringView name = c_.symbols_[symbol].name;
    for (const MathField& entry : MathFields) {
        if (entry.type == name && entry.field == field) {
            offset = entry.offset;
            count = entry.count;
            return true;
        }
    }

    return false;
}

uint32 IrBuilder::AddOperands(Span<const uint32> values) noexcept {
    const uint32 first = static_cast<uint32>(function_.operands.Size());
    for (const uint32 value : values) {
        if (auto pushed = function_.operands.TryPushBack(value); !pushed) {
            Fail(pushed.Error());
            return first;
        }
    }

    return first;
}

uint32 IrBuilder::LowerMember(uint32 node) noexcept {
    const uint32 symbol = c_.NodeSymbol(module_, node);
    const uint32 type = c_.ExpressionType(module_, node);
    if (symbol == NoSymbol) {
        return NoIrRegister;
    }

    const Symbol& entry = c_.symbols_[symbol];
    // 이름 공간의 언어 상수(`Math.Pi`)는 접는다(SparksVm 6a절).
    if (entry.kind == SymbolKind::LanguageField && entry.owner != NoSymbol &&
        c_.symbols_[entry.owner].kind == SymbolKind::PreludeValue &&
        c_.symbols_[entry.owner].name == "Math" && entry.name == "Pi") {
        ConstantValue value;
        value.kind = ConstantValue::Kind::Float;
        value.real = MathPi;
        return LoadConstant(value, type != NoType ? type : checker_.floatType_, node);
    }

    if (entry.kind == SymbolKind::EnumCase) {
        ConstantValue value;
        value.kind = ConstantValue::Kind::Enum;
        value.symbol = symbol;
        return LoadConstant(value, type != NoType ? type : checker_.TypeOfSymbol(entry.owner),
                            node);
    }

    if (entry.kind == SymbolKind::Const) {
        return LowerConst(symbol, type != NoType ? type : entry.type, node);
    }

    if (entry.kind != SymbolKind::Field && entry.kind != SymbolKind::EngineField &&
        entry.kind != SymbolKind::LanguageField) {
        return NoIrRegister; // 함수·이벤트(호출·await가 본다)
    }

    const uint32 objectNode = tree_->Child(node, 0);
    const bool chained = (tree_->Node(node).flags & SyntaxFlags::NullConditional) != 0;
    const uint32 object = Lower(objectNode, NoType);
    if (object == NoIrRegister) {
        return NoIrRegister;
    }

    const uint32 objectType = function_.registers[object].type;
    if (!chained) {
        return ReadMember(object, objectType, symbol, type, node);
    }

    // a?.f: a가 있으면 f의 값(T?), 없으면 없음. f가 이미 `T?`로 선언됐으면 그 값을 그대로
    // 쓴다(Scalar `T?`의 있음 표지를 잃지 않게).
    const uint32 declared =
        entry.kind == SymbolKind::Field ? checker_.FieldType(symbol) : entry.type;
    const uint32 readType =
        declared != NoType && checker_.IsOptional(declared) ? type : checker_.NonOptional(type);
    const uint32 result = NewValue(type);
    const uint32 some = NewValue(checker_.boolType_);
    Add(IrOp::IsSome, some, object, NoIrRegister, 0, node);
    const uint32 present = NewBlock();
    const uint32 absent = NewBlock();
    const uint32 done = NewBlock();
    Add(IrOp::Branch, present, some, absent, 0, node);
    StartBlock(present);
    const uint32 inner = checker_.NonOptional(objectType);
    const uint32 value = ReadMember(Unwrap(object, inner, node), inner, symbol, readType, node);
    MoveValue(result, Coerce(value, type, node), node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(absent);
    LoadNone(result, node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(done);
    return result;
}

uint32 IrBuilder::ReadMember(uint32 object, uint32 objectType, uint32 member, uint32 type,
                             uint32 node) noexcept {
    // 수학 타입의 필드는 slot이고, 그 밖은 GETF(수신자, 필드 심볼)다.
    uint32 offset = 0;
    uint32 count = 0;
    if (MathFieldSlots(objectType, c_.symbols_[member].name, offset, count)) {
        const uint32 result = NewValue(type);
        for (uint32 slot = 0; slot < count; ++slot) {
            Add(IrOp::Move, result + slot, object + offset + slot, NoIrRegister, 0, node);
        }

        return result;
    }

    // 언어 필드(`string.Length`·`Array.Count`·`Result.Value` 등)는 수신자 하나의 언어 함수 호출이다
    // (CALLL — SparksVm 6a절).
    const uint32 result = NewValue(type);
    if (c_.symbols_[member].kind == SymbolKind::LanguageField) {
        const uint32 operands[] = {object};
        Add(IrOp::Call, result, AddOperands(Span<const uint32>(operands, 1)), 1, member, node,
            IrCallDirect);
        return result;
    }

    Add(IrOp::GetField, result, object, NoIrRegister, member, node);
    return result;
}

void IrBuilder::LoadNone(uint32 value, uint32 node) noexcept {
    if (value == NoIrRegister) {
        return;
    }

    if (function_.registers[value].bank == IrBank::Scalar) {
        for (uint32 slot = 0; slot < SlotCount(value); ++slot) {
            Add(IrOp::LoadInt, value + slot, 0, NoIrRegister, 0, node);
        }
    } else {
        Add(IrOp::LoadNull, value, NoIrRegister, NoIrRegister, 0, node);
    }
}

uint32 IrBuilder::LowerCall(uint32 node, uint32 expected) noexcept {
    const uint32 calleeNode = tree_->Child(node, 0);
    const uint32 list = tree_->Child(node, 1);
    const Span<const uint32> arguments =
        list != NoNode && tree_->Node(list).kind == SyntaxKind::List ? tree_->Children(list)
                                                                     : Span<const uint32>();
    const uint32 type = TypeOf(node, expected);
    const uint32 callee = c_.NodeSymbol(module_, calleeNode);
    if (callee == NoSymbol) {
        return NoIrRegister;
    }

    const Symbol& entry = c_.symbols_[callee];
    // 타입 이름 호출: 숫자·bool·string 변환, 위치 생성(벡터), 이름 지정 생성.
    if (entry.kind == SymbolKind::BuiltinType || entry.kind == SymbolKind::PreludeType ||
        entry.kind == SymbolKind::Struct || entry.kind == SymbolKind::EngineType ||
        entry.kind == SymbolKind::Class) {
        return LowerConstruct(node, callee, arguments, type);
    }

    // 검증 리터럴(Tag·Action·Asset·Loc·CVar·Row): 이름 글자를 싣는 생성이다.
    if (entry.kind == SymbolKind::PreludeValue &&
        LanguageNames()[entry.index].kind == LanguageNameKind::Literal) {
        return LowerLiteralCall(node, callee, arguments, type);
    }

    // 수신자: 메서드의 수신자는 `.` 앞 값, 이름만 쓴 자기 메서드는 this, RPC 접두는 this. 값을
    // 바꾸는 언어 메서드(`Add`·`Remove`…, SL §3.11)의 수신자는 값 경로로 한 번 평가하고 호출 뒤 그
    // 경로에 다시 쓴다(수신자 묶음은 호출이 바꾸는 in/out 인자다).
    uint32 receiver = NoIrRegister;
    bool rpc = false;
    bool chained = false;
    const bool mutating = checker_.IsMutatingMember(callee);
    Array<PathStep> path(checker_.allocator_);
    if (tree_->Node(calleeNode).kind == SyntaxKind::Member) {
        uint32 objectNode = tree_->Child(calleeNode, 0);
        while (objectNode != NoNode && tree_->Node(objectNode).kind == SyntaxKind::Parenthesized) {
            objectNode = tree_->Child(objectNode, 0);
        }

        const uint32 objectSymbol = tree_->Node(objectNode).kind == SyntaxKind::Identifier
                                        ? c_.NodeSymbol(module_, objectNode)
                                        : NoSymbol;
        const SymbolKind objectKind =
            objectSymbol == NoSymbol ? SymbolKind::BuiltinType : c_.symbols_[objectSymbol].kind;
        rpc = objectKind == SymbolKind::RpcPrefix;
        const bool staticTarget =
            objectKind == SymbolKind::Alias || objectKind == SymbolKind::PreludeValue ||
            objectKind == SymbolKind::Class || objectKind == SymbolKind::Struct ||
            objectKind == SymbolKind::Enum || objectKind == SymbolKind::EngineType ||
            objectKind == SymbolKind::BuiltinType || objectKind == SymbolKind::PreludeType;
        chained = (tree_->Node(calleeNode).flags & SyntaxFlags::NullConditional) != 0;
        if (rpc) {
            receiver = this_;
        } else if (mutating && !chained) {
            receiver = EvaluatePath(objectNode, path, true);
        } else if (!staticTarget || objectSymbol == NoSymbol) {
            receiver = Lower(objectNode, NoType);
        }
    } else if (entry.kind == SymbolKind::Method || entry.kind == SymbolKind::EngineFunction) {
        receiver = (entry.flags & SymbolFlags::Static) != 0 ? NoIrRegister : this_;
    }

    if (!chained || receiver == NoIrRegister) {
        const uint32 result = EmitCall(node, callee, receiver, arguments, type,
                                       rpc        ? IrCallRpc
                                       : mutating ? IrCallMutating
                                                  : IrCallDirect);
        if (mutating && !path.IsEmpty()) {
            StorePath(path.View(), receiver);
        }

        return result;
    }

    // a?.F(...): a가 있으면 꺼낸 수신자로 부르고(인자도 그때만 계산한다) 결과는 T?, 없으면 없음.
    // 대기를 만드는 호출(`All(a?.F(), …)`)이면 결과는 대기 handle이고 없음은 null — 묶음이 바로
    // 끝난 인자로 본다(SparksRuntime 2.5절, LC6c2 리뷰 B5).
    const bool waits = entry.signature != NoSignature && c_.signatures_[entry.signature].isAsync;
    const uint32 result = waits ? NewWait() : NewValue(type);
    const uint32 some = NewValue(checker_.boolType_);
    Add(IrOp::IsSome, some, receiver, NoIrRegister, 0, node);
    const uint32 present = NewBlock();
    const uint32 absent = NewBlock();
    const uint32 done = NewBlock();
    Add(IrOp::Branch, present, some, absent, 0, node);
    StartBlock(present);
    const uint32 receiverType = checker_.NonOptional(function_.registers[receiver].type);
    // 결과가 이미 `T?`로 선언된 함수는 그 값 그대로(있음 표지를 잃지 않게), 아니면 `T`로 받아
    // 올린다.
    const uint32 signature = entry.signature;
    const uint32 declared =
        signature == NoSignature ? NoType : c_.signatures_[signature].returnType;
    const bool optionalResult = declared != NoType && checker_.IsOptional(declared);
    const uint32 inner = type == NoType || type == checker_.voidType_ || optionalResult
                             ? type
                             : checker_.NonOptional(type);
    const uint32 value = EmitCall(node, callee, Unwrap(receiver, receiverType, node), arguments,
                                  inner, mutating ? IrCallMutating : IrCallDirect);
    MoveValue(result, waits ? value : Coerce(value, type, node), node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(absent);
    LoadNone(result, node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(done);
    return result;
}

uint32 IrBuilder::EmitCall(uint32 node, uint32 callee, uint32 receiver,
                           Span<const uint32> arguments, uint32 type, uint8 sub) noexcept {
    const Symbol& entry = c_.symbols_[callee];
    Array<uint32> values(checker_.allocator_);
    if (receiver != NoIrRegister) {
        if (auto pushed = values.TryPushBack(receiver); !pushed) {
            Fail(pushed.Error());
            return NoIrRegister;
        }
    }

    // 매개변수 타입을 기대 타입으로 인자를 내린다(가변 인자 All·Race는 각자 타입).
    const uint32 signature = entry.signature;
    for (usize index = 0; index < arguments.Size(); ++index) {
        uint32 parameterType = NoType;
        if (signature != NoSignature && index < c_.signatures_[signature].parameterCount &&
            (entry.flags & SymbolFlags::Variadic) == 0) {
            parameterType =
                c_.signatureParameters_[c_.signatures_[signature].firstParameter + index];
        }

        const uint32 valueNode = tree_->Child(arguments[index], 1);
        // 이벤트 인자(`Race(Play(seq), Skip.Triggered)` — SL 6.1절 SD41)는 이벤트 대기 handle이다.
        if (const uint32 eventWait = LowerEventWait(valueNode); eventWait != NoIrRegister) {
            if (auto pushed = values.TryPushBack(eventWait); !pushed) {
                Fail(pushed.Error());
                return NoIrRegister;
            }

            continue;
        }

        const uint32 argumentType = c_.ExpressionType(module_, valueNode);
        // 자리표시·제네릭 매개변수는 실제 타입을 쓴다(검사기가 인자 식의 타입을 기록했다).
        const bool concrete =
            parameterType != NoType && !checker_.ContainsPlaceholder(parameterType);
        uint32 value = Lower(valueNode, concrete ? parameterType : argumentType);
        if (concrete) {
            value = Coerce(value, parameterType, valueNode);
        }

        if (auto pushed = values.TryPushBack(value); !pushed) {
            Fail(pushed.Error());
            return NoIrRegister;
        }
    }

    // 대기를 만드는 호출(`async fn`, `await void` 함수 — Delay·NextTick·All·Race)의 결과는 대기
    // handle이다(await가 정지하고 결과를 받는다).
    const bool wait = signature != NoSignature && c_.signatures_[signature].isAsync &&
                      !(node == directAwaitCall_ && IsDirectAwaitCallee(callee));
    const uint32 result = wait ? NewWait() : NewValue(type);
    const uint32 operands = AddOperands(values.View());
    // Sparks async 함수를 대기 값으로 쓰는 호출(`All(F(), G())` — 하위 실행은 LC6c2): 지금은 함수를
    // 부르고 대기 자리를 null로 둔다(묶음·AWAIT가 ContractViolation으로 확정적으로 멈춘다 — 반환
    // 창의 남은 값을 대기로 읽지 않게, LC6c1 리뷰 S4).
    const bool virtualCall = entry.kind == SymbolKind::InterfaceMethod ||
                             (entry.flags & (SymbolFlags::Open | SymbolFlags::Override)) != 0;
    if (wait && IsDirectAwaitCallee(callee) && !virtualCall) {
        // 하위 실행(CALLW — SparksRuntime 2.5절): 결과는 하위 대기 handle이다.
        Add(IrOp::Call, result, operands, static_cast<uint32>(values.Size()), callee, node,
            IrCallChild);
    } else if (wait && IsDirectAwaitCallee(callee)) {
        // 가상 async 호출의 하위 실행(CALLV 경계)은 아직 없다: 부르지 않고 그 자리에서
        // ContractViolation으로 멈춘다(동기로 부르면 그 안의 정지가 사슬 전체를 멈춰 묶음 의미가
        // 틀어진다 — LC6c2 리뷰). 뒤 코드는 도달하지 않는 block이다.
        Add(IrOp::Fault, NoIrRegister, NoIrRegister, NoIrRegister, IrFaultUnsupported, node);
        StartBlock(NewBlock());
        LoadNone(result, node);
    } else {
        Add(IrOp::Call, result, operands, static_cast<uint32>(values.Size()), callee, node, sub);
    }

    // 명시한 타입 인자(`Find<Lamp>()`·`LoadAll<T>(...)`)는 호출의 타입 인자 목록으로 둔다(실행 중
    // native 호출이 받는다).
    const uint32 calleeNode = tree_->Child(node, 0);
    const uint32 typeList =
        calleeNode == NoNode                                     ? NoNode
        : tree_->Node(calleeNode).kind == SyntaxKind::Member     ? tree_->Child(calleeNode, 1)
        : tree_->Node(calleeNode).kind == SyntaxKind::Identifier ? tree_->Child(calleeNode, 0)
                                                                 : NoNode;
    if (typeList != NoNode && tree_->Node(typeList).kind == SyntaxKind::List &&
        tree_->Node(typeList).childCount > 0 && !function_.instructions.IsEmpty()) {
        const uint32 first = static_cast<uint32>(function_.operands.Size());
        const uint32 count = tree_->Node(typeList).childCount;
        if (auto pushed = function_.operands.TryPushBack(count); !pushed) {
            Fail(pushed.Error());
            return result;
        }

        for (const uint32 typeNode : tree_->Children(typeList)) {
            if (auto pushed = function_.operands.TryPushBack(c_.NodeType(module_, typeNode));
                !pushed) {
                Fail(pushed.Error());
                return result;
            }
        }

        function_.instructions[function_.instructions.Size() - 1].types = first;
    }

    return result;
}

uint32 IrBuilder::LowerConstruct(uint32 node, uint32 typeSymbol, Span<const uint32> arguments,
                                 uint32 type) noexcept {
    const Symbol& entry = c_.symbols_[typeSymbol];
    const IrLayout layout = Layout(checker_, type);
    // 검증 리터럴이면서 타입인 것(`Tag("a.b")`·`Row<T>("x")`).
    if (entry.kind == SymbolKind::PreludeType &&
        LanguageNames()[entry.index].kind == LanguageNameKind::TypeLiteral) {
        return LowerLiteralCall(node, typeSymbol, arguments, type);
    }

    // 숫자·bool·string 변환(`int8(x)`·`float(n)`·`string(x)`).
    if (entry.kind == SymbolKind::BuiltinType && arguments.Size() == 1 &&
        tree_->Child(arguments[0], 0) == NoNode && layout.bank == IrBank::Scalar &&
        layout.slots == 1) {
        const uint32 valueNode = tree_->Child(arguments[0], 1);
        const uint32 operand = Lower(valueNode, c_.ExpressionType(module_, valueNode));
        const uint32 result = NewValue(type);
        Add(IrOp::Convert, result, operand, NoIrRegister, 0, node);
        return result;
    }

    if (entry.kind == SymbolKind::BuiltinType && entry.name == "string" && arguments.Size() == 1) {
        const uint32 valueNode = tree_->Child(arguments[0], 1);
        const uint32 operand = Lower(valueNode, c_.ExpressionType(module_, valueNode));
        const uint32 result = NewValue(type);
        Add(IrOp::Convert, result, operand, NoIrRegister, 0, node);
        return result;
    }

    // 성분 double로 펼친 수학 타입: 위치 인자(벡터·quat)나 이름 지정(Rotator·Color·Transform).
    if (layout.bank == IrBank::Scalar && layout.slots > 1) {
        const uint32 result = NewValue(type);
        // 기본값: quat·Transform은 단위, 나머지는 0(SL §3.2 기본값).
        for (uint32 slot = 0; slot < layout.slots; ++slot) {
            // quat W, Transform의 Rotation W·Scale, mat4의 대각.
            const bool identityW = (entry.name == "quat" && slot == 3) ||
                                   (entry.name == "Transform" && (slot == 6 || slot >= 7)) ||
                                   (entry.name == "mat4" && slot % 5 == 0);
            ConstantValue zero;
            zero.kind = ConstantValue::Kind::Float;
            zero.real = identityW ? 1.0 : 0.0;
            const uint64 bits = FloatBits(zero.real);
            Add(IrOp::LoadFloat, result + slot, static_cast<uint32>(bits), NoIrRegister,
                static_cast<uint32>(bits >> 32), node);
        }

        for (usize index = 0; index < arguments.Size(); ++index) {
            const uint32 nameNode = tree_->Child(arguments[index], 0);
            const uint32 valueNode = tree_->Child(arguments[index], 1);
            uint32 offset = static_cast<uint32>(index);
            uint32 count = 1;
            uint32 fieldType = checker_.floatType_;
            if (nameNode != NoNode) {
                const uint32 field = c_.NodeSymbol(module_, nameNode);
                if (field == NoSymbol ||
                    !MathFieldSlots(type, c_.symbols_[field].name, offset, count)) {
                    continue;
                }

                fieldType = c_.symbols_[field].type;
            }

            const uint32 value = Coerce(Lower(valueNode, fieldType), fieldType, valueNode);
            for (uint32 slot = 0; slot < count && value != NoIrRegister; ++slot) {
                Add(IrOp::Move, result + offset + slot, value + slot, NoIrRegister, 0, valueNode);
            }
        }

        return result;
    }

    // struct·엔진 struct·component·TagSet: NEW(타입, (필드, 값) 쌍). Sparks struct의 빠진 필드는
    // 생성 자리에서 초기값·0 값을 내린다(NEWS는 0·null 버퍼 — SparksBytecode 5절). 엔진 struct·
    // component의 빠진 필드는 실행 중 기본값(host — LC5d·LC6)이다.
    Array<uint32> values(checker_.allocator_);
    for (const uint32 argument : arguments) {
        const uint32 nameNode = tree_->Child(argument, 0);
        const uint32 valueNode = tree_->Child(argument, 1);
        const uint32 field = nameNode == NoNode ? NoSymbol : c_.NodeSymbol(module_, nameNode);
        if (field == NoSymbol) {
            continue;
        }

        const uint32 fieldType = c_.symbols_[field].kind == SymbolKind::Field
                                     ? checker_.FieldType(field)
                                     : c_.symbols_[field].type;
        const uint32 value = Coerce(Lower(valueNode, fieldType), fieldType, valueNode);
        if (!values.TryPushBack(field) || !values.TryPushBack(value)) {
            Fail(ErrorCode::OutOfMemory);
            return NoIrRegister;
        }
    }

    bool active = false;
    for (const uint32 symbol : defaulting_) {
        active = active || symbol == typeSymbol;
    }

    if (entry.kind == SymbolKind::Struct && !active) {
        if (!defaulting_.TryPushBack(typeSymbol)) {
            Fail(ErrorCode::OutOfMemory);
            return NoIrRegister;
        }

        const SyntaxTree& tree = c_.trees_[entry.module];
        const uint32 members = tree.Child(entry.node, 3);
        const usize given = values.Size();
        for (const uint32 member :
             members == NoNode ? Span<const uint32>() : tree.Children(members)) {
            if (tree.Node(member).kind != SyntaxKind::Field) {
                continue;
            }

            const uint32 field = c_.NodeSymbol(entry.module, tree.Child(member, 2));
            bool provided = field == NoSymbol;
            for (usize at = 0; at < given && !provided; at += 2) {
                provided = values[at] == field;
            }

            if (provided) {
                continue;
            }

            const uint32 value = LowerFieldDefault(field, node);
            if (value != NoIrRegister &&
                (!values.TryPushBack(field) || !values.TryPushBack(value))) {
                Fail(ErrorCode::OutOfMemory);
                return NoIrRegister;
            }
        }

        static_cast<void>(defaulting_.TryResize(defaulting_.Size() - 1));
    }

    const uint32 result = NewValue(type);
    Add(IrOp::New, result, AddOperands(values.View()), static_cast<uint32>(values.Size() / 2),
        typeSymbol, node);
    return result;
}

uint32 IrBuilder::LowerFieldDefault(uint32 field, uint32 node) noexcept {
    const Symbol entry = c_.symbols_[field];
    const uint32 fieldType = checker_.FieldType(field);
    const SyntaxTree& tree = c_.trees_[entry.module];
    const uint32 initializer = entry.node == NoNode ? NoNode : tree.Child(entry.node, 4);
    if (initializer == NoNode) {
        return LowerZero(fieldType, node);
    }

    // 초기값은 상수식이다(SL §3.11 — 지역·this 없음). 그 모듈의 트리로 내리고 위치는 생성 자리다.
    const SyntaxTree* savedTree = tree_;
    const uint32 savedModule = module_;
    const SourceSpan savedSpan = spanOverride_;
    const bool savedOverride = overrideSpan_;
    if (!overrideSpan_) {
        spanOverride_ = tree_->Node(node).span;
        overrideSpan_ = true;
    }

    tree_ = &tree;
    module_ = entry.module;
    const uint32 value = Coerce(Lower(initializer, fieldType), fieldType, initializer);
    tree_ = savedTree;
    module_ = savedModule;
    spanOverride_ = savedSpan;
    overrideSpan_ = savedOverride;
    return value;
}

uint32 IrBuilder::LowerZero(uint32 type, uint32 node) noexcept {
    if (type == NoType || checker_.IsOptional(type)) {
        return NoIrRegister;
    }

    const uint32 symbol = checker_.SymbolOf(type);
    if (symbol == NoSymbol) {
        return NoIrRegister;
    }

    const Symbol& entry = c_.symbols_[symbol];
    if (type == checker_.stringType_) {
        const uint32 result = NewValue(type);
        Add(IrOp::LoadString, result, AddString("\"\""), NoIrRegister, 0, node);
        return result;
    }

    if (checker_.IsLanguage(type, "Array") || checker_.IsLanguage(type, "Map") ||
        checker_.IsLanguage(type, "Set")) {
        const uint32 result = NewValue(type);
        Add(IrOp::New, result, AddOperands(Span<const uint32>()), 0, symbol, node, IrNewCollection);
        return result;
    }

    const bool identity =
        (entry.kind == SymbolKind::BuiltinType || entry.kind == SymbolKind::PreludeType) &&
        (entry.name == "quat" || entry.name == "mat4" || entry.name == "Transform");
    if (entry.kind == SymbolKind::Struct || identity) {
        return LowerConstruct(node, symbol, Span<const uint32>(), type);
    }

    return NoIrRegister;
}

uint32 IrBuilder::LowerLiteralCall(uint32 node, uint32 literal, Span<const uint32> arguments,
                                   uint32 type) noexcept {
    // 검증 리터럴: 첫 인자(이름 문자열)와 나머지 인자(Loc의 이름 지정 인자)를 NEW로 넘긴다.
    Array<uint32> values(checker_.allocator_);
    for (const uint32 argument : arguments) {
        const uint32 valueNode = tree_->Child(argument, 1);
        const uint32 nameNode = tree_->Child(argument, 0);
        const uint32 value = Lower(valueNode, c_.ExpressionType(module_, valueNode));
        const uint32 name =
            nameNode == NoNode ? NoSymbol : AddString(checker_.binder_.NodeText(module_, nameNode));
        if (!values.TryPushBack(name) || !values.TryPushBack(value)) {
            Fail(ErrorCode::OutOfMemory);
            return NoIrRegister;
        }
    }

    const uint32 result = NewValue(type);
    Add(IrOp::New, result, AddOperands(values.View()), static_cast<uint32>(values.Size() / 2),
        literal, node, IrNewLiteral);
    return result;
}

uint32 IrBuilder::LowerIndex(uint32 node) noexcept {
    const uint32 object = Lower(tree_->Child(node, 0), NoType);
    const uint32 keyNode = tree_->Child(node, 1);
    const uint32 objectType = object == NoIrRegister ? NoType : function_.registers[object].type;
    // 키 타입: Array는 int, Map은 K.
    uint32 keyType = checker_.intType_;
    if (objectType != NoType && checker_.IsLanguage(objectType, "Map")) {
        keyType = checker_.Argument(objectType, 0);
    }

    const uint32 key = Coerce(Lower(keyNode, keyType), keyType, keyNode);
    const uint32 result = NewValue(c_.ExpressionType(module_, node));
    Add(IrOp::GetElement, result, object, key, 0, node);
    return result;
}

uint32 IrBuilder::LowerString(uint32 node) noexcept {
    const uint32 type = checker_.stringType_;
    if (tree_->Node(node).kind == SyntaxKind::VerbatimString) {
        const uint32 result = NewValue(type);
        Add(IrOp::LoadString, result, AddString(checker_.TokenText(module_, node)), NoIrRegister, 0,
            node);
        return result;
    }

    // 보간 없는 문자열(조각 0·1개)은 상수 하나다.
    const Span<const uint32> parts = tree_->Children(node);
    if (parts.Size() == 0 ||
        (parts.Size() == 1 && tree_->Node(parts[0]).kind == SyntaxKind::StringText)) {
        const uint32 result = NewValue(type);
        Add(IrOp::LoadString, result,
            AddString(parts.Size() == 0 ? StringView() : checker_.TokenText(module_, parts[0])),
            NoIrRegister, 0, node, IrStringPiece);
        return result;
    }

    // 조각: 글자 조각은 원문 글자(escape를 풀지 않은 것 — bytecode가 푼다), 보간은 값의 문자열.
    Array<uint32> pieces(checker_.allocator_);
    for (const uint32 part : tree_->Children(node)) {
        uint32 piece = NoIrRegister;
        if (tree_->Node(part).kind == SyntaxKind::StringText) {
            piece = NewValue(type);
            Add(IrOp::LoadString, piece, AddString(checker_.TokenText(module_, part)), NoIrRegister,
                0, part, IrStringPiece);
        } else {
            const uint32 valueNode = tree_->Child(part, 0);
            const uint32 value = Lower(valueNode, c_.ExpressionType(module_, valueNode));
            piece = NewValue(type);
            Add(IrOp::Convert, piece, value, NoIrRegister, 0, part);
        }

        if (auto pushed = pieces.TryPushBack(piece); !pushed) {
            Fail(pushed.Error());
            return NoIrRegister;
        }
    }

    const uint32 result = NewValue(type);
    Add(IrOp::Concat, result, AddOperands(pieces.View()), static_cast<uint32>(pieces.Size()), 0,
        node);
    return result;
}

uint32 IrBuilder::LowerCollection(uint32 node, uint32 expected) noexcept {
    // 배열·Map 리터럴: NEW(타입, 원소 목록 — Map은 (키, 값) 쌍).
    const uint32 type = TypeOf(node, expected);
    const bool map = type != NoType && checker_.IsLanguage(checker_.NonOptional(type), "Map");
    const uint32 value = checker_.NonOptional(type);
    const uint32 elementType = map ? checker_.Argument(value, 1) : checker_.Argument(value, 0);
    const uint32 keyType = map ? checker_.Argument(value, 0) : NoType;
    Array<uint32> values(checker_.allocator_);
    for (const uint32 element : tree_->Children(node)) {
        if (tree_->Node(element).kind == SyntaxKind::MapEntry) {
            const uint32 keyNode = tree_->Child(element, 0);
            const uint32 itemNode = tree_->Child(element, 1);
            const uint32 key = Coerce(Lower(keyNode, keyType), keyType, keyNode);
            const uint32 item = Coerce(Lower(itemNode, elementType), elementType, itemNode);
            if (!values.TryPushBack(key) || !values.TryPushBack(item)) {
                Fail(ErrorCode::OutOfMemory);
                return NoIrRegister;
            }

            continue;
        }

        const uint32 item = Coerce(Lower(element, elementType), elementType, element);
        if (auto pushed = values.TryPushBack(item); !pushed) {
            Fail(pushed.Error());
            return NoIrRegister;
        }
    }

    const uint32 result = NewValue(type);
    Add(IrOp::New, result, AddOperands(values.View()), static_cast<uint32>(values.Size()),
        checker_.SymbolOf(value), node, IrNewCollection);
    return result;
}

uint32 IrBuilder::LowerCast(uint32 node) noexcept {
    // x as? T: 실행 중 타입 시험 — 맞으면 T?의 값, 아니면 없음.
    const uint32 operand = Lower(tree_->Child(node, 0), NoType);
    const uint32 type = c_.ExpressionType(module_, node);
    const uint32 target = c_.NodeType(module_, tree_->Child(node, 1));
    const uint32 result = NewValue(type);
    Add(IrOp::Convert, result, operand, NoIrRegister, checker_.SymbolOf(target), node,
        IrConvertCast);
    return result;
}

uint32 IrBuilder::LowerAwait(uint32 node, uint32 operandNode) noexcept {
    // await w: 대기 값(호출·요청·Delay)이나 이벤트로 AWAIT 정지 — 재개하면 결과를 받고 재개
    // 블록으로.
    while (operandNode != NoNode && tree_->Node(operandNode).kind == SyntaxKind::Parenthesized) {
        operandNode = tree_->Child(operandNode, 0);
    }

    // `await a?.F()`(async F): a가 있으면 부르고 기다려 결과를 `R?`로, 없으면 기다리지 않고 없음.
    if (operandNode != NoNode && tree_->Node(operandNode).kind == SyntaxKind::Call) {
        const uint32 calleeNode = tree_->Child(operandNode, 0);
        if (calleeNode != NoNode && tree_->Node(calleeNode).kind == SyntaxKind::Member &&
            (tree_->Node(calleeNode).flags & SyntaxFlags::NullConditional) != 0) {
            return LowerChainedAwait(node, operandNode);
        }
    }

    const uint32 resultType = AwaitResultType(operandNode);
    // `await F()`(Sparks async 함수)는 정지 지점이 아니다: 호출이 값을 직접 낸다.
    if (operandNode != NoNode && tree_->Node(operandNode).kind == SyntaxKind::Call &&
        IsDirectAwaitCallee(c_.NodeSymbol(module_, tree_->Child(operandNode, 0)))) {
        const uint32 outer = directAwaitCall_;
        directAwaitCall_ = operandNode;
        const uint32 value = Lower(operandNode, resultType);
        directAwaitCall_ = outer;
        return value;
    }

    // 이벤트(`await X.E`)는 이벤트 대기 handle을 만들어 기다린다(WAITEV + AWAIT — 2.5절).
    uint32 source = LowerEventWait(operandNode);
    if (source == NoIrRegister) {
        source = Lower(operandNode, c_.ExpressionType(module_, operandNode));
    }

    const uint32 result = NewValue(resultType);
    const uint32 resume = NewBlock();
    Add(IrOp::Await, result, source, resume, 0, node, IrAwaitValue);
    StartBlock(resume);
    return result;
}

uint32 IrBuilder::LowerEventWait(uint32 node) noexcept {
    // 이벤트 식(`x.E`·이름만 쓴 `E`)이면 발행자의 이벤트 대기 handle, 아니면 NoIrRegister.
    while (node != NoNode && tree_->Node(node).kind == SyntaxKind::Parenthesized) {
        node = tree_->Child(node, 0);
    }

    if (node == NoNode) {
        return NoIrRegister;
    }

    const SyntaxKind kind = tree_->Node(node).kind;
    const uint32 symbol = c_.NodeSymbol(module_, node);
    if (symbol == NoSymbol || (kind != SyntaxKind::Member && kind != SyntaxKind::Identifier) ||
        (c_.symbols_[symbol].kind != SymbolKind::Event &&
         c_.symbols_[symbol].kind != SymbolKind::EngineEvent &&
         c_.symbols_[symbol].kind != SymbolKind::LanguageEvent)) {
        return NoIrRegister;
    }

    // 발행자: `x.E`면 x, 이름만 쓴 `E`면 자기 객체다.
    const uint32 publisher =
        kind == SyntaxKind::Member ? Lower(tree_->Child(node, 0), NoType) : this_;
    const uint32 wait = NewWait();
    Add(IrOp::WaitEvent, wait, publisher, NoIrRegister, symbol, node);
    return wait;
}

uint32 IrBuilder::LowerChainedAwait(uint32 node, uint32 call) noexcept {
    const uint32 calleeNode = tree_->Child(call, 0);
    const uint32 callee = c_.NodeSymbol(module_, calleeNode);
    if (callee == NoSymbol) {
        return NoIrRegister;
    }

    const uint32 inner = AwaitResultType(call); // 대기가 돌려주는 R
    const bool value = inner != NoType && inner != checker_.voidType_;
    const uint32 resultType = value ? checker_.MakeOptional(inner) : checker_.voidType_;
    const uint32 list = tree_->Child(call, 1);
    const Span<const uint32> arguments =
        list != NoNode && tree_->Node(list).kind == SyntaxKind::List ? tree_->Children(list)
                                                                     : Span<const uint32>();
    const uint32 receiver = Lower(tree_->Child(calleeNode, 0), NoType);
    if (receiver == NoIrRegister) {
        return NoIrRegister;
    }

    const uint32 result = NewValue(resultType);
    const uint32 some = NewValue(checker_.boolType_);
    Add(IrOp::IsSome, some, receiver, NoIrRegister, 0, node);
    const uint32 present = NewBlock();
    const uint32 absent = NewBlock();
    const uint32 done = NewBlock();
    Add(IrOp::Branch, present, some, absent, 0, node);
    StartBlock(present);
    const uint32 receiverType = checker_.NonOptional(function_.registers[receiver].type);
    uint32 awaited = NoIrRegister;
    if (IsDirectAwaitCallee(callee)) {
        // Sparks async 함수: 호출이 값을 직접 낸다(정지는 호출된 함수 안에서 — 2.4절).
        const uint32 outer = directAwaitCall_;
        directAwaitCall_ = call;
        awaited = EmitCall(call, callee, Unwrap(receiver, receiverType, node), arguments, inner,
                           IrCallDirect);
        directAwaitCall_ = outer;
    } else {
        const uint32 wait = EmitCall(call, callee, Unwrap(receiver, receiverType, node), arguments,
                                     NoType, IrCallDirect);
        awaited = NewValue(inner);
        const uint32 resume = NewBlock();
        Add(IrOp::Await, awaited, wait, resume, 0, node, IrAwaitValue);
        StartBlock(resume);
    }

    MoveValue(result, Coerce(awaited, resultType, node), node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(absent);
    LoadNone(result, node);
    Add(IrOp::Jump, done, NoIrRegister, NoIrRegister, 0, node);
    StartBlock(done);
    return result;
}

bool IrBuilder::InStartupHook() const noexcept {
    // 지금 내리는 본문이 `on Awake`·`on Enable`·`on Start` hook인지다.
    const SyntaxTree& tree = c_.trees_[function_.module];
    if (tree.Node(function_.node).kind != SyntaxKind::Handler) {
        return false;
    }

    const uint32 path = tree.Child(function_.node, 2);
    if (path == NoNode || tree.Node(path).childCount != 1) {
        return false;
    }

    const StringView name = checker_.TokenText(function_.module, tree.Child(path, 0));
    return name == "Awake" || name == "Enable" || name == "Start";
}

bool IrBuilder::IsDirectAwaitCallee(uint32 callee) const noexcept {
    if (callee == NoSymbol) {
        return false;
    }

    const Symbol& entry = c_.symbols_[callee];
    return (entry.kind == SymbolKind::Function || entry.kind == SymbolKind::Method ||
            entry.kind == SymbolKind::InterfaceMethod) &&
           entry.signature != NoSignature && c_.signatures_[entry.signature].isAsync;
}

uint32 IrBuilder::AwaitResultType(uint32 operandNode) noexcept {
    // 3.10절 "await·try": async 함수 호출은 그 반환 타입, 로드 요청 `LoadRequest<T>`는 `Result<T>`,
    // 그 밖의 대기(Delay·NextTick·All·Race·이벤트)는 void다. `try await`의 Try 노드 타입은 꺼낸 T라
    // 대기 결과 타입을 따로 구한다.
    if (operandNode == NoNode) {
        return checker_.voidType_;
    }

    if (tree_->Node(operandNode).kind == SyntaxKind::Call) {
        const uint32 callee = c_.NodeSymbol(module_, tree_->Child(operandNode, 0));
        const uint32 signature = callee == NoSymbol ? NoSignature : c_.symbols_[callee].signature;
        if (signature != NoSignature && c_.signatures_[signature].isAsync) {
            const uint32 returns = c_.signatures_[signature].returnType;
            return returns == NoType ? checker_.voidType_ : returns;
        }
    }

    const uint32 type = c_.ExpressionType(module_, operandNode);
    if (type != NoType && !checker_.IsOptional(type) && checker_.IsLanguage(type, "LoadRequest")) {
        const uint32 element = checker_.Argument(type, 0);
        const uint32 result =
            checker_.binder_.FindName(SpaceKey(NameSpaceKind::Language, 0), "Result");
        return checker_.binder_.InternType(result, Span<const uint32>(&element, 1), false);
    }

    return checker_.voidType_;
}

uint32 IrBuilder::LowerTry(uint32 node) noexcept {
    // try x / try await x: Result가 실패면 그 오류로 함수에서 반환, 성공이면 값.
    const uint32 operandNode = tree_->Child(node, 0);
    const bool awaited = (tree_->Node(node).flags & SyntaxFlags::Await) != 0;
    const uint32 value = awaited ? LowerAwait(node, operandNode)
                                 : Lower(operandNode, c_.ExpressionType(module_, operandNode));
    // 성공이면 값을 꺼내 다음 블록으로, 실패면 지역 유지 자원을 모두 해제하고(D5 — 함수를 떠나는
    // 경로) 같은 오류를 이 함수의 Result로 반환한다.
    const uint32 result = NewValue(c_.ExpressionType(module_, node));
    const uint32 success = NewBlock();
    const uint32 failure = NewBlock();
    Add(IrOp::Try, result, value, failure, success, node);
    StartBlock(failure);
    ReleaseHeld(0, node);
    // Result 함수는 같은 오류를 반환하고, 그 밖의 함수·handler는 현재 실행을 끝낸다(위치별 1회 기록
    // — SL §8.4). Awake·Enable·Start 안이면 그 hook의 fault다(SD43).
    const bool resultFunction = returnType_ != NoType && returnType_ != checker_.voidType_ &&
                                checker_.IsLanguage(returnType_, "Result");
    if (resultFunction) {
        Add(IrOp::ReturnError, NoIrRegister, value, NoIrRegister, 0, node);
    } else if (InStartupHook()) {
        Add(IrOp::Fault, NoIrRegister, value, NoIrRegister, IrFaultTryInHook, node);
    } else {
        Add(IrOp::EndExecution, NoIrRegister, value, NoIrRegister, 0, node);
    }

    StartBlock(success);
    return result;
}

} // namespace rocks::sparks::detail
