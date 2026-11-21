#include "ExchangeInternal.h"

#include <Core/FileError.h>
#include <MeshSource/MeshSourceTypes.h>
#include <Object/NameField.h>
#include <Platform/Memory.h>

// blob 파일 검사와 읽기다(Docs/Design/UnrealExchange.ko.md 3.3절, 10.4절 12번). 크기를 먼저 보고,
// 읽는 모든 byte를 SHA-256에 넣으면서 가변 배치(MeshAnimationAssets 3.4.1절)를 함께 파싱한다.

namespace rocks {

namespace {

// 가변 배치의 고정 칸 크기다(MeshAnimationAssets 3.4.1절): 길이·번호·시작 위치 word, 영향 하나
// (uint16 본 번호 + uint16 가중치), Vec3 값(float32 셋).
constexpr uint64 BlobWordBytes = 4;
constexpr uint64 BlobInfluenceBytes = 4;
constexpr uint64 BlobVec3Bytes = 12;
constexpr usize BlobHexPrefixChars = 2;
constexpr StringView BlobDirectoryPrefix = "blobs/";
constexpr StringView BlobFileSuffix = ".bin";
// `blobs/<앞 2자>/<64자>.bin`의 byte 수다.
constexpr usize BlobRelativePathBytes = BlobDirectoryPrefix.ByteCount() + BlobHexPrefixChars + 1 +
                                        Sha256Digest::HexBytes + BlobFileSuffix.ByteCount();

// 검사할 byte를 차례로 내주는 원천이다. 내준 모든 byte는 SHA-256에 들어간다. 할당하지 않으므로
// 실패는 "끝·읽기 실패"(false) 하나다.
class BlobSource {
public:
    // little-endian uint32 하나를 읽는다. 끝이거나 읽지 못하면 false다.
    virtual bool ReadWord(uint32& value) noexcept = 0;

    // bytes를 건너뛴다(해시에는 넣는다). 끝을 넘거나 읽지 못하면 false다.
    virtual bool Skip(uint64 bytes) noexcept = 0;

    // 남은 byte 수다.
    virtual uint64 Remaining() const noexcept = 0;

    // 남은 byte를 모두 해시에 넣고 digest를 쓴다. 읽지 못하면 false이고 error에 파일 오류를 쓴다.
    virtual bool Finish(Sha256Digest& digest, ErrorCode& error) noexcept = 0;

protected:
    ~BlobSource() = default;
};

uint32 LoadWord(const uint8* bytes) noexcept {
    return static_cast<uint32>(bytes[0]) | (static_cast<uint32>(bytes[1]) << 8) |
           (static_cast<uint32>(bytes[2]) << 16) | (static_cast<uint32>(bytes[3]) << 24);
}

// 메모리에 읽은 blob 원천이다(LoadExchangeBlob).
class MemoryBlobSource final : public BlobSource {
public:
    explicit MemoryBlobSource(Span<const uint8> bytes) noexcept : bytes_(bytes) {
    }

    bool ReadWord(uint32& value) noexcept override {
        if (Remaining() < BlobWordBytes) {
            return false;
        }

        value = LoadWord(bytes_.Data() + at_);
        at_ += BlobWordBytes;
        return true;
    }

    bool Skip(uint64 bytes) noexcept override {
        if (Remaining() < bytes) {
            return false;
        }

        at_ += static_cast<usize>(bytes);
        return true;
    }

    uint64 Remaining() const noexcept override {
        return bytes_.Size() - at_;
    }

    bool Finish(Sha256Digest& digest, ErrorCode&) noexcept override {
        at_ = bytes_.Size();
        digest = Sha256::Hash(bytes_);
        return true;
    }

private:
    Span<const uint8> bytes_;
    usize at_ = 0;
};

// 파일을 조각으로 흘려 읽는 원천이다(읽기 중 검증). 읽은 조각은 바로 해시에 넣는다.
class FileBlobSource final : public BlobSource {
public:
    FileBlobSource(platform::File& file, uint64 total, Array<uint8>& chunk) noexcept
        : file_(file), chunk_(chunk), remaining_(total) {
    }

    bool ReadWord(uint32& value) noexcept override {
        uint8 bytes[BlobWordBytes];
        for (uint8& byte : bytes) {
            if (!Fill()) {
                return false;
            }

            byte = chunk_[at_++];
            --remaining_;
        }

        value = LoadWord(bytes);
        return true;
    }

    bool Skip(uint64 bytes) noexcept override {
        if (bytes > remaining_) {
            return false;
        }

        while (bytes != 0) {
            if (!Fill()) {
                return false;
            }

            const uint64 available = filled_ - at_;
            const usize step = static_cast<usize>(bytes < available ? bytes : available);
            at_ += step;
            remaining_ -= step;
            bytes -= step;
        }

        return true;
    }

    uint64 Remaining() const noexcept override {
        return remaining_;
    }

    bool Finish(Sha256Digest& digest, ErrorCode& error) noexcept override {
        if (!Skip(remaining_)) {
            error = FileErrorCode(status_);
            return false;
        }

        digest = hasher_.Finish();
        return true;
    }

private:
    // 조각이 비었으면 다음 조각을 읽는다. 남은 byte가 없거나 읽지 못하면 false다(status_에 이유).
    bool Fill() noexcept {
        if (at_ < filled_) {
            return true;
        }

        if (remaining_ == 0) {
            return false;
        }

        const usize want = static_cast<usize>(
            remaining_ < static_cast<uint64>(chunk_.Size()) ? remaining_ : chunk_.Size());
        const platform::FileTransfer read = file_.ReadExact(chunk_.Data(), want);
        if (!read.status) {
            status_ = read.status;
            return false;
        }

        hasher_.Update(Span<const uint8>(chunk_.Data(), want));
        at_ = 0;
        filled_ = want;
        return true;
    }

    platform::File& file_;
    Array<uint8>& chunk_;
    Sha256 hasher_;
    uint64 remaining_;
    usize at_ = 0;
    usize filled_ = 0;
    platform::FileStatus status_{};
};

// names: count × components개의 (uint32 길이 + byte)를 끝까지 정확히 소비한다.
bool ParseNamesLayout(BlobSource& source, const ExchangeBlobRef& blob) noexcept {
    const uint64 total = source.Remaining();
    // 항목마다 길이 word가 있으므로 항목 수는 byte 수 / 4를 넘을 수 없다(곱 넘침 방지).
    if (blob.count > total / BlobWordBytes / blob.components) {
        return false;
    }

    const uint64 entries = blob.count * blob.components;
    for (uint64 entry = 0; entry < entries; ++entry) {
        uint32 length = 0;
        if (!source.ReadWord(length) || length > MaxNameFieldBytes || !source.Skip(length)) {
            return false;
        }
    }

    return source.Remaining() == 0;
}

// boneInfluences: 시작 표(count + 1, 첫 값 0, 단조 증가, 차 ≤ MeshMaxSourceInfluences) + 영향.
bool ParseInfluencesLayout(BlobSource& source, const ExchangeBlobRef& blob) noexcept {
    const uint64 total = source.Remaining();
    uint32 previous = 0;
    if (blob.count >= total / BlobWordBytes || !source.ReadWord(previous) || previous != 0) {
        return false;
    }

    for (uint64 index = 0; index < blob.count; ++index) {
        uint32 next = 0;
        if (!source.ReadWord(next) || next < previous ||
            next - previous > MeshMaxSourceInfluences) {
            return false;
        }

        previous = next;
    }

    return source.Remaining() == previous * BlobInfluenceBytes;
}

// sparseVec3: 개수 K + 번호 K(엄격 증가, < count) + Vec3 K.
bool ParseSparseLayout(BlobSource& source, const ExchangeBlobRef& blob) noexcept {
    uint32 entries = 0;
    if (!source.ReadWord(entries) || entries > blob.count ||
        source.Remaining() != static_cast<uint64>(entries) * (BlobWordBytes + BlobVec3Bytes)) {
        return false;
    }

    uint64 next = 0;
    for (uint32 entry = 0; entry < entries; ++entry) {
        uint32 index = 0;
        if (!source.ReadWord(index) || index < next || index >= blob.count) {
            return false;
        }

        next = static_cast<uint64>(index) + 1;
    }

    return true;
}

// 배치를 파싱한다. 고정 크기 배치는 참조를 읽을 때 크기 곱을 확인했으므로 통과다.
bool ParseBlobLayout(BlobSource& source, const ExchangeBlobRef& blob) noexcept {
    switch (blob.layout) {
    case ExchangeBlobLayout::Names:
        return ParseNamesLayout(source, blob);
    case ExchangeBlobLayout::BoneInfluences:
        return ParseInfluencesLayout(source, blob);
    case ExchangeBlobLayout::SparseVec3:
        return ParseSparseLayout(source, blob);
    default:
        return true;
    }
}

bool FailBlobFile(exchange_detail::BlobCheckFailure& failure, ExchangeIssue issue,
                  ErrorCode error) noexcept {
    failure.issue = issue;
    failure.error = error;
    return false;
}

// 배치 파싱과 해시 비교를 한다. 읽기 실패가 먼저, 그다음 해시(변조), 마지막이 배치다.
bool FinishBlobCheck(BlobSource& source, const ExchangeBlobRef& blob,
                     exchange_detail::BlobCheckFailure& failure) noexcept {
    const bool parsed = ParseBlobLayout(source, blob);
    Sha256Digest digest;
    ErrorCode error = ErrorCode::None;
    if (!source.Finish(digest, error)) {
        return FailBlobFile(failure, ExchangeIssue::ReadFailed, error);
    }

    if (!(digest == blob.digest)) {
        return FailBlobFile(failure, ExchangeIssue::BlobHash, ErrorCode::InvalidArgument);
    }

    if (!parsed) {
        return FailBlobFile(failure, ExchangeIssue::BlobLayout, ErrorCode::InvalidArgument);
    }

    return true;
}

} // namespace

const char* ExchangeBlobLayoutName(ExchangeBlobLayout layout) noexcept {
    switch (layout) {
    case ExchangeBlobLayout::U8:
        return "u8";
    case ExchangeBlobLayout::U16:
        return "u16";
    case ExchangeBlobLayout::U32:
        return "u32";
    case ExchangeBlobLayout::I32:
        return "i32";
    case ExchangeBlobLayout::F16:
        return "f16";
    case ExchangeBlobLayout::F32:
        return "f32";
    case ExchangeBlobLayout::F64:
        return "f64";
    case ExchangeBlobLayout::Names:
        return "names";
    case ExchangeBlobLayout::BoneInfluences:
        return "boneInfluences";
    case ExchangeBlobLayout::SparseVec3:
        return "sparseVec3";
    }

    return "Unknown";
}

uint32 ExchangeBlobElementBytes(ExchangeBlobLayout layout) noexcept {
    switch (layout) {
    case ExchangeBlobLayout::U8:
        return 1;
    case ExchangeBlobLayout::U16:
    case ExchangeBlobLayout::F16:
        return 2;
    case ExchangeBlobLayout::U32:
    case ExchangeBlobLayout::I32:
    case ExchangeBlobLayout::F32:
        return 4;
    case ExchangeBlobLayout::F64:
        return 8;
    default:
        return 0;
    }
}

namespace exchange_detail {

Result<void> AppendBundlePath(StringView directory, StringView relative,
                              StringBuilderBase& out) noexcept {
    const usize size = directory.ByteCount();
    const bool separated =
        size == 0 || directory.Data()[size - 1] == '/' || directory.Data()[size - 1] == '\\';
    out.Append(directory).Append(separated ? StringView() : StringView("/")).Append(relative);
    return out.Status();
}

Result<bool> CheckBlobFile(platform::FileSystem& files, StringView directory,
                           const ExchangeBlobRef& blob, Allocator& allocator, Array<uint8>* keep,
                           BlobCheckFailure& failure) noexcept {
    // 묶음 안 상대 경로 `blobs/<앞 2자>/<64자>.bin`이다.
    char hex[Sha256Digest::HexBytes + 1];
    blob.digest.ToHex(hex);
    char relative[BlobRelativePathBytes];
    platform::MemoryCopy(relative, BlobDirectoryPrefix.Data(), BlobDirectoryPrefix.ByteCount());
    usize at = BlobDirectoryPrefix.ByteCount();
    platform::MemoryCopy(relative + at, hex, BlobHexPrefixChars);
    at += BlobHexPrefixChars;
    relative[at++] = '/';
    platform::MemoryCopy(relative + at, hex, Sha256Digest::HexBytes);
    at += Sha256Digest::HexBytes;
    platform::MemoryCopy(relative + at, BlobFileSuffix.Data(), BlobFileSuffix.ByteCount());
    StringBuilder<512> path(allocator);
    if (auto made = AppendBundlePath(directory, StringView(relative, BlobRelativePathBytes), path);
        !made) {
        return Result<bool>::Failure(made.Error());
    }

    platform::File file;
    if (const platform::FileStatus opened = file.Open(files, path.CString(), path.ByteCount(),
                                                      platform::FileOpenMode::ReadExisting);
        !opened) {
        return Result<bool>::Success(
            FailBlobFile(failure, ExchangeIssue::ReadFailed, FileErrorCode(opened)));
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = file.Size(size); !sized) {
        return Result<bool>::Success(
            FailBlobFile(failure, ExchangeIssue::ReadFailed, FileErrorCode(sized)));
    }

    // 크기를 먼저 본다(조작된 크기로 큰 할당을 만들지 못하게).
    if (size != blob.bytes) {
        return Result<bool>::Success(
            FailBlobFile(failure, ExchangeIssue::BlobSize, ErrorCode::InvalidArgument));
    }

    if (keep != nullptr) {
        keep->Clear();
        if (auto resized = keep->TryResize(static_cast<usize>(size)); !resized) {
            return Result<bool>::Failure(resized.Error());
        }

        const platform::FileTransfer read = file.ReadExact(keep->Data(), keep->Size());
        MemoryBlobSource source(keep->View());
        const bool checked = read.status ? FinishBlobCheck(source, blob, failure)
                                         : FailBlobFile(failure, ExchangeIssue::ReadFailed,
                                                        FileErrorCode(read.status));
        if (!checked) {
            keep->Clear();
        }

        return Result<bool>::Success(checked);
    }

    Array<uint8> chunk(allocator);
    const usize chunkBytes = static_cast<usize>(
        size < UnrealExchangeBlobChunkBytes ? size : UnrealExchangeBlobChunkBytes);
    if (auto resized = chunk.TryResize(chunkBytes); !resized) {
        return Result<bool>::Failure(resized.Error());
    }

    FileBlobSource source(file, size, chunk);
    return Result<bool>::Success(FinishBlobCheck(source, blob, failure));
}

} // namespace exchange_detail

Result<void> LoadExchangeBlob(const ExchangeBundle& bundle, uint32 asset,
                              const ExchangeBlobRef& blob, Array<uint8>& out,
                              ExchangeDiagnostics& diagnostics) noexcept {
    out.Clear();
    platform::FileSystem* files = ExchangeBundleAccess::Files(bundle);
    if (files == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    exchange_detail::BlobCheckFailure failure;
    auto checked =
        exchange_detail::CheckBlobFile(*files, bundle.Directory(), blob,
                                       ExchangeBundleAccess::GetAllocator(bundle), &out, failure);
    if (!checked) {
        out.Clear();
        return Result<void>::Failure(checked.Error());
    }

    if (checked.Value()) {
        return Result<void>::Success();
    }

    char hex[Sha256Digest::HexBytes + 1];
    blob.digest.ToHex(hex);
    ExchangeDiagnosticInput input;
    input.code = AssetDiagnosticCode::CorruptBundle;
    input.error = failure.error;
    input.issue = failure.issue;
    input.file = ExchangeFile::Blob;
    input.asset = asset;
    input.text = StringView(hex, Sha256Digest::HexBytes);
    if (auto added = diagnostics.TryAdd(input); !added) {
        return added;
    }

    return Result<void>::Failure(failure.error);
}

} // namespace rocks
