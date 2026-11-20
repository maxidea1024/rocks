#include "ContentSignCommandlet.h"

#include "CommandletFiles.h"

#include <AssetRuntime/ContentSignature.h>
#include <AssetRuntime/CookedManifest.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Crypto/Bytes.h>
#include <Crypto/KeyBlob.h>
#include <Crypto/PasswordHash.h>
#include <Crypto/Random.h>
#include <Platform/File.h>

// content-sign 명령 구현이다(Docs/Design/ContentSigning.ko.md 5·7절 — A1f). 봉투 형식·검증은
// AssetRuntime ContentSignature가 맡고, 여기서는 인수·파일·키 파일만 다룬다.
namespace rocks::commandlet_host {

namespace {

constexpr StringView CommandName = "content-sign";

constexpr StringView Usage =
    "usage:\n"
    "  content-sign keygen <out key blob> <out public> --password-file <path>\n"
    "      Create a content signing key (Ed25519) wrapped with the password (Argon2id). The\n"
    "      public file holds one line '<keyId hex> <public key hex>' for the game's trusted key\n"
    "      list. Never commit the key blob to the repository and never put it into cooked\n"
    "      output; keep it offline and use it only for releases. keygen refuses to overwrite.\n"
    "  content-sign sign <in.rman> <out.rsig> --key <key blob> --password-file <path>\n"
    "                    --version <u64> --product <id> --channel <name>\n"
    "      Check the cooked manifest and write the signed envelope (CookedManifest.rsig).\n"
    "      The output is written to a temporary file and replaced atomically.\n"
    "  content-sign verify <in.rsig> --trust <public> --product <id> --channel <name>\n"
    "                      [--minimum-version <u64>]\n"
    "      Verify an envelope the way the engine does and check the manifest inside.\n"
    "  The password is read from a file (one trailing newline is removed); it is never taken\n"
    "  from the command line. Product and channel are ASCII identifiers [A-Za-z0-9_.-].\n";

// 명령이 만드는 진단 한 줄의 버퍼 byte 수다(경로는 인수 상한까지 들어간다).
constexpr usize MessageBytes = CommandletRegistry::MaxArgumentBytes * 2 + 256;

// 출력 파일을 쓸 임시 파일은 "<출력>.<무작위 hex>.tmp"다(쓴 뒤 Replace로 바꾼다 — CS1 원자적 교체).
// 무작위 부분이 있어 같은 출력을 동시에 서명해도 임시 파일이 겹치지 않고, CreateNew로 만들어 남의
// 파일을 덮어쓰지 않는다.
constexpr StringView TemporarySuffix = ".tmp";
constexpr usize TemporaryRandomBytes = 8;

Result<int32> Write(CommandletOutput& output, CommandletStream stream, StringView text,
                    int32 code) noexcept {
    auto written = output.write(output.context, stream, text);
    return written ? Result<int32>::Success(code) : Result<int32>::Failure(written.Error());
}

Result<int32> UsageError(CommandletOutput& output) noexcept {
    return Write(output, CommandletStream::Error, Usage, 2);
}

// 한 줄 오류를 표준 오류에 쓰고 code로 돌려준다.
template <class... TArgs>
Result<int32> Fail(CommandletOutput& output, int32 code, const char* format,
                   const TArgs&... args) noexcept {
    char storage[MessageBytes];
    FixedStringBuilder text{Span<char>(storage)};
    (void)text.TryAppend(CommandName);
    (void)text.TryAppend(": ");
    (void)text.TryAppendFormat(format, args...);
    (void)text.TryAppend("\n");
    return Write(output, CommandletStream::Error, text.View(), code);
}

// 위치 인수 뒤의 "--이름 값" 선택지다. 같은 이름 두 번·모르는 이름·값 없음은 사용법 오류다.
struct Options {
    StringView key;
    StringView passwordFile;
    StringView version;
    StringView product;
    StringView channel;
    StringView trust;
    StringView minimumVersion;

    StringView* Slot(StringView name) noexcept {
        if (name == "--key") {
            return &key;
        }

        if (name == "--password-file") {
            return &passwordFile;
        }

        if (name == "--version") {
            return &version;
        }

        if (name == "--product") {
            return &product;
        }

        if (name == "--channel") {
            return &channel;
        }

        if (name == "--trust") {
            return &trust;
        }

        if (name == "--minimum-version") {
            return &minimumVersion;
        }

        return nullptr;
    }

    // allowed는 이 하위 명령이 받는 이름이다.
    bool Parse(Span<const StringView> args, Span<const StringView> allowed) noexcept {
        if (args.Size() % 2 != 0) {
            return false;
        }

        for (usize index = 0; index < args.Size(); index += 2) {
            // 모르는 이름(slot 없음)과 다른 하위 명령의 이름(allowed 밖)을 모두 거부한다.
            StringView* slot = Slot(args[index]);
            bool allowedHere = false;
            for (StringView name : allowed) {
                allowedHere = allowedHere || name == args[index];
            }

            if (slot == nullptr || !allowedHere || !slot->IsEmpty() || args[index + 1].IsEmpty()) {
                return false;
            }

            *slot = args[index + 1];
        }

        return true;
    }
};

// 비밀번호 파일을 읽어 끝의 줄바꿈("\n"·"\r\n") 하나를 뺀다. 실패하면 오류를 출력하고 그 종료
// 코드를 failure에 둔다. 호출자가 bytes를 SecureZero로 지운다.
bool ReadPassword(CommandletOutput& output, StringView path, Array<uint8>& bytes,
                  Result<int32>& failure) noexcept {
    if (!ReadCommandFile(output, CommandName, path, bytes, failure)) {
        return false;
    }

    usize length = bytes.Size();
    if (length != 0 && bytes[length - 1] == '\n') {
        --length;
        if (length != 0 && bytes[length - 1] == '\r') {
            --length;
        }
    }

    if (length == 0) {
        failure = Fail(output, 1, "the password file '{}' is empty", path);
        return false;
    }

    // 뺀 줄바꿈 byte도 지운다(길이만 줄이면 버퍼에 남는다).
    crypto::SecureZero(Span<uint8>(bytes.Data() + length, bytes.Size() - length));
    (void)bytes.TryResize(length);
    return true;
}

void ClearSecret(Array<uint8>& bytes) noexcept {
    crypto::SecureZero(Span<uint8>(bytes.Data(), bytes.Size()));
}

Result<int32> KeyGen(Span<const StringView> args, CommandletOutput& output,
                     Allocator& allocator) noexcept {
    Options options;
    const StringView allowed[] = {"--password-file"};
    if (args.Size() < 2 || !options.Parse(args.RightChop(2), allowed) ||
        options.passwordFile.IsEmpty()) {
        return UsageError(output);
    }

    Result<int32> failure = Result<int32>::Success(1);
    Array<uint8> password(allocator);
    if (!ReadPassword(output, options.passwordFile, password, failure)) {
        ClearSecret(password);
        return failure;
    }

    auto key = crypto::Ed25519SigningKey::TryGenerate();
    auto id = crypto::TryNewKeyId();
    if (!key || !id) {
        ClearSecret(password);
        return Fail(output, 1, "cannot create a key ({})",
                    ErrorCodeName(!key ? key.Error() : id.Error()));
    }

    // 키 파일 byte를 파일을 만들기 전에 만든다(실패하면 아무 파일도 남기지 않는다).
    uint8 blob[crypto::KeyBlobMaxBytes];
    auto wrapped = crypto::TryWrapKeyWithPassword(password.View(), crypto::PasswordCostModerate,
                                                  key.Value(), id.Value(), Span<uint8>(blob));
    ClearSecret(password);
    if (!wrapped) {
        return Fail(output, 1, "cannot wrap the key ({})", ErrorCodeName(wrapped.Error()));
    }

    TrustedContentKey trusted;
    trusted.id = id.Value();
    trusted.key = key.Value().PublicKey();
    // 공개 파일은 키 줄과 줄바꿈이다(NUL 자리를 줄바꿈으로 바꾼다).
    char line[TrustedContentKeyTextChars + 1];
    FormatTrustedContentKey(trusted, line);
    line[TrustedContentKeyTextChars] = '\n';

    // 두 파일을 모두 새로 만든 뒤에 쓴다. 하나라도 이미 있으면 아무것도 쓰지 않는다.
    const PathText keyPath(args[0]);
    const PathText publicPath(args[1]);
    platform::File keyFile;
    if (!CreateCommandFile(output, CommandName, keyPath, args[0], keyFile, failure)) {
        return failure;
    }

    platform::File publicFile;
    if (!CreateCommandFile(output, CommandName, publicPath, args[1], publicFile, failure)) {
        (void)keyFile.Close();
        (void)platform::PhysicalFileSystem().Delete(keyPath.text,
                                                    platform::CStringLength(keyPath.text));
        return failure;
    }

    const bool written =
        WriteCommandFileAndClose(output, CommandName, args[0], keyFile,
                                 Span<const uint8>(blob, wrapped.Value()), failure) &&
        WriteCommandFileAndClose(
            output, CommandName, args[1], publicFile,
            Span<const uint8>(reinterpret_cast<const uint8*>(line), TrustedContentKeyTextChars + 1),
            failure);
    if (!written) {
        // 한쪽만 남기지 않는다.
        (void)keyFile.Close();
        (void)publicFile.Close();
        (void)platform::PhysicalFileSystem().Delete(keyPath.text,
                                                    platform::CStringLength(keyPath.text));
        (void)platform::PhysicalFileSystem().Delete(publicPath.text,
                                                    platform::CStringLength(publicPath.text));
        return failure;
    }

    char storage[MessageBytes];
    FixedStringBuilder message{Span<char>(storage)};
    (void)message.TryAppendFormat(
        "content-sign keygen: wrote key '{}' and public key '{}'. Keep the key file out of the "
        "repository and cooked output.\n{}",
        args[0], args[1], StringView(line, TrustedContentKeyTextChars + 1));
    return Write(output, CommandletStream::Output, message.View(), 0);
}

// path 뒤에 "."·무작위 hex·접미사를 붙인 NUL 종료 경로다. 인수 상한 안의 경로는 늘 들어가므로
// valid가 거짓인 것은 OS 난수 실패뿐이다.
struct TemporaryPath {
    explicit TemporaryPath(StringView path) noexcept {
        uint8 random[TemporaryRandomBytes];
        char hex[TemporaryRandomBytes * 2 + 1];
        FixedStringBuilder builder{Span<char>(text, sizeof(text) - 1)};
        valid = crypto::TryFillRandom(Span<uint8>(random)).IsOk() &&
                crypto::TryToHex(Span<const uint8>(random), Span<char>(hex)).IsOk() &&
                builder.TryAppend(path).IsOk() && builder.TryAppend(".").IsOk() &&
                builder.TryAppend(StringView(hex, TemporaryRandomBytes * 2)).IsOk() &&
                builder.TryAppend(TemporarySuffix).IsOk();
        bytes = builder.View().ByteCount();
        text[bytes] = '\0';
    }

    char text[CommandletRegistry::MaxArgumentBytes + 1 + TemporaryRandomBytes * 2 + 8];
    usize bytes = 0;
    bool valid = false;
};

// bytes를 임시 파일에 다 쓰고 저장 매체까지 내보낸 뒤 path로 바꾼다(원자적 교체).
Result<int32> ReplaceFile(CommandletOutput& output, StringView path,
                          Span<const uint8> bytes) noexcept {
    const PathText target(path);
    const TemporaryPath temporary(path);
    if (!temporary.valid) {
        return Fail(output, 1, "cannot make a temporary file name (OS random failure)");
    }

    const StringView temporaryView(temporary.text, temporary.bytes);
    platform::File file;
    if (const platform::FileStatus opened =
            file.Open(temporary.text, platform::FileOpenMode::CreateNew);
        !opened) {
        return ReportFileFailure(output, CommandName, "create", temporaryView, opened);
    }

    platform::FileStatus status = file.WriteAll(bytes.Data(), bytes.Size()).status;
    const char* action = "write";
    if (status) {
        status = file.Flush();
        action = "flush";
    }

    const platform::FileStatus closed = file.Close();
    if (status && !closed) {
        status = closed;
        action = "close";
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    if (!status) {
        (void)files.Delete(temporary.text, temporary.bytes);
        return ReportFileFailure(output, CommandName, action, temporaryView, status);
    }

    if (const platform::FileStatus replaced = files.Replace(
            temporary.text, temporary.bytes, target.text, platform::CStringLength(target.text));
        !replaced) {
        (void)files.Delete(temporary.text, temporary.bytes);
        return ReportFileFailure(output, CommandName, "replace", path, replaced);
    }

    return Result<int32>::Success(0);
}

// product·channel로 scope를 만든다. 형식이 틀리면 사용법 오류다.
bool ParseScope(const Options& options, Sha256Digest& scope) noexcept {
    auto made = MakeContentScope(options.product, options.channel);
    if (!made) {
        return false;
    }

    scope = made.Value();
    return true;
}

Result<int32> Sign(Span<const StringView> args, CommandletOutput& output,
                   Allocator& allocator) noexcept {
    Options options;
    const StringView allowed[] = {"--key", "--password-file", "--version", "--product",
                                  "--channel"};
    if (args.Size() < 2 || !options.Parse(args.RightChop(2), allowed) || options.key.IsEmpty() ||
        options.passwordFile.IsEmpty() || options.version.IsEmpty()) {
        return UsageError(output);
    }

    auto version = TryParseUInt64(options.version);
    Sha256Digest scope;
    if (!version || !ParseScope(options, scope)) {
        return UsageError(output);
    }

    // 매니페스트를 먼저 확인한다(매니페스트가 아닌 파일에 서명하지 않는다).
    Result<int32> failure = Result<int32>::Success(1);
    Array<uint8> manifest(allocator);
    if (!ReadCommandFile(output, CommandName, args[0], manifest, failure)) {
        return failure;
    }

    AssetDiagnostic diagnostic;
    if (auto parsed = ReadCookedManifest(allocator, manifest.View(), &diagnostic); !parsed) {
        return Fail(output, 1, "'{}' is not a cooked manifest ({}, diagnostic {})", args[0],
                    ErrorCodeName(parsed.Error()), AssetDiagnosticName(diagnostic.code));
    }

    Array<uint8> blob(allocator);
    if (!ReadCommandFile(output, CommandName, options.key, blob, failure)) {
        return failure;
    }

    Array<uint8> password(allocator);
    if (!ReadPassword(output, options.passwordFile, password, failure)) {
        ClearSecret(password);
        return failure;
    }

    auto key =
        crypto::TryUnwrapKeyWithPassword<crypto::Ed25519SigningKey>(password.View(), blob.View());
    ClearSecret(password);
    if (!key) {
        return Fail(output, 1,
                    "cannot open the key '{}' ({} — wrong password or not a content signing key)",
                    options.key, ErrorCodeName(key.Error()));
    }

    Array<uint8> envelope(allocator);
    if (auto written = WriteContentEnvelope(allocator, manifest.View(), key.Value().key,
                                            key.Value().id, version.Value(), scope, envelope);
        !written) {
        return Fail(output, 1, "cannot sign '{}' ({})", args[0], ErrorCodeName(written.Error()));
    }

    if (auto replaced = ReplaceFile(output, args[1], envelope.View());
        !replaced || replaced.Value() != 0) {
        return replaced;
    }

    TrustedContentKey trusted;
    trusted.id = key.Value().id;
    trusted.key = key.Value().key.PublicKey();
    char line[TrustedContentKeyTextChars + 1];
    FormatTrustedContentKey(trusted, line);
    char storage[MessageBytes];
    FixedStringBuilder message{Span<char>(storage)};
    (void)message.TryAppendFormat("content-sign sign: wrote '{}' (key {}, content version {})\n",
                                  args[1], StringView(line, ContentKeyIdHexChars), version.Value());
    return Write(output, CommandletStream::Output, message.View(), 0);
}

Result<int32> Verify(Span<const StringView> args, CommandletOutput& output,
                     Allocator& allocator) noexcept {
    Options options;
    const StringView allowed[] = {"--trust", "--product", "--channel", "--minimum-version"};
    if (args.Size() < 1 || !options.Parse(args.RightChop(1), allowed) || options.trust.IsEmpty()) {
        return UsageError(output);
    }

    ContentTrust trust;
    if (!ParseScope(options, trust.scope)) {
        return UsageError(output);
    }

    if (!options.minimumVersion.IsEmpty()) {
        auto minimum = TryParseUInt64(options.minimumVersion);
        if (!minimum) {
            return UsageError(output);
        }

        trust.minimumContentVersion = minimum.Value();
    }

    Result<int32> failure = Result<int32>::Success(1);
    Array<uint8> publicText(allocator);
    if (!ReadCommandFile(output, CommandName, options.trust, publicText, failure)) {
        return failure;
    }

    auto key = ParseTrustedContentKey(
        StringView(reinterpret_cast<const char*>(publicText.Data()), publicText.Size()));
    if (!key) {
        return Fail(output, 1, "'{}' is not a content public key line", options.trust);
    }

    const TrustedContentKey keys[] = {key.Value()};
    trust.keys = keys;
    Array<uint8> envelope(allocator);
    if (!ReadCommandFile(output, CommandName, args[0], envelope, failure)) {
        return failure;
    }

    ContentEnvelopeInfo info;
    AssetDiagnostic diagnostic;
    auto verified = VerifyContentEnvelope(allocator, envelope.View(), trust, &info, &diagnostic);
    if (!verified) {
        return Fail(output, 1, "'{}' rejected at {}: {} (diagnostic {})", args[0],
                    ContentEnvelopeFieldName(static_cast<ContentEnvelopeField>(diagnostic.section)),
                    ErrorCodeName(verified.Error()), AssetDiagnosticName(diagnostic.code));
    }

    auto manifest = ReadCookedManifest(allocator, verified.Value(), &diagnostic);
    if (!manifest) {
        return Fail(output, 1, "'{}' holds a bad cooked manifest ({}, diagnostic {})", args[0],
                    ErrorCodeName(manifest.Error()), AssetDiagnosticName(diagnostic.code));
    }

    char line[TrustedContentKeyTextChars + 1];
    FormatTrustedContentKey(key.Value(), line);
    char storage[MessageBytes];
    FixedStringBuilder message{Span<char>(storage)};
    (void)message.TryAppendFormat(
        "content-sign verify: OK (key {}, content version {}, {} entries, profile '{}')\n",
        StringView(line, ContentKeyIdHexChars), info.contentVersion,
        manifest.Value().entries.Size(), manifest.Value().profileId.View());
    return Write(output, CommandletStream::Output, message.View(), 0);
}

} // namespace

Result<int32> RunContentSignCommand(void* context, Span<const StringView> args,
                                    CommandletOutput& output) noexcept {
    // 문맥은 실행 진입점이 넘긴 범용 할당자다(파일·봉투 버퍼가 거기서 나온다).
    Allocator& allocator = *static_cast<Allocator*>(context);
    if (args.Size() == 0) {
        return UsageError(output);
    }

    const Span<const StringView> rest = args.RightChop(1);
    if (args[0] == "keygen") {
        return KeyGen(rest, output, allocator);
    }

    if (args[0] == "sign") {
        return Sign(rest, output, allocator);
    }

    if (args[0] == "verify") {
        return Verify(rest, output, allocator);
    }

    if (args[0] == "--help" && args.Size() == 1) {
        return Write(output, CommandletStream::Output, Usage, 0);
    }

    return UsageError(output);
}

} // namespace rocks::commandlet_host
