#include "CompilationChecker.h"
#include "TypeChecker.h"

// 타입 표기의 타입 인자 제약이다(Docs/Design/SparksLanguage.ko.md 3.10절 "타입 인자 제약"). 이름
// 해석이 정한 모든 타입 표기(매개변수·필드·지역·반환·이벤트 매개변수·식 안의 타입 인자·`as?`
// 대상)를 원문 순서로 한 번씩 보며, 위반은 그 타입 인자의 표기 자리에 하나다(바깥 표기는 다시 보지
// 않는다).

namespace rocks::sparks::detail {

namespace {

// `?`를 담을 수 없는 prelude 제네릭이다(컨테이너와
// `Result`·`Soft`·`Row`·`LoadRequest`·`CVarRef`·`Class`).
constexpr StringView NonOptionalArgumentTypes[] = {
    "Array", "Set", "Map", "Result", "Soft", "Row", "LoadRequest", "CVarRef", "Class"};

} // namespace

bool TypeChecker::IsClassType(uint32 type) const noexcept {
    const uint32 symbol = SymbolOf(type);
    if (symbol == NoSymbol || IsOptional(type)) {
        return false;
    }

    const Symbol& entry = compilation_.symbols_[symbol];
    if (entry.kind == SymbolKind::Class) {
        return true;
    }

    return entry.kind == SymbolKind::EngineType &&
           compilation_.schemas_[entry.index].schema.Types()[entry.item].kind ==
               SchemaTypeKind::Object;
}

void TypeChecker::CheckTypeRules(uint32 module) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    CheckContext context;
    context.module = module;

    // `void`를 쓸 수 있는 표기: 함수·interface 메서드의 반환 타입과 `Result`의 타입 인자.
    Array<bool> voidAllowed(allocator_);
    if (auto resized = voidAllowed.TryResize(tree.NodeCount()); !resized) {
        binder_.error_ = resized.Error();
        return;
    }

    for (uint32 node = 0; node < tree.NodeCount(); ++node) {
        voidAllowed[node] = false;
    }

    for (uint32 node = 0; node < tree.NodeCount(); ++node) {
        const SyntaxNode& entry = tree.Node(node);
        if ((entry.kind == SyntaxKind::Function || entry.kind == SyntaxKind::InterfaceMethod) &&
            entry.childCount > 4 && tree.Child(node, 4) != NoNode) {
            voidAllowed[tree.Child(node, 4)] = true;
        } else if (entry.kind == SyntaxKind::Type &&
                   IsLanguage(NonOptional(c.NodeType(module, node)), "Result")) {
            const uint32 list = tree.Child(node, 1);
            if (list != NoNode && tree.Node(list).kind == SyntaxKind::List &&
                tree.Node(list).childCount != 0) {
                voidAllowed[tree.Child(list, 0)] = true;
            }
        }
    }

    for (uint32 node = 0; node < tree.NodeCount() && binder_.error_ == ErrorCode::None; ++node) {
        if (tree.Node(node).kind != SyntaxKind::Type) {
            continue;
        }

        const uint32 type = c.NodeType(module, node);
        if (type == NoType) {
            continue;
        }

        if (NonOptional(type) == voidType_ && (IsOptional(type) || !voidAllowed[node])) {
            Report(context, DiagnosticCode::NotAValue, node, "'void' is not a type of values.",
                   "'void' is only a return type and the type argument of 'Result<void>'.",
                   "Remove the type, or use 'Result<void>'.");
            continue;
        }

        const uint32 list = tree.Child(node, 1);
        if (list == NoNode || tree.Node(list).kind != SyntaxKind::List) {
            continue;
        }

        const Span<const uint32> arguments = tree.Children(list);
        const uint32 generic = NonOptional(type);
        bool optionalFree = false;
        StringView genericName;
        for (const StringView name : NonOptionalArgumentTypes) {
            if (IsLanguage(generic, name)) {
                optionalFree = true;
                genericName = name;
            }
        }

        if (!optionalFree) {
            continue;
        }

        for (usize index = 0; index < arguments.Size(); ++index) {
            const uint32 argumentNode = arguments[index];
            const uint32 argument = c.NodeType(module, argumentNode);
            if (argument == NoType || NonOptional(argument) == voidType_) {
                continue; // `void` 표기는 그 자리에서 따로 판정한다
            }

            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            if (IsOptional(argument)) {
                message.AppendFormat("'{}' cannot hold a type with '?'.", genericName);
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::TypeMismatch, argumentNode, message.View(),
                           genericName == "Result"
                               ? StringView("A missing value is an error case, not a successful "
                                            "null.")
                               : StringView("Containers and references hold values, not nulls."),
                           genericName == "Result"
                               ? StringView("Return 'Result<T>' and an 'Error' value instead of "
                                            "null.")
                               : StringView("Remove the '?'."));
                }

                continue;
            }

            const bool keyed = (genericName == "Map" && index == 0) || genericName == "Set";
            if (keyed && !IsEquatable(argument)) {
                message.Append("'");
                AppendDisplayType(message, argument);
                message.Append(genericName == "Map" ? StringView("' cannot be a map key.")
                                                    : StringView("' cannot be a set element."));
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::NotComparable, argumentNode, message.View(),
                           "Map keys and set elements are found with '=='.",
                           "Use a type that has '==', such as an integer, string, enum or tag.");
                }

                continue;
            }

            const bool softWrong = genericName == "Soft" && !IsAssetType(argument);
            const bool classWrong = genericName == "Class" && !IsClassType(argument);
            if (softWrong || classWrong) {
                message.Append("'");
                AppendDisplayType(message, argument);
                message.Append(softWrong ? StringView("' is not an asset type.")
                                         : StringView("' is not a class type."));
                if (binder_.Succeeded(message)) {
                    Report(context, DiagnosticCode::TypeArgumentRule, argumentNode, message.View(),
                           softWrong ? StringView("'Soft<T>' refers to an asset.")
                                     : StringView("'Class<T>' refers to a Sparks class or an "
                                                  "engine object type."),
                           softWrong ? StringView("Use an asset type, as in 'Soft<SoundAsset>'.")
                                     : StringView("Use a class type, as in 'Class<Enemy>'."));
                }
            }
        }
    }
}

} // namespace rocks::sparks::detail
