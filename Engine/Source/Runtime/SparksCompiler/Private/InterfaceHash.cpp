#include <Core/Encoding/Sha256.h>
#include <Core/Text/StringBuilder.h>

#include "CompilationChecker.h"
#include "TypeChecker.h"

// 모듈 interface의 정규 텍스트와 재컴파일 범위다(Docs/Design/SparksBytecode.ko.md 6.2절, LC4d).
// interface hash는 공개 표면(import, 선언의 이름·attribute·기반·필드 타입·초기값 원문·const·함수
// 시그니처·LC3b 효과 집합·이벤트)을 선언 순으로 쓴 글자의 SHA-256이다. 원문 위치·주석·본문은 넣지
// 않는다.

namespace rocks::sparks::detail {

void TypeChecker::AppendInterfaceType(StringBuilderBase& out, uint32 module, uint32 type) noexcept {
    // 다른 모듈의 Sparks 타입은 import 경로로 구별한다(같은 이름의 두 모듈 선언).
    const Compilation& c = compilation_;
    const uint32 symbol = type == NoType ? NoSymbol : SymbolOf(NonOptional(type));
    if (symbol != NoSymbol && c.symbols_[symbol].module < c.modules_.Size() &&
        c.symbols_[symbol].module != module) {
        out.Append(c.Text(c.modules_[c.symbols_[symbol].module].importPath)).Append(":");
    }

    if (type == NoType) {
        out.Append("void");
        return;
    }

    AppendDisplayType(out, type);
}

void TypeChecker::AppendInterfaceText(StringBuilderBase& out, uint32 module) noexcept {
    const Compilation& c = compilation_;
    const SyntaxTree& tree = c.trees_[module];
    const StringView source = c.sources_->Get(c.modules_[module].source).Text();
    auto text = [&](uint32 node) noexcept {
        const SourceSpan span = tree.Node(node).span;
        return StringView(source.Data() + span.begin, span.end - span.begin);
    };
    auto attributes = [&](uint32 declaration) noexcept {
        const uint32 list = tree.Child(declaration, 1);
        if (list == NoNode || tree.Node(list).kind != SyntaxKind::List) {
            return;
        }

        for (const uint32 attribute : tree.Children(list)) {
            out.Append(" ").Append(text(attribute));
        }
    };
    auto signature = [&](uint32 symbol) noexcept {
        const Symbol& entry = c.symbols_[symbol];
        if (entry.signature == NoSignature) {
            return;
        }

        const Signature& shape = c.signatures_[entry.signature];
        out.Append("(");
        for (uint32 index = 0; index < shape.parameterCount; ++index) {
            out.Append(index == 0 ? "" : ", ");
            AppendInterfaceType(out, module, c.signatureParameters_[shape.firstParameter + index]);
        }

        out.Append("): ");
        AppendInterfaceType(out, module, shape.returnType);
        out.AppendFormat("{}", shape.isAsync ? " async" : "");
    };
    auto effects = [&](uint32 symbol) noexcept {
        const uint32 body = symbol < effectOfSymbol_.Size() ? effectOfSymbol_[symbol] : NoSymbol;
        if (body < effectBodies_.Size()) {
            out.AppendFormat(" effects={:x}", effectBodies_[body].total);
        }
    };
    auto function = [&](uint32 node, StringView indent) noexcept {
        const uint32 symbol = c.NodeSymbol(module, tree.Child(node, 2));
        if (symbol == NoSymbol) {
            return;
        }

        const uint32 flags = tree.Node(node).flags;
        out.Append(indent).Append("fn ").Append(c.symbols_[symbol].name);
        signature(symbol);
        out.AppendFormat("{}{}", (flags & SyntaxFlags::Open) != 0 ? " open" : "",
                         (flags & SyntaxFlags::Override) != 0 ? " override" : "");
        attributes(node);
        effects(symbol);
        out.Append("\n");
    };

    // import(해석한 대상 모듈의 경로, 선언 순).
    const Compilation::ModuleEntry& entry = c.modules_[module];
    for (uint32 index = entry.firstImport; index < entry.firstImport + entry.importCount; ++index) {
        const uint32 target = c.imports_[index].module;
        if (target != NoModule) {
            out.Append("import ").Append(c.Text(c.modules_[target].importPath)).Append("\n");
        }
    }

    const uint32 root = tree.Root();
    const uint32 declarations = root == NoNode ? NoNode : tree.Child(root, 1);
    if (declarations == NoNode) {
        return;
    }

    for (const uint32 declaration : tree.Children(declarations)) {
        const SyntaxNode& node = tree.Node(declaration);
        switch (node.kind) {
        case SyntaxKind::Function:
            function(declaration, "");
            break;
        case SyntaxKind::Const: {
            const uint32 symbol = c.NodeSymbol(module, tree.Child(declaration, 2));
            if (symbol == NoSymbol) {
                break;
            }

            out.Append("const ").Append(c.symbols_[symbol].name).Append(": ");
            AppendInterfaceType(out, module, c.symbols_[symbol].type);
            out.Append(" = ").Append(text(tree.Child(declaration, 4)));
            attributes(declaration);
            out.Append("\n");
            break;
        }
        case SyntaxKind::ExtensionEvent: {
            out.Append("event ").Append(text(tree.Child(declaration, 2))).Append(".");
            out.Append(text(tree.Child(declaration, 3))).Append("(");
            const uint32 parameters = tree.Child(declaration, 4);
            bool first = true;
            for (const uint32 parameter : tree.Children(parameters)) {
                out.Append(first ? "" : ", ");
                AppendInterfaceType(out, module, c.NodeType(module, tree.Child(parameter, 1)));
                first = false;
            }

            out.Append(")");
            attributes(declaration);
            out.Append("\n");
            break;
        }
        case SyntaxKind::Class:
        case SyntaxKind::Struct:
        case SyntaxKind::Interface:
        case SyntaxKind::Enum: {
            const uint32 symbol = c.NodeSymbol(module, tree.Child(declaration, 2));
            if (symbol == NoSymbol) {
                break;
            }

            const Symbol& type = c.symbols_[symbol];
            out.Append(node.kind == SyntaxKind::Class       ? "class "
                       : node.kind == SyntaxKind::Struct    ? "struct "
                       : node.kind == SyntaxKind::Interface ? "interface "
                                                            : "enum ");
            out.Append(type.name);
            out.AppendFormat("{}", (node.flags & SyntaxFlags::Open) != 0 ? " open" : "");
            for (uint32 at = type.firstBase; at < type.firstBase + type.baseCount; ++at) {
                out.Append(at == type.firstBase ? " : " : ", ");
                AppendInterfaceType(out, module, TypeOfSymbol(c.symbolBases_[at]));
            }

            attributes(declaration);
            out.Append("\n");
            const uint32 members = tree.Child(declaration, node.kind == SyntaxKind::Class ? 4 : 3);
            if (members == NoNode || tree.Node(members).kind != SyntaxKind::List) {
                break;
            }

            for (const uint32 member : tree.Children(members)) {
                const SyntaxNode& item = tree.Node(member);
                if (item.kind == SyntaxKind::Function || item.kind == SyntaxKind::InterfaceMethod) {
                    function(member, "  ");
                    continue;
                }

                const uint32 memberSymbol =
                    item.childCount > 2 ? c.NodeSymbol(module, tree.Child(member, 2)) : NoSymbol;
                if (memberSymbol == NoSymbol) {
                    continue;
                }

                if (item.kind == SyntaxKind::EnumCase) {
                    out.Append("  case ").Append(c.symbols_[memberSymbol].name);
                } else if (item.kind == SyntaxKind::Field) {
                    out.AppendFormat("  {} ", (item.flags & SyntaxFlags::Let) != 0 ? "let" : "var");
                    out.Append(c.symbols_[memberSymbol].name).Append(": ");
                    AppendInterfaceType(out, module, FieldType(memberSymbol));
                    const uint32 initializer = tree.Child(member, 4);
                    if (initializer != NoNode) {
                        out.Append(" = ").Append(text(initializer));
                    }
                } else if (item.kind == SyntaxKind::Event) {
                    out.Append("  event ").Append(c.symbols_[memberSymbol].name);
                    signature(memberSymbol);
                } else {
                    continue; // handler는 interface가 아니다
                }

                attributes(member);
                out.Append("\n");
            }

            break;
        }
        default:
            break;
        }
    }
}

} // namespace rocks::sparks::detail

namespace rocks::sparks {

Result<void> FindRecompileScope(const Compilation& before, const Compilation& after,
                                Array<uint32>& modules, Array<uint64>& functions) noexcept {
    // 모듈은 import 경로로, 함수는 이름으로 맞춘다. interface가 바뀐(또는 새) 모듈과 그것을
    // 전이적으로 import하는 모듈을 다시 검사하고, interface가 같은 모듈의 본문이 바뀐 함수를 다시
    // 내린다.
    auto previous = [&](uint32 module) noexcept -> const BytecodeModule* {
        const StringView path = after.ModuleImportPath(module);
        for (uint32 index = 0; index < before.ModuleCount(); ++index) {
            if (before.ModuleImportPath(index) == path &&
                before.ModulePackage(index) < before.PackageCount() &&
                before.Manifest(before.ModulePackage(index)).Name() ==
                    after.Manifest(after.ModulePackage(module)).Name()) {
                return before.ModuleBytecode(index);
            }
        }

        return nullptr;
    };
    auto same = [](const uint8(&left)[BytecodeHashBytes],
                   const uint8(&right)[BytecodeHashBytes]) noexcept {
        for (usize index = 0; index < BytecodeHashBytes; ++index) {
            if (left[index] != right[index]) {
                return false;
            }
        }

        return true;
    };
    for (uint32 module = 0; module < after.ModuleCount(); ++module) {
        if (after.ModuleTree(module) != nullptr && after.ModuleBytecode(module) == nullptr) {
            return Result<void>::Failure(ErrorCode::InvalidArgument); // 내리지 못한 모듈
        }
    }

    Array<uint8> changed(modules.GetAllocator());
    if (auto resized = changed.TryResize(after.ModuleCount()); !resized) {
        return resized;
    }

    for (uint32 module = 0; module < after.ModuleCount(); ++module) {
        const BytecodeModule* now = after.ModuleBytecode(module);
        if (now == nullptr) {
            continue;
        }

        const BytecodeModule* old = previous(module);
        if (old == nullptr || !same(old->interfaceHash, now->interfaceHash)) {
            changed[module] = 1;
            continue;
        }

        for (uint32 index = 0; index < now->functions.Size(); ++index) {
            const StringView name = now->String(now->functions[index].name);
            bool found = false;
            for (const BytecodeFunction& candidate : old->functions) {
                if (old->String(candidate.name) == name) {
                    found = same(candidate.bodyHash, now->functions[index].bodyHash);
                    break;
                }
            }

            if (!found) {
                if (auto pushed = functions.TryPushBack((uint64(module) << 32) | index); !pushed) {
                    return pushed;
                }
            }
        }
    }

    // import 관계로 전이: 바뀐 모듈을 import하는 모듈도 바뀐 것으로 본다(고정점).
    for (bool grew = true; grew;) {
        grew = false;
        for (uint32 module = 0; module < after.ModuleCount(); ++module) {
            if (changed[module] != 0) {
                continue;
            }

            for (const ImportResolution& import : after.ModuleImports(module)) {
                if (import.module != NoModule && changed[import.module] != 0) {
                    changed[module] = 1;
                    grew = true;
                    break;
                }
            }
        }
    }

    for (uint32 module = 0; module < after.ModuleCount(); ++module) {
        if (changed[module] != 0) {
            if (auto pushed = modules.TryPushBack(module); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::sparks
