#include "CompilationChecker.h"
#include "TypeChecker.h"

// 대입·변경 경로의 쓰기 가능 여부다(Docs/Design/SparksLanguage.ko.md 3.11절 "대입 경로",
// S0701–S0706· S0731, SD73). 경로는 뿌리(이름, `this`를 생략한 자기 필드) 뒤에 멤버·원소가 이어진
// 것이며 뿌리에서 끝으로 보아 처음 어긋난 곳의 진단 하나를 낸다. 참조 타입 값을 지나면 그 뒤는 객체
// 속성 쓰기다.

namespace rocks::sparks::detail {

namespace {

// 값 타입 경로를 바꾸는 부록 B 변경 메서드다(수신자 타입 이름, 메서드 이름).
struct MutatingMember {
    StringView owner;
    StringView name;
};

constexpr MutatingMember MutatingMembers[] = {
    {"Array", "Add"},     {"Array", "Insert"}, {"Array", "RemoveAt"}, {"Array", "Clear"},
    {"Array", "Reverse"}, {"Array", "Remove"}, {"Array", "Sort"},     {"Array", "Compact"},
    {"Map", "Remove"},    {"Map", "Clear"},    {"Set", "Add"},        {"Set", "Remove"},
    {"Set", "Clear"},     {"TagSet", "Add"},   {"TagSet", "Remove"},
};

// 쓰기 가능한 경로의 표지다(진단 코드가 아니다).
constexpr DiagnosticCode WritablePath = static_cast<DiagnosticCode>(0);

} // namespace

bool TypeChecker::IsMutatingMember(uint32 symbol) const noexcept {
    const Symbol& entry = compilation_.symbols_[symbol];
    if (entry.kind != SymbolKind::LanguageFunction || entry.owner == NoSymbol) {
        return false;
    }

    const StringView owner = compilation_.symbols_[entry.owner].name;
    for (const MutatingMember& member : MutatingMembers) {
        if (member.owner == owner && member.name == entry.name) {
            return true;
        }
    }

    return false;
}

bool TypeChecker::IsEngineFieldWritable(uint32 symbol) const noexcept {
    const Symbol& entry = compilation_.symbols_[symbol];
    return compilation_.schemas_[entry.index].schema.Fields()[entry.item].readWrite;
}

void TypeChecker::CheckWritablePath(const CheckContext& context, uint32 path,
                                    uint32 reportNode) noexcept {
    if (!flow_) {
        return; // LC2d 단계에서만 본다
    }

    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[context.module];
    // 경로 구간(끝 → 뿌리)을 모은다. 괄호는 건너뛴다.
    Array<uint32> steps(allocator_);
    uint32 root = path;
    for (;;) {
        if (root == NoNode) {
            return;
        }

        const SyntaxKind kind = tree.Node(root).kind;
        if (kind == SyntaxKind::Parenthesized) {
            root = tree.Child(root, 0);
            continue;
        }

        if (kind != SyntaxKind::Member && kind != SyntaxKind::Index && kind != SyntaxKind::Call) {
            break;
        }

        if (auto pushed = steps.TryPushBack(root); !pushed) {
            binder_.error_ = pushed.Error();
            return;
        }

        root = tree.Child(root, 0);
    }

    auto report = [&](DiagnosticCode code, StringView message, StringView explanation,
                      StringView fix) noexcept {
        Report(context, code, reportNode, message, explanation, fix);
    };
    // 호출 결과·`?.`가 든 경로(S0731). 호출 결과나 그 뒤의 중간 값(끝 구간의 값 제외)이 참조
    // 타입이면 그 객체의 속성 쓰기다(아래 규칙 — `GetOwner().Location = v`).
    for (usize index = 0; index < steps.Size(); ++index) {
        const uint32 step = steps[index];
        const SyntaxNode& entry = tree.Node(step);
        bool throughReference = false;
        for (usize later = 1; later <= index && entry.kind == SyntaxKind::Call; ++later) {
            const uint32 type = c.ExpressionType(context.module, steps[later]);
            throughReference =
                throughReference || (type != NoType && IsReference(NonOptional(type)));
        }

        if (entry.kind == SyntaxKind::Call && !throughReference) {
            report(DiagnosticCode::AssignToCallResult,
                   "A function result cannot be changed in place.",
                   "The result is a temporary value; changing it has no effect.",
                   "Store it in a local variable, change it, and write it back.");
            return;
        }

        if (entry.kind == SyntaxKind::Member && (entry.flags & SyntaxFlags::NullConditional) != 0) {
            report(DiagnosticCode::AssignToCallResult, "A path with '?.' cannot be assigned to.",
                   "When the receiver is null there is nothing to write to.",
                   "Check it first with 'if (let x = value) { x.Field = ...; }'.");
            return;
        }
    }

    const SyntaxKind rootKind = tree.Node(root).kind;
    if (rootKind != SyntaxKind::Identifier) {
        return; // `this.X`(S0423)·리터럴 등은 앞 단계가 알렸다
    }

    const uint32 rootSymbol = c.NodeSymbol(context.module, root);
    if (rootSymbol == NoSymbol) {
        return;
    }

    // 뿌리의 쓰기 가능 여부(NotWritable이면 그 코드로 알린다).
    const Symbol& entry = c.symbols_[rootSymbol];
    DiagnosticCode reason = WritablePath;
    switch (entry.kind) {
    case SymbolKind::Local: {
        const SyntaxKind declaration =
            entry.node == NoNode ? SyntaxKind::Error : c.trees_[entry.module].Node(entry.node).kind;
        if (declaration == SyntaxKind::IfLet || declaration == SyntaxKind::For) {
            reason = DiagnosticCode::ParameterWrite;
        } else if ((entry.flags & SymbolFlags::Let) != 0) {
            reason = DiagnosticCode::LetWrite;
        }

        break;
    }
    case SymbolKind::Parameter:
        reason = DiagnosticCode::ParameterWrite;
        break;
    case SymbolKind::Field:
        if (context.owner != NoSymbol && c.symbols_[context.owner].kind == SymbolKind::Struct) {
            reason = DiagnosticCode::StructSelfWrite;
        } else if ((entry.flags & SymbolFlags::Let) != 0) {
            reason = DiagnosticCode::LetWrite;
        }

        break;
    case SymbolKind::EngineField:
        if (!IsEngineFieldWritable(rootSymbol)) {
            reason = DiagnosticCode::ReadOnlyProperty;
        }

        break;
    case SymbolKind::Const:
        reason = DiagnosticCode::LetWrite;
        break;
    default:
        reason = DiagnosticCode::NotAssignable;
        break;
    }

    // 구간: 참조 값을 지나면 객체 속성 쓰기(앞의 사유를 버린다), 값 타입은 앞 사유를 잇는다.
    uint32 current = c.ExpressionType(context.module, root);
    StringView culprit = entry.name;
    bool culpritIsConst = entry.kind == SymbolKind::Const;
    for (usize index = steps.Size(); index > 0; --index) {
        const uint32 step = steps[index - 1];
        if (current == NoType) {
            // 값이 아닌 뿌리(타입 이름·이름 공간·별칭)의 멤버: enum case·함수는 대입할 수 없고
            // const는 바꿀 수 없다. 그 밖(해석하지 못한 경로)은 앞에서 알렸다.
            const uint32 member = c.NodeSymbol(context.module, step);
            if (index != steps.Size() || reason != DiagnosticCode::NotAssignable ||
                member == NoSymbol) {
                return;
            }

            const Symbol& memberEntry = c.symbols_[member];
            reason = memberEntry.kind == SymbolKind::Const ? DiagnosticCode::LetWrite
                     : memberEntry.kind == SymbolKind::LanguageField
                         ? DiagnosticCode::ReadOnlyProperty
                         : DiagnosticCode::NotAssignable;
            culprit = memberEntry.name;
            culpritIsConst = memberEntry.kind == SymbolKind::Const;
            break;
        }

        const bool throughObject = IsReference(NonOptional(current));
        if (throughObject) {
            reason = WritablePath;
        }

        if (tree.Node(step).kind == SyntaxKind::Member) {
            const uint32 member = c.NodeSymbol(context.module, step);
            if (member == NoSymbol) {
                return;
            }

            const Symbol& memberEntry = c.symbols_[member];
            DiagnosticCode memberReason = WritablePath;
            if (memberEntry.kind == SymbolKind::Field && throughObject) {
                memberReason = DiagnosticCode::ForeignFieldWrite; // 다른 객체의 Sparks 필드
            } else if (memberEntry.kind == SymbolKind::Field &&
                       (memberEntry.flags & SymbolFlags::Let) != 0) {
                memberReason = DiagnosticCode::LetWrite; // struct의 `let` 필드
            } else if (memberEntry.kind == SymbolKind::EngineField &&
                       !IsEngineFieldWritable(member)) {
                memberReason = DiagnosticCode::ReadOnlyProperty;
            } else if (memberEntry.kind == SymbolKind::LanguageField &&
                       (memberEntry.flags & SymbolFlags::ReadOnly) != 0) {
                memberReason = DiagnosticCode::ReadOnlyProperty;
            } else if (memberEntry.kind != SymbolKind::Field &&
                       memberEntry.kind != SymbolKind::EngineField &&
                       memberEntry.kind != SymbolKind::LanguageField) {
                memberReason = DiagnosticCode::NotAssignable; // 함수·이벤트·enum case 등
            }

            if (reason == WritablePath && memberReason != WritablePath) {
                reason = memberReason;
                culprit = memberEntry.name;
                culpritIsConst = false;
            }
        }

        current = c.ExpressionType(context.module, step);
    }

    StringBuilder<CompilationMessageInlineBytes> message(allocator_);
    StringView explanation;
    StringView fix;
    switch (reason) {
    case WritablePath:
        return;
    case DiagnosticCode::LetWrite:
        message.AppendFormat(culpritIsConst
                                 ? StringView("'{}' is a constant and cannot be changed.")
                                 : StringView("'{}' is declared with 'let' and cannot be changed."),
                             culprit);
        explanation = culpritIsConst
                          ? StringView("A constant keeps the value written in its declaration.")
                          : StringView("A 'let' value and the values inside it stay the same "
                                       "after creation.");
        fix = culpritIsConst ? StringView("Use a 'var' field or a local variable.")
                             : StringView("Declare it with 'var'.");
        break;
    case DiagnosticCode::ParameterWrite:
        message.AppendFormat("'{}' cannot be changed.", culprit);
        explanation = "Parameters, loop variables and 'if (let)' names keep the value they got.";
        fix = "Copy it into a local 'var' and change the copy.";
        break;
    case DiagnosticCode::StructSelfWrite:
        message.AppendFormat("A struct function cannot change its field '{}'.", culprit);
        explanation = "Struct functions compute values; they do not change the struct.";
        fix = "Return the changed value and assign it where the struct is stored.";
        break;
    case DiagnosticCode::ForeignFieldWrite:
        message.AppendFormat("'{}' belongs to another object and can only be read.", culprit);
        explanation = "A class changes its own fields; other objects read them.";
        fix = "Call a function of that class that changes the field.";
        break;
    case DiagnosticCode::ReadOnlyProperty:
        message.AppendFormat("'{}' is read-only.", culprit);
        explanation = "This property can be read but not written by scripts.";
        fix = "Use the API that changes it.";
        break;
    default:
        message.AppendFormat("'{}' cannot be assigned to.", culprit);
        explanation = "Only variables, fields and elements are assigned to.";
        fix = "Assign to a variable or a field.";
        break;
    }

    if (binder_.Succeeded(message)) {
        Report(context, reason, reportNode, message.View(), explanation, fix);
    }
}

} // namespace rocks::sparks::detail
