#include "LogCommandlet.h"

#include <Core/Containers/Array.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Core/Text/NumberText.h>
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
    "      Encrypt a plain file in the same format (diagnostics and tests).\n"
    "  log view <file|folder> [--key <private key file>] [--level <minimum>] [--category "
    "<prefix>]\n"
    "           [--event <id>] [--since <UTC>] [--until <UTC>] [--grep <text>]\n"
    "           [--correlation <session|task|asset|entity|request>=<id>]\n"
    "      Print matching lines of plain or encrypted log files. A folder is read in start-time\n"
    "      and part order (*.log). JSON lines files filter on every field; text files with the\n"
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
    char storage[MessageBytes];
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

// 새 파일을 만든다(있으면 덮어쓰지 않는다). 실패하면 오류를 출력하고 그 종료 코드를 failure에 둔다.
bool CreateNewFile(CommandletOutput& output, const PathText& path, StringView shown,
                   platform::File& file, Result<int32>& failure) noexcept {
    if (const platform::FileStatus opened = file.Open(path.text, platform::FileOpenMode::CreateNew);
        !opened) {
        failure = FileFailure(output, "create", shown, opened);
        return false;
    }

    return true;
}

// 열어 둔 새 파일에 bytes를 다 쓰고 닫는다.
bool WriteAndClose(CommandletOutput& output, StringView shown, platform::File& file,
                   Span<const uint8> bytes, Result<int32>& failure) noexcept {
    if (const platform::FileTransfer written = file.WriteAll(bytes.Data(), bytes.Size());
        !written.status) {
        failure = FileFailure(output, "write", shown, written.status);
        return false;
    }

    if (const platform::FileStatus closed = file.Close(); !closed) {
        failure = FileFailure(output, "close", shown, closed);
        return false;
    }

    return true;
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

// log view가 읽는 파일 확장자와 번호 해석의 진법이다.
constexpr StringView LogFileExtensionText = ".log";
constexpr uint64 DecimalBase = 10;
// 상관 id 칸 수다(session·task·asset·entity·request).
constexpr usize CorrelationFieldCount = 5;

Result<void> AppendPlaintext(void* raw, Span<const uint8> plaintext) noexcept {
    return static_cast<Array<uint8>*>(raw)->TryAppend(plaintext);
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

constexpr StringView CorrelationFieldNames[CorrelationFieldCount] = {"session", "task", "asset",
                                                                     "entity", "request"};

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

    if (!filter.event.IsEmpty() && !EqualsIgnoringAsciiCase(fields.event, filter.event)) {
        return false;
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
    if (entry.info.type == platform::FileType::File && name.EndsWith(LogFileExtensionText) &&
        list.starts.TryPushBack(list.names.Size())) {
        (void)list.names.TryAppend(Span<const char>(entry.name, entry.nameBytes));
        (void)list.names.TryPushBack('\0');
    }

    return true;
}

// 파일 이름을 (번호 없는 이름, 번호) 순으로 비교한다. "<host>-<시각>.log"가 그 시각의 "-2" 앞이다.
void SplitPart(StringView name, StringView& stem, uint64& part) noexcept {
    stem = StringView(name.Data(), name.ByteCount() - LogFileExtensionText.ByteCount());
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

Result<int32> View(Span<const StringView> args, CommandletOutput& output) noexcept {
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

    SystemAllocator allocator;
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
        file.Clear();
        if (!ReadWholeFile(output, path.View(), file, failure)) {
            return failure;
        }

        Span<const uint8> text = file.View();
        if (pair) {
            plain.Clear();
            auto decoded = crypto::TryDecodeLogFile(pair.Value(), file.View(), allocator,
                                                    &AppendPlaintext, &plain);
            if (!decoded) {
                char storage[MessageBytes];
                FixedStringBuilder message{Span<char>(storage)};
                (void)message.TryAppendFormat("log view: '{}': {}\n", path.View(),
                                              ErrorCodeName(decoded.Error()));
                return Write(output, CommandletStream::Error, message.View(), 1);
            }

            text = plain.View();
        }

        usize lineStart = 0;
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

            ViewFields fields;
            if (line.Data()[0] == '{') {
                ReadJsonFields(document, line, fields);
            } else {
                ReadTextFields(line, fields);
            }

            if (!Accepts(filter, fields, line)) {
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

    if (args[0] == "view") {
        return View(rest, output);
    }

    if (args[0] == "--help" && args.Size() == 1) {
        return Write(output, CommandletStream::Output, Usage, 0);
    }

    return UsageError(output);
}

} // namespace rocks::commandlet_host
