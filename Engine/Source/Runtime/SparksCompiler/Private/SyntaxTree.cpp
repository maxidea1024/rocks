#include <SparksCompiler/SyntaxTree.h>

#include <Platform/Fatal.h>

#include "SyntaxTreeBuilder.h"

// 구문 트리 접근과 ParseFile 진입점이다(분석 자체는 SyntaxTreeBuilder.h의 파일들).

namespace rocks::sparks {

SyntaxTree::SyntaxTree(Allocator& allocator) noexcept
    : nodes_(allocator), children_(allocator), tokens_(allocator) {
}

const SyntaxNode& SyntaxTree::Node(uint32 id) const noexcept {
    if (id >= nodes_.Size()) {
        platform::Fatal("Sparks syntax node id is out of range");
    }

    return nodes_[id];
}

Span<const uint32> SyntaxTree::Children(uint32 id) const noexcept {
    const SyntaxNode& node = Node(id);
    return Span<const uint32>(children_.Data() + node.firstChild, node.childCount);
}

uint32 SyntaxTree::Child(uint32 id, uint32 index) const noexcept {
    const SyntaxNode& node = Node(id);
    if (index >= node.childCount) {
        platform::Fatal("Sparks syntax child index is out of range");
    }

    return children_[node.firstChild + index];
}

Result<void> ParseFile(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                       SyntaxTree& tree) noexcept {
    if (!sources.Get(id).IsUsable()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::SyntaxTreeBuilder builder(sources, id, diagnostics, tree);
    return builder.Run();
}

StringView SyntaxKindName(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::Error:
        return "Error";
    case SyntaxKind::File:
        return "File";
    case SyntaxKind::List:
        return "List";
    case SyntaxKind::Name:
        return "Name";
    case SyntaxKind::Discard:
        return "Discard";
    case SyntaxKind::Path:
        return "Path";
    case SyntaxKind::DocComment:
        return "DocComment";
    case SyntaxKind::Attribute:
        return "Attribute";
    case SyntaxKind::Argument:
        return "Argument";
    case SyntaxKind::Import:
        return "Import";
    case SyntaxKind::Class:
        return "Class";
    case SyntaxKind::Interface:
        return "Interface";
    case SyntaxKind::InterfaceMethod:
        return "InterfaceMethod";
    case SyntaxKind::Struct:
        return "Struct";
    case SyntaxKind::Enum:
        return "Enum";
    case SyntaxKind::EnumCase:
        return "EnumCase";
    case SyntaxKind::ExtensionEvent:
        return "ExtensionEvent";
    case SyntaxKind::Const:
        return "Const";
    case SyntaxKind::Function:
        return "Function";
    case SyntaxKind::Field:
        return "Field";
    case SyntaxKind::Event:
        return "Event";
    case SyntaxKind::Handler:
        return "Handler";
    case SyntaxKind::EventPath:
        return "EventPath";
    case SyntaxKind::Parameter:
        return "Parameter";
    case SyntaxKind::Type:
        return "Type";
    case SyntaxKind::Block:
        return "Block";
    case SyntaxKind::LocalVariable:
        return "LocalVariable";
    case SyntaxKind::ExitReturn:
        return "ExitReturn";
    case SyntaxKind::If:
        return "If";
    case SyntaxKind::IfLet:
        return "IfLet";
    case SyntaxKind::For:
        return "For";
    case SyntaxKind::While:
        return "While";
    case SyntaxKind::Switch:
        return "Switch";
    case SyntaxKind::SwitchCase:
        return "SwitchCase";
    case SyntaxKind::SwitchDefault:
        return "SwitchDefault";
    case SyntaxKind::Return:
        return "Return";
    case SyntaxKind::Emit:
        return "Emit";
    case SyntaxKind::Break:
        return "Break";
    case SyntaxKind::Continue:
        return "Continue";
    case SyntaxKind::Assignment:
        return "Assignment";
    case SyntaxKind::DiscardAssignment:
        return "DiscardAssignment";
    case SyntaxKind::ExpressionStatement:
        return "ExpressionStatement";
    case SyntaxKind::Try:
        return "Try";
    case SyntaxKind::Await:
        return "Await";
    case SyntaxKind::Binary:
        return "Binary";
    case SyntaxKind::Unary:
        return "Unary";
    case SyntaxKind::CastTest:
        return "CastTest";
    case SyntaxKind::Member:
        return "Member";
    case SyntaxKind::Call:
        return "Call";
    case SyntaxKind::Index:
        return "Index";
    case SyntaxKind::Identifier:
        return "Identifier";
    case SyntaxKind::This:
        return "This";
    case SyntaxKind::IntegerLiteral:
        return "IntegerLiteral";
    case SyntaxKind::FloatLiteral:
        return "FloatLiteral";
    case SyntaxKind::BoolLiteral:
        return "BoolLiteral";
    case SyntaxKind::NullLiteral:
        return "NullLiteral";
    case SyntaxKind::VerbatimString:
        return "VerbatimString";
    case SyntaxKind::String:
        return "String";
    case SyntaxKind::StringText:
        return "StringText";
    case SyntaxKind::Interpolation:
        return "Interpolation";
    case SyntaxKind::Parenthesized:
        return "Parenthesized";
    case SyntaxKind::CollectionLiteral:
        return "CollectionLiteral";
    case SyntaxKind::MapEntry:
        return "MapEntry";
    }

    return "Unknown";
}

} // namespace rocks::sparks
