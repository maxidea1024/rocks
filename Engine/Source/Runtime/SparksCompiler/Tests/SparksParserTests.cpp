#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksCompiler/SyntaxTree.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// Sparks 구문 분석 시험이다(Docs/Design/SparksLanguage.ko.md 3.6절 EBNF, 8.1절 "구문 분석"·"복구"·
// "문법 세부", S0201–S0219). 구조는 시험용 S-식(아래 AppendTree)으로 비교하고, 위치까지 포함한 덤프
// 형식은 DumpFormat 시험과 conformance corpus(Syntax 영역의 .ast)가 맡는다. 모든 분석 결과는
// ValidTree로 트리 불변식(종류별 자식 수, 범위 포함·순서, 고아 노드 없음)을 확인한다.
using namespace rocks;
using namespace rocks::sparks;
namespace sparks_parser_tests_file {

namespace {

constexpr usize TextInlineBytes = 512;

struct Parsed {
    explicit Parsed(Allocator& owner) noexcept
        : allocator(owner), sources(owner), diagnostics(owner), tree(owner) {
    }

    Allocator& allocator;
    SourceSet sources;
    DiagnosticList diagnostics;
    SyntaxTree tree;
    SourceId id;
};

bool Parse(Parsed& parsed, StringView text) noexcept {
    auto added = parsed.sources.TryAdd("Test.spark", text, parsed.diagnostics);
    if (!added || !parsed.sources.Get(added.Value()).IsUsable()) {
        return false;
    }

    parsed.id = added.Value();
    if (!ParseFile(parsed.sources, parsed.id, parsed.diagnostics, parsed.tree)) {
        return false;
    }

    parsed.diagnostics.Sort(parsed.sources);
    return true;
}

StringView TokenText(const Parsed& parsed, uint32 token) noexcept {
    const SourceSpan span = parsed.tree.Tokens()[token].span;
    return StringView(parsed.sources.Get(parsed.id).Text().Data() + span.begin,
                      span.end - span.begin);
}

void AppendTree(StringBuilderBase& out, const Parsed& parsed, uint32 node) noexcept;

void AppendTypeArguments(StringBuilderBase& out, const Parsed& parsed, uint32 list) noexcept {
    out.Append("<");
    const Span<const uint32> children = parsed.tree.Children(list);
    for (usize index = 0; index < children.Size(); ++index) {
        if (index != 0) {
            out.Append(", ");
        }

        AppendTree(out, parsed, children[index]);
    }

    out.Append(">");
}

// 시험용 S-식이다. 이름·리터럴은 토큰 글자, Path는 점으로 이은 이름, Type은 `A.B<C>?`, List는
// `[...]`, 인자·map 원소는 `이름: 값`, 문자열 조각은 '...'이다. 그 밖은 `(종류[:flag] [토큰]
// 자식...)`이며 없는 선택 자식은 `-`다.
void AppendTree(StringBuilderBase& out, const Parsed& parsed, uint32 node) noexcept {
    if (node == NoNode) {
        out.Append("-");
        return;
    }

    const SyntaxNode& value = parsed.tree.Node(node);
    const Span<const uint32> children = parsed.tree.Children(node);
    switch (value.kind) {
    case SyntaxKind::Name:
    case SyntaxKind::Discard:
    case SyntaxKind::IntegerLiteral:
    case SyntaxKind::FloatLiteral:
    case SyntaxKind::BoolLiteral:
    case SyntaxKind::NullLiteral:
    case SyntaxKind::This:
    case SyntaxKind::VerbatimString:
        out.Append(TokenText(parsed, value.token));
        return;
    case SyntaxKind::StringText:
        out.Append("'").Append(TokenText(parsed, value.token)).Append("'");
        return;
    case SyntaxKind::Identifier:
        out.Append(TokenText(parsed, value.token));
        if (children[0] != NoNode) {
            AppendTypeArguments(out, parsed, children[0]);
        }

        return;
    case SyntaxKind::Path:
        for (usize index = 0; index < children.Size(); ++index) {
            if (index != 0) {
                out.Append(".");
            }

            AppendTree(out, parsed, children[index]);
        }

        return;
    case SyntaxKind::Type:
        AppendTree(out, parsed, children[0]);
        if (children[1] != NoNode) {
            AppendTypeArguments(out, parsed, children[1]);
        }

        if ((value.flags & SyntaxFlags::Optional) != 0) {
            out.Append("?");
        }

        return;
    case SyntaxKind::List:
        out.Append("[");
        for (usize index = 0; index < children.Size(); ++index) {
            if (index != 0) {
                out.Append(" ");
            }

            AppendTree(out, parsed, children[index]);
        }

        out.Append("]");
        return;
    case SyntaxKind::Argument:
    case SyntaxKind::MapEntry:
        if (children[0] != NoNode) {
            AppendTree(out, parsed, children[0]);
            out.Append(": ");
        }

        AppendTree(out, parsed, children[1]);
        return;
    default:
        break;
    }

    out.Append("(").Append(SyntaxKindName(value.kind));
    struct Flag {
        uint32 bit;
        StringView name;
    };
    constexpr Flag Flags[] = {
        {SyntaxFlags::Open, "open"},          {SyntaxFlags::Override, "override"},
        {SyntaxFlags::Async, "async"},        {SyntaxFlags::Let, "let"},
        {SyntaxFlags::NullConditional, "?."}, {SyntaxFlags::Await, "await"}};
    for (const Flag& flag : Flags) {
        if ((value.flags & flag.bit) != 0) {
            out.Append(":").Append(flag.name);
        }
    }

    if (value.token != NoToken) {
        out.Append(" ").Append(TokenText(parsed, value.token));
    }

    for (const uint32 child : children) {
        out.Append(" ");
        AppendTree(out, parsed, child);
    }

    out.Append(")");
}

void Print(StringView text) noexcept {
    platform::WriteDiagnostic(text.Data(), text.ByteCount());
    platform::WriteDiagnostic("\n", 1);
}

void PrintDiagnostics(const Parsed& parsed) noexcept {
    StringBuilder<TextInlineBytes> out(parsed.allocator);
    static_cast<void>(AppendDiagnostics(out, parsed.sources, parsed.diagnostics));
    Print(out.View());
}

// 자식 수가 정해진 종류의 자식 수다(가변이면 -1, 하나 이상이면 -2).
int FixedChildCount(SyntaxKind kind) noexcept {
    switch (kind) {
    case SyntaxKind::Error:
    case SyntaxKind::Name:
    case SyntaxKind::Discard:
    case SyntaxKind::DocComment:
    case SyntaxKind::Break:
    case SyntaxKind::Continue:
    case SyntaxKind::This:
    case SyntaxKind::IntegerLiteral:
    case SyntaxKind::FloatLiteral:
    case SyntaxKind::BoolLiteral:
    case SyntaxKind::NullLiteral:
    case SyntaxKind::VerbatimString:
    case SyntaxKind::StringText:
        return 0;
    case SyntaxKind::Attribute:
    case SyntaxKind::ExitReturn:
    case SyntaxKind::SwitchDefault:
    case SyntaxKind::Return:
    case SyntaxKind::DiscardAssignment:
    case SyntaxKind::ExpressionStatement:
    case SyntaxKind::Try:
    case SyntaxKind::Await:
    case SyntaxKind::Unary:
    case SyntaxKind::Identifier:
    case SyntaxKind::Interpolation:
    case SyntaxKind::Parenthesized:
        return 1;
    case SyntaxKind::File:
    case SyntaxKind::Argument:
    case SyntaxKind::Import:
    case SyntaxKind::Parameter:
    case SyntaxKind::Type:
    case SyntaxKind::IfLet:
    case SyntaxKind::While:
    case SyntaxKind::SwitchCase:
    case SyntaxKind::Emit:
    case SyntaxKind::Assignment:
    case SyntaxKind::Binary:
    case SyntaxKind::CastTest:
    case SyntaxKind::Member:
    case SyntaxKind::Call:
    case SyntaxKind::Index:
    case SyntaxKind::MapEntry:
        return 2;
    case SyntaxKind::EnumCase:
    case SyntaxKind::If:
    case SyntaxKind::Switch:
        return 3;
    case SyntaxKind::Interface:
    case SyntaxKind::Struct:
    case SyntaxKind::Enum:
    case SyntaxKind::Event:
    case SyntaxKind::LocalVariable:
        return 4;
    case SyntaxKind::Class:
    case SyntaxKind::InterfaceMethod:
    case SyntaxKind::ExtensionEvent:
    case SyntaxKind::Const:
    case SyntaxKind::Field:
    case SyntaxKind::Handler:
    case SyntaxKind::For:
        return 5;
    case SyntaxKind::Function:
        return 6;
    case SyntaxKind::Path:
    case SyntaxKind::EventPath:
        return -2;
    case SyntaxKind::List:
    case SyntaxKind::Block:
    case SyntaxKind::String:
    case SyntaxKind::CollectionLiteral:
        return -1;
    }

    return -1;
}

using K = SyntaxKind;

constexpr uint64 Bit(SyntaxKind kind) noexcept {
    return uint64{1} << static_cast<uint32>(kind);
}

static_assert(static_cast<uint32>(SyntaxKind::MapEntry) < 64, "SyntaxKind bits must fit in uint64");

constexpr uint64 ExpressionKinds =
    Bit(K::Binary) | Bit(K::Unary) | Bit(K::CastTest) | Bit(K::Member) | Bit(K::Call) |
    Bit(K::Index) | Bit(K::Identifier) | Bit(K::This) | Bit(K::IntegerLiteral) |
    Bit(K::FloatLiteral) | Bit(K::BoolLiteral) | Bit(K::NullLiteral) | Bit(K::VerbatimString) |
    Bit(K::String) | Bit(K::Parenthesized) | Bit(K::CollectionLiteral) | Bit(K::Try) |
    Bit(K::Await);
constexpr uint64 StatementKinds = Bit(K::LocalVariable) | Bit(K::If) | Bit(K::For) | Bit(K::While) |
                                  Bit(K::Switch) | Bit(K::Return) | Bit(K::Emit) | Bit(K::Break) |
                                  Bit(K::Continue) | Bit(K::Assignment) |
                                  Bit(K::DiscardAssignment) | Bit(K::ExpressionStatement);
constexpr uint64 DeclarationKinds = Bit(K::Class) | Bit(K::Struct) | Bit(K::Enum) |
                                    Bit(K::Interface) | Bit(K::Const) | Bit(K::ExtensionEvent) |
                                    Bit(K::Function);

// 자식 자리 하나의 규칙이다. Error 노드는 어느 자리든 된다(SyntaxTree.h).
struct Slot {
    uint64 kinds = 0;      // 허용하는 종류
    bool optional = false; // NoNode 허용
    uint64 listOf = 0;     // 0이 아니면 List이고 원소가 이 종류(Error 허용, NoNode 불가)
};

constexpr Slot One(uint64 kinds) noexcept {
    return Slot{kinds, false, 0};
}

constexpr Slot Optional(uint64 kinds) noexcept {
    return Slot{kinds, true, 0};
}

constexpr Slot ListOf(uint64 kinds, bool optional = false) noexcept {
    return Slot{Bit(K::List), optional, kinds};
}

bool ElementsFit(const SyntaxTree& tree, uint32 node, uint64 kinds) noexcept {
    for (const uint32 element : tree.Children(node)) {
        if (element == NoNode) {
            return false;
        }

        const SyntaxKind kind = tree.Node(element).kind;
        if (kind != K::Error && (kinds & Bit(kind)) == 0) {
            return false;
        }
    }

    return true;
}

bool SlotFits(const SyntaxTree& tree, uint32 child, Slot slot) noexcept {
    if (child == NoNode) {
        return slot.optional;
    }

    const SyntaxKind kind = tree.Node(child).kind;
    if (kind == K::Error) {
        return true;
    }

    if ((slot.kinds & Bit(kind)) == 0) {
        return false;
    }

    return slot.listOf == 0 || ElementsFit(tree, child, slot.listOf);
}

bool SlotsFit(const SyntaxTree& tree, uint32 node, Span<const Slot> slots) noexcept {
    const Span<const uint32> children = tree.Children(node);
    if (children.Size() != slots.Size()) {
        return false;
    }

    for (usize index = 0; index < slots.Size(); ++index) {
        if (!SlotFits(tree, children[index], slots[index])) {
            return false;
        }
    }

    return true;
}

// 노드 하나의 자식이 SyntaxTree.h의 자리 설명대로인지 본다.
bool ChildrenMatchLayout(const SyntaxTree& tree, uint32 node) noexcept {
    const Slot doc = Optional(Bit(K::DocComment));
    const Slot attributes = ListOf(Bit(K::Attribute));
    const Slot name = One(Bit(K::Name));
    const Slot parameters = ListOf(Bit(K::Parameter));
    const Slot type = One(Bit(K::Type));
    const Slot optionalType = Optional(Bit(K::Type));
    const Slot expression = One(ExpressionKinds);
    const Slot optionalExpression = Optional(ExpressionKinds);
    const Slot block = One(Bit(K::Block));
    const Slot binder = One(Bit(K::Name) | Bit(K::Discard));
    const Slot typeArguments = ListOf(Bit(K::Type), true);
    switch (tree.Node(node).kind) {
    case K::File: {
        const Slot slots[] = {ListOf(Bit(K::Import)), ListOf(DeclarationKinds)};
        return SlotsFit(tree, node, slots);
    }
    case K::Path:
        return ElementsFit(tree, node, Bit(K::Name));
    case K::EventPath:
        return ElementsFit(tree, node, Bit(K::Name) | Bit(K::This));
    case K::Attribute: {
        const Slot slots[] = {ListOf(Bit(K::Argument))};
        return SlotsFit(tree, node, slots);
    }
    case K::Argument:
    case K::IfLet: {
        const Slot slots[] = {tree.Node(node).kind == K::IfLet ? name : Optional(Bit(K::Name)),
                              expression};
        return SlotsFit(tree, node, slots);
    }
    case K::Import: {
        const Slot slots[] = {One(Bit(K::Path)), Optional(Bit(K::Name))};
        return SlotsFit(tree, node, slots);
    }
    case K::Class: {
        const Slot slots[] = {
            doc, attributes, name, ListOf(Bit(K::Path)),
            ListOf(Bit(K::Field) | Bit(K::Function) | Bit(K::Event) | Bit(K::Handler))};
        return SlotsFit(tree, node, slots);
    }
    case K::Interface: {
        const Slot slots[] = {doc, attributes, name, ListOf(Bit(K::InterfaceMethod))};
        return SlotsFit(tree, node, slots);
    }
    case K::InterfaceMethod: {
        const Slot slots[] = {doc, attributes, name, parameters, optionalType};
        return SlotsFit(tree, node, slots);
    }
    case K::Struct: {
        const Slot slots[] = {doc, attributes, name, ListOf(Bit(K::Field) | Bit(K::Function))};
        return SlotsFit(tree, node, slots);
    }
    case K::Enum: {
        const Slot slots[] = {doc, attributes, name, ListOf(Bit(K::EnumCase))};
        return SlotsFit(tree, node, slots);
    }
    case K::EnumCase: {
        const Slot slots[] = {doc, attributes, name};
        return SlotsFit(tree, node, slots);
    }
    case K::ExtensionEvent: {
        const Slot slots[] = {doc, attributes, name, name, parameters};
        return SlotsFit(tree, node, slots);
    }
    case K::Const: {
        const Slot slots[] = {doc, attributes, name, type, expression};
        return SlotsFit(tree, node, slots);
    }
    case K::Function: {
        const Slot slots[] = {doc, attributes, name, parameters, optionalType, block};
        return SlotsFit(tree, node, slots);
    }
    case K::Field: {
        const Slot slots[] = {doc, attributes, name, optionalType, optionalExpression};
        return SlotsFit(tree, node, slots);
    }
    case K::Event: {
        const Slot slots[] = {doc, attributes, name, parameters};
        return SlotsFit(tree, node, slots);
    }
    case K::Handler: {
        const Slot slots[] = {doc, attributes, One(Bit(K::EventPath)), parameters, block};
        return SlotsFit(tree, node, slots);
    }
    case K::Parameter: {
        const Slot slots[] = {binder, type};
        return SlotsFit(tree, node, slots);
    }
    case K::Type: {
        const Slot slots[] = {One(Bit(K::Path)), typeArguments};
        return SlotsFit(tree, node, slots);
    }
    case K::Block:
        return ElementsFit(tree, node, StatementKinds);
    case K::LocalVariable: {
        const Slot slots[] = {name, optionalType, expression, Optional(Bit(K::ExitReturn))};
        return SlotsFit(tree, node, slots);
    }
    case K::ExitReturn:
    case K::Return: {
        const Slot slots[] = {optionalExpression};
        return SlotsFit(tree, node, slots);
    }
    case K::If: {
        const Slot slots[] = {One(ExpressionKinds | Bit(K::IfLet)), block,
                              Optional(Bit(K::If) | Bit(K::Block))};
        return SlotsFit(tree, node, slots);
    }
    case K::For: {
        const Slot slots[] = {binder, Optional(Bit(K::Name) | Bit(K::Discard)), expression,
                              optionalExpression, block};
        return SlotsFit(tree, node, slots);
    }
    case K::While: {
        const Slot slots[] = {expression, block};
        return SlotsFit(tree, node, slots);
    }
    case K::Switch: {
        const Slot slots[] = {expression, ListOf(Bit(K::SwitchCase)),
                              Optional(Bit(K::SwitchDefault))};
        return SlotsFit(tree, node, slots);
    }
    case K::SwitchCase: {
        const Slot slots[] = {ListOf(ExpressionKinds), block};
        return SlotsFit(tree, node, slots);
    }
    case K::SwitchDefault: {
        const Slot slots[] = {block};
        return SlotsFit(tree, node, slots);
    }
    case K::Emit: {
        const Slot slots[] = {One(Bit(K::EventPath)), ListOf(ExpressionKinds)};
        return SlotsFit(tree, node, slots);
    }
    case K::Assignment:
    case K::Binary:
    case K::Index:
    case K::MapEntry: {
        const Slot slots[] = {expression, expression};
        return SlotsFit(tree, node, slots);
    }
    case K::DiscardAssignment:
    case K::ExpressionStatement:
    case K::Try:
    case K::Await:
    case K::Unary:
    case K::Interpolation:
    case K::Parenthesized: {
        const Slot slots[] = {expression};
        return SlotsFit(tree, node, slots);
    }
    case K::CastTest: {
        const Slot slots[] = {expression, One(Bit(K::Path))};
        return SlotsFit(tree, node, slots);
    }
    case K::Member: {
        const Slot slots[] = {expression, typeArguments};
        return SlotsFit(tree, node, slots);
    }
    case K::Call: {
        const Slot slots[] = {expression, ListOf(Bit(K::Argument))};
        return SlotsFit(tree, node, slots);
    }
    case K::Identifier: {
        const Slot slots[] = {typeArguments};
        return SlotsFit(tree, node, slots);
    }
    case K::String:
        return ElementsFit(tree, node, Bit(K::StringText) | Bit(K::Interpolation));
    case K::CollectionLiteral:
        return ElementsFit(tree, node, ExpressionKinds | Bit(K::MapEntry));
    case K::List:
        return true; // 원소 종류는 그 List를 가진 부모의 자리가 정한다.
    default:
        return tree.Node(node).childCount == 0;
    }
}

// 트리 불변식: 종류별 자식 수와 자리별 종류, 토큰 번호 범위, 부모 범위가 자식 범위를 포함, 형제
// 시작 위치 순서, 모든 노드가 뿌리에서 정확히 한 번 닿는다(고아·공유 노드 없음), 범위가 원문 안,
// Error 노드가 있으면 진단도 있다.
bool ValidTree(const Parsed& parsed) noexcept {
    const SyntaxTree& tree = parsed.tree;
    if (tree.Root() == NoNode || tree.Node(tree.Root()).kind != SyntaxKind::File) {
        return false;
    }

    const uint32 size = static_cast<uint32>(parsed.sources.Get(parsed.id).Text().ByteCount());
    Array<uint8> seen(parsed.allocator);
    if (!seen.TryResize(tree.NodeCount())) {
        return false;
    }

    for (usize index = 0; index < seen.Size(); ++index) {
        seen[index] = 0;
    }

    Array<uint32> pending(parsed.allocator);
    if (!pending.TryPushBack(tree.Root())) {
        return false;
    }

    uint32 visited = 0;
    while (!pending.IsEmpty()) {
        const uint32 node = pending[pending.Size() - 1];
        static_cast<void>(pending.TryPopBack());
        if (node >= tree.NodeCount() || seen[node] != 0) {
            return false;
        }

        seen[node] = 1;
        ++visited;
        const SyntaxNode& value = tree.Node(node);
        if (value.span.begin > value.span.end || value.span.end > size) {
            return false;
        }

        if (value.token != NoToken && value.token >= tree.Tokens().Size()) {
            return false;
        }

        const int expected = FixedChildCount(value.kind);
        if ((expected >= 0 && value.childCount != static_cast<uint32>(expected)) ||
            (expected == -2 && value.childCount == 0) || !ChildrenMatchLayout(tree, node)) {
            return false;
        }

        if (value.kind == SyntaxKind::Error && parsed.diagnostics.IsEmpty()) {
            return false;
        }

        uint32 previousBegin = value.span.begin;
        for (const uint32 child : tree.Children(node)) {
            if (child == NoNode) {
                continue;
            }

            const SourceSpan span = tree.Node(child).span;
            if (span.begin < value.span.begin || span.end > value.span.end ||
                span.begin < previousBegin) {
                return false;
            }

            previousBegin = span.begin;
            if (!pending.TryPushBack(child)) {
                return false;
            }
        }
    }

    return visited == tree.NodeCount();
}

// `fn F() { _ = <식>; }`의 식이다.
uint32 ExpressionNode(const Parsed& parsed) noexcept {
    const SyntaxTree& tree = parsed.tree;
    const uint32 declarations = tree.Child(tree.Root(), 1);
    const uint32 function = tree.Child(declarations, 0);
    const uint32 block = tree.Child(function, 5);
    const uint32 statement = tree.Child(block, 0);
    return tree.Child(statement, 0);
}

// 식 하나를 분석해 S-식과 진단 수를 확인한다.
bool ExpressionIs(StringView expression, StringView expected, usize diagnostics = 0) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        StringBuilder<TextInlineBytes> source(allocator);
        source.Append("fn F() { _ = ").Append(expression).Append("; }");
        Parsed parsed(allocator);
        if (Parse(parsed, source.View()) && ValidTree(parsed)) {
            StringBuilder<TextInlineBytes> out(allocator);
            AppendTree(out, parsed, ExpressionNode(parsed));
            same = out.View() == expected && parsed.diagnostics.Count() == diagnostics;
            if (!same) {
                Print(expression);
                Print(out.View());
                PrintDiagnostics(parsed);
            }
        } else {
            Print(expression);
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

// 원문 전체(File)의 S-식을 확인한다.
bool FileIs(StringView source, StringView expected, usize diagnostics = 0) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        Parsed parsed(allocator);
        if (Parse(parsed, source) && ValidTree(parsed)) {
            StringBuilder<TextInlineBytes> out(allocator);
            AppendTree(out, parsed, parsed.tree.Root());
            same = out.View() == expected && parsed.diagnostics.Count() == diagnostics;
            if (!same) {
                Print(source);
                Print(out.View());
                PrintDiagnostics(parsed);
            }
        } else {
            Print(source);
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

// `fn F() { <문장들> }`의 Block을 확인한다.
bool BlockIs(StringView statements, StringView expected, usize diagnostics = 0) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        StringBuilder<TextInlineBytes> source(allocator);
        source.Append("fn F() { ").Append(statements).Append(" }");
        Parsed parsed(allocator);
        if (Parse(parsed, source.View()) && ValidTree(parsed)) {
            const uint32 declarations = parsed.tree.Child(parsed.tree.Root(), 1);
            const uint32 block = parsed.tree.Child(parsed.tree.Child(declarations, 0), 5);
            // 문장 목록만 `[...]`로 보인다.
            StringBuilder<TextInlineBytes> out(allocator);
            out.Append("[");
            const Span<const uint32> children = parsed.tree.Children(block);
            for (usize index = 0; index < children.Size(); ++index) {
                if (index != 0) {
                    out.Append(" ");
                }

                AppendTree(out, parsed, children[index]);
            }

            out.Append("]");
            same = out.View() == expected && parsed.diagnostics.Count() == diagnostics;
            if (!same) {
                Print(statements);
                Print(out.View());
                PrintDiagnostics(parsed);
            }
        } else {
            Print(statements);
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

struct ExpectedDiagnostic {
    DiagnosticCode code;
    uint32 line;
    uint32 column;
};

// 진단이 순서·코드·위치까지 정확히 이것뿐인지 본다.
bool DiagnosticsAre(const Parsed& parsed, Span<const ExpectedDiagnostic> expected) noexcept {
    bool same = parsed.diagnostics.Count() == expected.Size();
    const SourceFile& file = parsed.sources.Get(parsed.id);
    for (usize index = 0; same && index < expected.Size(); ++index) {
        const Diagnostic& actual = parsed.diagnostics[index];
        const SourceLocation location = file.Locate(actual.Span().begin);
        same = actual.Code() == expected[index].code && location.line == expected[index].line &&
               location.column == expected[index].column;
    }

    if (!same) {
        PrintDiagnostics(parsed);
    }

    return same;
}

// 원문 하나를 분석해 진단을 확인한다(트리 불변식 포함).
bool SourceReports(StringView source, Span<const ExpectedDiagnostic> expected) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        Parsed parsed(allocator);
        if (Parse(parsed, source) && ValidTree(parsed)) {
            same = DiagnosticsAre(parsed, expected);
            if (!same) {
                Print(source);
            }
        } else {
            Print(source);
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

bool SourceReports(StringView source, DiagnosticCode code, uint32 line, uint32 column) noexcept {
    const ExpectedDiagnostic expected[] = {{code, line, column}};
    return SourceReports(source, expected);
}

bool SourceIsClean(StringView source) noexcept {
    return SourceReports(source, Span<const ExpectedDiagnostic>());
}

// 첫 진단의 문장 세 개를 확인한다.
bool FirstDiagnosticSays(StringView source, StringView message, StringView explanation,
                         StringView fix) noexcept {
    SystemAllocator allocator;
    bool same = false;
    {
        Parsed parsed(allocator);
        if (Parse(parsed, source) && !parsed.diagnostics.IsEmpty()) {
            const Diagnostic& first = parsed.diagnostics[0];
            same = first.Message() == message && first.Explanation() == explanation &&
                   first.Fix() == fix;
            if (!same) {
                Print(first.Message());
                Print(first.Explanation());
                Print(first.Fix());
            }
        }
    }

    return same && allocator.Stats().liveAllocations == 0;
}

using C = DiagnosticCode;

} // namespace

// ---- 식: 우선순위와 결합(3.6절 우선순위 문단) ----

TEST(SparksParser_ExpressionPrecedence) {
    CHECK(ExpressionIs("1 + 2 * 3", "(Binary + 1 (Binary * 2 3))"));
    CHECK(ExpressionIs("1 * 2 + 3", "(Binary + (Binary * 1 2) 3)"));
    CHECK(ExpressionIs("1 - 2 - 3", "(Binary - (Binary - 1 2) 3)"));
    CHECK(ExpressionIs("8 / 4 % 3 * 2", "(Binary * (Binary % (Binary / 8 4) 3) 2)"));
    CHECK(ExpressionIs("a || b && c", "(Binary || a (Binary && b c))"));
    CHECK(ExpressionIs("a && b || c && d", "(Binary || (Binary && a b) (Binary && c d))"));
    CHECK(ExpressionIs("a || b || c", "(Binary || (Binary || a b) c)"));
    CHECK(ExpressionIs("a && b && c", "(Binary && (Binary && a b) c)"));
    CHECK(ExpressionIs("a == b && c != d", "(Binary && (Binary == a b) (Binary != c d))"));
    // 비교가 같음보다 먼저 묶인다(결합 없는 두 단계).
    CHECK(ExpressionIs("a < b == c > d", "(Binary == (Binary < a b) (Binary > c d))"));
    CHECK(ExpressionIs("a <= b", "(Binary <= a b)"));
    CHECK(ExpressionIs("a >= b", "(Binary >= a b)"));
    // `??`는 비교보다 높고 덧셈보다 낮으며 오른쪽 결합이다.
    CHECK(ExpressionIs("n?.Count ?? 0 > 5", "(Binary > (Binary ?? (Member:?. Count n -) 0) 5)"));
    CHECK(ExpressionIs("a ?? b ?? c", "(Binary ?? a (Binary ?? b c))"));
    CHECK(ExpressionIs("a ?? b + c", "(Binary ?? a (Binary + b c))"));
    CHECK(ExpressionIs("a + b ?? c", "(Binary ?? (Binary + a b) c)"));
    // 단항은 곱셈보다 높고, `as?`는 단항보다 높다.
    CHECK(ExpressionIs("-a * b", "(Binary * (Unary - a) b)"));
    CHECK(ExpressionIs("!a && b", "(Binary && (Unary ! a) b)"));
    CHECK(ExpressionIs("- -a", "(Unary - (Unary - a))"));
    CHECK(ExpressionIs("!!a", "(Unary ! (Unary ! a))"));
    CHECK(ExpressionIs("-a as? T", "(Unary - (CastTest a T))"));
    CHECK(ExpressionIs("a as? T ?? b", "(Binary ?? (CastTest a T) b)"));
    CHECK(ExpressionIs("a.b as? Weapons.Sword", "(CastTest (Member b a -) Weapons.Sword)"));
    // 후위는 가장 높고 왼쪽부터 이어진다.
    CHECK(ExpressionIs("-a.b(c)[d]", "(Unary - (Index (Call (Member b a -) [c]) d))"));
    CHECK(ExpressionIs("a.b.c", "(Member c (Member b a -) -)"));
    CHECK(ExpressionIs("a?.b?.c()", "(Call (Member:?. c (Member:?. b a -) -) [])"));
    CHECK(ExpressionIs("f()()", "(Call (Call f []) [])"));
    CHECK(ExpressionIs("a[1][2]", "(Index (Index a 1) 2)"));
    CHECK(ExpressionIs("(a + b) * c", "(Binary * (Parenthesized (Binary + a b)) c)"));
    CHECK(ExpressionIs("((a))", "(Parenthesized (Parenthesized a))"));
    CHECK(ExpressionIs("this.health", "(Member health this -)"));
}

TEST(SparksParser_PrimaryExpressions) {
    CHECK(ExpressionIs("1_000", "1_000"));
    CHECK(ExpressionIs("0xFF", "0xFF"));
    CHECK(ExpressionIs(".5", ".5"));
    CHECK(ExpressionIs("1.5e3", "1.5e3"));
    CHECK(ExpressionIs("true", "true"));
    CHECK(ExpressionIs("false", "false"));
    CHECK(ExpressionIs("null", "null"));
    CHECK(ExpressionIs("this", "this"));
    CHECK(ExpressionIs("@\"C:\\x\"", "@\"C:\\x\""));
    CHECK(ExpressionIs("\"\"", "(String)"));
    CHECK(ExpressionIs("\"abc\"", "(String 'abc')"));
    CHECK(ExpressionIs("\"HP ${current}/${max}\"",
                       "(String 'HP ' (Interpolation current) '/' (Interpolation max))"));
    CHECK(ExpressionIs("\"${a + b}\"", "(String (Interpolation (Binary + a b)))"));
    CHECK(ExpressionIs("\"a\\n${x}\"", "(String 'a\\n' (Interpolation x))"));
    CHECK(ExpressionIs("[]", "(CollectionLiteral)"));
    CHECK(ExpressionIs("[1, 2, 3]", "(CollectionLiteral 1 2 3)"));
    CHECK(ExpressionIs("[\"a\": 1, \"b\": 2]",
                       "(CollectionLiteral (String 'a'): 1 (String 'b'): 2)"));
    CHECK(ExpressionIs("[[1], [2: 3]]", "(CollectionLiteral (CollectionLiteral 1) "
                                        "(CollectionLiteral 2: 3))"));
}

TEST(SparksParser_CallsAndArguments) {
    CHECK(ExpressionIs("F()", "(Call F [])"));
    CHECK(ExpressionIs("F(1, 2)", "(Call F [1 2])"));
    // 이름 지정 인자는 `이름 :` 모양이면 받는다(쓸 수 있는 호출인지는 뒤 단계, 3.6절).
    CHECK(ExpressionIs("Spawn(at: p, speed: 2)", "(Call Spawn [at: p speed: 2])"));
    CHECK(ExpressionIs("F(a, b: c)", "(Call F [a b: c])"));
    CHECK(ExpressionIs("F(a.b, (c))", "(Call F [(Member b a -) (Parenthesized c)])"));
}

// 식 안의 `<`는 `Ident` 뒤에서 `Type {, Type} >`가 이어지고 그다음이 `(`·`.`일 때만 타입 인자다.
TEST(SparksParser_TypeArgumentsInExpressions) {
    CHECK(ExpressionIs("Owner.Find<Door>()", "(Call (Member Find Owner [Door]) [])"));
    CHECK(ExpressionIs("Array<int>()", "(Call Array<int> [])"));
    CHECK(ExpressionIs("Map<string, Array<int>>()", "(Call Map<string, Array<int>> [])"));
    CHECK(ExpressionIs("List<A.B?>.Empty", "(Member Empty List<A.B?> -)"));
    CHECK(ExpressionIs("x?.Get<T>()", "(Call (Member:?. Get x [T]) [])"));
    // 뒤가 `(`·`.`가 아니면 비교다.
    CHECK(ExpressionIs("a < b", "(Binary < a b)"));
    CHECK(ExpressionIs("F(a < b, c > d)", "(Call F [(Binary < a b) (Binary > c d)])"));
    // 쉼표로 나뉜 두 비교(`F(a < b, c > (d))`)는 타입 인자로 읽힌다(3.6절: 괄호로 구분한다).
    CHECK(ExpressionIs("F(a < b, c > (d))", "(Call F [(Call a<b, c> [d])])"));
    CHECK(ExpressionIs("F((a < b), c > (d))",
                       "(Call F [(Parenthesized (Binary < a b)) (Binary > c (Parenthesized d))])"));
    // 타입 인자 모양이 아니면(`<` 뒤가 타입이 아니면) 비교다.
    CHECK(ExpressionIs("a < 1", "(Binary < a 1)"));
    // 타입 모양이 `.` 뒤에서 끊기면 타입 인자가 아니다(뒤의 S0207은 멤버 이름이 빠진 것이다).
    CHECK(SourceReports("fn F() { _ = a < b. > (d); }", C::ExpectedName, 1, 21));
    CHECK(ExpressionIs("a < b > c", "(Binary > (Binary < a b) c)", 1));
}

TEST(SparksParser_TryAndAwaitValues) {
    CHECK(BlockIs("let x = try F();", "[(LocalVariable:let x - (Try (Call F [])) -)]"));
    CHECK(BlockIs("let x = try await F();", "[(LocalVariable:let x - (Try:await (Call F [])) -)]"));
    CHECK(BlockIs("let x = await F();", "[(LocalVariable:let x - (Await (Call F [])) -)]"));
    CHECK(BlockIs("try F();", "[(ExpressionStatement (Try (Call F [])))]"));
    CHECK(BlockIs("await F();", "[(ExpressionStatement (Await (Call F [])))]"));
    CHECK(BlockIs("x = await F();", "[(Assignment = x (Await (Call F [])))]"));
    CHECK(BlockIs("_ = try F();", "[(DiscardAssignment (Try (Call F [])))]"));
    CHECK(BlockIs("return try await F();", "[(Return (Try:await (Call F [])))]"));
    // try·await는 Postfix 하나에만 붙는다.
    CHECK(BlockIs("let x = await a.b.c();",
                  "[(LocalVariable:let x - (Await (Call (Member c (Member b a -) -) [])) -)]"));
}

// ---- 문장(3.6절 Stmt) ----

TEST(SparksParser_LocalVariablesAndExit) {
    CHECK(BlockIs("var count = 0;", "[(LocalVariable count - 0 -)]"));
    CHECK(BlockIs("let x: int? = null;", "[(LocalVariable:let x int? null -)]"));
    CHECK(BlockIs("let d = e ?? return;", "[(LocalVariable:let d - e (ExitReturn -))]"));
    CHECK(BlockIs("let d = e ?? return 0;", "[(LocalVariable:let d - e (ExitReturn 0))]"));
    CHECK(BlockIs("let d = a ?? b ?? return;",
                  "[(LocalVariable:let d - (Binary ?? a b) (ExitReturn -))]"));
    CHECK(BlockIs("let d = try F() ?? return;",
                  "[(LocalVariable:let d - (Try (Call F [])) (ExitReturn -))]"));
    CHECK(
        BlockIs("var x: Array<int>= [];", "[(LocalVariable x Array<int> (CollectionLiteral) -)]"));
}

TEST(SparksParser_AssignmentsAndStatements) {
    CHECK(BlockIs("x = 1;", "[(Assignment = x 1)]"));
    CHECK(BlockIs("a.b = 1;", "[(Assignment = (Member b a -) 1)]"));
    CHECK(BlockIs("a[0] = 1;", "[(Assignment = (Index a 0) 1)]"));
    CHECK(BlockIs("x += 1; x -= 2; x *= 3; x /= 4;",
                  "[(Assignment += x 1) (Assignment -= x 2) (Assignment *= x 3) "
                  "(Assignment /= x 4)]"));
    // 경로 안의 호출 결과·`?.`·`this.`는 구문 분석이 받아들이고 뒤 단계가 판정한다(S0731).
    CHECK(BlockIs("F().x = 1;", "[(Assignment = (Member x (Call F []) -) 1)]"));
    CHECK(BlockIs("a?.b = 1;", "[(Assignment = (Member:?. b a -) 1)]"));
    CHECK(BlockIs("this.hp = 1;", "[(Assignment = (Member hp this -) 1)]"));
    CHECK(BlockIs("_ = F();", "[(DiscardAssignment (Call F []))]"));
    CHECK(BlockIs("F();", "[(ExpressionStatement (Call F []))]"));
    CHECK(BlockIs("a + b;", "[(ExpressionStatement (Binary + a b))]"));
    CHECK(BlockIs("break; continue;", "[(Break break) (Continue continue)]"));
    CHECK(BlockIs("return;", "[(Return -)]"));
    CHECK(BlockIs("return a + 1;", "[(Return (Binary + a 1))]"));
    CHECK(BlockIs("emit Opened();", "[(Emit (EventPath Opened) [])]"));
    CHECK(BlockIs("emit this.Hit(1, x);", "[(Emit (EventPath this Hit) [1 x])]"));
    CHECK(BlockIs("emit Owner.Damaged(a);", "[(Emit (EventPath Owner Damaged) [a])]"));
    CHECK(BlockIs("", "[]"));
    // 값으로 시작하는 문장(3.6절 `Value ";"`)은 모든 Primary·단항으로 시작할 수 있다.
    CHECK(
        BlockIs("true; false; null; this; (a); [1]; !a; -a; 1.5; 0x1; \"s\"; @\"v\";",
                "[(ExpressionStatement true) (ExpressionStatement false) (ExpressionStatement "
                "null) (ExpressionStatement this) (ExpressionStatement (Parenthesized a)) "
                "(ExpressionStatement (CollectionLiteral 1)) (ExpressionStatement (Unary ! a)) "
                "(ExpressionStatement (Unary - a)) (ExpressionStatement 1.5) (ExpressionStatement "
                "0x1) (ExpressionStatement (String 's')) (ExpressionStatement @\"v\")]"));
}

TEST(SparksParser_ControlFlow) {
    CHECK(BlockIs("if (a) { }", "[(If a (Block) -)]"));
    CHECK(BlockIs("if (a) { F(); } else { G(); }",
                  "[(If a (Block (ExpressionStatement (Call F []))) "
                  "(Block (ExpressionStatement (Call G []))))]"));
    CHECK(
        BlockIs("if (a) { } else if (b) { } else { }", "[(If a (Block) (If b (Block) (Block)))]"));
    CHECK(BlockIs("if (let d = Find()) { }", "[(If (IfLet d (Call Find [])) (Block) -)]"));
    CHECK(BlockIs("for (i in 0..<n) { }", "[(For i - 0 n (Block))]"));
    CHECK(BlockIs("for (e in enemies) { }", "[(For e - enemies - (Block))]"));
    CHECK(BlockIs("for (k, v in map) { }", "[(For k v map - (Block))]"));
    CHECK(BlockIs("for (_, v in map) { }", "[(For _ v map - (Block))]"));
    CHECK(BlockIs("for (i in a + 1..<b * 2) { }",
                  "[(For i - (Binary + a 1) (Binary * b 2) (Block))]"));
    CHECK(BlockIs("while (running) { Step(); }",
                  "[(While running (Block (ExpressionStatement (Call Step []))))]"));
    CHECK(BlockIs("switch (s) { case A: { } case B, C: { F(); } default: { } }",
                  "[(Switch s [(SwitchCase [A] (Block)) (SwitchCase [B C] (Block "
                  "(ExpressionStatement (Call F []))))] (SwitchDefault (Block)))]"));
    CHECK(BlockIs("switch (s) { }", "[(Switch s [] -)]"));
    CHECK(BlockIs("switch (s) { default: { } }", "[(Switch s [] (SwitchDefault (Block)))]"));
    CHECK(BlockIs("switch (s) { case State.Open: { } }",
                  "[(Switch s [(SwitchCase [(Member Open State -)] (Block))] -)]"));
}

// ---- 선언(3.6절 Decl·Member) ----

TEST(SparksParser_Declarations) {
    CHECK(FileIs("", "(File [] [])"));
    CHECK(FileIs("import Weapons; import Weapons.Sword as Sword;",
                 "(File [(Import Weapons -) (Import Weapons.Sword Sword)] [])"));
    CHECK(FileIs("const MaxStack: int = 99;", "(File [] [(Const - [] MaxStack int 99)])"));
    CHECK(FileIs("class Door : ScriptComponent { }",
                 "(File [] [(Class - [] Door [ScriptComponent] [])])"));
    CHECK(FileIs("open class Base : ScriptComponent, IUsable, Game.ITarget { }",
                 "(File [] [(Class:open - [] Base [ScriptComponent IUsable Game.ITarget] [])])"));
    CHECK(FileIs("struct Hit { var amount: int = 0; fn Twice(): int { return amount * 2; } }",
                 "(File [] [(Struct - [] Hit [(Field - [] amount int 0) (Function - [] Twice [] "
                 "int (Block (Return (Binary * amount 2))))])])"));
    CHECK(FileIs("enum State { Closed, Open, }",
                 "(File [] [(Enum - [] State [(EnumCase - [] Closed) (EnumCase - [] Open)])])"));
    CHECK(FileIs("enum One { Only }", "(File [] [(Enum - [] One [(EnumCase - [] Only)])])"));
    CHECK(FileIs("interface IUsable { fn Use(user: Actor): bool; async fn Wait(); }",
                 "(File [] [(Interface - [] IUsable [(InterfaceMethod - [] Use [(Parameter user "
                 "Actor)] bool) (InterfaceMethod:async - [] Wait [] -)])])"));
    CHECK(FileIs("event Actor.Interacted(by: Actor);",
                 "(File [] [(ExtensionEvent - [] Actor Interacted [(Parameter by Actor)])])"));
    CHECK(FileIs("fn Helper(a: int, _: float): int { return a; }",
                 "(File [] [(Function - [] Helper [(Parameter a int) (Parameter _ float)] int "
                 "(Block (Return a)))])"));
    CHECK(FileIs("async fn Load() { }", "(File [] [(Function:async - [] Load [] - (Block))])"));
}

TEST(SparksParser_ClassMembers) {
    CHECK(FileIs("class A : B { var hp: int = 100; let id = 1; var t: Timer?; }",
                 "(File [] [(Class - [] A [B] [(Field - [] hp int 100) (Field:let - [] id - 1) "
                 "(Field - [] t Timer? -)])])"));
    CHECK(FileIs("class A : B { open fn F() { } override fn G() { } async fn H() { } "
                 "open async fn I() { } override async fn J() { } }",
                 "(File [] [(Class - [] A [B] [(Function:open - [] F [] - (Block)) "
                 "(Function:override - [] G [] - (Block)) (Function:async - [] H [] - (Block)) "
                 "(Function:open:async - [] I [] - (Block)) "
                 "(Function:override:async - [] J [] - (Block))])])"));
    CHECK(FileIs("class A : B { event Opened(by: Actor); on Opened(by: Actor) { } "
                 "on this.Hit(_: int) { } on Owner.Damaged() { } }",
                 "(File [] [(Class - [] A [B] [(Event - [] Opened [(Parameter by Actor)]) "
                 "(Handler - [] (EventPath Opened) [(Parameter by Actor)] (Block)) "
                 "(Handler - [] (EventPath this Hit) [(Parameter _ int)] (Block)) "
                 "(Handler - [] (EventPath Owner Damaged) [] (Block))])])"));
}

TEST(SparksParser_AttributesAndDocComments) {
    CHECK(FileIs("@edit @range(0, 10) class A : B { @save var x = 1; }",
                 "(File [] [(Class - [(Attribute edit []) (Attribute range [0 10])] A [B] "
                 "[(Field - [(Attribute save [])] x - 1)])])"));
    CHECK(FileIs("@formerly(name: \"Old\") const X: int = 1;",
                 "(File [] [(Const - [(Attribute formerly [name: (String 'Old')])] X int 1)])"));
    CHECK(FileIs("enum E { @formerly(\"A\") B }",
                 "(File [] [(Enum - [] E [(EnumCase - [(Attribute formerly [(String 'A')])] "
                 "B)])])"));
    // 문서 주석은 attribute보다 앞에서 선언·멤버·enum case에 붙는다(여러 줄이면 하나로).
    CHECK(FileIs("/// One.\n/// Two.\n@edit class A : B {\n/// Field.\nvar x = 1; }",
                 "(File [] [(Class (DocComment /// One.) [(Attribute edit [])] A [B] "
                 "[(Field (DocComment /// Field.) [] x - 1)])])"));
    CHECK(FileIs("enum E {\n/// First.\nA,\n/// Second.\nB }",
                 "(File [] [(Enum - [] E [(EnumCase (DocComment /// First.) [] A) "
                 "(EnumCase (DocComment /// Second.) [] B)])])"));
    CHECK(FileIs("interface I {\n/// Use it.\nfn Use(); }",
                 "(File [] [(Interface - [] I [(InterfaceMethod (DocComment /// Use it.) [] Use "
                 "[] -)])])"));
}

TEST(SparksParser_Types) {
    CHECK(FileIs("const A: Array<Map<string, int?>>? = null;",
                 "(File [] [(Const - [] A Array<Map<string, int?>>? null)])"));
    CHECK(FileIs("const A: Game.Items.Sword = null;",
                 "(File [] [(Const - [] A Game.Items.Sword null)])"));
    // 타입 인자를 닫는 `>=`는 `>`와 `=`로 나뉜다. 바깥 타입 인자만 닫을 때도 같다.
    CHECK(FileIs("const A: Array<Array<int>>= [];",
                 "(File [] [(Const - [] A Array<Array<int>> (CollectionLiteral))])"));
}

// `>=`를 나누면 토큰이 `=`(한 글자)로 바뀌고 `>`는 타입 인자 List 범위에만 남는다.
TEST(SparksParser_GreaterEqualSplitRewritesTheToken) {
    SystemAllocator allocator;
    {
        Parsed parsed(allocator);
        REQUIRE(Parse(parsed, "const A: Array<int>= [];"));
        CHECK(parsed.diagnostics.IsEmpty());
        CHECK(ValidTree(parsed));
        bool found = false;
        for (usize index = 0; index < parsed.tree.Tokens().Size(); ++index) {
            const Token& token = parsed.tree.Tokens()[index];
            if (token.kind == TokenKind::Equal) {
                found = true;
                CHECK(token.span.begin == 19);
                CHECK(token.span.end == 20);
            }

            CHECK(token.kind != TokenKind::GreaterEqual);
        }

        CHECK(found);
        const uint32 declarations = parsed.tree.Child(parsed.tree.Root(), 1);
        const uint32 type = parsed.tree.Child(parsed.tree.Child(declarations, 0), 3);
        const SyntaxNode& arguments = parsed.tree.Node(parsed.tree.Child(type, 1));
        CHECK(arguments.span.begin == 14);
        CHECK(arguments.span.end == 19);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// ---- 진단: 코드마다 위치(8.1절 S02xx 표) ----

TEST(SparksParser_ExpectedTokenS0201) {
    CHECK(SourceReports("const X: int = 1", C::ExpectedToken, 1, 17));
    CHECK(SourceReports("fn F() { if (x) return; }", C::ExpectedToken, 1, 17));
    CHECK(SourceReports("fn F() { while (x) F(); }", C::ExpectedToken, 1, 20));
    CHECK(SourceReports("fn F() { for (i in 0..<n) F(); }", C::ExpectedToken, 1, 27));
    CHECK(SourceReports("fn F() { if (x) { } else return; }", C::ExpectedToken, 1, 26));
    CHECK(SourceReports("override open fn F() { }", C::ExpectedToken, 1, 10));
    CHECK(SourceReports("async override fn F() { }", C::ExpectedToken, 1, 7));
    CHECK(SourceReports("async open fn F() { }", C::ExpectedToken, 1, 7));
    CHECK(SourceReports("fn F() { _ = x as T; }", C::ExpectedToken, 1, 16));
    CHECK(SourceReports("class A { }", C::ExpectedToken, 1, 9));
    CHECK(SourceReports("fn F() { G(1; }", C::ExpectedToken, 1, 13));
    CHECK(SourceReports("fn F() { var x = 1 y; }", C::ExpectedToken, 1, 20));
    CHECK(SourceReports("fn F() { var x; }", C::ExpectedToken, 1, 15));
    CHECK(SourceReports("fn F() { switch (x) { F(); } }", C::ExpectedToken, 1, 23));
    CHECK(SourceReports("fn F() { switch (x) { case 1 { } } }", C::ExpectedToken, 1, 30));
    CHECK(SourceReports("fn F() { switch (x) { case 1: F(); } }", C::ExpectedToken, 1, 31));
    CHECK(SourceReports("enum E { A B }", C::ExpectedToken, 1, 12));
    CHECK(SourceReports("interface I { fn F() { } }", C::ExpectedToken, 1, 22));
    CHECK(SourceReports("event Interacted();", C::ExpectedToken, 1, 17));
    CHECK(SourceReports("const X = 1;", C::ExpectedToken, 1, 9));
    CHECK(SourceReports("const X: int;", C::ExpectedToken, 1, 13));
    CHECK(SourceReports("fn F(a) { }", C::ExpectedToken, 1, 7));
    CHECK(SourceReports("fn F { }", C::ExpectedToken, 1, 6));
    CHECK(SourceReports("fn F() { a[1; }", C::ExpectedToken, 1, 13));
    CHECK(SourceReports("fn F() { _ = [1, 2; }", C::ExpectedToken, 1, 19));
    CHECK(SourceReports("fn F() { _ = \"${a b}\"; }", C::ExpectedToken, 1, 19));
    CHECK(SourceReports("fn F() { for (i 0..<n) { } }", C::ExpectedToken, 1, 17));
    CHECK(SourceReports("fn F() { emit Opened; }", C::ExpectedToken, 1, 21));
    CHECK(SourceReports("fn F() { break }", C::ExpectedToken, 1, 16));
    CHECK(SourceReports("import A", C::ExpectedToken, 1, 9));
    CHECK(SourceReports("const A: Array<int = 1;", C::ExpectedToken, 1, 20));
    CHECK(SourceReports("fn F() { _ = (1; }", C::ExpectedToken, 1, 16));
}

TEST(SparksParser_ExpectedExpressionS0202) {
    CHECK(SourceReports("fn F() { _ = ; }", C::ExpectedExpression, 1, 14));
    CHECK(SourceReports("fn F() { G(1,); }", C::ExpectedExpression, 1, 14));
    CHECK(SourceReports("fn F() { _ = [1, 2,]; }", C::ExpectedExpression, 1, 20));
    CHECK(SourceReports("fn F() { _ = 1 + ; }", C::ExpectedExpression, 1, 18));
    CHECK(SourceReports("fn F() { _ = -; }", C::ExpectedExpression, 1, 15));
    CHECK(SourceReports("fn F() { return }", C::ExpectedExpression, 1, 17));
    CHECK(SourceReports("fn F() { if () { } }", C::ExpectedExpression, 1, 14));
    CHECK(SourceReports("fn F() { _ = \"${}\"; }", C::ExpectedExpression, 1, 17));
    CHECK(SourceReports("fn F() { G(", C::ExpectedExpression, 1, 12));
}

TEST(SparksParser_ExpectedDeclarationS0203) {
    CHECK(SourceReports("x = 1;", C::ExpectedDeclaration, 1, 1));
    CHECK(SourceReports("class A : B { };", C::ExpectedDeclaration, 1, 16));
    CHECK(SourceReports("}", C::ExpectedDeclaration, 1, 1));
    CHECK(SourceReports("var x = 1;", C::ExpectedDeclaration, 1, 1));
    CHECK(SourceReports("on Opened() { }", C::ExpectedDeclaration, 1, 1));
    CHECK(SourceReports("@edit", C::ExpectedDeclaration, 1, 6));
    CHECK(SourceReports("{ fn F() { } }", C::ExpectedDeclaration, 1, 1));
}

TEST(SparksParser_ExpectedMemberS0204) {
    CHECK(SourceReports("class A : B { if }", C::ExpectedMember, 1, 15));
    CHECK(SourceReports("class A : B { class C : D { } }", C::ExpectedMember, 1, 15));
    CHECK(SourceReports("struct S { event E(); }", C::ExpectedMember, 1, 12));
    CHECK(SourceReports("struct S { on E() { } }", C::ExpectedMember, 1, 12));
    CHECK(SourceReports("interface I { var x = 1; }", C::ExpectedMember, 1, 15));
    CHECK(SourceReports("interface I { override fn F(); }", C::ExpectedMember, 1, 15));
    CHECK(SourceReports("class A : B { 1 }", C::ExpectedMember, 1, 15));
    CHECK(SourceReports("class A : B { @edit }", C::ExpectedMember, 1, 21));
}

TEST(SparksParser_ExpectedStatementS0205) {
    CHECK(SourceReports("fn F() { ; }", C::ExpectedStatement, 1, 10));
    CHECK(SourceReports("fn F() { { } }", C::ExpectedStatement, 1, 10));
    CHECK(SourceReports("fn F() { class A : B { } }", C::ExpectedStatement, 1, 10));
    CHECK(SourceReports("fn F() { fn G() { } }", C::ExpectedStatement, 1, 10));
    CHECK(SourceReports("fn F() { on X() { } }", C::ExpectedStatement, 1, 10));
    CHECK(SourceReports("fn F() { else { } }", C::ExpectedStatement, 1, 10));
    CHECK(SourceReports("fn F() { case 1: }", C::ExpectedStatement, 1, 10));
    CHECK(SourceReports("fn F() { _; }", C::ExpectedStatement, 1, 10));
    CHECK(SourceReports("fn F() { ) }", C::ExpectedStatement, 1, 10));
    // 빈 문장은 건너뛸 것이 없으므로 뒤 문장의 오류도 낸다.
    const ExpectedDiagnostic twoEmpty[] = {{C::ExpectedStatement, 1, 10},
                                           {C::ExpectedStatement, 1, 12}};
    CHECK(SourceReports("fn F() { ; ; }", twoEmpty));
}

TEST(SparksParser_ExpectedTypeS0206) {
    CHECK(SourceReports("class A : B { var x: = 1; }", C::ExpectedType, 1, 22));
    CHECK(SourceReports("fn F(a: 1) { }", C::ExpectedType, 1, 9));
    CHECK(SourceReports("fn F(): { }", C::ExpectedType, 1, 9));
    CHECK(SourceReports("const X: class = 1;", C::ExpectedType, 1, 10));
    CHECK(SourceReports("const X: Array<> = 1;", C::ExpectedType, 1, 16));
    CHECK(SourceReports("fn F() { let x: = 1; }", C::ExpectedType, 1, 17));
    // 같은 줄의 키워드는 이름·타입 자리에 잘못 쓴 것으로 보고 소비한다(그 키워드로 다시 읽지
    // 않는다).
    CHECK(FileIs("const X: class = 1;", "(File [] [(Const - [] X (Error class) 1)])", 1));
    // 다른 줄이면 다음 선언의 시작으로 남긴다.
    CHECK(FileIs("const X:\nclass A : B { }",
                 "(File [] [(Const - [] X (Error) (Error)) (Class - [] A [B] [])])", 1));
}

TEST(SparksParser_ExpectedNameS0207) {
    CHECK(SourceReports("class { }", C::ExpectedName, 1, 7));
    CHECK(SourceReports("class A : B { var class = 1; }", C::ExpectedName, 1, 19));
    CHECK(SourceReports("@ edit const X: int = 1;", C::ExpectedName, 1, 3));
    CHECK(SourceReports("@ const X: int = 1;", C::ExpectedName, 1, 3));
    CHECK(SourceReports("fn F() { _ = x.; }", C::ExpectedName, 1, 16));
    CHECK(SourceReports("fn F() { _ = x?.1; }", C::ExpectedName, 1, 17));
    CHECK(SourceReports("fn F() { var _ = 1; }", C::ExpectedName, 1, 14));
    CHECK(SourceReports("import A.;", C::ExpectedName, 1, 10));
    CHECK(SourceReports("import A as;", C::ExpectedName, 1, 12));
    CHECK(SourceReports("enum E { 1 }", C::ExpectedName, 1, 10));
    CHECK(SourceReports("class A : { }", C::ExpectedName, 1, 11));
    CHECK(SourceReports("fn F() { _ = x as? 1; }", C::ExpectedName, 1, 20));
    CHECK(SourceReports("class A : B { on . () { } }", C::ExpectedName, 1, 18));
    CHECK(SourceReports("fn F() { for (1 in a) { } }", C::ExpectedName, 1, 15));
    CHECK(SourceReports("fn F() { var if = 1; }", C::ExpectedName, 1, 14));
    CHECK(BlockIs("var if = 1;", "[(LocalVariable (Error if) - 1 -)]", 1));
    CHECK(BlockIs("var\nif (a) { }", "[(LocalVariable (Error) - (Error) -) (If a (Block) -)]", 1));
}

TEST(SparksParser_MisplacedDocCommentS0208) {
    CHECK(SourceReports("fn F() {\n/// Not here.\nG(); }", C::MisplacedDocComment, 2, 1));
    CHECK(SourceReports("const X: int = 1;\n/// Dangling.", C::MisplacedDocComment, 2, 1));
    CHECK(SourceReports("@edit\n/// After the attribute.\nconst X: int = 1;",
                        C::MisplacedDocComment, 2, 1));
    CHECK(SourceReports("/// Before an import.\nimport A;", C::MisplacedDocComment, 1, 1));
    CHECK(SourceReports("fn F() { _ = 1 +\n/// Inside.\n2; }", C::MisplacedDocComment, 2, 1));
    CHECK(SourceReports("class A : B {\n/// Before the brace.\n}", C::MisplacedDocComment, 2, 1));
    CHECK(SourceIsClean("/// Doc.\nconst X: int = 1;"));
    // 붙은 자리를 벗어난 여러 줄은 줄마다 낸다.
    const ExpectedDiagnostic twoLines[] = {{C::MisplacedDocComment, 2, 1},
                                           {C::MisplacedDocComment, 3, 1}};
    CHECK(SourceReports("fn F() {\n/// A.\n/// B.\n}", twoLines));
}

TEST(SparksParser_ChainedComparisonS0209) {
    CHECK(SourceReports("fn F() { _ = a < b < c; }", C::ChainedComparison, 1, 20));
    CHECK(SourceReports("fn F() { _ = a == b == c; }", C::ChainedComparison, 1, 21));
    CHECK(SourceReports("fn F() { _ = a != b == c; }", C::ChainedComparison, 1, 21));
    CHECK(SourceReports("fn F() { _ = a <= b >= c; }", C::ChainedComparison, 1, 21));
    // 한 사슬에 한 번만 낸다.
    CHECK(SourceReports("fn F() { _ = a < b < c < d; }", C::ChainedComparison, 1, 20));
    CHECK(ExpressionIs("a < b < c", "(Binary < (Binary < a b) c)", 1));
    CHECK(ExpressionIs("a == b == c", "(Binary == (Binary == a b) c)", 1));
    CHECK(SourceIsClean("fn F() { _ = a < b == c < d; }"));
    CHECK(SourceIsClean("fn F() { _ = (a < b) < c; }"));
}

TEST(SparksParser_ImportAfterDeclarationS0210) {
    CHECK(SourceReports("const X: int = 1;\nimport A;", C::ImportAfterDeclaration, 2, 1));
    // 뒤늦은 import는 트리에 넣지 않는다.
    CHECK(FileIs("import A; const X: int = 1; import B;",
                 "(File [(Import A -)] [(Const - [] X int 1)])", 1));
}

TEST(SparksParser_InvalidAssignmentTargetS0211) {
    CHECK(SourceReports("fn F() { 1 = 2; }", C::InvalidAssignmentTarget, 1, 10));
    CHECK(SourceReports("fn F() { a + b = 1; }", C::InvalidAssignmentTarget, 1, 10));
    CHECK(SourceReports("fn F() { (a) = 1; }", C::InvalidAssignmentTarget, 1, 10));
    CHECK(SourceReports("fn F() { this = x; }", C::InvalidAssignmentTarget, 1, 10));
    CHECK(SourceReports("fn F() { G() = 1; }", C::InvalidAssignmentTarget, 1, 10));
    CHECK(SourceReports("fn F() { -a = 1; }", C::InvalidAssignmentTarget, 1, 10));
    CHECK(SourceReports("fn F() { a as? T += 1; }", C::InvalidAssignmentTarget, 1, 10));
    CHECK(SourceReports("fn F() { \"s\" = 1; }", C::InvalidAssignmentTarget, 1, 10));
    CHECK(SourceReports("fn F() { await G() = 1; }", C::InvalidAssignmentTarget, 1, 10));
    // 대입 대상이 틀려도 오른쪽은 끝까지 읽는다(트리 모양 유지).
    CHECK(BlockIs("1 = 2;", "[(Assignment = 1 2)]", 1));
}

TEST(SparksParser_MisplacedAwaitS0212) {
    CHECK(SourceReports("fn F() { G(await H()); }", C::MisplacedAwait, 1, 12));
    CHECK(SourceReports("fn F() { x = 1 + try G(); }", C::MisplacedAwait, 1, 18));
    CHECK(SourceReports("fn F() { x += await G(); }", C::MisplacedAwait, 1, 15));
    CHECK(SourceReports("fn F() { x -= try G(); }", C::MisplacedAwait, 1, 15));
    CHECK(SourceReports("fn F() { await try G(); }", C::MisplacedAwait, 1, 16));
    CHECK(SourceReports("fn F() { if (await G()) { } }", C::MisplacedAwait, 1, 14));
    CHECK(SourceReports("class A : B { var x = await G(); }", C::MisplacedAwait, 1, 23));
    CHECK(SourceReports("fn F() { return 1 + await G(); }", C::MisplacedAwait, 1, 21));
    CHECK(SourceReports("fn F() { let x = (await G()); }", C::MisplacedAwait, 1, 19));
    CHECK(FirstDiagnosticSays("fn F() { await try G(); }", "'try' comes before 'await'.",
                              "A failing awaited call is written 'try await F()'.",
                              "Write 'try await' in that order."));
}

TEST(SparksParser_MisplacedExitS0213) {
    CHECK(SourceReports("fn F() { G(x ?? return); }", C::MisplacedExit, 1, 14));
    CHECK(SourceReports("fn F() { let x = y ?? break; }", C::MisplacedExit, 1, 20));
    CHECK(SourceReports("fn F() { let x = y ?? continue; }", C::MisplacedExit, 1, 20));
    CHECK(SourceReports("fn F() { _ = y ?? return; }", C::MisplacedExit, 1, 16));
    CHECK(SourceReports("fn F() { return x ?? return; }", C::MisplacedExit, 1, 19));
    CHECK(SourceReports("fn F() { x = y ?? return; }", C::MisplacedExit, 1, 16));
    // `??`와 그 키워드는 오류의 일부로 소비한다(`return`을 새 문장으로 읽지 않는다).
    CHECK(BlockIs("G(x ?? return); H();",
                  "[(ExpressionStatement (Call G [x])) (ExpressionStatement (Call H []))]", 1));
    CHECK(FirstDiagnosticSays("fn F() { let x = y ?? break; }",
                              "'?? break' and '?? continue' do not exist.",
                              "Only '?? return' leaves early when a value is null.",
                              "Test the value with 'if (let x = value) { ... }' instead."));
}

TEST(SparksParser_MisplacedAttributeS0214) {
    CHECK(SourceReports("fn F() { @edit var x = 1; }", C::MisplacedAttribute, 1, 10));
    CHECK(SourceReports("fn F(@edit x: int) { }", C::MisplacedAttribute, 1, 6));
    CHECK(SourceReports("interface I { @edit fn F(); }", C::MisplacedAttribute, 1, 15));
    // 버린 attribute는 트리에 남지 않는다(문장·매개변수). interface 메서드는 남긴다.
    CHECK(BlockIs("@edit @save var x = 1;", "[(LocalVariable x - 1 -)]", 1));
    CHECK(FileIs(
        "interface I { @edit fn F(); }",
        "(File [] [(Interface - [] I [(InterfaceMethod - [(Attribute edit [])] F [] -)])])", 1));
}

TEST(SparksParser_RangeOutsideForS0216) {
    CHECK(SourceReports("fn F() { var r = 0..<10; }", C::RangeOutsideFor, 1, 19));
    CHECK(SourceReports("fn F() { G(0..<10); }", C::RangeOutsideFor, 1, 13));
    CHECK(SourceReports("fn F() { _ = ..<10; }", C::RangeOutsideFor, 1, 14));
    CHECK(SourceReports("fn F() { for (i in 0..<1..<2) { } }", C::RangeOutsideFor, 1, 25));
}

TEST(SparksParser_MisplacedDefaultS0217) {
    CHECK(SourceReports("fn F() { switch (x) { default: { } case 1: { } } }", C::MisplacedDefault,
                        1, 36));
    CHECK(SourceReports("fn F() { switch (x) { default: { } default: { } } }", C::MisplacedDefault,
                        1, 36));
    // case가 여럿 뒤따라도 한 번만 낸다. 두 번째 default는 트리에 넣지 않는다.
    CHECK(SourceReports("fn F() { switch (x) { default: { } case 1: { } case 2: { } } }",
                        C::MisplacedDefault, 1, 36));
    CHECK(BlockIs("switch (x) { default: { F(); } default: { G(); } }",
                  "[(Switch x [] (SwitchDefault (Block (ExpressionStatement (Call F [])))))]", 1));
    CHECK(BlockIs("switch (x) { default: { } case 1: { } }",
                  "[(Switch x [(SwitchCase [1] (Block))] (SwitchDefault (Block)))]", 1));
}

TEST(SparksParser_MixedCollectionLiteralS0218) {
    CHECK(SourceReports("fn F() { _ = [1, \"a\": 2]; }", C::MixedCollectionLiteral, 1, 18));
    CHECK(SourceReports("fn F() { _ = [\"a\": 1, 2]; }", C::MixedCollectionLiteral, 1, 23));
    CHECK(SourceReports("fn F() { _ = [1, 2: 3, 4, 5: 6]; }", C::MixedCollectionLiteral, 1, 18));
}

TEST(SparksParser_EmptyEnumS0219) {
    CHECK(SourceReports("enum State { }", C::EmptyEnum, 1, 6));
    CHECK(SourceReports("enum State {}", C::EmptyEnum, 1, 6));
    CHECK(FileIs("enum State { }", "(File [] [(Enum - [] State [])])", 1));
}

// 진단 문장은 고정된 영어다(형식 8.1절). 대표 문장을 그대로 확인한다.
TEST(SparksParser_DiagnosticTexts) {
    CHECK(FirstDiagnosticSays("const X: int = 1",
                              "Expected ';' after the constant, found end of file.", "",
                              "Insert ';'."));
    CHECK(FirstDiagnosticSays("fn F() { if (x) return; }",
                              "Expected '{' to start the body of 'if', found 'return'.", "",
                              "Wrap the body in braces, as in 'if (ready) { Open(); }'."));
    CHECK(FirstDiagnosticSays("fn F() { var x = 1 y; }",
                              "Expected ';' after the local variable, found identifier 'y'.", "",
                              "Insert ';'."));
    CHECK(FirstDiagnosticSays(
        "override open fn F() { }", "Expected 'fn' after the function modifiers, found 'open'.", "",
        "Write the modifiers in this order: 'open' or 'override', then 'async', then 'fn'."));
    CHECK(FirstDiagnosticSays("fn F() { _ = x as T; }", "Use 'as?' for a downcast.",
                              "Sparks has only the checked downcast 'as?', which gives null when "
                              "the object is not of that type.",
                              "Write 'x as? T'."));
    CHECK(FirstDiagnosticSays("x = 1;", "Expected a declaration, found identifier 'x'.",
                              "Top-level code is made of declarations; statements go inside "
                              "functions.",
                              "Start a declaration with class, struct, enum, interface, event, "
                              "const or fn."));
    CHECK(FirstDiagnosticSays("class A : B { var class = 1; }",
                              "Expected a name for the field, found 'class'.",
                              "Keywords cannot be used as names.",
                              "Write a name made of English letters and digits, as in "
                              "'doorSpeed'."));
    CHECK(FirstDiagnosticSays("fn F() { var _ = 1; }",
                              "Expected a name for the local variable, found '_'.",
                              "A lone '_' discards a value and is not a name.",
                              "Write a name made of English letters and digits, as in "
                              "'doorSpeed'."));
    CHECK(FirstDiagnosticSays("fn F() { ; }", "Empty statement.", "A ';' on its own does nothing.",
                              "Remove the extra ';'."));
    CHECK(FirstDiagnosticSays("fn F() {\n/// X.\n}",
                              "Documentation comment is not attached to a declaration.",
                              "'///' documents the declaration, member or enum case right after "
                              "it and goes before its attributes.",
                              "Use '//' for an ordinary comment."));
    CHECK(FirstDiagnosticSays("fn F() { _ = a < b < c; }", "Comparisons cannot be chained.",
                              "Comparison and equality operators take exactly two operands; 'a < "
                              "b < c' would compare a bool with c.",
                              "Combine two comparisons with '&&', as in 'a < b && b < c'."));
    CHECK(FirstDiagnosticSays("enum E { }", "An enum needs at least one case.",
                              "The zero value of an enum is its first case.",
                              "Add a case, as in 'enum DoorState { Closed, Open }'."));
    CHECK(FirstDiagnosticSays("fn F() { var r = 0..<1; }",
                              "A range is only allowed in a 'for' header.",
                              "'a..<b' names the values a for loop visits; it is not a value.",
                              "Write 'for (i in a..<b) { ... }'."));
    // 긴 식별자는 앞 48 byte 뒤에 "..."를 붙인다.
    CHECK(FirstDiagnosticSays("fn F() { var x = 1 "
                              "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijXYZ; }",
                              "Expected ';' after the local variable, found identifier "
                              "'abcdefghijabcdefghijabcdefghijabcdefghijabcdefgh...'.",
                              "", "Insert ';'."));
}

// ---- 복구(8.1절 "복구") ----

TEST(SparksParser_RecoveryReportsOneDiagnosticPerCause) {
    // 문장 단위: 오류 뒤 `;`까지 건너뛰고 다음 문장을 읽는다.
    const ExpectedDiagnostic statements[] = {{C::ExpectedExpression, 1, 14},
                                             {C::ExpectedExpression, 1, 25}};
    CHECK(SourceReports("fn F() { x = ) a b; y = ; }", statements));
    // 문장 키워드에서 멈춘다(`;`를 잃어도 다음 문장을 읽는다).
    const ExpectedDiagnostic keyword[] = {{C::ExpectedToken, 1, 20},
                                          {C::ExpectedExpression, 1, 28}};
    CHECK(SourceReports("fn F() { var x = 1 var y = ; }", keyword));
    // 문장이 `;`로 끝났으면 건너뛰지 않는다.
    const ExpectedDiagnostic finished[] = {{C::ExpectedExpression, 1, 18},
                                           {C::ExpectedExpression, 1, 28}};
    CHECK(SourceReports("fn F() { var x = ; var y = ; }", finished));
    // 멤버 단위.
    const ExpectedDiagnostic members[] = {{C::ExpectedType, 1, 22}, {C::ExpectedExpression, 1, 35}};
    CHECK(SourceReports("class A : B { var x: = 1; var y = ; }", members));
    // 선언 단위: 다음 선언 키워드까지.
    const ExpectedDiagnostic declarations[] = {{C::ExpectedDeclaration, 1, 1},
                                               {C::ExpectedDeclaration, 1, 30}};
    CHECK(SourceReports("garbage here class A : B { } junk struct S { }", declarations));
    // 블록은 짝을 맞춰 통째로 건너뛴다.
    CHECK(SourceReports("fn F() { x = ) { y = ; } ; }", C::ExpectedExpression, 1, 14));
    // 빠진 `)` 뒤의 본문은 그대로 읽는다(본문을 여는 `{`가 동기화 지점이므로 본문 안의 오류도
    // 낸다).
    const ExpectedDiagnostic body[] = {{C::ExpectedToken, 1, 16}, {C::ExpectedExpression, 1, 22}};
    CHECK(SourceReports("fn F() { if (x { y = ; } }", body));
    // 수식어 순서 오류 뒤 다음 멤버를 그대로 읽는다.
    CHECK(FileIs("override open fn F() { }",
                 "(File [] [(Function:override - [] (Error) [] - (Error)) (Function:open - [] F "
                 "[] - (Block))])",
                 1));
}

// 파일 끝에 닿은 복구는 빠진 `}`·`)`를 다시 알리지 않는다.
TEST(SparksParser_RecoveryAtEndOfFileStaysQuiet) {
    CHECK(SourceReports("fn F() { G(", C::ExpectedExpression, 1, 12));
    CHECK(SourceReports("class A : B { fn F() { x = 1 +", C::ExpectedExpression, 1, 31));
    // 오류 없이 끝에 닿으면 빠진 `}`를 알린다. 여럿이 빠져도 하나다.
    CHECK(SourceReports("fn F() { G();", C::ExpectedToken, 1, 14));
    CHECK(SourceReports("class A : B {", C::ExpectedToken, 1, 14));
    CHECK(
        SourceReports("class A : B { fn F() { while (a) { if (b) { G();", C::ExpectedToken, 1, 49));
    CHECK(SourceReports("fn F() { switch (s) { case 1: { G();", C::ExpectedToken, 1, 37));
    // switch 본문의 잡토큰 뒤 파일 끝도 진단 하나다.
    CHECK(SourceReports("fn F() { switch (s) { junk", C::ExpectedToken, 1, 23));
}

// Invalid 토큰(어휘 오류 자리)에서는 구문 오류를 더 내지 않는다.
TEST(SparksParser_InvalidTokensDoNotCascade) {
    CHECK(SourceReports("fn F() { x = 1 # 2; y = 3; }", C::InvalidCharacter, 1, 16));
    CHECK(SourceReports("fn F() { # }", C::InvalidCharacter, 1, 10));
    CHECK(SourceReports("# const X: int = 1;", C::InvalidCharacter, 1, 1));
    CHECK(SourceReports("fn F() { G(\"abc); }\nfn H() { }", C::UnterminatedString, 1, 12));
    CHECK(SourceReports("fn F() { _ = \"${a\n}", C::UnterminatedString, 1, 14));
    CHECK(SourceReports("fn F() { _ = @\"open", C::UnterminatedString, 1, 14));
    CHECK(SourceReports("fn F() { /* open", C::UnterminatedBlockComment, 1, 10));
    // 시작 위치에 어휘 진단이 있는 토큰에서도 구문 오류를 내지 않는다(보간 안의 문자열, `_`가 든
    // 식별자). 보간을 닫지 못한 자리는 안쪽 문자열을 짝 맞춰 건너뛴다.
    CHECK(SourceReports("fn F() { _ = \"${a \"x\" b}\"; G(); }", C::StringInInterpolation, 1, 19));
    CHECK(SourceReports("move_speed = 1;", C::UnderscoreInIdentifier, 1, 1));
    // 어휘 오류 뒤의 다른 구문 오류는 그대로 낸다.
    const ExpectedDiagnostic later[] = {{C::InvalidCharacter, 1, 16},
                                        {C::ExpectedExpression, 1, 25}};
    CHECK(SourceReports("fn F() { x = 1 # 2; y = ; }", later));
}

// ---- 중첩·깊이 상한(MaxSyntaxNesting, MaxSyntaxTreeDepth) ----

namespace {

// 반복 조각으로 원문을 만든다.
String Repeat(Allocator& allocator, StringView prefix, StringView open, usize count,
              StringView middle, StringView close, StringView suffix) noexcept {
    StringBuilder<TextInlineBytes> out(allocator);
    out.Append(prefix);
    for (usize index = 0; index < count; ++index) {
        out.Append(open);
    }

    out.Append(middle);
    for (usize index = 0; index < count; ++index) {
        out.Append(close);
    }

    out.Append(suffix);
    String result(allocator);
    static_cast<void>(result.TryAssign(out.View()));
    return result;
}

// count 단계 원문이 깨끗하고 count + 1 단계는 S0215 하나(그 위치)인지 본다.
bool LimitIs(StringView prefix, StringView open, usize count, StringView middle, StringView close,
             StringView suffix, uint32 failColumn) noexcept {
    SystemAllocator allocator;
    bool good = false;
    {
        const String fits = Repeat(allocator, prefix, open, count, middle, close, suffix);
        const String over = Repeat(allocator, prefix, open, count + 1, middle, close, suffix);
        good = SourceIsClean(fits.View()) &&
               SourceReports(over.View(), C::NestingTooDeep, 1, failColumn);
    }

    return good && allocator.Stats().liveAllocations == 0;
}

} // namespace

// 단계 수: 문장 1, 식 1(괄호·대괄호·인자·보간·인덱스 안의 식마다 새로 1), 단항 연산자 1, `??` 1,
// try·await 1, 타입 1(타입 인자마다 새로 1), `else if` 1. 128단계까지 받는다.
TEST(SparksParser_NestingLimit) {
    constexpr StringView Prefix = "fn F() { _ = ";
    constexpr uint32 PrefixColumns = 13;
    // 문장 1 + 식 1 + 괄호 n.
    CHECK(LimitIs(Prefix, "(", MaxSyntaxNesting - 2, "1", ")", "; }",
                  PrefixColumns + (MaxSyntaxNesting - 1) + 1));
    // 대괄호(컬렉션) n.
    CHECK(LimitIs(Prefix, "[", MaxSyntaxNesting - 2, "1", "]", "; }",
                  PrefixColumns + (MaxSyntaxNesting - 1) + 1));
    // 단항 n: 넘는 자리는 그 단항 연산자다.
    CHECK(LimitIs(Prefix, "!", MaxSyntaxNesting - 2, "x", "", "; }",
                  PrefixColumns + (MaxSyntaxNesting - 1)));
    // 호출 인자 n.
    CHECK(LimitIs(Prefix, "f(", MaxSyntaxNesting - 2, "1", ")", "; }",
                  PrefixColumns + 2 * (MaxSyntaxNesting - 1) + 1));
    // `??` n: 넘는 자리는 그 `??`다.
    CHECK(LimitIs(Prefix, "a ?? ", MaxSyntaxNesting - 2, "a", "", "; }",
                  PrefixColumns + 5 * (MaxSyntaxNesting - 2) + 3));
    // 블록: if 문 n개가 중첩되면 가장 안쪽 조건식이 n + 1단계다.
    CHECK(LimitIs("fn F() { ", "if (a) { ", MaxSyntaxNesting - 1, "", "} ", "}",
                  10 + 9 * (MaxSyntaxNesting - 1) + 4));
    // `else if` 사슬도 한 단계씩이다(가장 안쪽 조건식이 n + 2단계).
    CHECK(LimitIs("fn F() { if (a) { }", " else if (a) { }", MaxSyntaxNesting - 2, "", "", " }",
                  19 + 16 * (MaxSyntaxNesting - 2) + 11));
    // try·await 한 단계: 가장 안쪽 if 블록의 `await x;`는 문장 n + 1단계, await n + 2단계다.
    CHECK(LimitIs("fn F() { ", "if (a) { ", MaxSyntaxNesting - 2, "await x; ", "} ", "}",
                  10 + 9 * (MaxSyntaxNesting - 1)));
    // 타입: 필드 타입 n단계(바깥 타입이 1단계).
    CHECK(LimitIs("class A : B { var x: ", "A<", MaxSyntaxNesting - 1, "int", ">", "; }",
                  22 + 2 * MaxSyntaxNesting));
}

// 식 안의 타입 인자 판정은 `<`가 128단계 넘게 중첩되면 타입 인자로 본다. 그 분석은 깊이 상한에
// 걸리므로 S0215 하나다(비교 사슬 S0209 따위로 바뀌지 않는다).
TEST(SparksParser_DeepTypeArgumentsInExpressionsHitTheNestingLimit) {
    SystemAllocator allocator;
    {
        // 문장 1 + 식 1 + 타입 k: k번째 A가 129단계다.
        const String deep =
            Repeat(allocator, "fn F() { _ = F<", "A<", MaxSyntaxNesting, "int", ">", "> (); }");
        CHECK(SourceReports(deep.View(), C::NestingTooDeep, 1, 16 + 2 * (MaxSyntaxNesting - 2)));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 중첩 상한을 넘으면 그 파일의 나머지는 분석하지 않는다(뒤의 오류를 내지 않는다).
TEST(SparksParser_NestingLimitStopsTheFile) {
    SystemAllocator allocator;
    {
        const String deep =
            Repeat(allocator, "fn F() { _ = ", "(", MaxSyntaxNesting, "1", ")", "; }\nx y z\n");
        CHECK(SourceReports(deep.View(), C::NestingTooDeep, 1, 13 + MaxSyntaxNesting));
        // 복구 중에도 S0215는 낸다.
        const String afterError =
            Repeat(allocator, "fn F() { _ = ) ; _ = ", "(", MaxSyntaxNesting, "1", ")", "; }");
        const ExpectedDiagnostic both[] = {{C::ExpectedExpression, 1, 14},
                                           {C::NestingTooDeep, 1, 21 + MaxSyntaxNesting}};
        CHECK(SourceReports(afterError.View(), both));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 트리 깊이: File·List·Function·Block·DiscardAssignment 5단계 아래에 연산자 k개와 리터럴 하나면
// k + 6단계다. 넘으면 끝까지 분석한 뒤 가장 깊은 경로의 상한 + 1단계 노드(가장 왼쪽 피연산자)에
// 낸다.
TEST(SparksParser_TreeDepthLimit) {
    SystemAllocator allocator;
    {
        constexpr usize Fits = MaxSyntaxTreeDepth - 6;
        const String sum = Repeat(allocator, "fn F() { _ = 1", "+1", Fits, "", "", "; }");
        CHECK(SourceIsClean(sum.View()));
        const String tooDeep = Repeat(allocator, "fn F() { _ = 1", "+1", Fits + 1, "", "", "; }");
        CHECK(SourceReports(tooDeep.View(), C::NestingTooDeep, 1, 14));
        const String members = Repeat(allocator, "fn F() { _ = a", ".b", Fits + 1, "", "", "; }");
        CHECK(SourceReports(members.View(), C::NestingTooDeep, 1, 14));
        // 분석은 끝까지 하므로 뒤의 오류도 낸다.
        const String after =
            Repeat(allocator, "fn F() { _ = 1", "+1", Fits + 1, "", "", "; }\nconst X = 1;");
        const ExpectedDiagnostic both[] = {{C::NestingTooDeep, 1, 14}, {C::ExpectedToken, 2, 9}};
        CHECK(SourceReports(after.View(), both));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 연산자 사슬이 uint16 범위(65535단계)를 넘어도 깊이를 정확히 세어 S0215 하나를 낸다.
TEST(SparksParser_VeryLongOperatorChains) {
    SystemAllocator allocator;
    {
        constexpr usize Terms = 65540;
        const String sum = Repeat(allocator, "fn F() { _ = 1", "+1", Terms, "", "", "; }");
        CHECK(SourceReports(sum.View(), C::NestingTooDeep, 1, 14));
        const String members = Repeat(allocator, "fn F() { _ = a", ".a", Terms, "", "", "; }");
        CHECK(SourceReports(members.View(), C::NestingTooDeep, 1, 14));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 리뷰에서 찾은 "원인 하나에 진단 둘" 사례와 규칙 경계(8.1절 "복구").
TEST(SparksParser_OneDiagnosticPerCauseEdgeCases) {
    // 멤버 이름 자리의 같은 줄 키워드는 소비한다. 다른 줄이면 남긴다.
    CHECK(SourceReports("fn F() { _ = x.if; }", C::ExpectedName, 1, 16));
    CHECK(SourceReports("fn F() { x.return = 1; }", C::ExpectedName, 1, 12));
    CHECK(BlockIs("_ = x.if;", "[(DiscardAssignment (Member x -))]", 1));
    CHECK(BlockIs("_ = x.\nif (a) { }", "[(DiscardAssignment (Member x -)) (If a (Block) -)]", 1));
    // attribute 이름 자리의 키워드는 소비하지 않는다(선언 키워드일 수 있다).
    CHECK(FileIs("@class A : B { }", "(File [] [(Class - [(Attribute [])] A [B] [])])", 1));
    // 어휘 진단이 있는 `..` 토큰에서는 S0216을 더 내지 않는다.
    CHECK(SourceReports("fn F() { G(0..10); }", C::RangeOperator, 1, 13));
    // try·await 값 뒤의 이항 연산자·`as?`는 S0212(그 키워드 위치) 하나다.
    CHECK(SourceReports("fn F() { let x = await G() + 1; }", C::MisplacedAwait, 1, 18));
    CHECK(SourceReports("fn F() { _ = try G() ?? 0; }", C::MisplacedAwait, 1, 14));
    CHECK(SourceReports("fn F() { x = await G() as? T; }", C::MisplacedAwait, 1, 14));
    CHECK(SourceReports("fn F() { return try await G() == 1; }", C::MisplacedAwait, 1, 17));
    CHECK(SourceIsClean("fn F() { let x = try G() ?? return; }"));
    // 식 안에 잘못 놓인 await는 S0212 하나이고 뒤 연산은 이어 읽는다.
    CHECK(BlockIs("F(await G() + 1);",
                  "[(ExpressionStatement (Call F [(Binary + (Await (Call G [])) 1)]))]", 1));
    // 버린 attribute 뒤에 시작할 것이 없으면 S0214 하나다.
    CHECK(SourceReports("fn F() { @edit }", C::MisplacedAttribute, 1, 10));
    CHECK(SourceReports("fn F(@a) { }", C::MisplacedAttribute, 1, 6));
    // 오류로 건너뛴 최상위 `}`·`;`는 동기화 지점이 아니다.
    CHECK(SourceReports("} x", C::ExpectedDeclaration, 1, 1));
    CHECK(SourceReports("x }", C::ExpectedDeclaration, 1, 1));
    // 첫 선언 전의 잡토큰은 import 단계에 남고, 뒤의 import는 S0210이 아니다.
    CHECK(FileIs("import A; junk import B;", "(File [(Import A -) (Error junk) (Import B -)] [])",
                 1));
    CHECK(SourceReports("@a import X;", C::ExpectedDeclaration, 1, 4));
    // 머리 오류로 남은 토큰이 있으면 본문은 문장 동기화로 통째로 건너뛴다.
    CHECK(SourceReports("fn F() { while (a b) { x = ; } }", C::ExpectedToken, 1, 19));
    // 이름이 빠진 enum에는 S0219를 내지 않는다. case 자리가 있으면(이름이 빠져도) S0219가 아니다.
    CHECK(SourceReports("enum { }", C::ExpectedName, 1, 6));
    CHECK(SourceReports("enum E { , }", C::ExpectedName, 1, 10));
    // 동기화가 오류 난 토큰에서 멈춰도 그 위치에는 구문 진단 하나다.
    CHECK(SourceReports("interface\nimport A;", C::ExpectedName, 2, 1));
    CHECK(SourceReports("interface I { fn Use(): bool override ; }", C::ExpectedToken, 1, 30));
    // `?? return`이 괄호 안에 있으면 S0213 하나다.
    CHECK(SourceReports("fn F() { let x = (a ?? return); }", C::MisplacedExit, 1, 21));
}

// 문서 주석: 연속한 `///` 줄은 빈 줄·일반 주석이 끼어도 하나다. 코드 뒤 같은 줄의 `///`는
// S0208이다.
TEST(SparksParser_DocCommentPlacement) {
    CHECK(FileIs("/// A.\n\n/// B.\nconst X: int = 1;",
                 "(File [] [(Const (DocComment /// A.) [] X int 1)])"));
    CHECK(FileIs("/// A.\n// note\n/// B.\nconst X: int = 1;",
                 "(File [] [(Const (DocComment /// A.) [] X int 1)])"));
    CHECK(SourceReports("class A : B {\n    var x = 1; /// x\n    var y = 2;\n}",
                        C::MisplacedDocComment, 2, 16));
    CHECK(FileIs("class A : B {\n    var x = 1; /// x\n    var y = 2;\n}",
                 "(File [] [(Class - [] A [B] [(Field - [] x - 1) (Field - [] y - 2)])])", 1));
    // 코드 뒤 문서 주석 다음 줄에 이어진 문서 주석은 붙는다.
    CHECK(FileIs(
        "enum E { A, /// trailing\n/// B.\nB }",
        "(File [] [(Enum - [] E [(EnumCase - [] A) (EnumCase (DocComment /// B.) [] B)])])", 1));
}

// 목록 끝 쉼표는 enum case 목록만 받는다. 기반 목록·`as?` 대상에는 타입 인자·`?`가 없다.
TEST(SparksParser_TrailingCommasAndPlainTypeNames) {
    CHECK(SourceReports("fn F(a: int,) { }", C::ExpectedName, 1, 13));
    CHECK(SourceReports("fn F() { switch (x) { case 1, : { } } }", C::ExpectedExpression, 1, 31));
    CHECK(SourceReports("fn F() { emit E(1,); }", C::ExpectedExpression, 1, 19));
    CHECK(SourceReports("@a(1,) const X: int = 1;", C::ExpectedExpression, 1, 6));
    CHECK(SourceReports("class A : B, { }", C::ExpectedName, 1, 14));
    CHECK(SourceReports("class A : B<int> { }", C::ExpectedToken, 1, 12));
    CHECK(SourceReports("class A : B? { }", C::ExpectedToken, 1, 12));
    // 진단 뒤에도 기반 목록과 본문은 그대로 읽는다.
    CHECK(FileIs("class A : B<int>, C { var x = 1; }",
                 "(File [] [(Class - [] A [B C] [(Field - [] x - 1)])])", 1));
    CHECK(SourceReports("fn F() { _ = x as? A as? B; }", C::ExpectedToken, 1, 22));
    CHECK(SourceReports("fn F() { _ = x as? List<int>; }", C::ExpectedToken, 1, 24));
    CHECK(SourceReports("fn F() { _ = x as? T?; }", C::ExpectedToken, 1, 21));
    CHECK(ExpressionIs("x as? List<int>", "(CastTest x List)", 1));
}

// ---- 덤프 형식(SD58, SyntaxTree.h AppendSyntaxDump) ----

TEST(SparksParser_DumpFormat) {
    SystemAllocator allocator;
    {
        Parsed parsed(allocator);
        REQUIRE(Parse(parsed,
                      "/// Doc\n/// More\n@edit open class A : B {\n"
                      "  override async fn F(t: Tab?) { let s = \"a\\\"\\\\b\\t\" ?? return; }\n"
                      "}\n"));
        CHECK(parsed.diagnostics.IsEmpty());
        StringBuilder<TextInlineBytes> dump(allocator);
        REQUIRE(AppendSyntaxDump(dump, parsed.sources, parsed.tree));
        constexpr StringView Expected =
            StringView("File @1:1-6:1\n"
                       "  List @1:1-1:1\n"
                       "  List @1:1-5:2\n"
                       "    Class open @1:1-5:2\n"
                       "      DocComment \"/// Doc\" lines=2 @1:1-2:9\n"
                       "      List @3:1-3:6\n"
                       "        Attribute \"edit\" @3:1-3:6\n"
                       "          List @3:6-3:6\n"
                       "      Name \"A\" @3:18-3:19\n"
                       "      List @3:22-3:23\n"
                       "        Path @3:22-3:23\n"
                       "          Name \"B\" @3:22-3:23\n"
                       "      List @3:24-5:2\n"
                       "        Function override async @4:3-4:65\n"
                       "          -\n"
                       "          List @4:3-4:3\n"
                       "          Name \"F\" @4:21-4:22\n"
                       "          List @4:22-4:31\n"
                       "            Parameter @4:23-4:30\n"
                       "              Name \"t\" @4:23-4:24\n"
                       "              Type optional @4:26-4:30\n"
                       "                Path @4:26-4:29\n"
                       "                  Name \"Tab\" @4:26-4:29\n"
                       "                -\n"
                       "          -\n"
                       "          Block @4:32-4:65\n"
                       "            LocalVariable let @4:34-4:63\n"
                       "              Name \"s\" @4:38-4:39\n"
                       "              -\n"
                       "              String @4:42-4:52\n"
                       "                StringText \"a\\\\\\\"\\\\\\\\b\\\\t\" @4:43-4:51\n"
                       "              ExitReturn @4:53-4:62\n"
                       "                -\n");
        CHECK(dump.View() == Expected);
        if (dump.View() != Expected) {
            Print(dump.View());
        }

        // 분석하지 않은 트리는 덤프할 수 없다.
        SyntaxTree empty(allocator);
        StringBuilder<TextInlineBytes> none(allocator);
        auto failed = AppendSyntaxDump(none, parsed.sources, empty);
        CHECK(!failed && failed.Error() == ErrorCode::InvalidArgument);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 토큰 글자의 제어 문자와 tab·CR·LF는 escape한다(verbatim 문자열은 여러 줄일 수 있다).
TEST(SparksParser_DumpEscapesTokenText) {
    SystemAllocator allocator;
    {
        Parsed parsed(allocator);
        REQUIRE(Parse(parsed, "const S: string = @\"a\tb\r\nc\x01\x7F\";"));
        StringBuilder<TextInlineBytes> dump(allocator);
        REQUIRE(AppendSyntaxDump(dump, parsed.sources, parsed.tree));
        CHECK(dump.View().Contains(
            "VerbatimString \"@\\\"a\\tb\\r\\nc\\u{01}\\u{7F}\\\"\" @1:19-2:5\n"));
        if (!dump.View().Contains("VerbatimString")) {
            Print(dump.View());
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 같은 원문은 같은 트리·덤프·진단이다. 이전 내용이 있는 트리를 다시 써도 같다.
TEST(SparksParser_IsDeterministicAndReusesTrees) {
    SystemAllocator allocator;
    {
        constexpr StringView Source = "class A : B { var x = ; fn F() { G(1, ; } }";
        Parsed first(allocator);
        REQUIRE(Parse(first, Source));
        StringBuilder<TextInlineBytes> firstDump(allocator);
        REQUIRE(AppendSyntaxDump(firstDump, first.sources, first.tree));

        Parsed second(allocator);
        REQUIRE(Parse(second, "const Y: int = 2;"));
        auto added = second.sources.TryAdd("Again.spark", Source, second.diagnostics);
        REQUIRE(added);
        DiagnosticList again(allocator);
        REQUIRE(ParseFile(second.sources, added.Value(), again, second.tree));
        CHECK(second.tree.Source().index == added.Value().index);
        StringBuilder<TextInlineBytes> secondDump(allocator);
        REQUIRE(AppendSyntaxDump(secondDump, second.sources, second.tree));
        CHECK(firstDump.View() == secondDump.View());
        REQUIRE(again.Count() == first.diagnostics.Count());
        for (usize index = 0; index < again.Count(); ++index) {
            CHECK(again[index].Code() == first.diagnostics[index].Code());
            CHECK(again[index].Span().begin == first.diagnostics[index].Span().begin);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 원문 검사에서 막힌 파일은 분석하지 않는다(InvalidArgument).
TEST(SparksParser_RejectsUnusableSources) {
    SystemAllocator allocator;
    {
        SourceSet sources(allocator);
        DiagnosticList diagnostics(allocator);
        auto added = sources.TryAdd("Bad.spark", "\xFF", diagnostics);
        REQUIRE(added);
        SyntaxTree tree(allocator);
        auto parsed = ParseFile(sources, added.Value(), diagnostics, tree);
        CHECK(!parsed && parsed.Error() == ErrorCode::InvalidArgument);
        CHECK(tree.Root() == NoNode);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 할당 실패는 그 오류로 끝나고 새지 않는다. 실패 지점을 하나씩 옮겨 모두 시험한다.
TEST(SparksParser_AllocationFailureEverywhere) {
    SystemAllocator backing;
    constexpr StringView Source =
        "import A;\n/// Doc\n@edit class D : B { var x: Array<int> = [1, 2]; "
        "on Opened(by: Actor) { if (let a = by.Find<Door>()) { emit Hit(1); } "
        "let s = \"v ${x[0]}\"; x = ; } }\n";
    bool succeeded = false;
    uint64 failures = 0;
    for (uint64 failAt = 1; !succeeded && failAt < 100000; ++failAt) {
        test::NFailAllocator allocator(backing);
        {
            SourceSet sources(allocator);
            DiagnosticList diagnostics(allocator);
            allocator.DisableFailure();
            auto added = sources.TryAdd("Fail.spark", Source, diagnostics);
            REQUIRE(added);
            SyntaxTree tree(allocator);
            allocator.FailOnAttempt(failAt);
            auto parsed = ParseFile(sources, added.Value(), diagnostics, tree);
            if (parsed) {
                succeeded = true;
                CHECK(diagnostics.Count() == 1);
            } else {
                ++failures;
                CHECK(parsed.Error() == ErrorCode::OutOfMemory);
            }
        }
        CHECK(backing.Stats().liveAllocations == 0);
    }

    CHECK(succeeded);
    // 토큰·노드·자식·작업 stack·깊이 배열·진단 문장이 각각 여러 번 자라므로 실패 지점이 여럿이다.
    CHECK(failures >= 10);
}

// ---- 결정적 무작위 입력(고정 seed): 멈춤·충돌 없이 불변식을 지키고 결정적이다 ----

namespace {

constexpr StringView RandomPieces[] = {
    "import",   "as",   "class",    "struct",     "enum",      "event",  "fn",        "async",
    "const",    "var",  "let",      "if",         "else",      "for",    "in",        "while",
    "switch",   "case", "default",  "break",      "continue",  "return", "on",        "emit",
    "await",    "try",  "override", "this",       "true",      "false",  "null",      "interface",
    "open",     "{",    "}",        "(",          ")",         "[",      "]",         ";",
    ",",        ".",    "?.",       "?",          ":",         "=",      "==",        "!=",
    "<",        "<=",   ">",        ">=",         "+",         "-",      "*",         "/",
    "%",        "+=",   "-=",       "*=",         "/=",        "!",      "&&",        "||",
    "??",       "..<",  "@",        "as?",        "_",         "a",      "Door",      "x",
    "1",        "0x1F", "1.5",      "\"s\"",      "\"v${x}\"", "@\"v\"", "\n/// d\n", "#",
    "\"open\n", "/*",   "/// d\n",  "move_speed", "0..1",
};

// 조각 사이에 넣는 것이다(붙여 쓰기·줄 바꿈으로 같은 줄 규칙과 `@edit` 꼴도 만든다).
constexpr StringView RandomSeparators[] = {" ", "", "\n"};

constexpr StringView ValidPrograms[] = {
    "import A; class D : B { var x = 1; fn F(a: int): int { return a + x; } }",
    "enum E { A, B } struct S { let v: Array<int>? = null; }",
    "class C : B { on Opened(by: Actor) { if (let d = by.Find<Door>()) { emit Hit(1); } "
    "for (i in 0..<3) { x[i] = await G(); } } }",
    "interface I { fn Use(u: Actor): bool; } const K: int = 1;",
    "fn F() { switch (s) { case A, B: { } default: { } } while (a < b) { break; } }",
};

// 구문 진단(S02xx, S0215 제외)은 위치마다 하나다(원인 하나에 진단 하나).
bool SyntaxDiagnosticsHaveDistinctPositions(const Parsed& parsed) noexcept {
    constexpr uint32 FirstSyntaxCode = 201;
    constexpr uint32 LastSyntaxCode = 299;
    for (usize left = 0; left < parsed.diagnostics.Count(); ++left) {
        const Diagnostic& first = parsed.diagnostics[left];
        const uint32 firstCode = static_cast<uint32>(first.Code());
        if (firstCode < FirstSyntaxCode || firstCode > LastSyntaxCode ||
            first.Code() == C::NestingTooDeep) {
            continue;
        }

        for (usize right = left + 1; right < parsed.diagnostics.Count(); ++right) {
            const Diagnostic& second = parsed.diagnostics[right];
            const uint32 secondCode = static_cast<uint32>(second.Code());
            if (secondCode >= FirstSyntaxCode && secondCode <= LastSyntaxCode &&
                second.Code() != C::NestingTooDeep && first.Span().begin == second.Span().begin) {
                return false;
            }
        }
    }

    return true;
}

// 무작위 원문을 분석해 불변식과 결정성을 본다.
bool SurvivesRandomSource(Allocator& allocator, StringView source) noexcept {
    Parsed first(allocator);
    Parsed second(allocator);
    if (!Parse(first, source)) {
        // 원문 검사에서 막히는 조각은 없으므로 분석은 늘 된다.
        Print(source);
        return false;
    }

    if (!ValidTree(first) || !SyntaxDiagnosticsHaveDistinctPositions(first) ||
        !Parse(second, source)) {
        Print(source);
        PrintDiagnostics(first);
        return false;
    }

    StringBuilder<TextInlineBytes> left(allocator);
    StringBuilder<TextInlineBytes> right(allocator);
    if (!AppendSyntaxDump(left, first.sources, first.tree) ||
        !AppendSyntaxDump(right, second.sources, second.tree) || left.View() != right.View() ||
        first.diagnostics.Count() != second.diagnostics.Count()) {
        Print(source);
        return false;
    }

    return true;
}

} // namespace

TEST(SparksParser_RandomTokenSequences) {
    SystemAllocator allocator;
    {
        RandomStream random(20261120);
        constexpr usize Cases = 3000;
        constexpr uint32 MaxPieces = 48;
        constexpr usize PieceCount = sizeof(RandomPieces) / sizeof(RandomPieces[0]);
        for (usize run = 0; run < Cases; ++run) {
            StringBuilder<TextInlineBytes> source(allocator);
            const uint32 pieces = random.NextUInt32() % MaxPieces;
            constexpr usize SeparatorCount = sizeof(RandomSeparators) / sizeof(RandomSeparators[0]);
            for (uint32 index = 0; index < pieces; ++index) {
                source.Append(RandomPieces[random.NextUInt32() % PieceCount])
                    .Append(RandomSeparators[random.NextUInt32() % SeparatorCount]);
            }

            if (!SurvivesRandomSource(allocator, source.View())) {
                CHECK(false);
                break;
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 깊은 중첩을 자주 만드는 조각으로 상한 근처를 훑는다(S0215와 그 뒤의 멈춤).
TEST(SparksParser_RandomDeepInputs) {
    SystemAllocator allocator;
    {
        constexpr StringView DeepPieces[] = {
            "(",         "[",
            "!",         "-",
            "f(",        "a ?? ",
            "if (a) { ", "} else if (a) { ",
            "A<",        ")",
            "]",         "}",
            ">",         "1",
            "x",         ";",
            ", ",        "+",
            ".b",        "try ",
            "await ",    "fn F() { _ = ",
            "{",         "class A : B { ",
        };
        constexpr usize PieceCount = sizeof(DeepPieces) / sizeof(DeepPieces[0]);
        RandomStream random(128);
        constexpr usize Cases = 300;
        constexpr uint32 MaxPieces = 700;
        for (usize run = 0; run < Cases; ++run) {
            StringBuilder<TextInlineBytes> source(allocator);
            source.Append("fn F() { _ = ");
            const uint32 pieces = random.NextUInt32() % MaxPieces;
            for (uint32 index = 0; index < pieces; ++index) {
                // 여는 조각을 더 자주 골라 깊게 만든다.
                const uint32 pick = random.NextUInt32() % (PieceCount + 8);
                source.Append(DeepPieces[pick < PieceCount ? pick : pick % 6]);
            }

            if (!SurvivesRandomSource(allocator, source.View())) {
                CHECK(false);
                break;
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 올바른 원문에서 byte 구간을 지우거나 조각을 끼워 넣어 본다.
TEST(SparksParser_RandomMutationsOfValidPrograms) {
    SystemAllocator allocator;
    {
        RandomStream random(1120);
        constexpr usize RunsPerProgram = 400;
        constexpr usize PieceCount = sizeof(RandomPieces) / sizeof(RandomPieces[0]);
        constexpr uint32 MaxCut = 6;
        for (const StringView program : ValidPrograms) {
            CHECK(SourceIsClean(program));
            for (usize run = 0; run < RunsPerProgram; ++run) {
                const usize at = random.NextUInt32() % (program.ByteCount() + 1);
                StringBuilder<TextInlineBytes> source(allocator);
                source.Append(StringView(program.Data(), at));
                usize resume = at;
                if ((random.NextUInt32() & 1u) == 0) {
                    const usize cut = 1 + random.NextUInt32() % MaxCut;
                    resume = at + cut < program.ByteCount() ? at + cut : program.ByteCount();
                } else {
                    source.Append(" ")
                        .Append(RandomPieces[random.NextUInt32() % PieceCount])
                        .Append(" ");
                }

                source.Append(StringView(program.Data() + resume, program.ByteCount() - resume));
                if (!SurvivesRandomSource(allocator, source.View())) {
                    CHECK(false);
                    break;
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace sparks_parser_tests_file
