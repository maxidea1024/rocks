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
