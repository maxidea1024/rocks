#include "ShaderPreprocessorInternal.h"

#include <Platform/Memory.h>

// 매크로 정의와 전개다(C 표준 6.10.3). 재귀 금지는 숨김 집합으로 한다: 객체형 전개 결과의 집합은
// HS(이름) ∪ {M}, 함수형은 (HS(이름) ∩ HS(')')) ∪ {M}이고 집합에 든 매크로는 그 토큰에서 펼치지
// 않는다. 인자는 '#'·'##' 옆이 아니면 따로 끝까지 펼친 뒤 넣고, 결과는 뒤 토큰과 함께 다시 훑는다.
namespace rocks::shader::preprocess_detail {

namespace preprocess_macro_detail {

constexpr StringView VariadicName("__VA_ARGS__");
constexpr StringView DefinedName("defined");
constexpr StringView LineName("__LINE__");
constexpr StringView FileName("__FILE__");
// 결정성을 깨는 내장 이름이다(SC3).
constexpr StringView NondeterministicNames[] = {"__DATE__", "__TIME__", "__TIMESTAMP__"};
// __LINE__ 값의 최대 자릿수(uint32)다.
constexpr usize MaxLineDigits = 10;

bool IsReservedMacroName(StringView name) noexcept {
    return name == DefinedName || name == LineName || name == FileName || name == VariadicName;
}

bool IsNondeterministicName(StringView name) noexcept {
    for (const StringView candidate : NondeterministicNames) {
        if (name == candidate) {
            return true;
        }
    }

    return false;
}

// name이 매크로 인자 이름이면 그 색인, 아니면 IndexNone이다.
usize ParameterIndex(const Macro& macro, const Token& token) noexcept {
    if (macro.kind != MacroKind::Function || token.kind != TokenKind::Identifier) {
        return IndexNone;
    }

    for (usize index = 0; index < macro.parameters.Size(); ++index) {
        if (macro.parameters[index] == token.text) {
            return index;
        }
    }

    return IndexNone;
}

bool SameDefinition(const Macro& left, const Macro& right) noexcept {
    if (left.kind != right.kind || left.variadic != right.variadic ||
        left.parameters.Size() != right.parameters.Size() ||
        left.body.Size() != right.body.Size()) {
        return false;
    }

    for (usize index = 0; index < left.parameters.Size(); ++index) {
        if (left.parameters[index] != right.parameters[index]) {
            return false;
        }
    }

    for (usize index = 0; index < left.body.Size(); ++index) {
        const Token& a = left.body[index];
        const Token& b = right.body[index];
        if (a.text != b.text || (index != 0 && a.leadingSpace != b.leadingSpace)) {
            return false;
        }
    }

    return true;
}

// 전개 결과를 모으는 sink다(인자 미리 전개).
class TokenCollector final : public TokenSink {
public:
    explicit TokenCollector(Array<Token>& out) noexcept : out_(&out) {
    }

    bool Put(const Token& token) noexcept override {
        return static_cast<bool>(out_->TryPushBack(token));
    }

private:
    Array<Token>* out_;
};

// tokens[begin, end)를 replacement로 바꾸고 그 앞(이미 내보낸 토큰)은 버린다.
bool SpliceTokens(Array<Token>& tokens, usize end, Array<Token>& replacement) noexcept {
    if (!replacement.TryAppend(Span<const Token>(tokens.Data() + end, tokens.Size() - end))) {
        return false;
    }

    tokens = Move(replacement);
    return true;
}

// 함수형 매크로 호출의 인자(펼치기 전 토큰)다.
struct MacroArguments {
    explicit MacroArguments(Allocator& allocator) noexcept
        : raw(allocator), expanded(allocator), ready(allocator) {
    }

    Array<Array<Token>> raw;
    Array<Array<Token>> expanded;
    Array<bool> ready;
};

bool AddArgument(Allocator& allocator, MacroArguments& arguments) noexcept {
    return static_cast<bool>(arguments.raw.TryEmplaceBack(allocator)) &&
           static_cast<bool>(arguments.expanded.TryEmplaceBack(allocator)) &&
           static_cast<bool>(arguments.ready.TryPushBack(false));
}

// tokens[open]이 '('인 호출의 인자를 모은다. close에 ')' 색인을 둔다.
bool CollectArguments(Context& context, Array<Token>& tokens, usize open, TokenFeeder* feeder,
                      const Macro& macro, const Token& name, MacroArguments& arguments,
                      usize& close) noexcept {
    if (!AddArgument(context.allocator, arguments)) {
        return context.OutOfMemory(name.line);
    }

    const usize fixed = macro.variadic ? macro.parameters.Size() - 1 : MaxSize;
    usize nesting = 0;
    usize index = open + 1;
    while (true) {
        if (index >= tokens.Size()) {
            const TokenFeeder::Outcome more =
                feeder != nullptr ? feeder->More(tokens) : TokenFeeder::Outcome::End;
            if (more == TokenFeeder::Outcome::Failed) {
                return false;
            }

            if (more == TokenFeeder::Outcome::End) {
                return context.Fail(ShaderSourceError::InvalidMacroCall, name.line,
                                    "unterminated argument list invoking macro", macro.name);
            }

            continue;
        }

        const Token token = tokens[index];
        if (IsPunctuator(token, "(")) {
            ++nesting;
        } else if (IsPunctuator(token, ")")) {
            if (nesting == 0) {
                close = index;
                break;
            }

            --nesting;
        } else if (IsPunctuator(token, ",") && nesting == 0 && arguments.raw.Size() - 1 < fixed) {
            if (!AddArgument(context.allocator, arguments)) {
                return context.OutOfMemory(name.line);
            }

            ++index;
            continue;
        }

        if (!arguments.raw[arguments.raw.Size() - 1].TryPushBack(token)) {
            return context.OutOfMemory(name.line);
        }

        ++index;
    }

    // 인자 수: 인자 없는 매크로는 빈 인자 하나(F())만, 가변 인자는 생략 가능, 그 밖은 같아야 한다.
    const usize parameters = macro.parameters.Size();
    usize given = arguments.raw.Size();
    if (parameters == 0 && given == 1 && arguments.raw[0].IsEmpty()) {
        given = 0;
    }

    if (macro.variadic && given == parameters - 1) {
        if (!AddArgument(context.allocator, arguments)) {
            return context.OutOfMemory(name.line);
        }

        given = parameters;
    }

    if (given != parameters) {
        return context.Fail(ShaderSourceError::InvalidMacroCall, name.line,
                            "wrong number of arguments in macro invocation", macro.name);
    }

    return true;
}

bool AppendText(char* storage, usize& used, StringView text) noexcept {
    if (!text.IsEmpty()) {
        platform::MemoryCopy(storage + used, text.Data(), text.ByteCount());
    }

    used += text.ByteCount();
    return true;
}

// '#' 인자: 펼치기 전 인자 토큰을 문자열 리터럴로 만든다(토큰 사이 공백은 하나, 리터럴 안의 '"'와
// '\'는 이스케이프).
bool Stringize(Context& context, Span<const Token> argument, uint32 line, Token& out) noexcept {
    usize bytes = 2;
    for (usize index = 0; index < argument.Size(); ++index) {
        const Token& token = argument[index];
        bytes += token.text.ByteCount() * 2 + 1;
    }

    char* storage = static_cast<char*>(context.scratch.Allocate(bytes));
    if (storage == nullptr) {
        return context.OutOfMemory(line);
    }

    usize used = 0;
    storage[used++] = '"';
    for (usize index = 0; index < argument.Size(); ++index) {
        const Token& token = argument[index];
        if (index != 0 && token.leadingSpace) {
            storage[used++] = ' ';
        }

        const bool literal = token.kind == TokenKind::String || token.kind == TokenKind::Character;
        for (const char value : token.text) {
            if (literal && (value == '"' || value == '\\')) {
                storage[used++] = '\\';
            }

            storage[used++] = value;
        }
    }

    storage[used++] = '"';
    out = Token{};
    out.text = StringView(storage, used);
    out.kind = TokenKind::String;
    out.line = line;
    return true;
}

// '##': 두 토큰을 이어 토큰 하나를 만든다.
bool Paste(Context& context, const Token& left, const Token& right, Token& out) noexcept {
    const usize bytes = left.text.ByteCount() + right.text.ByteCount();
    char* storage = static_cast<char*>(context.scratch.Allocate(bytes));
    if (storage == nullptr) {
        return context.OutOfMemory(left.line);
    }

    usize used = 0;
    AppendText(storage, used, left.text);
    AppendText(storage, used, right.text);
    const StringView text(storage, used);
    TokenKind kind = TokenKind::Other;
    if (!IsSingleToken(text, kind)) {
        return context.Fail(ShaderSourceError::InvalidPaste, left.line,
                            "pasting does not form a valid preprocessing token", text);
    }

    out = left;
    out.text = text;
    out.kind = kind;
    return true;
}

// 인자 index를 미리 펼친 결과다(한 번만 계산한다).
bool ExpandedArgument(Context& context, MacroArguments& arguments, usize index, uint32 line,
                      uint32 depth, Span<const Token>& out) noexcept {
    if (!arguments.ready[index]) {
        if (depth + 1 > context.limits.macroExpansionDepth) {
            return context.Fail(ShaderSourceError::MacroDepthExceeded, line,
                                "macro argument expansion is nested too deeply");
        }

        Array<Token> work(context.allocator);
        if (!work.TryAppend(arguments.raw[index].View())) {
            return context.OutOfMemory(line);
        }

        TokenCollector collector(arguments.expanded[index]);
        if (!ExpandTokens(context, work, collector, nullptr, depth + 1)) {
            return false;
        }

        arguments.ready[index] = true;
    }

    out = arguments.expanded[index].View();
    return true;
}

bool AppendTokens(Array<Token>& out, Span<const Token> tokens, bool leadingSpace) noexcept {
    const usize first = out.Size();
    if (!out.TryAppend(tokens)) {
        return false;
    }

    if (out.Size() > first) {
        out[first].leadingSpace = leadingSpace;
    }

    return true;
}

// 본문을 인자로 바꾸고(#·##·인자 미리 전개) 숨김 집합 hideSet을 단다. 결과의 첫 토큰은 이름 토큰의
// 공백을 이어받고 모든 토큰의 줄은 호출 줄이다.
bool Substitute(Context& context, const Macro& macro, MacroArguments* arguments, const Token& name,
                uint32 hideSet, uint32 depth, Array<Token>& out) noexcept {
    const Array<Token>& body = macro.body;
    for (usize index = 0; index < body.Size(); ++index) {
        const Token& token = body[index];
        const bool pasteNext = index + 1 < body.Size() && IsPunctuator(body[index + 1], "##");
        if (arguments != nullptr && IsPunctuator(token, "#") && index + 1 < body.Size()) {
            const usize parameter = ParameterIndex(macro, body[index + 1]);
            if (parameter != IndexNone) {
                Token text;
                if (!Stringize(context, arguments->raw[parameter].View(), name.line, text)) {
                    return false;
                }

                text.leadingSpace = token.leadingSpace;
                if (!out.TryPushBack(text)) {
                    return context.OutOfMemory(name.line);
                }

                ++index;
                continue;
            }
        }

        if (IsPunctuator(token, "##") && index + 1 < body.Size()) {
            const Token& right = body[index + 1];
            ++index;
            const usize parameter = ParameterIndex(macro, right);
            const Span<const Token> rightTokens = parameter != IndexNone && arguments != nullptr
                                                      ? arguments->raw[parameter].View()
                                                      : Span<const Token>(&right, 1);
            if (rightTokens.IsEmpty()) {
                continue;
            }

            if (out.IsEmpty() || out[out.Size() - 1].kind == TokenKind::Placemarker) {
                const bool leadingSpace = out.IsEmpty() ? false : out[out.Size() - 1].leadingSpace;
                if (!out.IsEmpty()) {
                    (void)out.TryPopBack();
                }

                if (!AppendTokens(out, rightTokens, leadingSpace)) {
                    return context.OutOfMemory(name.line);
                }

                continue;
            }

            Token pasted;
            if (!Paste(context, out[out.Size() - 1], rightTokens[0], pasted)) {
                return false;
            }

            out[out.Size() - 1] = pasted;
            if (!out.TryAppend(rightTokens.Mid(1))) {
                return context.OutOfMemory(name.line);
            }

            continue;
        }

        const usize parameter = arguments != nullptr ? ParameterIndex(macro, token) : IndexNone;
        if (parameter != IndexNone) {
            if (pasteNext) {
                if (arguments->raw[parameter].IsEmpty()) {
                    Token placemarker;
                    placemarker.kind = TokenKind::Placemarker;
                    placemarker.leadingSpace = token.leadingSpace;
                    if (!out.TryPushBack(placemarker)) {
                        return context.OutOfMemory(name.line);
                    }
                } else if (!AppendTokens(out, arguments->raw[parameter].View(),
                                         token.leadingSpace)) {
                    return context.OutOfMemory(name.line);
                }

                continue;
            }

            Span<const Token> expanded;
            if (!ExpandedArgument(context, *arguments, parameter, name.line, depth, expanded)) {
                return false;
            }

            if (!AppendTokens(out, expanded, token.leadingSpace)) {
                return context.OutOfMemory(name.line);
            }

            continue;
        }

        if (!out.TryPushBack(token)) {
            return context.OutOfMemory(name.line);
        }
    }

    // 자리 표시를 지우고 숨김 집합·줄·공백을 정한다.
    usize kept = 0;
    usize bytes = 0;
    for (usize index = 0; index < out.Size(); ++index) {
        Token token = out[index];
        if (token.kind == TokenKind::Placemarker) {
            continue;
        }

        uint32 merged = 0;
        if (!context.hideSets.Union(token.hideSet, hideSet, merged)) {
            return context.OutOfMemory(name.line);
        }

        token.hideSet = merged;
        token.line = name.line;
        token.space = StringView();
        bytes += token.text.ByteCount() + 1;
        out[kept++] = token;
    }

    (void)out.TryResize(kept);
    if (!out.IsEmpty()) {
        out[0].leadingSpace = name.leadingSpace;
        out[0].space = name.space;
    }

    context.expandedBytes += bytes;
    if (context.expandedBytes > context.limits.outputBytes) {
        return context.Fail(ShaderSourceError::OutputTooLarge, name.line,
                            "macro expansion exceeds the preprocess output limit", macro.name);
    }

    return true;
}

// __LINE__ 값 토큰이다.
bool LineToken(Context& context, const Token& name, Token& out) noexcept {
    char digits[MaxLineDigits];
    usize count = 0;
    uint32 value = name.line;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    char* storage = static_cast<char*>(context.scratch.Allocate(count));
    if (storage == nullptr) {
        return context.OutOfMemory(name.line);
    }

    for (usize index = 0; index < count; ++index) {
        storage[index] = digits[count - 1 - index];
    }

    out = name;
    out.text = StringView(storage, count);
    out.kind = TokenKind::Number;
    return true;
}

// __FILE__ 값 토큰(지금 파일의 가상 경로 문자열)이다. 가상 경로에는 '"'·'\'가 없다.
bool FileToken(Context& context, const Token& name, Token& out) noexcept {
    const usize bytes = context.path.ByteCount() + 2;
    char* storage = static_cast<char*>(context.scratch.Allocate(bytes));
    if (storage == nullptr) {
        return context.OutOfMemory(name.line);
    }

    usize used = 0;
    storage[used++] = '"';
    AppendText(storage, used, context.path);
    storage[used++] = '"';
    out = name;
    out.text = StringView(storage, used);
    out.kind = TokenKind::String;
    return true;
}

// 매크로를 등록한다. 이미 같은 이름이 있으면 같은 내용일 때만 그대로 둔다.
bool RegisterMacro(Context& context, Macro&& macro, uint32 line) noexcept {
    if (const Macro* existing = context.FindMacro(macro.name)) {
        if (SameDefinition(*existing, macro)) {
            return true;
        }

        return context.Fail(ShaderSourceError::MacroRedefined, line,
                            "macro redefined with a different definition", macro.name);
    }

    macro.id = static_cast<uint32>(context.macros.Size()) + 1;
    const StringView name = macro.name;
    if (!context.macros.TryPushBack(Move(macro)) ||
        !context.macroIndex.InsertOrAssign(name, static_cast<uint32>(context.macros.Size() - 1))) {
        return context.OutOfMemory(line);
    }

    return true;
}

} // namespace preprocess_macro_detail

bool HideSets::Contains(uint32 set, uint32 macro) const noexcept {
    for (const uint32 member : Members(set)) {
        if (member == macro) {
            return true;
        }

        if (member > macro) {
            return false;
        }
    }

    return false;
}

uint32 HideSets::Size(uint32 set) const noexcept {
    return set == 0 ? 0u : sets_[set - 1].count;
}

Span<const uint32> HideSets::Members(uint32 set) const noexcept {
    if (set == 0) {
        return Span<const uint32>();
    }

    const Range& range = sets_[set - 1];
    return Span<const uint32>(members_.Data() + range.offset, range.count);
}

bool HideSets::Publish(usize offset, uint32& out) noexcept {
    const usize count = members_.Size() - offset;
    if (count == 0) {
        out = 0;
        return true;
    }

    Range range;
    range.offset = static_cast<uint32>(offset);
    range.count = static_cast<uint32>(count);
    if (!sets_.TryPushBack(range)) {
        (void)members_.TryResize(offset);
        return false;
    }

    out = static_cast<uint32>(sets_.Size());
    return true;
}

bool HideSets::Union(uint32 left, uint32 right, uint32& out) noexcept {
    if (left == 0 || left == right) {
        out = right;
        return true;
    }

    if (right == 0) {
        out = left;
        return true;
    }

    const usize offset = members_.Size();
    if (!members_.TryEnsureCapacity(offset + Size(left) + Size(right))) {
        return false;
    }

    const Span<const uint32> a = Members(left);
    const Span<const uint32> b = Members(right);
    usize i = 0;
    usize j = 0;
    while (i < a.Size() || j < b.Size()) {
        uint32 next = 0;
        if (j >= b.Size() || (i < a.Size() && a[i] < b[j])) {
            next = a[i++];
        } else if (i >= a.Size() || b[j] < a[i]) {
            next = b[j++];
        } else {
            next = a[i++];
            ++j;
        }

        (void)members_.TryPushBack(next);
    }

    return Publish(offset, out);
}

bool HideSets::Intersect(uint32 left, uint32 right, uint32& out) noexcept {
    if (left == 0 || right == 0) {
        out = 0;
        return true;
    }

    if (left == right) {
        out = left;
        return true;
    }

    const usize offset = members_.Size();
    if (!members_.TryEnsureCapacity(offset + Size(left))) {
        return false;
    }

    const Span<const uint32> a = Members(left);
    const Span<const uint32> b = Members(right);
    usize i = 0;
    usize j = 0;
    while (i < a.Size() && j < b.Size()) {
        if (a[i] < b[j]) {
            ++i;
        } else if (b[j] < a[i]) {
            ++j;
        } else {
            (void)members_.TryPushBack(a[i]);
            ++i;
            ++j;
        }
    }

    return Publish(offset, out);
}

bool HideSets::Add(uint32 set, uint32 macro, uint32& out) noexcept {
    if (Contains(set, macro)) {
        out = set;
        return true;
    }

    const usize offset = members_.Size();
    if (!members_.TryEnsureCapacity(offset + Size(set) + 1)) {
        return false;
    }

    const Span<const uint32> a = Members(set);
    bool placed = false;
    for (const uint32 member : a) {
        if (!placed && macro < member) {
            (void)members_.TryPushBack(macro);
            placed = true;
        }

        (void)members_.TryPushBack(member);
    }

    if (!placed) {
        (void)members_.TryPushBack(macro);
    }

    return Publish(offset, out);
}

void HideSets::Reset() noexcept {
    members_.Clear();
    sets_.Clear();
}

bool DefineBuiltins(Context& context) noexcept {
    using namespace preprocess_macro_detail;
    Macro line(context.allocator);
    line.name = LineName;
    line.kind = MacroKind::Line;
    Macro file(context.allocator);
    file.name = FileName;
    file.kind = MacroKind::File;
    return RegisterMacro(context, Move(line), 0) && RegisterMacro(context, Move(file), 0);
}

bool DefineMacro(Context& context, Span<const Token> tokens, uint32 line) noexcept {
    using namespace preprocess_macro_detail;
    if (tokens.IsEmpty() || tokens[0].kind != TokenKind::Identifier) {
        return context.Fail(ShaderSourceError::InvalidMacroDefinition, line,
                            "macro name must be an identifier");
    }

    if (IsReservedMacroName(tokens[0].text)) {
        return context.Fail(ShaderSourceError::InvalidMacroDefinition, line,
                            "this name cannot be defined as a macro", tokens[0].text);
    }

    Macro macro(context.allocator);
    if (!context.persistent.Copy(tokens[0].text, macro.name)) {
        return context.OutOfMemory(line);
    }

    usize index = 1;
    if (index < tokens.Size() && IsPunctuator(tokens[index], "(") && !tokens[index].leadingSpace) {
        macro.kind = MacroKind::Function;
        ++index;
        bool closed = false;
        bool expectName = true;
        while (index < tokens.Size()) {
            const Token& token = tokens[index++];
            if (IsPunctuator(token, ")") && (!expectName || macro.parameters.IsEmpty())) {
                closed = true;
                break;
            }

            if (expectName && IsPunctuator(token, "...")) {
                macro.variadic = true;
                if (!macro.parameters.TryPushBack(VariadicName)) {
                    return context.OutOfMemory(line);
                }

                if (index < tokens.Size() && IsPunctuator(tokens[index], ")")) {
                    ++index;
                    closed = true;
                }

                break;
            }

            if (expectName && token.kind == TokenKind::Identifier && token.text != VariadicName) {
                for (const StringView parameter : macro.parameters) {
                    if (parameter == token.text) {
                        return context.Fail(ShaderSourceError::InvalidMacroDefinition, line,
                                            "duplicate macro parameter name", token.text);
                    }
                }

                StringView name;
                if (!context.persistent.Copy(token.text, name) ||
                    !macro.parameters.TryPushBack(name)) {
                    return context.OutOfMemory(line);
                }

                expectName = false;
                continue;
            }

            if (!expectName && IsPunctuator(token, ",")) {
                expectName = true;
                continue;
            }

            break;
        }

        if (!closed) {
            return context.Fail(ShaderSourceError::InvalidMacroDefinition, line,
                                "invalid macro parameter list", macro.name);
        }
    }

    for (; index < tokens.Size(); ++index) {
        Token token = tokens[index];
        if (!context.persistent.Copy(token.text, token.text)) {
            return context.OutOfMemory(line);
        }

        token.space = StringView();
        token.hideSet = 0;
        token.line = 0;
        if (!macro.body.TryPushBack(token)) {
            return context.OutOfMemory(line);
        }
    }

    const Array<Token>& body = macro.body;
    if (!body.IsEmpty() &&
        (IsPunctuator(body[0], "##") || IsPunctuator(body[body.Size() - 1], "##"))) {
        return context.Fail(ShaderSourceError::InvalidMacroDefinition, line,
                            "'##' cannot appear at either end of a macro expansion", macro.name);
    }

    for (usize position = 0; position < body.Size(); ++position) {
        const Token& token = body[position];
        if (token.kind == TokenKind::Identifier && token.text == VariadicName && !macro.variadic) {
            return context.Fail(ShaderSourceError::InvalidMacroDefinition, line,
                                "__VA_ARGS__ can only appear in a variadic macro", macro.name);
        }

        if (macro.kind == MacroKind::Function && IsPunctuator(token, "#") &&
            (position + 1 >= body.Size() ||
             ParameterIndex(macro, body[position + 1]) == IndexNone)) {
            return context.Fail(ShaderSourceError::InvalidMacroDefinition, line,
                                "'#' is not followed by a macro parameter", macro.name);
        }
    }

    return RegisterMacro(context, Move(macro), line);
}

bool DefinePredefined(Context& context, StringView name, StringView value) noexcept {
    using namespace preprocess_macro_detail;
    bool identifier = !name.IsEmpty() && IsIdentifierStart(name.Data()[0]);
    for (const char character : name) {
        identifier = identifier && IsIdentifierContinue(character);
    }

    if (!identifier || IsReservedMacroName(name)) {
        return context.Fail(ShaderSourceError::InvalidDefine, 0,
                            "define name must be an identifier", name);
    }

    if (context.FindMacro(name) != nullptr) {
        return context.Fail(ShaderSourceError::DuplicateDefine, 0, "define is given twice", name);
    }

    for (const char character : value) {
        if (character == '\n' || character == '\r') {
            return context.Fail(ShaderSourceError::InvalidDefine, 0,
                                "define value cannot contain a line break", name);
        }
    }

    // "NAME VALUE" 한 줄로 만들어 #define과 같은 길로 읽는다(값이 '('로 시작해도 객체형이다).
    const usize bytes = name.ByteCount() + 1 + value.ByteCount();
    char* storage = static_cast<char*>(context.persistent.Allocate(bytes));
    if (storage == nullptr) {
        return context.OutOfMemory(0);
    }

    usize used = 0;
    AppendText(storage, used, name);
    storage[used++] = ' ';
    AppendText(storage, used, value);
    LogicalLine line;
    line.text = StringView(storage, used);
    Array<Token> tokens(context.allocator);
    if (const ShaderSourceStatus tokenized = Tokenize(line, context.path, tokens); !tokenized) {
        return context.Fail(ShaderSourceError::InvalidDefine, 0, "define value is not valid", name);
    }

    if (!DefineMacro(context, tokens.View(), 0)) {
        if (context.status.code == ShaderSourceError::InvalidMacroDefinition) {
            context.status.code = ShaderSourceError::InvalidDefine;
        }

        return false;
    }

    return true;
}

bool UndefineMacro(Context& context, Span<const Token> tokens, uint32 line) noexcept {
    using namespace preprocess_macro_detail;
    if (tokens.Size() != 1 || tokens[0].kind != TokenKind::Identifier) {
        return context.Fail(ShaderSourceError::InvalidDirective, line,
                            "#undef takes exactly one macro name");
    }

    if (IsReservedMacroName(tokens[0].text)) {
        return context.Fail(ShaderSourceError::InvalidMacroDefinition, line,
                            "this name cannot be undefined", tokens[0].text);
    }

    (void)context.macroIndex.Erase(tokens[0].text);
    return true;
}

bool ExpandTokens(Context& context, Array<Token>& tokens, TokenSink& sink, TokenFeeder* feeder,
                  uint32 depth) noexcept {
    using namespace preprocess_macro_detail;
    usize cursor = 0;
    while (cursor < tokens.Size()) {
        const Token token = tokens[cursor];
        if (token.kind == TokenKind::Placemarker) {
            ++cursor;
            continue;
        }

        if (token.kind != TokenKind::Identifier) {
            if (!sink.Put(token)) {
                return context.status ? context.OutOfMemory(token.line) : false;
            }

            ++cursor;
            continue;
        }

        if (IsNondeterministicName(token.text)) {
            return context.Fail(ShaderSourceError::NondeterministicMacro, token.line,
                                "nondeterministic built-in macro is not supported", token.text);
        }

        const uint32* found = context.macroIndex.Find(token.text);
        const Macro* macro = found != nullptr ? &context.macros[*found] : nullptr;
        if (macro == nullptr || context.hideSets.Contains(token.hideSet, macro->id)) {
            if (!sink.Put(token)) {
                return context.status ? context.OutOfMemory(token.line) : false;
            }

            ++cursor;
            continue;
        }

        if (macro->kind == MacroKind::Line || macro->kind == MacroKind::File) {
            Token value;
            if (!(macro->kind == MacroKind::Line ? LineToken(context, token, value)
                                                 : FileToken(context, token, value))) {
                return false;
            }

            if (!sink.Put(value)) {
                return context.status ? context.OutOfMemory(token.line) : false;
            }

            ++cursor;
            continue;
        }

        usize end = cursor + 1;
        uint32 hideSet = 0;
        MacroArguments arguments(context.allocator);
        if (macro->kind == MacroKind::Function) {
            // 이름 뒤 '('가 이번 줄에 없으면 다음 원문 줄을 본다(빈 줄은 건너뛴다).
            while (end >= tokens.Size() && feeder != nullptr) {
                const TokenFeeder::Outcome more = feeder->More(tokens);
                if (more == TokenFeeder::Outcome::Failed) {
                    return false;
                }

                if (more == TokenFeeder::Outcome::End) {
                    break;
                }
            }

            if (end >= tokens.Size() || !IsPunctuator(tokens[end], "(")) {
                if (!sink.Put(token)) {
                    return context.status ? context.OutOfMemory(token.line) : false;
                }

                ++cursor;
                continue;
            }

            usize close = 0;
            if (!CollectArguments(context, tokens, end, feeder, *macro, token, arguments, close)) {
                return false;
            }

            if (!context.hideSets.Intersect(token.hideSet, tokens[close].hideSet, hideSet)) {
                return context.OutOfMemory(token.line);
            }

            end = close + 1;
        } else {
            hideSet = token.hideSet;
        }

        if (!context.hideSets.Add(hideSet, macro->id, hideSet)) {
            return context.OutOfMemory(token.line);
        }

        if (context.hideSets.Size(hideSet) > context.limits.macroExpansionDepth) {
            return context.Fail(ShaderSourceError::MacroDepthExceeded, token.line,
                                "macro expansion is nested too deeply", macro->name);
        }

        Array<Token> expansion(context.allocator);
        if (!Substitute(context, *macro, macro->kind == MacroKind::Function ? &arguments : nullptr,
                        token, hideSet, depth, expansion)) {
            return false;
        }

        if (!SpliceTokens(tokens, end, expansion)) {
            return context.OutOfMemory(token.line);
        }

        cursor = 0;
    }

    return true;
}

} // namespace rocks::shader::preprocess_detail
