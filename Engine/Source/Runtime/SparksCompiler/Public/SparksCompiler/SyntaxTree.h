#pragma once

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksCompiler/Token.h>

// Sparks 구문 트리와 구문 분석이다(Docs/Design/SparksLanguage.ko.md 3.6절 문법, 8.1절 "구문 분석"·
// "복구", S02xx).
//
// 사용 예:
//   SyntaxTree tree(allocator);
//   auto parsed = ParseFile(sources, id, diagnostics, tree);   // 어휘 분석 + 구문 분석
//   StringBuilder<1024> dump(allocator);
//   auto written = AppendSyntaxDump(dump, sources, tree);      // 검증용 텍스트(SD58)
//
// 구조: 노드는 배열에 있고 번호(uint32)로 가리킨다. 노드마다 종류·flag·대표 토큰·원문 범위·자식
// 목록을 가진다. 자식의 자리와 수는 종류마다 정해져 있고(아래 SyntaxKind 설명, 구문 오류가 있어도
// 같다), 없는 선택 자식은 NoNode다. 구문 오류를 복구한 자리에는 어느 자식 자리든 Error 노드가
// 들어갈 수 있다. 의미 규칙(이름 해석·타입·문맥)은 뒤 단계가 판정한다.
//
// 깊이: 재귀하는 구문의 중첩은 MaxSyntaxNesting 단계까지다(단계를 세는 법은 그 상수 설명, 넘으면
// S0215를 내고 파일의 나머지를 분석하지 않는다). 트리 전체 깊이(연산자 사슬 `a + b + c`·`a.b.c`의
// 연산자도 한 단계씩)는 MaxSyntaxTreeDepth까지다(넘으면 끝까지 분석한 뒤 가장 깊은 경로에서 상한 +
// 1 단계 노드의 위치에 S0215, 가장 깊은 자식이 여럿이면 앞의 것). S0215가 없는 트리는 뒤 단계가
// 재귀로 걸어도 된다. S0215가 난 트리는 뒤 단계로 넘기지 않는다.
//
// 범위: 부모 노드의 범위는 자식 노드의 범위를 모두 포함하고, 형제 노드의 시작 위치는 순서대로다.
//
// 수명·스레드: 트리는 생성 allocator를 빌린다. 원문(SourceSet)은 덤프·토큰 글자를 읽을 때만
// 필요하다. 한 트리는 한 스레드에서 만든다. 다 만든 트리는 여러 스레드가 읽어도 된다.

namespace rocks::sparks {

// 선택 자식이 없음을 나타낸다.
inline constexpr uint32 NoNode = NumericLimits<uint32>::Max();

// 대표 토큰이 없음을 나타낸다.
inline constexpr uint32 NoToken = NumericLimits<uint32>::Max();

// 재귀 구문의 중첩 상한이다(S0215). 단계는 문장 하나, 식 하나(괄호·대괄호·호출 인자·인덱스·보간
// 안의 식마다 새로 하나), 단항 연산자 하나, `??`의 오른쪽 하나, try·await 하나, 타입 하나(타입
// 인자마다 새로 하나), `else if` 하나마다 늘어난다(Docs/Design/SparksLanguage.ko.md 8.1절 "깊이
// 상한"). 한 단계가 parser 함수 프레임 여러 개(식 한 단계에 약 12개)를 쓰므로 Debug 빌드에서도 기본
// stack(1 MiB) 안에 들도록 정한 값이다. 넘으면 그 파일의 나머지는 분석하지 않는다.
inline constexpr uint32 MaxSyntaxNesting = 128;

// 구문 트리 깊이 상한이다(File 노드가 1단계, 뿌리에서 가장 먼 노드까지의 노드 수, S0215). 연산자
// 사슬은 parser stack을 쓰지 않지만 트리를 깊게 만들므로 따로 막는다. 뒤 단계가 노드 한 단계에
// 프레임 하나를 써도 Debug 빌드의 기본 stack 안에 들도록 정한 값이다.
inline constexpr uint32 MaxSyntaxTreeDepth = 1024;

// 노드 종류와 자식 배치다. "Doc"은 DocComment 또는 NoNode, "Attrs"는 Attribute 노드의 List다.
enum class SyntaxKind : uint8 {
    Error,               // 복구로 채운 자리. 자식 없음
    File,                // [List(Import), List(선언)]
    List,                // 자식이 곧 원소다
    Name,                // 선언 자리의 이름. token = 식별자
    Discard,             // `_`. token
    Path,                // [Name...] (`Weapons.Sword`)
    DocComment,          // token = 첫 `///` 토큰, extra = 이어진 줄 수
    Attribute,           // token = 이름 토큰. [List(Argument)] (괄호가 없으면 빈 List)
    Argument,            // [Name 또는 NoNode, 식] (attribute 인자·호출 인자)
    Import,              // [Path, 별칭 Name 또는 NoNode]
    Class,               // flag Open. [Doc, Attrs, Name, List(기반 Path), List(멤버)]
    Interface,           // [Doc, Attrs, Name, List(InterfaceMethod)]
    InterfaceMethod,     // flag Async. [Doc, Attrs, Name, List(Parameter), 반환 Type 또는 NoNode]
    Struct,              // [Doc, Attrs, Name, List(멤버)]
    Enum,                // [Doc, Attrs, Name, List(EnumCase)]
    EnumCase,            // [Doc, Attrs, Name]
    ExtensionEvent,      // [Doc, Attrs, 대상 Name, 이벤트 Name, List(Parameter)]
    Const,               // [Doc, Attrs, Name, Type, 식]
    Function,            // flag Open·Override·Async. [Doc, Attrs, Name, List(Parameter), Type 또는
                         // NoNode, Block]
    Field,               // flag Let. [Doc, Attrs, Name, Type 또는 NoNode, 초기값 식 또는 NoNode]
    Event,               // [Doc, Attrs, Name, List(Parameter)]
    Handler,             // [Doc, Attrs, EventPath, List(Parameter), Block]
    EventPath,           // [Name 또는 This ...]
    Parameter,           // [Name 또는 Discard, Type]
    Type,                // flag Optional. [Path, List(타입 인자 Type) 또는 NoNode]
    Block,               // [문장...]
    LocalVariable,       // flag Let. [Name, Type 또는 NoNode, 값, ExitReturn 또는 NoNode]
    ExitReturn,          // [식 또는 NoNode] (`?? return e`)
    If,                  // [조건(식 또는 IfLet), Block, else(If 또는 Block) 또는 NoNode]
    IfLet,               // [Name, 식]
    For,                 // [binder(Name·Discard), binder 또는 NoNode, 식, 범위 끝 식 또는 NoNode,
                         // Block]
    While,               // [식, Block]
    Switch,              // [식, List(SwitchCase), SwitchDefault 또는 NoNode]
    SwitchCase,          // [List(값 식), Block]
    SwitchDefault,       // [Block]
    Return,              // [값 또는 NoNode]
    Emit,                // [EventPath, List(식)]
    Break,               // 자식 없음
    Continue,            // 자식 없음
    Assignment,          // token = 연산자(`=`·`+=`·`-=`·`*=`·`/=`). [대상 식, 값]
    DiscardAssignment,   // [값] (`_ = v;`)
    ExpressionStatement, // [값]
    Try,                 // flag Await. [식] (`try [await] X`)
    Await,               // [식]
    Binary,              // token = 연산자. [왼쪽, 오른쪽]
    Unary,               // token = 연산자(`!`·`-`). [피연산자]
    CastTest,            // [피연산자, Path] (`x as? T`)
    Member,              // token = 이름. flag NullConditional(`?.`). [객체, List(타입 인자) 또는
                         // NoNode]
    Call,                // [호출 대상, List(Argument)]
    Index,               // [객체, 식]
    Identifier,          // token = 이름. [List(타입 인자) 또는 NoNode]
    This,                // token
    IntegerLiteral,      // token
    FloatLiteral,        // token
    BoolLiteral,         // token(`true`·`false`)
    NullLiteral,         // token
    VerbatimString,      // token(`@"..."` 전체)
    String,              // [StringText 또는 Interpolation ...] (빈 문자열은 자식 없음)
    StringText,          // token(escape를 풀지 않은 조각)
    Interpolation,       // [식]
    Parenthesized,       // [식]
    CollectionLiteral,   // [원소 식 또는 MapEntry ...] (Array·Map 리터럴, 섞이면 S0218)
    MapEntry             // [키 식, 값 식]
};

// 노드 flag다.
struct SyntaxFlags {
    static constexpr uint32 Open = 1u << 0;
    static constexpr uint32 Override = 1u << 1;
    static constexpr uint32 Async = 1u << 2;
    static constexpr uint32 Let = 1u << 3;
    static constexpr uint32 Optional = 1u << 4;
    static constexpr uint32 NullConditional = 1u << 5;
    static constexpr uint32 Await = 1u << 6;
};

struct SyntaxNode {
    SyntaxKind kind = SyntaxKind::Error;
    uint32 flags = 0;
    uint32 token = NoToken;
    SourceSpan span;
    uint32 firstChild = 0;
    uint32 childCount = 0;
    uint32 extra = 0;
};

namespace detail {
class SyntaxTreeBuilder;
} // namespace detail

// 파일 하나의 구문 트리다. ParseFile이 채운다.
class SyntaxTree final {
public:
    explicit SyntaxTree(Allocator& allocator) noexcept;

    SyntaxTree(const SyntaxTree&) = delete;
    SyntaxTree& operator=(const SyntaxTree&) = delete;
    SyntaxTree(SyntaxTree&&) = delete;
    SyntaxTree& operator=(SyntaxTree&&) = delete;
    ~SyntaxTree() noexcept = default;

    SourceId Source() const noexcept {
        return source_;
    }

    // File 노드다. 아직 분석하지 않았으면 NoNode다.
    uint32 Root() const noexcept {
        return root_;
    }

    uint32 NodeCount() const noexcept {
        return static_cast<uint32>(nodes_.Size());
    }

    // 노드다. 범위 밖은 Fatal이다.
    const SyntaxNode& Node(uint32 id) const noexcept;

    // 자식 목록이다(NoNode가 섞일 수 있다).
    Span<const uint32> Children(uint32 id) const noexcept;

    // index번째 자식이다(NoNode일 수 있다). index가 자식 수 이상이면 Fatal이다.
    uint32 Child(uint32 id, uint32 index) const noexcept;

    // 분석에 쓴 토큰이다(문서 주석 포함, 마지막은 EndOfFile). 노드의 token이 이 배열의 번호다. 타입
    // 인자를 닫는 자리의 `>=`는 `=` 토큰(범위는 `=` 한 글자)으로 바뀌고 `>`는 그 타입 인자 노드의
    // 범위에만 남는다(3.6절의 `>=` 분리).
    Span<const Token> Tokens() const noexcept {
        return tokens_.View();
    }

private:
    friend class detail::SyntaxTreeBuilder;

    Array<SyntaxNode> nodes_;
    Array<uint32> children_;
    Array<Token> tokens_;
    SourceId source_;
    uint32 root_ = NoNode;
};

// id 파일을 어휘·구문 분석해 tree를 채운다(tree의 이전 내용은 버린다). 파일은 IsUsable()이어야 한다
// (아니면 InvalidArgument). 구문 오류는 diagnostics에 붙고 트리는 복구된 모양으로 끝까지 만든다.
// 실패: 할당 실패는 그 오류이며 tree·diagnostics의 내용은 정해지지 않는다(호출자가 버린다).
Result<void> ParseFile(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                       SyntaxTree& tree) noexcept;

// 노드 종류의 이름이다(덤프·진단용, 영어 — 예: "LocalVariable").
StringView SyntaxKindName(SyntaxKind kind) noexcept;

// 구문 트리를 검증용 텍스트로 붙인다(SD58). 형식: 한 줄에 노드 하나(LF로 끝난다), 깊이마다 공백 2칸
// 들여쓰기,
//   <종류>[ <flag>...][ "<토큰 글자>"][ lines=<n>] @<줄>:<열>-<줄>:<열>
// flag는 open·override·async·let·optional·null-conditional·await 순서다. lines는 DocComment의 줄
// 수다. 위치는 노드 범위의 시작과 끝(끝 byte 다음 자리)이다. 없는 선택 자식은 "-" 한 줄이다. 토큰
// 글자의 `\`·`"`·LF·CR·tab은 `\\`·`\"`·`\n`·`\r`·`\t`로, 나머지 제어 문자(U+0000–U+001F, U+007F)는
// `\u{XX}`로 쓴다. 결정적이다(같은 원문 → 같은 바이트).
// 실패: tree를 분석하지 않았으면(Root() == NoNode) InvalidArgument, out의 할당 실패는 그 오류.
Result<void> AppendSyntaxDump(StringBuilderBase& out, const SourceSet& sources,
                              const SyntaxTree& tree) noexcept;

} // namespace rocks::sparks
