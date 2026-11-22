#include "NuggetInternal.h"

#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Compression/CompressedBlocks.h>
#include <Core/Algorithms.h>
#include <Core/FileError.h>
#include <Core/Guid.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>

// 파일 시스템 에셋 너겟 backend다(Docs/Design/AssetSystem.ko.md 9.1절 결정 7–9 —
// NuggetFileSystemBackend.h).
namespace rocks {

namespace {

constexpr usize KeyHexBytes = Sha256Digest::HexBytes;
constexpr usize DirectoryHexBytes = 2;
// Guid 표준 문자열 길이다(36자).
constexpr usize GuidTextBytes = 36;
constexpr StringView EntryExtension = ".rnug";
constexpr StringView TemporaryExtension = ".tmp";
constexpr StringView LockExtension = ".lock";
constexpr StringView TemporaryDirectory = "tmp";
constexpr int64 TicksPerDay = 24ll * 60 * 60 * platform::FileTicksPerSecond;
// Exists 판정에서 값 byte를 흘려 읽는 버퍼 크기다.
constexpr usize VerifyBufferBytes = 64 * 1024;

// 파일의 범위 읽기다(열린 동안 크기는 연 때의 값).
class FileReader final : public NuggetRangeReader {
public:
    Result<void> Open(platform::FileSystem& files, StringView path) noexcept {
        if (auto opened = file_.Open(files, path.Data(), path.ByteCount(),
                                     platform::FileOpenMode::ReadExisting);
            !opened) {
            return Result<void>::Failure(FileErrorCode(opened));
        }

        if (auto sized = file_.Size(size_); !sized) {
            return Result<void>::Failure(FileErrorCode(sized));
        }

        return Result<void>::Success();
    }

    uint64 Size() const noexcept override {
        return size_;
    }

    Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept override {
        if (offset > size_ || out.Size() > size_ - offset) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        uint64 position = 0;
        if (auto moved =
                file_.Seek(static_cast<int64>(offset), platform::FileSeekOrigin::Begin, position);
            !moved) {
            return Result<void>::Failure(FileErrorCode(moved));
        }

        const platform::FileTransfer read = file_.ReadExact(out.Data(), out.Size());
        return read.status ? Result<void>::Success()
                           : Result<void>::Failure(FileErrorCode(read.status));
    }

    void Close() noexcept {
        (void)file_.Close();
    }

private:
    platform::File file_;
    uint64 size_ = 0;
};

// 항목 하나를 연 결과다.
enum class EntryState : uint8 {
    Missing,
    Ready,
    Unsupported,
    Corrupt,
    Failed
};

// path의 항목을 열어 머리·Record·표를 검증한다. Failed면 error에 원인(IO·할당 실패)이다.
EntryState OpenEntry(Allocator& allocator, platform::FileSystem& files, StringView path,
                     FileReader& reader, Result<NuggetFileView>& view, ErrorCode& error) noexcept {
    error = ErrorCode::None;
    if (auto opened = reader.Open(files, path); !opened) {
        if (opened.Error() == ErrorCode::NotFound) {
            return EntryState::Missing;
        }

        error = opened.Error();
        return EntryState::Failed;
    }

    view = OpenNuggetFile(allocator, reader);
    if (view) {
        return EntryState::Ready;
    }

    error = view.Error();
    switch (view.Error()) {
    case ErrorCode::InvalidArgument:
        return EntryState::Corrupt;
    case ErrorCode::Unsupported:
        return EntryState::Unsupported;
    default:
        return EntryState::Failed;
    }
}

bool IsLowerHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool IsHexName(StringView name, usize bytes) noexcept {
    if (name.ByteCount() != bytes) {
        return false;
    }

    for (usize index = 0; index < bytes; ++index) {
        if (!IsLowerHex(name.Data()[index])) {
            return false;
        }
    }

    return true;
}

// 값 행과 ValueId의 순서 비교다(이진 검색 — 행은 ValueId 오름차순).
struct ValueIdLess {
    bool operator()(const NuggetValueRow& row, uint32 id) const noexcept {
        return row.id < id;
    }

    bool operator()(uint32 id, const NuggetValueRow& row) const noexcept {
        return id < row.id;
    }
};

// 저장 값 하나(원본·저장 byte)를 정한다: 임계 이상이면 RCBK로 압축해 더 작을 때만 쓴다.
Result<void> ChooseStorage(Allocator& allocator, const NuggetPutValue& value, Array<uint8>& packed,
                           NuggetStoredValue& out) noexcept {
    out = NuggetStoredValue{};
    out.id = value.id;
    out.rawSize = value.bytes.Size();
    out.rawHash = Sha256::Hash(value.bytes);
    out.stored = value.bytes;
    if (value.bytes.Size() < NuggetStoreCompressBytes) {
        return Result<void>::Success();
    }

    auto compression = CompressionSettings::TryDefault(CompressionFormat::Zstd);
    auto settings = compression ? CompressedBlocksSettings::TryMake(compression.Value(),
                                                                    NuggetStoreBlockSizeLog2)
                                : Result<CompressedBlocksSettings>::Failure(compression.Error());
    if (!settings) {
        return Result<void>::Failure(settings.Error());
    }

    if (auto compressed = TryCompressBlocksAppend(settings.Value(), value.bytes, packed, allocator);
        !compressed) {
        return Result<void>::Failure(compressed.Error());
    }

    if (packed.Size() >= value.bytes.Size()) {
        packed.Clear();
        return Result<void>::Success();
    }

    auto reader = CompressedBlocksReader::TryOpen(packed.View(), allocator);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    out.encoding = NuggetEncoding::Rcbk;
    out.indexLength = reader.Value().IndexBytes();
    out.indexHash = Sha256::Hash(packed.View().Slice(0, reader.Value().IndexBytes()));
    out.stored = packed.View();
    return Result<void>::Success();
}

} // namespace

int64 NuggetSystemClock::NowTicks() noexcept {
    return DateTime::UtcNow().Ticks();
}

NuggetFileSystemBackend::NuggetFileSystemBackend(Allocator& allocator, platform::FileSystem& files,
                                                 StringView root, NuggetClock& clock) noexcept
    : allocator_(&allocator), files_(&files), root_(allocator), clock_(&clock) {
    (void)root_.TryAssign(root);
}

Result<void> NuggetFileSystemBackend::EntryPath(const Sha256Digest& key,
                                                String& out) const noexcept {
    out.Clear();
    char hex[Sha256Digest::HexBytes + 1];
    key.ToHex(hex);
    Result<void> step = out.TryAppend(root_.View());
    if (step) {
        step = out.TryAppend("/");
    }

    if (step) {
        step = out.TryAppend(StringView(hex, DirectoryHexBytes));
    }

    if (step) {
        step = out.TryAppend("/");
    }

    if (step) {
        step = out.TryAppend(StringView(hex, KeyHexBytes));
    }

    if (step) {
        step = out.TryAppend(EntryExtension);
    }

    return step;
}

Result<void> NuggetFileSystemBackend::Open() noexcept {
    if (root_.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 루트는 있어야 한다(이미 있는 디렉터리는 성공 — 읽기 전용 루트의 판정은 아래 tmp 만들기다).
    if (auto made =
            platform::MakeDirectoryTree(*files_, root_.View().Data(), root_.View().ByteCount());
        !made) {
        return Result<void>::Failure(FileErrorCode(made));
    }

    String temporary(*allocator_);
    Result<void> step = temporary.TryAppend(root_.View());
    if (step) {
        step = temporary.TryAppend("/");
    }

    if (step) {
        step = temporary.TryAppend(TemporaryDirectory);
    }

    if (!step) {
        return step;
    }

    if (auto made = platform::MakeDirectoryTree(*files_, temporary.View().Data(),
                                                temporary.View().ByteCount());
        !made) {
        readOnly_ = true;
    }

    // shard 잠금 파일은 물리 파일 시스템에 둔다(FileLock).
    if (!readOnly_ && files_ != &platform::PhysicalFileSystem()) {
        (void)platform::MakeDirectoryTree(platform::PhysicalFileSystem(), temporary.View().Data(),
                                          temporary.View().ByteCount());
    }

    opened_ = true;
    if (!readOnly_) {
        usize removed = 0;
        if (auto cleaned = CleanTemporaries(removed);
            !cleaned && cleaned.Error() == ErrorCode::OutOfMemory) {
            return cleaned;
        }
    }

    return Result<void>::Success();
}

namespace {

// 디렉터리 항목 이름 모음이다.
struct NameList {
    explicit NameList(Allocator& allocator) noexcept : names(allocator), infos(allocator) {
    }

    Array<String> names;
    Array<platform::FileInfo> infos;
    ErrorCode error = ErrorCode::None;
    Allocator* allocator = nullptr;
};

bool CollectName(const platform::DirectoryEntry& entry, void* context) noexcept {
    NameList& list = *static_cast<NameList*>(context);
    String name(*list.allocator);
    Result<void> step = name.TryAssign(StringView(entry.name, entry.nameBytes));
    if (step) {
        step = list.names.TryPushBack(Move(name));
    }

    if (step) {
        step = list.infos.TryPushBack(entry.info);
    }

    if (!step) {
        list.error = step.Error();
        return false;
    }

    return true;
}

Result<void> ListDirectory(Allocator& allocator, platform::FileSystem& files, StringView path,
                           NameList& out) noexcept {
    out.allocator = &allocator;
    if (auto listed = files.EnumerateDirectory(path.Data(), path.ByteCount(), &CollectName, &out);
        !listed) {
        return Result<void>::Failure(FileErrorCode(listed));
    }

    return out.error == ErrorCode::None ? Result<void>::Success()
                                        : Result<void>::Failure(out.error);
}

} // namespace

Result<void> NuggetFileSystemBackend::CleanTemporaries(usize& removed) noexcept {
    removed = 0;
    String directory(*allocator_);
    Result<void> step = directory.TryAppend(root_.View());
    if (step) {
        step = directory.TryAppend("/");
    }

    if (step) {
        step = directory.TryAppend(TemporaryDirectory);
    }

    if (!step) {
        return step;
    }

    NameList list(*allocator_);
    if (auto listed = ListDirectory(*allocator_, *files_, directory.View(), list); !listed) {
        return listed;
    }

    const int64 now = clock_->NowTicks();
    String path(*allocator_);
    for (usize index = 0; index < list.names.Size(); ++index) {
        const StringView name = list.names[index].View();
        const platform::FileInfo& info = list.infos[index];
        // 잠금 파일(`*.lock`)은 지우지 않는다 — FileLock 계약(9.1절 결정 7·8, shard라 쌓이지
        // 않는다).
        const bool temporary = name.EndsWith(TemporaryExtension, StringComparison::CaseSensitive);
        if (info.type != platform::FileType::File || !temporary ||
            now - info.modifiedTicks < NuggetTemporaryLifetimeTicks) {
            continue;
        }

        path.Clear();
        step = path.TryAppend(directory.View());
        if (step) {
            step = path.TryAppend("/");
        }

        if (step) {
            step = path.TryAppend(name);
        }

        if (!step) {
            return step;
        }

        if (files_->Delete(path.View().Data(), path.View().ByteCount())) {
            ++removed;
        }
    }

    return Result<void>::Success();
}

NuggetGetResult NuggetFileSystemBackend::Get(const NuggetBackendGet& request) noexcept {
    NuggetGetResult result(*allocator_);
    if (!opened_) {
        result.error = ErrorCode::InvalidArgument;
        return result;
    }

    String path(*allocator_);
    if (auto built = EntryPath(request.key, path); !built) {
        result.error = built.Error();
        return result;
    }

    FileReader reader;
    Result<NuggetFileView> view = Result<NuggetFileView>::Failure(ErrorCode::NotFound);
    ErrorCode error = ErrorCode::None;
    const EntryState state = OpenEntry(*allocator_, *files_, path.View(), reader, view, error);
    auto corrupt = [&](ErrorCode cause) noexcept {
        reader.Close();
        // 손상 항목은 지운다(경합으로 정상 항목을 지워도 다시 만들어진다 — 무해).
        (void)files_->Delete(path.View().Data(), path.View().ByteCount());
        result.status = NuggetStatus::Corrupt;
        result.error = cause;
        result.values.Clear();
        return Move(result);
    };
    switch (state) {
    case EntryState::Missing:
        return result;
    case EntryState::Unsupported:
    case EntryState::Failed:
        result.error = error;
        return result;
    case EntryState::Corrupt:
        return corrupt(error);
    case EntryState::Ready:
        break;
    }

    NuggetFileView& file = view.Value();
    if (!(file.record.key == request.key)) {
        return corrupt(ErrorCode::InvalidArgument);
    }

    if (file.record.bucket.View() != request.bucket) {
        result.error = ErrorCode::InvalidArgument;
        return result;
    }

    // 고를 값: 비면 전체, 아니면 청한 ValueId(없는 것은 partial이면 건너뛴다).
    Array<usize> chosen(*allocator_);
    bool missing = false;
    if (request.values.IsEmpty()) {
        if (auto reserved = chosen.TryReserve(file.record.values.Size()); !reserved) {
            result.error = reserved.Error();
            return result;
        }

        for (usize index = 0; index < file.record.values.Size(); ++index) {
            (void)chosen.TryPushBack(index);
        }
    } else {
        if (auto reserved = chosen.TryReserve(request.values.Size()); !reserved) {
            result.error = reserved.Error();
            return result;
        }

        for (usize at = 0; at < request.values.Size(); ++at) {
            const uint32 id = request.values[at];
            // 같은 ValueId를 두 번 청하면 잘못된 요청이다(결과 값이 겹친다).
            for (usize earlier = 0; earlier < at; ++earlier) {
                if (request.values[earlier] == id) {
                    result.error = ErrorCode::InvalidArgument;
                    return result;
                }
            }

            // 행은 ValueId 오름차순이다(정규 형식 — ValidateNuggetRecord).
            const usize found = algo::BinarySearch(file.record.values.View(), id, ValueIdLess{});
            if (found == algo::NotFound) {
                missing = true;
            } else {
                (void)chosen.TryPushBack(found);
            }
        }
    }

    if (missing && !request.partial) {
        return result;
    }

    if (!request.skipData) {
        if (auto reserved = result.values.TryReserve(chosen.Size()); !reserved) {
            result.error = reserved.Error();
            return result;
        }

        Array<uint8> stored(*allocator_);
        for (usize index : chosen) {
            stored.Clear();
            NuggetValue value(*allocator_);
            value.id = file.record.values[index].id;
            if (auto read = ReadNuggetValue(file, reader, index, stored); !read) {
                if (read.Error() == ErrorCode::InvalidArgument) {
                    return corrupt(read.Error());
                }

                result.error = read.Error();
                result.values.Clear();
                return result;
            }

            if (auto decoded = DecodeNuggetValue(*allocator_, file.record.values[index],
                                                 stored.View(), value.bytes);
                !decoded) {
                if (decoded.Error() == ErrorCode::InvalidArgument) {
                    return corrupt(decoded.Error());
                }

                result.error = decoded.Error();
                result.values.Clear();
                return result;
            }

            (void)result.values.TryPushBack(Move(value));
        }
    }

    reader.Close();
    result.digest = file.digest;
    result.record = Move(file.record);
    if (request.skipMeta) {
        result.record.meta.Clear();
    }

    result.status = missing ? NuggetStatus::Partial : NuggetStatus::Hit;
    if (request.touch) {
        (void)files_->SetModifiedTime(path.View().Data(), path.View().ByteCount(),
                                      clock_->NowTicks());
    }

    return result;
}

NuggetPutResult NuggetFileSystemBackend::Put(const NuggetBackendPut& request) noexcept {
    NuggetPutResult result(*allocator_);
    if (!opened_ || readOnly_) {
        // 읽기 전용 루트(공유 경로 등)는 쓰지 않는다.
        result.error = opened_ ? ErrorCode::Unsupported : ErrorCode::InvalidArgument;
        return result;
    }

    // 저장 표현(압축 판단)과 계획.
    Array<Array<uint8>> packed(*allocator_);
    Array<NuggetStoredValue> stored(*allocator_);
    Result<void> step = packed.TryReserve(request.values.Size());
    if (step) {
        step = stored.TryReserve(request.values.Size());
    }

    for (usize index = 0; step && index < request.values.Size(); ++index) {
        (void)packed.TryPushBack(Array<uint8>(*allocator_));
        NuggetStoredValue value;
        step = ChooseStorage(*allocator_, request.values[index], packed[index], value);
        if (step) {
            (void)stored.TryPushBack(value);
        }
    }

    if (!step) {
        result.error = step.Error();
        return result;
    }

    auto plan = PlanNuggetFile(*allocator_, request.bucket, request.key, stored.View(),
                               request.inputs, request.meta);
    if (!plan) {
        result.error = plan.Error();
        return result;
    }

    // 게시 전 왕복 해제로 RawHash를 확인한다(압축기 결함이 틀린 byte를 게시하지 않게 —
    // Rockpile 5.4절).
    for (usize index = 0; index < stored.Size(); ++index) {
        if (stored[index].encoding != NuggetEncoding::Rcbk) {
            continue;
        }

        Array<uint8> check(*allocator_);
        if (auto decoded = DecodeNuggetValue(*allocator_, plan.Value().record.values[index],
                                             stored[index].stored, check);
            !decoded) {
            result.error = decoded.Error();
            return result;
        }
    }

    result.digest = NuggetRecordDigest(plan.Value().record);
    // 계획한 Record를 결과에 둔다(NuggetCache의 Memory 항목 — 아래 쓰기는 머리·표 byte만 쓴다).
    result.record = Move(plan.Value().record);
    String entry(*allocator_);
    String temporary(*allocator_);
    String directory(*allocator_);
    auto guid = TryNewGuid();
    step = guid ? EntryPath(request.key, entry) : Result<void>::Failure(guid.Error());
    if (step) {
        step = temporary.TryAppend(root_.View());
    }

    if (step) {
        step = temporary.TryAppend("/");
    }

    if (step) {
        step = temporary.TryAppend(TemporaryDirectory);
    }

    if (step) {
        step = temporary.TryAppend("/");
    }

    char guidText[GuidTextBytes + 1] = {};
    FixedStringBuilder guidName{Span<char>(guidText)};
    if (step && guid) {
        step = guidName.TryAppendFormat("{}", guid.Value());
    }

    if (step) {
        step = temporary.TryAppend(guidName.View());
    }

    if (step) {
        step = temporary.TryAppend(TemporaryExtension);
    }

    if (step) {
        step = directory.TryAssign(
            StringView(entry.View().Data(), root_.View().ByteCount() + 1 + DirectoryHexBytes));
    }

    if (!step) {
        result.error = step.Error();
        return result;
    }

    // 임시 파일에 머리·값·표를 흘려 쓴다(값 byte를 한 버퍼에 모으지 않는다).
    {
        platform::File file;
        platform::FileStatus status =
            file.Open(*files_, temporary.View().Data(), temporary.View().ByteCount(),
                      platform::FileOpenMode::CreateNew);
        if (status) {
            status = file.WriteAll(plan.Value().head.Data(), plan.Value().head.Size()).status;
        }

        for (usize index = 0; status && index < stored.Size(); ++index) {
            status = file.WriteAll(stored[index].stored.Data(), stored[index].stored.Size()).status;
        }

        for (usize index = 0; status && index < plan.Value().tables.Size(); ++index) {
            status =
                file.WriteAll(plan.Value().tables[index].Data(), plan.Value().tables[index].Size())
                    .status;
        }

        if (status) {
            status = file.Flush();
        }

        const platform::FileStatus closed = file.Close();
        if (status) {
            status = closed;
        }

        if (!status) {
            (void)files_->Delete(temporary.View().Data(), temporary.View().ByteCount());
            result.error = FileErrorCode(status);
            return result;
        }
    }

    Notify(NuggetBackendStep::BeforePublish, request.key);

    if (auto made = platform::MakeDirectoryTree(*files_, directory.View().Data(),
                                                directory.View().ByteCount());
        !made) {
        (void)files_->Delete(temporary.View().Data(), temporary.View().ByteCount());
        result.error = FileErrorCode(made);
        return result;
    }

    const platform::FileStatus moved =
        files_->Move(temporary.View().Data(), temporary.View().ByteCount(), entry.View().Data(),
                     entry.View().ByteCount(), false);
    if (moved) {
        result.status = NuggetStatus::Stored;
        return result;
    }

    if (moved.code != platform::FileCode::AlreadyExists) {
        // delete-pending 이름(AccessDenied) 등 — 일시적이라 버린다.
        (void)files_->Delete(temporary.View().Data(), temporary.View().ByteCount());
        result.error = FileErrorCode(moved);
        return result;
    }

    return Settle(request.key, temporary.View(), entry.View(), Move(result));
}

// 대상이 이미 있을 때의 판정이다(9절 쓰기 — Rockpile 6.9절): 같은 digest Exists, 다른 digest
// Conflict, 손상이면 shard 잠금 안에서 다시 보고 여전히 손상일 때만 교체, 다른 버전은 손대지
// 않는다.
NuggetPutResult NuggetFileSystemBackend::Settle(const Sha256Digest& key, StringView temporary,
                                                StringView entry, NuggetPutResult result) noexcept {
    // 결과를 정하고 임시 파일을 지운다(결과는 result에 남는다 — Record를 옮기지 않는다).
    auto finish = [&](NuggetStatus status, ErrorCode error) noexcept {
        (void)files_->Delete(temporary.Data(), temporary.ByteCount());
        result.status = status;
        result.error = error;
        return true;
    };
    // 기존 항목을 판정한다. 손상이 아니면 결과를 정하고 true다(손상이면 false — 교체로 간다).
    auto judge = [&]() noexcept {
        FileReader reader;
        Result<NuggetFileView> view = Result<NuggetFileView>::Failure(ErrorCode::NotFound);
        ErrorCode error = ErrorCode::None;
        EntryState state = OpenEntry(*allocator_, *files_, entry, reader, view, error);
        // 같은 digest면 Exists를 약속하기 전에 값 byte까지 본다(ContentHash — 손상 항목을 Exists로
        // 남기지 않는다). 값은 고정 버퍼로 흘려 읽는다(값 하나가 수 GiB일 수 있다).
        if (state == EntryState::Ready && view.Value().digest == result.digest) {
            Array<uint8> buffer(*allocator_);
            if (auto sized = buffer.TryResize(VerifyBufferBytes); !sized) {
                error = sized.Error();
                state = EntryState::Failed;
            }

            for (usize index = 0;
                 state == EntryState::Ready && index < view.Value().record.values.Size(); ++index) {
                if (auto verified = VerifyNuggetValue(view.Value(), reader, index, buffer.View());
                    !verified) {
                    error = verified.Error();
                    state = verified.Error() == ErrorCode::InvalidArgument ? EntryState::Corrupt
                                                                           : EntryState::Failed;
                }
            }
        }

        reader.Close();
        switch (state) {
        case EntryState::Ready:
            result.existing = view.Value().digest;
            if (view.Value().digest == result.digest) {
                (void)files_->SetModifiedTime(entry.Data(), entry.ByteCount(), clock_->NowTicks());
                return finish(NuggetStatus::Exists, ErrorCode::None);
            }

            return finish(NuggetStatus::Conflict, ErrorCode::None);
        case EntryState::Missing:
            // 판정 사이에 지워졌다(GC·손상 삭제) — 한 번 더 게시한다.
            if (files_->Move(temporary.Data(), temporary.ByteCount(), entry.Data(),
                             entry.ByteCount(), false)) {
                return finish(NuggetStatus::Stored, ErrorCode::None);
            }

            return finish(NuggetStatus::Dropped, ErrorCode::Busy);
        case EntryState::Unsupported:
        case EntryState::Failed:
            return finish(NuggetStatus::Dropped, error);
        case EntryState::Corrupt:
            break;
        }

        return false;
    };

    Notify(NuggetBackendStep::BeforeJudge, key);
    if (judge()) {
        return Move(result);
    }

    // 손상 항목 교체: shard 잠금(키 첫 byte — 256개, 지우지 않는다) 안에서 다시 보고 여전히 손상일
    // 때만 바꾼다(조건부 갱신).
    char hex[Sha256Digest::HexBytes + 1];
    key.ToHex(hex);
    String lockPath(*allocator_);
    Result<void> step = lockPath.TryAppend(root_.View());
    if (step) {
        step = lockPath.TryAppend("/");
    }

    if (step) {
        step = lockPath.TryAppend(TemporaryDirectory);
    }

    if (step) {
        step = lockPath.TryAppend("/");
    }

    if (step) {
        step = lockPath.TryAppend(StringView(hex, DirectoryHexBytes));
    }

    if (step) {
        step = lockPath.TryAppend(LockExtension);
    }

    if (!step) {
        finish(NuggetStatus::Dropped, step.Error());
        return Move(result);
    }

    platform::FileLock lock;
    if (auto held =
            lock.Acquire(lockPath.View().Data(), lockPath.View().ByteCount(), replaceLockMs_);
        !held) {
        finish(NuggetStatus::Dropped, FileErrorCode(held));
        return Move(result);
    }

    Notify(NuggetBackendStep::BeforeJudgeLocked, key);
    if (judge()) {
        return Move(result);
    }

    if (auto replaced = files_->Replace(temporary.Data(), temporary.ByteCount(), entry.Data(),
                                        entry.ByteCount());
        !replaced && replaced.code != platform::FileCode::NotDurable) {
        finish(NuggetStatus::Dropped, FileErrorCode(replaced));
        return Move(result);
    }

    result.status = NuggetStatus::Stored;
    result.error = ErrorCode::None;
    return Move(result);
}

Result<void> NuggetFileSystemBackend::Touch(const Sha256Digest& key) noexcept {
    String path(*allocator_);
    if (auto built = EntryPath(key, path); !built) {
        return built;
    }

    if (auto touched = files_->SetModifiedTime(path.View().Data(), path.View().ByteCount(),
                                               clock_->NowTicks());
        !touched) {
        return Result<void>::Failure(FileErrorCode(touched));
    }

    return Result<void>::Success();
}

Result<bool> NuggetFileSystemBackend::Remove(const Sha256Digest& key) noexcept {
    String path(*allocator_);
    if (auto built = EntryPath(key, path); !built) {
        return Result<bool>::Failure(built.Error());
    }

    const platform::FileStatus deleted =
        files_->Delete(path.View().Data(), path.View().ByteCount());
    if (deleted) {
        return Result<bool>::Success(true);
    }

    if (deleted.code == platform::FileCode::NotFound) {
        return Result<bool>::Success(false);
    }

    return Result<bool>::Failure(FileErrorCode(deleted));
}

namespace {

// GC 후보 항목 하나다.
struct CollectEntry {
    explicit CollectEntry(Allocator& allocator) noexcept : path(allocator) {
    }

    String path;
    uint64 bytes = 0;
    int64 modified = 0;
};

} // namespace

Result<NuggetCollectResult>
NuggetFileSystemBackend::Collect(const NuggetCollectOptions& options) noexcept {
    using Out = NuggetCollectResult;
    NuggetCollectResult result;
    if (!opened_) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    NameList top(*allocator_);
    if (auto listed = ListDirectory(*allocator_, *files_, root_.View(), top); !listed) {
        return Result<Out>::Failure(listed.Error());
    }

    // 항목 파일만 모은다(`<k0k1>/<key>.rnug` — 다른 이름은 건드리지 않는다).
    Array<CollectEntry> entries(*allocator_);
    String directory(*allocator_);
    for (usize at = 0; at < top.names.Size(); ++at) {
        const StringView name = top.names[at].View();
        if (top.infos[at].type != platform::FileType::Directory || top.infos[at].isLink ||
            !IsHexName(name, DirectoryHexBytes)) {
            continue;
        }

        directory.Clear();
        Result<void> step = directory.TryAppend(root_.View());
        if (step) {
            step = directory.TryAppend("/");
        }

        if (step) {
            step = directory.TryAppend(name);
        }

        if (!step) {
            return Result<Out>::Failure(step.Error());
        }

        NameList files(*allocator_);
        if (auto listed = ListDirectory(*allocator_, *files_, directory.View(), files); !listed) {
            if (listed.Error() == ErrorCode::OutOfMemory) {
                return Result<Out>::Failure(listed.Error());
            }

            ++result.skipped;
            continue;
        }

        for (usize index = 0; index < files.names.Size(); ++index) {
            const StringView file = files.names[index].View();
            const platform::FileInfo& info = files.infos[index];
            if (info.type != platform::FileType::File || info.isLink ||
                file.ByteCount() != KeyHexBytes + EntryExtension.ByteCount() ||
                !file.EndsWith(EntryExtension, StringComparison::CaseSensitive) ||
                !IsHexName(StringView(file.Data(), KeyHexBytes), KeyHexBytes) ||
                StringView(file.Data(), DirectoryHexBytes) != name) {
                continue;
            }

            CollectEntry entry(*allocator_);
            step = entry.path.TryAppend(directory.View());
            if (step) {
                step = entry.path.TryAppend("/");
            }

            if (step) {
                step = entry.path.TryAppend(file);
            }

            if (step) {
                entry.bytes = info.size;
                entry.modified = info.modifiedTicks;
                step = entries.TryPushBack(Move(entry));
            }

            if (!step) {
                return Result<Out>::Failure(step.Error());
            }
        }
    }

    // 오래된 것부터(같은 시각이면 경로 byte 순 — 결정적).
    algo::Sort(entries.View(), [](const CollectEntry& left, const CollectEntry& right) noexcept {
        if (left.modified != right.modified) {
            return left.modified < right.modified;
        }

        return StringView::CompareBytes(left.path.View(), right.path.View()) < 0;
    });

    uint64 total = 0;
    for (const CollectEntry& entry : entries) {
        total += entry.bytes;
    }

    const int64 now = clock_->NowTicks();
    const int64 unused = static_cast<int64>(options.unusedDays) * TicksPerDay;
    for (const CollectEntry& entry : entries) {
        const bool old = now - entry.modified > unused;
        const bool over = total > options.maxBytes;
        if (!old && !over) {
            ++result.kept;
            result.keptBytes += entry.bytes;
            continue;
        }

        if (files_->Delete(entry.path.View().Data(), entry.path.View().ByteCount())) {
            ++result.removed;
            result.removedBytes += entry.bytes;
            total -= entry.bytes;
        } else {
            // 열린 파일·권한 — 건너뛴다(남은 합에서 빼지 않는다).
            ++result.skipped;
            ++result.kept;
            result.keptBytes += entry.bytes;
        }
    }

    if (auto cleaned = CleanTemporaries(result.temporaries);
        !cleaned && cleaned.Error() == ErrorCode::OutOfMemory) {
        return Result<Out>::Failure(cleaned.Error());
    }

    return Result<Out>::Success(result);
}

} // namespace rocks
