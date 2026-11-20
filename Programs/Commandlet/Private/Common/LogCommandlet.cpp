#include "LogCommandlet.h"

#include "CommandletFiles.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Correlation.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Json.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Compression/Compression.h>
#include <Compression/CompressionStream.h>
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
    "  log seal <plain input> <out-file> --public <public key file> [--compress]\n"
    "      Encrypt a plain file in the same format (diagnostics and tests). --compress seals\n"
    "      zstd-compressed batches like log.file.compress.\n"
    "  log pack <plain input> <out-file>\n"
    "      Write a plain file as independent zstd frames per batch (*.log.zst, diagnostics and\n"
    "      tests).\n"
    "  log view <file|folder> [--key <private key file>] [--level <minimum>] [--category "
    "<prefix>]\n"
    "           [--event <id>] [--since <UTC>] [--until <UTC>] [--grep <text>]\n"
    "           [--correlation <session|task|asset|entity|request>=<id>]\n"
    "      Print matching lines of plain or encrypted log files. A folder is read in start-time\n"
    "      and part order (*.log, *.log.zst). JSON lines files filter on every field; text files "
    "with the\n"
    "      default layout filter on time, level and category, other text layouts only on --grep.\n";

// 봉인 도구가 묶음 하나에 담는 평문 byte 수다.
constexpr usize SealBatchBytes = 4096;

// 명령이 만드는 진단 한 줄의 버퍼 byte 수다.
constexpr usize MessageBytes = 512;

// base64 공개키 문자열(1216 byte → 1624자)과 NUL의 버퍼 byte 수다.
constexpr usize KeyTextBytes = 2048;

Result<int32> Write(CommandletOutput& output, CommandletStream stream, StringView text,
                    int32 code) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(code) : Result<int32>::Failure(written.Error());
}

Result<int32> UsageError(CommandletOutput& output) noexcept {
    return Write(output, CommandletStream::Error, Usage, 2);
}

// log 명령의 파일 도우미다(오류 문구 접두 "log" — CommandletFiles.h).
constexpr StringView LogCommandName = "log";

Result<int32> FileFailure(CommandletOutput& output, const char* action, StringView path,
                          platform::FileStatus status) noexcept {
    return ReportFileFailure(output, LogCommandName, action, path, status);
}

bool ReadWholeFile(CommandletOutput& output, StringView path, Array<uint8>& bytes,
                   Result<int32>& failure) noexcept {
    return ReadCommandFile(output, LogCommandName, path, bytes, failure);
}

Result<int32> WriteNewFile(CommandletOutput& output, StringView path,
                           Span<const uint8> bytes) noexcept {
    return WriteNewCommandFile(output, LogCommandName, path, bytes);
}

bool CreateNewFile(CommandletOutput& output, const PathText& path, StringView shown,
                   platform::File& file, Result<int32>& failure) noexcept {
    return CreateCommandFile(output, LogCommandName, path, shown, file, failure);
}

bool WriteAndClose(CommandletOutput& output, StringView shown, platform::File& file,
                   Span<const uint8> bytes, Result<int32>& failure) noexcept {
    return WriteCommandFileAndClose(output, LogCommandName, shown, file, bytes, failure);
}

Result<int32> KeyGen(Span<const StringView> args, CommandletOutput& output) noexcept {
    if (args.Size() != 2) {
        return UsageError(output);
    }

    auto pair = crypto::KemKeyPair::TryGenerate(crypto::KemAlgorithm::XWing);
    if (!pair) {
        return Result<int32>::Failure(pair.Error());
    }

    // 비밀 seed는 파일을 만들기 전에 꺼낸다(실패하면 아무 파일도 남기지 않는다).
    uint8 seed[crypto::LogFilePrivateKeyBytes];
    if (auto exported = crypto::hazmat::TryExportSeed(pair.Value(), Span<uint8>(seed)); !exported) {
        crypto::SecureZero(Span<uint8>(seed));
        return Result<int32>::Failure(exported.Error());
    }

    // 두 파일을 모두 새로 만든 뒤에 쓴다. 하나라도 이미 있으면 아무것도 쓰지 않는다(짝이 맞지 않는
    // 키 쌍을 남기지 않는다).
    const PathText publicPath(args[0]);
    const PathText privatePath(args[1]);
    Result<int32> failure = Result<int32>::Success(1);
    platform::File publicFile;
    if (!CreateNewFile(output, publicPath, args[0], publicFile, failure)) {
        crypto::SecureZero(Span<uint8>(seed));
        return failure;
    }

    platform::File privateFile;
    if (!CreateNewFile(output, privatePath, args[1], privateFile, failure)) {
        crypto::SecureZero(Span<uint8>(seed));
        (void)publicFile.Close();
        (void)platform::PhysicalFileSystem().Delete(publicPath.text,
                                                    platform::CStringLength(publicPath.text));
        return failure;
    }

    const bool secretWritten =
        WriteAndClose(output, args[1], privateFile, Span<const uint8>(seed), failure);
    crypto::SecureZero(Span<uint8>(seed));
    const bool publicWritten =
        secretWritten &&
        WriteAndClose(output, args[0], publicFile, pair.Value().PublicKey().View(), failure);
    if (!publicWritten) {
        // 한쪽만 남기지 않는다. 만든 두 파일을 지운다.
        (void)publicFile.Close();
        (void)privateFile.Close();
        (void)platform::PhysicalFileSystem().Delete(privatePath.text,
                                                    platform::CStringLength(privatePath.text));
        (void)platform::PhysicalFileSystem().Delete(publicPath.text,
                                                    platform::CStringLength(publicPath.text));
        return failure;
    }

    // 설정 log.file.recipient_key에 넣을 base64 공개키도 함께 알린다(비밀이 아니다).
    char base64[KeyTextBytes];
    auto encoded = crypto::TryToBase64(pair.Value().PublicKey().View(),
                                       crypto::Base64Variant::Standard, Span<char>(base64));
    char storage[MessageBytes + KeyTextBytes];
    FixedStringBuilder text{Span<char>(storage)};
    (void)text.TryAppendFormat("log keygen: wrote public key '{}' and private key '{}'. Keep the "
                               "private key out of the repository and cooked output.\n"
                               "log.file.recipient_key = {}\n",
                               args[0], args[1],
                               encoded ? StringView(base64, encoded.Value()) : StringView());
    return Write(output, CommandletStream::Output, text.View(), 0);
}

// 압축 로그 하나를 풀 때 받아들이는 최대 평문 byte 수다(악성 입력의 메모리 폭주 방지). 스트림으로
// 풀므로 인증되지 않은 frame 머리가 주장하는 크기를 미리 잡지 않는다.
constexpr usize MaxDecompressedLogBytes = usize{1} << 30;

// 스트림 해제에 한 번에 넘기는 압축 입력 byte 수다(실패해도 앞 조각의 출력은 남는다).
constexpr usize InflateChunkBytes = 64 * 1024;

// 이어 붙인 zstd frame을 스트림으로 풀어 out 뒤에 붙인다. 잘렸거나 손상된 frame에서 멈추고 그
// 전까지 푼 내용은 남긴다. 실패(잘림·손상·상한 초과)는 그 오류이고, 성공이면 끝까지 풀었다.
Result<void> InflateFrames(Span<const uint8> input, Allocator& allocator,
                           Array<uint8>& out) noexcept {
    DecompressStreamLimits limits;
    limits.maxOutputBytes = MaxDecompressedLogBytes;
    auto stream = DecompressStream::TryCreate(CompressionFormat::Zstd, limits, allocator);
    if (!stream) {
        return Result<void>::Failure(stream.Error());
    }

    for (usize offset = 0; offset < input.Size(); offset += InflateChunkBytes) {
        const usize count =
            input.Size() - offset < InflateChunkBytes ? input.Size() - offset : InflateChunkBytes;
        auto written = stream.Value().TryWriteAppend(input.Slice(offset, count), out);
        if (!written) {
            return Result<void>::Failure(written.Error());
        }

        if (out.Size() > MaxDecompressedLogBytes) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }
    }

    return stream.Value().TryFinish();
}

// 묶음 평문이 zstd frame(압축 뒤 봉인, 머리 LogFileFlagCompressedBatches)이면 풀어서 넘긴다.
struct DecodeContext {
    CommandletOutput* output = nullptr;
    uint64 batches = 0;
    Array<uint8>* inflated = nullptr;
    Allocator* allocator = nullptr;
};

Result<void> PrintPlaintext(void* raw, Span<const uint8> plaintext) noexcept {
    auto& context = *static_cast<DecodeContext*>(raw);
    ++context.batches;
    Span<const uint8> bytes = plaintext;
    if (context.inflated != nullptr && plaintext.Size() != 0) {
        context.inflated->Clear();
        if (auto inflated = InflateFrames(plaintext, *context.allocator, *context.inflated);
            !inflated) {
            return inflated;
        }

        bytes = context.inflated->View();
    }

    return context.output->write(
        context.output->context, CommandletStream::Output,
        StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()));
}

Result<int32> Decode(Span<const StringView> args, CommandletOutput& output,
                     Allocator& allocator) noexcept {
    if (args.Size() != 3 && args.Size() != 4) {
        return UsageError(output);
    }

    if (args[1] != "--key" || (args.Size() == 4 && args[3] != "--json")) {
        return UsageError(output);
    }

    const bool json = args.Size() == 4;
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

    Array<uint8> inflated(allocator);
    DecodeContext context{&output, 0,
                          crypto::LogFileHasCompressedBatches(file.View()) ? &inflated : nullptr,
                          &allocator};
    auto decoded =
        crypto::TryDecodeLogFile(pair.Value(), file.View(), allocator, &PrintPlaintext, &context);
    char storage[MessageBytes];
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
            (void)text.TryAppendFormat("{{\"batches\": {}, \"endMarker\": false, "
                                       "\"partialTail\": false, \"error\": \"{}\"}}\n",
                                       context.batches, ErrorCodeName(error));
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
Result<int32> Seal(Span<const StringView> args, CommandletOutput& output,
                   Allocator& allocator) noexcept {
    const bool compress = args.Size() == 5 && args[4] == "--compress";
    if ((args.Size() != 4 && !compress) || args[2] != "--public") {
        return UsageError(output);
    }

    Array<uint8> recipient(allocator);
    Array<uint8> plain(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!ReadWholeFile(output, args[3], recipient, failure) ||
        !ReadWholeFile(output, args[0], plain, failure)) {
        return failure;
    }

    crypto::LogFileSealer sealer(recipient.View(),
                                 compress ? crypto::LogFileFlagCompressedBatches : uint8{0});
    auto settings = CompressionSettings::TryDefault(CompressionFormat::Zstd);
    auto bound = TryGetCompressBound(CompressionFormat::Zstd, SealBatchBytes);
    if (!settings || !bound) {
        return Result<int32>::Failure(ErrorCode::DependencyFailed);
    }

    Array<uint8> sealed(allocator);
    Array<uint8> frame(allocator);
    const usize batches = (plain.Size() + SealBatchBytes - 1) / SealBatchBytes;
    if (!sealed.TryResize(crypto::LogFileHeaderBytes +
                          (batches + 1) * (bound.Value() + sealer.SealOverhead()))) {
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
        Span<const uint8> batch = plain.View().Slice(offset, count);
        if (compress) {
            frame.Clear();
            if (auto packed = TryCompressAppend(settings.Value(), batch, frame); !packed) {
                return Result<int32>::Failure(packed.Error());
            }

            batch = frame.View();
        }

        auto record = sealer.TrySeal(batch, false, sealed.View().RightChop(used));
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

// 평문 파일을 묶음(SealBatchBytes)마다 독립 zstd frame으로 압축한다(log.file.compress 파일과 같은
// 모양, 진단·시험용).
Result<int32> Pack(Span<const StringView> args, CommandletOutput& output,
                   Allocator& allocator) noexcept {
    if (args.Size() != 2) {
        return UsageError(output);
    }

    Array<uint8> plain(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!ReadWholeFile(output, args[0], plain, failure)) {
        return failure;
    }

    auto settings = CompressionSettings::TryDefault(CompressionFormat::Zstd);
    if (!settings) {
        return Result<int32>::Failure(settings.Error());
    }

    Array<uint8> packed(allocator);
    for (usize offset = 0; offset < plain.Size(); offset += SealBatchBytes) {
        const usize count =
            plain.Size() - offset < SealBatchBytes ? plain.Size() - offset : SealBatchBytes;
        if (auto frame =
                TryCompressAppend(settings.Value(), plain.View().Slice(offset, count), packed);
            !frame) {
            return Result<int32>::Failure(frame.Error());
        }
    }

    return WriteNewFile(output, args[1], packed.View());
}

// log view가 읽는 파일 확장자와 번호 해석의 진법이다.
constexpr StringView LogFileExtensionText = ".log";
constexpr uint64 DecimalBase = 10;
constexpr uint32 HexBase = 16;

// 압축 로그 파일(봉인하지 않음)의 확장자다.
constexpr StringView CompressedLogFileExtensionText = ".log.zst";

// 묶음 평문을 모은다. compressed면 묶음마다 zstd frame을 풀어 붙인다.
struct ViewPlaintext {
    Array<uint8>* text = nullptr;
    bool compressed = false;
    Allocator* allocator = nullptr;
};

Result<void> AppendPlaintext(void* raw, Span<const uint8> plaintext) noexcept {
    auto& context = *static_cast<ViewPlaintext*>(raw);
    if (!context.compressed || plaintext.Size() == 0) {
        return context.text->TryAppend(plaintext);
    }

    return InflateFrames(plaintext, *context.allocator, *context.text);
}

// 봉인한 로그 파일(magic "RKLOGENC")인지다.
bool IsSealedLogFile(Span<const uint8> file) noexcept {
    return file.Size() >= sizeof(crypto::LogFileMagic) &&
           platform::MemoryEquals(file.Data(), crypto::LogFileMagic, sizeof(crypto::LogFileMagic));
}

// 파일 하나를 읽지 못했을 때 경고를 남기고 다음 파일로 넘어간다.
Result<void> WarnFile(CommandletOutput& output, StringView path, StringView reason) noexcept {
    char storage[MessageBytes];
    FixedStringBuilder message{Span<char>(storage)};
    (void)message.TryAppendFormat("log view: '{}': {}\n", path, reason);
    return output.write(output.context, CommandletStream::Error, message.View());
}

// 로그 파일 확장자(".log" 또는 ".log.zst")다. 아니면 빈 view다.
StringView LogExtensionOf(StringView name) noexcept {
    if (name.EndsWith(CompressedLogFileExtensionText)) {
        return CompressedLogFileExtensionText;
    }

    return name.EndsWith(LogFileExtensionText) ? LogFileExtensionText : StringView();
}

// 레벨 이름(Trace…Fatal, 대소문자 무시)을 읽는다.
bool TryParseViewLevel(StringView text, LogLevel& level) noexcept {
    for (usize index = 0; index < LogLevelCount; ++index) {
        if (EqualsIgnoringAsciiCase(text, LogLevelName(static_cast<LogLevel>(index)))) {
            level = static_cast<LogLevel>(index);
            return true;
        }
    }

    return false;
}

// byte 사전순 비교다(음수·0·양수).
int CompareText(StringView left, StringView right) noexcept {
    const usize common =
        left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < common; ++index) {
        const auto a = static_cast<uint8>(left.Data()[index]);
        const auto b = static_cast<uint8>(right.Data()[index]);
        if (a != b) {
            return a < b ? -1 : 1;
        }
    }

    return left.ByteCount() == right.ByteCount() ? 0
                                                 : (left.ByteCount() < right.ByteCount() ? -1 : 1);
}

// ---- log view ----

// 한 줄에서 읽은 필드다. 읽지 못한 필드는 빈 값이고, 그 필드로 거르면 그 줄은 빠진다.
struct ViewFields {
    StringView time;
    StringView level;
    StringView category;
    StringView event;
    uint64 correlation[CorrelationFieldCount]{};
    bool hasCorrelation = false;
};

// log view의 거르기 조건이다.
struct ViewFilter {
    StringView level;
    StringView category;
    StringView event;
    StringView since;
    StringView until;
    StringView grep;
    usize correlationField = CorrelationFieldCount;
    uint64 correlationValue = 0;
};

// 기본 레이아웃 줄 "[시각][레벨][카테고리] 메시지"의 대괄호 칸 하나를 읽는다.
bool ReadBracket(StringView line, usize& cursor, StringView& value) noexcept {
    if (cursor >= line.ByteCount() || line.Data()[cursor] != '[') {
        return false;
    }

    usize end = cursor + 1;
    while (end < line.ByteCount() && line.Data()[end] != ']') {
        ++end;
    }

    if (end == line.ByteCount()) {
        return false;
    }

    value = StringView(line.Data() + cursor + 1, end - cursor - 1);
    cursor = end + 1;
    return true;
}

// JSON lines 줄이면 필드를 읽는다(문서는 호출자가 재사용한다).
void ReadJsonFields(JsonDocument& document, StringView line, ViewFields& fields) noexcept {
    if (!document.Parse(line).Succeeded() || !document.Root().IsObject()) {
        return;
    }

    const JsonValue root = document.Root();
    const auto text = [&](StringView key) noexcept {
        const JsonValue value = root.Find(key);
        return value.IsValid() && value.IsString() ? value.GetString() : StringView();
    };
    fields.time = text("t");
    fields.level = text("lvl");
    fields.category = text("cat");
    fields.event = text("event");
    const JsonValue correlation = root.Find("corr");
    if (correlation.IsValid() && correlation.IsObject()) {
        fields.hasCorrelation = true;
        for (usize index = 0; index < CorrelationFieldCount; ++index) {
            const JsonValue value = correlation.Find(CorrelationFieldNames[index]);
            if (value.IsValid() && value.IsNumber()) {
                auto number = value.TryGetUInt64();
                fields.correlation[index] = number ? number.Value() : 0;
            }
        }
    }
}

// 텍스트 줄(기본 레이아웃)의 필드를 읽는다. 다른 레이아웃이면 아무것도 읽지 않는다.
void ReadTextFields(StringView line, ViewFields& fields) noexcept {
    usize cursor = 0;
    StringView time;
    StringView level;
    StringView category;
    if (!ReadBracket(line, cursor, time) || !ReadBracket(line, cursor, level) ||
        !ReadBracket(line, cursor, category)) {
        return;
    }

    LogLevel parsed = LogLevel::Info;
    if (!TryParseViewLevel(level, parsed)) {
        return;
    }

    fields.time = time;
    fields.level = level;
    fields.category = category;
}

// 카테고리 이름이 점 접두사 prefix에 들어가는지다(대소문자 무시).
bool MatchesCategory(StringView category, StringView prefix) noexcept {
    if (category.ByteCount() < prefix.ByteCount() ||
        !EqualsIgnoringAsciiCase(StringView(category.Data(), prefix.ByteCount()), prefix)) {
        return false;
    }

    return category.ByteCount() == prefix.ByteCount() || category.Data()[prefix.ByteCount()] == '.';
}

bool Accepts(const ViewFilter& filter, const ViewFields& fields, StringView line) noexcept {
    if (!filter.level.IsEmpty()) {
        LogLevel minimum = LogLevel::Trace;
        LogLevel level = LogLevel::Trace;
        if (!TryParseViewLevel(filter.level, minimum) || !TryParseViewLevel(fields.level, level) ||
            level < minimum) {
            return false;
        }
    }

    if (!filter.category.IsEmpty() && !MatchesCategory(fields.category, filter.category)) {
        return false;
    }

    if (!filter.event.IsEmpty()) {
        auto wanted = TryParseUInt64(filter.event, HexBase);
        auto actual = TryParseUInt64(fields.event, HexBase);
        if (!wanted || !actual || wanted.Value() != actual.Value()) {
            return false;
        }
    }

    // UTC ISO 8601은 같은 모양이면 사전순이 시간순이다.
    if (!filter.since.IsEmpty() &&
        (fields.time.IsEmpty() || CompareText(fields.time, filter.since) < 0)) {
        return false;
    }

    if (!filter.until.IsEmpty() &&
        (fields.time.IsEmpty() || CompareText(fields.time, filter.until) > 0)) {
        return false;
    }

    if (filter.correlationField != CorrelationFieldCount &&
        (!fields.hasCorrelation ||
         fields.correlation[filter.correlationField] != filter.correlationValue)) {
        return false;
    }

    return filter.grep.IsEmpty() || line.Contains(filter.grep, StringComparison::CaseSensitive);
}

// 읽을 파일 이름 목록이다(폴더면 *.log, 시작 시각·번호 순).
struct ViewFileList {
    explicit ViewFileList(Allocator& allocator) noexcept : names(allocator), starts(allocator) {
    }

    Array<char> names;
    Array<usize> starts;
};

bool CollectLogFile(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& list = *static_cast<ViewFileList*>(raw);
    const StringView name(entry.name, entry.nameBytes);
    if (entry.info.type == platform::FileType::File && !LogExtensionOf(name).IsEmpty() &&
        list.starts.TryPushBack(list.names.Size())) {
        (void)list.names.TryAppend(Span<const char>(entry.name, entry.nameBytes));
        (void)list.names.TryPushBack('\0');
    }

    return true;
}

// 파일 이름을 (번호 없는 이름, 번호) 순으로 비교한다. "<host>-<시각>.log"가 그 시각의 "-2" 앞이다.
void SplitPart(StringView name, StringView& stem, uint64& part) noexcept {
    stem = StringView(name.Data(), name.ByteCount() - LogExtensionOf(name).ByteCount());
    part = 0;
    usize digits = stem.ByteCount();
    while (digits > 0 && stem.Data()[digits - 1] >= '0' && stem.Data()[digits - 1] <= '9') {
        --digits;
    }

    if (digits == stem.ByteCount() || digits == 0 || stem.Data()[digits - 1] != '-' || digits < 2 ||
        stem.Data()[digits - 2] != 'Z') {
        return;
    }

    for (usize index = digits; index < stem.ByteCount(); ++index) {
        part = part * DecimalBase + static_cast<uint64>(stem.Data()[index] - '0');
    }

    stem = StringView(stem.Data(), digits - 1);
}

Result<int32> View(Span<const StringView> args, CommandletOutput& output,
                   Allocator& allocator) noexcept {
    if (args.Size() == 0 || args.Size() % 2 == 0) {
        return UsageError(output);
    }

    ViewFilter filter;
    StringView keyPath;
    for (usize index = 1; index < args.Size(); index += 2) {
        const StringView option = args[index];
        const StringView value = args[index + 1];
        if (option == "--key") {
            keyPath = value;
        } else if (option == "--level") {
            filter.level = value;
        } else if (option == "--category") {
            filter.category = value;
        } else if (option == "--event") {
            filter.event = value;
        } else if (option == "--since") {
            filter.since = value;
        } else if (option == "--until") {
            filter.until = value;
        } else if (option == "--grep") {
            filter.grep = value;
        } else if (option == "--correlation") {
            StringView field;
            StringView number;
            if (!value.Split("=", field, number, StringComparison::CaseSensitive)) {
                return UsageError(output);
            }

            for (usize slot = 0; slot < CorrelationFieldCount; ++slot) {
                if (field == CorrelationFieldNames[slot]) {
                    filter.correlationField = slot;
                }
            }

            auto parsed = TryParseUInt64(number);
            if (filter.correlationField == CorrelationFieldCount || !parsed) {
                return UsageError(output);
            }

            filter.correlationValue = parsed.Value();
        } else {
            return UsageError(output);
        }
    }

    LogLevel ignored = LogLevel::Trace;
    if (!filter.level.IsEmpty() && !TryParseViewLevel(filter.level, ignored)) {
        return UsageError(output);
    }

    Result<int32> failure = Result<int32>::Success(1);
    Result<crypto::KemKeyPair> pair = Result<crypto::KemKeyPair>::Failure(ErrorCode::NotFound);
    if (!keyPath.IsEmpty()) {
        Array<uint8> key(allocator);
        if (!ReadWholeFile(output, keyPath, key, failure)) {
            return failure;
        }

        if (key.Size() != crypto::LogFilePrivateKeyBytes) {
            crypto::SecureZero(key.View());
            return Write(output, CommandletStream::Error,
                         "log view: the key file is not a log private key (expected 32 bytes)\n",
                         1);
        }

        pair = crypto::KemKeyPair::TryFromSeed(crypto::KemAlgorithm::XWing, key.View());
        crypto::SecureZero(key.View());
        if (!pair) {
            return Result<int32>::Failure(pair.Error());
        }
    }

    // 폴더면 *.log를 모으고, 아니면 그 파일 하나다.
    const PathText root(args[0]);
    platform::FileInfo info;
    const usize rootBytes = platform::CStringLength(root.text);
    if (const platform::FileStatus stat =
            platform::PhysicalFileSystem().Stat(root.text, rootBytes, info);
        !stat) {
        return FileFailure(output, "open", args[0], stat);
    }

    ViewFileList list(allocator);
    const bool folder = info.type == platform::FileType::Directory;
    if (folder) {
        if (const platform::FileStatus listed = platform::PhysicalFileSystem().EnumerateDirectory(
                root.text, rootBytes, &CollectLogFile, &list);
            !listed) {
            return FileFailure(output, "list", args[0], listed);
        }

        const auto nameAt = [&](usize start) noexcept {
            return StringView(list.names.Data() + start);
        };
        list.starts.Sort([&](usize left, usize right) noexcept {
            StringView leftStem;
            StringView rightStem;
            uint64 leftPart = 0;
            uint64 rightPart = 0;
            SplitPart(nameAt(left), leftStem, leftPart);
            SplitPart(nameAt(right), rightStem, rightPart);
            const int order = CompareText(leftStem, rightStem);
            return order != 0 ? order < 0 : leftPart < rightPart;
        });
    } else if (!list.starts.TryPushBack(0) ||
               !list.names.TryAppend(Span<const char>(root.text, rootBytes + 1))) {
        return Result<int32>::Failure(ErrorCode::OutOfMemory);
    }

    JsonDocument document(allocator);
    Array<uint8> file(allocator);
    Array<uint8> plain(allocator);
    char pathStorage[CommandletRegistry::MaxArgumentBytes + 1];
    for (const usize start : list.starts) {
        FixedStringBuilder path{Span<char>(pathStorage)};
        if (folder) {
            (void)path.TryAppend(StringView(root.text, rootBytes));
            (void)path.TryAppendAscii('/');
        }

        (void)path.TryAppend(StringView(list.names.Data() + start));
        if (path.Truncated()) {
            return Write(output, CommandletStream::Error, "log view: a log file path is too long\n",
                         1);
        }

        file.Clear();
        if (!ReadWholeFile(output, path.View(), file, failure)) {
            return failure;
        }

        // 봉인 여부는 magic으로 정한다. 압축 파일은 잘린 frame 앞까지 보여 주고 경고한 뒤 다음
        // 파일로 넘어간다.
        Span<const uint8> text = file.View();
        if (IsSealedLogFile(file.View())) {
            if (!pair) {
                if (auto warned =
                        WarnFile(output, path.View(), "encrypted log file needs --key; skipped");
                    !warned) {
                    return Result<int32>::Failure(warned.Error());
                }

                continue;
            }

            plain.Clear();
            ViewPlaintext collected{&plain, crypto::LogFileHasCompressedBatches(file.View()),
                                    &allocator};
            auto decoded = crypto::TryDecodeLogFile(pair.Value(), file.View(), allocator,
                                                    &AppendPlaintext, &collected);
            if (!decoded) {
                (void)WarnFile(output, path.View(), ErrorCodeName(decoded.Error()));
            }

            text = plain.View();
        } else if (path.View().EndsWith(CompressedLogFileExtensionText)) {
            plain.Clear();
            if (auto inflated = InflateFrames(file.View(), allocator, plain); !inflated) {
                (void)WarnFile(output, path.View(),
                               "compressed log is truncated or damaged; showing the frames "
                               "before it");
            }

            text = plain.View();
        }

        usize lineStart = 0;
        bool previousAccepted = false;
        for (usize index = 0; index <= text.Size(); ++index) {
            if (index < text.Size() && text[index] != '\n') {
                continue;
            }

            StringView line(reinterpret_cast<const char*>(text.Data()) + lineStart,
                            index - lineStart);
            lineStart = index + 1;
            if (!line.IsEmpty() && line.Data()[line.ByteCount() - 1] == '\r') {
                line = StringView(line.Data(), line.ByteCount() - 1);
            }

            if (line.IsEmpty()) {
                continue;
            }

            // 기본 레이아웃 기록의 이어진 줄(여러 줄 메시지)은 앞 기록의 판정을 따른다.
            const char first = line.Data()[0];
            bool accepted = false;
            if (first != '{' && first != '[') {
                accepted = previousAccepted;
            } else {
                ViewFields fields;
                if (first == '{') {
                    ReadJsonFields(document, line, fields);
                } else {
                    ReadTextFields(line, fields);
                }

                accepted = Accepts(filter, fields, line);
                previousAccepted = accepted;
            }

            if (!accepted) {
                continue;
            }

            auto written = output.write(output.context, CommandletStream::Output, line);
            if (written) {
                written = output.write(output.context, CommandletStream::Output, "\n");
            }

            if (!written) {
                return Result<int32>::Failure(written.Error());
            }
        }
    }

    return Result<int32>::Success(0);
}

} // namespace

Result<int32> RunLogCommand(void* context, Span<const StringView> args,
                            CommandletOutput& output) noexcept {
    // 문맥은 실행 진입점이 넘긴 범용 할당자다(파일·복호·압축 풀기 버퍼가 거기서 나온다).
    Allocator& allocator = *static_cast<Allocator*>(context);
    if (args.Size() == 0) {
        return UsageError(output);
    }

    const Span<const StringView> rest = args.RightChop(1);
    if (args[0] == "keygen") {
        return KeyGen(rest, output);
    }

    if (args[0] == "decode") {
        return Decode(rest, output, allocator);
    }

    if (args[0] == "seal") {
        return Seal(rest, output, allocator);
    }

    if (args[0] == "view") {
        return View(rest, output, allocator);
    }

    if (args[0] == "pack") {
        return Pack(rest, output, allocator);
    }

    if (args[0] == "--help" && args.Size() == 1) {
        return Write(output, CommandletStream::Output, Usage, 0);
    }

    return UsageError(output);
}

} // namespace rocks::commandlet_host
