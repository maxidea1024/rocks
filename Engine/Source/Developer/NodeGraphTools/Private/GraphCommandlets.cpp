#include "GraphTextInternal.h"

#include <Core/Text/NamePool.h>
#include <NodeGraph/GraphDiagnostic.h>
#include <NodeGraph/GraphReadView.h>
#include <NodeGraph/GraphSchema.h>
#include <NodeGraph/GraphValidation.h>
#include <NodeGraphTools/GraphCommandlets.h>
#include <NodeGraphTools/GraphDiff.h>
#include <NodeGraphTools/GraphText.h>
#include <Object/ObjectRegistry.h>
#include <Platform/File.h>

// 노드 그래프 commandlet이다(Docs/Design/NodeGraph.ko.md 9.4절 "commandlet"). 명령마다 등록표 묶음
// (GraphSession)을 만들어 문서 텍스트 파일을 읽고 쓴다.
namespace rocks::nodegraph {
namespace {

// commandlet 객체 Registry의 문맥 id다("NODEGRAP").
constexpr ContextId GraphCommandletObjects{0x4e4f444547524150ull, 1};

// 내부 오류 줄 버퍼 크기다(명령 이름 + 오류 이름 — 넘치면 잘린다).
constexpr usize InternalErrorLineBytes = 160;

// 문서 파일 크기 상한이다(JSON 읽기 기본 입력 한도와 같다 — JsonReadOptions::maxInputBytes).
constexpr uint64 MaxGraphDocumentBytes = 64ull * 1024ull * 1024ull;

constexpr StringView DumpUsage =
    "usage: graph-dump <file> [--semantic-only] [--records] [--out=<file>]\n";
constexpr StringView ValidateUsage = "usage: graph-validate <file>\n";
constexpr StringView DiffUsage = "usage: graph-diff <file-a> <file-b> [--semantic-only]\n";
constexpr StringView OutOption = "--out=";

Result<void> Write(CommandletOutput& output, CommandletStream stream, StringView text) noexcept {
    return output.write(output.context, stream, text);
}

Result<int32> Exit(CommandletOutput& output, CommandletStream stream, StringView text,
                   int32 code) noexcept {
    if (auto written = Write(output, stream, text); !written) {
        return Result<int32>::Failure(written.Error());
    }

    return Result<int32>::Success(code);
}

bool StartsWith(StringView text, StringView prefix) noexcept {
    return text.ByteCount() >= prefix.ByteCount() &&
           StringView(text.Data(), prefix.ByteCount()) == prefix;
}

// 명령 하나의 등록표 묶음이다: Name pool·노드 종류 등록표(문맥이 주면 그것, 아니면 새로), 리플렉션
// Registry(늘 새로), 텍스트 문맥.
class GraphSession {
public:
    GraphSession(Allocator& allocator, const GraphCommandletContext& context) noexcept
        : allocator_(allocator), external_(context.names != nullptr && context.registry != nullptr),
          ownNames_(allocator), ownRegistry_(allocator, ownNames_),
          names_(external_ ? *context.names : ownNames_),
          registry_(external_ ? *context.registry : ownRegistry_), types_(allocator),
          objects_(allocator, types_, GraphCommandletObjects, Options(names_)),
          text_(allocator, names_, objects_, objects_.OwnerAccess()) {
    }

    GraphSession(const GraphSession&) = delete;
    GraphSession& operator=(const GraphSession&) = delete;

    ~GraphSession() {
        static_cast<void>(objects_.Shutdown(objects_.OwnerAccess()));
        static_cast<void>(types_.Shutdown());
    }

    Result<void> Start(const GraphCommandletContext& context) noexcept {
        if ((context.names == nullptr) != (context.registry == nullptr)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (!external_) {
            if (auto initialized = ownNames_.Initialize(); !initialized) {
                return initialized;
            }

            if (auto initialized = ownRegistry_.Initialize(); !initialized) {
                return initialized;
            }

            if (context.setupNodes != nullptr) {
                if (auto registered = context.setupNodes(context.user, ownRegistry_); !registered) {
                    return registered;
                }
            }
        }

        if (context.setupTypes != nullptr) {
            return context.setupTypes(context.user, types_);
        }

        return Result<void>::Success();
    }

    Allocator& AllocatorRef() noexcept {
        return allocator_;
    }

    NamePool& Names() noexcept {
        return names_;
    }

    NodeTypeRegistry& Registry() noexcept {
        return registry_;
    }

    GraphTextContext& Text() noexcept {
        return text_;
    }

private:
    static RegistryOptions Options(NamePool& names) noexcept {
        RegistryOptions options;
        options.names = &names;
        return options;
    }

    Allocator& allocator_;
    bool external_;
    NamePool ownNames_;
    NodeTypeRegistry ownRegistry_;
    NamePool& names_;
    NodeTypeRegistry& registry_;
    TypeRegistry types_;
    ObjectRegistry objects_;
    GraphTextContext text_;
};

Result<int32> FileFailure(GraphSession& session, CommandletOutput& output, StringView command,
                          StringView action, StringView path,
                          platform::FileStatus status) noexcept {
    StringBuilder<> text(session.AllocatorRef());
    text << command << ": cannot " << action << " '" << path << "' (file code "
         << static_cast<int32>(status.code) << ", native error " << status.nativeError << ")\n";
    if (auto ok = text.Status(); !ok) {
        return Result<int32>::Failure(ok.Error());
    }

    return Exit(output, CommandletStream::Error, text.View(), GraphCommandInputError);
}

// 파일 전체를 읽는다. 실패하면 오류 줄을 쓰고 입력 오류 종료 코드다(성공하면 -1).
Result<int32> ReadFile(GraphSession& session, CommandletOutput& output, StringView command,
                       StringView path, Array<uint8>& bytes) noexcept {
    // 길이를 받는 Open은 경로 안의 NUL을 거부한다(잘린 경로로 다른 파일을 열지 않는다).
    platform::File file;
    if (const platform::FileStatus opened =
            file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                      platform::FileOpenMode::ReadExisting);
        !opened) {
        return FileFailure(session, output, command, "open", path, opened);
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = file.Size(size); !sized) {
        return FileFailure(session, output, command, "size", path, sized);
    }

    if (size > MaxGraphDocumentBytes) {
        StringBuilder<> text(session.AllocatorRef());
        text << command << ": '" << path << "' is too large\n";
        if (auto ok = text.Status(); !ok) {
            return Result<int32>::Failure(ok.Error());
        }

        return Exit(output, CommandletStream::Error, text.View(), GraphCommandInputError);
    }

    if (auto resized = bytes.TryResize(static_cast<usize>(size)); !resized) {
        return Result<int32>::Failure(resized.Error());
    }

    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    if (!read.status) {
        return FileFailure(session, output, command, "read", path, read.status);
    }

    if (const platform::FileStatus closed = file.Close(); !closed) {
        return FileFailure(session, output, command, "close", path, closed);
    }

    return Result<int32>::Success(-1);
}

Result<int32> WriteFile(GraphSession& session, CommandletOutput& output, StringView command,
                        StringView path, StringView text) noexcept {
    platform::File file;
    if (const platform::FileStatus opened =
            file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                      platform::FileOpenMode::CreateAlways);
        !opened) {
        return FileFailure(session, output, command, "create", path, opened);
    }

    const platform::FileTransfer written = file.WriteAll(text.Data(), text.ByteCount());
    if (!written.status) {
        return FileFailure(session, output, command, "write", path, written.status);
    }

    if (const platform::FileStatus closed = file.Close(); !closed) {
        return FileFailure(session, output, command, "close", path, closed);
    }

    return Result<int32>::Success(-1);
}

// 문서 파일을 읽는다. 형식 오류는 진단 줄들과 입력 오류 종료 코드다(성공하면 -1).
Result<int32> ReadDocument(GraphSession& session, CommandletOutput& output, StringView command,
                           StringView path, GraphDocument& document,
                           GraphDocumentLayout& layout) noexcept {
    Array<uint8> bytes(session.AllocatorRef());
    auto read = ReadFile(session, output, command, path, bytes);
    if (!read || read.Value() >= 0) {
        return read;
    }

    GraphTextDiagnostics diagnostics(session.AllocatorRef());
    const StringView text(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
    auto parsed = ReadGraphDocumentJson(session.Text(), text, document, layout, diagnostics);
    if (parsed) {
        return Result<int32>::Success(-1);
    }

    if (parsed.Error() != ErrorCode::InvalidArgument) {
        return Result<int32>::Failure(parsed.Error());
    }

    StringBuilder<> lines(session.AllocatorRef());
    for (const GraphTextDiagnostic& item : diagnostics.items) {
        lines << path << ":" << item.line << ":" << item.column << ": "
              << GraphTextCodeName(item.code) << ": " << item.message.View() << "\n";
    }

    if (auto ok = lines.Status(); !ok) {
        return Result<int32>::Failure(ok.Error());
    }

    return Exit(output, CommandletStream::Error, lines.View(), GraphCommandInputError);
}

StringView SeverityText(GraphDiagnosticSeverity severity) noexcept {
    switch (severity) {
    case GraphDiagnosticSeverity::Info:
        return "info";
    case GraphDiagnosticSeverity::Warning:
        return "warning";
    case GraphDiagnosticSeverity::Error:
        return "error";
    }

    return "error";
}

Result<GraphSession*> Begin(void* context, GraphSession& session) noexcept {
    const auto* graphContext = static_cast<const GraphCommandletContext*>(context);
    if (auto started = session.Start(*graphContext); !started) {
        return Result<GraphSession*>::Failure(started.Error());
    }

    return Result<GraphSession*>::Success(&session);
}

const GraphCommandletContext& ContextOf(void* context) noexcept {
    return *static_cast<const GraphCommandletContext*>(context);
}

Allocator& ContextAllocator(void* context) noexcept {
    return *static_cast<const GraphCommandletContext*>(context)->allocator;
}

// 명령 본문의 Result 실패(할당·등록 hook·문맥 오류 등)를 내부 오류 줄과 종료 코드 4로 바꾼다 —
// 1(차이· 위반)과 구별되게. 그 줄도 쓰지 못하면 실패를 그대로 돌려준다.
Result<int32> Finish(CommandletOutput& output, StringView command, Result<int32> result) noexcept {
    if (result) {
        return result;
    }

    char buffer[InternalErrorLineBytes] = {};
    FixedStringBuilder line(Span<char>(buffer, sizeof(buffer)));
    line << command << ": internal error (" << ErrorCodeName(result.Error()) << ")\n";
    if (auto written = Write(output, CommandletStream::Error, line.View()); !written) {
        return Result<int32>::Failure(result.Error());
    }

    return Result<int32>::Success(GraphCommandInternalError);
}

Result<int32> DumpBody(void* context, Span<const StringView> args,
                       CommandletOutput& output) noexcept {
    StringView input;
    StringView out;
    GraphTextWriteOptions options;
    for (const StringView& arg : args) {
        if (arg == "--semantic-only") {
            options.semanticOnly = true;
        } else if (arg == "--records") {
            options.records = true;
        } else if (StartsWith(arg, OutOption) && arg.ByteCount() > OutOption.ByteCount() &&
                   out.IsEmpty()) {
            out = StringView(arg.Data() + OutOption.ByteCount(),
                             arg.ByteCount() - OutOption.ByteCount());
        } else if (StartsWith(arg, "--") || !input.IsEmpty()) {
            // 모르는 옵션, 두 번째 입력이나 --out, 빈 --out=은 사용법 오류다.
            return Exit(output, CommandletStream::Error, DumpUsage, GraphCommandUsage);
        } else {
            input = arg;
        }
    }

    if (input.IsEmpty()) {
        return Exit(output, CommandletStream::Error, DumpUsage, GraphCommandUsage);
    }

    GraphSession session(ContextAllocator(context), ContextOf(context));
    if (auto begun = Begin(context, session); !begun) {
        return Result<int32>::Failure(begun.Error());
    }

    GraphDocument document(session.AllocatorRef());
    GraphDocumentLayout layout(session.AllocatorRef());
    auto read = ReadDocument(session, output, "graph-dump", input, document, layout);
    if (!read || read.Value() >= 0) {
        return read;
    }

    StringBuilder<> text(session.AllocatorRef());
    if (auto written = WriteGraphDocumentJson(session.Text(), document, &layout, options, text);
        !written) {
        return Result<int32>::Failure(written.Error());
    }

    if (out.IsEmpty()) {
        return Exit(output, CommandletStream::Output, text.View(), GraphCommandSuccess);
    }

    auto saved = WriteFile(session, output, "graph-dump", out, text.View());
    if (!saved || saved.Value() >= 0) {
        return saved;
    }

    return Result<int32>::Success(GraphCommandSuccess);
}

Result<int32> ValidateBody(void* context, Span<const StringView> args,
                           CommandletOutput& output) noexcept {
    if (args.Size() != 1 || StartsWith(args[0], "--")) {
        return Exit(output, CommandletStream::Error, ValidateUsage, GraphCommandUsage);
    }

    GraphSession session(ContextAllocator(context), ContextOf(context));
    if (auto begun = Begin(context, session); !begun) {
        return Result<int32>::Failure(begun.Error());
    }

    GraphDocument document(session.AllocatorRef());
    GraphDocumentLayout layout(session.AllocatorRef());
    auto read = ReadDocument(session, output, "graph-validate", args[0], document, layout);
    if (!read || read.Value() >= 0) {
        return read;
    }

    Allocator& allocator = session.AllocatorRef();
    GraphDiagnosticList diagnostics(allocator);
    GraphViolationList violations(allocator);
    if (auto valid =
            ValidateGraphDocument(document, &layout, session.Registry(), allocator, violations);
        !valid) {
        if (valid.Error() != ErrorCode::InvalidArgument) {
            return Result<int32>::Failure(valid.Error());
        }

        if (auto appended = AppendViolationDiagnostics(violations, Guid{}, diagnostics);
            !appended) {
            return Result<int32>::Failure(appended.Error());
        }
    }

    // 도메인 schema 검사는 공통 불변식을 지킨 문서에만 한다(GraphReadView는 정규·유효 문서를 전제).
    const GraphSchema* schema = session.Registry().FindSchema(document.domain);
    if (schema != nullptr && diagnostics.items.IsEmpty()) {
        const GraphReadView view(document, session.Registry());
        if (auto validated = schema->Validate(view, diagnostics); !validated) {
            return Result<int32>::Failure(validated.Error());
        }
    } else if (schema == nullptr) {
        if (auto added = diagnostics.Add(GraphDiagnosticSeverity::Error,
                                         GraphDomainNotRegisteredCode, Guid{}, Guid{}, Guid{},
                                         Name(), "document domain is not registered");
            !added) {
            return Result<int32>::Failure(added.Error());
        }
    }

    StringBuilder<> lines(allocator);
    bool errors = false;
    for (const GraphDiagnostic& item : diagnostics.items) {
        errors = errors || item.severity == GraphDiagnosticSeverity::Error;
        lines << SeverityText(item.severity) << " " << item.code << " graph "
              << text_detail::FormatGuidText(item.graph).View() << " node "
              << text_detail::FormatGuidText(item.node).View();
        if (!(item.pin == Name())) {
            auto pin = session.Names().Acquire(item.pin);
            if (!pin) {
                return Result<int32>::Failure(pin.Error());
            }

            lines << " pin " << pin.Value().View();
        }

        lines << ": " << item.message.View() << "\n";
    }

    if (auto ok = lines.Status(); !ok) {
        return Result<int32>::Failure(ok.Error());
    }

    if (!lines.IsEmpty()) {
        if (auto written = Write(output, CommandletStream::Output, lines.View()); !written) {
            return Result<int32>::Failure(written.Error());
        }
    }

    return Result<int32>::Success(errors ? GraphCommandDifferent : GraphCommandSuccess);
}

Result<int32> DiffBody(void* context, Span<const StringView> args,
                       CommandletOutput& output) noexcept {
    StringView files[2];
    usize count = 0;
    GraphDiffOptions options;
    for (const StringView& arg : args) {
        if (arg == "--semantic-only") {
            options.semanticOnly = true;
        } else if (StartsWith(arg, "--") || count == 2) {
            return Exit(output, CommandletStream::Error, DiffUsage, GraphCommandUsage);
        } else {
            files[count++] = arg;
        }
    }

    if (count != 2) {
        return Exit(output, CommandletStream::Error, DiffUsage, GraphCommandUsage);
    }

    GraphSession session(ContextAllocator(context), ContextOf(context));
    if (auto begun = Begin(context, session); !begun) {
        return Result<int32>::Failure(begun.Error());
    }

    Allocator& allocator = session.AllocatorRef();
    GraphDocument before(allocator);
    GraphDocumentLayout beforeLayout(allocator);
    GraphDocument after(allocator);
    GraphDocumentLayout afterLayout(allocator);
    auto first = ReadDocument(session, output, "graph-diff", files[0], before, beforeLayout);
    if (!first || first.Value() >= 0) {
        return first;
    }

    auto second = ReadDocument(session, output, "graph-diff", files[1], after, afterLayout);
    if (!second || second.Value() >= 0) {
        return second;
    }

    Array<GraphDiffEntry> entries(allocator);
    if (auto computed = ComputeGraphDiff(before, &beforeLayout, after, &afterLayout,
                                         session.Names(), options, entries);
        !computed) {
        return Result<int32>::Failure(computed.Error());
    }

    StringBuilder<> lines(allocator);
    if (auto formatted = FormatGraphDiff(session.Names(), entries.View(), lines); !formatted) {
        return Result<int32>::Failure(formatted.Error());
    }

    if (!lines.IsEmpty()) {
        if (auto written = Write(output, CommandletStream::Output, lines.View()); !written) {
            return Result<int32>::Failure(written.Error());
        }
    }

    return Result<int32>::Success(entries.IsEmpty() ? GraphCommandSuccess : GraphCommandDifferent);
}

} // namespace

Result<int32> RunGraphDumpCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept {
    return Finish(output, "graph-dump", DumpBody(context, args, output));
}

Result<int32> RunGraphValidateCommand(void* context, Span<const StringView> args,
                                      CommandletOutput& output) noexcept {
    return Finish(output, "graph-validate", ValidateBody(context, args, output));
}

Result<int32> RunGraphDiffCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept {
    return Finish(output, "graph-diff", DiffBody(context, args, output));
}

Result<void> RegisterGraphCommandlets(CommandletRegistry& registry,
                                      GraphCommandletContext& context) noexcept {
    // Name pool과 노드 종류 등록표는 둘 다 주거나 둘 다 비운다.
    if (context.allocator == nullptr ||
        (context.names == nullptr) != (context.registry == nullptr)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto registered =
            registry.Register({"graph-dump", "Write a node graph document text in canonical form.",
                               &RunGraphDumpCommand, &context});
        !registered) {
        return registered;
    }

    if (auto registered = registry.Register({"graph-validate", "Check a node graph document text.",
                                             &RunGraphValidateCommand, &context});
        !registered) {
        return registered;
    }

    return registry.Register({"graph-diff", "List the differences of two node graph documents.",
                              &RunGraphDiffCommand, &context});
}

} // namespace rocks::nodegraph
