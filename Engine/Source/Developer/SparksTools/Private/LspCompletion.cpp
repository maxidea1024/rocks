#include <Core/Algorithms.h>
#include <SparksCompiler/BindingSchema.h>
#include <SparksCompiler/LanguageCatalog.h>
#include <SparksCompiler/Lexer.h>
#include "LspServer.h"

// 자동완성 후보 모으기다(Docs/Design/SparksTools.ko.md 11.6절). 구멍 식별자를 넣은 snapshot
// 컴파일의 결과에서 구멍 토큰의 자리(앞 토큰·노드)로 후보 종류를 정하고, 접두로 거른 뒤 순위·이름
// 순으로 정렬한다.
namespace rocks::sparks::tools::detail {

namespace {

// LSP CompletionItemKind 번호다(11.6절 표).
constexpr uint8 LspItemMethod = 2;
constexpr uint8 LspItemFunction = 3;
constexpr uint8 LspItemField = 5;
constexpr uint8 LspItemVariable = 6;
constexpr uint8 LspItemClass = 7;
constexpr uint8 LspItemInterface = 8;
constexpr uint8 LspItemModule = 9;
constexpr uint8 LspItemProperty = 10;
constexpr uint8 LspItemEnum = 13;
constexpr uint8 LspItemKeyword = 14;
constexpr uint8 LspItemEnumMember = 20;
constexpr uint8 LspItemConstant = 21;
constexpr uint8 LspItemStruct = 22;
constexpr uint8 LspItemEvent = 23;
constexpr uint8 LspItemTypeParameter = 25;

// 가까움 순위다(sortText 첫 글자 — 11.6절).
constexpr uint8 LspRankLocal = 0;
constexpr uint8 LspRankMember = 1;
constexpr uint8 LspRankModule = 2;
constexpr uint8 LspRankImport = 3;
constexpr uint8 LspRankPrelude = 4;
constexpr uint8 LspRankEngine = 5;
constexpr uint8 LspRankKeyword = 6;

constexpr StringView LspChangedName = "Changed";
constexpr StringView LspLineCommentMarker = "//";
constexpr StringView LspBlockCommentMarker = "/*";
constexpr StringView LspPathDot = ".";
constexpr char LspPathSeparator = '.';
constexpr usize LspTypedPathBytes = 128;

// For 노드의 본문 자리와 If의 then 자리다(SyntaxTree.h 자식 배치).
constexpr uint32 LspForBodyChild = 4;
constexpr uint32 LspIfThenChild = 1;
// Class 노드의 이름·기반 목록 자리다.
constexpr uint32 LspClassNameChild = 2;
constexpr uint32 LspClassBasesChild = 3;
// Import 노드의 별칭 자리다.
constexpr uint32 LspImportAliasChild = 1;

// 후보를 찾는 자리다.
enum class LspPlace : uint8 {
    Scope,     // 식·문장 자리(보이는 이름 전부)
    Types,     // 타입 자리
    FirstBase, // class 기반 목록의 첫 자리(확장 가능한 엔진 object·asset, Sparks `open class`)
    LaterBase, // class 기반 목록의 뒤 자리(interface — SL 3.9절)
};

// 체인 멤버 중 무엇을 내는지다.
enum class LspMembers : uint8 {
    All,           // 인스턴스 멤버 전부
    Fields,        // 필드
    ChangedFields, // `Changed`가 있는 필드(`on F.G.Changed`)
    Events,        // 이벤트 전부(Sparks·엔진·언어·확장)
    EngineEvents,  // 엔진 이벤트와 확장 이벤트(`on this.E` — Sparks `event`는 아니다)
};

constexpr StringView LspActorTypeName = "Actor";
constexpr StringView LspWorldName = "World";
constexpr StringView LspArrayTypeName = "Array";
constexpr StringView LspThisKeyword = "this";
// `on` 경로의 앞 구간 상한이다(`F.G.Changed` — 넷 이상은 S0807).
constexpr usize MaxLspHandlerPathNames = 2;

bool LspIsFieldKind(SymbolKind kind) noexcept {
    return kind == SymbolKind::Field || kind == SymbolKind::EngineField ||
           kind == SymbolKind::LanguageField;
}

bool LspIsEventKind(SymbolKind kind) noexcept {
    return kind == SymbolKind::Event || kind == SymbolKind::EngineEvent ||
           kind == SymbolKind::LanguageEvent;
}

bool LspIsMemberKind(SymbolKind kind) noexcept {
    switch (kind) {
    case SymbolKind::Field:
    case SymbolKind::Method:
    case SymbolKind::Event:
    case SymbolKind::InterfaceMethod:
    case SymbolKind::EngineField:
    case SymbolKind::EngineFunction:
    case SymbolKind::EngineEvent:
    case SymbolKind::LanguageField:
    case SymbolKind::LanguageFunction:
    case SymbolKind::LanguageEvent:
        return true;
    default:
        return false;
    }
}

class LspCompletionCollector final {
public:
    LspCompletionCollector(Allocator& allocator, const Compilation& compilation,
                           const SourceSet& sources, uint32 module, uint32 hole, StringView prefix,
                           Array<LspCompletionItem>& items) noexcept
        : allocator_(allocator), compilation_(compilation), sources_(sources), module_(module),
          hole_(hole), prefix_(prefix), items_(items), parents_(allocator) {
    }

    Result<void> Run() noexcept;

private:
    const Symbol& SymbolAt(uint32 symbol) const noexcept {
        return compilation_.Symbols()[symbol];
    }

    StringView TokenText(uint32 token) const noexcept;
    bool Contains(uint32 node) const noexcept;
    uint32 Parent(uint32 node) const noexcept;
    void Add(LspCompletionItem item) noexcept;
    void AddSymbol(uint32 symbol, uint8 rank) noexcept;
    void AddWord(LspCompletionSource source, StringView label, StringView text, uint8 kind,
                 uint8 rank) noexcept;
    uint8 KindOf(uint32 symbol) const noexcept;
    uint32 EnclosingType() const noexcept;
    uint32 TypeSymbolOf(uint32 type) const noexcept;
    bool IsEngineBase(uint32 symbol, LspPlace place) const noexcept;
    bool IsObjectSymbol(uint32 symbol) const noexcept;
    bool HasChanged(uint32 field) const noexcept;
    bool IsVisibleExtension(uint32 symbol) const noexcept;
    bool IsVisibleLocal(uint32 symbol) const noexcept;
    uint32 EngineAncestor(uint32 type, StringView name) const noexcept;
    uint32 ContextSymbol(StringView name) const noexcept;
    uint32 FindInChain(uint32 type, StringView name) const noexcept;
    uint32 FindNamed(uint32 owner, StringView name) const noexcept;
    void AddChainMembers(uint32 type, uint32 receiver, LspMembers members) noexcept;
    void AddExtensionEvents(uint32 target) noexcept;
    void AddStaticMembers(uint32 type) noexcept;
    void AddTopLevel(uint32 module, uint8 rank, LspPlace place) noexcept;
    void AddLocals() noexcept;
    void AddScope(LspPlace place) noexcept;
    void AddMember(uint32 node) noexcept;
    void AddPathType(uint32 path, LspPlace place) noexcept;
    void AddHandlerPath(Span<const uint32> names) noexcept;
    void AddEmitTargets(Span<const uint32> names) noexcept;
    void AddImportPath(Span<const uint32> names) noexcept;
    void AddImportSegment(StringView path, StringView typed, StringView before) noexcept;
    void AddKeywords() noexcept;

    Allocator& allocator_;
    const Compilation& compilation_;
    const SourceSet& sources_;
    const uint32 module_;
    const uint32 hole_; // 구멍 토큰 번호
    const StringView prefix_;
    Array<LspCompletionItem>& items_;
    Array<uint32> parents_; // 노드 → 부모 노드
    const SyntaxTree* tree_ = nullptr;
    uint32 holeBegin_ = 0;
    uint32 holeNode_ = NoNode;
    uint32 declaring_ = NoSymbol; // 기반 목록을 쓰는 class(자기 자신은 기반 후보가 아니다)
    bool core_ = false;
    ErrorCode error_ = ErrorCode::None;
};

StringView LspCompletionCollector::TokenText(uint32 token) const noexcept {
    const SourceSpan span = tree_->Tokens()[token].span;
    const StringView text = sources_.Get(compilation_.ModuleSource(module_)).Text();
    return StringView(text.Data() + span.begin, span.end - span.begin);
}

bool LspCompletionCollector::Contains(uint32 node) const noexcept {
    // 끝 자리는 밖이다(`{ }|`의 구멍은 블록 밖 — 닫히지 않은 본문은 parser가 구멍까지 담는다).
    if (node == NoNode) {
        return false;
    }

    const SourceSpan span = tree_->Node(node).span;
    return span.begin <= holeBegin_ && holeBegin_ < span.end;
}

uint32 LspCompletionCollector::Parent(uint32 node) const noexcept {
    return node == NoNode || node >= parents_.Size() ? NoNode : parents_[node];
}

void LspCompletionCollector::Add(LspCompletionItem item) noexcept {
    if (error_ != ErrorCode::None || item.label.IsEmpty()) {
        return;
    }

    if (item.label.StartsWith(prefix_)) {
        item.order = static_cast<uint32>(items_.Size());
        if (auto pushed = items_.TryPushBack(item); !pushed) {
            error_ = pushed.Error();
            return;
        }
    }

    // Unity 이름으로도 찾게 같은 항목을 하나 더 낸다(filterText = Unity 이름 — 11.6절).
    for (const UnityAlias& alias : UnityAliases()) {
        const bool matches =
            alias.rocks == item.label &&
            (alias.hook ? item.source == LspCompletionSource::Hook
                        : item.source == LspCompletionSource::Symbol &&
                              SymbolAt(item.symbol).kind == SymbolKind::EngineFunction);
        if (!matches || !alias.unity.StartsWith(prefix_)) {
            continue;
        }

        LspCompletionItem copy = item;
        copy.filterText = alias.unity;
        copy.order = static_cast<uint32>(items_.Size());
        if (auto pushed = items_.TryPushBack(copy); !pushed) {
            error_ = pushed.Error();
            return;
        }
    }
}

void LspCompletionCollector::AddSymbol(uint32 symbol, uint8 rank) noexcept {
    LspCompletionItem item;
    item.source = LspCompletionSource::Symbol;
    item.label = SymbolAt(symbol).name;
    item.symbol = symbol;
    item.kind = KindOf(symbol);
    item.rank = rank;
    Add(item);
}

void LspCompletionCollector::AddWord(LspCompletionSource source, StringView label, StringView text,
                                     uint8 kind, uint8 rank) noexcept {
    LspCompletionItem item;
    item.source = source;
    item.label = label;
    item.text = text;
    item.kind = kind;
    item.rank = rank;
    Add(item);
}

uint8 LspCompletionCollector::KindOf(uint32 symbol) const noexcept {
    const Symbol& entry = SymbolAt(symbol);
    switch (entry.kind) {
    case SymbolKind::BuiltinType:
    case SymbolKind::PreludeType:
    case SymbolKind::Class:
        return LspItemClass;
    case SymbolKind::PreludeValue:
        // 최상위 prelude 함수(`Delay`)는 그 값 심볼 자신이 함수다(시그니처가 있다).
        return entry.signature != NoSignature ? LspItemFunction : LspItemModule;
    case SymbolKind::RpcPrefix:
    case SymbolKind::Alias:
        return LspItemModule;
    case SymbolKind::ContextName:
    case SymbolKind::Parameter:
    case SymbolKind::Local:
        return LspItemVariable;
    case SymbolKind::EngineType: {
        if (!compilation_.SchemaIsValid(entry.index) ||
            entry.item >= compilation_.Schema(entry.index).Types().Size()) {
            return LspItemClass;
        }

        switch (compilation_.Schema(entry.index).Types()[entry.item].kind) {
        case SchemaTypeKind::Struct:
            return LspItemStruct;
        case SchemaTypeKind::Enum:
            return LspItemEnum;
        case SchemaTypeKind::Interface:
            return LspItemInterface;
        default:
            return LspItemClass;
        }
    }
    case SymbolKind::EngineField: {
        const bool computed = compilation_.SchemaIsValid(entry.index) &&
                              entry.item < compilation_.Schema(entry.index).Fields().Size() &&
                              compilation_.Schema(entry.index).Fields()[entry.item].computed;
        return computed ? LspItemProperty : LspItemField;
    }
    case SymbolKind::EngineFunction:
    case SymbolKind::LanguageFunction:
        return (entry.flags & SymbolFlags::Static) != 0 ? LspItemFunction : LspItemMethod;
    case SymbolKind::EngineEvent:
    case SymbolKind::Event:
    case SymbolKind::LanguageEvent:
        return LspItemEvent;
    case SymbolKind::EngineTypeParameter:
    case SymbolKind::LanguageTypeParameter:
        return LspItemTypeParameter;
    case SymbolKind::Struct:
        return LspItemStruct;
    case SymbolKind::Enum:
        return LspItemEnum;
    case SymbolKind::Interface:
        return LspItemInterface;
    case SymbolKind::Const:
        return LspItemConstant;
    case SymbolKind::Function:
        return LspItemFunction;
    case SymbolKind::Field:
        return LspItemField;
    case SymbolKind::Method:
    case SymbolKind::InterfaceMethod:
        return LspItemMethod;
    case SymbolKind::EnumCase:
        return LspItemEnumMember;
    case SymbolKind::LanguageField:
        // 이름 공간의 상수(`Math.Pi`)·읽기 전용 속성·필드.
        if (entry.owner != NoSymbol && SymbolAt(entry.owner).kind == SymbolKind::PreludeValue) {
            return LspItemConstant;
        }

        return (entry.flags & SymbolFlags::ReadOnly) != 0 ? LspItemProperty : LspItemField;
    }

    return LspItemVariable;
}

uint32 LspCompletionCollector::EnclosingType() const noexcept {
    // 구멍을 담은 가장 안쪽 class·struct 선언의 심볼이다.
    uint32 found = NoNode;
    for (uint32 node = 0; node < tree_->NodeCount(); ++node) {
        const SyntaxNode& entry = tree_->Node(node);
        if ((entry.kind != SyntaxKind::Class && entry.kind != SyntaxKind::Struct) ||
            !Contains(node)) {
            continue;
        }

        const SourceSpan best = found == NoNode ? SourceSpan() : tree_->Node(found).span;
        if (found == NoNode || entry.span.end - entry.span.begin < best.end - best.begin) {
            found = node;
        }
    }

    return found == NoNode
               ? NoSymbol
               : compilation_.NodeSymbol(module_, tree_->Child(found, LspClassNameChild));
}

// 기반 목록 자리에 쓸 수 있는 엔진 타입인지다(SL 3.9절): 첫 자리는 `scriptExtensible`인 object·
// asset, 뒤 자리는 interface.
bool LspCompletionCollector::IsEngineBase(uint32 symbol, LspPlace place) const noexcept {
    const Symbol& entry = SymbolAt(symbol);
    if (!compilation_.SchemaIsValid(entry.index) ||
        entry.item >= compilation_.Schema(entry.index).Types().Size()) {
        return false;
    }

    const SchemaType& type = compilation_.Schema(entry.index).Types()[entry.item];
    if (place == LspPlace::LaterBase) {
        return type.kind == SchemaTypeKind::Interface;
    }

    return (type.kind == SchemaTypeKind::Object || type.kind == SchemaTypeKind::Asset) &&
           type.scriptExtensible;
}

// 객체 타입(Sparks class·interface, 엔진 object·interface)의 심볼인지다(`on F.G.Changed`의 F).
bool LspCompletionCollector::IsObjectSymbol(uint32 symbol) const noexcept {
    if (symbol == NoSymbol) {
        return false;
    }

    const Symbol& entry = SymbolAt(symbol);
    if (entry.kind == SymbolKind::Class || entry.kind == SymbolKind::Interface) {
        return true;
    }

    if (entry.kind != SymbolKind::EngineType || !compilation_.SchemaIsValid(entry.index) ||
        entry.item >= compilation_.Schema(entry.index).Types().Size()) {
        return false;
    }

    const SchemaTypeKind kind = compilation_.Schema(entry.index).Types()[entry.item].kind;
    return kind == SchemaTypeKind::Object || kind == SchemaTypeKind::Interface;
}

// `on 필드.Changed`를 쓸 수 있는 필드인지다: Sparks `var` 필드, schema `changed`가 있는 엔진
// 필드(SL 3.12절 — `let` 필드·언어 멤버에는 없다).
bool LspCompletionCollector::HasChanged(uint32 field) const noexcept {
    const Symbol& entry = SymbolAt(field);
    if (entry.kind == SymbolKind::Field) {
        return (entry.flags & SymbolFlags::Let) == 0;
    }

    return entry.kind == SymbolKind::EngineField && compilation_.SchemaIsValid(entry.index) &&
           entry.item < compilation_.Schema(entry.index).Fields().Size() &&
           compilation_.Schema(entry.index).Fields()[entry.item].changed;
}

// 확장 이벤트(`event Actor.X`)는 이 모듈이나 이 모듈이 직접 import한 모듈의 것만 보인다(S0815).
// 확장 이벤트가 아닌 심볼은 참이다.
bool LspCompletionCollector::IsVisibleExtension(uint32 symbol) const noexcept {
    const Symbol& entry = SymbolAt(symbol);
    if (entry.kind != SymbolKind::Event || entry.owner == NoSymbol ||
        SymbolAt(entry.owner).kind != SymbolKind::EngineType || entry.module == module_) {
        return true;
    }

    for (const ImportResolution& resolution : compilation_.ModuleImports(module_)) {
        if (resolution.module == entry.module) {
            return true;
        }
    }

    return false;
}

// 구멍 앞에 선언되어 구멍을 담는 범위의 지역·매개변수인지다(블록 범위 근사 — 11.6절): 지역
// 선언은 그 블록의 선언 뒤(자기 초기값 안은 아님), `if (let …)`는 then 블록, `for (x in …)`는
// 본문, 매개변수는 선언 전체.
bool LspCompletionCollector::IsVisibleLocal(uint32 symbol) const noexcept {
    const Symbol& entry = SymbolAt(symbol);
    if (entry.module != module_ || entry.nameNode >= tree_->NodeCount() ||
        (entry.kind != SymbolKind::Parameter && entry.kind != SymbolKind::Local)) {
        return false;
    }

    const uint32 holder = Parent(entry.nameNode);
    if (holder == NoNode) {
        return false;
    }

    if (entry.kind == SymbolKind::Parameter) {
        return Contains(Parent(Parent(holder))); // Name → Parameter → List → 함수·handler·이벤트
    }

    const SyntaxKind kind = tree_->Node(holder).kind;
    const bool before = tree_->Node(entry.nameNode).span.end <= holeBegin_;
    if (kind == SyntaxKind::LocalVariable) {
        return before && Contains(Parent(holder)) && !Contains(holder);
    }

    if (kind == SyntaxKind::IfLet) {
        const uint32 branch = Parent(holder);
        return before && branch != NoNode && Contains(tree_->Child(branch, LspIfThenChild));
    }

    if (kind == SyntaxKind::For) {
        return before && Contains(tree_->Child(holder, LspForBodyChild));
    }

    return false;
}

// type 체인에서 이름이 name인 엔진 타입이다(없으면 NoSymbol — `Actor` 파생 판정).
uint32 LspCompletionCollector::EngineAncestor(uint32 type, StringView name) const noexcept {
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 step = 0; type != NoSymbol && step < symbols.Size(); ++step) {
        if (symbols[type].kind == SymbolKind::EngineType && symbols[type].name == name) {
            return type;
        }

        type = symbols[type].base;
    }

    return NoSymbol;
}

uint32 LspCompletionCollector::ContextSymbol(StringView name) const noexcept {
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        if (symbols[at].kind == SymbolKind::ContextName && symbols[at].name == name) {
            return at;
        }
    }

    return NoSymbol;
}

uint32 LspCompletionCollector::TypeSymbolOf(uint32 type) const noexcept {
    return type == NoType || type >= compilation_.Types().Size()
               ? NoSymbol
               : compilation_.Types()[type].symbol;
}

uint32 LspCompletionCollector::FindInChain(uint32 type, StringView name) const noexcept {
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 step = 0; type != NoSymbol && step < symbols.Size(); ++step) {
        for (uint32 at = 0; at < symbols.Size(); ++at) {
            if (symbols[at].owner == type && symbols[at].name == name &&
                LspIsMemberKind(symbols[at].kind)) {
                return at;
            }
        }

        type = symbols[type].base;
    }

    return NoSymbol;
}

// 이름 하나(보이는 지역·매개변수, 아니면 owner 체인의 필드)의 심볼이다(`emit t.X`의 t).
uint32 LspCompletionCollector::FindNamed(uint32 owner, StringView name) const noexcept {
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        if (symbols[at].name == name && IsVisibleLocal(at)) {
            return at;
        }
    }

    const uint32 member = owner == NoSymbol ? NoSymbol : FindInChain(owner, name);
    return member != NoSymbol && LspIsFieldKind(SymbolAt(member).kind) ? member : NoSymbol;
}

// type(심볼)과 기반 체인의 인스턴스 멤버다. receiver는 언어 멤버의 원소 조건을 판정할 받는 값의
// 타입(모르면 NoType — 조건을 보지 않는다)이다. members가 낼 종류를 고른다.
void LspCompletionCollector::AddChainMembers(uint32 type, uint32 receiver,
                                             LspMembers members) noexcept {
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 step = 0; type != NoSymbol && step < symbols.Size(); ++step) {
        for (uint32 at = 0; at < symbols.Size(); ++at) {
            const Symbol& entry = symbols[at];
            if (entry.owner != type || !LspIsMemberKind(entry.kind) ||
                (entry.flags & SymbolFlags::Static) != 0 || !IsVisibleExtension(at)) {
                continue;
            }

            bool wanted = true;
            switch (members) {
            case LspMembers::All:
                break;
            case LspMembers::Fields:
                wanted = LspIsFieldKind(entry.kind);
                break;
            case LspMembers::ChangedFields:
                wanted = LspIsFieldKind(entry.kind) && HasChanged(at);
                break;
            case LspMembers::Events:
                wanted = LspIsEventKind(entry.kind);
                break;
            case LspMembers::EngineEvents:
                wanted = entry.kind == SymbolKind::EngineEvent ||
                         (entry.kind == SymbolKind::Event &&
                          symbols[entry.owner].kind == SymbolKind::EngineType);
                break;
            }

            if (wanted && (receiver == NoType || compilation_.MemberAvailable(at, receiver))) {
                AddSymbol(at, LspRankMember);
            }
        }

        type = symbols[type].base;
    }
}

// target(엔진 `Actor`·`World`)의 보이는 확장 이벤트다(`emit this.X`·`emit t.X`).
void LspCompletionCollector::AddExtensionEvents(uint32 target) noexcept {
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 at = 0; target != NoSymbol && at < symbols.Size(); ++at) {
        if (symbols[at].kind == SymbolKind::Event && symbols[at].owner == target &&
            IsVisibleExtension(at)) {
            AddSymbol(at, LspRankMember);
        }
    }
}

// 타입 이름·이름 공간으로 쓰는 멤버(enum case·static 함수, prelude 이름 공간의 멤버)다.
void LspCompletionCollector::AddStaticMembers(uint32 type) noexcept {
    const Span<const Symbol> symbols = compilation_.Symbols();
    const bool space = symbols[type].kind == SymbolKind::PreludeValue;
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        const Symbol& entry = symbols[at];
        if (entry.owner != type) {
            continue;
        }

        const bool member =
            space ? LspIsMemberKind(entry.kind)
                  : entry.kind == SymbolKind::EnumCase ||
                        (LspIsMemberKind(entry.kind) && (entry.flags & SymbolFlags::Static) != 0);
        if (member) {
            AddSymbol(at, LspRankMember);
        }
    }
}

// module의 최상위 선언이다(타입 자리면 타입만).
void LspCompletionCollector::AddTopLevel(uint32 module, uint8 rank, LspPlace place) noexcept {
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        const Symbol& entry = symbols[at];
        if (entry.module != module || entry.owner != NoSymbol) {
            continue;
        }

        const bool type = entry.kind == SymbolKind::Class || entry.kind == SymbolKind::Struct ||
                          entry.kind == SymbolKind::Enum || entry.kind == SymbolKind::Interface;
        const bool value = entry.kind == SymbolKind::Const || entry.kind == SymbolKind::Function;
        if (!type && !(value && place == LspPlace::Scope)) {
            continue;
        }

        // 기반 목록(SL 3.9절): 첫 자리는 `open class`, 뒤 자리는 interface, 자기 자신은 아니다.
        // core 패키지는 엔진 타입만 쓴다(S1113).
        const bool firstBase =
            entry.kind == SymbolKind::Class && (entry.flags & SymbolFlags::Open) != 0;
        if ((place == LspPlace::FirstBase && (core_ || !firstBase || at == declaring_)) ||
            (place == LspPlace::LaterBase && (core_ || entry.kind != SymbolKind::Interface))) {
            continue;
        }

        AddSymbol(at, rank);
    }
}

// 구멍 자리에서 보이는 지역·매개변수다(IsVisibleLocal).
void LspCompletionCollector::AddLocals() noexcept {
    for (uint32 at = 0; at < compilation_.Symbols().Size(); ++at) {
        if (IsVisibleLocal(at)) {
            AddSymbol(at, LspRankLocal);
        }
    }
}

void LspCompletionCollector::AddScope(LspPlace place) noexcept {
    if (place == LspPlace::Scope) {
        AddLocals();
        const uint32 owner = EnclosingType();
        if (owner != NoSymbol) {
            AddChainMembers(owner, NoType, LspMembers::All);
        }
    }

    AddTopLevel(module_, LspRankModule, place);
    for (const ImportResolution& resolution : compilation_.ModuleImports(module_)) {
        if (resolution.module == NoModule) {
            continue;
        }

        const uint32 alias = tree_->Child(resolution.node, LspImportAliasChild);
        const uint32 symbol = alias == NoNode ? NoSymbol : compilation_.NodeSymbol(module_, alias);
        if (alias == NoNode) {
            AddTopLevel(resolution.module, LspRankImport, place);
        } else if (symbol != NoSymbol) {
            AddSymbol(symbol, LspRankImport);
        }
    }

    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        const Symbol& entry = symbols[at];
        switch (entry.kind) {
        case SymbolKind::BuiltinType:
        case SymbolKind::PreludeType:
            if ((place == LspPlace::Scope || place == LspPlace::Types) &&
                IsUsableLanguageName(entry) &&
                !(core_ && entry.kind == SymbolKind::PreludeType &&
                  entry.name == FullOnlyPreludeTypeName)) {
                AddSymbol(at, LspRankPrelude);
            }

            break;
        case SymbolKind::PreludeValue:
            if (place == LspPlace::Scope && IsUsableLanguageName(entry)) {
                AddSymbol(at, LspRankPrelude);
            }

            break;
        case SymbolKind::ContextName:
            if (place == LspPlace::Scope) {
                AddSymbol(at, LspRankPrelude);
            }

            break;
        case SymbolKind::EngineType:
            if (place == LspPlace::Scope || place == LspPlace::Types || IsEngineBase(at, place)) {
                AddSymbol(at, LspRankEngine);
            }

            break;
        default:
            break;
        }
    }

    if (place == LspPlace::Scope) {
        AddKeywords();
    }
}

// `식.|`: 받는 쪽이 import 별칭이면 그 모듈의 최상위 선언, 타입 이름·이름 공간이면 static 멤버,
// 값이면 그 타입(문맥 이름은 그 타입)의 인스턴스 멤버다.
void LspCompletionCollector::AddMember(uint32 node) noexcept {
    const uint32 object = tree_->Child(node, 0);
    if (object == NoNode) {
        return;
    }

    const uint32 symbol = compilation_.NodeSymbol(module_, object);
    if (symbol != NoSymbol) {
        const Symbol& entry = SymbolAt(symbol);
        switch (entry.kind) {
        case SymbolKind::Alias:
            AddTopLevel(entry.index, LspRankMember, LspPlace::Scope);
            return;
        case SymbolKind::Enum:
        case SymbolKind::EngineType:
        case SymbolKind::PreludeType:
        case SymbolKind::PreludeValue:
        case SymbolKind::BuiltinType:
        case SymbolKind::Class:
        case SymbolKind::Struct:
        case SymbolKind::Interface:
            AddStaticMembers(symbol);
            return;
        case SymbolKind::ContextName:
            AddChainMembers(TypeSymbolOf(entry.type), entry.type, LspMembers::All);
            return;
        default:
            break;
        }
    }

    const uint32 type = compilation_.ExpressionType(module_, object);
    if (type != NoType) {
        AddChainMembers(TypeSymbolOf(type), type, LspMembers::All);
    }
}

// 타입 경로(`Kit.|`·`|`)다: 첫 이름이면 보이는 타입, 별칭 뒤면 그 모듈의 타입.
void LspCompletionCollector::AddPathType(uint32 path, LspPlace place) noexcept {
    const Span<const uint32> names = tree_->Children(path);
    uint32 index = 0;
    while (index < names.Size() && names[index] != holeNode_) {
        ++index;
    }

    if (index == 0) {
        AddScope(place);
        return;
    }

    if (index != 1 || names[0] == NoNode || tree_->Node(names[0]).token == NoToken) {
        return;
    }

    const StringView first = TokenText(tree_->Node(names[0]).token);
    const Span<const Symbol> symbols = compilation_.Symbols();
    for (uint32 at = 0; at < symbols.Size(); ++at) {
        if (symbols[at].kind == SymbolKind::Alias && symbols[at].module == module_ &&
            symbols[at].name == first) {
            AddTopLevel(symbols[at].index, LspRankMember, place);
        }
    }
}

// `on |`·`on A.|`·`on A.B.|`(SL 3.12절의 원천 표).
void LspCompletionCollector::AddHandlerPath(Span<const uint32> names) noexcept {
    const uint32 owner = EnclosingType();
    if (owner == NoSymbol || (SymbolAt(owner).flags & SymbolFlags::Lifecycle) == 0 ||
        names.Size() > MaxLspHandlerPathNames) {
        return; // 수명 class 밖의 handler는 S0804, 구간 넷 이상은 S0807
    }

    const bool actor = EngineAncestor(owner, LspActorTypeName) != NoSymbol;
    const uint32 world = ContextSymbol(LspWorldName);
    if (names.IsEmpty()) {
        // 구간 하나는 hook이다(자기 이벤트는 받지 않는다 — S0803). 경로 첫 구간:
        // 필드·`this`·`World`.
        for (const LanguageHook& hook : LanguageHooks()) {
            if (!(core_ && hook.fullOnly)) {
                AddWord(LspCompletionSource::Hook, hook.name, HookParameterText(hook.parameters),
                        LspItemFunction, LspRankMember);
            }
        }

        AddChainMembers(owner, NoType, LspMembers::Fields);
        if (actor) {
            AddWord(LspCompletionSource::Keyword, LspThisKeyword, StringView(), LspItemKeyword,
                    LspRankMember);
        }

        if (world != NoSymbol) {
            AddSymbol(world, LspRankMember);
        }

        return;
    }

    const Span<const Token> tokens = tree_->Tokens();
    if (tokens[names[0]].kind == TokenKind::This) {
        // `this.E`: Actor 파생 class의 엔진 Actor 이벤트와 확장 이벤트(S0807·S0808).
        if (actor && names.Size() == 1) {
            AddChainMembers(owner, NoType, LspMembers::EngineEvents);
        }

        return;
    }

    const StringView first = TokenText(names[0]);
    const uint32 field = FindInChain(owner, first);
    if (field == NoSymbol || !LspIsFieldKind(SymbolAt(field).kind)) {
        // `World.E`: 엔진 World 이벤트와 World 확장 이벤트.
        if (first == LspWorldName && world != NoSymbol && names.Size() == 1) {
            const uint32 type = SymbolAt(world).type;
            AddChainMembers(TypeSymbolOf(type), type, LspMembers::Events);
        }

        return;
    }

    const uint32 type = SymbolAt(field).type;
    const uint32 symbol = TypeSymbolOf(type);
    if (names.Size() == 2) {
        // `F.G.Changed`만(F는 객체 타입 — `F.G.E`는 S0807).
        const uint32 inner =
            IsObjectSymbol(symbol) ? FindInChain(symbol, TokenText(names[1])) : NoSymbol;
        if (inner != NoSymbol && LspIsFieldKind(SymbolAt(inner).kind) && HasChanged(inner)) {
            AddWord(LspCompletionSource::Changed, LspChangedName, StringView(), LspItemEvent,
                    LspRankMember);
        }

        return;
    }

    if (HasChanged(field)) {
        AddWord(LspCompletionSource::Changed, LspChangedName, StringView(), LspItemEvent,
                LspRankMember);
    }

    // 원소가 객체인 `Array<T>`는 T의 이벤트, 객체 타입은 그 이벤트와 `Changed`가 있는 필드, 그 밖은
    // 그 타입의 언어 이벤트(`InputAction.Triggered`).
    if (symbol != NoSymbol && SymbolAt(symbol).kind == SymbolKind::PreludeType &&
        SymbolAt(symbol).name == LspArrayTypeName) {
        const TypeEntry& entry = compilation_.Types()[type];
        const uint32 element =
            entry.argumentCount == 0 ? NoType : compilation_.TypeArguments()[entry.firstArgument];
        if (IsObjectSymbol(TypeSymbolOf(element))) {
            AddChainMembers(TypeSymbolOf(element), element, LspMembers::Events);
        }

        return;
    }

    AddChainMembers(symbol, type, LspMembers::Events);
    if (IsObjectSymbol(symbol)) {
        AddChainMembers(symbol, type, LspMembers::ChangedFields);
    }
}

// `emit |`·`emit this.|`·`emit t.|`(SL 3.12절): 이 class가 선언한 이벤트, Actor 파생이면 `this`,
// 발행 대상이 될 이름(Actor 파생·World 타입의 지역·매개변수·필드), 그 대상의 확장 이벤트.
void LspCompletionCollector::AddEmitTargets(Span<const uint32> names) noexcept {
    const uint32 owner = EnclosingType();
    if (owner == NoSymbol || names.Size() > 1) {
        return;
    }

    const Span<const Symbol> symbols = compilation_.Symbols();
    const uint32 actor = EngineAncestor(owner, LspActorTypeName);
    // 발행 대상 타입 → 확장 이벤트 주인(Actor 파생이면 엔진 Actor, World면 엔진 World).
    auto targetOf = [this](uint32 type) noexcept {
        if (type == NoType || type >= compilation_.Types().Size() ||
            compilation_.Types()[type].optional) {
            return NoSymbol; // `T?`는 S0412
        }

        const uint32 symbol = TypeSymbolOf(type);
        const uint32 found = EngineAncestor(symbol, LspActorTypeName);
        if (found != NoSymbol) {
            return found;
        }

        return EngineAncestor(symbol, LspWorldName);
    };
    if (names.IsEmpty()) {
        for (uint32 at = 0; at < symbols.Size(); ++at) {
            if (symbols[at].owner == owner && symbols[at].kind == SymbolKind::Event) {
                AddSymbol(at, LspRankMember);
            }
        }

        if (actor != NoSymbol) {
            AddWord(LspCompletionSource::Keyword, LspThisKeyword, StringView(), LspItemKeyword,
                    LspRankMember);
        }

        for (uint32 at = 0; at < symbols.Size(); ++at) {
            if (IsVisibleLocal(at) && targetOf(symbols[at].type) != NoSymbol) {
                AddSymbol(at, LspRankLocal);
            }
        }

        uint32 type = owner;
        for (uint32 step = 0; type != NoSymbol && step < symbols.Size(); ++step) {
            for (uint32 at = 0; at < symbols.Size(); ++at) {
                if (symbols[at].owner == type && LspIsFieldKind(symbols[at].kind) &&
                    targetOf(symbols[at].type) != NoSymbol) {
                    AddSymbol(at, LspRankMember);
                }
            }

            type = symbols[type].base;
        }

        return;
    }

    if (tree_->Tokens()[names[0]].kind == TokenKind::This) {
        AddExtensionEvents(actor);
        return;
    }

    const uint32 named = FindNamed(owner, TokenText(names[0]));
    if (named != NoSymbol) {
        AddExtensionEvents(targetOf(symbols[named].type));
    }
}

// `import |`·`import A.|`: 같은 패키지와 의존 패키지 모듈의 다음 segment다.
void LspCompletionCollector::AddImportPath(Span<const uint32> names) noexcept {
    const uint32 package = compilation_.ModulePackage(module_);
    if (package == NoPackage) {
        return;
    }

    const Span<const Token> tokens = tree_->Tokens();
    const StringView text = sources_.Get(compilation_.ModuleSource(module_)).Text();
    const uint32 start = names.IsEmpty() ? holeBegin_ : tokens[names[0]].span.begin;
    const StringView before(text.Data() + start, holeBegin_ - start);
    StringBuilder<LspTypedPathBytes> typed(allocator_);
    for (usize at = 0; at < names.Size(); ++at) {
        if (tokens[names[at]].kind != TokenKind::Identifier) {
            return;
        }

        typed.Append(TokenText(names[at])).Append(LspPathDot);
    }

    if (typed.HasFailed()) {
        error_ = typed.Status().Error();
        return;
    }

    const PackageManifest& manifest = compilation_.Manifest(package);
    uint32 target = package;
    StringView rest = typed.View();
    if (names.IsEmpty()) {
        for (const ManifestName& dependency : manifest.Dependencies()) {
            AddWord(LspCompletionSource::Module, manifest.Text(dependency), before, LspItemModule,
                    LspRankModule);
        }
    } else {
        const StringView first = TokenText(names[0]);
        for (const ManifestName& dependency : manifest.Dependencies()) {
            if (manifest.Text(dependency) == first) {
                target = compilation_.FindPackage(first);
                rest = StringView(typed.View().Data() + first.ByteCount() + 1,
                                  typed.ByteCount() - first.ByteCount() - 1);
            }
        }
    }

    if (target == NoPackage) {
        return;
    }

    for (uint32 module = 0; module < compilation_.ModuleCount(); ++module) {
        if (compilation_.ModulePackage(module) == target && module != module_) {
            AddImportSegment(compilation_.ModuleImportPath(module), rest, before);
        }
    }
}

void LspCompletionCollector::AddImportSegment(StringView path, StringView typed,
                                              StringView before) noexcept {
    if (path.IsEmpty() || path.ByteCount() <= typed.ByteCount() ||
        !path.StartsWith(typed, StringComparison::CaseSensitive)) {
        return;
    }

    usize end = typed.ByteCount();
    while (end < path.ByteCount() && path.Data()[end] != LspPathSeparator) {
        ++end;
    }

    const StringView segment(path.Data() + typed.ByteCount(), end - typed.ByteCount());
    AddWord(LspCompletionSource::Module, segment, before, LspItemModule, LspRankModule);
}

void LspCompletionCollector::AddKeywords() noexcept {
    for (uint32 at = 0; at < LanguageKeywordCount(); ++at) {
        const LanguageWord word = LanguageKeywordAt(at);
        if (!(core_ && word.fullOnly)) {
            AddWord(LspCompletionSource::Keyword, word.name, StringView(), LspItemKeyword,
                    LspRankKeyword);
        }
    }
}

Result<void> LspCompletionCollector::Run() noexcept {
    tree_ = compilation_.ModuleTree(module_);
    if (tree_ == nullptr || hole_ >= tree_->Tokens().Size()) {
        return Result<void>::Success();
    }

    const uint32 package = compilation_.ModulePackage(module_);
    core_ = package != NoPackage && compilation_.Manifest(package).GetProfile() == Profile::Core;
    const Span<const Token> tokens = tree_->Tokens();
    holeBegin_ = tokens[hole_].span.begin;

    // 노드 → 부모, 구멍 토큰을 가진 노드(Member·Identifier·Name 순으로 가깝다).
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

        const SyntaxNode& entry = tree_->Node(node);
        if (entry.token == hole_ &&
            (entry.kind == SyntaxKind::Member || entry.kind == SyntaxKind::Identifier ||
             entry.kind == SyntaxKind::Name)) {
            holeNode_ = holeNode_ == NoNode || entry.kind == SyntaxKind::Member ? node : holeNode_;
        }
    }

    // 앞 토큰 열 `이름 . 이름 . |`과 그 앞 토큰으로 자리를 정한다.
    Array<uint32> names(allocator_);
    uint32 at = hole_;
    while (
        at >= 2 && tokens[at - 1].kind == TokenKind::Dot &&
        (tokens[at - 2].kind == TokenKind::Identifier || tokens[at - 2].kind == TokenKind::This)) {
        if (auto pushed = names.TryPushBack(at - 2); !pushed) {
            return pushed;
        }

        at -= 2;
    }

    for (usize left = 0, right = names.Size(); left + 1 < right; ++left, --right) {
        const uint32 swap = names[left];
        names[left] = names[right - 1];
        names[right - 1] = swap;
    }

    const TokenKind lead = at >= 1 ? tokens[at - 1].kind : TokenKind::EndOfFile;
    if (names.IsEmpty() && lead == TokenKind::At) {
        for (uint32 index = 0; index < LanguageAttributeCount(); ++index) {
            const LanguageWord word = LanguageAttributeAt(index);
            if (!(core_ && word.fullOnly)) {
                AddWord(LspCompletionSource::Attribute, word.name, StringView(), LspItemKeyword,
                        LspRankMember);
            }
        }
    } else if (lead == TokenKind::On) {
        AddHandlerPath(names.View());
    } else if (lead == TokenKind::Emit) {
        AddEmitTargets(names.View());
    } else if (lead == TokenKind::Import) {
        AddImportPath(names.View());
    } else if (holeNode_ == NoNode || tree_->Node(holeNode_).kind == SyntaxKind::Identifier) {
        AddScope(LspPlace::Scope);
    } else if (tree_->Node(holeNode_).kind == SyntaxKind::Member) {
        AddMember(holeNode_);
    } else {
        // 이름 노드: 타입 경로면 타입, 선언 자리의 새 이름이면 후보가 없다.
        const uint32 path = Parent(holeNode_);
        const uint32 holder = Parent(path);
        if (path != NoNode && tree_->Node(path).kind == SyntaxKind::Path && holder != NoNode) {
            const SyntaxKind kind = tree_->Node(holder).kind;
            const uint32 declaration = Parent(holder);
            if (kind == SyntaxKind::Type || kind == SyntaxKind::CastTest) {
                AddPathType(path, LspPlace::Types);
            } else if (kind == SyntaxKind::List && declaration != NoNode &&
                       tree_->Node(declaration).kind == SyntaxKind::Class &&
                       tree_->Child(declaration, LspClassBasesChild) == holder) {
                declaring_ =
                    compilation_.NodeSymbol(module_, tree_->Child(declaration, LspClassNameChild));
                const bool first =
                    tree_->Node(holder).childCount != 0 && tree_->Child(holder, 0) == path;
                AddPathType(path, first ? LspPlace::FirstBase : LspPlace::LaterBase);
            }
        }
    }

    return error_ == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error_);
}

} // namespace

Result<bool> LspCollectCompletions(Allocator& allocator, const Compilation& compilation,
                                   const SourceSet& sources, uint32 module, uint32 hole,
                                   StringView prefix, Array<LspCompletionItem>& items) noexcept {
    LspCompletionCollector collector(allocator, compilation, sources, module, hole, prefix, items);
    if (auto collected = collector.Run(); !collected) {
        return Result<bool>::Failure(collected.Error());
    }

    // 이름(byte) → 종류 → Unity 이름 → 순위 → 넣은 순서로 정렬해 label·종류·Unity 이름이 같은 것은
    // 하나만 남긴다(가까운 순위, 기반 체인에서 가까운 것 — 재정의·같은 모듈을 두 경로로 import).
    auto sameName = [](const LspCompletionItem& left, const LspCompletionItem& right) noexcept {
        if (const int label = StringView::CompareBytes(left.label, right.label); label != 0) {
            return label;
        }

        if (left.kind != right.kind) {
            return left.kind < right.kind ? -1 : 1;
        }

        return StringView::CompareBytes(left.filterText, right.filterText);
    };
    algo::Sort(items.View(), [&sameName](const LspCompletionItem& left,
                                         const LspCompletionItem& right) noexcept {
        if (const int name = sameName(left, right); name != 0) {
            return name < 0;
        }

        return left.rank != right.rank ? left.rank < right.rank : left.order < right.order;
    });
    usize kept = 0;
    for (usize at = 0; at < items.Size(); ++at) {
        if (kept == 0 || sameName(items[kept - 1], items[at]) != 0) {
            items[kept++] = items[at];
        }
    }

    // 보이는 순서: 순위 → 이름 → 종류 → Unity 이름.
    algo::Sort(Span<LspCompletionItem>(items.Data(), kept),
               [&sameName](const LspCompletionItem& left, const LspCompletionItem& right) noexcept {
                   if (left.rank != right.rank) {
                       return left.rank < right.rank;
                   }

                   return sameName(left, right) < 0;
               });

    const bool incomplete = kept > MaxLspCompletionItems;
    const usize count = incomplete ? MaxLspCompletionItems : kept;
    while (items.Size() > count) {
        static_cast<void>(items.TryPopBack());
    }

    return Result<bool>::Success(incomplete);
}

Result<bool> LspInCommentOrString(Allocator& allocator, const SourceSet& sources, SourceId id,
                                  uint32 offset) noexcept {
    Array<Token> tokens(allocator);
    Array<SourceSpan> comments(allocator);
    DiagnosticList diagnostics(allocator);
    if (auto lexed = TokenizeWithComments(sources, id, diagnostics, tokens, comments); !lexed) {
        return Result<bool>::Failure(lexed.Error());
    }

    const StringView text = sources.Get(id).Text();
    auto startsWith = [&text](SourceSpan span, StringView marker) noexcept {
        return span.end - span.begin >= marker.ByteCount() &&
               StringView(text.Data() + span.begin, marker.ByteCount()) == marker;
    };
    // 줄 주석은 줄 끝(범위 끝)에서도 안이고, 닫힌 블록 주석은 `*/` 뒤가 밖이다.
    for (const SourceSpan comment : comments) {
        if (comment.begin < offset &&
            (offset < comment.end ||
             (offset == comment.end && startsWith(comment, LspLineCommentMarker)))) {
            return Result<bool>::Success(true);
        }
    }

    // 문자열 중첩(문자열 안 보간 안 문자열 …)을 커서 앞 토큰으로 따라간다.
    constexpr uint8 InString = 0;
    constexpr uint8 InInterpolation = 1;
    Array<uint8> modes(allocator);
    for (usize at = 0; at < tokens.Size(); ++at) {
        const Token& token = tokens[at];
        const bool straddles = token.span.begin < offset && offset < token.span.end;
        switch (token.kind) {
        case TokenKind::StringText:
            if (straddles) {
                return Result<bool>::Success(true);
            }

            break;
        case TokenKind::DocComment:
            if (straddles || (token.span.begin < offset && offset == token.span.end)) {
                return Result<bool>::Success(true);
            }

            break;
        case TokenKind::VerbatimString: {
            // 닫히지 않은 verbatim 문자열은 끝 자리도 안이다(뒤에 빈 Invalid 토큰이 붙는다).
            const bool unclosed = at + 1 < tokens.Size() &&
                                  tokens[at + 1].kind == TokenKind::Invalid &&
                                  tokens[at + 1].span.begin == token.span.end &&
                                  tokens[at + 1].span.end == token.span.end;
            if (straddles || (unclosed && offset == token.span.end)) {
                return Result<bool>::Success(true);
            }

            break;
        }
        case TokenKind::Invalid:
            // 닫히지 않은 블록 주석(`/*` 범위)은 파일 끝까지 주석이다.
            if (startsWith(token.span, LspBlockCommentMarker) && token.span.end <= offset) {
                return Result<bool>::Success(true);
            }

            break;
        default:
            break;
        }

        if (token.span.end > offset || token.kind == TokenKind::EndOfFile) {
            continue;
        }

        if (token.kind == TokenKind::StringBegin || token.kind == TokenKind::InterpolationBegin) {
            const uint8 mode = token.kind == TokenKind::StringBegin ? InString : InInterpolation;
            if (auto pushed = modes.TryPushBack(mode); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }
        } else if ((token.kind == TokenKind::StringEnd && !modes.IsEmpty() &&
                    // 닫히지 않은 문자열의 빈 StringEnd 자리(줄 끝)는 아직 안이다.
                    !(token.span.begin == token.span.end && token.span.end == offset) &&
                    modes[modes.Size() - 1] == InString) ||
                   (token.kind == TokenKind::InterpolationEnd && !modes.IsEmpty() &&
                    modes[modes.Size() - 1] == InInterpolation)) {
            static_cast<void>(modes.TryPopBack());
        }
    }

    return Result<bool>::Success(!modes.IsEmpty() && modes[modes.Size() - 1] == InString);
}

Result<void> LspAppendCompletionDetail(const Compilation& compilation,
                                       const LspCompletionItem& item,
                                       StringBuilderBase& out) noexcept {
    if (!item.filterText.IsEmpty()) {
        out.Append("Unity: ").Append(item.filterText);
    } else {
        switch (item.source) {
        case LspCompletionSource::Symbol:
            return LspAppendDeclaration(compilation, item.symbol, out);
        case LspCompletionSource::Hook:
            out.Append("on ").Append(item.label).Append(item.text);
            break;
        case LspCompletionSource::Attribute:
            out.Append("@").Append(item.label);
            break;
        case LspCompletionSource::Keyword:
            out.Append("keyword");
            break;
        case LspCompletionSource::Module:
            out.Append("import ").Append(item.text).Append(item.label);
            break;
        case LspCompletionSource::Changed:
            out.Append("event ").Append(item.label);
            break;
        }
    }

    return out.HasFailed() ? Result<void>::Failure(out.Status().Error()) : Result<void>::Success();
}

} // namespace rocks::sparks::tools::detail
