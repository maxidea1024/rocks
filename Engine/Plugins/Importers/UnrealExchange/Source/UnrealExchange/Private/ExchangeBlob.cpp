#include "ExchangeInternal.h"

#include <Core/FileError.h>
#include <MeshSource/MeshSourceTypes.h>
#include <Object/NameField.h>

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

// 검사할 byte를 차례로 내주는 원천이다. 내준 모든 byte는 SHA-256에 들어간다.
class BlobSource {
public:
    virtual ~BlobSource() = default;

    // little-endian uint32 하나를 읽는다. 끝이면 false다.
    virtual Result<bool> ReadWord(uint32& value) noexcept = 0;

    // bytes를 건너뛴다(해시에는 넣는다). 끝을 넘으면 false다.
    virtual Result<bool> Skip(uint64 bytes) noexcept = 0;

    // 남은 byte 수다.
    virtual uint64 Remaining() const noexcept = 0;

    // 남은 byte를 모두 해시에 넣고 digest를 돌려준다.
    virtual Result<Sha256Digest> Finish() noexcept = 0;
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

    Result<bool> ReadWord(uint32& value) noexcept override {
        if (Remaining() < BlobWordBytes) {
            return Result<bool>::Success(false);
        }

        value = LoadWord(bytes_.Data() + at_);
        at_ += BlobWordBytes;
        return Result<bool>::Success(true);
    }

    Result<bool> Skip(uint64 bytes) noexcept override {
        if (Remaining() < bytes) {
            return Result<bool>::Success(false);
        }

        at_ += static_cast<usize>(bytes);
        return Result<bool>::Success(true);
    }

    uint64 Remaining() const noexcept override {
        return bytes_.Size() - at_;
    }

    Result<Sha256Digest> Finish() noexcept override {
        at_ = bytes_.Size();
        return Result<Sha256Digest>::Success(Sha256::Hash(bytes_));
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

    // 마지막 읽기 실패다(성공이면 Success).
    platform::FileStatus LastStatus() const noexcept {
        return status_;
    }

    Result<bool> ReadWord(uint32& value) noexcept override {
        uint8 bytes[BlobWordBytes];
        for (uint8& byte : bytes) {
            auto filled = Fill();
            if (!filled || !filled.Value()) {
                return filled;
            }

            byte = chunk_[at_++];
            --remaining_;
        }

        value = LoadWord(bytes);
        return Result<bool>::Success(true);
    }

    Result<bool> Skip(uint64 bytes) noexcept override {
        if (bytes > remaining_) {
            return Result<bool>::Success(false);
        }

        while (bytes != 0) {
            auto filled = Fill();
            if (!filled || !filled.Value()) {
                return filled;
            }

            const usize step = static_cast<usize>(
                bytes < static_cast<uint64>(filled_ - at_) ? bytes : filled_ - at_);
            at_ += step;
            remaining_ -= step;
            bytes -= step;
        }

        return Result<bool>::Success(true);
    }

    uint64 Remaining() const noexcept override {
        return remaining_;
    }

    Result<Sha256Digest> Finish() noexcept override {
        auto skipped = Skip(remaining_);
        if (!skipped) {
            return Result<Sha256Digest>::Failure(skipped.Error());
        }

        if (!skipped.Value()) {
            return Result<Sha256Digest>::Failure(FileErrorCode(status_));
        }

        return Result<Sha256Digest>::Success(hasher_.Finish());
    }

private:
    // 조각이 비었으면 다음 조각을 읽는다. 남은 byte가 없거나 읽지 못하면 false다(status_에 이유).
    Result<bool> Fill() noexcept {
        if (at_ < filled_) {
            return Result<bool>::Success(true);
        }

        if (remaining_ == 0) {
            return Result<bool>::Success(false);
        }

        const usize want = static_cast<usize>(
            remaining_ < static_cast<uint64>(chunk_.Size()) ? remaining_ : chunk_.Size());
        const platform::FileTransfer read = file_.ReadExact(chunk_.Data(), want);
        if (!read.status) {
            status_ = read.status;
            return Result<bool>::Success(false);
        }

        hasher_.Update(Span<const uint8>(chunk_.Data(), want));
        at_ = 0;
        filled_ = want;
        return Result<bool>::Success(true);
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
Result<bool> ParseNamesLayout(BlobSource& source, const ExchangeBlobRef& blob) noexcept {
    const uint64 total = source.Remaining();
    // 항목마다 길이 word가 있으므로 항목 수는 byte 수 / 4를 넘을 수 없다(곱 넘침 방지).
    if (blob.count > total / BlobWordBytes / blob.components) {
        return Result<bool>::Success(false);
    }

    const uint64 entries = blob.count * blob.components;
    for (uint64 entry = 0; entry < entries; ++entry) {
        uint32 length = 0;
        auto read = source.ReadWord(length);
        if (!read || !read.Value()) {
            return read;
        }

        if (length > MaxNameFieldBytes) {
            return Result<bool>::Success(false);
        }

        auto skipped = source.Skip(length);
        if (!skipped || !skipped.Value()) {
            return skipped;
        }
    }

    return Result<bool>::Success(source.Remaining() == 0);
}

// boneInfluences: 시작 표(count + 1, 첫 값 0, 단조 증가, 차 ≤ MeshMaxSourceInfluences) + 영향.
Result<bool> ParseInfluencesLayout(BlobSource& source, const ExchangeBlobRef& blob) noexcept {
    const uint64 total = source.Remaining();
    if (blob.count >= total / BlobWordBytes) {
        return Result<bool>::Success(false);
    }

    uint32 previous = 0;
    auto read = source.ReadWord(previous);
    if (!read) {
        return read;
    }

    if (previous != 0) {
        return Result<bool>::Success(false);
    }

    for (uint64 index = 0; index < blob.count; ++index) {
        uint32 next = 0;
        read = source.ReadWord(next);
        if (!read || !read.Value()) {
            return read;
        }

        if (next < previous || next - previous > MeshMaxSourceInfluences) {
            return Result<bool>::Success(false);
        }

        previous = next;
    }

    return Result<bool>::Success(source.Remaining() == previous * BlobInfluenceBytes);
}

// sparseVec3: 개수 K + 번호 K(엄격 증가, < count) + Vec3 K.
Result<bool> ParseSparseLayout(BlobSource& source, const ExchangeBlobRef& blob) noexcept {
    uint32 entries = 0;
    auto read = source.ReadWord(entries);
    if (!read || !read.Value()) {
        return read;
    }

    if (entries > blob.count ||
        source.Remaining() != static_cast<uint64>(entries) * (BlobWordBytes + BlobVec3Bytes)) {
        return Result<bool>::Success(false);
    }

    uint64 next = 0;
    for (uint32 entry = 0; entry < entries; ++entry) {
        uint32 index = 0;
        read = source.ReadWord(index);
        if (!read || !read.Value()) {
            return read;
        }

        if (index < next || index >= blob.count) {
            return Result<bool>::Success(false);
        }

        next = static_cast<uint64>(index) + 1;
    }

    return Result<bool>::Success(true);
}

// 배치를 파싱한다. 고정 크기 배치는 참조를 읽을 때 크기 곱을 확인했으므로 통과다.
Result<bool> ParseBlobLayout(BlobSource& source, const ExchangeBlobRef& blob) noexcept {
    switch (blob.layout) {
    case ExchangeBlobLayout::Names:
        return ParseNamesLayout(source, blob);
    case ExchangeBlobLayout::BoneInfluences:
        return ParseInfluencesLayout(source, blob);
    case ExchangeBlobLayout::SparseVec3:
        return ParseSparseLayout(source, blob);
    default:
        return Result<bool>::Success(true);
    }
}

// 배치 파싱과 해시 비교를 한다. 해시가 다르면 배치 결과와 무관하게 BlobHash다(변조가 먼저다).
Result<bool> FinishBlobCheck(BlobSource& source, const ExchangeBlobRef& blob,
                             exchange_detail::BlobCheckFailure& failure) noexcept {
    auto parsed = ParseBlobLayout(source, blob);
    if (!parsed) {
        return parsed;
    }

    auto digest = source.Finish();
    if (!digest) {
        failure.issue = ExchangeIssue::ReadFailed;
        failure.error = digest.Error();
        return Result<bool>::Success(false);
    }

    if (!(digest.Value() == blob.digest)) {
        failure.issue = ExchangeIssue::BlobHash;
        failure.error = ErrorCode::InvalidArgument;
        return Result<bool>::Success(false);
    }

    if (!parsed.Value()) {
        failure.issue = ExchangeIssue::BlobLayout;
        failure.error = ErrorCode::InvalidArgument;
        return Result<bool>::Success(false);
    }

    return Result<bool>::Success(true);
}

Result<bool> FailBlobFile(exchange_detail::BlobCheckFailure& failure, ExchangeIssue issue,
                          ErrorCode error) noexcept {
    failure.issue = issue;
    failure.error = error;
    return Result<bool>::Success(false);
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
    if (auto added = out.TryAppend(directory); !added) {
        return added;
    }

    const usize size = directory.ByteCount();
    const bool separated =
        size != 0 && (directory.Data()[size - 1] == '/' || directory.Data()[size - 1] == '\\');
    if (!separated && size != 0) {
        if (auto added = out.TryAppendAscii('/'); !added) {
            return added;
        }
    }

    return out.TryAppend(relative);
}

Result<void> AppendBlobRelativePath(const Sha256Digest& digest, StringBuilderBase& out) noexcept {
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    if (auto added = out.TryAppend("blobs/"); !added) {
        return added;
    }

    if (auto added = out.TryAppend(StringView(hex, BlobHexPrefixChars)); !added) {
        return added;
    }

    if (auto added = out.TryAppendAscii('/'); !added) {
        return added;
    }

    if (auto added = out.TryAppend(StringView(hex, Sha256Digest::HexBytes)); !added) {
        return added;
    }

    return out.TryAppend(".bin");
}

Result<bool> CheckBlobFile(platform::FileSystem& files, StringView directory,
                           const ExchangeBlobRef& blob, Allocator& allocator, Array<uint8>* keep,
                           BlobCheckFailure& failure) noexcept {
    StringBuilder<256> relative(allocator);
    if (auto made = AppendBlobRelativePath(blob.digest, relative); !made) {
        return Result<bool>::Failure(made.Error());
    }

    StringBuilder<512> path(allocator);
    if (auto made = AppendBundlePath(directory, relative.View(), path); !made) {
        return Result<bool>::Failure(made.Error());
    }

    platform::File file;
    if (const platform::FileStatus opened = file.Open(files, path.CString(), path.ByteCount(),
                                                      platform::FileOpenMode::ReadExisting);
        !opened) {
        return FailBlobFile(failure, ExchangeIssue::ReadFailed, FileErrorCode(opened));
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = file.Size(size); !sized) {
        return FailBlobFile(failure, ExchangeIssue::ReadFailed, FileErrorCode(sized));
    }

    // 크기를 먼저 본다(조작된 크기로 큰 할당을 만들지 못하게).
    if (size != blob.bytes) {
        return FailBlobFile(failure, ExchangeIssue::BlobSize, ErrorCode::InvalidArgument);
    }

    if (keep != nullptr) {
        keep->Clear();
        if (auto resized = keep->TryResize(static_cast<usize>(size)); !resized) {
            return Result<bool>::Failure(resized.Error());
        }

        const platform::FileTransfer read = file.ReadExact(keep->Data(), keep->Size());
        if (!read.status) {
            keep->Clear();
            return FailBlobFile(failure, ExchangeIssue::ReadFailed, FileErrorCode(read.status));
        }

        MemoryBlobSource source(keep->View());
        auto checked = FinishBlobCheck(source, blob, failure);
        if (!checked || !checked.Value()) {
            keep->Clear();
        }

        return checked;
    }

    Array<uint8> chunk(allocator);
    const usize chunkBytes = static_cast<usize>(
        size < UnrealExchangeBlobChunkBytes ? size : UnrealExchangeBlobChunkBytes);
    if (auto resized = chunk.TryResize(chunkBytes); !resized) {
        return Result<bool>::Failure(resized.Error());
    }

    FileBlobSource source(file, size, chunk);
    return FinishBlobCheck(source, blob, failure);
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
