#include <SparksCompiler/LanguageCatalog.h>
#include "LspServer.h"

// 빠른 수정 편집 생성기다(Docs/Design/SparksTools.ko.md 11.7절 표 Q1–Q10). 진단 문구를 해석하지
// 않고 진단의 코드·위치와 그 컴파일의 구문 트리·심볼·타입으로 편집을 만든다.
namespace rocks::sparks::tools::detail {

namespace {

constexpr StringView LspOptionalMark = "?";
constexpr StringView LspExitReturnText = " ?? return";
constexpr StringView LspNullConditionalText = "?.";
constexpr StringView LspDiscardText = "_ = ";
constexpr StringView LspAsyncText = "async ";
constexpr StringView LspImportKeyword = "import ";
constexpr StringView LspImportEnd = ";";
constexpr StringView LspPackageSeparator = ".";
// 재진입 정책 attribute다(Q5 — 수정 순서).
constexpr StringView LspReentrancyAttributes[] = {"@skipWhileBusy", "@restart"};
constexpr StringView LspLineFeed = "\n";
constexpr StringView LspCrLf = "\r\n";
constexpr usize LspFixTextBytes = 256;

// 구문 트리 자식 자리다(SyntaxTree.h 배치).
constexpr uint32 LspFileImportsChild = 0;
constexpr uint32 LspDeclarationNameChild = 2;
constexpr uint32 LspHandlerPathChild = 2;
constexpr uint32 LspFunctionReturnChild = 4;
constexpr uint32 LspLocalTypeChild = 1;
constexpr uint32 LspLocalValueChild = 2;
constexpr uint32 LspLocalExitChild = 3;
constexpr uint32 LspMemberObjectChild = 0;
constexpr uint32 LspCallTargetChild = 0;
constexpr uint32 LspFixImportAliasChild = 1;
constexpr uint32 LspIfLetValueChild = 1;
constexpr uint32 LspBinaryLeftChild = 0;
constexpr uint32 LspBinaryRightChild = 1;
constexpr uint32 LspClassMembersChild = 4;
constexpr uint32 LspHandlerParametersChild = 3;

constexpr StringView LspLineComment = "//";
constexpr StringView LspFixActorTypeName = "Actor";
constexpr StringView LspFixWorldName = "World";
constexpr StringView LspRpcAttribute = "rpc";
constexpr StringView LspTestAttribute = "test";

bool LspIsBlank(char value) noexcept {
    return value == ' ' || value == '\t';
}

// 이름이 name인 엔진 타입 심볼이다(없으면 NoSymbol).
uint32 LspEngineType(const Compilation& compilation, StringView name) noexcept {
    const Span<const Symbol> symbols = compilation.Symbols();
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        if (symbols[at].kind == SymbolKind::EngineType && symbols[at].name == name) {
            return at;
        }
    }

    return NoSymbol;
}

class LspFixBuilder final {
public:
    LspFixBuilder(Allocator& allocator, const Compilation& compilation, const SourceSet& sources,
                  uint32 module, const Diagnostic& diagnostic, Array<LspFix>& fixes) noexcept
        : allocator_(allocator), compilation_(compilation), module_(module),
          diagnostic_(diagnostic), fixes_(fixes), parents_(allocator),
          text_(sources.Get(compilation.ModuleSource(module)).Text()),
          contentBegin_(sources.Get(compilation.ModuleSource(module)).ContentBegin()) {
    }

    Result<void> Run() noexcept;

private:
    const Symbol& SymbolAt(uint32 symbol) const noexcept {
        return compilation_.Symbols()[symbol];
    }

    StringView Slice(SourceSpan span) const noexcept {
        return StringView(text_.Data() + span.begin, span.end - span.begin);
    }

    uint32 Parent(uint32 node) const noexcept {
        return node == NoNode || node >= parents_.Size() ? NoNode : parents_[node];
    }

    uint32 NodeWithSpan(SyntaxKind kind) const noexcept;
    uint32 Enclosing(SyntaxKind kind, uint32 offset) const noexcept;
    uint32 TokenAt(uint32 begin) const noexcept;
    uint32 LineStart(uint32 offset) const noexcept;
    uint32 NextLineStart(uint32 offset) const noexcept;
    StringView LineEnding() const noexcept;
    StringView Indent(uint32 offset) const noexcept;
    void Add(StringView title, uint32 begin, uint32 end, StringView text) noexcept;
    void AddDeletion(StringView title, SourceSpan span) noexcept;
    void AddImport(uint32 target) noexcept;
    bool HasAwait(uint32 from, uint32 to) const noexcept;
    bool CanImport(uint32 target) const noexcept;
    bool ImplementsInterfaceMethod(uint32 classNode, StringView name) const noexcept;
    void FixOptionalField() noexcept;
    void FixMayBeNull() noexcept;
    void FixUnknownName(bool typesOnly) noexcept;
    void FixHiddenExtension() noexcept;
    void FixReentrancy() noexcept;
    void FixImportRemoval() noexcept;
    void FixAwait() noexcept;
    void FixHook() noexcept;
    void FixAttributeName() noexcept;

    Allocator& allocator_;
    const Compilation& compilation_;
    const uint32 module_;
    const Diagnostic& diagnostic_;
    Array<LspFix>& fixes_;
    Array<uint32> parents_;
    const SyntaxTree* tree_ = nullptr;
    const StringView text_;
    const uint32 contentBegin_;
    ErrorCode error_ = ErrorCode::None;
};

// 진단 범위와 같은 범위의 kind 노드다(없으면 NoNode).
uint32 LspFixBuilder::NodeWithSpan(SyntaxKind kind) const noexcept {
    const SourceSpan span = diagnostic_.Span();
    for (uint32 node = 0; node < tree_->NodeCount(); ++node) {
        const SyntaxNode& entry = tree_->Node(node);
        if (entry.kind == kind && entry.span.begin == span.begin && entry.span.end == span.end) {
            return node;
        }
    }

    return NoNode;
}

// offset을 담은 가장 안쪽 kind 노드다.
uint32 LspFixBuilder::Enclosing(SyntaxKind kind, uint32 offset) const noexcept {
    uint32 found = NoNode;
    for (uint32 node = 0; node < tree_->NodeCount(); ++node) {
        const SyntaxNode& entry = tree_->Node(node);
        if (entry.kind != kind || entry.span.begin > offset || offset >= entry.span.end) {
            continue;
        }

        const SourceSpan best = found == NoNode ? SourceSpan() : tree_->Node(found).span;
        if (found == NoNode || entry.span.end - entry.span.begin < best.end - best.begin) {
            found = node;
        }
    }

    return found;
}

// begin에서 시작하는 토큰 번호다(없으면 NoToken).
uint32 LspFixBuilder::TokenAt(uint32 begin) const noexcept {
    const Span<const Token> tokens = tree_->Tokens();
    for (uint32 at = 0; at < tokens.Size(); ++at) {
        if (tokens[at].span.begin == begin && tokens[at].kind != TokenKind::EndOfFile) {
            return at;
        }
    }

    return NoToken;
}

uint32 LspFixBuilder::LineStart(uint32 offset) const noexcept {
    uint32 at = offset;
    while (at > contentBegin_ && text_.Data()[at - 1] != '\n') {
        --at;
    }

    return at;
}

// offset 뒤 첫 LF 다음 자리다(없으면 원문 끝).
uint32 LspFixBuilder::NextLineStart(uint32 offset) const noexcept {
    uint32 at = offset;
    while (at < text_.ByteCount() && text_.Data()[at] != '\n') {
        ++at;
    }

    return at < text_.ByteCount() ? at + 1 : at;
}

// 문서의 첫 줄 끝 모양이다(없으면 LF).
StringView LspFixBuilder::LineEnding() const noexcept {
    for (usize at = contentBegin_; at < text_.ByteCount(); ++at) {
        if (text_.Data()[at] == '\n') {
            return at > contentBegin_ && text_.Data()[at - 1] == '\r' ? LspCrLf : LspLineFeed;
        }
    }

    return LspLineFeed;
}

// offset이 있는 줄의 들여쓰기다.
StringView LspFixBuilder::Indent(uint32 offset) const noexcept {
    const uint32 begin = LineStart(offset);
    uint32 end = begin;
    while (end < text_.ByteCount() && LspIsBlank(text_.Data()[end])) {
        ++end;
    }

    return StringView(text_.Data() + begin, end - begin);
}

void LspFixBuilder::Add(StringView title, uint32 begin, uint32 end, StringView text) noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    LspFix fix(allocator_);
    fix.begin = begin;
    fix.end = end;
    if (!fix.title.TryAssign(title) || !fix.text.TryAssign(text)) {
        error_ = ErrorCode::OutOfMemory;
        return;
    }

    if (auto pushed = fixes_.TryPushBack(Move(fix)); !pushed) {
        error_ = pushed.Error();
    }
}

// span을 지운다: 그 줄의 나머지가 공백뿐이면 줄 전체(줄 끝 포함), 아니면 span과 뒤 공백.
void LspFixBuilder::AddDeletion(StringView title, SourceSpan span) noexcept {
    const uint32 lineBegin = LineStart(span.begin);
    uint32 after = span.end;
    while (after < text_.ByteCount() && LspIsBlank(text_.Data()[after])) {
        ++after;
    }

    bool alone = true;
    for (uint32 at = lineBegin; at < span.begin; ++at) {
        alone = alone && LspIsBlank(text_.Data()[at]);
    }

    const bool lineEnds = after == text_.ByteCount() || text_.Data()[after] == '\n' ||
                          (text_.Data()[after] == '\r' && after + 1 < text_.ByteCount() &&
                           text_.Data()[after + 1] == '\n');
    if (alone && lineEnds) {
        Add(title, lineBegin, NextLineStart(after), StringView());
    } else {
        Add(title, span.begin, after, StringView());
    }
}

// target 모듈의 import 한 줄을 마지막 import 뒤(없으면 원문 처음)에 넣는다(Q3·Q4). 마지막 import
// 뒤 같은 줄에 공백·줄 주석 밖의 것(선언·블록 주석)이 있으면 그 import 바로 뒤에 줄을 바꿔 넣는다.
void LspFixBuilder::AddImport(uint32 target) noexcept {
    const uint32 package = compilation_.ModulePackage(module_);
    const uint32 targetPackage = compilation_.ModulePackage(target);
    const StringView path = compilation_.ModuleImportPath(target);
    if (path.IsEmpty() || package == NoPackage || targetPackage == NoPackage) {
        return;
    }

    StringBuilder<LspFixTextBytes> line(allocator_);
    StringBuilder<LspFixTextBytes> title(allocator_);
    line.Append(LspImportKeyword);
    if (targetPackage != package) {
        line.Append(compilation_.Manifest(targetPackage).Name()).Append(LspPackageSeparator);
    }

    line.Append(path).Append(LspImportEnd);
    title.Append("Add '").Append(line.View()).Append("'");
    const StringView ending = LineEnding();
    uint32 last = NoNode;
    const uint32 imports = tree_->Child(tree_->Root(), LspFileImportsChild);
    if (imports != NoNode) {
        for (const uint32 node : tree_->Children(imports)) {
            last = node != NoNode && tree_->Node(node).kind == SyntaxKind::Import ? node : last;
        }
    }

    StringBuilder<LspFixTextBytes> inserted(allocator_);
    uint32 at = contentBegin_;
    if (last != NoNode) {
        const uint32 end = tree_->Node(last).span.end;
        uint32 rest = end;
        while (rest < text_.ByteCount() && LspIsBlank(text_.Data()[rest])) {
            ++rest;
        }

        const StringView tail(text_.Data() + rest, text_.ByteCount() - rest);
        const bool lineEnds = rest == text_.ByteCount() || text_.Data()[rest] == '\n' ||
                              text_.Data()[rest] == '\r' ||
                              tail.StartsWith(LspLineComment, StringComparison::CaseSensitive);
        if (!lineEnds) {
            at = end;
            inserted.Append(ending).Append(line.View());
        } else {
            at = NextLineStart(end);
            if (at == text_.ByteCount() && (at == 0 || text_.Data()[at - 1] != '\n')) {
                inserted.Append(ending); // 마지막 import가 원문 끝 줄이다
            }

            inserted.Append(line.View()).Append(ending);
        }
    } else {
        inserted.Append(line.View()).Append(ending).Append(ending);
    }

    if (line.HasFailed() || title.HasFailed() || inserted.HasFailed()) {
        error_ = ErrorCode::OutOfMemory;
        return;
    }

    Add(title.View(), at, at, inserted.View());
}

// [from, to)에서 시작하는 대기(`await`·`try await`)가 있는지다.
bool LspFixBuilder::HasAwait(uint32 from, uint32 to) const noexcept {
    for (uint32 node = 0; node < tree_->NodeCount(); ++node) {
        const SyntaxNode& entry = tree_->Node(node);
        const bool waits =
            entry.kind == SyntaxKind::Await ||
            (entry.kind == SyntaxKind::Try && (entry.flags & SyntaxFlags::Await) != 0);
        if (waits && entry.span.begin >= from && entry.span.begin < to) {
            return true;
        }
    }

    return false;
}

// target 모듈을 이 모듈에 import할 수 있는지다: 같은 패키지나 의존 패키지(S0302), 아직 import하지
// 않았고(S0303), 그 최상위 이름이 이 모듈·import한 모듈의 최상위 이름과 겹치지 않는다(SL 3.4절).
bool LspFixBuilder::CanImport(uint32 target) const noexcept {
    const uint32 package = compilation_.ModulePackage(module_);
    const uint32 other = compilation_.ModulePackage(target);
    if (package == NoPackage || other == NoPackage || target == module_) {
        return false;
    }

    const PackageManifest& manifest = compilation_.Manifest(package);
    bool reachable = other == package;
    for (const ManifestName& dependency : manifest.Dependencies()) {
        reachable = reachable || manifest.Text(dependency) == compilation_.Manifest(other).Name();
    }

    for (const ImportResolution& resolution : compilation_.ModuleImports(module_)) {
        reachable = reachable && resolution.target != target;
    }

    if (!reachable) {
        return false;
    }

    // 이 모듈과 별칭 없이 import한 모듈의 최상위 이름.
    auto visible = [this](uint32 module) noexcept {
        if (module == module_) {
            return true;
        }

        for (const ImportResolution& resolution : compilation_.ModuleImports(module_)) {
            if (resolution.module == module &&
                tree_->Child(resolution.node, LspFixImportAliasChild) == NoNode) {
                return true;
            }
        }

        return false;
    };
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        if (symbols[at].module != target || symbols[at].owner != NoSymbol ||
            symbols[at].kind == SymbolKind::Alias) {
            continue;
        }

        for (uint32 other = 0; other < symbols.Size(); ++other) {
            if (symbols[other].name == symbols[at].name && symbols[other].owner == NoSymbol &&
                symbols[other].kind != SymbolKind::Alias && symbols[other].module != NoModule &&
                visible(symbols[other].module)) {
                return false;
            }
        }
    }

    return true;
}

// classNode의 interface 기반에 name인 메서드가 있는지다(구현 메서드의 `async`는 시그니처를 바꾼다).
bool LspFixBuilder::ImplementsInterfaceMethod(uint32 classNode, StringView name) const noexcept {
    const uint32 owner =
        compilation_.NodeSymbol(module_, tree_->Child(classNode, LspDeclarationNameChild));
    if (owner == NoSymbol) {
        return false;
    }

    const Span<const Symbol> symbols = compilation_.Symbols();
    const Symbol& entry = symbols[owner];
    for (uint32 index = 0; index < entry.baseCount; ++index) {
        const uint32 base = compilation_.SymbolBases()[entry.firstBase + index];
        if (symbols[base].kind != SymbolKind::Interface) {
            continue;
        }

        for (uint32 at = 0; at < symbols.Size(); ++at) {
            if (symbols[at].owner == base && symbols[at].kind == SymbolKind::InterfaceMethod &&
                symbols[at].name == name) {
                return true;
            }
        }
    }

    return false;
}

// Q1: `?` 없는 객체 필드 — 타입 표기 끝에 `?`.
void LspFixBuilder::FixOptionalField() noexcept {
    const uint32 type = NodeWithSpan(SyntaxKind::Type);
    if (type != NoNode && (tree_->Node(type).flags & SyntaxFlags::Optional) == 0) {
        const uint32 end = tree_->Node(type).span.end;
        Add("Add '?' to the type", end, end, LspOptionalMark);
    }
}

// Q2: null일 수 있는 값 — `let` 지역 선언에 `?? return`, 문장인 멤버 접근에 `?.`.
void LspFixBuilder::FixMayBeNull() noexcept {
    const SourceSpan span = diagnostic_.Span();
    uint32 node = NoNode;
    for (uint32 at = 0; at < tree_->NodeCount() && node == NoNode; ++at) {
        const SyntaxNode& entry = tree_->Node(at);
        if (entry.span.begin == span.begin && entry.span.end == span.end &&
            entry.kind == SyntaxKind::Identifier) {
            node = at;
        }
    }

    const uint32 symbol = node == NoNode ? NoSymbol : compilation_.NodeSymbol(module_, node);
    if (symbol != NoSymbol && SymbolAt(symbol).kind == SymbolKind::Local &&
        SymbolAt(symbol).module == module_ && SymbolAt(symbol).nameNode < tree_->NodeCount()) {
        const Symbol& local = SymbolAt(symbol);
        const uint32 declaration = Parent(local.nameNode);
        const bool optional = local.type != NoType && local.type < compilation_.Types().Size() &&
                              compilation_.Types()[local.type].optional;
        bool fits = declaration != NoNode &&
                    tree_->Node(declaration).kind == SyntaxKind::LocalVariable &&
                    (tree_->Node(declaration).flags & SyntaxFlags::Let) != 0 && optional &&
                    tree_->Child(declaration, LspLocalTypeChild) == NoNode &&
                    tree_->Child(declaration, LspLocalExitChild) == NoNode &&
                    tree_->Child(declaration, LspLocalValueChild) != NoNode;
        // 선언과 진단 사이의 대기는 await 뒤 `T?`라 `?? return`이 풀지 못한다(SL 3.11절).
        fits = fits && !HasAwait(tree_->Node(declaration).span.end, span.begin);
        // 다른 쓰임이 null을 다루면(`?.`·`??`·`if let`·null 비교) `?? return` 뒤에 그 쓰임이
        // 오류다.
        for (uint32 at = 0; fits && at < tree_->NodeCount(); ++at) {
            if (at == node || tree_->Node(at).kind != SyntaxKind::Identifier ||
                compilation_.NodeSymbol(module_, at) != symbol) {
                continue;
            }

            const uint32 holder = Parent(at);
            if (holder == NoNode) {
                continue;
            }

            const SyntaxNode& use = tree_->Node(holder);
            const TokenKind op =
                use.token == NoToken ? TokenKind::EndOfFile : tree_->Tokens()[use.token].kind;
            const uint32 other =
                use.kind == SyntaxKind::Binary
                    ? tree_->Child(holder, tree_->Child(holder, LspBinaryLeftChild) == at
                                               ? LspBinaryRightChild
                                               : LspBinaryLeftChild)
                    : NoNode;
            const bool nullCompare = use.kind == SyntaxKind::Binary &&
                                     (op == TokenKind::EqualEqual || op == TokenKind::BangEqual) &&
                                     other != NoNode &&
                                     tree_->Node(other).kind == SyntaxKind::NullLiteral;
            fits = !(
                (use.kind == SyntaxKind::Member &&
                 (use.flags & SyntaxFlags::NullConditional) != 0) ||
                (use.kind == SyntaxKind::Binary && op == TokenKind::QuestionQuestion &&
                 tree_->Child(holder, LspBinaryLeftChild) == at) ||
                (use.kind == SyntaxKind::IfLet && tree_->Child(holder, LspIfLetValueChild) == at) ||
                nullCompare);
        }

        // 감싼 함수가 값을 돌려주지 않아야 `?? return`(값 없는 return)이 맞다.
        uint32 callable = Parent(declaration);
        while (fits && callable != NoNode && tree_->Node(callable).kind != SyntaxKind::Function &&
               tree_->Node(callable).kind != SyntaxKind::Handler) {
            callable = Parent(callable);
        }

        if (fits && callable != NoNode &&
            (tree_->Node(callable).kind == SyntaxKind::Handler ||
             tree_->Child(callable, LspFunctionReturnChild) == NoNode)) {
            const uint32 end = tree_->Node(tree_->Child(declaration, LspLocalValueChild)).span.end;
            Add("Add '?? return'", end, end, LspExitReturnText);
        }
    }

    // 진단 노드가 문장인 `x.멤버`·`x.멤버(...)`의 받는 쪽이면 `.`을 `?.`로(대입 대상·값으로 쓰는
    // 자리는 `T?` 결과가 다른 오류다).
    for (uint32 at = 0; at < tree_->NodeCount(); ++at) {
        const SyntaxNode& entry = tree_->Node(at);
        if (entry.kind != SyntaxKind::Member || (entry.flags & SyntaxFlags::NullConditional) != 0 ||
            entry.token == NoToken || entry.token == 0) {
            continue;
        }

        const uint32 object = tree_->Child(at, LspMemberObjectChild);
        if (object == NoNode || tree_->Node(object).span.begin != span.begin ||
            tree_->Node(object).span.end != span.end) {
            continue;
        }

        uint32 value = at;
        const uint32 call = Parent(at);
        if (call != NoNode && tree_->Node(call).kind == SyntaxKind::Call &&
            tree_->Child(call, LspCallTargetChild) == at) {
            value = call;
        }

        const uint32 statement = Parent(value);
        const bool alone = statement != NoNode &&
                           (tree_->Node(statement).kind == SyntaxKind::ExpressionStatement ||
                            tree_->Node(statement).kind == SyntaxKind::DiscardAssignment);
        const Token& dot = tree_->Tokens()[entry.token - 1];
        if (alone && dot.kind == TokenKind::Dot) {
            Add("Use '?.'", dot.span.begin, dot.span.end, LspNullConditionalText);
        }

        break;
    }
}

// Q3: 모르는 이름·타입 — 그 이름의 최상위 선언을 가진 모듈의 import.
void LspFixBuilder::FixUnknownName(bool typesOnly) noexcept {
    const StringView name = Slice(diagnostic_.Span());
    if (name.IsEmpty()) {
        return;
    }

    // 이름 하나(식의 Identifier, 경로가 한 구간인 타입 이름)만 — `L.Y`의 `Y`는 import로 풀리지
    // 않는다.
    bool single = NodeWithSpan(SyntaxKind::Identifier) != NoNode;
    const uint32 written = NodeWithSpan(SyntaxKind::Name);
    const uint32 path = Parent(written);
    single =
        single || (written != NoNode && path != NoNode &&
                   tree_->Node(path).kind == SyntaxKind::Path && tree_->Node(path).childCount == 1);
    if (!single) {
        return;
    }

    Array<uint32> targets(allocator_);
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        const Symbol& entry = symbols[at];
        const bool type = entry.kind == SymbolKind::Class || entry.kind == SymbolKind::Struct ||
                          entry.kind == SymbolKind::Enum || entry.kind == SymbolKind::Interface;
        const bool value = entry.kind == SymbolKind::Const || entry.kind == SymbolKind::Function;
        if (entry.name != name || entry.owner != NoSymbol || entry.module == NoModule ||
            entry.module == module_ || !(type || (value && !typesOnly))) {
            continue;
        }

        const bool reachable = CanImport(entry.module);
        bool seen = false;
        for (const uint32 target : targets) {
            seen = seen || target == entry.module;
        }

        if (reachable && !seen) {
            if (auto pushed = targets.TryPushBack(entry.module); !pushed) {
                error_ = pushed.Error();
                return;
            }
        }
    }

    // 모듈 경로(패키지 이름 → import 경로) 순.
    for (usize left = 0; left < targets.Size(); ++left) {
        for (usize right = left + 1; right < targets.Size(); ++right) {
            const uint32 a = targets[left];
            const uint32 b = targets[right];
            int order = StringView::CompareBytes(
                compilation_.Manifest(compilation_.ModulePackage(a)).Name(),
                compilation_.Manifest(compilation_.ModulePackage(b)).Name());
            order = order != 0 ? order
                               : StringView::CompareBytes(compilation_.ModuleImportPath(a),
                                                          compilation_.ModuleImportPath(b));
            if (order > 0) {
                targets[left] = b;
                targets[right] = a;
            }
        }
    }

    for (const uint32 target : targets) {
        AddImport(target);
    }
}

// Q4: import하지 않은 확장 이벤트 — 그 대상(`World` 원천이면 World, 그 밖은 Actor)의 그 이름
// 확장 이벤트를 선언한, import할 수 있는 모듈의 import.
void LspFixBuilder::FixHiddenExtension() noexcept {
    const StringView name = Slice(diagnostic_.Span());
    const uint32 path = Enclosing(SyntaxKind::EventPath, diagnostic_.Span().begin);
    const uint32 first =
        path == NoNode || tree_->Node(path).childCount == 0 ? NoNode : tree_->Child(path, 0);
    const bool world = first != NoNode && tree_->Node(first).token != NoToken &&
                       Slice(tree_->Tokens()[tree_->Node(first).token].span) == LspFixWorldName;
    const uint32 target =
        LspEngineType(compilation_, world ? LspFixWorldName : LspFixActorTypeName);
    const Span<const Symbol> symbols = compilation_.Symbols();
    Array<uint32> targets(allocator_);
    for (uint32 at = 0; target != NoSymbol && at < symbols.Size(); ++at) {
        const Symbol& entry = symbols[at];
        if (entry.kind != SymbolKind::Event || entry.name != name || entry.owner != target ||
            entry.module == NoModule || entry.module == module_) {
            continue;
        }

        bool skip = !CanImport(entry.module);
        for (const ImportResolution& resolution : compilation_.ModuleImports(module_)) {
            skip = skip || resolution.module == entry.module;
        }

        for (const uint32 known : targets) {
            skip = skip || known == entry.module;
        }

        if (!skip) {
            if (auto pushed = targets.TryPushBack(entry.module); !pushed) {
                error_ = pushed.Error();
                return;
            }
        }
    }

    for (const uint32 module : targets) {
        AddImport(module);
    }
}

// Q5: 재진입 정책 없음 — `on` 줄 앞에 attribute 한 줄.
void LspFixBuilder::FixReentrancy() noexcept {
    const uint32 handler = Enclosing(SyntaxKind::Handler, diagnostic_.Span().begin);
    const uint32 path = handler == NoNode ? NoNode : tree_->Child(handler, LspHandlerPathChild);
    const uint32 first = path == NoNode ? NoToken : TokenAt(tree_->Node(path).span.begin);
    if (first == NoToken || first == 0 || tree_->Tokens()[first - 1].kind != TokenKind::On) {
        return;
    }

    const uint32 at = tree_->Tokens()[first - 1].span.begin;
    const StringView indent = Indent(at);
    const StringView ending = LineEnding();
    for (const StringView attribute : LspReentrancyAttributes) {
        StringBuilder<LspFixTextBytes> title(allocator_);
        StringBuilder<LspFixTextBytes> text(allocator_);
        title.Append("Add '").Append(attribute).Append("'");
        text.Append(attribute).Append(ending).Append(indent);
        if (title.HasFailed() || text.HasFailed()) {
            error_ = ErrorCode::OutOfMemory;
            return;
        }

        Add(title.View(), at, at, text.View());
    }
}

// Q7: 쓰지 않는·겹친·자기·Rocks import — 그 import 문을 지운다. 겹친 import가 별칭을 두면 그
// 별칭이 쓰일 수 있어 지우지 않는다.
void LspFixBuilder::FixImportRemoval() noexcept {
    const uint32 import = Enclosing(SyntaxKind::Import, diagnostic_.Span().begin);
    if (import == NoNode || (diagnostic_.Code() == DiagnosticCode::DuplicateImport &&
                             tree_->Child(import, LspFixImportAliasChild) != NoNode)) {
        return;
    }

    AddDeletion("Remove the import", tree_->Node(import).span);
}

// Q9: `async` 밖의 `await` — class·struct 함수(재정의·`open`·`@rpc`·interface 구현은 시그니처의
// `async`가 바뀌어 다른 오류라 빼고)와 최상위 `@test` 함수면 `fn` 앞에 `async `, 대기 없는 최상위
// 함수의 `async`는 지운다.
void LspFixBuilder::FixAwait() noexcept {
    const uint32 function = Enclosing(SyntaxKind::Function, diagnostic_.Span().begin);
    if (function == NoNode) {
        return;
    }

    const uint32 name = tree_->Child(function, LspDeclarationNameChild);
    const uint32 token = name == NoNode ? NoToken : tree_->Node(name).token;
    if (token == NoToken || token == 0 || tree_->Tokens()[token - 1].kind != TokenKind::Fn) {
        return;
    }

    const uint32 list = Parent(function);
    const uint32 owner = Parent(list);
    const bool member = owner != NoNode && (tree_->Node(owner).kind == SyntaxKind::Class ||
                                            tree_->Node(owner).kind == SyntaxKind::Struct);
    const uint32 symbol = compilation_.NodeSymbol(module_, name);
    const Token& fn = tree_->Tokens()[token - 1];
    const uint32 flags = tree_->Node(function).flags;
    const bool async = (flags & SyntaxFlags::Async) != 0;
    if (!async) {
        const bool signatureBound =
            (flags & (SyntaxFlags::Override | SyntaxFlags::Open)) != 0 ||
            (symbol != NoSymbol && compilation_.HasAttribute(symbol, LspRpcAttribute)) ||
            (member && tree_->Node(owner).kind == SyntaxKind::Class &&
             ImplementsInterfaceMethod(owner, Slice(tree_->Tokens()[token].span)));
        const bool test = symbol != NoSymbol && compilation_.HasAttribute(symbol, LspTestAttribute);
        if ((member && !signatureBound) || (!member && test)) {
            Add("Declare the function 'async'", fn.span.begin, fn.span.begin, LspAsyncText);
        }
    } else if (!member && token >= 2 && tree_->Tokens()[token - 2].kind == TokenKind::Async &&
               !HasAwait(tree_->Node(function).span.begin, tree_->Node(function).span.end)) {
        const Token& keyword = tree_->Tokens()[token - 2];
        Add("Remove 'async'", keyword.span.begin, fn.span.begin, StringView());
    }
}

// Q10: 모르는 hook — 컴파일러 수정 예와 같은 제안으로 이름을 바꾼다. 자기 class의 Sparks 이벤트를
// hook으로 쓴 경우(컴파일러도 제안하지 않는다), 인자를 쓴 handler(hook마다 인자 모양이 다르다),
// 그 hook이 이미 있는 class(S0806)는 수정이 없다.
void LspFixBuilder::FixHook() noexcept {
    const StringView name = Slice(diagnostic_.Span());
    const StringView best = SimilarHook(name);
    const uint32 handler = Enclosing(SyntaxKind::Handler, diagnostic_.Span().begin);
    const uint32 parameters =
        handler == NoNode ? NoNode : tree_->Child(handler, LspHandlerParametersChild);
    if (best.IsEmpty() || parameters == NoNode || tree_->Node(parameters).childCount != 0) {
        return;
    }

    const uint32 owner = Enclosing(SyntaxKind::Class, diagnostic_.Span().begin);
    if (owner == NoNode) {
        return;
    }

    // 컴파일러는 class 이름 공간(Sparks class 체인)의 멤버만 본다 — 엔진 타입의 확장 이벤트는
    // 아니다.
    uint32 type = compilation_.NodeSymbol(module_, tree_->Child(owner, LspDeclarationNameChild));
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 step = 0;
         type != NoSymbol && symbols[type].kind == SymbolKind::Class && step < symbols.Size();
         ++step) {
        for (uint32 at = 0; at < symbols.Size(); ++at) {
            if (symbols[at].owner == type && symbols[at].name == name &&
                symbols[at].kind == SymbolKind::Event) {
                return;
            }
        }

        type = symbols[type].base;
    }

    const uint32 members = tree_->Child(owner, LspClassMembersChild);
    for (const uint32 member :
         members == NoNode ? Span<const uint32>() : tree_->Children(members)) {
        if (member == NoNode || tree_->Node(member).kind != SyntaxKind::Handler) {
            continue;
        }

        const uint32 path = tree_->Child(member, LspHandlerPathChild);
        if (path != NoNode && tree_->Node(path).childCount == 1 &&
            tree_->Child(path, 0) != NoNode &&
            tree_->Node(tree_->Child(path, 0)).token != NoToken &&
            Slice(tree_->Tokens()[tree_->Node(tree_->Child(path, 0)).token].span) == best) {
            return;
        }
    }

    StringBuilder<LspFixTextBytes> title(allocator_);
    title.Append("Change to '").Append(best).Append("'");
    if (title.HasFailed()) {
        error_ = ErrorCode::OutOfMemory;
        return;
    }

    Add(title.View(), diagnostic_.Span().begin, diagnostic_.Span().end, best);
}

// Q10: 모르는 attribute — 같은 제안으로 이름 토큰을 바꾼다.
void LspFixBuilder::FixAttributeName() noexcept {
    const uint32 attribute = NodeWithSpan(SyntaxKind::Attribute);
    const uint32 token = attribute == NoNode ? NoToken : tree_->Node(attribute).token;
    if (token == NoToken) {
        return;
    }

    const SourceSpan span = tree_->Tokens()[token].span;
    const StringView best = SimilarAttribute(Slice(span));
    if (best.IsEmpty()) {
        return;
    }

    StringBuilder<LspFixTextBytes> title(allocator_);
    title.Append("Change to '@").Append(best).Append("'");
    if (title.HasFailed()) {
        error_ = ErrorCode::OutOfMemory;
        return;
    }

    Add(title.View(), span.begin, span.end, best);
}

Result<void> LspFixBuilder::Run() noexcept {
    tree_ = compilation_.ModuleTree(module_);
    if (tree_ == nullptr || !diagnostic_.HasSpan() || diagnostic_.Span().end > text_.ByteCount()) {
        return Result<void>::Success();
    }

    if (auto sized = parents_.TryResize(tree_->NodeCount()); !sized) {
        return sized;
    }

    for (uint32& parent : parents_) {
        parent = NoNode;
    }

    for (uint32 node = 0; node < tree_->NodeCount(); ++node) {
        for (const uint32 child : tree_->Children(node)) {
            if (child != NoNode && child < parents_.Size()) {
                parents_[child] = node;
            }
        }
    }

    switch (diagnostic_.Code()) {
    case DiagnosticCode::ObjectFieldNeedsOptional:
    case DiagnosticCode::LevelScriptRequiredObject:
        FixOptionalField();
        break;
    case DiagnosticCode::MayBeNull:
        FixMayBeNull();
        break;
    case DiagnosticCode::UnknownName:
        FixUnknownName(false);
        break;
    case DiagnosticCode::UnknownType:
        FixUnknownName(true);
        break;
    case DiagnosticCode::ExtensionEventNotImported:
        FixHiddenExtension();
        break;
    case DiagnosticCode::MissingReentrancyPolicy:
        FixReentrancy();
        break;
    case DiagnosticCode::MisplacedReentrancyPolicy:
    case DiagnosticCode::DuplicateAttribute: {
        const uint32 attribute = NodeWithSpan(SyntaxKind::Attribute);
        if (attribute != NoNode) {
            AddDeletion("Remove the attribute", tree_->Node(attribute).span);
        }

        break;
    }
    case DiagnosticCode::UnusedImport:
    case DiagnosticCode::DuplicateImport:
    case DiagnosticCode::SelfImport:
    case DiagnosticCode::RocksImport:
        FixImportRemoval();
        break;
    case DiagnosticCode::DiscardedResult:
        Add("Discard the result with '_ ='", diagnostic_.Span().begin, diagnostic_.Span().begin,
            LspDiscardText);
        break;
    case DiagnosticCode::AwaitOutsideAsync:
        FixAwait();
        break;
    case DiagnosticCode::UnknownHook:
        FixHook();
        break;
    case DiagnosticCode::UnknownAttribute:
        FixAttributeName();
        break;
    default:
        break;
    }

    return error_ == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error_);
}

} // namespace

Result<void> LspCollectFixes(Allocator& allocator, const Compilation& compilation,
                             const SourceSet& sources, uint32 module, const Diagnostic& diagnostic,
                             Array<LspFix>& fixes) noexcept {
    LspFixBuilder builder(allocator, compilation, sources, module, diagnostic, fixes);
    return builder.Run();
}

} // namespace rocks::sparks::tools::detail
