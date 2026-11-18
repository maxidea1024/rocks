#include <Crypto/LogFileSealer.h>

#include <Core/Move.h>
#include <Crypto/Labels.h>
#include <Platform/Memory.h>

namespace rocks::crypto {

namespace {

// 머리 안 위치다.
constexpr usize VersionOffset = sizeof(LogFileMagic);
constexpr usize KemOffset = VersionOffset + 1;
constexpr usize CiphertextOffset = KemOffset + 1 + 2;
constexpr usize StreamHeaderOffset = CiphertextOffset + LogFileKemCiphertextBytes;

// 파일 키의 subkey 번호다(파일마다 공유 비밀이 다르므로 하나만 쓴다).
constexpr uint64 LogFileSubkeyId = 1;

void StoreLength(uint8* out, uint32 value) noexcept {
    out[0] = static_cast<uint8>(value);
    out[1] = static_cast<uint8>(value >> 8);
    out[2] = static_cast<uint8>(value >> 16);
    out[3] = static_cast<uint8>(value >> 24);
}

uint32 LoadLength(const uint8* in) noexcept {
    return static_cast<uint32>(in[0]) | (static_cast<uint32>(in[1]) << 8) |
           (static_cast<uint32>(in[2]) << 16) | (static_cast<uint32>(in[3]) << 24);
}

// 공유 비밀에서 이 파일의 스트림 키를 뽑는다.
Result<SecretStreamKey> DeriveFileKey(const KdfKey& shared) noexcept {
    return TryDeriveSecretStreamKey(shared, LogFileSubkeyId, labels::LogFile);
}

} // namespace

LogFileSealer::LogFileSealer(Span<const uint8> recipientPublicKey, uint8 flags) noexcept
    : flags_(flags) {
    auto key = KemPublicKey::TryFromBytes(KemAlgorithm::XWing, recipientPublicKey);
    if (key) {
        recipient_ = key.Value();
    } else {
        setupError_ = key.Error();
    }
}

Result<usize> LogFileSealer::TryWriteHeader(Span<uint8> out) noexcept {
    if (setupError_ != ErrorCode::None) {
        return Result<usize>::Failure(setupError_);
    }

    if (out.Size() < LogFileHeaderBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    writer_.Reset();
    auto shared =
        TryEncapsulate(recipient_, out.Slice(CiphertextOffset, LogFileKemCiphertextBytes));
    if (!shared) {
        return Result<usize>::Failure(shared.Error());
    }

    auto key = DeriveFileKey(shared.Value());
    if (!key) {
        return Result<usize>::Failure(key.Error());
    }

    auto writer = SecretStreamWriter::TryCreate(
        key.Value(), out.Slice(StreamHeaderOffset, SecretStreamWriter::HeaderBytes));
    if (!writer) {
        return Result<usize>::Failure(writer.Error());
    }

    writer_ = Move(writer.Value());
    platform::MemoryCopy(out.Data(), LogFileMagic, sizeof(LogFileMagic));
    out[VersionOffset] = LogFileFormatVersion;
    out[KemOffset] = static_cast<uint8>(KemAlgorithm::XWing);
    out[KemOffset + 1] = flags_;
    out[KemOffset + 2] = 0;
    return Result<usize>::Success(LogFileHeaderBytes);
}

Result<usize> LogFileSealer::TrySeal(Span<const uint8> batch, bool last, Span<uint8> out) noexcept {
    if (writer_.IsEmpty() || writer_.IsFinished() || out.Size() < LogFileRecordLengthBytes ||
        batch.Size() > NumericLimits<uint32>::Max() - SecretStreamWriter::OverheadBytes) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    auto sealed =
        writer_.TryPush(batch, {}, last ? SecretStreamTag::Final : SecretStreamTag::Message,
                        out.RightChop(LogFileRecordLengthBytes));
    if (!sealed) {
        return Result<usize>::Failure(sealed.Error());
    }

    StoreLength(out.Data(), static_cast<uint32>(sealed.Value()));
    return Result<usize>::Success(LogFileRecordLengthBytes + sealed.Value());
}

bool LogFileHasCompressedBatches(Span<const uint8> file) noexcept {
    return file.Size() >= LogFileHeaderBytes &&
           platform::MemoryEquals(file.Data(), LogFileMagic, sizeof(LogFileMagic)) &&
           (file[KemOffset + 1] & LogFileFlagCompressedBatches) != 0;
}

Result<LogFileDecodeSummary> TryDecodeLogFile(const KemKeyPair& recipient, Span<const uint8> file,
                                              Allocator& allocator, LogFileDecodeOutput output,
                                              void* context) noexcept {
    using Summary = Result<LogFileDecodeSummary>;
    if (file.Size() < LogFileHeaderBytes ||
        !platform::MemoryEquals(file.Data(), LogFileMagic, sizeof(LogFileMagic))) {
        return Summary::Failure(ErrorCode::InvalidArgument);
    }

    if (file[VersionOffset] != LogFileFormatVersion ||
        file[KemOffset] != static_cast<uint8>(KemAlgorithm::XWing) ||
        (file[KemOffset + 1] & ~LogFileFlagCompressedBatches) != 0 || file[KemOffset + 2] != 0 ||
        recipient.Algorithm() != KemAlgorithm::XWing) {
        return Summary::Failure(ErrorCode::Unsupported);
    }

    auto shared =
        TryDecapsulate(recipient, file.Slice(CiphertextOffset, LogFileKemCiphertextBytes));
    if (!shared) {
        return Summary::Failure(shared.Error());
    }

    auto key = DeriveFileKey(shared.Value());
    if (!key) {
        return Summary::Failure(key.Error());
    }

    auto reader = SecretStreamReader::TryCreate(
        key.Value(), file.Slice(StreamHeaderOffset, SecretStreamWriter::HeaderBytes));
    if (!reader) {
        return Summary::Failure(reader.Error());
    }

    LogFileDecodeSummary summary;
    summary.compressedBatches = (file[KemOffset + 1] & LogFileFlagCompressedBatches) != 0;
    uint8* plain = nullptr;
    usize plainCapacity = 0;
    Result<LogFileDecodeSummary> outcome = Summary::Success(summary);
    usize offset = LogFileHeaderBytes;
    while (offset < file.Size()) {
        if (summary.endMarker) {
            // 끝 표시 뒤의 byte는 붙인 것이다.
            outcome = Summary::Failure(ErrorCode::AuthenticationFailed);
            break;
        }

        if (file.Size() - offset < LogFileRecordLengthBytes) {
            summary.partialTail = true;
            break;
        }

        const usize length = LoadLength(file.Data() + offset);
        if (length < SecretStreamWriter::OverheadBytes) {
            outcome = Summary::Failure(ErrorCode::AuthenticationFailed);
            break;
        }

        if (file.Size() - offset - LogFileRecordLengthBytes < length) {
            // 쓰는 중에 끝난 기록이다(비정상 종료). 앞의 묶음은 이미 넘겼다.
            summary.partialTail = true;
            break;
        }

        const usize plainBytes = length - SecretStreamWriter::OverheadBytes;
        if (plainBytes > plainCapacity) {
            if (plain != nullptr) {
                (void)allocator.Deallocate(plain);
                plain = nullptr;
                plainCapacity = 0;
            }

            auto allocated = allocator.Allocate(plainBytes == 0 ? 1 : plainBytes, 16);
            if (!allocated) {
                outcome = Summary::Failure(allocated.Error());
                break;
            }

            plain = static_cast<uint8*>(allocated.Value());
            plainCapacity = plainBytes;
        }

        auto message = reader.Value().TryPull(file.Slice(offset + LogFileRecordLengthBytes, length),
                                              {}, Span<uint8>(plain, plainCapacity));
        if (!message) {
            outcome = Summary::Failure(message.Error());
            break;
        }

        offset += LogFileRecordLengthBytes + length;
        ++summary.batches;
        summary.plaintextBytes += message.Value().bytes;
        summary.endMarker = message.Value().tag == SecretStreamTag::Final;
        if (message.Value().bytes != 0) {
            auto delivered = output(context, Span<const uint8>(plain, message.Value().bytes));
            if (!delivered) {
                outcome = Summary::Failure(delivered.Error());
                break;
            }
        }
    }

    if (plain != nullptr) {
        (void)allocator.Deallocate(plain);
    }

    if (!outcome) {
        return outcome;
    }

    return Summary::Success(summary);
}

} // namespace rocks::crypto
