#include "ShaderPreprocessorInternal.h"

#include <Core/Encoding/Sha256.h>
#include <Platform/Memory.h>

// 전처리 진행이다: 파일 읽기·include 폐포, 지시문, 조건 상태, 출력(#line). 규칙은 공개 헤더
// ShaderPreprocessor.h 주석을 따른다.
namespace rocks::shader {

namespace preprocess_detail {

namespace preprocess_driver_detail {

// 출력 #line의 줄 번호 최대 자릿수(uint32)다.
constexpr usize MaxLineNumberDigits = 10;
// 두 토큰이 붙어 하나가 되는지 볼 때 앞 토큰 끝·뒤 토큰 앞에서 보는 byte 수(가장 긴 구두점
// 길이)다.
constexpr usize PasteWindowBytes = 3;

constexpr StringView EntryExtension(".rsf");

// 출력 쓰기다. 토큰을 원문 줄 위치에 놓는다(파일 주석의 출력 규칙).
class OutputWriter final : public TokenSink {
public:
    OutputWriter(Context& context, String& out) noexcept : context_(context), out_(out) {
    }

    bool Put(const Token& token) noexcept override {
        if (!MoveTo(context_.path, token.line)) {
            return false;
        }

        if (atLineStart_) {
            if (!Write(token.space)) {
                return false;
            }
        } else if (!token.space.IsEmpty()) {
            if (!Write(token.space)) {
                return false;
            }
        } else if ((token.leadingSpace || WouldMerge(token)) && !Write(StringView(" "))) {
            return false;
        }

        if (!Write(token.text)) {
            return false;
        }

        atLineStart_ = false;
        previous_ = token.text;
        previousKind_ = token.kind;
        return true;
    }

    // #pragma 줄을 원문 그대로(끝 공백 제외) 그 줄 위치에 쓴다.
    bool WriteLine(uint32 line, StringView text) noexcept {
        if (!MoveTo(context_.path, line)) {
            return false;
        }

        if (!atLineStart_ && !Write(StringView("\n"))) {
            return false;
        }

        usize bytes = text.ByteCount();
        while (bytes > 0 && (text.Data()[bytes - 1] == ' ' || text.Data()[bytes - 1] == '\t')) {
            --bytes;
        }

        if (!Write(StringView(text.Data(), bytes))) {
            return false;
        }

        atLineStart_ = false;
        previous_ = StringView();
        return true;
    }

    bool Finish() noexcept {
        return atLineStart_ || Write(StringView("\n"));
    }

private:
    bool Write(StringView text) noexcept {
        if (text.IsEmpty()) {
            return true;
        }

        if (out_.ByteCount() + text.ByteCount() > context_.limits.outputBytes) {
            return context_.Fail(ShaderSourceError::OutputTooLarge, line_,
                                 "preprocessed shader exceeds the output limit");
        }

        if (!out_.TryAppend(text)) {
            return context_.OutOfMemory(line_);
        }

        return true;
    }

    bool WriteLineDirective(StringView path, uint32 line) noexcept {
        char digits[MaxLineNumberDigits];
        usize count = 0;
        uint32 value = line;
        do {
            digits[MaxLineNumberDigits - 1 - count] = static_cast<char>('0' + value % 10);
            value /= 10;
            ++count;
        } while (value != 0);

        return (atLineStart_ || Write(StringView("\n"))) && Write(StringView("#line ")) &&
               Write(StringView(digits + MaxLineNumberDigits - count, count)) &&
               Write(StringView(" \"")) && Write(path) && Write(StringView("\"\n"));
    }

    // 출력 위치를 path의 line으로 옮긴다. 이미 그 줄이거나 지나쳤으면(매크로 호출이 여러 줄에
    // 걸친 경우) 그대로 둔다.
    bool MoveTo(StringView path, uint32 line) noexcept {
        if (path != path_) {
            if (!WriteLineDirective(path, line)) {
                return false;
            }

            path_ = path;
            line_ = line;
            atLineStart_ = true;
            previous_ = StringView();
            return true;
        }

        if (line <= line_) {
            return true;
        }

        const uint32 gap = line - line_;
        if (gap <= MaxShaderBlankLineRun) {
            for (uint32 index = 0; index < gap; ++index) {
                if (!Write(StringView("\n"))) {
                    return false;
                }
            }
        } else if (!WriteLineDirective(path, line)) {
            return false;
        }

        line_ = line;
        atLineStart_ = true;
        previous_ = StringView();
        return true;
    }

    // 공백 없이 붙이면 다른 토큰이 되는가(매크로 전개 결과가 옆 토큰과 붙는 경우).
    bool WouldMerge(const Token& token) const noexcept {
        if (previous_.IsEmpty() || token.text.IsEmpty()) {
            return false;
        }

        const char last = previous_.Data()[previous_.ByteCount() - 1];
        const char first = token.text.Data()[0];
        if (last == '/' && (first == '/' || first == '*')) {
            return true;
        }

        if ((previousKind_ == TokenKind::String || previousKind_ == TokenKind::Character ||
             previousKind_ == TokenKind::Number || previousKind_ == TokenKind::Identifier) &&
            (token.kind == TokenKind::Identifier || token.kind == TokenKind::Number)) {
            return true;
        }

        char window[PasteWindowBytes * 2];
        const usize tail =
            previous_.ByteCount() < PasteWindowBytes ? previous_.ByteCount() : PasteWindowBytes;
        const usize head =
            token.text.ByteCount() < PasteWindowBytes ? token.text.ByteCount() : PasteWindowBytes;
        platform::MemoryCopy(window, previous_.Data() + previous_.ByteCount() - tail, tail);
        platform::MemoryCopy(window + tail, token.text.Data(), head);
        return FirstTokenLength(StringView(window, tail + head)) > tail;
    }

    Context& context_;
    String& out_;
    StringView path_;
    uint32 line_ = 0;
    bool atLineStart_ = true;
    StringView previous_;
    TokenKind previousKind_ = TokenKind::Other;
};

// 읽은 원문 파일 하나다. path·text는 persistent arena를 빌린다.
struct SourceFile {
    StringView path;
    StringView text;
    Sha256Digest sha256;
    bool once = false;
};

struct Conditional {
    uint32 line = 0;
    // 바깥이 활성인가.
    bool parentActive = false;
    // 지금 갈래가 활성인가.
    bool active = false;
    // 앞 갈래 가운데 참인 것이 있었다(뒤 #elif·#else는 활성이 아니다).
    bool taken = false;
    bool sawElse = false;
};

class Preprocessor;

// 함수형 매크로 호출이 줄을 넘을 때 같은 파일의 다음 원문 줄을 더 읽는다. 지시문 줄을 만나면
// lookahead에 두고 멈춘다.
class FileFeeder final : public TokenFeeder {
public:
    FileFeeder(Context& context, LineReader& reader) noexcept : context_(context), reader_(reader) {
    }

    Outcome More(Array<Token>& tokens) noexcept override {
        while (!hasLookahead) {
            LogicalLine line;
            ShaderSourceStatus status;
            if (!reader_.Next(line, context_.scratch, context_.path, status)) {
                if (!status) {
                    context_.status = status;
                    return Outcome::Failed;
                }

                return Outcome::End;
            }

            if (line.directive) {
                lookahead = line;
                hasLookahead = true;
                return Outcome::End;
            }

            const usize before = tokens.Size();
            if (const ShaderSourceStatus tokenized = Tokenize(line, context_.path, tokens);
                !tokenized) {
                context_.status = tokenized;
                return Outcome::Failed;
            }

            if (tokens.Size() > before) {
                return Outcome::Added;
            }
        }

        return Outcome::End;
    }

    LogicalLine lookahead;
    bool hasLookahead = false;

private:
    Context& context_;
    LineReader& reader_;
};

class Preprocessor final {
public:
    Preprocessor(Context& context, String& text) noexcept
        : context_(context), writer_(context, text), files_(context.allocator),
          fileIndex_(context.allocator), conditionals_(context.allocator) {
    }

    // path를 읽어 처리한다. includeLine은 include 지시문 줄(0이면 엔트리·강제 include — 읽기
    // 오류를 그 파일 위치로 보고한다).
    bool Process(StringView path, uint32 depth, uint32 includeLine) noexcept {
        usize index = 0;
        if (!LoadFile(path, includeLine, index)) {
            return false;
        }

        if (files_[index].once) {
            return true;
        }

        const StringView previousPath = context_.path;
        context_.path = files_[index].path;
        if (!ProcessFile(index, depth)) {
            return false;
        }

        context_.path = previousPath;
        return true;
    }

    bool Finish() noexcept {
        return writer_.Finish();
    }

    Span<const SourceFile> Files() const noexcept {
        return files_.View();
    }

private:
    bool LoadFile(StringView path, uint32 includeLine, usize& index) noexcept {
        if (const uint32* found = fileIndex_.Find(path)) {
            index = *found;
            return true;
        }

        Array<uint8> bytes(context_.allocator);
        const ShaderGeneratedInclude* memory = nullptr;
        for (const ShaderGeneratedInclude& generated : context_.generated) {
            if (!generated.contents.IsEmpty() && generated.virtualPath == path) {
                memory = &generated;
                break;
            }
        }

        if (memory != nullptr) {
            // 메모리 원문이다(16.2절 SSD52 — 트리의 <루트>/Generated/ 자리).
            if (memory->contents.ByteCount() > context_.limits.fileBytes ||
                !bytes.TryResize(memory->contents.ByteCount())) {
                return context_.Fail(ShaderSourceError::FileTooLarge, includeLine,
                                     "generated shader source is too large", path);
            }

            if (!bytes.IsEmpty()) {
                platform::MemoryCopy(bytes.Data(), memory->contents.Data(), bytes.Size());
            }
        } else if (const ShaderSourceStatus loaded =
                       context_.tree.Load(path, bytes, context_.limits.fileBytes);
                   !loaded) {
            if (includeLine == 0) {
                context_.status = loaded;
                return false;
            }

            return context_.Fail(loaded.code, includeLine, StringView(loaded.message), path);
        }

        SourceFile file;
        file.sha256 = Sha256::Hash(bytes.View());
        if (!context_.persistent.Copy(path, file.path)) {
            return context_.OutOfMemory(includeLine);
        }

        if (const ShaderSourceStatus normalized =
                NormalizeSource(bytes.View(), file.path, context_.persistent, file.text);
            !normalized) {
            context_.status = normalized;
            return false;
        }

        index = files_.Size();
        if (!files_.TryPushBack(file) ||
            !fileIndex_.Insert(file.path, static_cast<uint32>(index))) {
            return context_.OutOfMemory(includeLine);
        }

        return true;
    }

    bool Active() const noexcept {
        return conditionals_.IsEmpty() || conditionals_[conditionals_.Size() - 1].active;
    }

    bool ProcessFile(usize fileIndex, uint32 depth) noexcept {
        LineReader reader(context_.allocator, files_[fileIndex].text);
        FileFeeder feeder(context_, reader);
        const usize base = conditionals_.Size();
        while (true) {
            LogicalLine line;
            if (feeder.hasLookahead) {
                line = feeder.lookahead;
                feeder.hasLookahead = false;
            } else {
                // 남은 토큰이 없으므로 한 줄 동안의 텍스트·숨김 집합을 비운다.
                context_.scratch.Reset();
                context_.hideSets.Reset();
                context_.expandedBytes = 0;
                ShaderSourceStatus status;
                if (!reader.Next(line, context_.scratch, context_.path, status)) {
                    if (!status) {
                        context_.status = status;
                        return false;
                    }

                    break;
                }
            }

            if (line.directive) {
                if (!Directive(line, fileIndex, depth, base)) {
                    return false;
                }

                continue;
            }

            if (!Active()) {
                continue;
            }

            Array<Token> tokens(context_.allocator);
            if (const ShaderSourceStatus tokenized = Tokenize(line, context_.path, tokens);
                !tokenized) {
                context_.status = tokenized;
                return false;
            }

            if (!ExpandTokens(context_, tokens, writer_, &feeder, 0)) {
                return false;
            }
        }

        if (conditionals_.Size() > base) {
            return context_.Fail(ShaderSourceError::UnbalancedConditional,
                                 conditionals_[conditionals_.Size() - 1].line,
                                 "unterminated conditional directive");
        }

        return true;
    }

    // 지시문 이름(# 뒤 첫 식별자)이다. 없으면 빈 값.
    static StringView DirectiveName(StringView text) noexcept {
        usize cursor = 0;
        const usize size = text.ByteCount();
        const auto skipSpace = [&text, &cursor, size]() noexcept {
            while (cursor < size && (text.Data()[cursor] == ' ' || text.Data()[cursor] == '\t' ||
                                     text.Data()[cursor] == '\f' || text.Data()[cursor] == '\v')) {
                ++cursor;
            }
        };
        skipSpace();
        ++cursor;
        skipSpace();
        const usize start = cursor;
        while (cursor < size && IsIdentifierContinue(text.Data()[cursor])) {
            ++cursor;
        }

        return StringView(text.Data() + start, cursor - start);
    }

    bool Directive(const LogicalLine& line, usize fileIndex, uint32 depth, usize base) noexcept {
        const StringView name = DirectiveName(line.text);
        const bool conditional =
            name == StringView("if") || name == StringView("ifdef") || name == StringView("ifndef");
        if (!Active()) {
            if (conditional) {
                Conditional skipped;
                skipped.line = line.firstLine;
                skipped.taken = true;
                if (!conditionals_.TryPushBack(skipped)) {
                    return context_.OutOfMemory(line.firstLine);
                }

                return true;
            }

            if (name == StringView("elif") || name == StringView("else") ||
                name == StringView("endif")) {
                return Branch(line, name, base);
            }

            // 비활성 구역의 다른 지시문은 보지 않는다.
            return true;
        }

        Array<Token> tokens(context_.allocator);
        if (const ShaderSourceStatus tokenized = Tokenize(line, context_.path, tokens);
            !tokenized) {
            context_.status = tokenized;
            return false;
        }

        if (tokens.Size() == 1) {
            return true;
        }

        if (tokens[1].kind != TokenKind::Identifier) {
            return context_.Fail(ShaderSourceError::InvalidDirective, line.firstLine,
                                 "invalid preprocessing directive", tokens[1].text);
        }

        const Span<const Token> rest = tokens.View().Mid(2);
        const uint32 at = line.firstLine;
        if (name == StringView("define")) {
            return DefineMacro(context_, rest, at);
        }

        if (name == StringView("undef")) {
            return UndefineMacro(context_, rest, at);
        }

        if (name == StringView("include")) {
            return Include(rest, at, depth);
        }

        if (name == StringView("include_next")) {
            return context_.Fail(ShaderSourceError::UnsupportedInclude, at,
                                 "#include_next is not supported");
        }

        if (name == StringView("pragma")) {
            if (rest.Size() == 1 && rest[0].kind == TokenKind::Identifier &&
                rest[0].text == StringView("once")) {
                files_[fileIndex].once = true;
                return true;
            }

            return writer_.WriteLine(at, line.text);
        }

        if (conditional) {
            return OpenConditional(name, rest, at);
        }

        if (name == StringView("elif") || name == StringView("else") ||
            name == StringView("endif")) {
            return Branch(line, name, base);
        }

        if (name == StringView("error")) {
            StringView message;
            if (rest.Size() > 0) {
                const char* begin = rest[0].text.Data();
                const Token& last = rest[rest.Size() - 1];
                message = StringView(
                    begin, static_cast<usize>(last.text.Data() + last.text.ByteCount() - begin));
            }

            return context_.Fail(ShaderSourceError::ErrorDirective, at, "#error", message);
        }

        return context_.Fail(ShaderSourceError::InvalidDirective, at,
                             "unsupported preprocessing directive", name);
    }

    bool OpenConditional(StringView name, Span<const Token> rest, uint32 line) noexcept {
        bool value = false;
        if (name == StringView("if")) {
            if (!EvaluateCondition(context_, rest, line, value)) {
                return false;
            }
        } else {
            if (rest.Size() != 1 || rest[0].kind != TokenKind::Identifier) {
                return context_.Fail(ShaderSourceError::InvalidDirective, line,
                                     "#ifdef and #ifndef take exactly one macro name");
            }

            if (IsCompilerBuiltinName(rest[0].text)) {
                return context_.Fail(ShaderSourceError::CompilerBuiltinInCondition, line,
                                     "compiler built-in names cannot be used in preprocessor "
                                     "conditions; use the ROCKS_* defines",
                                     rest[0].text);
            }

            const bool defined = context_.FindMacro(rest[0].text) != nullptr;
            value = name == StringView("ifdef") ? defined : !defined;
        }

        Conditional conditional;
        conditional.line = line;
        conditional.parentActive = true;
        conditional.active = value;
        conditional.taken = value;
        if (!conditionals_.TryPushBack(conditional)) {
            return context_.OutOfMemory(line);
        }

        return true;
    }

    bool Branch(const LogicalLine& line, StringView name, usize base) noexcept {
        const uint32 at = line.firstLine;
        if (conditionals_.Size() <= base) {
            return context_.Fail(ShaderSourceError::UnbalancedConditional, at,
                                 "conditional directive without #if", name);
        }

        Conditional& current = conditionals_[conditionals_.Size() - 1];
        if (name == StringView("endif")) {
            (void)conditionals_.TryPopBack();
            return true;
        }

        if (current.sawElse) {
            return context_.Fail(ShaderSourceError::UnbalancedConditional, at,
                                 "conditional directive after #else", name);
        }

        if (name == StringView("else")) {
            current.sawElse = true;
            current.active = current.parentActive && !current.taken;
            current.taken = true;
            return true;
        }

        // #elif: 바깥이 활성이고 앞 갈래가 모두 거짓일 때만 식을 계산한다.
        if (!current.parentActive || current.taken) {
            current.active = false;
            return true;
        }

        Array<Token> tokens(context_.allocator);
        if (const ShaderSourceStatus tokenized = Tokenize(line, context_.path, tokens);
            !tokenized) {
            context_.status = tokenized;
            return false;
        }

        bool value = false;
        if (!EvaluateCondition(context_, tokens.View().Mid(2), at, value)) {
            return false;
        }

        // 식 계산이 배열을 바꾸지 않으므로 current는 그대로 유효하다.
        current.active = value;
        current.taken = value;
        return true;
    }

    // 상대 include 경로를 포함한 파일 디렉터리에 붙이고 "."·".."를 접는다.
    bool ResolveInclude(StringView spelling, uint32 line, char (&out)[MaxShaderVirtualPathBytes],
                        usize& bytes) noexcept {
        bytes = 0;
        const auto append = [&out, &bytes](StringView part) noexcept {
            if (bytes + part.ByteCount() >= MaxShaderVirtualPathBytes) {
                return false;
            }

            if (!part.IsEmpty()) {
                platform::MemoryCopy(out + bytes, part.Data(), part.ByteCount());
            }

            bytes += part.ByteCount();
            return true;
        };
        if (spelling.Data()[0] == '/') {
            if (!append(spelling)) {
                return context_.Fail(ShaderSourceError::InvalidPath, line,
                                     "include path is too long", spelling);
            }

            return true;
        }

        // 포함한 파일의 디렉터리(마지막 '/'까지)에서 시작한다.
        const StringView includer = context_.path;
        usize directory = includer.ByteCount();
        while (directory > 0 && includer.Data()[directory - 1] != '/') {
            --directory;
        }

        if (!append(StringView(includer.Data(), directory > 0 ? directory - 1 : 0))) {
            return context_.Fail(ShaderSourceError::InvalidPath, line, "include path is too long",
                                 spelling);
        }

        usize cursor = 0;
        while (cursor <= spelling.ByteCount()) {
            usize end = cursor;
            while (end < spelling.ByteCount() && spelling.Data()[end] != '/') {
                ++end;
            }

            const StringView component(spelling.Data() + cursor, end - cursor);
            cursor = end + 1;
            if (component == StringView(".")) {
                continue;
            }

            if (component == StringView("..")) {
                while (bytes > 0 && out[bytes - 1] != '/') {
                    --bytes;
                }

                if (bytes == 0) {
                    return context_.Fail(ShaderSourceError::InvalidPath, line,
                                         "include path leaves the virtual root", spelling);
                }

                --bytes;
                continue;
            }

            if (component.IsEmpty()) {
                return context_.Fail(ShaderSourceError::InvalidPath, line,
                                     "include path has an empty component", spelling);
            }

            if (!append(StringView("/")) || !append(component)) {
                return context_.Fail(ShaderSourceError::InvalidPath, line,
                                     "include path is too long", spelling);
            }
        }

        return true;
    }

    bool Include(Span<const Token> rest, uint32 line, uint32 depth) noexcept {
        if (rest.IsEmpty()) {
            return context_.Fail(ShaderSourceError::InvalidDirective, line,
                                 "#include expects \"FILENAME\"");
        }

        const Token& target = rest[0];
        if (IsPunctuator(target, "<")) {
            return context_.Fail(ShaderSourceError::UnsupportedInclude, line,
                                 "#include <...> is not supported; use a quoted virtual path");
        }

        if (target.kind == TokenKind::Identifier) {
            return context_.Fail(ShaderSourceError::UnsupportedInclude, line,
                                 "macro-expanded #include is not supported", target.text);
        }

        if (target.kind != TokenKind::String || target.text.Data()[0] != '"' || rest.Size() != 1 ||
            target.text.ByteCount() <= 2) {
            return context_.Fail(ShaderSourceError::InvalidDirective, line,
                                 "#include expects a single quoted path");
        }

        const StringView spelling(target.text.Data() + 1, target.text.ByteCount() - 2);
        const bool driveLetter = spelling.ByteCount() >= 2 && spelling.Data()[1] == ':';
        bool backslash = false;
        for (const char value : spelling) {
            backslash = backslash || value == '\\';
        }

        if (driveLetter || backslash) {
            return context_.Fail(ShaderSourceError::UnsupportedInclude, line,
                                 "operating system paths cannot be included; use a virtual path",
                                 spelling);
        }

        char resolved[MaxShaderVirtualPathBytes];
        usize bytes = 0;
        if (!ResolveInclude(spelling, line, resolved, bytes)) {
            return false;
        }

        StringView path(resolved, bytes);
        if (const ShaderSourceStatus valid = ValidateShaderVirtualPath(path); !valid) {
            return context_.Fail(valid.code, line, StringView(valid.message), path);
        }

        // 생성 include 연결이면 연결 대상 파일을 읽는다(RenderBase 10.6절 2항).
        for (const ShaderGeneratedInclude& generated : context_.generated) {
            if (generated.virtualPath != path) {
                continue;
            }

            if (!generated.contents.IsEmpty()) {
                // 메모리 원문은 경로를 바꾸지 않는다(LoadFile이 contents를 읽는다).
                if (!generated.sourcePath.IsEmpty()) {
                    return context_.Fail(ShaderSourceError::InvalidArgument, line,
                                         "a generated include is either a link or memory "
                                         "contents",
                                         path);
                }

                break;
            }

            if (const ShaderSourceStatus valid = ValidateShaderVirtualPath(generated.sourcePath);
                !valid) {
                return context_.Fail(valid.code, line, StringView(valid.message),
                                     generated.sourcePath);
            }

            path = generated.sourcePath;
            break;
        }

        if (depth + 1 > context_.limits.includeDepth) {
            return context_.Fail(ShaderSourceError::IncludeDepthExceeded, line,
                                 "#include nested too deeply", path);
        }

        return Process(path, depth + 1, line);
    }

    Context& context_;
    OutputWriter writer_;
    Array<SourceFile> files_;
    HashMap<StringView, uint32> fileIndex_;
    Array<Conditional> conditionals_;
};

// 정의를 이름 byte 순으로 적용한다(같은 이름은 DuplicateDefine).
bool ApplyDefines(Context& context, Span<const ShaderDefine> defines) noexcept {
    Array<usize> order(context.allocator);
    for (usize index = 0; index < defines.Size(); ++index) {
        if (!order.TryPushBack(index)) {
            return context.OutOfMemory(0);
        }
    }

    order.Sort([&defines](usize left, usize right) noexcept {
        return StringView::CompareBytes(defines[left].name, defines[right].name) < 0;
    });
    for (const usize index : order) {
        if (!DefinePredefined(context, defines[index].name, defines[index].value)) {
            return false;
        }
    }

    return true;
}

bool Run(Context& context, StringView entryPath, Span<const ShaderDefine> defines,
         PreprocessedShader& out) noexcept {
    if (const ShaderSourceStatus valid = ValidateShaderVirtualPath(entryPath); !valid) {
        context.status = valid;
        return false;
    }

    if (!entryPath.EndsWith(EntryExtension, StringComparison::CaseSensitive)) {
        return context.Fail(ShaderSourceError::InvalidArgument, 0,
                            "shader entry file must be a .rsf file", entryPath);
    }

    if (!DefineBuiltins(context) || !ApplyDefines(context, defines)) {
        return false;
    }

    Preprocessor preprocessor(context, out.text);
    if (!preprocessor.Process(StringView(ShaderPlatformHeaderPath), 1, 0) ||
        !preprocessor.Process(entryPath, 0, 0) || !preprocessor.Finish()) {
        return false;
    }

    // include 폐포: 읽은 파일 전부, 가상 경로 byte 순.
    for (const SourceFile& file : preprocessor.Files()) {
        ShaderIncludeFile* entry = nullptr;
        if (auto added = out.includes.TryEmplaceBack(out.text.GetAllocator()); added) {
            entry = added.Value();
        } else {
            return context.OutOfMemory(0);
        }

        entry->sha256 = file.sha256;
        if (!entry->virtualPath.TryAssign(file.path)) {
            return context.OutOfMemory(0);
        }
    }

    out.includes.Sort([](const ShaderIncludeFile& left, const ShaderIncludeFile& right) noexcept {
        return StringView::CompareBytes(left.virtualPath.View(), right.virtualPath.View()) < 0;
    });
    return true;
}

} // namespace preprocess_driver_detail

} // namespace preprocess_detail

ShaderSourceStatus PreprocessShader(const ShaderSourceTree& tree, StringView entryPath,
                                    Span<const ShaderDefine> defines, PreprocessedShader& out,
                                    const ShaderPreprocessLimits& limits,
                                    Span<const ShaderGeneratedInclude> generated) noexcept {
    using namespace preprocess_detail;
    out.Reset();
    Context context(out.text.GetAllocator(), tree, limits);
    context.generated = generated;
    context.path = entryPath;
    if (!preprocess_driver_detail::Run(context, entryPath, defines, out)) {
        out.Reset();
        return context.status;
    }

    return ShaderSourceStatus{};
}

} // namespace rocks::shader
