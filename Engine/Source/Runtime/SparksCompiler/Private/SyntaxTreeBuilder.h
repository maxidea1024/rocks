#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Source.h>
#include <SparksCompiler/SyntaxTree.h>
#include <SparksCompiler/Token.h>

// Sparks 구문 분석기의 내부 선언이다(ParseFile만 쓴다, Docs/Design/SparksLanguage 3.6절 EBNF,
// 8.1절 "구문 분석"·"복구"·"문법 세부"). 재귀 하강이며 함수 하나가 EBNF 생산 규칙 하나를 맡는다.
// 구현은 기능별 파일로 나뉜다:
//   ParserSupport.cpp       토큰 읽기·노드 만들기·진단·복구·깊이 상한
//   ParseDeclarations.cpp   파일·import·선언·멤버·enum·매개변수·attribute·문서 주석
//   ParseTypes.cpp          타입·타입 인자·식 안의 타입 인자 판정
//   ParseStatements.cpp     블록과 문장
//   ParseExpressions.cpp    식·호출 인자·문자열·컬렉션 리터럴
//
// 노드 만들기: 자식을 먼저 만들어 작업 stack(stack_)에 쌓은 뒤 Make가 mark 뒤의 것을 한 번에 묶는다
// (자식 목록이 SyntaxTree::children_의 연속 구간이 되게). 종류마다 자식 수가 정해져 있으므로 parse
// 함수는 오류가 나도 모든 자리를 채운다(Error 노드 또는 NoNode).
//
// 복구(8.1절 "복구"):
//   - Report는 진단을 내고 복구 상태(panic_)가 된다. 복구 상태에서는 진단을 더 내지 않는다.
//   - ReportInPlace는 건너뛸 것이 없는 오류다(분석이 문법대로 이어지므로 복구 상태를 만들지
//   않는다).
//   - 현재 토큰이 어휘 오류 자리(Invalid 토큰, 또는 시작 위치에 어휘 진단이 있는 토큰)면 진단을
//     내지 않고 복구 상태만 된다.
//   - 문장·멤버·선언 하나를 마치면 Recover가 복구 상태를 정리한다: 마지막으로 읽은 토큰이 `;`·`}`면
//     그 자리가 동기화 지점이므로 풀기만 하고, 아니면 Sync*로 동기화 지점까지 건너뛴다. 파일 끝에
//     닿으면 복구 상태를 유지한다(빠진 `}` 따위를 같은 원인으로 다시 알리지 않게).
//   - 본문(블록·클래스·struct·interface·enum·switch)을 여는 `{`를 읽으면 복구 상태가 풀린다(머리의
//     오류와 본문 안의 오류는 원인이 다르다).
//   - 파일 끝에서는 복구 상태가 풀리지 않는다(빠진 `}`가 여럿이어도 진단은 하나).
//   - 같은 위치에는 구문 진단을 하나만 낸다(S0215 제외 — 동기화가 오류 난 토큰에서 멈춘 경우).

namespace rocks::sparks::detail {

class SyntaxTreeBuilder final {
public:
    SyntaxTreeBuilder(const SourceSet& sources, SourceId id, DiagnosticList& diagnostics,
                      SyntaxTree& tree) noexcept;

    SyntaxTreeBuilder(const SyntaxTreeBuilder&) = delete;
    SyntaxTreeBuilder& operator=(const SyntaxTreeBuilder&) = delete;

    // 어휘·구문 분석을 끝까지 한다. 실패는 할당 실패뿐이다(ParseFile 설명).
    Result<void> Run() noexcept;

private:
    // 멤버 본문의 종류다(올 수 있는 멤버가 다르다).
    enum class MemberContext : uint8 {
        Class,
        Struct,
        Interface
    };

    // 오류 뒤 건너뛰기의 단위다.
    enum class SyncLevel : uint8 {
        Statement,
        Member,
        Declaration
    };

    // 재귀 구문 한 단계다(MaxSyntaxNesting). 넘으면 S0215를 내고 파일의 나머지를 건너뛴다.
    class Nesting final {
    public:
        explicit Nesting(SyntaxTreeBuilder& builder) noexcept;
        ~Nesting() noexcept;

        Nesting(const Nesting&) = delete;
        Nesting& operator=(const Nesting&) = delete;

        // 분석을 멈췄다(이 단계에서 넘었거나 이미 멈춘 상태다).
        bool TooDeep() const noexcept;

    private:
        SyntaxTreeBuilder& builder_;
    };

    // 버릴 노드를 만들기 전의 자리다(RestoreNodes가 그 뒤의 노드를 지운다).
    struct NodeCheckpoint {
        usize nodes = 0;
        usize children = 0;
    };

    // ---- 토큰 분류(ParserSupport.cpp) ----
    static bool IsStatementKeyword(TokenKind kind) noexcept;
    static bool IsMemberStart(TokenKind kind) noexcept;
    static bool IsDeclarationStart(TokenKind kind) noexcept;
    static bool StartsValue(TokenKind kind) noexcept;
    static bool IsComparison(TokenKind kind) noexcept;
    static bool IsEquality(TokenKind kind) noexcept;
    static bool IsCompoundAssignment(TokenKind kind) noexcept;
    static bool IsExitKeyword(TokenKind kind) noexcept;
    static bool IsAssignable(SyntaxKind kind) noexcept;

    // ---- 토큰 읽기(ParserSupport.cpp) ----
    const Token& TokenAt(usize index) const noexcept;
    usize SkipDocs(usize index) const noexcept;
    // 문서 주석을 건너뛴 ahead번째 토큰의 종류다(진단을 내지 않는다).
    TokenKind PeekKind(usize ahead) const noexcept;
    // 현재 자리의 문서 주석을 S0208로 알리고 건너뛴다(붙을 선언이 없다).
    void ReportMisplacedDocs() noexcept;
    TokenKind Kind() noexcept;
    bool At(TokenKind kind) noexcept;
    // 문서 주석을 알리지 않은 현재 위치다(문서 주석이 있으면 그 시작).
    uint32 RawBegin() const noexcept;
    uint32 CurrentBegin() noexcept;
    SourceSpan CurrentSpan() noexcept;
    // 현재 토큰을 소비하고 그 번호를 돌려준다(파일 끝에서는 소비하지 않는다).
    uint32 Advance() noexcept;
    bool Accept(TokenKind kind) noexcept;
    // 진단 없이 토큰 하나(앞의 문서 주석 포함)를 건너뛴다. 복구용이다.
    void SkipOne() noexcept;
    // 현재 `{`부터 짝이 맞는 `}`까지 진단 없이 건너뛴다.
    void SkipBalancedBraces() noexcept;

    // ---- 노드(ParserSupport.cpp) ----
    usize Mark() const noexcept;
    void Push(uint32 node) noexcept;
    uint32 MakeSpan(SyntaxKind kind, usize mark, SourceSpan span, uint32 token, uint32 flags,
                    uint32 extra) noexcept;
    // 범위는 begin부터 마지막으로 읽은 토큰의 끝까지다(자식 범위가 더 뒤로 가면 그 끝까지 —
    // 부모 범위는 늘 자식 범위를 포함한다).
    uint32 Make(SyntaxKind kind, usize mark, uint32 begin, uint32 token = NoToken,
                uint32 flags = 0) noexcept;
    uint32 Leaf(SyntaxKind kind, uint32 token) noexcept;
    // 현재 자리의 빈 Error 노드다.
    uint32 MakeError() noexcept;
    // 이름·타입 자리의 오류 노드다. 이 자리의 오류가 복구 상태 밖에서 난 것(inSync)이고 현재
    // 토큰이 앞 토큰과 같은 줄의 키워드면 이름 자리에 잘못 쓴 것으로 보고 소비한다(`var class =
    // 1;`이 `class` 선언으로 다시 읽히지 않게). 다른 줄이면 새 구문의 시작일 수 있고, 복구 중이면
    // 이름 자리인지부터 확실하지 않으므로 남긴다.
    uint32 MakeNameError(bool inSync) noexcept;
    // MakeNameError가 소비할 키워드인지다(inSync이고 앞 토큰과 같은 줄의 키워드).
    bool AtMistakenKeyword(bool inSync) const noexcept;
    // offset 앞이 그 줄의 처음까지 공백·tab뿐인지다(문서 주석이 줄을 시작하는지).
    bool StartsLine(uint32 offset) const noexcept;
    NodeCheckpoint SaveNodes() const noexcept;
    void RestoreNodes(NodeCheckpoint checkpoint) noexcept;
    uint32 NodeBegin(uint32 node) const noexcept;
    SyntaxKind NodeKind(uint32 node) const noexcept;
    SourceSpan NodeSpan(uint32 node) const noexcept;
    uint32 ChildCount(uint32 node) const noexcept;

    // ---- 진단(ParserSupport.cpp) ----
    bool CanReport() const noexcept;
    // 현재 토큰이 어휘 오류 자리다: Invalid 토큰이거나 그 토큰 시작 위치에 어휘 진단이 있다.
    bool AtLexicalError() const noexcept;
    void AddDiagnostic(DiagnosticCode code, SourceSpan span, StringView message,
                       StringView explanation, StringView fix) noexcept;
    void Report(DiagnosticCode code, SourceSpan span, StringView message, StringView explanation,
                StringView fix) noexcept;
    void ReportAtCurrent(DiagnosticCode code, StringView message, StringView explanation,
                         StringView fix) noexcept;
    void ReportInPlace(DiagnosticCode code, SourceSpan span, StringView message,
                       StringView explanation, StringView fix) noexcept;
    // "Expected <what>[ <context>], found <현재 토큰>." 진단을 낸다(복구 상태가 된다).
    void ReportExpected(DiagnosticCode code, StringView what, StringView context,
                        StringView explanation, StringView fix) noexcept;
    // kind가 있으면 소비하고 true다. 없으면 S0201(그 자리에 더 맞는 S0213·S0216이 있으면 그것)을
    // 내고 소비하지 않는다. fix가 비면 "Insert <토큰>."이다.
    bool Expect(TokenKind kind, StringView context, StringView fix = StringView()) noexcept;
    void ReportMisplacedExit() noexcept;
    void ReportRangeOutsideFor() noexcept;

    // ---- 복구·깊이(ParserSupport.cpp) ----
    // 문장·멤버·선언 하나를 마친 뒤의 복구다. consumedBefore는 그 단위를 시작할 때의 consumed_다.
    void Recover(SyncLevel level, usize consumedBefore) noexcept;
    void SyncStatement() noexcept;
    void SyncMember() noexcept;
    void SyncDeclaration() noexcept;
    // 선언·멤버·문장을 시작할 수 없는 토큰을 Error 노드로 바꿔 건너뛴다. 본문을 닫는 `}`와 파일
    // 끝은 건너뛰지 않는다(topLevel이면 `}`도 건너뛴다). `{`는 짝이 맞는 `}`까지 건너뛴다.
    uint32 SkipErrorToken(bool topLevel) noexcept;
    // 중첩 상한(MaxSyntaxNesting)을 넘었다: S0215를 내고(복구 상태와 상관없이) 파일의 나머지를
    // 건너뛴다.
    void StopTooDeep(SourceSpan span) noexcept;
    // 다 만든 트리의 깊이가 MaxSyntaxTreeDepth를 넘으면 가장 깊은 경로에서 그 상한 바로 아래 단계
    // 노드의 위치에 S0215를 낸다(분석은 이미 끝까지 했다).
    void CheckTreeDepth(uint32 root) noexcept;

    // ---- 파일·선언(ParseDeclarations.cpp) ----
    uint32 ParseFileNode() noexcept;
    uint32 ParseImport() noexcept;
    uint32 ParseName(StringView context) noexcept;
    uint32 ParsePath(StringView context) noexcept;
    uint32 CollectDoc() noexcept;
    uint32 ParseAttributes() noexcept;
    uint32 ParseAttribute() noexcept;
    // 붙을 수 없는 자리의 attribute를 S0214로 알리고 버린다.
    void DiscardAttributes(StringView message) noexcept;
    uint32 ParseDeclaration() noexcept;
    uint32 ParseClass(uint32 begin, uint32 doc, uint32 attributes, uint32 flags) noexcept;
    uint32 ParseStruct(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseInterface(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseMemberBody(MemberContext context, StringView openContext,
                           StringView closeContext) noexcept;
    uint32 ParseMember(MemberContext context) noexcept;
    uint32 ParseField(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseFunction(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseInterfaceMethod(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseEvent(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseExtensionEvent(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseHandler(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseEventPath(StringView context) noexcept;
    uint32 ParseConst(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseEnum(uint32 begin, uint32 doc, uint32 attributes) noexcept;
    uint32 ParseEnumCase() noexcept;
    uint32 ParseParameters() noexcept;
    uint32 ParseParameter() noexcept;

    // ---- 타입(ParseTypes.cpp) ----
    uint32 ParseType(StringView context) noexcept;
    uint32 ParseTypeArguments() noexcept;
    // 식 안의 `<`가 타입 인자인지다(3.6절: `<` Type {, Type} `>` 다음이 `(` 또는 `.`). `<`가
    // MaxSyntaxNesting 단계 넘게 중첩되면 타입 인자로 본다(그 분석이 S0215에 걸린다).
    bool TypeArgumentsAhead() const noexcept;
    // index의 `<`부터 타입 인자 모양인지 본다. tooDeep이 서면 깊이 상한에 닿아 판정을 멈춘 것이다.
    bool ScanTypeArguments(usize& index, uint32 depth, bool& tooDeep) const noexcept;
    bool ScanType(usize& index, uint32 depth, bool& tooDeep) const noexcept;

    // ---- 문장(ParseStatements.cpp) ----
    uint32 ParseBlock(StringView context, StringView fix) noexcept;
    uint32 ParseStatement() noexcept;
    uint32 ParseLocalVariable() noexcept;
    // 문장 수준의 값이다(3.6절 Value). try·await 값 뒤에 이항 연산자·`as?`가 오면 S0212다.
    uint32 ParseValue() noexcept;
    // `try [await] Postfix` 또는 `await Postfix` 하나다(연산자 검사 없음).
    uint32 ParseTryAwait() noexcept;
    // 현재 토큰이 앞의 값과 이항 연산을 이을 토큰인지다(`?? return` 따위의 Exit 제외).
    bool AtBinaryOperator() noexcept;
    uint32 ParseExpressionStatement() noexcept;
    uint32 ParseDiscardAssignment() noexcept;
    uint32 ParseIf() noexcept;
    uint32 ParseBinder(StringView context) noexcept;
    uint32 ParseFor() noexcept;
    uint32 ParseWhile() noexcept;
    uint32 ParseSwitch() noexcept;
    uint32 ParseSwitchCase() noexcept;
    uint32 ParseSwitchDefault() noexcept;
    // switch의 case·default 하나를 마친 뒤의 복구다(다음 case·default·`}`까지).
    void RecoverSwitchPart(usize consumedBefore) noexcept;
    uint32 ParseReturn() noexcept;
    uint32 ParseEmit() noexcept;
    uint32 ParseJump() noexcept;

    // ---- 식(ParseExpressions.cpp) ----
    uint32 ParseExpression() noexcept;
    uint32 MakeBinary(uint32 op, uint32 left, uint32 right) noexcept;
    uint32 ParseOr() noexcept;
    uint32 ParseAnd() noexcept;
    uint32 ParseEquality() noexcept;
    uint32 ParseComparison() noexcept;
    void ReportChainedComparison() noexcept;
    uint32 ParseCoalesce() noexcept;
    uint32 ParseAdditive() noexcept;
    uint32 ParseMultiplicative() noexcept;
    uint32 ParseUnary() noexcept;
    uint32 ParsePostfix() noexcept;
    uint32 ParseArguments() noexcept;
    uint32 ParsePrimary() noexcept;
    uint32 ParseString() noexcept;
    uint32 ParseCollection() noexcept;

    const SourceSet& sources_;
    SourceId id_;
    DiagnosticList& diagnostics_;
    SyntaxTree& tree_;
    Allocator& allocator_;
    Array<uint32> stack_;
    // 노드마다 트리 깊이다(nodes_와 같은 번호, MaxSyntaxTreeDepth 검사용).
    Array<uint32> depths_;
    // 이 파일의 어휘 진단 시작 위치(오름차순)다. 그 위치의 토큰에서는 구문 오류를 내지 않는다.
    Array<uint32> lexicalErrors_;
    usize position_ = 0;
    // 마지막으로 소비한 토큰의 끝·종류와 지금까지 소비한 토큰 수다(문서 주석 제외).
    uint32 previousEnd_ = 0;
    TokenKind lastKind_ = TokenKind::EndOfFile;
    usize consumed_ = 0;
    // 마지막으로 낸 구문 진단의 시작 위치다(같은 위치에는 하나만 낸다). 없으면 NoToken과 같은
    // 값이다.
    uint32 lastReportedBegin_ = NoToken;
    uint32 depth_ = 0;
    bool panic_ = false;
    bool stopped_ = false;
    ErrorCode error_ = ErrorCode::None;
};

} // namespace rocks::sparks::detail
