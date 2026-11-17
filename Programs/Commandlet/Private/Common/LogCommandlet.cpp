#include "LogCommandlet.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Crypto/Bytes.h>
#include <Crypto/LogFileSealer.h>
#include <Platform/File.h>
#include <Platform/Memory.h>

namespace rocks::commandlet_host {

namespace {

constexpr StringView Usage =
    "usage:\n"
    "  log keygen <out-public> <out-private>\n"
    "      Create a developer key pair for encrypted log files (X-Wing). The public key goes into\n"
    "      the game's log settings; the private key opens the logs. Never commit the private key\n"
    "      to the repository and never put it into cooked output. keygen refuses to overwrite.\n"
    "  log decode <file> --key <private key file> [--json]\n"
    "      Print the plaintext of an encrypted log file. Reports a missing end marker (abnormal\n"
    "      termination) after decoding every complete batch, and reports tampering as an error.\n"
    "      --json writes the status as one JSON object on stderr instead of text.\n"
    "  log seal <plain input> <out-file> --public <public key file>\n"
    "      Encrypt a plain file in the same format (diagnostics and tests).\n";

// 봉인 도구가 묶음 하나에 담는 평문 byte 수다.
constexpr usize SealBatchBytes = 4096;

Result<int32> Write(CommandletOutput& output, CommandletStream stream, StringView text,
                    int32 code) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(code) : Result<int32>::Failure(written.Error());
}

Result<int32> UsageError(CommandletOutput& output) noexcept {
    return Write(output, CommandletStream::Error, Usage, 2);
}

// 인수 view는 NUL 종료를 보장하지 않으므로 경로를 복사한다.
struct PathText {
    explicit PathText(StringView path) noexcept {
        const usize bytes =
            path.ByteCount() < sizeof(text) - 1 ? path.ByteCount() : sizeof(text) - 1;
        platform::MemoryCopy(text, path.Data(), bytes);
        text[bytes] = '\0';
    }

    char text[CommandletRegistry::MaxArgumentBytes + 1];
};

Result<int32> FileFailure(CommandletOutput& output, const char* action, StringView path,
                          platform::FileStatus status) noexcept {
    char storage[512];
    FixedStringBuilder text{Span<char>(storage)};
    (void)text.TryAppendFormat("log: cannot {} '{}' (file code {}, native error {})\n", action,
                               path, static_cast<int32>(status.code), status.nativeError);
    return Write(output, CommandletStream::Error, text.View(), 1);
}

// 파일 전체를 읽는다. 실패하면 오류를 출력하고 그 종료 코드를 failure에 둔다.
bool ReadWholeFile(CommandletOutput& output, StringView path, Array<uint8>& bytes,
                   Result<int32>& failure) noexcept {
    const PathText native(path);
    platform::File file;
    if (const platform::FileStatus opened =
            file.Open(native.text, platform::FileOpenMode::ReadExisting);
        !opened) {
        failure = FileFailure(output, "open", path, opened);
        return false;
    }

    uint64 size = 0;
    if (const platform::FileStatus sized = file.Size(size); !sized) {
        failure = FileFailure(output, "size", path, sized);
        return false;
    }

    if (!bytes.TryResize(static_cast<usize>(size))) {
        failure = Result<int32>::Failure(ErrorCode::OutOfMemory);
        return false;
    }

    if (const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
        !read.status) {
        failure = FileFailure(output, "read", path, read.status);
        return false;
    }

    return true;
}

// 새 파일에 쓴다(있으면 덮어쓰지 않는다 — 개인키를 실수로 바꾸지 않게).
Result<int32> WriteNewFile(CommandletOutput& output, StringView path,
                           Span<const uint8> bytes) noexcept {
    const PathText native(path);
    platform::File file;
    if (const platform::FileStatus opened =
            file.Open(native.text, platform::FileOpenMode::CreateNew);
        !opened) {
        return FileFailure(output, "create", path, opened);
    }

    if (const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
        !written.status) {
        return FileFailure(output, "write", path, written.status);
    }

    if (const platform::FileStatus closed = file.Close(); !closed) {
        return FileFailure(output, "close", path, closed);
    }

    return Result<int32>::Success(0);
}

Result<int32> KeyGen(Span<const StringView> args, CommandletOutput& output) noexcept {
    if (args.Size() != 2) {
        return UsageError(output);
    }

    auto pair = crypto::KemKeyPair::TryGenerate(crypto::KemAlgorithm::XWing);
    if (!pair) {
        return Result<int32>::Failure(pair.Error());
    }

    uint8 seed[crypto::LogFilePrivateKeyBytes];
    if (auto exported = crypto::hazmat::TryExportSeed(pair.Value(), Span<uint8>(seed)); !exported) {
        return Result<int32>::Failure(exported.Error());
    }

    // 개인키를 먼저 쓴다(공개키만 남고 개인키가 없는 상태를 피한다).
    auto secret = WriteNewFile(output, args[1], Span<const uint8>(seed));
    crypto::SecureZero(Span<uint8>(seed));
    if (!secret || secret.Value() != 0) {
        return secret;
    }

    auto published = WriteNewFile(output, args[0], pair.Value().PublicKey().View());
    if (!published || published.Value() != 0) {
        return published;
    }

    char storage[512];
    FixedStringBuilder text{Span<char>(storage)};
    (void)text.TryAppendFormat("log keygen: wrote public key '{}' and private key '{}'. Keep the "
                               "private key out of the repository and cooked output.\n",
                               args[0], args[1]);
    return Write(output, CommandletStream::Output, text.View(), 0);
}

struct DecodeContext {
    CommandletOutput* output = nullptr;
    uint64 batches = 0;
};

Result<void> PrintPlaintext(void* raw, Span<const uint8> plaintext) noexcept {
    auto& context = *static_cast<DecodeContext*>(raw);
    ++context.batches;
    return context.output->write(
        context.output->context, CommandletStream::Output,
        StringView(reinterpret_cast<const char*>(plaintext.Data()), plaintext.Size()));
}

Result<int32> Decode(Span<const StringView> args, CommandletOutput& output) noexcept {
    if (args.Size() != 3 && args.Size() != 4) {
        return UsageError(output);
    }

    if (args[1] != "--key" || (args.Size() == 4 && args[3] != "--json")) {
        return UsageError(output);
    }

    const bool json = args.Size() == 4;
    SystemAllocator allocator;
    Array<uint8> key(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!ReadWholeFile(output, args[2], key, failure)) {
        return failure;
    }

    if (key.Size() != crypto::LogFilePrivateKeyBytes) {
        crypto::SecureZero(key.View());
        return Write(output, CommandletStream::Error,
                     "log decode: the key file is not a log private key (expected 32 bytes)\n", 1);
    }

    auto pair = crypto::KemKeyPair::TryFromSeed(crypto::KemAlgorithm::XWing, key.View());
    crypto::SecureZero(key.View());
    if (!pair) {
        return Result<int32>::Failure(pair.Error());
    }

    Array<uint8> file(allocator);
    if (!ReadWholeFile(output, args[0], file, failure)) {
        return failure;
    }

    DecodeContext context{&output, 0};
    auto decoded =
        crypto::TryDecodeLogFile(pair.Value(), file.View(), allocator, &PrintPlaintext, &context);
    char storage[512];
    FixedStringBuilder text{Span<char>(storage)};
    int32 code = 0;
    if (!decoded) {
        code = 1;
        const ErrorCode error = decoded.Error();
        const char* reason =
            error == ErrorCode::AuthenticationFailed
                ? "authentication failed: the file was modified or the key does not match"
            : error == ErrorCode::InvalidArgument ? "not an encrypted Rocks log file"
            : error == ErrorCode::Unsupported     ? "unsupported log file version"
                                                  : ErrorCodeName(error);
        if (json) {
            (void)text.TryAppendFormat(
                "{{\"batches\": {}, \"endMarker\": false, \"error\": \"{}\"}}\n", context.batches,
                ErrorCodeName(error));
        } else {
            (void)text.TryAppendFormat("log decode: '{}': {} (after {} decoded batches)\n", args[0],
                                       reason, context.batches);
        }
    } else if (json) {
        (void)text.TryAppendFormat("{{\"batches\": {}, \"endMarker\": {}, "
                                   "\"partialTail\": {}, \"error\": null}}\n",
                                   decoded.Value().batches,
                                   decoded.Value().endMarker ? "true" : "false",
                                   decoded.Value().partialTail ? "true" : "false");
    } else if (!decoded.Value().endMarker) {
        (void)text.TryAppendFormat(
            "log decode: '{}': no end marker (abnormal termination); decoded {} batches up to the "
            "last complete batch{}\n",
            args[0], decoded.Value().batches,
            decoded.Value().partialTail ? ", ignored an incomplete trailing batch" : "");
    }

    if (text.ByteCount() == 0) {
        return Result<int32>::Success(0);
    }

    return Write(output, CommandletStream::Error, text.View(), code);
}

// 평문 파일을 같은 형식으로 봉인한다(진단·시험용).
Result<int32> Seal(Span<const StringView> args, CommandletOutput& output) noexcept {
    if (args.Size() != 4 || args[2] != "--public") {
        return UsageError(output);
    }

    SystemAllocator allocator;
    Array<uint8> recipient(allocator);
    Array<uint8> plain(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!ReadWholeFile(output, args[3], recipient, failure) ||
        !ReadWholeFile(output, args[0], plain, failure)) {
        return failure;
    }

    crypto::LogFileSealer sealer(recipient.View());
    Array<uint8> sealed(allocator);
    const usize batches = (plain.Size() + SealBatchBytes - 1) / SealBatchBytes;
    if (!sealed.TryResize(crypto::LogFileHeaderBytes +
                          (batches + 1) * (SealBatchBytes + sealer.SealOverhead()))) {
        return Result<int32>::Failure(ErrorCode::OutOfMemory);
    }

    auto header = sealer.TryWriteHeader(sealed.View());
    if (!header) {
        return Write(output, CommandletStream::Error,
                     "log seal: the public key file is not a log public key (expected 1216 "
                     "bytes)\n",
                     1);
    }

    usize used = header.Value();
    for (usize offset = 0; offset < plain.Size(); offset += SealBatchBytes) {
        const usize count =
            plain.Size() - offset < SealBatchBytes ? plain.Size() - offset : SealBatchBytes;
        auto record =
            sealer.TrySeal(plain.View().Slice(offset, count), false, sealed.View().RightChop(used));
        if (!record) {
            return Result<int32>::Failure(record.Error());
        }

        used += record.Value();
    }

    auto last = sealer.TrySeal({}, true, sealed.View().RightChop(used));
    if (!last) {
        return Result<int32>::Failure(last.Error());
    }

    used += last.Value();
    return WriteNewFile(output, args[1], sealed.View().Left(used));
}

} // namespace

Result<int32> RunLogCommand(void*, Span<const StringView> args, CommandletOutput& output) noexcept {
    if (args.Size() == 0) {
        return UsageError(output);
    }

    const Span<const StringView> rest = args.RightChop(1);
    if (args[0] == "keygen") {
        return KeyGen(rest, output);
    }

    if (args[0] == "decode") {
        return Decode(rest, output);
    }

    if (args[0] == "seal") {
        return Seal(rest, output);
    }

    if (args[0] == "--help" && args.Size() == 1) {
        return Write(output, CommandletStream::Output, Usage, 0);
    }

    return UsageError(output);
}

} // namespace rocks::commandlet_host
