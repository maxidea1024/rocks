#pragma once

#include "ShaderSourceStatusInternal.h"

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/Span.h>
#include <ShaderCompiler/ShaderPreprocessor.h>

// 셰이더 전처리기 내부 형식이다(Docs/Design/ShaderSystem 11.1절 SC3). 파일은 기능별로
// 나뉜다.
// - ShaderPreprocessorText.cpp: 원문 정규화(BOM·CRLF·UTF-8), 논리 줄(줄 이음·주석), 토큰 나누기.
// - ShaderPreprocessorMacros.cpp: 매크로 정의·숨김 집합·전개(#·##·가변 인자).
// - ShaderPreprocessorExpression.cpp: #if 식 계산.
// - ShaderPreprocessorDirectives.cpp: 파일·지시문·조건 상태·출력(#line)과 PreprocessShader.
//
// 매크로 재귀 금지는 숨김 집합(Prosser 알고리즘, C 표준 6.10.3.4의 "다시 펼치지 않음")이다.
// 토큰마다 자기를 만든 매크로 집합을 달고, 집합에 든 매크로는 그 토큰에서 펼치지 않는다.
namespace rocks::shader::preprocess_detail {

// 수명이 다른 텍스트(논리 줄, 매크로 본문, 붙이기·문자열화 결과)를 담는 byte arena다. 블록 안에서
// 앞으로만 할당하고 Reset은 모든 할당을 한꺼번에 무효로 한다(첫 블록은 다시 쓴다).
class TextArena final {
public:
    explicit TextArena(Allocator& allocator) noexcept : allocator_(&allocator) {
    }

    ~TextArena() noexcept;
    TextArena(const TextArena&) = delete;
    TextArena& operator=(const TextArena&) = delete;

    // bytes byte를 alignment(2의 거듭제곱)에 맞춰 할당한다. 실패하면 nullptr다. 0 byte도 유효한
    // 주소를 준다.
    void* Allocate(usize bytes, usize alignment = 1) noexcept;

    // text를 복사한 view를 out에 둔다. 실패하면 false다.
    bool Copy(StringView text, StringView& out) noexcept;

    // 모든 할당을 무효로 한다. 첫 블록 밖의 블록은 돌려준다.
    void Reset() noexcept;

private:
    struct Block {
        Block* next;
        usize capacity;
        usize used;
    };

    // 기본 블록 크기다. 이보다 큰 요청은 그 크기의 블록 하나를 따로 잡는다.
    static constexpr usize BlockBytes = usize{64} * 1024;

    static char* BlockData(Block* block) noexcept {
        return reinterpret_cast<char*>(block + 1);
    }

    void ReleaseFrom(Block* block) noexcept;

    Allocator* allocator_;
    // 가장 최근 블록이다. next는 이전 블록이다(마지막이 첫 블록).
    Block* head_ = nullptr;
};

enum class TokenKind : uint8 {
    Identifier,
    // pp-number(숫자로 시작하거나 '.' + 숫자).
    Number,
    String,
    Character,
    Punctuator,
    // 그 밖의 문자 하나(UTF-8 code point 하나, '@'·'$' 등).
    Other,
    // 빈 인자를 '##' 처리 동안 나타내는 자리 표시다(출력하지 않는다).
    Placemarker
};

// 전처리 토큰이다. text·space는 arena(원문 논리 줄·매크로 본문·붙이기 결과)를 빌린다.
struct Token {
    StringView text;
    // 원문 토큰 앞의 같은 줄 공백(공백·탭, 주석은 공백 하나)이다. 매크로 전개로 만든 토큰은
    // 비어 있고 leadingSpace만 쓴다.
    StringView space;
    // 원문 물리 줄(1부터)이다. 매크로 전개 결과는 호출 위치(매크로 이름) 줄이다.
    uint32 line = 0;
    // 숨김 집합 번호다(0은 빈 집합, HideSets).
    uint32 hideSet = 0;
    TokenKind kind = TokenKind::Other;
    // 앞에 공백(주석 포함)이 있었다.
    bool leadingSpace = false;
};

// 논리 줄이다(줄 이음을 없애고 주석을 공백 하나로 바꾼 한 줄). text·breaks는 scratch arena를
// 빌린다. breaks는 text 안에서 물리 줄이 바뀐 위치(오름차순)다.
struct LogicalLine {
    StringView text;
    Span<const uint32> breaks;
    uint32 firstLine = 0;
    // 첫 공백 아닌 문자가 '#'이다.
    bool directive = false;
};

// 원래 파일 byte를 처리할 텍스트로 바꾼다: 앞 UTF-8 BOM 제거, CRLF·CR → LF. UTF-8이 아니거나
// NUL이 있으면 InvalidEncoding(그 줄). out은 arena를 빌린다.
ShaderSourceStatus NormalizeSource(Span<const uint8> bytes, StringView path, TextArena& arena,
                                   StringView& out) noexcept;

// 정규화한 텍스트를 논리 줄로 읽는다.
class LineReader final {
public:
    LineReader(Allocator& allocator, StringView text) noexcept
        : text_(text), buffer_(allocator), breaks_(allocator) {
    }

    // 다음 논리 줄을 scratch에 만든다. 끝이면 false이고 status는 성공이다. 오류(닫히지 않은 주석,
    // trigraph, 메모리)면 false이고 status에 있다.
    bool Next(LogicalLine& line, TextArena& scratch, StringView path,
              ShaderSourceStatus& status) noexcept;

private:
    StringView text_;
    usize position_ = 0;
    uint32 line_ = 1;
    Array<char> buffer_;
    Array<uint32> breaks_;
};

// 논리 줄(또는 정의 값)을 토큰으로 나눈다. 토큰의 text는 line.text를 빌린다. 닫히지 않은
// 리터럴은 UnterminatedLiteral이다. 끝 공백은 버린다.
ShaderSourceStatus Tokenize(const LogicalLine& line, StringView path, Array<Token>& out) noexcept;

// text가 정확히 토큰 하나인지 본다(붙이기 결과 확인). 하나면 kind에 종류를 둔다.
bool IsSingleToken(StringView text, TokenKind& kind) noexcept;

// text 앞 토큰 하나의 byte 길이다(출력에서 두 토큰이 붙어 다른 토큰이 되는지 볼 때 쓴다). text가
// 공백으로 시작하거나 비면 0이다.
usize FirstTokenLength(StringView text) noexcept;

bool IsIdentifierStart(char value) noexcept;
bool IsIdentifierContinue(char value) noexcept;

inline bool IsPunctuator(const Token& token, StringView text) noexcept {
    return token.kind == TokenKind::Punctuator && token.text == text;
}

// 이름이 컴파일러 내장 이름(__로 시작, __LINE__·__FILE__ 제외)인가(SC3).
bool IsCompilerBuiltinName(StringView name) noexcept;

// 숨김 집합 표다. 집합은 정렬된 매크로 번호 목록이며 번호 0은 빈 집합이다. 한 줄 처리가 끝나면
// Reset으로 비운다(토큰이 남지 않을 때).
class HideSets final {
public:
    explicit HideSets(Allocator& allocator) noexcept : members_(allocator), sets_(allocator) {
    }

    bool Contains(uint32 set, uint32 macro) const noexcept;
    uint32 Size(uint32 set) const noexcept;
    // 실패(메모리)하면 false다.
    bool Union(uint32 left, uint32 right, uint32& out) noexcept;
    bool Intersect(uint32 left, uint32 right, uint32& out) noexcept;
    bool Add(uint32 set, uint32 macro, uint32& out) noexcept;
    void Reset() noexcept;

private:
    struct Range {
        uint32 offset = 0;
        uint32 count = 0;
    };

    Span<const uint32> Members(uint32 set) const noexcept;
    bool Publish(usize offset, uint32& out) noexcept;

    Array<uint32> members_;
    Array<Range> sets_;
};

enum class MacroKind : uint8 {
    Object,
    Function,
    // __LINE__·__FILE__.
    Line,
    File
};

// 매크로 정의다. name·parameters·body의 text는 persistent arena를 빌린다.
struct Macro {
    explicit Macro(Allocator& allocator) noexcept : parameters(allocator), body(allocator) {
    }

    StringView name;
    // 가변 인자 매크로는 마지막이 "__VA_ARGS__"다.
    Array<StringView> parameters;
    Array<Token> body;
    // 숨김 집합에 쓰는 번호(1부터, 정의마다 새 번호)다.
    uint32 id = 0;
    MacroKind kind = MacroKind::Object;
    bool variadic = false;
};

// 전처리 한 번의 상태다. 지시문·전개·식 계산이 함께 쓴다.
struct Context {
    Context(Allocator& allocator, const ShaderSourceTree& sourceTree,
            const ShaderPreprocessLimits& preprocessLimits) noexcept
        : allocator(allocator), tree(sourceTree), limits(preprocessLimits), persistent(allocator),
          scratch(allocator), macros(allocator), macroIndex(allocator), hideSets(allocator) {
    }

    Allocator& allocator;
    const ShaderSourceTree& tree;
    ShaderPreprocessLimits limits;
    // 전처리가 끝날 때까지 사는 텍스트(매크로 정의, 가상 경로, __FILE__ 문자열, 정규화한 원문).
    TextArena persistent;
    // 한 줄(여러 줄 매크로 호출이면 그 줄들) 동안 사는 텍스트.
    TextArena scratch;
    Array<Macro> macros;
    // 정의된 이름 → macros 색인. #undef는 여기서만 지운다.
    HashMap<StringView, uint32> macroIndex;
    HideSets hideSets;
    // 한 줄 처리에서 전개가 만든 토큰 byte 합(중간 결과 한도).
    usize expandedBytes = 0;
    // 지금 처리하는 파일의 가상 경로(오류 위치)다.
    StringView path;
    // 첫 오류다. 오류가 나면 모든 처리가 false로 돌아온다.
    ShaderSourceStatus status;

    bool Fail(ShaderSourceError code, uint32 line, StringView message,
              StringView detail = StringView()) noexcept {
        status = source_detail::MakeSourceStatus(code, path, line, message, detail);
        return false;
    }

    bool OutOfMemory(uint32 line) noexcept {
        return Fail(ShaderSourceError::OutOfMemory, line, "out of memory");
    }

    // 정의된 매크로다. 없으면 nullptr.
    const Macro* FindMacro(StringView name) const noexcept {
        const uint32* index = macroIndex.Find(name);
        return index == nullptr ? nullptr : &macros[*index];
    }
};

// 전개 결과를 받는 쪽이다(출력 쓰기·토큰 모으기).
class TokenSink {
public:
    virtual bool Put(const Token& token) noexcept = 0;

protected:
    ~TokenSink() = default;
};

// 함수형 매크로 호출이 줄 끝에 걸리면 다음 원문 줄의 토큰을 더 읽는다.
class TokenFeeder {
public:
    enum class Outcome : uint8 {
        Added,
        // 파일 끝이거나 다음 줄이 지시문이다(더 읽지 않는다).
        End,
        Failed
    };

    virtual Outcome More(Array<Token>& tokens) noexcept = 0;

protected:
    ~TokenFeeder() = default;
};

// 매크로 정의를 읽는다(#define 뒤 토큰). 이미 같은 이름이 같은 내용이면 그대로 두고, 다르면
// MacroRedefined다.
bool DefineMacro(Context& context, Span<const Token> tokens, uint32 line) noexcept;

// 정의 값 하나(이름·값)를 객체형 매크로로 정의한다(job 정의).
bool DefinePredefined(Context& context, StringView name, StringView value) noexcept;

// __LINE__·__FILE__ 내장 매크로를 등록한다.
bool DefineBuiltins(Context& context) noexcept;

// #undef 이름을 지운다.
bool UndefineMacro(Context& context, Span<const Token> tokens, uint32 line) noexcept;

// tokens를 앞에서부터 끝까지 펼쳐 sink에 넘긴다. feeder가 있으면 줄을 넘는 호출을 위해 더
// 읽는다. depth는 인자 미리 전개의 중첩 수다. 오류면 false이고 context.status에 있다.
bool ExpandTokens(Context& context, Array<Token>& tokens, TokenSink& sink, TokenFeeder* feeder,
                  uint32 depth) noexcept;

// #if·#elif 식(지시문 이름 뒤 토큰)을 계산한다.
bool EvaluateCondition(Context& context, Span<const Token> tokens, uint32 line,
                       bool& value) noexcept;

} // namespace rocks::shader::preprocess_detail
