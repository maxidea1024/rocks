#include <Core/Algorithms.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Path.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/Formatter.h>
#include <SparksTools/LanguageServer.h>
#include "LspFraming.h"
#include "LspServer.h"
#include "LspText.h"
#include "Workspace.h"

// 언어 서버의 수명·문서 저장소·분석·진단 발행·요청
// 분기다(Docs/Design/SparksTools.ko.md 10.3–10.5절).
namespace rocks::sparks::tools {

namespace {

using namespace detail;

// JSON-RPC·LSP 오류 코드다(10.2절).
constexpr int64 LspParseError = -32700;
constexpr int64 LspInvalidRequest = -32600;
constexpr int64 LspMethodNotFound = -32601;
constexpr int64 LspInvalidParams = -32602;
constexpr int64 LspServerNotInitialized = -32002;

// 종료 코드(RunCommand와 같은 뜻 — 10.3절)다.
constexpr int32 LspExitClean = 0;
constexpr int32 LspExitUnclean = 1;
constexpr int32 LspExitUsage = 2;
constexpr int32 LspExitError = 3;

constexpr usize LspJsonInlineBytes = 1024;
constexpr usize LspLogInlineBytes = 256;
constexpr uint32 MaxLspDocuments = 4096;
constexpr usize LspPathBytes = 4096; // 작업 디렉터리 경로 버퍼
constexpr int64 LspSyncFull = 1;
constexpr int64 LspSeverityError = 1;
constexpr StringView LspServerName = "sparks-lsp";
constexpr StringView LspSource = "sparks";
constexpr StringView LspFixPrefix = "\nfix: ";
constexpr StringView LspEmptyList = "[]";
constexpr StringView LspSourceExtension = ".spark";
constexpr StringView LspUriMessage = "the request needs 'textDocument.uri'";
constexpr StringView LspPositionMessage = "the request needs 'textDocument.uri' and 'position'";
constexpr StringView LspUsage = "usage: sparks-lsp [--stdio] [--schema <folder>]...\n";

// JsonWriter 호출의 결과를 버리는 감싸개다 — writer는 첫 오류를 기억하고 Finish가 돌려준다.
class LspOut final {
public:
    explicit LspOut(JsonWriter& writer) noexcept : writer_(writer) {
    }

    LspOut& Object() noexcept {
        static_cast<void>(writer_.BeginObject());
        return *this;
    }

    LspOut& End() noexcept {
        static_cast<void>(writer_.EndObject());
        return *this;
    }

    LspOut& Array() noexcept {
        static_cast<void>(writer_.BeginArray());
        return *this;
    }

    LspOut& EndArray() noexcept {
        static_cast<void>(writer_.EndArray());
        return *this;
    }

    LspOut& Key(StringView key) noexcept {
        static_cast<void>(writer_.Key(key));
        return *this;
    }

    LspOut& Text(StringView value) noexcept {
        static_cast<void>(writer_.String(value));
        return *this;
    }

    LspOut& Number(int64 value) noexcept {
        static_cast<void>(writer_.Int64(value));
        return *this;
    }

    LspOut& Flag(bool value) noexcept {
        static_cast<void>(writer_.Bool(value));
        return *this;
    }

    LspOut& Null() noexcept {
        static_cast<void>(writer_.Null());
        return *this;
    }

    LspOut& Value(JsonValue value) noexcept {
        static_cast<void>(writer_.Value(value));
        return *this;
    }

    LspOut& Position(LspPosition position) noexcept {
        return Object()
            .Key("line")
            .Number(position.line)
            .Key("character")
            .Number(position.character)
            .End();
    }

private:
    JsonWriter& writer_;
};

// 열린 문서 하나다.
struct LspDocument {
    explicit LspDocument(Allocator& allocator) noexcept
        : uri(allocator), key(allocator), text(allocator) {
    }

    String uri;
    String key; // 경로 비교 key(file URI가 아니면 빈 글자)
    String text;
    int64 version = 0;
};

// 지난 발행 하나(URI와 진단 배열 JSON)다.
struct LspPublished {
    explicit LspPublished(Allocator& allocator) noexcept : uri(allocator), json(allocator) {
    }

    String uri;
    String json;
};

// 분석 한 번의 결과다(10.4절).
struct LspAnalysis {
    explicit LspAnalysis(Allocator& allocator) noexcept
        : workspace(allocator), sources(allocator), diagnostics(allocator),
          compilation(allocator, sources), modules(allocator), packages(allocator),
          keys(allocator) {
    }

    Workspace workspace;
    SourceSet sources;
    DiagnosticList diagnostics;
    Compilation compilation;
    Array<uint32> modules;
    Array<uint32> packages;
    Array<String> keys; // SourceSet 번호마다 경로 key(작업 공간 파일이 아니면 빈 글자)
    bool compiled = false;
};

class LspServer final {
public:
    LspServer(Allocator& allocator, platform::FileSystem& files, MessageChannel& channel,
              Span<const StringView> schemas) noexcept
        : allocator_(allocator), files_(files), channel_(channel), schemas_(allocator),
          roots_(allocator), documents_(allocator), published_(allocator), lastProblem_(allocator) {
        // 명령줄 schema 폴더는 작업 디렉터리 기준이다(발행 URI가 절대 경로여야 한다).
        for (const StringView schema : schemas) {
            outOfMemory_ = outOfMemory_ || !AddSchema(schema, {});
        }
    }

    bool RanOutOfMemory() const noexcept {
        return outOfMemory_;
    }

    int32 ExitCode() const noexcept {
        return shutdown_ ? LspExitClean : LspExitUnclean;
    }

    // 메시지 하나를 처리한다. 값 false는 exit(ExitCode). 실패: 입출력·할당.
    Result<bool> Handle(StringView body) noexcept;

private:
    template <typename Fill>
    Result<void> Send(Fill&& fill) noexcept;
    Result<void> Respond(JsonValue id, JsonValue result) noexcept;
    Result<void> Fail(JsonValue id, int64 code, StringView message) noexcept;
    void Log(StringView line) noexcept;

    Result<void> Initialize(JsonValue id, JsonValue params) noexcept;
    Result<void> SemanticTokens(JsonValue id, JsonValue params) noexcept;
    Result<void> Hover(JsonValue id, JsonValue params) noexcept;
    Result<void> Definition(JsonValue id, JsonValue params) noexcept;
    Result<void> Formatting(JsonValue id, JsonValue params) noexcept;
    Result<uint32> ModuleOfUri(StringView uri) noexcept;
    Result<bool> UriOfSource(uint32 source, String& uri) noexcept;
    static bool PositionParams(JsonValue params, StringView& uri, LspPosition& position) noexcept;
    Result<void> Opened(JsonValue params) noexcept;
    Result<void> Changed(JsonValue params) noexcept;
    Result<void> Closed(JsonValue params) noexcept;
    Result<void> Analyze() noexcept;
    Result<void> Publish() noexcept;
    Result<void> AddRoot(StringView uri) noexcept;
    Result<void> AddSchema(StringView folder, StringView base) noexcept;
    LspDocument* FindDocument(StringView uri) noexcept;
    Result<void> AppendDiagnostics(LspOut& out, uint32 source) noexcept;

    Allocator& allocator_;
    platform::FileSystem& files_;
    MessageChannel& channel_;
    Array<String> schemas_;
    Array<String> roots_;
    Array<LspDocument> documents_;
    Array<LspPublished> published_;
    UniquePtr<LspAnalysis> analysis_;
    String lastProblem_; // 마지막으로 기록한 작업 공간 읽기 실패(같은 문구는 다시 쓰지 않는다)
    bool initialized_ = false;
    bool shutdown_ = false;
    bool outOfMemory_ = false;
};

template <typename Fill>
Result<void> LspServer::Send(Fill&& fill) noexcept {
    StringBuilder<LspJsonInlineBytes> text(allocator_);
    JsonWriterOptions options;
    options.compact = true;
    JsonWriter writer(text, options);
    LspOut out(writer);
    out.Object().Key("jsonrpc").Text("2.0");
    if (auto filled = fill(out); !filled) {
        return filled;
    }

    out.End();
    if (auto finished = writer.Finish(); !finished) {
        return finished;
    }

    StringView body = text.View();
    while (body.EndsWith("\n")) {
        body = StringView(body.Data(), body.ByteCount() - 1);
    }

    return LspWriteFrame(channel_, allocator_, body);
}

Result<void> LspServer::Respond(JsonValue id, JsonValue result) noexcept {
    return Send([&](LspOut& out) noexcept {
        out.Key("id").Value(id).Key("result");
        if (result.IsValid()) {
            out.Value(result);
        } else {
            out.Null();
        }

        return Result<void>::Success();
    });
}

Result<void> LspServer::Fail(JsonValue id, int64 code, StringView message) noexcept {
    return Send([&](LspOut& out) noexcept {
        out.Key("id");
        if (id.IsValid()) {
            out.Value(id);
        } else {
            out.Null();
        }

        out.Key("error").Object().Key("code").Number(code).Key("message").Text(message).End();
        return Result<void>::Success();
    });
}

void LspServer::Log(StringView line) noexcept {
    StringBuilder<LspLogInlineBytes> text(allocator_);
    text.AppendFormat("{}: {}\n", LspServerName, line);
    if (!text.HasFailed()) {
        channel_.Log(text.View());
    }
}

LspDocument* LspServer::FindDocument(StringView uri) noexcept {
    for (LspDocument& document : documents_) {
        if (document.uri.View() == uri) {
            return &document;
        }
    }

    return nullptr;
}

Result<void> LspServer::AddRoot(StringView uri) noexcept {
    String path(allocator_);
    auto converted = LspPathFromUri(uri, path);
    if (!converted) {
        return Result<void>::Failure(converted.Error());
    }

    if (!converted.Value()) {
        Log("a workspace folder is not a file URI; it is ignored");
        return Result<void>::Success();
    }

    return roots_.TryPushBack(Move(path));
}

Result<void> LspServer::AddSchema(StringView folder, StringView base) noexcept {
    const bool absolute = folder.StartsWith("/") || folder.StartsWith("\\") ||
                          (folder.ByteCount() >= 2 && folder.Data()[1] == ':');
    String full(allocator_);
    if (!absolute) {
        // 기준이 없으면 작업 디렉터리다. 그것도 못 얻으면 상대 경로 그대로(발행 URI는 틀린다).
        char working[LspPathBytes];
        usize bytes = 0;
        StringView prefix = base;
        if (prefix.IsEmpty() && platform::GetStandardDirectory(platform::StandardDirectory::Working,
                                                               working, LspPathBytes, bytes)) {
            prefix = StringView(working, bytes);
        }

        if (!prefix.IsEmpty() && (!full.TryAssign(prefix) || !full.TryAppend("/"))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    if (!full.TryAppend(folder)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    return schemas_.TryPushBack(Move(full));
}

Result<void> LspServer::Initialize(JsonValue id, JsonValue params) noexcept {
    if (initialized_) {
        return Fail(id, LspInvalidRequest, "the server is already initialized");
    }

    if (!params.IsObject()) {
        return Fail(id, LspInvalidParams, "'initialize' needs an object of parameters");
    }

    const JsonValue folders = params.Find("workspaceFolders");
    const JsonValue root = params.Find("rootUri");
    if (folders.IsValid() && folders.IsArray()) {
        for (usize at = 0; at < folders.Size(); ++at) {
            const JsonValue uri =
                folders.At(at).IsObject() ? folders.At(at).Find("uri") : JsonValue();
            if (uri.IsValid() && uri.IsString()) {
                if (auto added = AddRoot(uri.GetString()); !added) {
                    return added;
                }
            }
        }
    } else if (root.IsValid() && root.IsString()) {
        if (auto added = AddRoot(root.GetString()); !added) {
            return added;
        }
    }

    // 초기화 옵션의 schema 폴더(첫 작업 공간 뿌리 기준 — 뿌리가 없으면 작업 디렉터리 — 또는 절대
    // 경로).
    const JsonValue options = params.Find("initializationOptions");
    const JsonValue schemas =
        options.IsValid() && options.IsObject() ? options.Find("schemas") : JsonValue();
    if (schemas.IsValid() && schemas.IsArray()) {
        for (usize at = 0; at < schemas.Size(); ++at) {
            if (!schemas.At(at).IsString()) {
                continue;
            }

            const StringView base = roots_.IsEmpty() ? StringView() : roots_[0].View();
            if (auto added = AddSchema(schemas.At(at).GetString(), base); !added) {
                return added;
            }
        }
    }

    if (roots_.IsEmpty()) {
        Log("no workspace folder is open; documents are not analyzed");
    }

    initialized_ = true;
    return Send([&](LspOut& out) noexcept {
        out.Key("id").Value(id).Key("result").Object().Key("capabilities").Object();
        out.Key("positionEncoding").Text("utf-16");
        out.Key("textDocumentSync")
            .Object()
            .Key("openClose")
            .Flag(true)
            .Key("change")
            .Number(LspSyncFull)
            .End();
        out.Key("hoverProvider").Flag(true).Key("definitionProvider").Flag(true);
        out.Key("documentFormattingProvider").Flag(true);
        out.Key("semanticTokensProvider").Object().Key("legend").Object();
        out.Key("tokenTypes").Array();
        for (const StringView type : LspTokenTypes) {
            out.Text(type);
        }

        out.EndArray().Key("tokenModifiers").Array();
        for (const StringView modifier : LspTokenModifiers) {
            out.Text(modifier);
        }

        out.EndArray().End().Key("full").Flag(true).End();
        out.End().Key("serverInfo").Object().Key("name").Text(LspServerName).End().End();
        return Result<void>::Success();
    });
}

Result<void> LspServer::Opened(JsonValue params) noexcept {
    const JsonValue document = params.IsObject() ? params.Find("textDocument") : JsonValue();
    const JsonValue uri =
        document.IsValid() && document.IsObject() ? document.Find("uri") : JsonValue();
    const JsonValue text =
        document.IsValid() && document.IsObject() ? document.Find("text") : JsonValue();
    const JsonValue version =
        document.IsValid() && document.IsObject() ? document.Find("version") : JsonValue();
    if (!uri.IsValid() || !uri.IsString() || !text.IsValid() || !text.IsString()) {
        Log("'textDocument/didOpen' has no uri or text; it is ignored");
        return Result<void>::Success();
    }

    LspDocument* existing = FindDocument(uri.GetString());
    if (existing == nullptr) {
        if (documents_.Size() >= MaxLspDocuments) {
            Log("too many open documents; the document is ignored");
            return Result<void>::Success();
        }

        if (auto pushed = documents_.TryPushBack(LspDocument(allocator_)); !pushed) {
            return pushed;
        }

        existing = &documents_[documents_.Size() - 1];
        String path(allocator_);
        auto converted = LspPathFromUri(uri.GetString(), path);
        if (!converted) {
            return Result<void>::Failure(converted.Error());
        }

        if (!existing->uri.TryAssign(uri.GetString()) ||
            (converted.Value() && !LspPathKey(path.View(), existing->key))) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    auto number =
        version.IsValid() && version.IsNumber() ? version.TryGetInt64() : Result<int64>::Success(0);
    existing->version = number ? number.Value() : 0;
    if (auto assigned = existing->text.TryAssign(text.GetString()); !assigned) {
        return assigned;
    }

    return Analyze();
}

Result<void> LspServer::Changed(JsonValue params) noexcept {
    const JsonValue document = params.IsObject() ? params.Find("textDocument") : JsonValue();
    const JsonValue uri =
        document.IsValid() && document.IsObject() ? document.Find("uri") : JsonValue();
    const JsonValue changes = params.IsObject() ? params.Find("contentChanges") : JsonValue();
    LspDocument* existing =
        uri.IsValid() && uri.IsString() ? FindDocument(uri.GetString()) : nullptr;
    if (existing == nullptr || !changes.IsValid() || !changes.IsArray() || changes.Size() == 0) {
        Log("'textDocument/didChange' names no open document or has no change; it is ignored");
        return Result<void>::Success();
    }

    // 전체 내용 동기화(change 1)라 마지막 변경의 text가 새 내용이다.
    const JsonValue last = changes.At(changes.Size() - 1);
    const JsonValue text = last.IsObject() ? last.Find("text") : JsonValue();
    if (!text.IsValid() || !text.IsString() || last.Find("range").IsValid()) {
        Log("'textDocument/didChange' is not a full-content change; it is ignored");
        return Result<void>::Success();
    }

    const JsonValue version = document.Find("version");
    auto number =
        version.IsValid() && version.IsNumber() ? version.TryGetInt64() : Result<int64>::Success(0);
    existing->version = number ? number.Value() : existing->version;
    if (auto assigned = existing->text.TryAssign(text.GetString()); !assigned) {
        return assigned;
    }

    return Analyze();
}

Result<void> LspServer::Closed(JsonValue params) noexcept {
    const JsonValue document = params.IsObject() ? params.Find("textDocument") : JsonValue();
    const JsonValue uri =
        document.IsValid() && document.IsObject() ? document.Find("uri") : JsonValue();
    if (!uri.IsValid() || !uri.IsString()) {
        Log("'textDocument/didClose' has no uri; it is ignored");
        return Result<void>::Success();
    }

    for (usize at = 0; at < documents_.Size(); ++at) {
        if (documents_[at].uri.View() == uri.GetString()) {
            for (usize next = at + 1; next < documents_.Size(); ++next) {
                documents_[next - 1] = Move(documents_[next]);
            }

            static_cast<void>(documents_.TryPopBack());
            return Analyze();
        }
    }

    return Result<void>::Success();
}

Result<void> LspServer::Analyze() noexcept {
    // 작업 공간이 없으면 분석하지 않는다(10.3절) — 발행한 진단도 없으므로 지울 것도 없다.
    if (roots_.IsEmpty()) {
        return Result<void>::Success();
    }

    auto made = UniquePtr<LspAnalysis>::TryMake(allocator_, allocator_);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    LspAnalysis& analysis = *made.Value();
    {
        Array<StringView> paths(allocator_);
        Array<StringView> schemas(allocator_);
        for (const String& root : roots_) {
            if (auto pushed = paths.TryPushBack(root.View()); !pushed) {
                return pushed;
            }
        }

        for (const String& schema : schemas_) {
            if (auto pushed = schemas.TryPushBack(schema.View()); !pushed) {
                return pushed;
            }
        }

        StringBuilder<LspLogInlineBytes> problem(allocator_);
        bool ioFailure = false;
        auto loaded =
            analysis.workspace.Load(files_, paths.View(), schemas.View(), problem, ioFailure);
        if (!loaded) {
            return Result<void>::Failure(loaded.Error());
        }

        if (problem.HasFailed()) {
            return Result<void>::Failure(problem.Status().Error());
        }

        // 읽기 실패는 문구가 바뀔 때만 기록한다(문서 알림마다 같은 줄을 쓰지 않게).
        const StringView reason = loaded.Value() ? StringView() : problem.View();
        if (reason != lastProblem_.View()) {
            if (!reason.IsEmpty()) {
                Log(reason);
            }

            if (auto assigned = lastProblem_.TryAssign(reason); !assigned) {
                return assigned;
            }
        }

        analysis.compiled = loaded.Value();
    }

    if (analysis.compiled) {
        auto keyOf = [](StringView path, String& key) noexcept { return LspPathKey(path, key); };
        for (const LspDocument& document : documents_) {
            if (document.key.IsEmpty()) {
                continue;
            }

            auto overlaid =
                analysis.workspace.Overlay(document.key.View(), document.text.View(), keyOf);
            if (!overlaid) {
                return Result<void>::Failure(overlaid.Error());
            }
        }

        if (auto added =
                analysis.workspace.AddTo(analysis.sources, analysis.compilation,
                                         analysis.diagnostics, analysis.modules, analysis.packages);
            !added) {
            return added;
        }

        if (auto checked = analysis.compilation.Check(analysis.diagnostics); !checked) {
            return checked;
        }

        analysis.diagnostics.Sort(analysis.sources);
        // SourceSet 번호마다 작업 공간 파일의 경로 key(진단 경로로 맞춘다).
        for (uint32 source = 0; source < analysis.sources.Count(); ++source) {
            if (auto pushed = analysis.keys.TryPushBack(String(allocator_)); !pushed) {
                return pushed;
            }
        }

        for (uint32 source = 0; source < analysis.sources.Count(); ++source) {
            const StringView display = analysis.sources.Get(SourceId{source}).Path();
            for (const WorkspaceFile& file : analysis.workspace.Files()) {
                if (file.path.View() == display) {
                    if (auto made = LspPathKey(file.full.View(), analysis.keys[source]); !made) {
                        return made;
                    }

                    break;
                }
            }
        }
    }

    analysis_ = Move(made.Value());
    return Publish();
}

Result<void> LspServer::AppendDiagnostics(LspOut& out, uint32 source) noexcept {
    const LspAnalysis& analysis = *analysis_;
    const SourceFile& file = analysis.sources.Get(SourceId{source});
    out.Array();
    for (const Diagnostic& diagnostic : analysis.diagnostics.View()) {
        if (diagnostic.Source().index != source) {
            continue;
        }

        LspPosition begin;
        LspPosition end;
        if (diagnostic.HasSpan()) {
            begin = LspPositionOf(file, diagnostic.Span().begin);
            end = LspPositionOf(file, diagnostic.Span().end);
        }

        StringBuilder<DiagnosticCodeDigits + 2> code(allocator_);
        StringBuilder<LspJsonInlineBytes> message(allocator_);
        message.Append(diagnostic.Message());
        if (!diagnostic.Explanation().IsEmpty()) {
            message.Append("\n").Append(diagnostic.Explanation());
        }

        if (!diagnostic.Fix().IsEmpty()) {
            message.Append(LspFixPrefix).Append(diagnostic.Fix());
        }

        if (auto appended = AppendDiagnosticCode(code, static_cast<uint32>(diagnostic.Code()));
            !appended) {
            return appended;
        }

        if (message.HasFailed()) {
            return Result<void>::Failure(message.Status().Error());
        }

        out.Object().Key("range").Object().Key("start").Position(begin).Key("end").Position(end);
        out.End().Key("severity").Number(LspSeverityError).Key("code").Text(code.View());
        out.Key("source").Text(LspSource).Key("message").Text(message.View()).End();
    }

    out.EndArray();
    return Result<void>::Success();
}

Result<void> LspServer::Publish() noexcept {
    // 이번 발행 대상: 작업 공간 파일과 열린 문서(URI byte 순).
    struct Target {
        explicit Target(Allocator& allocator) noexcept : uri(allocator), json(allocator) {
        }

        String uri;
        String json;
        uint32 source = NumericLimits<uint32>::Max();
        const LspDocument* document = nullptr;
        bool clear = false; // 지난 발행에만 있던 URI의 지움
    };

    Array<Target> targets(allocator_);
    Array<uint8> covered(allocator_); // 문서마다 작업 공간 파일 대상이 있는지
    if (auto sized = covered.TryResize(documents_.Size()); !sized) {
        return sized;
    }

    for (uint8& flag : covered) {
        flag = 0;
    }

    const LspAnalysis& analysis = *analysis_;
    for (uint32 source = 0; source < analysis.keys.Size(); ++source) {
        if (analysis.keys[source].IsEmpty()) {
            continue;
        }

        if (auto pushed = targets.TryPushBack(Target(allocator_)); !pushed) {
            return pushed;
        }

        Target& target = targets[targets.Size() - 1];
        target.source = source;
        for (usize at = 0; at < documents_.Size(); ++at) {
            if (documents_[at].key.View() == analysis.keys[source].View()) {
                target.document = &documents_[at];
                covered[at] = 1;
            }
        }

        if (target.document != nullptr) {
            if (auto assigned = target.uri.TryAssign(target.document->uri.View()); !assigned) {
                return assigned;
            }
        } else if (auto made = LspUriFromPath(analysis.keys[source].View(), target.uri); !made) {
            return made;
        }
    }

    for (usize at = 0; at < documents_.Size(); ++at) {
        if (covered[at] != 0) {
            continue;
        }

        if (auto pushed = targets.TryPushBack(Target(allocator_)); !pushed) {
            return pushed;
        }

        Target& target = targets[targets.Size() - 1];
        target.document = &documents_[at];
        if (auto assigned = target.uri.TryAssign(documents_[at].uri.View()); !assigned) {
            return assigned;
        }
    }

    for (Target& target : targets) {
        StringBuilder<LspJsonInlineBytes> text(allocator_);
        JsonWriterOptions options;
        options.compact = true;
        JsonWriter writer(text, options);
        LspOut out(writer);
        if (target.source != NumericLimits<uint32>::Max()) {
            if (auto appended = AppendDiagnostics(out, target.source); !appended) {
                return appended;
            }
        } else {
            out.Array().EndArray();
        }

        if (auto finished = writer.Finish(); !finished) {
            return finished;
        }

        StringView json = text.View();
        while (json.EndsWith("\n")) {
            json = StringView(json.Data(), json.ByteCount() - 1);
        }

        if (auto assigned = target.json.TryAssign(json); !assigned) {
            return assigned;
        }
    }

    // 지난 발행에만 있던 URI(지운 파일·닫은 새 문서·다른 철자로 다시 연 문서)는 빈 목록 대상이다.
    // 지움을 먼저 보낸다: 두 철자를 한 key로 보는 클라이언트에서 지움이 새 진단을 덮지 않게.
    for (const LspPublished& entry : published_) {
        bool present = false;
        for (const Target& target : targets) {
            present = present || target.uri.View() == entry.uri.View();
        }

        if (present) {
            continue;
        }

        if (auto pushed = targets.TryPushBack(Target(allocator_)); !pushed) {
            return pushed;
        }

        Target& target = targets[targets.Size() - 1];
        target.clear = true;
        if (!target.uri.TryAssign(entry.uri.View()) || !target.json.TryAssign(LspEmptyList)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    algo::Sort(targets.View(), [](const Target& left, const Target& right) noexcept {
        if (left.clear != right.clear) {
            return left.clear;
        }

        return StringView::CompareBytes(left.uri.View(), right.uri.View()) < 0;
    });

    // published_에는 빈 목록이 아닌 URI만 둔다(없음 = 빈 목록).
    for (const Target& target : targets) {
        LspPublished* previous = nullptr;
        for (LspPublished& entry : published_) {
            previous = entry.uri.View() == target.uri.View() ? &entry : previous;
        }

        const bool same = previous != nullptr ? previous->json.View() == target.json.View()
                                              : target.json.View() == LspEmptyList;
        if (same) {
            continue;
        }

        if (auto sent = Send([&](LspOut& out) noexcept {
                out.Key("method").Text("textDocument/publishDiagnostics").Key("params").Object();
                out.Key("uri").Text(target.uri.View());
                if (target.document != nullptr) {
                    out.Key("version").Number(target.document->version);
                }

                // 미리 만든 배열 JSON을 값으로 다시 읽어 넣는다(같은 byte가 나온다).
                JsonDocument parsed(allocator_);
                const JsonError error = parsed.Parse(target.json.View());
                if (!error.Succeeded()) {
                    return Result<void>::Failure(error.code == JsonErrorCode::OutOfMemory
                                                     ? ErrorCode::OutOfMemory
                                                     : ErrorCode::InvalidArgument);
                }

                out.Key("diagnostics").Value(parsed.Root()).End();
                return Result<void>::Success();
            });
            !sent) {
            return sent;
        }

        if (target.json.View() == LspEmptyList) {
            // previous는 있다(없었으면 same). 표에서 뺀다.
            const usize at = static_cast<usize>(previous - published_.Data());
            for (usize next = at + 1; next < published_.Size(); ++next) {
                published_[next - 1] = Move(published_[next]);
            }

            static_cast<void>(published_.TryPopBack());
            continue;
        }

        if (previous == nullptr) {
            if (auto pushed = published_.TryPushBack(LspPublished(allocator_)); !pushed) {
                return pushed;
            }

            previous = &published_[published_.Size() - 1];
            if (auto assigned = previous->uri.TryAssign(target.uri.View()); !assigned) {
                return assigned;
            }
        }

        if (auto assigned = previous->json.TryAssign(target.json.View()); !assigned) {
            return assigned;
        }
    }

    return Result<void>::Success();
}

Result<void> LspServer::SemanticTokens(JsonValue id, JsonValue params) noexcept {
    const JsonValue document = params.IsObject() ? params.Find("textDocument") : JsonValue();
    const JsonValue uri =
        document.IsValid() && document.IsObject() ? document.Find("uri") : JsonValue();
    if (!uri.IsValid() || !uri.IsString()) {
        return Fail(id, LspInvalidParams, LspUriMessage);
    }

    auto module = ModuleOfUri(uri.GetString());
    if (!module) {
        return Result<void>::Failure(module.Error());
    }

    Array<uint32> data(allocator_);
    if (module.Value() != NoModule) {
        const LspAnalysis& analysis = *analysis_;
        if (auto appended = LspAppendSemanticTokens(analysis.compilation, analysis.sources,
                                                    module.Value(), data);
            !appended) {
            return appended;
        }
    }

    return Send([&](LspOut& out) noexcept {
        out.Key("id").Value(id).Key("result").Object().Key("data").Array();
        for (const uint32 value : data) {
            out.Number(value);
        }

        out.EndArray().End();
        return Result<void>::Success();
    });
}

Result<uint32> LspServer::ModuleOfUri(StringView uri) noexcept {
    // 문서의 모듈(URI의 경로 key)을 마지막 분석에서 찾는다.
    String path(allocator_);
    String key(allocator_);
    auto converted = LspPathFromUri(uri, path);
    if (!converted) {
        return Result<uint32>::Failure(converted.Error());
    }

    if (converted.Value()) {
        if (auto made = LspPathKey(path.View(), key); !made) {
            return Result<uint32>::Failure(made.Error());
        }
    }

    if (!analysis_ || !analysis_->compiled || key.IsEmpty()) {
        return Result<uint32>::Success(NoModule);
    }

    const LspAnalysis& analysis = *analysis_;
    for (uint32 module = 0; module < analysis.compilation.ModuleCount(); ++module) {
        const uint32 source = analysis.compilation.ModuleSource(module).index;
        if (source < analysis.keys.Size() && analysis.keys[source].View() == key.View()) {
            return Result<uint32>::Success(module);
        }
    }

    return Result<uint32>::Success(NoModule);
}

Result<bool> LspServer::UriOfSource(uint32 source, String& uri) noexcept {
    const LspAnalysis& analysis = *analysis_;
    if (source >= analysis.keys.Size() || analysis.keys[source].IsEmpty()) {
        return Result<bool>::Success(false);
    }

    for (const LspDocument& document : documents_) {
        if (document.key.View() == analysis.keys[source].View()) {
            if (auto assigned = uri.TryAssign(document.uri.View()); !assigned) {
                return Result<bool>::Failure(assigned.Error());
            }

            return Result<bool>::Success(true);
        }
    }

    if (auto made = LspUriFromPath(analysis.keys[source].View(), uri); !made) {
        return Result<bool>::Failure(made.Error());
    }

    return Result<bool>::Success(true);
}

bool LspServer::PositionParams(JsonValue params, StringView& uri, LspPosition& position) noexcept {
    const JsonValue document = params.IsObject() ? params.Find("textDocument") : JsonValue();
    const JsonValue text =
        document.IsValid() && document.IsObject() ? document.Find("uri") : JsonValue();
    const JsonValue place = params.IsObject() ? params.Find("position") : JsonValue();
    if (!text.IsValid() || !text.IsString() || !place.IsValid() || !place.IsObject()) {
        return false;
    }

    const JsonValue line = place.Find("line");
    const JsonValue character = place.Find("character");
    if (!line.IsValid() || !line.IsNumber() || !character.IsValid() || !character.IsNumber()) {
        return false;
    }

    auto lineNumber = line.TryGetUInt64();
    auto characterNumber = character.TryGetUInt64();
    if (!lineNumber || !characterNumber || lineNumber.Value() > NumericLimits<uint32>::Max() ||
        characterNumber.Value() > NumericLimits<uint32>::Max()) {
        return false;
    }

    uri = text.GetString();
    position.line = static_cast<uint32>(lineNumber.Value());
    position.character = static_cast<uint32>(characterNumber.Value());
    return true;
}

Result<void> LspServer::Hover(JsonValue id, JsonValue params) noexcept {
    StringView uri;
    LspPosition position;
    if (!PositionParams(params, uri, position)) {
        return Fail(id, LspInvalidParams, LspPositionMessage);
    }

    auto module = ModuleOfUri(uri);
    if (!module) {
        return Result<void>::Failure(module.Error());
    }

    if (module.Value() == NoModule) {
        return Respond(id, JsonValue());
    }

    const LspAnalysis& analysis = *analysis_;
    const SourceFile& file =
        analysis.sources.Get(analysis.compilation.ModuleSource(module.Value()));
    LspNamed named;
    if (!LspNameAt(analysis.compilation, module.Value(), LspOffsetOf(file, position), named)) {
        return Respond(id, JsonValue());
    }

    StringBuilder<LspJsonInlineBytes> value(allocator_);
    auto shown = LspAppendHover(analysis.compilation, analysis.sources, named, value);
    if (!shown) {
        return Result<void>::Failure(shown.Error());
    }

    if (!shown.Value()) {
        return Respond(id, JsonValue());
    }

    const SourceSpan span =
        analysis.compilation.ModuleTree(module.Value())->Tokens()[named.token].span;
    return Send([&](LspOut& out) noexcept {
        out.Key("id").Value(id).Key("result").Object().Key("contents").Object();
        out.Key("kind").Text("markdown").Key("value").Text(value.View()).End();
        out.Key("range").Object().Key("start").Position(LspPositionOf(file, span.begin));
        out.Key("end").Position(LspPositionOf(file, span.end)).End().End();
        return Result<void>::Success();
    });
}

Result<void> LspServer::Definition(JsonValue id, JsonValue params) noexcept {
    StringView uri;
    LspPosition position;
    if (!PositionParams(params, uri, position)) {
        return Fail(id, LspInvalidParams, LspPositionMessage);
    }

    auto module = ModuleOfUri(uri);
    if (!module) {
        return Result<void>::Failure(module.Error());
    }

    if (module.Value() == NoModule) {
        return Respond(id, JsonValue());
    }

    const LspAnalysis& analysis = *analysis_;
    const SourceFile& file =
        analysis.sources.Get(analysis.compilation.ModuleSource(module.Value()));
    LspNamed named;
    SourceId source;
    SourceSpan span;
    if (!LspNameAt(analysis.compilation, module.Value(), LspOffsetOf(file, position), named) ||
        !LspDefinition(analysis.compilation, named, source, span)) {
        return Respond(id, JsonValue());
    }

    String target(allocator_);
    auto made = UriOfSource(source.index, target);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    if (!made.Value()) {
        return Respond(id, JsonValue());
    }

    const SourceFile& destination = analysis.sources.Get(source);
    return Send([&](LspOut& out) noexcept {
        out.Key("id").Value(id).Key("result").Object().Key("uri").Text(target.View());
        out.Key("range").Object().Key("start").Position(LspPositionOf(destination, span.begin));
        out.Key("end").Position(LspPositionOf(destination, span.end)).End().End();
        return Result<void>::Success();
    });
}

Result<void> LspServer::Formatting(JsonValue id, JsonValue params) noexcept {
    const JsonValue document = params.IsObject() ? params.Find("textDocument") : JsonValue();
    const JsonValue uri =
        document.IsValid() && document.IsObject() ? document.Find("uri") : JsonValue();
    if (!uri.IsValid() || !uri.IsString()) {
        return Fail(id, LspInvalidParams, LspUriMessage);
    }

    // 열린 `.spark` 문서만(11.5절). 옵션은 받고 무시한다(단일 공식 서식).
    const LspDocument* open = FindDocument(uri.GetString());
    if (open == nullptr ||
        !open->key.View().EndsWith(LspSourceExtension, StringComparison::CaseSensitive)) {
        return Respond(id, JsonValue());
    }

    SourceSet sources(allocator_);
    DiagnosticList diagnostics(allocator_);
    auto added = sources.TryAdd(open->key.View(), open->text.View(), diagnostics);
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    const SourceFile& file = sources.Get(added.Value());
    StringBuilder<LspJsonInlineBytes> formatted(allocator_);
    bool changed = false;
    if (file.IsUsable()) {
        auto done = FormatSource(allocator_, sources, added.Value(), diagnostics, formatted);
        if (!done) {
            return Result<void>::Failure(done.Error());
        }

        // BOM 뒤 내용과 비교한다(formatter는 BOM을 뗀다 — 편집 범위도 BOM 뒤부터다).
        const StringView content(file.Text().Data() + file.ContentBegin(),
                                 file.Text().ByteCount() - file.ContentBegin());
        changed = done.Value() && formatted.View() != content;
    }

    const LspPosition end = LspPositionOf(file, static_cast<uint32>(file.Text().ByteCount()));
    return Send([&](LspOut& out) noexcept {
        out.Key("id").Value(id).Key("result").Array();
        if (changed) {
            out.Object().Key("range").Object().Key("start").Position(LspPosition());
            out.Key("end").Position(end).End().Key("newText").Text(formatted.View()).End();
        }

        out.EndArray();
        return Result<void>::Success();
    });
}

Result<bool> LspServer::Handle(StringView body) noexcept {
    JsonDocument document(allocator_);
    const JsonError error = document.Parse(body);
    if (error.code == JsonErrorCode::OutOfMemory) {
        return Result<bool>::Failure(ErrorCode::OutOfMemory);
    }

    auto done = [](Result<void> result) noexcept {
        return result ? Result<bool>::Success(true) : Result<bool>::Failure(result.Error());
    };
    if (!error.Succeeded()) {
        return done(Fail(JsonValue(), LspParseError, "the message is not JSON"));
    }

    const JsonValue root = document.Root();
    const JsonValue method = root.IsObject() ? root.Find("method") : JsonValue();
    const JsonValue id = root.IsObject() ? root.Find("id") : JsonValue();
    const bool validId = id.IsValid() && (id.IsString() || id.IsNumber());
    if (root.IsObject() && !method.IsValid() && id.IsValid() &&
        (root.Find("result").IsValid() || root.Find("error").IsValid())) {
        return Result<bool>::Success(true); // 클라이언트 응답(서버는 요청을 보내지 않는다)
    }

    if (!root.IsObject() || !method.IsValid() || !method.IsString() || (id.IsValid() && !validId)) {
        return done(Fail(validId ? id : JsonValue(), LspInvalidRequest,
                         "the message is not a JSON-RPC request or notification"));
    }

    const StringView name = method.GetString();
    const JsonValue params = root.Find("params");
    const bool request = id.IsValid();
    if (!request) {
        if (name == "exit") {
            return Result<bool>::Success(false);
        }

        if (!initialized_ || shutdown_) {
            return Result<bool>::Success(true);
        }

        if (name == "initialized") {
            return done(Analyze());
        }

        if (name == "textDocument/didOpen") {
            return done(Opened(params));
        }

        if (name == "textDocument/didChange") {
            return done(Changed(params));
        }

        if (name == "textDocument/didClose") {
            return done(Closed(params));
        }

        return Result<bool>::Success(true); // 모르는 알림·`$/` 알림은 무시한다
    }

    if (name == "initialize") {
        return done(Initialize(id, params));
    }

    if (!initialized_) {
        return done(Fail(id, LspServerNotInitialized, "the server is not initialized"));
    }

    if (shutdown_) {
        return done(Fail(id, LspInvalidRequest, "the server is shutting down"));
    }

    if (name == "shutdown") {
        shutdown_ = true;
        return done(Respond(id, JsonValue()));
    }

    if (name == "textDocument/semanticTokens/full") {
        return done(SemanticTokens(id, params));
    }

    if (name == "textDocument/hover") {
        return done(Hover(id, params));
    }

    if (name == "textDocument/definition") {
        return done(Definition(id, params));
    }

    if (name == "textDocument/formatting") {
        return done(Formatting(id, params));
    }

    return done(Fail(id, LspMethodNotFound, "the method is not supported"));
}

} // namespace

int32 RunLanguageServer(Allocator& allocator, platform::FileSystem& files,
                        Span<const StringView> arguments, MessageChannel& channel) noexcept {
    Array<StringView> schemas(allocator);
    for (usize at = 0; at < arguments.Size(); ++at) {
        if (arguments[at] == "--stdio") {
            continue;
        }

        if (arguments[at] == "--schema" && at + 1 < arguments.Size()) {
            if (!schemas.TryPushBack(arguments[++at])) {
                return LspExitError;
            }

            continue;
        }

        StringBuilder<LspLogInlineBytes> text(allocator);
        text.AppendFormat("{}: unknown argument '{}'\n", LspServerName, arguments[at]);
        text.Append(LspUsage);
        if (!text.HasFailed()) {
            channel.Log(text.View());
        }

        return LspExitUsage;
    }

    LspServer server(allocator, files, channel, schemas.View());
    if (server.RanOutOfMemory()) {
        return LspExitError;
    }

    LspReader reader(allocator);
    for (;;) {
        StringView body;
        StringView problem;
        auto frame = reader.Next(channel, body, problem);
        if (!frame) {
            return LspExitError;
        }

        if (frame.Value() != LspFrame::Message) {
            StringBuilder<LspLogInlineBytes> text(allocator);
            text.AppendFormat("{}: {}\n", LspServerName,
                              frame.Value() == LspFrame::End ? StringView("the input ended "
                                                                          "without 'exit'")
                                                             : problem);
            if (!text.HasFailed()) {
                channel.Log(text.View());
            }

            return LspExitUnclean;
        }

        auto handled = server.Handle(body);
        if (!handled) {
            return LspExitError;
        }

        if (!handled.Value()) {
            return server.ExitCode();
        }
    }
}

} // namespace rocks::sparks::tools
