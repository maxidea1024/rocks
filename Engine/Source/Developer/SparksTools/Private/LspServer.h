#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Compilation.h>
#include <SparksCompiler/Symbols.h>
#include <SparksCompiler/SyntaxTree.h>

// 언어 서버 내부 공용이다(Docs/Design/SparksTools.ko.md 10절·11절). 파일:
//   LspFraming.cpp        Content-Length 틀
//   LspText.cpp           UTF-16 위치·file URI
//   LspServer.cpp         수명·문서 저장소·분석·진단 발행·요청 분기·서식
//   LspSemanticTokens.cpp 의미 강조(T12)
//   LspQueries.cpp        커서의 이름·hover·정의 이동(11.2–11.4절)
//   LspCompletion.cpp     자동완성 후보(11.6절)
//   LspCodeActions.cpp    빠른 수정 편집(11.7절)
namespace rocks::sparks::tools::detail {

// 의미 강조 legend다(10.7절 — 순서가 번호다).
inline constexpr StringView LspTokenTypes[] = {
    "namespace",  "type",  "class",    "struct", "enum",     "interface", "typeParameter",
    "enumMember", "event", "function", "method", "property", "parameter", "variable"};
inline constexpr StringView LspTokenModifiers[] = {"declaration", "readonly", "static", "async",
                                                   "defaultLibrary"};

// compilation 모듈의 의미 강조 data(토큰마다 줄 차이·시작 차이·길이·종류·modifier bit — UTF-16)를
// data 뒤에 붙인다. 구문 트리가 없는 모듈은 아무것도 붙이지 않는다. 실패: 할당 실패.
Result<void> LspAppendSemanticTokens(const Compilation& compilation, const SourceSet& sources,
                                     uint32 module, Array<uint32>& data) noexcept;

// 원문의 이름 토큰 하나와 그 대상이다(11.2절 — 의미 강조와 위치 질의가 같은 규칙을 쓴다).
struct LspNamed {
    uint32 token = NoToken;   // 이름 토큰(구문 트리 토큰 번호)
    uint32 node = NoNode;     // 이름을 가진 노드
    uint32 symbol = NoSymbol; // 대상 심볼
    uint32 module = NoModule; // import 경로 이름이면 그 경로가 가리키는 모듈(경로의 이름마다)
    bool import = false;      // import 경로의 이름이다
};

// 노드 하나가 가진 이름을 visit(const LspNamed&) → Result<void>로 넘긴다: Name·Identifier·Member는
// NodeSymbol, Type·기반 Path는 경로 마지막 이름에 그 타입의 심볼(이름 노드에 심볼이 없을 때),
// Import는 경로의 이름마다(심볼이 없을 때). 심볼도 import도 아닌 이름은 넘기지 않는다.
template <typename Visit>
Result<void> LspVisitNames(const Compilation& compilation, uint32 module, uint32 node,
                           Visit&& visit) noexcept;

// offset 자리의 이름이다: begin ≤ offset < end인 식별자 토큰, 없으면 end == offset인 식별자 토큰의
// 이름. 값 false는 이름이 없음이다.
bool LspNameAt(const Compilation& compilation, uint32 module, uint32 offset,
               LspNamed& named) noexcept;

// 읽기 전용 이름인지다: `let` 필드·지역, 읽기 전용 언어 속성, schema `access: readWrite`가 아닌
// 엔진 필드(schema 기본은 읽기 전용).
bool LspIsReadOnly(const Compilation& compilation, const Symbol& symbol) noexcept;

// 심볼의 선언 모양 한 줄(11.3절 표 — hover 코드 블록·자동완성 detail)을 out에 붙인다. 실패: 할당.
Result<void> LspAppendDeclaration(const Compilation& compilation, uint32 symbol,
                                  StringBuilderBase& out) noexcept;

// named의 hover 본문(markdown — 코드 블록·문서 주석·선언 위치)을 out에 붙인다. 값 false는 보일
// 것이 없음이다. 실패: 할당.
Result<bool> LspAppendHover(const Compilation& compilation, const SourceSet& sources,
                            const LspNamed& named, StringBuilderBase& out) noexcept;

// named의 정의 자리다(11.4절): 원문 파일과 범위. 값 false는 정의 자리가 없음(언어·prelude·문맥
// 이름·해석하지 못한 import)이다.
bool LspDefinition(const Compilation& compilation, const LspNamed& named, SourceId& source,
                   SourceSpan& span) noexcept;

// 자동완성 구멍 식별자다(11.6절 — 커서 자리에 넣어 snapshot을 컴파일한다).
inline constexpr StringView LspCompletionHole = "SparksCompletionHole";

// 자동완성 결과 상한이다(넘으면 isIncomplete).
inline constexpr usize MaxLspCompletionItems = 1000;

// 자동완성 후보의 출처다(detail 모양이 다르다).
enum class LspCompletionSource : uint8 {
    Symbol,    // 심볼(선언 모양)
    Hook,      // 수명 hook(`on Tick(dt: float)`)
    Attribute, // attribute(`@edit`)
    Keyword,   // 키워드
    Module,    // import 경로 segment(`import Tools.Kit`)
    Changed    // `on 필드.Changed`
};

// 자동완성 후보 하나다. 글자는 compilation·원문·정적 표를 가리킨다(그것들이 사는 동안 유효).
struct LspCompletionItem {
    StringView label;
    StringView filterText; // Unity 이름(그 이름으로 찾는 같은 항목 — 아니면 빈 글자)
    StringView text;       // Hook은 인자 표기, Module은 앞 경로(`Tools.`)
    uint32 symbol = NoSymbol;
    uint32 order = 0; // 모은 순서(같은 이름 사이의 결정적 순서)
    uint8 kind = 0;   // LSP CompletionItemKind
    uint8 rank = 0;   // 가까움 순위(sortText 첫 글자)
    LspCompletionSource source = LspCompletionSource::Symbol;
};

// snapshot compilation의 module에서 구멍 토큰(hole)의 자동완성 후보를 접두(prefix — ASCII 대소문자
// 무시)로 걸러 정렬해 items에 둔다(11.6절). 값은 상한으로 잘랐는지(isIncomplete)다. 실패: 할당.
Result<bool> LspCollectCompletions(Allocator& allocator, const Compilation& compilation,
                                   const SourceSet& sources, uint32 module, uint32 hole,
                                   StringView prefix, Array<LspCompletionItem>& items) noexcept;

// 빠른 수정 하나다(11.7절 — 편집 하나: 원문 byte 범위 [begin, end)를 text로 바꾼다).
struct LspFix {
    explicit LspFix(Allocator& allocator) noexcept : title(allocator), text(allocator) {
    }

    String title; // 영어(실행 중 문구)
    uint32 begin = 0;
    uint32 end = 0;
    String text;
};

// compilation의 module 진단 하나에 대한 빠른 수정을 fixes 뒤에 붙인다(11.7절 표 Q1–Q10 — 없으면
// 붙이지 않는다). 실패: 할당.
Result<void> LspCollectFixes(Allocator& allocator, const Compilation& compilation,
                             const SourceSet& sources, uint32 module, const Diagnostic& diagnostic,
                             Array<LspFix>& fixes) noexcept;

// 식별자 글자(ASCII 영문·숫자·`_`)인지다(자동완성 접두).
constexpr bool LspIsNameByte(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_';
}

// id 파일의 offset이 주석·문서 주석·문자열 조각(보간 밖)·verbatim 문자열 안인지다(11.6절 — 그
// 자리는 자동완성 후보가 없다). 파일은 IsUsable()이어야 한다. 실패: 할당.
Result<bool> LspInCommentOrString(Allocator& allocator, const SourceSet& sources, SourceId id,
                                  uint32 offset) noexcept;

// 후보의 detail 한 줄을 out에 붙인다. 실패: 할당.
Result<void> LspAppendCompletionDetail(const Compilation& compilation,
                                       const LspCompletionItem& item,
                                       StringBuilderBase& out) noexcept;

template <typename Visit>
Result<void> LspVisitNames(const Compilation& compilation, uint32 module, uint32 node,
                           Visit&& visit) noexcept {
    const SyntaxTree* tree = compilation.ModuleTree(module);
    const SyntaxNode& entry = tree->Node(node);
    switch (entry.kind) {
    case SyntaxKind::Name:
    case SyntaxKind::Identifier:
    case SyntaxKind::Member: {
        LspNamed named;
        named.token = entry.token;
        named.node = node;
        named.symbol = compilation.NodeSymbol(module, node);
        if (named.symbol == NoSymbol || named.token == NoToken) {
            return Result<void>::Success();
        }

        return visit(named);
    }
    case SyntaxKind::Type:
    case SyntaxKind::Path: {
        const uint32 type = compilation.NodeType(module, node);
        const uint32 path = entry.kind == SyntaxKind::Path ? node : tree->Child(node, 0);
        if (type == NoType || type >= compilation.Types().Size() || path == NoNode ||
            tree->Node(path).kind != SyntaxKind::Path || tree->Node(path).childCount == 0) {
            return Result<void>::Success();
        }

        const Span<const uint32> names = tree->Children(path);
        const uint32 last = names[names.Size() - 1];
        LspNamed named;
        named.token = tree->Node(last).token;
        named.node = last;
        named.symbol = compilation.Types()[type].symbol;
        if (named.symbol == NoSymbol || named.token == NoToken ||
            compilation.NodeSymbol(module, last) != NoSymbol) {
            return Result<void>::Success();
        }

        return visit(named);
    }
    case SyntaxKind::Import: {
        const uint32 path = tree->Child(node, 0);
        if (path == NoNode || tree->Node(path).kind != SyntaxKind::Path) {
            return Result<void>::Success();
        }

        uint32 target = NoModule;
        for (const ImportResolution& resolution : compilation.ModuleImports(module)) {
            target = resolution.node == node ? resolution.target : target;
        }

        const Span<const uint32> names = tree->Children(path);
        for (usize at = 0; at < names.Size(); ++at) {
            if (names[at] == NoNode || compilation.NodeSymbol(module, names[at]) != NoSymbol) {
                continue;
            }

            LspNamed named;
            named.token = tree->Node(names[at]).token;
            named.node = names[at];
            named.import = true;
            named.module = target;
            if (named.token == NoToken) {
                continue;
            }

            if (auto visited = visit(named); !visited) {
                return visited;
            }
        }

        return Result<void>::Success();
    }
    default:
        return Result<void>::Success();
    }
}

} // namespace rocks::sparks::tools::detail
