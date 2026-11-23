#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>

#include <Core/Build/BuildInfo.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/Memory.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>

// 셰이더 워커 메시지(Docs/Design/ShaderSystem.ko.md 11.1절 SC5) 인코딩이다. 최상위는 Record 하나,
// job·형식·결과 목록은 RecordList, 작은 튜플 목록(처리기 버전·hook 인자·값·도구 확인)은 Bytes 값
// 안의 Archive(u32 개수 + 항목)다. 읽기는 모든 필드의 wire tag와 값 끝을 확인하고, 손상은
// InvalidArgument로 모은다(할당 실패만 OutOfMemory).
namespace rocks::shader {

namespace worker_protocol_detail {

// schema·필드 Guid다(고정 — 바꾸면 ShaderWorkerMessageVersion을 올린다).
constexpr Guid RequestSchema{0x5253574952455155ull, 0x0000000000000001ull};
constexpr Guid ResponseSchema{0x525357524553504full, 0x0000000000000001ull};
constexpr Guid FormatSchema{0x52535746524d4154ull, 0x0000000000000001ull};
constexpr Guid JobSchema{0x525357494a4f4252ull, 0x0000000000000001ull};
constexpr Guid ResultSchema{0x525357524a4f4252ull, 0x0000000000000001ull};
constexpr uint32 SchemaVersion = 1;

constexpr uint64 FieldBase = 0x5253574649454c44ull;

constexpr Guid FieldId(uint64 index) noexcept {
    return Guid{FieldBase, index};
}

// 요청.
constexpr Guid KindField = FieldId(1);
constexpr Guid ToolTableField = FieldId(2);
constexpr Guid ToolchainField = FieldId(3);
constexpr Guid MathVersionField = FieldId(4);
constexpr Guid ProcessorsField = FieldId(5);
constexpr Guid FormatsField = FieldId(6);
constexpr Guid JobsField = FieldId(7);
// 형식 항목.
constexpr Guid FormatIdField = FieldId(10);
constexpr Guid FormatBytesField = FieldId(11);
// job 항목.
constexpr Guid JobKindField = FieldId(20);
constexpr Guid JobKeyField = FieldId(21);
constexpr Guid JobFormatField = FieldId(22);
constexpr Guid JobStageField = FieldId(23);
constexpr Guid JobEntryField = FieldId(24);
constexpr Guid JobSourceField = FieldId(25);
constexpr Guid JobHlslField = FieldId(26);
constexpr Guid JobOptimizationField = FieldId(27);
constexpr Guid JobDebugField = FieldId(28);
constexpr Guid JobMatrixField = FieldId(29);
constexpr Guid JobLayoutField = FieldId(30);
constexpr Guid JobHookField = FieldId(31);
// 응답.
constexpr Guid ResultsField = FieldId(40);
constexpr Guid ToolsField = FieldId(41);
constexpr Guid SystemToolsField = FieldId(42);
// 결과 항목.
constexpr Guid StatusField = FieldId(50);
constexpr Guid ValuesField = FieldId(51);
constexpr Guid DiagnosticsField = FieldId(52);

// 워커 메시지의 Record 한도다(파일 하나 1 GiB).
constexpr uint64 MaxOperations = uint64{64} * 1024 * 1024;
constexpr uint64 WorkPerByte = 8;
constexpr uint32 MaxFields = 64;
constexpr uint32 MaxListItems = 1u << 20;

RecordLimits Limits() noexcept {
    RecordLimits limits;
    limits.archive.maxTotalBytes = static_cast<usize>(MaxShaderWorkerFileBytes);
    limits.archive.maxValueBytes = static_cast<usize>(MaxShaderWorkerFileBytes);
    limits.archive.maxOperations = MaxOperations;
    limits.archive.maxWork = MaxShaderWorkerFileBytes * WorkPerByte;
    limits.maxFields = MaxFields;
    return limits;
}

RecordListLimits ListLimits() noexcept {
    RecordListLimits limits;
    limits.maxItems = MaxListItems;
    limits.maxPayloadBytes = static_cast<usize>(MaxShaderWorkerFileBytes);
    limits.maxItemBytes = static_cast<usize>(MaxShaderWorkerFileBytes);
    return limits;
}

Result<void> Failure(ErrorCode error) noexcept {
    return Result<void>::Failure(error);
}

// 쓰기 실패를 그대로 올린다.
template <class T>
Result<void> Check(const Result<T>& result) noexcept {
    return result ? Result<void>::Success() : Failure(result.Error());
}

// 읽기 실패는 할당 실패만 그대로 두고 나머지는 손상(InvalidArgument)이다.
ErrorCode Damaged(ErrorCode error) noexcept {
    return error == ErrorCode::OutOfMemory ? ErrorCode::OutOfMemory : ErrorCode::InvalidArgument;
}

// 필드 하나를 한 번에 쓰는 도구다.
class FieldBuilder final {
public:
    FieldBuilder(Allocator& allocator, Guid schema) noexcept
        : allocator_(allocator), builder_(allocator, schema, SchemaVersion, Limits()) {
    }

    template <class Write>
    Result<void> Add(Guid id, WireType type, Write&& write) noexcept {
        ArchiveWriter writer(allocator_, Limits().archive);
        if (auto written = write(writer); !written) {
            return written;
        }

        return builder_.AddField(id, type, writer.View());
    }

    Result<void> U32(Guid id, uint32 value) noexcept {
        return Add(id, WireType::U32,
                   [value](ArchiveWriter& w) noexcept { return w.WriteU32(value); });
    }

    Result<void> Bool(Guid id, bool value) noexcept {
        return Add(id, WireType::Bool,
                   [value](ArchiveWriter& w) noexcept { return w.WriteBool(value); });
    }

    Result<void> Text(Guid id, StringView value) noexcept {
        return Add(id, WireType::String,
                   [value](ArchiveWriter& w) noexcept { return w.WriteString(value); });
    }

    Result<void> Bytes(Guid id, Span<const uint8> value) noexcept {
        return Add(id, WireType::Bytes,
                   [value](ArchiveWriter& w) noexcept { return w.WriteBytes(value); });
    }

    Result<void> Digest(Guid id, const Sha256Digest& value) noexcept {
        return Bytes(id, Span<const uint8>(value.bytes, Sha256Digest::Bytes));
    }

    RecordBuilder& Builder() noexcept {
        return builder_;
    }

private:
    Allocator& allocator_;
    RecordBuilder builder_;
};

// 파일 = magic + 버전 + Record.
Result<void> Frame(const uint8 (&magic)[ShaderWorkerMagicBytes], Span<const uint8> record,
                   Array<uint8>& out) noexcept {
    out.Clear();
    if (record.Size() > MaxShaderWorkerFileBytes - ShaderWorkerPrefixBytes) {
        return Failure(ErrorCode::CapacityExceeded);
    }

    uint8 prefix[ShaderWorkerPrefixBytes];
    platform::MemoryCopy(prefix, magic, ShaderWorkerMagicBytes);
    const uint32 version = ShaderWorkerMessageVersion;
    for (usize index = 0; index < sizeof(uint32); ++index) {
        prefix[ShaderWorkerMagicBytes + index] = static_cast<uint8>(version >> (index * 8));
    }

    if (!out.TryReserve(ShaderWorkerPrefixBytes + record.Size()) || !out.TryAppend(prefix) ||
        !out.TryAppend(record)) {
        return Failure(ErrorCode::OutOfMemory);
    }

    return Result<void>::Success();
}

// 필드 값 reader다. wire tag가 다르면 손상이다.
Result<ArchiveReader> Value(RecordReader& record, Guid id, WireType type) noexcept {
    auto field = record.Find(id);
    if (!field || field.Value().wireType != static_cast<uint32>(type)) {
        return Result<ArchiveReader>::Failure(ErrorCode::InvalidArgument);
    }

    return record.OpenValue(field.Value());
}

template <class T, class Read>
Result<T> Single(RecordReader& record, Guid id, WireType type, Read&& read) noexcept {
    auto reader = Value(record, id, type);
    if (!reader) {
        return Result<T>::Failure(Damaged(reader.Error()));
    }

    auto value = read(reader.Value());
    if (!value || !reader.Value().AtEnd()) {
        return Result<T>::Failure(value ? ErrorCode::InvalidArgument : Damaged(value.Error()));
    }

    return value;
}

Result<uint32> ReadU32(RecordReader& record, Guid id) noexcept {
    return Single<uint32>(record, id, WireType::U32,
                          [](ArchiveReader& r) noexcept { return r.ReadU32(); });
}

Result<bool> ReadBool(RecordReader& record, Guid id) noexcept {
    return Single<bool>(record, id, WireType::Bool,
                        [](ArchiveReader& r) noexcept { return r.ReadBool(); });
}

Result<void> ReadText(RecordReader& record, Guid id, String& out) noexcept {
    auto text = Single<StringView>(record, id, WireType::String,
                                   [](ArchiveReader& r) noexcept { return r.ReadStringView(); });
    if (!text) {
        return Failure(text.Error());
    }

    return out.TryAssign(text.Value()) ? Result<void>::Success() : Failure(ErrorCode::OutOfMemory);
}

Result<Span<const uint8>> ReadBytesView(RecordReader& record, Guid id) noexcept {
    return Single<Span<const uint8>>(record, id, WireType::Bytes,
                                     [](ArchiveReader& r) noexcept { return r.ReadBytes(); });
}

Result<void> ReadBytes(RecordReader& record, Guid id, Array<uint8>& out) noexcept {
    auto bytes = ReadBytesView(record, id);
    if (!bytes) {
        return Failure(bytes.Error());
    }

    out.Clear();
    return out.TryAppend(bytes.Value()) ? Result<void>::Success() : Failure(ErrorCode::OutOfMemory);
}

Result<void> ReadDigest(RecordReader& record, Guid id, Sha256Digest& out) noexcept {
    auto bytes = ReadBytesView(record, id);
    if (!bytes) {
        return Failure(bytes.Error());
    }

    if (bytes.Value().Size() != Sha256Digest::Bytes) {
        return Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(out.bytes, bytes.Value().Data(), Sha256Digest::Bytes);
    return Result<void>::Success();
}

Result<void> ReadDigestFrom(ArchiveReader& reader, Sha256Digest& out) noexcept {
    auto bytes = reader.ReadBytes();
    if (!bytes || bytes.Value().Size() != Sha256Digest::Bytes) {
        return Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(out.bytes, bytes.Value().Data(), Sha256Digest::Bytes);
    return Result<void>::Success();
}

// Bytes 값 안의 Archive 목록(u32 개수 + 항목)을 읽는다. readItem(reader)가 항목 하나를 읽는다.
template <class ReadItem>
Result<void> ReadPacked(RecordReader& record, Guid id, ReadItem&& readItem) noexcept {
    auto bytes = ReadBytesView(record, id);
    if (!bytes) {
        return Failure(bytes.Error());
    }

    auto reader = ArchiveReader::Create(bytes.Value(), Limits().archive);
    if (!reader) {
        return Failure(Damaged(reader.Error()));
    }

    auto count = reader.Value().ReadU32();
    if (!count || count.Value() > MaxListItems) {
        return Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        if (auto item = readItem(reader.Value()); !item) {
            return Failure(Damaged(item.Error()));
        }
    }

    return reader.Value().AtEnd() ? Result<void>::Success() : Failure(ErrorCode::InvalidArgument);
}

template <class T>
Result<T*> Append(Array<T>& items) noexcept {
    return items.TryEmplaceBack(items.GetAllocator());
}

void WriteU32Le(Sha256& hasher, uint32 value) noexcept {
    uint8 bytes[sizeof(uint32)];
    for (usize index = 0; index < sizeof(bytes); ++index) {
        bytes[index] = static_cast<uint8>(value >> (index * 8));
    }

    hasher.Update(bytes);
}

void WriteU64Le(Sha256& hasher, uint64 value) noexcept {
    uint8 bytes[sizeof(uint64)];
    for (usize index = 0; index < sizeof(bytes); ++index) {
        bytes[index] = static_cast<uint8>(value >> (index * 8));
    }

    hasher.Update(bytes);
}

void WriteText(Sha256& hasher, const char* text) noexcept {
    const StringView view = text != nullptr ? StringView(text) : StringView();
    WriteU64Le(hasher, view.ByteCount());
    hasher.Update(Span<const uint8>(reinterpret_cast<const uint8*>(view.Data()), view.ByteCount()));
}

// 요청 job 항목 하나.
Result<void> EncodeJob(Allocator& allocator, const ShaderWorkerJob& job,
                       RecordListBuilder& list) noexcept {
    FieldBuilder fields(allocator, JobSchema);
    const auto packHooks = [&job](ArchiveWriter& w) noexcept -> Result<void> {
        if (auto count = w.WriteU32(static_cast<uint32>(job.hookArguments.Size())); !count) {
            return count;
        }

        for (const String& argument : job.hookArguments) {
            if (auto written = w.WriteString(argument.View()); !written) {
                return written;
            }
        }

        return Result<void>::Success();
    };
    const auto packHookBytes = [&](ArchiveWriter& w) noexcept -> Result<void> {
        ArchiveWriter inner(allocator, Limits().archive);
        if (auto packed = packHooks(inner); !packed) {
            return packed;
        }

        return w.WriteBytes(inner.View());
    };
    Result<void> step = fields.U32(JobKindField, static_cast<uint32>(job.kind));
    if (step) {
        step = fields.Digest(JobKeyField, job.key);
    }

    if (step) {
        step = fields.Text(JobFormatField, job.formatId.View());
    }

    if (step) {
        step = fields.U32(JobStageField, job.stage);
    }

    if (step) {
        step = fields.Text(JobEntryField, job.entry.View());
    }

    if (step) {
        step = fields.Text(JobSourceField, job.source.View());
    }

    if (step) {
        step = fields.Bool(JobHlslField, job.options.hlsl2021);
    }

    if (step) {
        step = fields.U32(JobOptimizationField, job.options.optimization);
    }

    if (step) {
        step = fields.Bool(JobDebugField, job.options.debugSymbols);
    }

    if (step) {
        step = fields.U32(JobMatrixField, job.options.matrixLayout);
    }

    if (step) {
        step = fields.Bytes(JobLayoutField, job.layout.View());
    }

    if (step) {
        step = fields.Add(JobHookField, WireType::Bytes, packHookBytes);
    }

    if (!step) {
        return step;
    }

    auto built = fields.Builder().Build();
    if (!built) {
        return Failure(built.Error());
    }

    return list.AddItem(built.Value().View());
}

Result<void> DecodeJob(RecordReader& item, ShaderWorkerJob& job) noexcept {
    auto kind = ReadU32(item, JobKindField);
    auto stage = ReadU32(item, JobStageField);
    auto hlsl = ReadBool(item, JobHlslField);
    auto optimization = ReadU32(item, JobOptimizationField);
    auto debug = ReadBool(item, JobDebugField);
    auto matrix = ReadU32(item, JobMatrixField);
    if (!kind || !stage || !hlsl || !optimization || !debug || !matrix ||
        kind.Value() > static_cast<uint32>(ShaderWorkerJobKind::Assert) ||
        optimization.Value() > 3 || matrix.Value() != 0) {
        return Failure(ErrorCode::InvalidArgument);
    }

    job.kind = static_cast<ShaderWorkerJobKind>(kind.Value());
    job.stage = stage.Value();
    job.options.hlsl2021 = hlsl.Value();
    job.options.optimization = static_cast<uint8>(optimization.Value());
    job.options.debugSymbols = debug.Value();
    job.options.matrixLayout = static_cast<uint8>(matrix.Value());
    Result<void> step = ReadDigest(item, JobKeyField, job.key);
    if (step) {
        step = ReadText(item, JobFormatField, job.formatId);
    }

    if (step) {
        step = ReadText(item, JobEntryField, job.entry);
    }

    if (step) {
        step = ReadText(item, JobSourceField, job.source);
    }

    if (step) {
        step = ReadBytes(item, JobLayoutField, job.layout);
    }

    if (!step) {
        return step;
    }

    return ReadPacked(item, JobHookField, [&job](ArchiveReader& r) noexcept -> Result<void> {
        auto text = r.ReadStringView();
        if (!text) {
            return Failure(text.Error());
        }

        auto argument = Append(job.hookArguments);
        if (!argument) {
            return Failure(ErrorCode::OutOfMemory);
        }

        return argument.Value()->TryAssign(text.Value());
    });
}

Result<void> EncodeResult(Allocator& allocator, const ShaderWorkerJobResult& result,
                          RecordListBuilder& list) noexcept {
    FieldBuilder fields(allocator, ResultSchema);
    const auto packValues = [&](ArchiveWriter& w) noexcept -> Result<void> {
        ArchiveWriter inner(allocator, Limits().archive);
        Result<void> step = inner.WriteU32(static_cast<uint32>(result.values.Size()));
        for (const ShaderWorkerValue& value : result.values) {
            if (step) {
                step = inner.WriteU32(value.id);
            }

            if (step) {
                step = inner.WriteBytes(value.bytes.View());
            }
        }

        if (!step) {
            return step;
        }

        return w.WriteBytes(inner.View());
    };
    Result<void> step = fields.U32(StatusField, static_cast<uint32>(result.status));
    if (step) {
        step = fields.Add(ValuesField, WireType::Bytes, packValues);
    }

    if (step) {
        step = fields.Text(DiagnosticsField, result.diagnostics.View());
    }

    if (!step) {
        return step;
    }

    auto built = fields.Builder().Build();
    if (!built) {
        return Failure(built.Error());
    }

    return list.AddItem(built.Value().View());
}

Result<void> DecodeResult(RecordReader& item, ShaderWorkerJobResult& result) noexcept {
    auto status = ReadU32(item, StatusField);
    if (!status ||
        status.Value() > static_cast<uint32>(ShaderWorkerJobStatus::UnsupportedCapability)) {
        return Failure(ErrorCode::InvalidArgument);
    }

    result.status = static_cast<ShaderWorkerJobStatus>(status.Value());
    if (auto text = ReadText(item, DiagnosticsField, result.diagnostics); !text) {
        return text;
    }

    return ReadPacked(item, ValuesField, [&result](ArchiveReader& r) noexcept -> Result<void> {
        auto id = r.ReadU32();
        auto bytes = r.ReadBytes();
        if (!id || !bytes) {
            return Failure(ErrorCode::InvalidArgument);
        }

        auto value = Append(result.values);
        if (!value) {
            return Failure(ErrorCode::OutOfMemory);
        }

        value.Value()->id = id.Value();
        return value.Value()->bytes.TryAppend(bytes.Value());
    });
}

// 최상위 Record를 연다(magic·버전 확인 포함).
Result<RecordReader> OpenMessage(Allocator& allocator, Span<const uint8> file,
                                 const uint8 (&magic)[ShaderWorkerMagicBytes],
                                 Guid schema) noexcept {
    auto version = ReadShaderWorkerMessageVersion(file, magic);
    if (!version) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }

    if (version.Value() != ShaderWorkerMessageVersion) {
        return Result<RecordReader>::Failure(ErrorCode::Unsupported);
    }

    auto record = RecordReader::Open(allocator, file.Mid(ShaderWorkerPrefixBytes), schema,
                                     SchemaVersion, SchemaVersion, Limits());
    if (!record) {
        return Result<RecordReader>::Failure(Damaged(record.Error()));
    }

    return record;
}

} // namespace worker_protocol_detail

const char* ShaderWorkerExitName(int32 code) noexcept {
    switch (static_cast<ShaderWorkerExit>(code)) {
    case ShaderWorkerExit::Success:
        return "Success";
    case ShaderWorkerExit::GeneralFailure:
        return "GeneralFailure";
    case ShaderWorkerExit::BadMessageVersion:
        return "BadMessageVersion";
    case ShaderWorkerExit::BadFormatVersion:
        return "BadFormatVersion";
    case ShaderWorkerExit::BadToolTable:
        return "BadToolTable";
    case ShaderWorkerExit::BadInputFile:
        return "BadInputFile";
    case ShaderWorkerExit::CannotDeleteInput:
        return "CannotDeleteInput";
    case ShaderWorkerExit::CannotSaveOutput:
        return "CannotSaveOutput";
    case ShaderWorkerExit::BadArguments:
        return "BadArguments";
    case ShaderWorkerExit::OutOfMemory:
        return "OutOfMemory";
    case ShaderWorkerExit::BadToolchain:
        return "BadToolchain";
    }

    return "unknown";
}

const char* ShaderWorkerExitDiagnostic(int32 code) noexcept {
    switch (static_cast<ShaderWorkerExit>(code)) {
    case ShaderWorkerExit::BadMessageVersion:
    case ShaderWorkerExit::BadFormatVersion:
    case ShaderWorkerExit::BadToolchain:
        return "ShaderCompileWorker was built from different sources; rebuild "
               "ShaderCompileWorker";
    case ShaderWorkerExit::BadToolTable:
        return "ShaderCompileWorker uses a shader tool table from a different build; rebuild "
               "ShaderCompileWorker";
    case ShaderWorkerExit::BadInputFile:
        return "ShaderCompileWorker could not read its input file";
    case ShaderWorkerExit::CannotDeleteInput:
        return "ShaderCompileWorker could not delete its input file";
    case ShaderWorkerExit::CannotSaveOutput:
        return "ShaderCompileWorker could not save its output file";
    case ShaderWorkerExit::BadArguments:
        return "ShaderCompileWorker was started with invalid arguments";
    case ShaderWorkerExit::OutOfMemory:
        return "ShaderCompileWorker ran out of memory";
    case ShaderWorkerExit::Success:
    case ShaderWorkerExit::GeneralFailure:
        break;
    }

    return "ShaderCompileWorker ended unexpectedly";
}

Sha256Digest ShaderToolTableSha256(Span<const ShaderTool> tools) noexcept {
    using namespace worker_protocol_detail;
    Sha256 hasher;
    WriteU64Le(hasher, tools.Size());
    for (const ShaderTool& tool : tools) {
        WriteText(hasher, tool.name);
        WriteText(hasher, tool.version);
        WriteU32Le(hasher, static_cast<uint32>(tool.origin));
        WriteText(hasher, tool.storeKey);
        WriteU64Le(hasher, tool.fileCount);
        for (usize index = 0; index < tool.fileCount; ++index) {
            const ShaderToolFile& file = tool.files[index];
            WriteText(hasher, file.path);
            hasher.Update(Span<const uint8>(file.sha256.bytes, Sha256Digest::Bytes));
            WriteU64Le(hasher, file.size);
            WriteU32Le(hasher, static_cast<uint32>(file.role));
            WriteU32Le(hasher, file.hasDigest ? 1u : 0u);
        }
    }

    return hasher.Finish();
}

Result<void> MakeShaderWorkerHeader(Span<const ShaderFormatDefinition* const> formats,
                                    ShaderWorkerHeader& out) noexcept {
    out.toolTable = ShaderToolTableSha256();
    out.toolchain = build::ToolchainFingerprint();
    out.mathVersion = deterministic::Version;
    out.processors.Clear();
    for (const ShaderFormatDefinition* format : formats) {
        ShaderWorkerProcessorVersion entry;
        platform::MemoryCopy(entry.formatId, format->id, sizeof(entry.formatId));
        entry.version = format->moduleVersion;
        if (!out.processors.TryPushBack(entry)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<void>::Success();
}

Result<uint32>
ReadShaderWorkerMessageVersion(Span<const uint8> file,
                               const uint8 (&magic)[ShaderWorkerMagicBytes]) noexcept {
    if (file.Size() < ShaderWorkerPrefixBytes) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < ShaderWorkerMagicBytes; ++index) {
        if (file[index] != magic[index]) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }
    }

    uint32 version = 0;
    for (usize index = 0; index < sizeof(uint32); ++index) {
        version |= static_cast<uint32>(file[ShaderWorkerMagicBytes + index]) << (index * 8);
    }

    return Result<uint32>::Success(version);
}

Result<void> EncodeShaderWorkerRequest(const ShaderWorkerRequest& request,
                                       Array<uint8>& out) noexcept {
    using namespace worker_protocol_detail;
    Allocator& allocator = out.GetAllocator();
    FieldBuilder fields(allocator, RequestSchema);
    const ShaderWorkerHeader& header = request.header;
    const auto packProcessors = [&](ArchiveWriter& w) noexcept -> Result<void> {
        ArchiveWriter inner(allocator, Limits().archive);
        Result<void> step = inner.WriteU32(static_cast<uint32>(header.processors.Size()));
        for (const ShaderWorkerProcessorVersion& processor : header.processors) {
            if (step) {
                step = inner.WriteString(StringView(processor.formatId));
            }

            if (step) {
                step = inner.WriteU32(processor.version);
            }
        }

        if (!step) {
            return step;
        }

        return w.WriteBytes(inner.View());
    };
    Result<void> step = fields.U32(KindField, static_cast<uint32>(request.kind));
    if (step) {
        step = fields.Digest(ToolTableField, header.toolTable);
    }

    if (step) {
        step = fields.Digest(ToolchainField, header.toolchain);
    }

    if (step) {
        step = fields.U32(MathVersionField, header.mathVersion);
    }

    if (step) {
        step = fields.Add(ProcessorsField, WireType::Bytes, packProcessors);
    }

    if (!step) {
        return step;
    }

    RecordListBuilder formats(allocator, ListLimits());
    for (const ShaderWorkerFormat& format : request.formats) {
        FieldBuilder item(allocator, FormatSchema);
        Result<void> added = item.Text(FormatIdField, format.id.View());
        if (added) {
            added = item.Bytes(FormatBytesField, format.bytes.View());
        }

        auto built = added ? item.Builder().Build() : Result<Array<uint8>>::Failure(added.Error());
        if (!built) {
            return Failure(built.Error());
        }

        if (auto appended = formats.AddItem(built.Value().View()); !appended) {
            return appended;
        }
    }

    RecordListBuilder jobs(allocator, ListLimits());
    for (const ShaderWorkerJob& job : request.jobs) {
        if (auto encoded = EncodeJob(allocator, job, jobs); !encoded) {
            return encoded;
        }
    }

    step = formats.Finish(fields.Builder(), FormatsField);
    if (step) {
        step = jobs.Finish(fields.Builder(), JobsField);
    }

    if (!step) {
        return step;
    }

    auto record = fields.Builder().Build();
    if (!record) {
        return Failure(record.Error());
    }

    return Frame(ShaderWorkerInputMagic, record.Value().View(), out);
}

Result<void> DecodeShaderWorkerRequest(Span<const uint8> file, ShaderWorkerRequest& out) noexcept {
    using namespace worker_protocol_detail;
    Allocator& allocator = out.jobs.GetAllocator();
    out.header.processors.Clear();
    out.formats.Clear();
    out.jobs.Clear();
    auto opened = OpenMessage(allocator, file, ShaderWorkerInputMagic, RequestSchema);
    if (!opened) {
        return Failure(opened.Error());
    }

    RecordReader& record = opened.Value();
    auto kind = ReadU32(record, KindField);
    auto math = ReadU32(record, MathVersionField);
    if (!kind || !math ||
        (kind.Value() != static_cast<uint32>(ShaderWorkerRequestKind::ToolQuery) &&
         kind.Value() != static_cast<uint32>(ShaderWorkerRequestKind::Compile))) {
        return Failure(ErrorCode::InvalidArgument);
    }

    out.kind = static_cast<ShaderWorkerRequestKind>(kind.Value());
    out.header.mathVersion = math.Value();
    Result<void> step = ReadDigest(record, ToolTableField, out.header.toolTable);
    if (step) {
        step = ReadDigest(record, ToolchainField, out.header.toolchain);
    }

    if (!step) {
        return step;
    }

    step = ReadPacked(record, ProcessorsField, [&out](ArchiveReader& r) noexcept -> Result<void> {
        auto id = r.ReadStringView();
        auto version = r.ReadU32();
        if (!id || !version || id.Value().ByteCount() >= MaxShaderFormatIdBytes) {
            return Failure(ErrorCode::InvalidArgument);
        }

        ShaderWorkerProcessorVersion entry;
        if (!id.Value().IsEmpty()) {
            platform::MemoryCopy(entry.formatId, id.Value().Data(), id.Value().ByteCount());
        }

        entry.version = version.Value();
        return out.header.processors.TryPushBack(entry);
    });
    if (!step) {
        return step;
    }

    auto formatsField = record.Find(FormatsField);
    auto jobsField = record.Find(JobsField);
    if (!formatsField || !jobsField) {
        return Failure(ErrorCode::InvalidArgument);
    }

    auto formats = RecordListReader::OpenField(record, formatsField.Value(), FormatSchema,
                                               SchemaVersion, SchemaVersion, ListLimits());
    if (!formats) {
        return Failure(Damaged(formats.Error()));
    }

    for (usize index = 0; index < formats.Value().ItemCount(); ++index) {
        auto item = formats.Value().OpenItem(index);
        auto format = Append(out.formats);
        if (!item || !format) {
            return Failure(item ? ErrorCode::OutOfMemory : Damaged(item.Error()));
        }

        Result<void> read = ReadText(item.Value(), FormatIdField, format.Value()->id);
        if (read) {
            read = ReadBytes(item.Value(), FormatBytesField, format.Value()->bytes);
        }

        if (!read) {
            return read;
        }
    }

    auto jobs = RecordListReader::OpenField(record, jobsField.Value(), JobSchema, SchemaVersion,
                                            SchemaVersion, ListLimits());
    if (!jobs) {
        return Failure(Damaged(jobs.Error()));
    }

    for (usize index = 0; index < jobs.Value().ItemCount(); ++index) {
        auto item = jobs.Value().OpenItem(index);
        auto job = Append(out.jobs);
        if (!item || !job) {
            return Failure(item ? ErrorCode::OutOfMemory : Damaged(item.Error()));
        }

        if (auto read = DecodeJob(item.Value(), *job.Value()); !read) {
            return Failure(Damaged(read.Error()));
        }
    }

    return Result<void>::Success();
}

Result<void> EncodeShaderWorkerResponse(const ShaderWorkerResponse& response,
                                        Array<uint8>& out) noexcept {
    using namespace worker_protocol_detail;
    Allocator& allocator = out.GetAllocator();
    FieldBuilder fields(allocator, ResponseSchema);
    const auto packTools = [&](ArchiveWriter& w) noexcept -> Result<void> {
        ArchiveWriter inner(allocator, Limits().archive);
        Result<void> step = inner.WriteU32(static_cast<uint32>(response.tools.Size()));
        for (const ShaderWorkerToolCheck& tool : response.tools) {
            if (step) {
                step = inner.WriteString(tool.name.View());
            }

            if (step) {
                step = inner.WriteU8(static_cast<uint8>(tool.state));
            }
        }

        if (!step) {
            return step;
        }

        return w.WriteBytes(inner.View());
    };
    const auto packSystem = [&](ArchiveWriter& w) noexcept -> Result<void> {
        ArchiveWriter inner(allocator, Limits().archive);
        Result<void> step = inner.WriteU32(static_cast<uint32>(response.systemTools.Size()));
        for (const ShaderWorkerSystemTool& tool : response.systemTools) {
            if (step) {
                step = inner.WriteString(tool.name.View());
            }

            if (step) {
                step = inner.WriteBool(tool.found);
            }

            if (step) {
                step = inner.WriteString(tool.version.View());
            }

            for (const Sha256Digest& digest : tool.executables) {
                if (step) {
                    step = inner.WriteBytes(Span<const uint8>(digest.bytes, Sha256Digest::Bytes));
                }
            }
        }

        if (!step) {
            return step;
        }

        return w.WriteBytes(inner.View());
    };
    Result<void> step = fields.Add(ToolsField, WireType::Bytes, packTools);
    if (step) {
        step = fields.Add(SystemToolsField, WireType::Bytes, packSystem);
    }

    if (!step) {
        return step;
    }

    RecordListBuilder results(allocator, ListLimits());
    for (const ShaderWorkerJobResult& result : response.jobs) {
        if (auto encoded = EncodeResult(allocator, result, results); !encoded) {
            return encoded;
        }
    }

    if (auto finished = results.Finish(fields.Builder(), ResultsField); !finished) {
        return finished;
    }

    auto record = fields.Builder().Build();
    if (!record) {
        return Failure(record.Error());
    }

    return Frame(ShaderWorkerOutputMagic, record.Value().View(), out);
}

Result<void> DecodeShaderWorkerResponse(Span<const uint8> file,
                                        ShaderWorkerResponse& out) noexcept {
    using namespace worker_protocol_detail;
    Allocator& allocator = out.jobs.GetAllocator();
    out.jobs.Clear();
    out.tools.Clear();
    out.systemTools.Clear();
    auto opened = OpenMessage(allocator, file, ShaderWorkerOutputMagic, ResponseSchema);
    if (!opened) {
        return Failure(opened.Error());
    }

    RecordReader& record = opened.Value();
    Result<void> step =
        ReadPacked(record, ToolsField, [&out](ArchiveReader& r) noexcept -> Result<void> {
            auto name = r.ReadStringView();
            auto state = r.ReadU8();
            if (!name || !state ||
                state.Value() > static_cast<uint8>(ShaderWorkerToolState::Failed)) {
                return Failure(ErrorCode::InvalidArgument);
            }

            auto tool = Append(out.tools);
            if (!tool) {
                return Failure(ErrorCode::OutOfMemory);
            }

            tool.Value()->state = static_cast<ShaderWorkerToolState>(state.Value());
            return tool.Value()->name.TryAssign(name.Value());
        });
    if (step) {
        step =
            ReadPacked(record, SystemToolsField, [&out](ArchiveReader& r) noexcept -> Result<void> {
                auto tool = Append(out.systemTools);
                if (!tool) {
                    return Failure(ErrorCode::OutOfMemory);
                }

                auto name = r.ReadStringView();
                auto found = r.ReadBool();
                auto version = r.ReadStringView();
                if (!name || !found || !version) {
                    return Failure(ErrorCode::InvalidArgument);
                }

                tool.Value()->found = found.Value();
                if (!tool.Value()->name.TryAssign(name.Value()) ||
                    !tool.Value()->version.TryAssign(version.Value())) {
                    return Failure(ErrorCode::OutOfMemory);
                }

                for (Sha256Digest& digest : tool.Value()->executables) {
                    if (auto read = ReadDigestFrom(r, digest); !read) {
                        return read;
                    }
                }

                return Result<void>::Success();
            });
    }

    if (!step) {
        return step;
    }

    auto resultsField = record.Find(ResultsField);
    if (!resultsField) {
        return Failure(ErrorCode::InvalidArgument);
    }

    auto results = RecordListReader::OpenField(record, resultsField.Value(), ResultSchema,
                                               SchemaVersion, SchemaVersion, ListLimits());
    if (!results) {
        return Failure(Damaged(results.Error()));
    }

    for (usize index = 0; index < results.Value().ItemCount(); ++index) {
        auto item = results.Value().OpenItem(index);
        auto result = Append(out.jobs);
        if (!item || !result) {
            return Failure(item ? ErrorCode::OutOfMemory : Damaged(item.Error()));
        }

        if (auto read = DecodeResult(item.Value(), *result.Value()); !read) {
            return Failure(Damaged(read.Error()));
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::shader
