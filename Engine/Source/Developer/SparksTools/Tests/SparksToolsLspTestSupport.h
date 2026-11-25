#pragma once

#include <Core/Algorithms.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Core/Text/Path.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/Uri.h>
#include <HostConfig/HostSettings.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/File.h>
#include <Platform/Path.h>
#include <Platform/Process.h>
#include <SparksTools/LanguageServer.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 언어 서버 시험의 공용 준비다(Docs/Design/SparksTools.ko.md 10.8절 — LC8e): 각본 입력을 주는
// 채널, 메시지 각본, 출력 본문·발행 진단·의미 강조 요약, file URI·경로 비교 모양, 사용자 임시
// 폴더 아래 공백·한글이 든 절대 경로의 시험 폴더.
namespace rocks::test::sparks_lsp {

using namespace rocks::sparks::tools;

inline constexpr usize LspTestPathCapacity = 4096;
inline constexpr usize LspTestWholeChunk = usize(1) << 20;
inline constexpr usize LspTestHeaderLimit = 1024;
inline constexpr usize LspTestHeaderBlockLimit = 4096;
inline constexpr int32 LspTestClean = 0;
inline constexpr int32 LspTestUnclean = 1;
inline constexpr int32 LspTestUsage = 2;
inline constexpr int32 LspTestError = 3;

// 시험이 보내는 LSP 위치다(0부터 센 줄·UTF-16 열).
struct LspTestPlace {
    uint32 line = 0;
    uint32 character = 0;
};

// 작업 공간 없이 initialize했을 때의 기록 줄이다(10.3절).
inline constexpr StringView LspTestNoWorkspaceLog =
    "sparks-lsp: no workspace folder is open; documents are not analyzed\n";

// 한글 두 글자(U+D55C U+AE00)와 이모지(U+1F600 — 서로게이트 쌍)의 UTF-8이다.
inline constexpr StringView LspTestHangul = "\xED\x95\x9C\xEA\xB8\x80";
inline constexpr StringView LspTestEmoji = "\xF0\x9F\x98\x80";

inline constexpr StringView LspTestCoreManifest =
    "{ name: \"Game\", sparks: 1, profile: \"core\" }\n";
inline constexpr StringView LspTestFullManifest =
    "{ name: \"Game\", sparks: 1, profile: \"full\" }\n";

// 서버 legend(10.7절)의 이름이다 — 토큰 data를 읽을 수 있는 줄로 바꿀 때 쓴다.
inline constexpr StringView LspTestTypes[] = {
    "namespace",  "type",  "class",    "struct", "enum",     "interface", "typeParameter",
    "enumMember", "event", "function", "method", "property", "parameter", "variable"};
inline constexpr StringView LspTestModifiers[] = {"declaration", "readonly", "static", "async",
                                                  "defaultLibrary"};

// 각본 입력을 chunk byte씩 주고 출력·기록을 모으는 채널이다. failRead·failWrite(1부터)는 그 번째
// 호출을 입출력 오류로 만든다.
class LspScriptChannel final : public MessageChannel {
public:
    LspScriptChannel(Allocator& allocator, StringView input, usize chunk) noexcept
        : output(allocator), log(allocator), input_(input), chunk_(chunk) {
    }

    Result<usize> Read(Span<char> buffer) noexcept override {
        ++reads;
        if (failRead != 0 && reads == failRead) {
            return Result<usize>::Failure(ErrorCode::DependencyFailed);
        }

        usize count = input_.ByteCount() - at_;
        count = count < chunk_ ? count : chunk_;
        count = count < buffer.Size() ? count : buffer.Size();
        for (usize index = 0; index < count; ++index) {
            buffer[index] = input_.Data()[at_ + index];
        }

        at_ += count;
        return Result<usize>::Success(count);
    }

    Result<void> Write(StringView bytes) noexcept override {
        ++writes;
        if (failWrite != 0 && writes == failWrite) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        return output.TryAppend(bytes);
    }

    void Log(StringView line) noexcept override {
        static_cast<void>(log.TryAppend(line));
    }

    String output;
    String log;
    usize reads = 0;
    usize writes = 0;
    usize failRead = 0;
    usize failWrite = 0;

private:
    StringView input_;
    usize at_ = 0;
    usize chunk_;
};

// JSON 문자열 값(따옴표 포함)을 out 뒤에 붙인다.
inline void LspQuote(String& out, StringView text) noexcept {
    constexpr char Hex[] = "0123456789abcdef";
    // escape할 byte 사이 구간은 통째로 붙인다(String은 UTF-8 sequence를 쪼개 붙일 수 없다).
    CHECK(out.TryAppend("\""));
    usize run = 0;
    for (usize at = 0; at < text.ByteCount(); ++at) {
        const char c = text.Data()[at];
        const auto value = static_cast<uint8>(c);
        if (c != '"' && c != '\\' && value >= 0x20) {
            continue;
        }

        CHECK(out.TryAppend(StringView(text.Data() + run, at - run)));
        run = at + 1;
        bool ok = true;
        if (c == '"') {
            ok = static_cast<bool>(out.TryAppend("\\\""));
        } else if (c == '\\') {
            ok = static_cast<bool>(out.TryAppend("\\\\"));
        } else if (c == '\n') {
            ok = static_cast<bool>(out.TryAppend("\\n"));
        } else if (c == '\r') {
            ok = static_cast<bool>(out.TryAppend("\\r"));
        } else if (c == '\t') {
            ok = static_cast<bool>(out.TryAppend("\\t"));
        } else {
            const char escaped[] = {'\\', 'u', '0', '0', Hex[value >> 4], Hex[value & 0xF]};
            ok = static_cast<bool>(out.TryAppend(StringView(escaped, sizeof(escaped))));
        }

        CHECK(ok);
    }

    CHECK(out.TryAppend(StringView(text.Data() + run, text.ByteCount() - run)));
    CHECK(out.TryAppend("\""));
}

// byte 그대로의 글자 모음이다(String과 달리 잘못된 UTF-8도 담는다 — 깨진 본문 시험).
class LspBytes final {
public:
    explicit LspBytes(Allocator& allocator) noexcept : bytes_(allocator) {
    }

    Result<void> TryAppend(StringView text) noexcept {
        return bytes_.TryAppend(Span<const char>(text.Data(), text.ByteCount()));
    }

    StringView View() const noexcept {
        return StringView(bytes_.Data(), bytes_.Size());
    }

    usize ByteCount() const noexcept {
        return bytes_.Size();
    }

private:
    Array<char> bytes_;
};

// 각본(틀을 붙인 메시지들)이다.
class LspScript final {
public:
    explicit LspScript(Allocator& allocator) noexcept : text(allocator), allocator_(allocator) {
    }

    LspScript& Raw(StringView bytes) noexcept {
        CHECK(text.TryAppend(bytes));
        return *this;
    }

    LspScript& Message(StringView body) noexcept {
        StringBuilder<64> head(allocator_);
        head.AppendFormat("Content-Length: {}\r\n\r\n", body.ByteCount());
        CHECK(!head.HasFailed());
        CHECK(text.TryAppend(head.View()));
        CHECK(text.TryAppend(body));
        return *this;
    }

    // `{"jsonrpc":"2.0","id":<id>,"method":"<method>","params":<params>}`(params 빈 값이면 뺀다).
    LspScript& Call(int64 id, StringView method, StringView params = {}) noexcept {
        StringBuilder<256> head(allocator_);
        head.AppendFormat("{{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"{}\"", id, method);
        return Finish(head.View(), params);
    }

    LspScript& Notify(StringView method, StringView params = {}) noexcept {
        StringBuilder<256> head(allocator_);
        head.AppendFormat("{{\"jsonrpc\":\"2.0\",\"method\":\"{}\"", method);
        return Finish(head.View(), params);
    }

    LspScript& Initialize(int64 id, StringView rootUri) noexcept {
        String params(allocator_);
        CHECK(params.TryAppend("{\"capabilities\":{}"));
        if (!rootUri.IsEmpty()) {
            CHECK(params.TryAppend(",\"rootUri\":"));
            LspQuote(params, rootUri);
        }

        CHECK(params.TryAppend("}"));
        return Call(id, "initialize", params.View());
    }

    LspScript& Open(StringView uri, int64 version, StringView content) noexcept {
        String params(allocator_);
        CHECK(params.TryAppend("{\"textDocument\":{\"uri\":"));
        LspQuote(params, uri);
        StringBuilder<64> number(allocator_);
        number.AppendFormat(",\"languageId\":\"sparks\",\"version\":{},\"text\":", version);
        CHECK(params.TryAppend(number.View()));
        LspQuote(params, content);
        CHECK(params.TryAppend("}}"));
        return Notify("textDocument/didOpen", params.View());
    }

    LspScript& Change(StringView uri, int64 version, StringView content) noexcept {
        String params(allocator_);
        CHECK(params.TryAppend("{\"textDocument\":{\"uri\":"));
        LspQuote(params, uri);
        StringBuilder<64> number(allocator_);
        number.AppendFormat(",\"version\":{}}},\"contentChanges\":[{{\"text\":", version);
        CHECK(params.TryAppend(number.View()));
        LspQuote(params, content);
        CHECK(params.TryAppend("}]}"));
        return Notify("textDocument/didChange", params.View());
    }

    LspScript& Close(StringView uri) noexcept {
        String params(allocator_);
        CHECK(params.TryAppend("{\"textDocument\":{\"uri\":"));
        LspQuote(params, uri);
        CHECK(params.TryAppend("}}"));
        return Notify("textDocument/didClose", params.View());
    }

    LspScript& Tokens(int64 id, StringView uri) noexcept {
        String params(allocator_);
        CHECK(params.TryAppend("{\"textDocument\":{\"uri\":"));
        LspQuote(params, uri);
        CHECK(params.TryAppend("}}"));
        return Call(id, "textDocument/semanticTokens/full", params.View());
    }

    // 위치 요청(`textDocument/hover`·`definition`·`completion`)이다.
    LspScript& At(int64 id, StringView method, StringView uri, uint32 line,
                  uint32 character) noexcept {
        String params(allocator_);
        CHECK(params.TryAppend("{\"textDocument\":{\"uri\":"));
        LspQuote(params, uri);
        StringBuilder<96> position(allocator_);
        position.AppendFormat("}},\"position\":{{\"line\":{},\"character\":{}}}}}", line,
                              character);
        CHECK(!position.HasFailed());
        CHECK(params.TryAppend(position.View()));
        return Call(id, method, params.View());
    }

    LspScript& Format(int64 id, StringView uri) noexcept {
        String params(allocator_);
        CHECK(params.TryAppend("{\"textDocument\":{\"uri\":"));
        LspQuote(params, uri);
        CHECK(params.TryAppend("},\"options\":{\"tabSize\":2,\"insertSpaces\":true}}"));
        return Call(id, "textDocument/formatting", params.View());
    }

    LspScript& End(int64 shutdownId) noexcept {
        Call(shutdownId, "shutdown");
        return Notify("exit");
    }

    LspBytes text;

private:
    LspScript& Finish(StringView head, StringView params) noexcept {
        String body(allocator_);
        CHECK(body.TryAppend(head));
        if (!params.IsEmpty()) {
            CHECK(body.TryAppend(",\"params\":"));
            CHECK(body.TryAppend(params));
        }

        CHECK(body.TryAppend("}"));
        return Message(body.View());
    }

    Allocator& allocator_;
};

// 서버 한 번의 결과다.
struct LspRun {
    explicit LspRun(Allocator& allocator) noexcept : output(allocator), log(allocator) {
    }

    int32 code = -1;
    String output;
    String log;
};

inline LspRun LspRunScript(Allocator& allocator, Span<const StringView> arguments,
                           StringView script, usize chunk = LspTestWholeChunk) noexcept {
    LspScriptChannel channel(allocator, script, chunk);
    LspRun run(allocator);
    run.code = RunLanguageServer(allocator, platform::PhysicalFileSystem(), arguments, channel);
    CHECK(run.output.TryAssign(channel.output.View()));
    CHECK(run.log.TryAssign(channel.log.View()));
    return run;
}

// 서버 출력(`Content-Length: N\r\n\r\n` + 본문 — 서버가 쓰는 모양 그대로)을 본문들로 나눈다.
inline bool LspBodies(StringView output, Array<String>& bodies, Allocator& allocator) noexcept {
    constexpr StringView Head = "Content-Length: ";
    usize at = 0;
    while (at < output.ByteCount()) {
        const StringView rest(output.Data() + at, output.ByteCount() - at);
        if (!rest.StartsWith(Head)) {
            return false;
        }

        usize cursor = Head.ByteCount();
        usize length = 0;
        while (cursor < rest.ByteCount() && rest.Data()[cursor] >= '0' &&
               rest.Data()[cursor] <= '9') {
            length = length * 10 + usize(rest.Data()[cursor] - '0');
            ++cursor;
        }

        const StringView tail(rest.Data() + cursor, rest.ByteCount() - cursor);
        if (!tail.StartsWith("\r\n\r\n") || tail.ByteCount() - 4 < length) {
            return false;
        }

        String body(allocator);
        if (!body.TryAssign(StringView(tail.Data() + 4, length)) ||
            !bodies.TryPushBack(Move(body))) {
            return false;
        }

        at += cursor + 4 + length;
    }

    return true;
}

// 같지 않으면 둘 다 진단 출력으로 보인다(기대값을 고칠 때 실제 값을 본다).
inline void LspExpectText(StringView actual, StringView expected, StringView what) noexcept {
    CHECK(actual == expected);
    if (actual != expected) {
        SystemAllocator allocator;
        String report(allocator);
        static_cast<void>(report.TryAppend("LSP mismatch: "));
        static_cast<void>(report.TryAppend(what));
        static_cast<void>(report.TryAppend("\n--- expected\n"));
        static_cast<void>(report.TryAppend(expected));
        static_cast<void>(report.TryAppend("\n--- actual\n"));
        static_cast<void>(report.TryAppend(actual));
        static_cast<void>(report.TryAppend("\n"));
        platform::WriteDiagnostic(report.View().Data(), report.View().ByteCount());
    }
}

// 본문들을 줄마다 하나씩 이은 글자다.
inline String LspJoined(Allocator& allocator, StringView output) noexcept {
    Array<String> bodies(allocator);
    CHECK(LspBodies(output, bodies, allocator));
    String joined(allocator);
    for (const String& body : bodies) {
        CHECK(joined.TryAppend(body.View()));
        CHECK(joined.TryAppend("\n"));
    }

    return joined;
}

inline void LspAppendNumber(String& out, int64 value, Allocator& allocator) noexcept {
    StringBuilder<32> text(allocator);
    text.AppendFormat("{}", value);
    CHECK(out.TryAppend(text.View()));
}

inline int64 LspNumber(JsonValue value) noexcept {
    if (!value.IsValid() || !value.IsNumber()) {
        return -1;
    }

    auto number = value.TryGetInt64();
    return number ? number.Value() : -1;
}

inline void LspAppendPosition(String& out, JsonValue position, Allocator& allocator) noexcept {
    LspAppendNumber(out, LspNumber(position.Find("line")), allocator);
    CHECK(out.TryAppend(":"));
    LspAppendNumber(out, LspNumber(position.Find("character")), allocator);
}

// 발행 진단 하나를 요약한다: `<uri 대신 이름> v<version> [<code> <l:c>-<l:c>, ...]`. 이름은 uri에서
// rootUri 접두를 뗀 나머지(접두가 다르면 uri 전체)다.
struct LspPublish {
    explicit LspPublish(Allocator& allocator) noexcept : uri(allocator), summary(allocator) {
    }

    String uri;
    String summary;
};

inline bool LspPublishes(Allocator& allocator, StringView output, StringView rootUri,
                         Array<LspPublish>& out) noexcept {
    Array<String> bodies(allocator);
    if (!LspBodies(output, bodies, allocator)) {
        return false;
    }

    for (const String& body : bodies) {
        JsonDocument document(allocator);
        if (!document.Parse(body.View()).Succeeded()) {
            return false;
        }

        const JsonValue root = document.Root();
        const JsonValue method = root.Find("method");
        if (!method.IsValid() || method.GetString() != "textDocument/publishDiagnostics") {
            continue;
        }

        const JsonValue params = root.Find("params");
        LspPublish entry(allocator);
        const StringView uri = params.Find("uri").GetString();
        CHECK(entry.uri.TryAssign(uri));
        const bool under =
            !rootUri.IsEmpty() && uri.StartsWith(rootUri, StringComparison::CaseSensitive) &&
            uri.ByteCount() > rootUri.ByteCount() && uri.Data()[rootUri.ByteCount()] == '/';
        CHECK(entry.summary.TryAppend(under ? StringView(uri.Data() + rootUri.ByteCount() + 1,
                                                         uri.ByteCount() - rootUri.ByteCount() - 1)
                                            : uri));
        const JsonValue version = params.Find("version");
        if (version.IsValid()) {
            CHECK(entry.summary.TryAppend(" v"));
            LspAppendNumber(entry.summary, LspNumber(version), allocator);
        }

        CHECK(entry.summary.TryAppend(" ["));
        const JsonValue diagnostics = params.Find("diagnostics");
        for (usize at = 0; at < diagnostics.Size(); ++at) {
            const JsonValue diagnostic = diagnostics.At(at);
            CHECK(LspNumber(diagnostic.Find("severity")) == 1);
            CHECK(diagnostic.Find("source").GetString() == "sparks");
            CHECK(diagnostic.Find("message").IsString());
            CHECK(entry.summary.TryAppend(at == 0 ? "" : ", "));
            CHECK(entry.summary.TryAppend(diagnostic.Find("code").GetString()));
            CHECK(entry.summary.TryAppend(" "));
            const JsonValue range = diagnostic.Find("range");
            LspAppendPosition(entry.summary, range.Find("start"), allocator);
            CHECK(entry.summary.TryAppend("-"));
            LspAppendPosition(entry.summary, range.Find("end"), allocator);
        }

        CHECK(entry.summary.TryAppend("]"));
        if (!out.TryPushBack(Move(entry))) {
            return false;
        }
    }

    return true;
}

// 발행 요약을 줄마다 이은 글자다.
inline String LspPublishText(Allocator& allocator, StringView output, StringView rootUri) noexcept {
    Array<LspPublish> publishes(allocator);
    CHECK(LspPublishes(allocator, output, rootUri, publishes));
    String text(allocator);
    for (const LspPublish& entry : publishes) {
        CHECK(text.TryAppend(entry.summary.View()));
        CHECK(text.TryAppend("\n"));
    }

    return text;
}

inline bool LspDrive(StringView path) noexcept {
    return path.ByteCount() >= 2 && path.Data()[1] == ':' &&
           ((path.Data()[0] >= 'a' && path.Data()[0] <= 'z') ||
            (path.Data()[0] >= 'A' && path.Data()[0] <= 'Z'));
}

// 경로의 비교 모양(`/` 구분·드라이브 소문자)이다.
inline String LspKeyOf(Allocator& allocator, StringView path) noexcept {
    String key(allocator);
    usize run = 0;
    if (LspDrive(path)) {
        const char c = path.Data()[0];
        const char drive = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        CHECK(key.TryAppend(StringView(&drive, 1)));
        run = 1;
    }

    for (usize at = run; at <= path.ByteCount(); ++at) {
        if (at < path.ByteCount() && path.Data()[at] != '\\') {
            continue;
        }

        CHECK(key.TryAppend(StringView(path.Data() + run, at - run)));
        if (at < path.ByteCount()) {
            CHECK(key.TryAppend("/"));
        }

        run = at + 1;
    }

    return key;
}

// 경로의 file URI(segment마다 percent-encoding, 드라이브 소문자 — 드라이브 `:`도 `%3A`)다.
inline String LspUriOf(Allocator& allocator, StringView path) noexcept {
    String uri(allocator);
    CHECK(uri.TryAssign("file://"));
    const String key = LspKeyOf(allocator, path);
    const StringView text = key.View();
    usize at = text.StartsWith("/") ? 1 : 0;
    while (at <= text.ByteCount()) {
        usize end = at;
        while (end < text.ByteCount() && text.Data()[end] != '/') {
            ++end;
        }

        const StringView segment(text.Data() + at, end - at);
        auto size = PercentEncodedSize(segment);
        Array<char> buffer(allocator);
        const bool sized = size && buffer.TryResize(size.Value());
        CHECK(sized);
        auto written = sized ? TryPercentEncode(segment, buffer.View())
                             : Result<usize>::Failure(ErrorCode::OutOfMemory);
        CHECK(written);
        if (!written) {
            return uri;
        }

        CHECK(uri.TryAppend("/"));
        CHECK(uri.TryAppend(StringView(buffer.Data(), written.Value())));
        if (end == text.ByteCount()) {
            break;
        }

        at = end + 1;
    }

    return uri;
}

// file URI의 경로 비교 모양이다(받지 않는 URI는 빈 글자).
inline String LspPathOfUri(Allocator& allocator, StringView uri) noexcept {
    String path(allocator);
    constexpr StringView Prefix = "file://";
    if (!uri.StartsWith(Prefix)) {
        return path;
    }

    const StringView encoded(uri.Data() + Prefix.ByteCount(), uri.ByteCount() - Prefix.ByteCount());
    Array<char> buffer(allocator);
    const bool sized = static_cast<bool>(buffer.TryResize(encoded.ByteCount()));
    CHECK(sized);
    auto written = sized ? TryPercentDecode(encoded, buffer.View())
                         : Result<usize>::Failure(ErrorCode::OutOfMemory);
    if (!written) {
        return path;
    }

    StringView decoded(buffer.Data(), written.Value());
    if (decoded.ByteCount() >= 3 && decoded.Data()[0] == '/' &&
        LspDrive(StringView(decoded.Data() + 1, decoded.ByteCount() - 1))) {
        decoded = StringView(decoded.Data() + 1, decoded.ByteCount() - 1);
    }

    return LspKeyOf(allocator, decoded);
}

// 사용자 임시 폴더 아래 공백·한글이 든 시험 폴더다(끝에 지운다).
class LspFolder final {
public:
    explicit LspFolder(Allocator& allocator) noexcept : root_(allocator), allocator_(&allocator) {
        char buffer[LspTestPathCapacity];
        usize bytes = 0;
        const bool found = static_cast<bool>(platform::GetStandardDirectory(
            platform::StandardDirectory::Temporary, buffer, LspTestPathCapacity, bytes));
        CHECK(found);
        StringBuilder<LspTestPathCapacity> text(allocator);
        text.AppendFormat("{}/rocks sparks {}-{}-{}", StringView(buffer, found ? bytes : 0),
                          LspTestHangul, platform::CurrentProcessId(), platform::MonotonicTicks());
        CHECK(!text.HasFailed());
        const String key = LspKeyOf(allocator, text.View());
        CHECK(root_.TryAssign(key.View()));
    }

    ~LspFolder() noexcept {
        static_cast<void>(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(),
                                                        root_.View().Data(), root_.ByteCount()));
    }

    LspFolder(const LspFolder&) = delete;
    LspFolder& operator=(const LspFolder&) = delete;

    StringView Root() const noexcept {
        return root_.View();
    }

    String PathOf(StringView relative) const noexcept {
        String path(*allocator_);
        CHECK(path.TryAssign(root_.View()));
        if (!relative.IsEmpty()) {
            CHECK(path.TryAppend("/"));
            CHECK(path.TryAppend(relative));
        }

        return path;
    }

    String UriOf(StringView relative) const noexcept {
        const String path = PathOf(relative);
        return LspUriOf(*allocator_, path.View());
    }

    bool Write(StringView relative, StringView content) const noexcept {
        platform::FileSystem& files = platform::PhysicalFileSystem();
        const String path = PathOf(relative);
        const StringView full = path.View();
        usize slash = full.ByteCount();
        while (slash > 0 && full.Data()[slash - 1] != '/') {
            --slash;
        }

        if (slash > 0 && !platform::MakeDirectoryTree(files, full.Data(), slash - 1)) {
            return false;
        }

        platform::File file;
        if (!file.Open(files, full.Data(), full.ByteCount(),
                       platform::FileOpenMode::CreateAlways)) {
            return false;
        }

        const platform::FileTransfer written = file.WriteAll(content.Data(), content.ByteCount());
        const bool closed = static_cast<bool>(file.Close());
        return static_cast<bool>(written.status) && closed;
    }

    bool Remove(StringView relative) const noexcept {
        const String path = PathOf(relative);
        return static_cast<bool>(
            platform::PhysicalFileSystem().Delete(path.View().Data(), path.ByteCount()));
    }

private:
    String root_;
    Allocator* allocator_;
};

// 토큰 data를 줄마다 `<줄>:<열> <길이> <종류>[ <modifier>...]`(절대 위치)로 바꾼다.
inline String LspTokenLines(Allocator& allocator, StringView output, int64 id) noexcept {
    Array<String> bodies(allocator);
    CHECK(LspBodies(output, bodies, allocator));
    String lines(allocator);
    for (const String& body : bodies) {
        JsonDocument document(allocator);
        const bool parsed = document.Parse(body.View()).Succeeded();
        CHECK(parsed);
        const JsonValue root = parsed ? document.Root() : JsonValue();
        if (!parsed || LspNumber(root.Find("id")) != id) {
            continue;
        }

        const JsonValue result = root.Find("result");
        const JsonValue data = result.IsObject() ? result.Find("data") : JsonValue();
        CHECK(data.IsArray());
        CHECK(data.Size() % 5 == 0);
        int64 line = 0;
        int64 character = 0;
        for (usize at = 0; at + 5 <= data.Size(); at += 5) {
            const int64 deltaLine = LspNumber(data.At(at));
            const int64 deltaStart = LspNumber(data.At(at + 1));
            line += deltaLine;
            character = deltaLine == 0 ? character + deltaStart : deltaStart;
            const int64 type = LspNumber(data.At(at + 3));
            const int64 modifiers = LspNumber(data.At(at + 4));
            LspAppendNumber(lines, line, allocator);
            CHECK(lines.TryAppend(":"));
            LspAppendNumber(lines, character, allocator);
            CHECK(lines.TryAppend(" "));
            LspAppendNumber(lines, LspNumber(data.At(at + 2)), allocator);
            CHECK(lines.TryAppend(" "));
            const usize typeCount = sizeof(LspTestTypes) / sizeof(LspTestTypes[0]);
            CHECK(type >= 0 && usize(type) < typeCount);
            CHECK(lines.TryAppend(type >= 0 && usize(type) < typeCount ? LspTestTypes[type]
                                                                       : StringView("?")));
            for (usize bit = 0; bit < sizeof(LspTestModifiers) / sizeof(LspTestModifiers[0]);
                 ++bit) {
                if ((modifiers & (int64(1) << bit)) != 0) {
                    CHECK(lines.TryAppend(" "));
                    CHECK(lines.TryAppend(LspTestModifiers[bit]));
                }
            }

            CHECK(modifiers < (int64(1) << 5));
            CHECK(lines.TryAppend("\n"));
        }
    }

    return lines;
}

// 파일 내용 전체를 읽는다(UTF-8이 아니면 false).
inline bool LspReadText(Allocator& allocator, StringView path, String& out) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    Array<char> bytes(allocator);
    const bool ok = file.Size(size) && bytes.TryResize(static_cast<usize>(size)) &&
                    file.ReadExact(bytes.Data(), bytes.Size()).status;
    const bool closed = static_cast<bool>(file.Close());
    return ok && closed && out.TryAssign(StringView(bytes.Data(), bytes.Size()));
}

inline String LspSchemaFolder(Allocator& allocator) noexcept {
    char engine[LspTestPathCapacity];
    auto found = FindEngineDirectory(Span<char>(engine, LspTestPathCapacity));
    CHECK(found);
    if (!found) {
        return String(allocator);
    }

    auto schema = Path::Combine(allocator, StringView(engine, found.Value()),
                                "Source/Developer/SparksTestHost/Schema");
    CHECK(schema);
    return schema ? Move(schema.Value()) : String(allocator);
}

// 시험 schema(Engine.sparkbind.json)를 folder 아래 relative 폴더로 복사한다.
inline bool LspCopySchema(Allocator& allocator, const LspFolder& folder,
                          StringView relative) noexcept {
    const String schema = LspSchemaFolder(allocator);
    String source(allocator);
    String text(allocator);
    String target(allocator);
    return source.TryAssign(schema.View()) && source.TryAppend("/Engine.sparkbind.json") &&
           LspReadText(allocator, source.View(), text) && target.TryAssign(relative) &&
           target.TryAppend("/Engine.sparkbind.json") && folder.Write(target.View(), text.View());
}

// 응답 id의 `result` JSON 글자(서버의 compact 출력 그대로)다. 없으면 빈 글자.
inline String LspResultText(Allocator& allocator, StringView output, int64 id) noexcept {
    Array<String> bodies(allocator);
    CHECK(LspBodies(output, bodies, allocator));
    StringBuilder<64> prefix(allocator);
    prefix.AppendFormat("{{\"jsonrpc\":\"2.0\",\"id\":{},\"result\":", id);
    String result(allocator);
    for (const String& body : bodies) {
        const StringView text = body.View();
        if (text.StartsWith(prefix.View(), StringComparison::CaseSensitive) && text.EndsWith("}")) {
            CHECK(result.TryAssign(StringView(text.Data() + prefix.View().ByteCount(),
                                              text.ByteCount() - prefix.View().ByteCount() - 1)));
        }
    }

    return result;
}

// ASCII 원문 text에서 needle의 occurrence번째(0부터) 자리의 LSP 위치(+ shift 글자)다.
inline LspTestPlace LspPlaceOf(StringView text, StringView needle, uint32 occurrence = 0,
                               uint32 shift = 0) noexcept {
    usize from = 0;
    usize at = StringView::NotFound;
    for (uint32 count = 0; count <= occurrence; ++count) {
        at = text.Find(needle, StringComparison::CaseSensitive, StringSearchDirection::FromStart,
                       from);
        CHECK(at != StringView::NotFound);
        if (at == StringView::NotFound) {
            return LspTestPlace{};
        }

        from = at + 1;
    }

    LspTestPlace place;
    for (usize index = 0; index < at; ++index) {
        if (text.Data()[index] == '\n') {
            ++place.line;
            place.character = 0;
        } else {
            ++place.character;
        }
    }

    place.character += shift;
    return place;
}

// 각본 하나의 모든 할당 실패 자리: 3으로 끝나거나 출력이 같다(실패를 삼켜 다른 결과를 내지
// 않는다). 새는 할당이 없다. 기록(표준 오류)은 할당 실패 때 빠질 수 있어 비교하지 않는다.
inline void LspSweepAllocations(SystemAllocator& backing, Span<const StringView> arguments,
                                StringView script, int32 cleanCode) noexcept {
    String baseline(backing);
    uint64 attempts = 0;
    uint64 failures = 0;
    for (uint64 failAt = 0; failAt == 0 || failAt <= attempts; ++failAt) {
        test::NFailAllocator allocator(backing);
        const uint64 before = backing.Stats().liveAllocations;
        {
            LspScriptChannel channel(backing, script, LspTestWholeChunk);
            if (failAt != 0) {
                allocator.FailOnAttempt(failAt);
            }

            const int32 code =
                RunLanguageServer(allocator, platform::PhysicalFileSystem(), arguments, channel);
            const uint64 used = allocator.Attempts();
            allocator.DisableFailure();
            if (failAt == 0) {
                attempts = used;
                CHECK(code == cleanCode);
                CHECK(baseline.TryAssign(channel.output.View()));
                CHECK(!baseline.IsEmpty());
            } else if (code == LspTestError) {
                ++failures;
            } else {
                CHECK(code == cleanCode);
                CHECK(channel.output.View() == baseline.View());
            }
        }

        if (failAt != 0) { // 첫 반복은 baseline을 backing에 남긴다
            CHECK(backing.Stats().liveAllocations == before);
        }
    }

    CHECK(failures > 0);
}

} // namespace rocks::test::sparks_lsp
